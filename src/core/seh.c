#include "core/seh.h"

#include <stdio.h>
#include <string.h>

#include "core/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char g_crash_log_path[1024] = {0};
static DWORD g_tls_alive_idx = TLS_OUT_OF_INDEXES;
static volatile LONG g_handler_installed = 0;

typedef struct SehThreadCtx {
  SDL_ThreadFunction real_fn;
  void *real_arg;
  SDL_atomic_t *alive_out;
} SehThreadCtx;

static const char *seh_code_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:        return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:   return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:    return "FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:      return "FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:   return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:            return "FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:         return "FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:           return "FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:     return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:           return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:            return "INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:     return "INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:return "NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:        return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:          return "STACK_OVERFLOW";
    default:                                return "UNKNOWN";
  }
}

static int seh_code_is_fatal(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
      return 1;
    default:
      return 0;
  }
}

static LONG WINAPI seh_vectored_handler(EXCEPTION_POINTERS *info) {
  if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
  DWORD code = info->ExceptionRecord->ExceptionCode;

  // Only intervene on truly fatal codes. Software-raised exceptions used by C++ runtimes
  // and debuggers must be passed through.
  if (!seh_code_is_fatal(code)) return EXCEPTION_CONTINUE_SEARCH;

  // Log details. Even if logging itself faults, the worst case is the secondary fault
  // re-enters this handler — we use a simple guard via a TLS counter to avoid infinite
  // recursion.
  static __thread int in_handler = 0;
  if (in_handler) {
    // Recursive fault: just kill the thread.
    ExitThread(code);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  in_handler = 1;

  if (g_crash_log_path[0]) {
    char line[512];
    snprintf(line, sizeof(line),
             "thread=%lu code=0x%08lx (%s) addr=%p flags=0x%08lx",
             (unsigned long)GetCurrentThreadId(),
             (unsigned long)code,
             seh_code_name(code),
             info->ExceptionRecord->ExceptionAddress,
             (unsigned long)info->ExceptionRecord->ExceptionFlags);
    append_log_line_utf8(g_crash_log_path, line);
  }

  // Mark the thread's "alive" flag (if it registered one via TLS) so the supervisor sees
  // the death without needing to join.
  if (g_tls_alive_idx != TLS_OUT_OF_INDEXES) {
    void *p = TlsGetValue(g_tls_alive_idx);
    if (p) {
      SDL_AtomicSet((SDL_atomic_t *)p, 0);
    }
  }

  in_handler = 0;

  // Kill only this thread. The OS reclaims its stack; the process continues.
  ExitThread(code);
  return EXCEPTION_CONTINUE_SEARCH; // unreachable
}

void seh_install_global_handler(const char *base_dir) {
  // Idempotent install.
  if (InterlockedCompareExchange(&g_handler_installed, 1, 0) != 0) return;

  if (g_tls_alive_idx == TLS_OUT_OF_INDEXES) {
    g_tls_alive_idx = TlsAlloc();
  }

  const char *base_use = (base_dir && *base_dir) ? base_dir : ".\\";
  snprintf(g_crash_log_path, sizeof(g_crash_log_path), "%scache\\crash.log", base_use);

  // First (1) means this handler runs before any later-registered ones.
  AddVectoredExceptionHandler(1, seh_vectored_handler);

  // Boot marker so the log file shows the run started cleanly even when no crash has
  // occurred yet.
  char line[256];
  snprintf(line, sizeof(line), "seh_handler_installed pid=%lu", (unsigned long)GetCurrentProcessId());
  append_log_line_utf8(g_crash_log_path, line);
}

static int seh_thread_entry(void *ctx_v) {
  SehThreadCtx *ctx = (SehThreadCtx *)ctx_v;
  if (!ctx || !ctx->real_fn) {
    SDL_free(ctx);
    return 0;
  }

  if (ctx->alive_out) {
    SDL_AtomicSet(ctx->alive_out, 1);
    if (g_tls_alive_idx != TLS_OUT_OF_INDEXES) {
      TlsSetValue(g_tls_alive_idx, ctx->alive_out);
    }
  }

  int rc = ctx->real_fn(ctx->real_arg);

  if (ctx->alive_out) {
    SDL_AtomicSet(ctx->alive_out, 0);
    if (g_tls_alive_idx != TLS_OUT_OF_INDEXES) {
      TlsSetValue(g_tls_alive_idx, NULL);
    }
  }
  SDL_free(ctx);
  return rc;
}

SDL_Thread *seh_create_thread(SDL_ThreadFunction fn, const char *name, void *arg,
                              SDL_atomic_t *alive_out) {
  if (!fn) return NULL;
  SehThreadCtx *ctx = (SehThreadCtx *)SDL_malloc(sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->real_fn = fn;
  ctx->real_arg = arg;
  ctx->alive_out = alive_out;
  // "alive" means created-and-not-yet-exited, so it is raised HERE, before the OS
  // schedules the thread. Raising it in the entry left a create->entry window where a
  // valid handle reported alive == 0; the UI's "decode thread died silently" check then
  // flashed "Camera unavailable" right after expanding a tile and restarted the stream.
  if (alive_out) SDL_AtomicSet(alive_out, 1);

  SDL_Thread *t = SDL_CreateThread(seh_thread_entry, name, ctx);
  if (!t) {
    if (alive_out) SDL_AtomicSet(alive_out, 0);
    SDL_free(ctx);
    return NULL;
  }
  return t;
}

#else  /* !_WIN32 — pass-through stub */

typedef struct SehThreadCtx {
  SDL_ThreadFunction real_fn;
  void *real_arg;
  SDL_atomic_t *alive_out;
} SehThreadCtx;

void seh_install_global_handler(const char *base_dir) { (void)base_dir; }

static int seh_thread_entry(void *ctx_v) {
  SehThreadCtx *ctx = (SehThreadCtx *)ctx_v;
  if (!ctx || !ctx->real_fn) { SDL_free(ctx); return 0; }
  if (ctx->alive_out) SDL_AtomicSet(ctx->alive_out, 1);
  int rc = ctx->real_fn(ctx->real_arg);
  if (ctx->alive_out) SDL_AtomicSet(ctx->alive_out, 0);
  SDL_free(ctx);
  return rc;
}

SDL_Thread *seh_create_thread(SDL_ThreadFunction fn, const char *name, void *arg,
                              SDL_atomic_t *alive_out) {
  if (!fn) return NULL;
  SehThreadCtx *ctx = (SehThreadCtx *)SDL_malloc(sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->real_fn = fn;
  ctx->real_arg = arg;
  ctx->alive_out = alive_out;
  if (alive_out) SDL_AtomicSet(alive_out, 1); // see the Win32 variant: alive from creation

  SDL_Thread *t = SDL_CreateThread(seh_thread_entry, name, ctx);
  if (!t) {
    if (alive_out) SDL_AtomicSet(alive_out, 0);
    SDL_free(ctx);
    return NULL;
  }
  return t;
}
#endif
