#include "app/persist.h"

#include <string.h>

#include <SDL.h>

#include "app/config.h"
#include "core/seh.h"
#include "core/util.h"

typedef enum PersistCmdKind {
  PERSIST_SAVE_CAMERAS = 0,
  PERSIST_SET_INT = 1,
} PersistCmdKind;

typedef struct PersistCmd {
  struct PersistCmd *next;
  PersistCmdKind kind;
  char *path;

  // PERSIST_SAVE_CAMERAS: deep-copied snapshot of the persisted camera fields.
  CameraConfig *cams;  // only type/title_utf8/url/preview_url/record_enabled/keyint_ms set
  int count;
  int active;

  // PERSIST_SET_INT
  char *section;
  char *key;
  int value;
} PersistCmd;

struct PersistQueue {
  SDL_mutex *mutex;
  SDL_cond *cond;
  SDL_Thread *worker;
  SDL_atomic_t quit;
  PersistCmd *head;
  PersistCmd *tail;
};

static void cmd_free(PersistCmd *c) {
  if (!c) return;
  SDL_free(c->path);
  SDL_free(c->section);
  SDL_free(c->key);
  if (c->cams) {
    for (int i = 0; i < c->count; i++) {
      SDL_free(c->cams[i].title_utf8);
      SDL_free(c->cams[i].url);
      SDL_free(c->cams[i].preview_url);
      camera_clear_onvif(&c->cams[i]);
      camera_clear_ptz(&c->cams[i]);
    }
    SDL_free(c->cams);
  }
  SDL_free(c);
}

// Remove (and free) any already-queued command targeting the same destination as
// `incoming`, so only the latest write survives. Caller holds q->mutex.
static void coalesce_locked(PersistQueue *q, const PersistCmd *incoming) {
  PersistCmd *prev = NULL;
  PersistCmd *cur = q->head;
  while (cur) {
    int same = 0;
    if (cur->kind == incoming->kind && cur->path && incoming->path &&
        strcmp(cur->path, incoming->path) == 0) {
      if (cur->kind == PERSIST_SAVE_CAMERAS) {
        same = 1;
      } else if (cur->kind == PERSIST_SET_INT && cur->section && incoming->section && cur->key &&
                 incoming->key && strcmp(cur->section, incoming->section) == 0 &&
                 strcmp(cur->key, incoming->key) == 0) {
        same = 1;
      }
    }
    if (same) {
      PersistCmd *dead = cur;
      if (prev) {
        prev->next = cur->next;
      } else {
        q->head = cur->next;
      }
      if (q->tail == cur) q->tail = prev;
      cur = cur->next;
      cmd_free(dead);
    } else {
      prev = cur;
      cur = cur->next;
    }
  }
}

static void enqueue(PersistQueue *q, PersistCmd *c) {
  if (!q || !c) {
    cmd_free(c);
    return;
  }
  SDL_LockMutex(q->mutex);
  coalesce_locked(q, c);
  c->next = NULL;
  if (q->tail) {
    q->tail->next = c;
    q->tail = c;
  } else {
    q->head = q->tail = c;
  }
  SDL_CondSignal(q->cond);
  SDL_UnlockMutex(q->mutex);
}

static void cmd_execute(const PersistCmd *c) {
  if (!c) return;
#ifdef _WIN32
  if (c->kind == PERSIST_SAVE_CAMERAS) {
    save_cameras_to_ini(c->path, c->cams, c->count, c->active);
  } else if (c->kind == PERSIST_SET_INT) {
    ini_set_int(c->path, c->section, c->key, c->value);
  }
#else
  (void)c;
#endif
}

static int persist_worker_fn(void *userdata) {
  PersistQueue *q = (PersistQueue *)userdata;
  if (!q) return 0;
  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

  for (;;) {
    SDL_LockMutex(q->mutex);
    while (!q->head && !SDL_AtomicGet(&q->quit)) {
      SDL_CondWait(q->cond, q->mutex);
    }
    PersistCmd *c = q->head;
    if (c) {
      q->head = c->next;
      if (!q->head) q->tail = NULL;
    }
    int quitting = SDL_AtomicGet(&q->quit);
    SDL_UnlockMutex(q->mutex);

    if (c) {
      cmd_execute(c);
      cmd_free(c);
      continue;  // drain remaining work (even while quitting) before sleeping/exiting
    }
    if (quitting) break;
  }
  return 0;
}

PersistQueue *persist_start(void) {
  PersistQueue *q = (PersistQueue *)SDL_calloc(1, sizeof(PersistQueue));
  if (!q) return NULL;
  q->mutex = SDL_CreateMutex();
  q->cond = SDL_CreateCond();
  SDL_AtomicSet(&q->quit, 0);
  if (!q->mutex || !q->cond) {
    if (q->mutex) SDL_DestroyMutex(q->mutex);
    if (q->cond) SDL_DestroyCond(q->cond);
    SDL_free(q);
    return NULL;
  }
  q->worker = seh_create_thread(persist_worker_fn, "persist", q, NULL);
  if (!q->worker) {
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
    SDL_free(q);
    return NULL;
  }
  return q;
}

void persist_save_cameras(PersistQueue *q, const char *path, const CameraConfig *cams, int count,
                          int active_camera) {
  if (!q || !path) return;
  PersistCmd *c = (PersistCmd *)SDL_calloc(1, sizeof(PersistCmd));
  if (!c) return;
  c->kind = PERSIST_SAVE_CAMERAS;
  c->path = str_dup(path);
  c->count = count;
  c->active = active_camera;
  if (count > 0 && cams) {
    c->cams = (CameraConfig *)SDL_calloc((size_t)count, sizeof(CameraConfig));
    if (!c->cams) {
      cmd_free(c);
      return;
    }
    for (int i = 0; i < count; i++) {
      c->cams[i].type = cams[i].type;
      c->cams[i].enabled = cams[i].enabled;
      c->cams[i].record_enabled = cams[i].record_enabled;
      c->cams[i].keyint_ms = cams[i].keyint_ms;
      c->cams[i].title_utf8 = cams[i].title_utf8 ? str_dup(cams[i].title_utf8) : NULL;
      c->cams[i].url = cams[i].url ? str_dup(cams[i].url) : NULL;
      c->cams[i].preview_url = cams[i].preview_url ? str_dup(cams[i].preview_url) : NULL;
      c->cams[i].onvif_device_url =
          cams[i].onvif_device_url ? str_dup(cams[i].onvif_device_url) : NULL;
      camera_set_onvif_meta(&c->cams[i], cams[i].onvif_name, cams[i].onvif_manufacturer,
                            cams[i].onvif_model, cams[i].onvif_firmware, cams[i].onvif_serial,
                            cams[i].onvif_ip, cams[i].onvif_mac, cams[i].onvif_has_ptz);
      camera_set_ptz(&c->cams[i], cams[i].ptz_enabled, cams[i].ptz_url, cams[i].ptz_user,
                     cams[i].ptz_pass, cams[i].ptz_profile);
    }
  }
  enqueue(q, c);
}

void persist_set_int(PersistQueue *q, const char *path, const char *section, const char *key,
                     int v) {
  if (!q || !path || !section || !key) return;
  PersistCmd *c = (PersistCmd *)SDL_calloc(1, sizeof(PersistCmd));
  if (!c) return;
  c->kind = PERSIST_SET_INT;
  c->path = str_dup(path);
  c->section = str_dup(section);
  c->key = str_dup(key);
  c->value = v;
  enqueue(q, c);
}

void persist_flush_and_stop(PersistQueue *q) {
  if (!q) return;
  SDL_LockMutex(q->mutex);
  SDL_AtomicSet(&q->quit, 1);
  SDL_CondSignal(q->cond);
  SDL_UnlockMutex(q->mutex);
  if (q->worker) SDL_WaitThread(q->worker, NULL);

  // Worker drains the queue before exiting, but free anything left if it failed
  // to start.
  PersistCmd *c = q->head;
  while (c) {
    PersistCmd *n = c->next;
    cmd_free(c);
    c = n;
  }
  SDL_DestroyCond(q->cond);
  SDL_DestroyMutex(q->mutex);
  SDL_free(q);
}
