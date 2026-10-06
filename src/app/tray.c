#include "app/tray.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <SDL_syswm.h>
#include <string.h>

#include "app/i18n.h"
#include "app/single_instance.h"

#define WM_APP_TRAY (WM_APP + 4)
#define IDM_TRAY_SHOW 1
#define IDM_TRAY_EXIT 2

static HWND g_hwnd;
static WNDPROC g_prev_proc;
static NOTIFYICONDATAW g_nid;
static UINT g_wm_taskbar_created; // Explorer restarted: the icon must be added again
static UINT g_wm_show_request;    // a second copy of the app was launched: show our window instead
static HICON g_icon;
static SDL_Window *g_win;
static int g_exiting;

static void utf8_to_wide(const char *s, wchar_t *out, int cap) {
  if (!s || !out || cap <= 0) return;
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
  if (n <= 0) out[0] = 0;
  out[cap - 1] = 0;
}

static void tray_add(void) {
  memset(&g_nid, 0, sizeof g_nid);
  g_nid.cbSize = sizeof g_nid;
  g_nid.hWnd = g_hwnd;
  g_nid.uID = 1;
  g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_nid.uCallbackMessage = WM_APP_TRAY;
  g_nid.hIcon = g_icon;
  utf8_to_wide(g_win ? SDL_GetWindowTitle(g_win) : "", g_nid.szTip, (int)(sizeof(g_nid.szTip) / sizeof(wchar_t)));
  // The tooltip has room for 127 chars; drop the "(build ...)" suffix if present.
  wchar_t *p = wcsstr(g_nid.szTip, L" (build");
  if (p) *p = 0;
  Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void window_show(void) {
  if (!g_hwnd) return;
  ShowWindow(g_hwnd, SW_SHOW);
  if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
  SetForegroundWindow(g_hwnd);
}

static void window_hide(void) {
  if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE); // SDL sees WM_SHOWWINDOW and tracks the state
}

static void request_exit(void) {
  if (g_exiting) return;
  g_exiting = 1;
  SDL_Event ev;
  memset(&ev, 0, sizeof ev);
  ev.type = SDL_QUIT; // the main loop's normal exit path (WM_CLOSE no longer reaches SDL)
  SDL_PushEvent(&ev);
}

static void tray_menu(HWND h) {
  POINT pt;
  GetCursorPos(&pt);
  HMENU mnu = CreatePopupMenu();
  if (!mnu) return;
  wchar_t w_show[64], w_exit[64];
  utf8_to_wide(tr(STR_TRAY_SHOW), w_show, 64);
  utf8_to_wide(tr(STR_TRAY_EXIT), w_exit, 64);
  AppendMenuW(mnu, MF_STRING, IDM_TRAY_SHOW, w_show);
  AppendMenuW(mnu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(mnu, MF_STRING, IDM_TRAY_EXIT, w_exit);
  // Standard tray-menu dance: the menu only closes on an outside click when our
  // window is foreground, and the WM_NULL post lets it dismiss cleanly afterwards.
  SetForegroundWindow(h);
  int cmd = TrackPopupMenu(mnu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, h, NULL);
  PostMessageW(h, WM_NULL, 0, 0);
  DestroyMenu(mnu);
  if (cmd == IDM_TRAY_SHOW) window_show();
  else if (cmd == IDM_TRAY_EXIT) request_exit();
}

static LRESULT CALLBACK tray_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == g_wm_taskbar_created && g_wm_taskbar_created) {
    tray_add();
    return 0;
  }
  if (m == g_wm_show_request && g_wm_show_request) {
    window_show();
    return 0;
  }
  switch (m) {
  case WM_SYSCOMMAND:
    if ((wp & 0xFFF0) == SC_MINIMIZE) { // minimize button -> tray
      window_hide();
      return 0;
    }
    break;
  case WM_CLOSE: // X button / Alt+F4 -> tray, not quit
    if (!g_exiting) {
      window_hide();
      return 0;
    }
    break;
  // Windows shutdown/logoff and installers (RestartManager) close apps via these, not
  // WM_CLOSE: agree and really quit so the session can end / the exe can be replaced.
  case WM_QUERYENDSESSION:
    return TRUE;
  case WM_ENDSESSION:
    if (wp) request_exit();
    return 0;
  case WM_APP_TRAY:
    if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) window_show();
    else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) tray_menu(h);
    return 0;
  default:
    break;
  }
  return CallWindowProcW(g_prev_proc, h, m, wp, lp);
}

void tray_init(SDL_Window *win) {
  if (!win || g_hwnd) return;
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
  g_win = win;
  g_hwnd = info.info.win.window;
  // The exe's own icon (resources/app.rc.in: IDI_ICON1), else the stock one.
  g_icon = LoadIconW(GetModuleHandleW(NULL), L"IDI_ICON1");
  if (!g_icon) g_icon = LoadIconW(NULL, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
  g_wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  g_wm_show_request = RegisterWindowMessageW(SINGLE_INSTANCE_SHOW_MSG_NAME);
  g_prev_proc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)tray_wndproc);
  tray_add();
}

void tray_shutdown(void) {
  if (!g_hwnd) return;
  g_exiting = 1;
  Shell_NotifyIconW(NIM_DELETE, &g_nid);
  if (g_prev_proc && IsWindow(g_hwnd)) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_prev_proc);
  g_prev_proc = NULL;
  g_hwnd = NULL;
  g_win = NULL;
}

#else

void tray_init(SDL_Window *win) { (void)win; }
void tray_shutdown(void) {}

#endif
