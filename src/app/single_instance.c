#include "app/single_instance.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The "Local\" prefix scopes the mutex to the current logon session, so two different
// users on one machine can each run their own copy, but one user cannot run two.
#define SINGLE_INSTANCE_MUTEX_NAME L"Local\\UltralightIPCamViewerRecorder.SingleInstance"

static HANDLE g_mutex;

int single_instance_acquire(void) {
  if (g_mutex) return 1;
  HANDLE h = CreateMutexW(NULL, TRUE, SINGLE_INSTANCE_MUTEX_NAME);
  DWORD err = GetLastError();
  if (h && err != ERROR_ALREADY_EXISTS) {
    g_mutex = h;
    return 1;
  }
  // Another copy holds the mutex (or the name is owned by a process we cannot
  // open — treat that the same way rather than launch a duplicate).
  if (h) CloseHandle(h);

  // Let the running instance take the foreground: SetForegroundWindow is only
  // honoured for the process that currently has focus (us) unless we hand the
  // permission over first.
  AllowSetForegroundWindow(ASFW_ANY);
  UINT msg = RegisterWindowMessageW(SINGLE_INSTANCE_SHOW_MSG_NAME);
  if (msg) PostMessageW(HWND_BROADCAST, msg, 0, 0);
  return 0;
}

void single_instance_release(void) {
  if (!g_mutex) return;
  ReleaseMutex(g_mutex);
  CloseHandle(g_mutex);
  g_mutex = NULL;
}

#else

int single_instance_acquire(void) { return 1; }
void single_instance_release(void) {}

#endif
