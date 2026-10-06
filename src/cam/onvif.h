#pragma once

#include <stddef.h>

// Minimal ONVIF client: given a device service URL (e.g. "http://user:pass@192.168.1.10"
// or "192.168.1.10:8080"), queries the camera over SOAP/HTTP and collects the
// stream URIs (main + secondary media profile), device identification and basic
// network info. Deliberately does NOT touch imaging/quality settings.

typedef struct OnvifInfo {
  int ok;                // 1 when at least the main stream URI was discovered
  char error[256];       // human-readable failure reason when ok == 0

  char main_uri[2048];   // RTSP URI of the first media profile (credentials injected)
  char sub_uri[2048];    // RTSP URI of the second media profile ("" if none)

  char name[128];        // device hostname/name
  char manufacturer[128];
  char model[128];
  char firmware[128];
  char serial[128];
  char ip[64];
  char mac[64];
  int has_ptz;           // device advertises a PTZ service
  char ptz_url[1024];    // PTZ service endpoint ("" when has_ptz == 0)
  char profile_token[128];  // token of the main media profile (for PTZ commands)
  char user[128];        // credentials parsed out of the device URL, for reuse
  char pass[128];        // by PTZ calls against the same device
} OnvifInfo;

// Blocking probe (several HTTP round-trips, seconds). Run on a worker thread.
// Returns 0 and out->ok == 1 on success; -1 with out->error set otherwise.
int onvif_probe(const char *device_url, OnvifInfo *out);

// One blocking SOAP request against any ONVIF service endpoint, with
// WS-UsernameToken digest auth. `service_url` is a full http URL
// ("http://host:port/onvif/PTZ"); `user`/`pass` override credentials embedded
// in the URL when non-empty. `action` (may be NULL) is the SOAPAction URI added
// to the Content-Type header, which PTZ services generally require.
// On success returns 0 and stores a NUL-terminated response body in *resp
// (caller frees with SDL_free); otherwise -1 with err filled.
int onvif_soap_call(const char *service_url, const char *user, const char *pass,
                    const char *action, const char *body_inner, char **resp, char *err,
                    size_t err_sz);

// Async wrapper around onvif_probe for the UI thread.
typedef struct OnvifProbe OnvifProbe;

OnvifProbe *onvif_probe_start(const char *device_url);
// 1 = finished (out filled), 0 = still running, -1 = bad args.
int onvif_probe_poll(OnvifProbe *p, OnvifInfo *out);
// Safe in every state: joins a finished probe, abandons a running one (the
// worker frees itself on completion).
void onvif_probe_destroy(OnvifProbe *p);
