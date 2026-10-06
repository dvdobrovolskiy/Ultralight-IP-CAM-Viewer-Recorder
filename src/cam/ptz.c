#include "cam/ptz.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "cam/onvif.h"
#include "core/seh.h"
#include "core/util.h"

#define PTZ_NS_WSDL "http://www.onvif.org/ver20/ptz/wsdl"
#define PTZ_NS_SCHEMA "http://www.onvif.org/ver10/schema"
#define PTZ_DEFAULT_PATH "/onvif/PTZ"
// ContinuousMove is re-sent at this interval while a key/button is held, so a
// device that applies its own move timeout keeps panning for as long as the
// user holds. Independently, a move is force-stopped after PTZ_MAX_HOLD_MS so a
// lost release event can never leave the head turning indefinitely.
#define PTZ_REFRESH_MS 2000
#define PTZ_MAX_HOLD_MS 30000

typedef enum PtzCmdKind {
  PTZ_CMD_NONE = 0,
  PTZ_CMD_MOVE,
  PTZ_CMD_STOP,
} PtzCmdKind;

struct PtzController {
  SDL_Thread *th;
  SDL_mutex *mutex;
  SDL_cond *cond;
  SDL_atomic_t quit;
  SDL_atomic_t busy;
  // Teardown handshake: 0 = worker running, 1 = worker finished (the owner
  // reaps it), 2 = owner gave up waiting (the worker frees itself). Lets
  // shutdown bound how long it blocks on a camera that stopped answering.
  SDL_atomic_t state;

  // One-slot mailbox: a newer command replaces an unsent older one.
  PtzCmdKind pending;
  PtzTarget pending_target;
  float vx, vy, vz;

  // Last resolved profile token, keyed by endpoint, so the discovery round-trip
  // happens once per camera rather than on every nudge.
  char cached_url[2048];
  char cached_profile[128];

  char last_error[256];
};

// "1.00" / "-0.35" built without printf's %f so a comma decimal separator from
// a locale change can never corrupt the XML.
static void fmt_speed(float v, char *out, size_t out_sz) {
  int s = (int)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
  if (s > 100) s = 100;
  if (s < -100) s = -100;
  int neg = (s < 0);
  if (neg) s = -s;
  snprintf(out, out_sz, "%s%d.%02d", neg ? "-" : "", s / 100, s % 100);
}

// Accepts "host", "host:port", "http://host:port" or a full service URL and
// yields a full http URL with an explicit PTZ path.
static void ptz_normalize_url(const char *in, char *out, size_t out_sz) {
  if (!in || !*in) {
    if (out_sz) out[0] = 0;
    return;
  }
  char tmp[2048];
  const char *sep = strstr(in, "://");
  if (!sep) {
    snprintf(tmp, sizeof(tmp), "http://%s", in);
  } else if (SDL_strncasecmp(in, "http://", 7) == 0) {
    snprintf(tmp, sizeof(tmp), "%s", in);
  } else {
    // ONVIF services are always HTTP. Copying the camera's rtsp:// address into
    // this field is an easy mistake, so rewrite the scheme instead of failing.
    snprintf(tmp, sizeof(tmp), "http://%s", sep + 3);
  }
  const char *scheme = strstr(tmp, "://");
  const char *path = scheme ? strchr(scheme + 3, '/') : NULL;
  if (path && path[1]) {
    snprintf(out, out_sz, "%s", tmp);
  } else {
    // No path (or a bare "/"): use the conventional PTZ endpoint.
    size_t n = path ? (size_t)(path - tmp) : strlen(tmp);
    snprintf(out, out_sz, "%.*s%s", (int)n, tmp, PTZ_DEFAULT_PATH);
  }
}

static void set_error(PtzController *p, const char *msg) {
  SDL_LockMutex(p->mutex);
  snprintf(p->last_error, sizeof(p->last_error), "%s", msg ? msg : "");
  SDL_UnlockMutex(p->mutex);
}

// Minimal XML scrape for the first Profiles/@token attribute.
static int extract_first_profile_token(const char *xml, char *out, size_t out_sz) {
  const char *p = xml;
  while ((p = strstr(p, "token=\"")) != NULL) {
    p += 7;
    const char *end = strchr(p, '"');
    if (!end) break;
    size_t n = (size_t)(end - p);
    if (n > 0 && n < out_sz) {
      memcpy(out, p, n);
      out[n] = 0;
      return 0;
    }
    p = end + 1;
  }
  return -1;
}

// Replace the path of `url` with `path`, keeping scheme://host:port.
static void url_with_path(const char *url, const char *path, char *out, size_t out_sz) {
  const char *scheme = strstr(url, "://");
  const char *slash = scheme ? strchr(scheme + 3, '/') : NULL;
  size_t n = slash ? (size_t)(slash - url) : strlen(url);
  snprintf(out, out_sz, "%.*s%s", (int)n, url, path);
}

// Resolve the profile token to use for `url`: explicit config wins (ONVIF
// cameras get it from the probe, so they never reach the network here), then
// the per-endpoint cache, then a GetProfiles round-trip, and finally the "000"
// convention many devices use. Whatever is settled on — including the fallback
// — is cached, so a device that refuses GetProfiles costs one lookup, not one
// per nudge.
static void resolve_profile(PtzController *p, const char *url, const PtzTarget *t, char *out,
                            size_t out_sz) {
  if (t->profile[0]) {
    snprintf(out, out_sz, "%s", t->profile);
    return;
  }
  SDL_LockMutex(p->mutex);
  int cached = (strcmp(p->cached_url, url) == 0 && p->cached_profile[0]);
  if (cached) snprintf(out, out_sz, "%s", p->cached_profile);
  SDL_UnlockMutex(p->mutex);
  if (cached) return;

  // GetProfiles is a Media-service operation: ask the usual media endpoints
  // first, and only then the PTZ endpoint (some devices serve every service
  // from one URL).
  char token[128] = {0};
  const char *paths[] = {"/onvif/device_service", "/onvif/media", NULL};
  for (int i = 0; !token[0] && i < 3; i++) {
    char candidate[2048];
    if (paths[i]) {
      url_with_path(url, paths[i], candidate, sizeof(candidate));
    } else {
      snprintf(candidate, sizeof(candidate), "%s", url);
    }
    char *resp = NULL;
    char err[200] = {0};
    if (onvif_soap_call(candidate, t->user, t->pass,
                        "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
                        "<trt:GetProfiles xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"/>",
                        &resp, err, sizeof(err)) == 0) {
      extract_first_profile_token(resp, token, sizeof(token));
      SDL_free(resp);
    }
  }
  if (!token[0]) snprintf(token, sizeof(token), "%s", "000");

  snprintf(out, out_sz, "%s", token);
  SDL_LockMutex(p->mutex);
  snprintf(p->cached_url, sizeof(p->cached_url), "%s", url);
  snprintf(p->cached_profile, sizeof(p->cached_profile), "%s", token);
  SDL_UnlockMutex(p->mutex);
}

static void do_move(PtzController *p, const PtzTarget *t, float vx, float vy, float vz) {
  char url[2048];
  ptz_normalize_url(t->url, url, sizeof(url));
  if (!url[0]) {
    set_error(p, "no PTZ URL configured");
    return;
  }
  char profile[128];
  resolve_profile(p, url, t, profile, sizeof(profile));

  char sx[16], sy[16], sz[16];
  fmt_speed(vx, sx, sizeof(sx));
  fmt_speed(vy, sy, sizeof(sy));
  fmt_speed(vz, sz, sizeof(sz));

  char zoom_el[128] = "";
  if (vz != 0.0f) {
    snprintf(zoom_el, sizeof(zoom_el), "<Zoom x=\"%s\" xmlns=\"%s\"/>", sz, PTZ_NS_SCHEMA);
  }
  char pantilt_el[192] = "";
  if (vx != 0.0f || vy != 0.0f) {
    snprintf(pantilt_el, sizeof(pantilt_el), "<PanTilt x=\"%s\" y=\"%s\" xmlns=\"%s\"/>", sx, sy,
             PTZ_NS_SCHEMA);
  }

  char body[1024];
  snprintf(body, sizeof(body),
           "<ContinuousMove xmlns=\"%s\"><ProfileToken>%s</ProfileToken>"
           "<Velocity>%s%s</Velocity></ContinuousMove>",
           PTZ_NS_WSDL, profile, pantilt_el, zoom_el);

  char *resp = NULL;
  char err[200] = {0};
  if (onvif_soap_call(url, t->user, t->pass, PTZ_NS_WSDL "/ContinuousMove", body, &resp, err,
                      sizeof(err)) != 0) {
    set_error(p, err);
    return;
  }
  SDL_free(resp);
  set_error(p, "");
}

static void do_stop(PtzController *p, const PtzTarget *t) {
  char url[2048];
  ptz_normalize_url(t->url, url, sizeof(url));
  if (!url[0]) return;
  char profile[128];
  resolve_profile(p, url, t, profile, sizeof(profile));

  char body[512];
  snprintf(body, sizeof(body),
           "<Stop xmlns=\"%s\"><ProfileToken>%s</ProfileToken>"
           "<PanTilt>true</PanTilt><Zoom>true</Zoom></Stop>",
           PTZ_NS_WSDL, profile);

  char *resp = NULL;
  char err[200] = {0};
  if (onvif_soap_call(url, t->user, t->pass, PTZ_NS_WSDL "/Stop", body, &resp, err, sizeof(err)) !=
      0) {
    set_error(p, err);
    return;
  }
  SDL_free(resp);
  set_error(p, "");  // clear a stale banner once the camera answers again
}

static void ptz_free_resources(PtzController *p) {
  if (!p) return;
  if (p->cond) SDL_DestroyCond(p->cond);
  if (p->mutex) SDL_DestroyMutex(p->mutex);
  SDL_free(p);
}

static int ptz_worker_fn(void *ud) {
  PtzController *p = (PtzController *)ud;
  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

  int moving = 0;
  PtzTarget moving_target;
  memset(&moving_target, 0, sizeof(moving_target));
  float mvx = 0.0f, mvy = 0.0f, mvz = 0.0f;
  Uint32 refresh_at = 0, hard_stop_at = 0;

  for (;;) {
    SDL_LockMutex(p->mutex);
    while (p->pending == PTZ_CMD_NONE && !SDL_AtomicGet(&p->quit)) {
      if (moving) {
        Uint32 now = SDL_GetTicks();
        Uint32 wake = (refresh_at < hard_stop_at) ? refresh_at : hard_stop_at;
        if (now >= wake) break;  // time to refresh or force-stop, handled below
        SDL_CondWaitTimeout(p->cond, p->mutex, wake - now);
      } else {
        SDL_CondWait(p->cond, p->mutex);
      }
    }
    PtzCmdKind kind = p->pending;
    PtzTarget target = p->pending_target;
    float vx = p->vx, vy = p->vy, vz = p->vz;
    p->pending = PTZ_CMD_NONE;
    int quitting = SDL_AtomicGet(&p->quit);
    SDL_UnlockMutex(p->mutex);

    if (kind == PTZ_CMD_NONE && moving) {
      Uint32 now = SDL_GetTicks();
      if (now >= hard_stop_at) {
        // Safety net: a release was never delivered.
        kind = PTZ_CMD_STOP;
        target = moving_target;
      } else if (!quitting && now >= refresh_at) {
        // Keep-alive: re-assert the same velocity so the hold continues.
        SDL_AtomicSet(&p->busy, 1);
        do_move(p, &moving_target, mvx, mvy, mvz);
        SDL_AtomicSet(&p->busy, 0);
        refresh_at = SDL_GetTicks() + PTZ_REFRESH_MS;
      }
    }

    if (kind != PTZ_CMD_NONE) {
      SDL_AtomicSet(&p->busy, 1);
      if (kind == PTZ_CMD_MOVE) {
        // Switching cameras mid-move: stop the old head first, or it would keep
        // turning with no watchdog left to catch it.
        if (moving && strcmp(moving_target.url, target.url) != 0) {
          do_stop(p, &moving_target);
        }
        do_move(p, &target, vx, vy, vz);
        moving = 1;
        moving_target = target;
        mvx = vx;
        mvy = vy;
        mvz = vz;
        Uint32 now = SDL_GetTicks();
        refresh_at = now + PTZ_REFRESH_MS;
        hard_stop_at = now + PTZ_MAX_HOLD_MS;
      } else {
        do_stop(p, &target);
        moving = 0;
      }
      SDL_AtomicSet(&p->busy, 0);
    }

    if (quitting) {
      // Never leave the camera moving on shutdown.
      if (moving) {
        SDL_AtomicSet(&p->busy, 1);
        do_stop(p, &moving_target);
        SDL_AtomicSet(&p->busy, 0);
        moving = 0;
      }
      break;
    }
  }

  // If the owner stopped waiting for us (state 2), we own the struct now.
  if (!SDL_AtomicCAS(&p->state, 0, 1)) {
    ptz_free_resources(p);
  }
  return 0;
}

PtzController *ptz_start(void) {
  PtzController *p = (PtzController *)SDL_calloc(1, sizeof(PtzController));
  if (!p) return NULL;
  p->mutex = SDL_CreateMutex();
  p->cond = SDL_CreateCond();
  SDL_AtomicSet(&p->quit, 0);
  SDL_AtomicSet(&p->busy, 0);
  SDL_AtomicSet(&p->state, 0);
  if (!p->mutex || !p->cond) {
    if (p->mutex) SDL_DestroyMutex(p->mutex);
    if (p->cond) SDL_DestroyCond(p->cond);
    SDL_free(p);
    return NULL;
  }
  p->th = seh_create_thread(ptz_worker_fn, "ptz", p, NULL);
  if (!p->th) {
    SDL_DestroyMutex(p->mutex);
    SDL_DestroyCond(p->cond);
    SDL_free(p);
    return NULL;
  }
  return p;
}

void ptz_stop_and_free(PtzController *p) {
  if (!p) return;
  SDL_LockMutex(p->mutex);
  SDL_AtomicSet(&p->quit, 1);
  SDL_CondSignal(p->cond);
  SDL_UnlockMutex(p->mutex);

  SDL_Thread *th = p->th;
  // Give the worker time to deliver a final Stop, but never block shutdown on a
  // camera that stopped answering (a request can take ~10s to time out).
  const int kMaxWaitMs = 2500;
  for (int waited = 0; waited < kMaxWaitMs && SDL_AtomicGet(&p->state) == 0; waited += 25) {
    SDL_Delay(25);
  }
  if (SDL_AtomicCAS(&p->state, 0, 2)) {
    // Still working: hand ownership over and let it clean up on its own.
    if (th) SDL_DetachThread(th);
    return;
  }
  if (th) SDL_WaitThread(th, NULL);
  ptz_free_resources(p);
}

void ptz_move(PtzController *p, const PtzTarget *t, float vx, float vy, float vz) {
  if (!p || !t) return;
  SDL_LockMutex(p->mutex);
  p->pending = PTZ_CMD_MOVE;
  p->pending_target = *t;
  p->vx = vx;
  p->vy = vy;
  p->vz = vz;
  SDL_CondSignal(p->cond);
  SDL_UnlockMutex(p->mutex);
}

void ptz_release(PtzController *p) {
  if (!p) return;
  SDL_LockMutex(p->mutex);
  // Keep the target of the move being stopped; a STOP with no prior move is a
  // harmless no-op the worker skips when the URL is empty.
  p->pending = PTZ_CMD_STOP;
  SDL_CondSignal(p->cond);
  SDL_UnlockMutex(p->mutex);
}

int ptz_is_busy(PtzController *p) { return p ? SDL_AtomicGet(&p->busy) : 0; }

int ptz_last_error(PtzController *p, char *out, size_t out_sz) {
  if (!out || out_sz == 0) return 0;
  out[0] = 0;
  if (!p) return 0;
  SDL_LockMutex(p->mutex);
  snprintf(out, out_sz, "%s", p->last_error);
  SDL_UnlockMutex(p->mutex);
  return out[0] ? 1 : 0;
}
