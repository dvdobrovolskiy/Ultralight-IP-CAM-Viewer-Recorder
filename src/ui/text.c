#include "ui/text.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wingdi.h>
#endif

#ifdef _WIN32
TextUiItem make_text(SDL_Renderer *renderer, const char *utf8, int pt_size,
                     SDL_Color color) {
  TextUiItem out = {0};

  if (!utf8 || !*utf8) return out;

  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
  if (wlen <= 0) return out;
  WCHAR *wtext = (WCHAR *)SDL_malloc((size_t)wlen * sizeof(WCHAR));
  if (!wtext) return out;
  if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wtext, wlen) <= 0) {
    SDL_free(wtext);
    return out;
  }

  HDC hdc = CreateCompatibleDC(NULL);
  if (!hdc) {
    SDL_free(wtext);
    return out;
  }

  int height = -MulDiv(pt_size, GetDeviceCaps(hdc, LOGPIXELSY), 72);
  HFONT hfont = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  if (!hfont) {
    DeleteDC(hdc);
    SDL_free(wtext);
    return out;
  }

  HGDIOBJ old_font = SelectObject(hdc, hfont);
  SetBkMode(hdc, TRANSPARENT);

  SIZE sz = {0, 0};
  GetTextExtentPoint32W(hdc, wtext, (int)wcslen(wtext), &sz);
  if (sz.cx <= 0 || sz.cy <= 0) {
    SelectObject(hdc, old_font);
    DeleteObject(hfont);
    DeleteDC(hdc);
    SDL_free(wtext);
    return out;
  }

  BITMAPINFO bmi;
  ZeroMemory(&bmi, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = sz.cx;
  bmi.bmiHeader.biHeight = -sz.cy; // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *bits = NULL;
  HBITMAP dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (!dib || !bits) {
    SelectObject(hdc, old_font);
    DeleteObject(hfont);
    DeleteDC(hdc);
    SDL_free(wtext);
    return out;
  }

  HGDIOBJ old_bmp = SelectObject(hdc, dib);
  memset(bits, 0, (size_t)sz.cx * (size_t)sz.cy * 4);

  COLORREF gdi_color = RGB(color.r, color.g, color.b);
  SetTextColor(hdc, gdi_color);
  TextOutW(hdc, 0, 0, wtext, (int)wcslen(wtext));

  out.w = (int)sz.cx;
  out.h = (int)sz.cy;
  out.tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA8888, SDL_TEXTUREACCESS_STATIC, out.w,
                              out.h);
  if (out.tex) {
    SDL_UpdateTexture(out.tex, NULL, bits, out.w * 4);
    SDL_SetTextureBlendMode(out.tex, SDL_BLENDMODE_BLEND);
  }

  SelectObject(hdc, old_bmp);
  DeleteObject(dib);
  SelectObject(hdc, old_font);
  DeleteObject(hfont);
  DeleteDC(hdc);
  SDL_free(wtext);

  return out;
}
#else
TextUiItem make_text(SDL_Renderer *renderer, const char *utf8, int pt_size,
                     SDL_Color color) {
  (void)renderer;
  (void)utf8;
  (void)pt_size;
  (void)color;
  TextUiItem out = {0};
  return out;
}
#endif

void destroy_text(TextUiItem *t) {
  if (!t) return;
  if (t->tex) SDL_DestroyTexture(t->tex);
  t->tex = NULL;
  t->w = 0;
  t->h = 0;
}

void text_set(SDL_Renderer *renderer, TextUiItem *t, const char *utf8, int text_pt,
              SDL_Color color) {
  if (!t) return;
  destroy_text(t);
  if (!utf8) utf8 = "";
  *t = make_text(renderer, utf8, text_pt, color);
}

#ifdef _WIN32
int camui_measure_px(int pt_size, const char *utf8, int bytes) {
  if (!utf8) return 0;
  int slen = (int)strlen(utf8);
  if (bytes < 0 || bytes > slen) bytes = slen;
  if (bytes <= 0) return 0;

  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, bytes, NULL, 0);
  if (wlen <= 0) return 0;
  WCHAR *wtext = (WCHAR *)SDL_malloc((size_t)(wlen + 1) * sizeof(WCHAR));
  if (!wtext) return 0;
  if (MultiByteToWideChar(CP_UTF8, 0, utf8, bytes, wtext, wlen) <= 0) {
    SDL_free(wtext);
    return 0;
  }
  wtext[wlen] = 0;

  static HDC s_hdc = NULL;
  static HFONT s_font = NULL;
  static HFONT s_old = NULL;
  static int s_pt = 0;

  if (!s_hdc) s_hdc = CreateCompatibleDC(NULL);
  if (!s_hdc) {
    SDL_free(wtext);
    return 0;
  }

  if (!s_font || s_pt != pt_size) {
    if (s_font) {
      SelectObject(s_hdc, s_old);
      DeleteObject(s_font);
      s_font = NULL;
      s_old = NULL;
    }
    int height = -MulDiv(pt_size, GetDeviceCaps(s_hdc, LOGPIXELSY), 72);
    s_font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    s_pt = pt_size;
    if (s_font) s_old = (HFONT)SelectObject(s_hdc, s_font);
  }

  SIZE sz = {0, 0};
  GetTextExtentPoint32W(s_hdc, wtext, wlen, &sz);
  SDL_free(wtext);
  return (int)sz.cx;
}
#else
int camui_measure_px(int pt_size, const char *utf8, int bytes) {
  (void)pt_size;
  if (!utf8) return 0;
  int slen = (int)strlen(utf8);
  if (bytes < 0 || bytes > slen) bytes = slen;
  if (bytes <= 0) return 0;
  return bytes * 8;
}
#endif

int camui_utf8_prev(const char *s, int i) {
  if (!s || i <= 0) return 0;
  int j = i - 1;
  while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
  return j;
}

int camui_utf8_next(const char *s, int i) {
  if (!s) return 0;
  int n = (int)strlen(s);
  if (i < 0) i = 0;
  if (i >= n) return n;
  unsigned char c = (unsigned char)s[i];
  int step = 1;
  if ((c & 0x80) == 0x00) step = 1;
  else if ((c & 0xE0) == 0xC0) step = 2;
  else if ((c & 0xF0) == 0xE0) step = 3;
  else if ((c & 0xF8) == 0xF0) step = 4;
  int j = i + step;
  if (j > n) j = n;
  return j;
}

int camui_cursor_from_x(const char *s, int text_pt, int x_px) {
  if (!s) return 0;
  int len = (int)strlen(s);
  if (x_px <= 0) return 0;
  int full = camui_measure_px(text_pt, s, len);
  if (x_px >= full) return len;

  int cap = len + 2;
  int *offs = (int *)SDL_malloc((size_t)cap * sizeof(int));
  if (!offs) return 0;
  int k = 0;
  int i = 0;
  while (i < len && k + 1 < cap) {
    offs[k++] = i;
    int ni = camui_utf8_next(s, i);
    if (ni <= i) break;
    i = ni;
  }
  offs[k++] = len;

  int lo = 0;
  int hi = k - 1;
  while (lo + 1 < hi) {
    int mid = (lo + hi) / 2;
    int w = camui_measure_px(text_pt, s, offs[mid]);
    if (w <= x_px) lo = mid;
    else hi = mid;
  }
  int cursor = offs[lo];
  SDL_free(offs);
  return cursor;
}

int camui_insert_at(char *buf, size_t cap, int *cursor, int text_pt, const char *ins) {
  if (!buf || cap == 0 || !cursor || !ins) return -1;
  size_t n = strlen(buf);
  size_t m = strlen(ins);
  if (m == 0) return 0;
  if (*cursor < 0) *cursor = 0;
  if ((size_t)*cursor > n) *cursor = (int)n;
  if (n + m + 1 > cap) return -1;
  memmove(buf + *cursor + (int)m, buf + *cursor, n - (size_t)*cursor + 1);
  memcpy(buf + *cursor, ins, m);
  *cursor += (int)m;
  (void)text_pt;
  return 0;
}

void camui_backspace_at(char *buf, int *cursor) {
  if (!buf || !cursor) return;
  if (*cursor <= 0) return;
  int start = camui_utf8_prev(buf, *cursor);
  int end = *cursor;
  int len = (int)strlen(buf);
  memmove(buf + start, buf + end, (size_t)(len - end + 1));
  *cursor = start;
}

void camui_delete_at(char *buf, int *cursor) {
  if (!buf || !cursor) return;
  int len = (int)strlen(buf);
  if (*cursor < 0) *cursor = 0;
  if (*cursor >= len) return;
  int start = *cursor;
  int end = camui_utf8_next(buf, *cursor);
  memmove(buf + start, buf + end, (size_t)(len - end + 1));
}
