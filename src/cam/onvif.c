#include "cam/onvif.h"

// winsock2.h must come before anything that could pull in windows.h (and with
// it the legacy winsock.h), so keep the network includes at the very top.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "core/seh.h"

#ifdef _WIN32
typedef SOCKET onvif_sock;
#define ONVIF_BAD_SOCK INVALID_SOCKET
#define onvif_closesock closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int onvif_sock;
#define ONVIF_BAD_SOCK (-1)
#define onvif_closesock close
#endif

#define ONVIF_DEFAULT_PATH "/onvif/device_service"
#define ONVIF_CONNECT_TIMEOUT_MS 4000
#define ONVIF_IO_TIMEOUT_MS 6000
#define ONVIF_MAX_RESPONSE (512 * 1024)

// ---------------------------------------------------------------------------
// SHA-1 (needed for the WS-UsernameToken password digest; no external deps).

typedef struct Sha1Ctx {
  unsigned int h[5];
  unsigned char block[64];
  size_t block_len;
  unsigned long long total_len;
} Sha1Ctx;

static unsigned int sha1_rol(unsigned int v, int bits) {
  return (v << bits) | (v >> (32 - bits));
}

static void sha1_process(Sha1Ctx *c, const unsigned char *p) {
  unsigned int w[80];
  for (int i = 0; i < 16; i++) {
    w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
           ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
  }
  for (int i = 16; i < 80; i++) w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  unsigned int a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
  for (int i = 0; i < 80; i++) {
    unsigned int f, k;
    if (i < 20) {
      f = (b & cc) | ((~b) & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ cc ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & cc) | (b & d) | (cc & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ cc ^ d;
      k = 0xCA62C1D6u;
    }
    unsigned int t = sha1_rol(a, 5) + f + e + k + w[i];
    e = d;
    d = cc;
    cc = sha1_rol(b, 30);
    b = a;
    a = t;
  }
  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += cc;
  c->h[3] += d;
  c->h[4] += e;
}

static void sha1_init(Sha1Ctx *c) {
  c->h[0] = 0x67452301u;
  c->h[1] = 0xEFCDAB89u;
  c->h[2] = 0x98BADCFEu;
  c->h[3] = 0x10325476u;
  c->h[4] = 0xC3D2E1F0u;
  c->block_len = 0;
  c->total_len = 0;
}

static void sha1_update(Sha1Ctx *c, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  c->total_len += len;
  while (len > 0) {
    size_t take = 64 - c->block_len;
    if (take > len) take = len;
    memcpy(c->block + c->block_len, p, take);
    c->block_len += take;
    p += take;
    len -= take;
    if (c->block_len == 64) {
      sha1_process(c, c->block);
      c->block_len = 0;
    }
  }
}

static void sha1_final(Sha1Ctx *c, unsigned char out[20]) {
  unsigned long long bits = c->total_len * 8ull;
  unsigned char pad = 0x80;
  sha1_update(c, &pad, 1);
  unsigned char zero = 0;
  while (c->block_len != 56) sha1_update(c, &zero, 1);
  unsigned char len_be[8];
  for (int i = 0; i < 8; i++) len_be[i] = (unsigned char)(bits >> (56 - i * 8));
  sha1_update(c, len_be, 8);
  for (int i = 0; i < 5; i++) {
    out[i * 4] = (unsigned char)(c->h[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
    out[i * 4 + 3] = (unsigned char)(c->h[i]);
  }
}

static void base64_encode(const unsigned char *in, size_t len, char *out, size_t out_sz) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    if (o + 5 > out_sz) break;
    unsigned int v = (unsigned int)in[i] << 16;
    if (i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
    if (i + 2 < len) v |= (unsigned int)in[i + 2];
    out[o++] = tbl[(v >> 18) & 63];
    out[o++] = tbl[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? tbl[v & 63] : '=';
  }
  out[o] = 0;
}

// ---------------------------------------------------------------------------
// Device URL parsing.

typedef struct DevUrl {
  char user[128];
  char pass[128];
  char host[256];
  char path[512];
  int port;
} DevUrl;

static void pct_decode(char *s) {
  char *w = s;
  for (char *r = s; *r; r++) {
    if (*r == '%' && r[1] && r[2]) {
      char hex[3] = {r[1], r[2], 0};
      *w++ = (char)strtol(hex, NULL, 16);
      r += 2;
    } else {
      *w++ = *r;
    }
  }
  *w = 0;
}

static int parse_device_url(const char *in, DevUrl *d, char *err, size_t err_sz) {
  memset(d, 0, sizeof(*d));
  d->port = 80;
  if (!in || !*in) {
    snprintf(err, err_sz, "empty device URL");
    return -1;
  }
  const char *p = in;
  const char *scheme = strstr(in, "://");
  if (scheme) {
    if (strncmp(in, "https://", 8) == 0) {
      snprintf(err, err_sz, "https not supported, use http");
      return -1;
    }
    if (strncmp(in, "http://", 7) != 0) {
      snprintf(err, err_sz, "unsupported URL scheme");
      return -1;
    }
    p = scheme + 3;
  }

  const char *path = strchr(p, '/');
  const char *authority_end = path ? path : p + strlen(p);

  const char *at = NULL;
  for (const char *q = p; q < authority_end; q++) {
    if (*q == '@') at = q;
  }
  if (at) {
    const char *colon = NULL;
    for (const char *q = p; q < at; q++) {
      if (*q == ':') {
        colon = q;
        break;
      }
    }
    if (colon) {
      snprintf(d->user, sizeof(d->user), "%.*s", (int)(colon - p), p);
      snprintf(d->pass, sizeof(d->pass), "%.*s", (int)(at - colon - 1), colon + 1);
    } else {
      snprintf(d->user, sizeof(d->user), "%.*s", (int)(at - p), p);
    }
    pct_decode(d->user);
    pct_decode(d->pass);
    p = at + 1;
  }

  const char *port_colon = NULL;
  for (const char *q = p; q < authority_end; q++) {
    if (*q == ':') port_colon = q;
  }
  if (port_colon) {
    snprintf(d->host, sizeof(d->host), "%.*s", (int)(port_colon - p), p);
    d->port = atoi(port_colon + 1);
    if (d->port <= 0 || d->port > 65535) {
      snprintf(err, err_sz, "invalid port in device URL");
      return -1;
    }
  } else {
    snprintf(d->host, sizeof(d->host), "%.*s", (int)(authority_end - p), p);
  }
  if (!d->host[0]) {
    snprintf(err, err_sz, "no host in device URL");
    return -1;
  }

  if (path && path[0] && strcmp(path, "/") != 0) {
    snprintf(d->path, sizeof(d->path), "%s", path);
  } else {
    snprintf(d->path, sizeof(d->path), "%s", ONVIF_DEFAULT_PATH);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Tiny HTTP/1.1 POST client (Connection: close).

static int sock_wait_writable(onvif_sock s, int timeout_ms) {
  // A failed non-blocking connect() is signalled on the *except* set on
  // Winsock (the socket never becomes writable), so watch both sets.
  fd_set wf, ef;
  FD_ZERO(&wf);
  FD_ZERO(&ef);
  FD_SET(s, &wf);
  FD_SET(s, &ef);
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  if (select((int)(s + 1), NULL, &wf, &ef, &tv) < 1) return -1;
  if (FD_ISSET(s, &ef)) return -1;
  return FD_ISSET(s, &wf) ? 0 : -1;
}

static void sock_set_blocking(onvif_sock s, int blocking) {
#ifdef _WIN32
  u_long nb = blocking ? 0 : 1;
  ioctlsocket(s, FIONBIO, &nb);
#else
  int fl = fcntl(s, F_GETFL, 0);
  if (fl < 0) return;
  fcntl(s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

static void sock_set_timeouts(onvif_sock s, int timeout_ms) {
#ifdef _WIN32
  DWORD tv = (DWORD)timeout_ms;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static const char *strcasestr_local(const char *hay, const char *needle) {
  size_t nl = strlen(needle);
  for (const char *p = hay; *p; p++) {
    if (SDL_strncasecmp(p, needle, nl) == 0) return p;
  }
  return NULL;
}

// Decode a chunked transfer-encoded body in place into a fresh buffer.
static char *dechunk(const char *p, size_t len) {
  char *out = (char *)SDL_malloc(len + 1);
  if (!out) return NULL;
  size_t o = 0;
  const char *end = p + len;
  while (p < end) {
    char *next = NULL;
    long chunk = strtol(p, &next, 16);
    if (chunk <= 0 || !next) break;
    p = strstr(next, "\r\n");
    if (!p) break;
    p += 2;
    if ((size_t)(end - p) < (size_t)chunk) break;
    memcpy(out + o, p, (size_t)chunk);
    o += (size_t)chunk;
    p += chunk;
    if (p + 2 <= end && p[0] == '\r' && p[1] == '\n') p += 2;
  }
  out[o] = 0;
  return out;
}

// POST `body` to http://host:port`path`. `action`, when non-NULL, is appended
// to the Content-Type as the SOAPAction URI. On success returns 0 and hands
// back a NUL-terminated malloc'd response body plus the HTTP status code.
static int http_post(const DevUrl *d, const char *path, const char *action, const char *body,
                     char **out_body, int *out_status, char *err, size_t err_sz) {
  *out_body = NULL;
  *out_status = 0;

#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    snprintf(err, err_sz, "WSAStartup failed");
    return -1;
  }
#endif

  int rc = -1;
  onvif_sock s = ONVIF_BAD_SOCK;
  char *resp = NULL;
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", d->port);

  if (getaddrinfo(d->host, portstr, &hints, &res) != 0 || !res) {
    snprintf(err, err_sz, "cannot resolve host %s", d->host);
    goto done;
  }

  s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == ONVIF_BAD_SOCK) {
    snprintf(err, err_sz, "socket() failed");
    goto done;
  }

  sock_set_blocking(s, 0);
  if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
    if (sock_wait_writable(s, ONVIF_CONNECT_TIMEOUT_MS) != 0) {
      snprintf(err, err_sz, "cannot connect to %s:%d", d->host, d->port);
      goto done;
    }
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
    if (soerr != 0) {
      snprintf(err, err_sz, "connection to %s:%d refused", d->host, d->port);
      goto done;
    }
  }
  sock_set_blocking(s, 1);
  sock_set_timeouts(s, ONVIF_IO_TIMEOUT_MS);

  char ctype[512];
  if (action && *action) {
    snprintf(ctype, sizeof(ctype), "application/soap+xml; charset=utf-8; action=\"%s\"", action);
  } else {
    snprintf(ctype, sizeof(ctype), "application/soap+xml; charset=utf-8");
  }

  size_t body_len = strlen(body);
  size_t req_cap = body_len + strlen(path) + strlen(d->host) + strlen(ctype) + 256;
  char *req = (char *)SDL_malloc(req_cap);
  if (!req) {
    snprintf(err, err_sz, "out of memory");
    goto done;
  }
  int req_len = snprintf(req, req_cap,
                         "POST %s HTTP/1.1\r\n"
                         "Host: %s:%d\r\n"
                         "Content-Type: %s\r\n"
                         "Content-Length: %u\r\n"
                         "Connection: close\r\n"
                         "\r\n"
                         "%s",
                         path, d->host, d->port, ctype, (unsigned)body_len, body);
  if (req_len < 0 || (size_t)req_len >= req_cap) {
    SDL_free(req);
    snprintf(err, err_sz, "request too large");
    goto done;
  }

  size_t sent = 0;
  while (sent < (size_t)req_len) {
    int n = (int)send(s, req + sent, (int)((size_t)req_len - sent), 0);
    if (n <= 0) {
      SDL_free(req);
      snprintf(err, err_sz, "send failed");
      goto done;
    }
    sent += (size_t)n;
  }
  SDL_free(req);

  size_t cap = 16384, len = 0;
  resp = (char *)SDL_malloc(cap);
  if (!resp) {
    snprintf(err, err_sz, "out of memory");
    goto done;
  }
  for (;;) {
    if (len + 4096 + 1 > cap) {
      if (cap >= ONVIF_MAX_RESPONSE) break;
      cap *= 2;
      char *nr = (char *)SDL_realloc(resp, cap);
      if (!nr) break;
      resp = nr;
    }
    int n = (int)recv(s, resp + len, 4096, 0);
    if (n <= 0) break;
    len += (size_t)n;
  }
  resp[len] = 0;

  if (len == 0 || strncmp(resp, "HTTP/", 5) != 0) {
    snprintf(err, err_sz, "no HTTP response from %s:%d", d->host, d->port);
    goto done;
  }
  {
    const char *sp = strchr(resp, ' ');
    *out_status = sp ? atoi(sp + 1) : 0;
  }

  char *hdr_end = strstr(resp, "\r\n\r\n");
  if (!hdr_end) {
    snprintf(err, err_sz, "malformed HTTP response");
    goto done;
  }
  *hdr_end = 0;
  const char *body_start = hdr_end + 4;
  size_t body_bytes = len - (size_t)(body_start - resp);

  const char *te = strcasestr_local(resp, "Transfer-Encoding:");
  if (te && strcasestr_local(te, "chunked")) {
    *out_body = dechunk(body_start, body_bytes);
  } else {
    *out_body = (char *)SDL_malloc(body_bytes + 1);
    if (*out_body) {
      memcpy(*out_body, body_start, body_bytes);
      (*out_body)[body_bytes] = 0;
    }
  }
  if (!*out_body) {
    snprintf(err, err_sz, "out of memory");
    goto done;
  }
  rc = 0;

done:
  if (resp) SDL_free(resp);
  if (s != ONVIF_BAD_SOCK) onvif_closesock(s);
  if (res) freeaddrinfo(res);
#ifdef _WIN32
  WSACleanup();
#endif
  return rc;
}

// ---------------------------------------------------------------------------
// Namespace-agnostic XML scraping. ONVIF responses are small and flat enough
// that matching elements by local name (ignoring the prefix) is reliable, and
// it spares us an XML parser dependency.

static int xml_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.';
}

// Next opening tag with the given local name within [p, end). Returns the '<'.
static const char *xml_find_open(const char *p, const char *end, const char *local) {
  size_t ll = strlen(local);
  while (p < end && (p = memchr(p, '<', (size_t)(end - p))) != NULL) {
    const char *q = p + 1;
    if (q >= end || *q == '/' || *q == '?' || *q == '!') {
      p = q;
      continue;
    }
    const char *name = q;
    while (q < end && xml_name_char(*q)) q++;
    if (q < end && *q == ':') {
      name = q + 1;
      q = name;
      while (q < end && xml_name_char(*q)) q++;
    }
    if ((size_t)(q - name) == ll && strncmp(name, local, ll) == 0 && q < end &&
        (*q == '>' || *q == '/' || *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) {
      return p;
    }
    p = q;
  }
  return NULL;
}

static const char *xml_find_close(const char *p, const char *end, const char *local) {
  size_t ll = strlen(local);
  while (p < end && (p = memchr(p, '<', (size_t)(end - p))) != NULL) {
    if (p + 1 >= end || p[1] != '/') {
      p++;
      continue;
    }
    const char *q = p + 2;
    const char *name = q;
    while (q < end && xml_name_char(*q)) q++;
    if (q < end && *q == ':') {
      name = q + 1;
      q = name;
      while (q < end && xml_name_char(*q)) q++;
    }
    if ((size_t)(q - name) == ll && strncmp(name, local, ll) == 0 && q < end && *q == '>') return p;
    p = q;
  }
  return NULL;
}

static void xml_unescape(char *s) {
  char *w = s;
  for (char *r = s; *r;) {
    if (*r == '&') {
      if (strncmp(r, "&amp;", 5) == 0) {
        *w++ = '&';
        r += 5;
        continue;
      }
      if (strncmp(r, "&lt;", 4) == 0) {
        *w++ = '<';
        r += 4;
        continue;
      }
      if (strncmp(r, "&gt;", 4) == 0) {
        *w++ = '>';
        r += 4;
        continue;
      }
      if (strncmp(r, "&quot;", 6) == 0) {
        *w++ = '"';
        r += 6;
        continue;
      }
      if (strncmp(r, "&apos;", 6) == 0) {
        *w++ = '\'';
        r += 6;
        continue;
      }
    }
    *w++ = *r++;
  }
  *w = 0;
}

// Text content of the first <..:local> element within [xml, end). end == NULL
// means "until the terminating NUL". Trims whitespace, resolves entities.
static int xml_get_text(const char *xml, const char *end, const char *local, char *out,
                        size_t out_sz) {
  if (!xml || !out || out_sz == 0) return -1;
  out[0] = 0;
  if (!end) end = xml + strlen(xml);
  const char *open = xml_find_open(xml, end, local);
  if (!open) return -1;
  const char *gt = memchr(open, '>', (size_t)(end - open));
  if (!gt) return -1;
  if (gt > open && gt[-1] == '/') return 0;  // empty element
  const char *close = xml_find_close(gt + 1, end, local);
  if (!close) return -1;
  const char *a = gt + 1;
  const char *b = close;
  while (a < b && (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n')) a++;
  while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) b--;
  size_t n = (size_t)(b - a);
  if (n >= out_sz) n = out_sz - 1;
  memcpy(out, a, n);
  out[n] = 0;
  xml_unescape(out);
  return 0;
}

// Content range of the first <..:local>...</..:local> section within [xml, end).
static int xml_get_section(const char *xml, const char *end, const char *local,
                           const char **sec_start, const char **sec_end) {
  if (!end) end = xml + strlen(xml);
  const char *open = xml_find_open(xml, end, local);
  if (!open) return -1;
  const char *gt = memchr(open, '>', (size_t)(end - open));
  if (!gt || (gt > open && gt[-1] == '/')) return -1;
  const char *close = xml_find_close(gt + 1, end, local);
  if (!close) return -1;
  *sec_start = gt + 1;
  *sec_end = close;
  return 0;
}

// Value of attribute `attr` inside the opening tag starting at `open` ('<').
static int xml_get_attr(const char *open, const char *attr, char *out, size_t out_sz) {
  out[0] = 0;
  const char *gt = strchr(open, '>');
  if (!gt) return -1;
  size_t al = strlen(attr);
  for (const char *p = open + 1; p + al < gt; p++) {
    if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n') &&
        strncmp(p, attr, al) == 0) {
      const char *q = p + al;
      while (q < gt && (*q == ' ' || *q == '\t')) q++;
      if (q >= gt || *q != '=') continue;
      q++;
      while (q < gt && (*q == ' ' || *q == '\t')) q++;
      if (q >= gt || (*q != '"' && *q != '\'')) continue;
      char quote = *q++;
      const char *close = memchr(q, quote, (size_t)(gt - q));
      if (!close) return -1;
      size_t n = (size_t)(close - q);
      if (n >= out_sz) n = out_sz - 1;
      memcpy(out, q, n);
      out[n] = 0;
      xml_unescape(out);
      return 0;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// SOAP envelope + WS-UsernameToken digest authentication.

static void xml_escape_into(const char *in, char *out, size_t out_sz) {
  size_t o = 0;
  for (const char *p = in; *p && o + 7 < out_sz; p++) {
    if (*p == '&') {
      o += (size_t)snprintf(out + o, out_sz - o, "&amp;");
    } else if (*p == '<') {
      o += (size_t)snprintf(out + o, out_sz - o, "&lt;");
    } else if (*p == '>') {
      o += (size_t)snprintf(out + o, out_sz - o, "&gt;");
    } else if (*p == '"') {
      o += (size_t)snprintf(out + o, out_sz - o, "&quot;");
    } else {
      out[o++] = *p;
    }
  }
  out[o] = 0;
}

static void make_wsse_header(const char *user, const char *pass, char *out, size_t out_sz) {
  unsigned char nonce[16];
  Uint64 x = SDL_GetPerformanceCounter() ^ ((Uint64)time(NULL) << 20) ^ (Uint64)(uintptr_t)out;
  for (int i = 0; i < 16; i++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    nonce[i] = (unsigned char)(x & 0xff);
  }

  char created[40];
  time_t t = time(NULL);
  struct tm g;
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &g);

  Sha1Ctx c;
  unsigned char digest[20];
  sha1_init(&c);
  sha1_update(&c, nonce, sizeof(nonce));
  sha1_update(&c, created, strlen(created));
  sha1_update(&c, pass, strlen(pass));
  sha1_final(&c, digest);

  char nonce_b64[32], digest_b64[32], user_esc[256];
  base64_encode(nonce, sizeof(nonce), nonce_b64, sizeof(nonce_b64));
  base64_encode(digest, sizeof(digest), digest_b64, sizeof(digest_b64));
  xml_escape_into(user, user_esc, sizeof(user_esc));

  snprintf(out, out_sz,
           "<s:Header><wsse:Security s:mustUnderstand=\"1\" "
           "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
           "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
           "<wsse:UsernameToken><wsse:Username>%s</wsse:Username>"
           "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</wsse:Password>"
           "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
           "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%s</wsse:Nonce>"
           "<wsu:Created>%s</wsu:Created></wsse:UsernameToken></wsse:Security></s:Header>",
           user_esc, digest_b64, nonce_b64, created);
}

// Performs one SOAP call; on success hands back the malloc'd response body.
static int soap_call_action(const DevUrl *d, const char *path, const char *action,
                            const char *body_inner, char **resp, char *err, size_t err_sz) {
  char header[1400] = "";
  if (d->user[0]) make_wsse_header(d->user, d->pass, header, sizeof(header));

  size_t env_sz = strlen(body_inner) + strlen(header) + 256;
  char *env = (char *)SDL_malloc(env_sz);
  if (!env) {
    snprintf(err, err_sz, "out of memory");
    return -1;
  }
  snprintf(env, env_sz,
           "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">%s"
           "<s:Body>%s</s:Body></s:Envelope>",
           header, body_inner);

  int status = 0;
  int rc = http_post(d, path, action, env, resp, &status, err, err_sz);
  SDL_free(env);
  if (rc != 0) return -1;

  if (status == 401) {
    snprintf(err, err_sz, "authentication rejected (check user:pass in the URL)");
    SDL_free(*resp);
    *resp = NULL;
    return -1;
  }
  if (status >= 300) {
    char reason[160] = {0};
    if (*resp) {
      // SOAP 1.2 fault: Reason/Text; fall back to the Subcode/Value.
      if (xml_get_text(*resp, NULL, "Text", reason, sizeof(reason)) != 0) {
        xml_get_text(*resp, NULL, "Value", reason, sizeof(reason));
      }
    }
    if (reason[0]) {
      snprintf(err, err_sz, "device error: %s", reason);
    } else {
      snprintf(err, err_sz, "device returned HTTP %d", status);
    }
    SDL_free(*resp);
    *resp = NULL;
    return -1;
  }
  return 0;
}

// Device-service calls: no SOAPAction needed.
static int soap_call(const DevUrl *d, const char *path, const char *body_inner, char **resp,
                     char *err, size_t err_sz) {
  return soap_call_action(d, path, NULL, body_inner, resp, err, err_sz);
}

int onvif_soap_call(const char *service_url, const char *user, const char *pass,
                    const char *action, const char *body_inner, char **resp, char *err,
                    size_t err_sz) {
  if (!resp || !body_inner) return -1;
  *resp = NULL;
  char local_err[200] = {0};
  if (!err || err_sz == 0) {
    err = local_err;
    err_sz = sizeof(local_err);
  }
  DevUrl d;
  if (parse_device_url(service_url, &d, err, err_sz) != 0) return -1;
  // Explicit credentials win over anything embedded in the URL.
  if (user && *user) {
    snprintf(d.user, sizeof(d.user), "%s", user);
    snprintf(d.pass, sizeof(d.pass), "%s", pass ? pass : "");
  }
  return soap_call_action(&d, d.path, action, body_inner, resp, err, err_sz);
}

// ---------------------------------------------------------------------------
// Probe.

static void pct_encode_userinfo(const char *in, char *out, size_t out_sz) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < out_sz; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
        *p == '-' || *p == '.' || *p == '_' || *p == '~') {
      out[o++] = (char)*p;
    } else {
      out[o++] = '%';
      out[o++] = hex[*p >> 4];
      out[o++] = hex[*p & 15];
    }
  }
  out[o] = 0;
}

// rtsp://host/... -> rtsp://user:pass@host/... (no-op when the URI already
// carries credentials or none were given).
static void uri_with_credentials(const char *uri, const DevUrl *d, char *out, size_t out_sz) {
  if (!d->user[0]) {
    snprintf(out, out_sz, "%s", uri);
    return;
  }
  const char *scheme = strstr(uri, "://");
  if (!scheme) {
    snprintf(out, out_sz, "%s", uri);
    return;
  }
  const char *auth = scheme + 3;
  const char *slash = strchr(auth, '/');
  const char *auth_end = slash ? slash : auth + strlen(auth);
  for (const char *q = auth; q < auth_end; q++) {
    if (*q == '@') {  // credentials already present
      snprintf(out, out_sz, "%s", uri);
      return;
    }
  }
  char eu[256], ep[256];
  pct_encode_userinfo(d->user, eu, sizeof(eu));
  pct_encode_userinfo(d->pass, ep, sizeof(ep));
  snprintf(out, out_sz, "%.*s://%s:%s@%s", (int)(scheme - uri), uri, eu, ep, auth);
}

// Take only the path (+query) of a service XAddr, keeping the host:port the
// user reached the device on — XAddr often advertises an internal address that
// is unreachable through NAT/port forwarding.
static void xaddr_to_path(const char *xaddr, char *out, size_t out_sz, const char *fallback) {
  const char *scheme = strstr(xaddr, "://");
  const char *path = scheme ? strchr(scheme + 3, '/') : NULL;
  if (path && *path) {
    snprintf(out, out_sz, "%s", path);
  } else {
    snprintf(out, out_sz, "%s", fallback);
  }
}

static int get_stream_uri(const DevUrl *d, const char *media_path, const char *token, char *out,
                          size_t out_sz, char *err, size_t err_sz) {
  char token_esc[256];
  xml_escape_into(token, token_esc, sizeof(token_esc));
  char body[1024];
  snprintf(body, sizeof(body),
           "<trt:GetStreamUri xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
           "<trt:StreamSetup>"
           "<tt:Stream xmlns:tt=\"http://www.onvif.org/ver10/schema\">RTP-Unicast</tt:Stream>"
           "<tt:Transport xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
           "<tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
           "</trt:StreamSetup>"
           "<trt:ProfileToken>%s</trt:ProfileToken>"
           "</trt:GetStreamUri>",
           token_esc);
  char *resp = NULL;
  if (soap_call(d, media_path, body, &resp, err, err_sz) != 0) return -1;
  char uri[2048] = {0};
  int rc = xml_get_text(resp, NULL, "Uri", uri, sizeof(uri));
  SDL_free(resp);
  if (rc != 0 || !uri[0]) {
    snprintf(err, err_sz, "no stream URI in reply");
    return -1;
  }
  uri_with_credentials(uri, d, out, out_sz);
  return 0;
}

int onvif_probe(const char *device_url, OnvifInfo *out) {
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (!device_url) {
    snprintf(out->error, sizeof(out->error), "no device URL");
    return -1;
  }

  DevUrl dev;
  if (parse_device_url(device_url, &dev, out->error, sizeof(out->error)) != 0) return -1;
  snprintf(out->user, sizeof(out->user), "%s", dev.user);
  snprintf(out->pass, sizeof(out->pass), "%s", dev.pass);

  char err[200] = {0};
  char *resp = NULL;

  // 1. Identification. This is also the connectivity/authentication check:
  //    if it fails, the rest would too, so surface the reason and stop.
  if (soap_call(&dev, dev.path,
                "<tds:GetDeviceInformation xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>",
                &resp, err, sizeof(err)) != 0) {
    snprintf(out->error, sizeof(out->error), "%s", err);
    return -1;
  }
  xml_get_text(resp, NULL, "Manufacturer", out->manufacturer, sizeof(out->manufacturer));
  xml_get_text(resp, NULL, "Model", out->model, sizeof(out->model));
  xml_get_text(resp, NULL, "FirmwareVersion", out->firmware, sizeof(out->firmware));
  xml_get_text(resp, NULL, "SerialNumber", out->serial, sizeof(out->serial));
  SDL_free(resp);
  resp = NULL;

  // 2. Device name (optional).
  if (soap_call(&dev, dev.path,
                "<tds:GetHostname xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>", &resp,
                err, sizeof(err)) == 0) {
    xml_get_text(resp, NULL, "Name", out->name, sizeof(out->name));
    SDL_free(resp);
    resp = NULL;
  }

  // 3. Network params (optional): MAC + IP of the first interface.
  if (soap_call(&dev, dev.path,
                "<tds:GetNetworkInterfaces xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/>",
                &resp, err, sizeof(err)) == 0) {
    xml_get_text(resp, NULL, "HwAddress", out->mac, sizeof(out->mac));
    xml_get_text(resp, NULL, "Address", out->ip, sizeof(out->ip));
    SDL_free(resp);
    resp = NULL;
  }
  if (!out->ip[0]) snprintf(out->ip, sizeof(out->ip), "%s", dev.host);

  // 4. Capabilities: media service endpoint + PTZ presence.
  char media_path[512];
  snprintf(media_path, sizeof(media_path), "%s", dev.path);
  if (soap_call(&dev, dev.path,
                "<tds:GetCapabilities xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
                "<tds:Category>All</tds:Category></tds:GetCapabilities>",
                &resp, err, sizeof(err)) == 0) {
    const char *ss = NULL, *se = NULL;
    if (xml_get_section(resp, NULL, "Media", &ss, &se) == 0) {
      char xaddr[512] = {0};
      if (xml_get_text(ss, se, "XAddr", xaddr, sizeof(xaddr)) == 0 && xaddr[0]) {
        xaddr_to_path(xaddr, media_path, sizeof(media_path), dev.path);
      }
    }
    if (xml_get_section(resp, NULL, "PTZ", &ss, &se) == 0) {
      char xaddr[512] = {0};
      if (xml_get_text(ss, se, "XAddr", xaddr, sizeof(xaddr)) == 0 && xaddr[0]) {
        out->has_ptz = 1;
        // Keep the host:port the device was reached on (the advertised XAddr
        // may be an internal address behind NAT) and take only its path.
        char ptz_path[512];
        xaddr_to_path(xaddr, ptz_path, sizeof(ptz_path), "/onvif/PTZ");
        snprintf(out->ptz_url, sizeof(out->ptz_url), "http://%s:%d%s", dev.host, dev.port,
                 ptz_path);
      }
    }
    SDL_free(resp);
    resp = NULL;
  }

  // 5. Media profiles. First profile = main stream, second = substream.
  if (soap_call(&dev, media_path,
                "<trt:GetProfiles xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"/>", &resp,
                err, sizeof(err)) != 0) {
    snprintf(out->error, sizeof(out->error), "cannot list media profiles: %s", err);
    return -1;
  }
  char tokens[2][256];
  int token_count = 0;
  {
    const char *end = resp + strlen(resp);
    const char *p = resp;
    while (token_count < 2 && (p = xml_find_open(p, end, "Profiles")) != NULL) {
      if (xml_get_attr(p, "token", tokens[token_count], sizeof(tokens[0])) == 0 &&
          tokens[token_count][0]) {
        token_count++;
      }
      p++;
    }
  }
  SDL_free(resp);
  resp = NULL;
  if (token_count == 0) {
    snprintf(out->error, sizeof(out->error), "device reports no media profiles");
    return -1;
  }
  snprintf(out->profile_token, sizeof(out->profile_token), "%s", tokens[0]);

  // 6. Stream URIs.
  if (get_stream_uri(&dev, media_path, tokens[0], out->main_uri, sizeof(out->main_uri), err,
                     sizeof(err)) != 0) {
    snprintf(out->error, sizeof(out->error), "cannot get stream URI: %s", err);
    return -1;
  }
  if (token_count > 1) {
    // Substream failure is not fatal — the main stream is enough.
    get_stream_uri(&dev, media_path, tokens[1], out->sub_uri, sizeof(out->sub_uri), err,
                   sizeof(err));
  }

  out->ok = 1;
  return 0;
}

// ---------------------------------------------------------------------------
// Async wrapper.

struct OnvifProbe {
  SDL_Thread *th;
  // 0 = running, 1 = finished (owner reaps), 2 = abandoned (worker frees).
  SDL_atomic_t state;
  char device_url[2048];
  OnvifInfo info;
};

static int onvif_probe_thread_fn(void *ud) {
  OnvifProbe *p = (OnvifProbe *)ud;
  onvif_probe(p->device_url, &p->info);
  if (!SDL_AtomicCAS(&p->state, 0, 1)) {
    // Owner abandoned us (state == 2): the thread is detached, free everything.
    SDL_free(p);
  }
  return 0;
}

OnvifProbe *onvif_probe_start(const char *device_url) {
  if (!device_url || !*device_url) return NULL;
  OnvifProbe *p = (OnvifProbe *)SDL_calloc(1, sizeof(OnvifProbe));
  if (!p) return NULL;
  snprintf(p->device_url, sizeof(p->device_url), "%s", device_url);
  SDL_AtomicSet(&p->state, 0);
  p->th = seh_create_thread(onvif_probe_thread_fn, "onvif-probe", p, NULL);
  if (!p->th) {
    SDL_free(p);
    return NULL;
  }
  return p;
}

int onvif_probe_poll(OnvifProbe *p, OnvifInfo *out) {
  if (!p) return -1;
  if (SDL_AtomicGet(&p->state) != 1) return 0;
  if (out) *out = p->info;
  return 1;
}

void onvif_probe_destroy(OnvifProbe *p) {
  if (!p) return;
  SDL_Thread *th = p->th;
  if (SDL_AtomicCAS(&p->state, 0, 2)) {
    // Still running: detach; the worker frees the struct when it finishes.
    SDL_DetachThread(th);
    return;
  }
  SDL_WaitThread(th, NULL);
  SDL_free(p);
}
