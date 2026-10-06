#include "ui/ui_cam_mgr.h"

#include <stdio.h>
#include <string.h>

#include "app/config.h"
#include "app/i18n.h"

void camera_manager_free_list(CameraManager *mgr) {
  if (!mgr) return;
  free_camera_ui(mgr->list_ui, mgr->list_count);
  mgr->list_ui = NULL;
  mgr->list_count = 0;
}

int camera_manager_rebuild_list(CameraManager *mgr, const CameraConfig *cams, int count,
                                int text_pt, SDL_Color color) {
  if (!mgr || !mgr->ren) return -1;
  camera_manager_free_list(mgr);
  if (count <= 0) return 0;

  CameraUiItem *ui = (CameraUiItem *)SDL_calloc((size_t)count, sizeof(CameraUiItem));
  if (!ui) return -1;
  for (int i = 0; i < count; i++) {
    char label[512];
    const char *title = (cams[i].title_utf8 && *cams[i].title_utf8) ? cams[i].title_utf8 : "Camera";
    if (cams[i].enabled) {
      snprintf(label, sizeof(label), "%s [%s]", title, camera_type_to_string(cams[i].type));
    } else {
      snprintf(label, sizeof(label), "%s [%s] - %s", title, camera_type_to_string(cams[i].type),
               tr(STR_DISABLED));
    }
    TextUiItem t = make_text(mgr->ren, label, text_pt, color);
    ui[i].text_tex = t.tex;
    ui[i].text_w = t.w;
    ui[i].text_h = t.h;
    if (!ui[i].text_tex) {
      free_camera_ui(ui, count);
      return -1;
    }
  }
  mgr->list_ui = ui;
  mgr->list_count = count;

  if (mgr->selected < 0) mgr->selected = 0;
  if (mgr->selected >= count) mgr->selected = count - 1;
  if (mgr->scroll < 0) mgr->scroll = 0;
  if (mgr->scroll > count - 1) mgr->scroll = count - 1;
  return 0;
}

CameraType camera_manager_cycle_type(CameraType t) {
  return (t == CAMERA_TYPE_RTSP) ? CAMERA_TYPE_MJPEG : CAMERA_TYPE_RTSP;
}

int camera_manager_field_order(CameraType type, int ptz_on, int *order) {
  int n = 0;
  order[n++] = CAMMGR_FIELD_ENABLED;  // master switch first
  order[n++] = CAMMGR_FIELD_TYPE;
  order[n++] = CAMMGR_FIELD_TITLE;
  if (type == CAMERA_TYPE_ONVIF) order[n++] = CAMMGR_FIELD_ONVIF_URL;
  order[n++] = CAMMGR_FIELD_URL;
  order[n++] = CAMMGR_FIELD_PREVIEW;
  // PTZ is an ONVIF-protocol feature: offered for RTSP cameras (endpoint and
  // credentials entered by hand) and for ONVIF ones (filled in by the probe).
  if (type == CAMERA_TYPE_RTSP || type == CAMERA_TYPE_ONVIF) {
    order[n++] = CAMMGR_FIELD_PTZ_ON;
    if (ptz_on) {
      order[n++] = CAMMGR_FIELD_PTZ_URL;
      order[n++] = CAMMGR_FIELD_PTZ_USER;
      order[n++] = CAMMGR_FIELD_PTZ_PASS;
    }
  }
  return n;
}

int camera_manager_next_field(CameraType type, int ptz_on, int f) {
  int order[CAMMGR_FIELD_COUNT];
  int n = camera_manager_field_order(type, ptz_on, order);
  for (int i = 0; i < n; i++) {
    if (order[i] == f) return order[(i + 1) % n];
  }
  return order[0];
}

void camera_manager_layout(const CameraManager *mgr, int win_w, int win_h, CamMgrLayout *out) {
  if (!mgr || !out) return;
  memset(out, 0, sizeof(*out));

  const int pad = 18;
  const int btn_h = 44;
  SDL_Rect main_rect = {pad, 60, win_w - 2 * pad, win_h - 60 - btn_h - pad - 10};
  out->main_rect = main_rect;

  int order[CAMMGR_FIELD_COUNT];
  int n = camera_manager_field_order(mgr->edit_type, mgr->edit_ptz_on, order);
  out->row_count = n;

  // Each row is [label][label_gap][box][row_gap]. A label must read as
  // belonging to the box *below* it, so the gap separating it from the previous
  // row's box is kept several times larger than the gap to its own box — that
  // ratio is preserved even when rows have to compress to fit.
  int field_x = main_rect.x + 20;
  int field_w = main_rect.w - 40;

  int label_h = 0;
  const TextUiItem *label_probe[] = {&mgr->lbl_type, &mgr->lbl_title, &mgr->lbl_url,
                                     &mgr->lbl_preview_url, &mgr->lbl_onvif_url,
                                     &mgr->lbl_ptz_on, &mgr->lbl_ptz_url, &mgr->lbl_ptz_user,
                                     &mgr->lbl_ptz_pass, &mgr->lbl_enabled};
  for (int i = 0; i < (int)(sizeof(label_probe) / sizeof(label_probe[0])); i++) {
    if (label_probe[i]->h > label_h) label_h = label_probe[i]->h;
  }
  if (label_h <= 0) label_h = 18;  // labels not built yet

  const int label_gap = 5;    // label -> its own box: tight
  const int min_row_gap = 14; // previous box -> next label: never tighter than this
  const int min_box_h = 26;
  int row_gap = 24;
  int box_h = 44;

  // The first label is drawn above the first box, so the rows start low enough
  // for it to stay inside the panel.
  int top = main_rect.y + label_h + label_gap + 14;
  // Reserve room under the last row for the ONVIF info lines and status text.
  int reserve = (mgr->edit_type == CAMERA_TYPE_ONVIF) ? 72 : 36;
  int avail = main_rect.h - (top - main_rect.y) - reserve;
  if (avail < 60) avail = 60;

  int pitch = label_h + label_gap + box_h + row_gap;
  // Compress the inter-row gap first, then the boxes; both stop at a floor that
  // keeps every label unambiguously attached to its own input.
  if (n > 0 && n * pitch > avail) {
    int cut = (n * pitch - avail + n - 1) / n;
    if (cut > row_gap - min_row_gap) cut = row_gap - min_row_gap;
    if (cut > 0) {
      row_gap -= cut;
      pitch = label_h + label_gap + box_h + row_gap;
    }
  }
  if (n > 0 && n * pitch > avail) {
    int cut = (n * pitch - avail + n - 1) / n;
    if (cut > box_h - min_box_h) cut = box_h - min_box_h;
    if (cut > 0) {
      box_h -= cut;
      pitch = label_h + label_gap + box_h + row_gap;
    }
  }

  if (n > 0 && n * pitch > avail) {
    // Last resort (all nine rows in a very short window): split what is left,
    // still spending it on the row gap before the box so labels stay attached.
    int p = avail / n;
    box_h = p - label_h - label_gap - min_row_gap;
    if (box_h < 18) box_h = 18;
    pitch = label_h + label_gap + box_h + min_row_gap;
  }

  out->box_h = box_h;
  out->label_gap = label_gap;

  for (int i = 0; i < n; i++) {
    int f = order[i];
    SDL_Rect r = {field_x, top + i * pitch, field_w, box_h};
    out->boxes[f] = r;
    out->visible[f] = 1;
  }
  out->content_bottom = top + (n > 0 ? (n - 1) * pitch + box_h : 0) + 16;
}

// Password fields render as asterisks. Passwords are ASCII in practice, so a
// byte-per-byte mask also keeps cursor measurement accurate.
static void mask_text(const char *in, char *out, size_t out_sz) {
  size_t n = in ? strlen(in) : 0;
  if (n > out_sz - 1) n = out_sz - 1;
  for (size_t i = 0; i < n; i++) out[i] = '*';
  out[n] = 0;
}

void camera_manager_update_edit_texts(CameraManager *mgr, int text_pt, SDL_Color color) {
  if (!mgr || !mgr->ren) return;
  char type_buf[64];
  snprintf(type_buf, sizeof(type_buf), "%s", camera_type_to_string(mgr->edit_type));
  text_set(mgr->ren, &mgr->edit_type_text, type_buf, text_pt, color);
  text_set(mgr->ren, &mgr->edit_title_text, mgr->edit_title, text_pt, color);
  text_set(mgr->ren, &mgr->edit_url_text, mgr->edit_url, text_pt, color);
  text_set(mgr->ren, &mgr->edit_preview_url_text, mgr->edit_preview_url, text_pt, color);
  text_set(mgr->ren, &mgr->edit_onvif_url_text, mgr->edit_onvif_url, text_pt, color);
  text_set(mgr->ren, &mgr->edit_enabled_text, mgr->edit_enabled ? tr(STR_ON) : tr(STR_OFF), text_pt,
           color);
  text_set(mgr->ren, &mgr->edit_ptz_on_text, mgr->edit_ptz_on ? tr(STR_ON) : tr(STR_OFF), text_pt,
           color);
  text_set(mgr->ren, &mgr->edit_ptz_url_text, mgr->edit_ptz_url, text_pt, color);
  text_set(mgr->ren, &mgr->edit_ptz_user_text, mgr->edit_ptz_user, text_pt, color);
  {
    char masked[sizeof(mgr->edit_ptz_pass)];
    mask_text(mgr->edit_ptz_pass, masked, sizeof(masked));
    text_set(mgr->ren, &mgr->edit_ptz_pass_text, masked, text_pt, color);
  }

  int tlen = (int)strlen(mgr->edit_title);
  int ulen = (int)strlen(mgr->edit_url);
  int plen = (int)strlen(mgr->edit_preview_url);
  int olen = (int)strlen(mgr->edit_onvif_url);
  int zulen = (int)strlen(mgr->edit_ptz_url);
  int zslen = (int)strlen(mgr->edit_ptz_user);
  int zplen = (int)strlen(mgr->edit_ptz_pass);
  if (mgr->title_cursor < 0) mgr->title_cursor = 0;
  if (mgr->title_cursor > tlen) mgr->title_cursor = tlen;
  if (mgr->url_cursor < 0) mgr->url_cursor = 0;
  if (mgr->url_cursor > ulen) mgr->url_cursor = ulen;
  if (mgr->preview_cursor < 0) mgr->preview_cursor = 0;
  if (mgr->preview_cursor > plen) mgr->preview_cursor = plen;
  if (mgr->onvif_cursor < 0) mgr->onvif_cursor = 0;
  if (mgr->onvif_cursor > olen) mgr->onvif_cursor = olen;
  if (mgr->ptz_url_cursor < 0) mgr->ptz_url_cursor = 0;
  if (mgr->ptz_url_cursor > zulen) mgr->ptz_url_cursor = zulen;
  if (mgr->ptz_user_cursor < 0) mgr->ptz_user_cursor = 0;
  if (mgr->ptz_user_cursor > zslen) mgr->ptz_user_cursor = zslen;
  if (mgr->ptz_pass_cursor < 0) mgr->ptz_pass_cursor = 0;
  if (mgr->ptz_pass_cursor > zplen) mgr->ptz_pass_cursor = zplen;
  mgr->title_cursor_px = camui_measure_px(text_pt, mgr->edit_title, mgr->title_cursor);
  mgr->url_cursor_px = camui_measure_px(text_pt, mgr->edit_url, mgr->url_cursor);
  mgr->preview_cursor_px = camui_measure_px(text_pt, mgr->edit_preview_url, mgr->preview_cursor);
  mgr->onvif_cursor_px = camui_measure_px(text_pt, mgr->edit_onvif_url, mgr->onvif_cursor);
  mgr->ptz_url_cursor_px = camui_measure_px(text_pt, mgr->edit_ptz_url, mgr->ptz_url_cursor);
  mgr->ptz_user_cursor_px = camui_measure_px(text_pt, mgr->edit_ptz_user, mgr->ptz_user_cursor);
  {
    char masked[sizeof(mgr->edit_ptz_pass)];
    mask_text(mgr->edit_ptz_pass, masked, sizeof(masked));
    mgr->ptz_pass_cursor_px = camui_measure_px(text_pt, masked, mgr->ptz_pass_cursor);
  }
}

int camera_manager_open(CameraManager *mgr, SDL_Window *host_win, SDL_Renderer *renderer,
                        int selected, const CameraConfig *cams, int count, int text_pt,
                        SDL_Color color) {
  if (!mgr || mgr->is_open) return 0;
  if (!host_win || !renderer) return -1;

  mgr->host_win = host_win;
  mgr->ren = renderer;
  mgr->is_open = 1;
  mgr->selected = selected;
  mgr->scroll = 0;
  mgr->mode = 0;
  mgr->edit_index = -1;
  mgr->edit_field = 1;
  mgr->edit_type = CAMERA_TYPE_MJPEG;
  mgr->edit_title[0] = 0;
  mgr->edit_url[0] = 0;
  mgr->edit_preview_url[0] = 0;
  mgr->edit_onvif_url[0] = 0;
  mgr->edit_enabled = 1;
  mgr->edit_ptz_on = 0;
  mgr->edit_ptz_url[0] = 0;
  mgr->edit_ptz_user[0] = 0;
  mgr->edit_ptz_pass[0] = 0;
  mgr->title_cursor = 0;
  mgr->url_cursor = 0;
  mgr->preview_cursor = 0;
  mgr->onvif_cursor = 0;
  mgr->ptz_url_cursor = 0;
  mgr->ptz_user_cursor = 0;
  mgr->ptz_pass_cursor = 0;
  mgr->title_cursor_px = 0;
  mgr->url_cursor_px = 0;
  mgr->preview_cursor_px = 0;
  mgr->onvif_cursor_px = 0;
  mgr->ptz_url_cursor_px = 0;
  mgr->ptz_user_cursor_px = 0;
  mgr->ptz_pass_cursor_px = 0;

  mgr->header          = make_text(mgr->ren, tr(STR_CAMMGR_TITLE),      text_pt, color);
  mgr->btn_add         = make_text(mgr->ren, tr(STR_ADD),                text_pt, color);
  mgr->btn_edit        = make_text(mgr->ren, tr(STR_EDIT),               text_pt, color);
  mgr->btn_delete      = make_text(mgr->ren, tr(STR_DELETE),             text_pt, color);
  mgr->btn_close       = make_text(mgr->ren, tr(STR_CLOSE),              text_pt, color);
  mgr->btn_save        = make_text(mgr->ren, tr(STR_SAVE),               text_pt, color);
  mgr->btn_cancel      = make_text(mgr->ren, tr(STR_CANCEL),             text_pt, color);
  mgr->lbl_type        = make_text(mgr->ren, tr(STR_TYPE),               text_pt, color);
  mgr->lbl_title       = make_text(mgr->ren, tr(STR_TITLE),              text_pt, color);
  mgr->lbl_url         = make_text(mgr->ren, tr(STR_URL),                text_pt, color);
  mgr->lbl_preview_url = make_text(mgr->ren, tr(STR_PREVIEW_URL_LABEL),  text_pt, color);
  mgr->lbl_onvif_url   = make_text(mgr->ren, tr(STR_ONVIF_DEVICE_URL),   text_pt, color);
  mgr->lbl_enabled     = make_text(mgr->ren, tr(STR_ENABLED),            text_pt, color);
  mgr->lbl_ptz_on      = make_text(mgr->ren, tr(STR_PTZ),                text_pt, color);
  mgr->lbl_ptz_url     = make_text(mgr->ren, tr(STR_PTZ_URL),            text_pt, color);
  mgr->lbl_ptz_user    = make_text(mgr->ren, tr(STR_PTZ_LOGIN),          text_pt, color);
  mgr->lbl_ptz_pass    = make_text(mgr->ren, tr(STR_PTZ_PASSWORD),       text_pt, color);

  mgr->edit_info1[0] = 0;
  mgr->edit_info2[0] = 0;
  mgr->status[0] = 0;

  camera_manager_update_edit_texts(mgr, text_pt, color);
  if (camera_manager_rebuild_list(mgr, cams, count, text_pt, color) != 0) return -1;
  return 0;
}

void camera_manager_close(CameraManager *mgr) {
  if (!mgr || !mgr->is_open) return;
  camera_manager_free_list(mgr);

  destroy_text(&mgr->header);
  destroy_text(&mgr->btn_add);
  destroy_text(&mgr->btn_edit);
  destroy_text(&mgr->btn_delete);
  destroy_text(&mgr->btn_close);
  destroy_text(&mgr->btn_save);
  destroy_text(&mgr->btn_cancel);
  destroy_text(&mgr->lbl_type);
  destroy_text(&mgr->lbl_title);
  destroy_text(&mgr->lbl_url);
  destroy_text(&mgr->lbl_preview_url);
  destroy_text(&mgr->lbl_onvif_url);
  destroy_text(&mgr->lbl_enabled);
  destroy_text(&mgr->edit_enabled_text);
  destroy_text(&mgr->lbl_ptz_on);
  destroy_text(&mgr->lbl_ptz_url);
  destroy_text(&mgr->lbl_ptz_user);
  destroy_text(&mgr->lbl_ptz_pass);
  destroy_text(&mgr->edit_ptz_on_text);
  destroy_text(&mgr->edit_ptz_url_text);
  destroy_text(&mgr->edit_ptz_user_text);
  destroy_text(&mgr->edit_ptz_pass_text);
  destroy_text(&mgr->edit_info1_text);
  destroy_text(&mgr->edit_info2_text);
  destroy_text(&mgr->status_text);
  mgr->edit_info1[0] = 0;
  mgr->edit_info2[0] = 0;
  mgr->status[0] = 0;
  destroy_text(&mgr->edit_type_text);
  destroy_text(&mgr->edit_title_text);
  destroy_text(&mgr->edit_url_text);
  destroy_text(&mgr->edit_preview_url_text);
  destroy_text(&mgr->edit_onvif_url_text);

  mgr->ren = NULL;
  mgr->host_win = NULL;
  mgr->is_open = 0;
  mgr->mode = 0;
  mgr->edit_index = -1;
  SDL_StopTextInput();
}

void camera_manager_begin_edit(CameraManager *mgr, int edit_index, const CameraConfig *cams,
                               int count, int text_pt, SDL_Color color) {
  if (!mgr) return;
  mgr->mode = 1;
  if (edit_index < 0 || edit_index >= count) edit_index = -1;
  mgr->edit_index = edit_index;
  mgr->edit_field = 1;
  mgr->edit_info1[0] = 0;
  mgr->edit_info2[0] = 0;
  if (edit_index >= 0 && edit_index < count) {
    const CameraConfig *cam = &cams[edit_index];
    mgr->edit_type = cam->type;
    snprintf(mgr->edit_title, sizeof(mgr->edit_title), "%s", cam->title_utf8);
    // For ONVIF the URL/Preview fields hold the discovered (or hand-tuned)
    // stream URLs; the device endpoint has its own field below. Hide the
    // pre-probe placeholder (url == device URL) instead of showing it twice.
    if (cam->type == CAMERA_TYPE_ONVIF && cam->onvif_device_url && cam->url &&
        strcmp(cam->url, cam->onvif_device_url) == 0) {
      mgr->edit_url[0] = 0;
    } else {
      snprintf(mgr->edit_url, sizeof(mgr->edit_url), "%s", cam->url);
    }
    if (cam->preview_url && *cam->preview_url) {
      snprintf(mgr->edit_preview_url, sizeof(mgr->edit_preview_url), "%s", cam->preview_url);
    } else {
      mgr->edit_preview_url[0] = 0;
    }
    if (cam->onvif_device_url && *cam->onvif_device_url) {
      snprintf(mgr->edit_onvif_url, sizeof(mgr->edit_onvif_url), "%s", cam->onvif_device_url);
    } else {
      mgr->edit_onvif_url[0] = 0;
    }
    mgr->edit_enabled = cam->enabled ? 1 : 0;
    mgr->edit_ptz_on = cam->ptz_enabled ? 1 : 0;
    snprintf(mgr->edit_ptz_url, sizeof(mgr->edit_ptz_url), "%s", cam->ptz_url ? cam->ptz_url : "");
    snprintf(mgr->edit_ptz_user, sizeof(mgr->edit_ptz_user), "%s",
             cam->ptz_user ? cam->ptz_user : "");
    snprintf(mgr->edit_ptz_pass, sizeof(mgr->edit_ptz_pass), "%s",
             cam->ptz_pass ? cam->ptz_pass : "");
    if (cam->type == CAMERA_TYPE_ONVIF &&
        (cam->onvif_manufacturer || cam->onvif_model || cam->onvif_serial || cam->onvif_mac)) {
      snprintf(mgr->edit_info1, sizeof(mgr->edit_info1), "%s %s  FW: %s  SN: %s",
               cam->onvif_manufacturer ? cam->onvif_manufacturer : "?",
               cam->onvif_model ? cam->onvif_model : "?",
               cam->onvif_firmware ? cam->onvif_firmware : "?",
               cam->onvif_serial ? cam->onvif_serial : "?");
      snprintf(mgr->edit_info2, sizeof(mgr->edit_info2), "%s  IP: %s  MAC: %s  PTZ: %s",
               cam->onvif_name ? cam->onvif_name : "",
               cam->onvif_ip ? cam->onvif_ip : "?",
               cam->onvif_mac ? cam->onvif_mac : "?",
               cam->onvif_has_ptz ? "yes" : "no");
    }
  } else {
    mgr->edit_type = CAMERA_TYPE_RTSP;
    mgr->edit_title[0] = 0;
    mgr->edit_url[0] = 0;
    mgr->edit_preview_url[0] = 0;
    mgr->edit_onvif_url[0] = 0;
    mgr->edit_enabled = 1;  // new cameras start enabled
    mgr->edit_ptz_on = 0;
    mgr->edit_ptz_url[0] = 0;
    mgr->edit_ptz_user[0] = 0;
    mgr->edit_ptz_pass[0] = 0;
  }
  text_set(mgr->ren, &mgr->edit_info1_text, mgr->edit_info1, text_pt, color);
  text_set(mgr->ren, &mgr->edit_info2_text, mgr->edit_info2, text_pt, color);
  mgr->title_cursor = (int)strlen(mgr->edit_title);
  mgr->url_cursor = (int)strlen(mgr->edit_url);
  mgr->preview_cursor = (int)strlen(mgr->edit_preview_url);
  mgr->onvif_cursor = (int)strlen(mgr->edit_onvif_url);
  mgr->ptz_url_cursor = (int)strlen(mgr->edit_ptz_url);
  mgr->ptz_user_cursor = (int)strlen(mgr->edit_ptz_user);
  mgr->ptz_pass_cursor = (int)strlen(mgr->edit_ptz_pass);
  camera_manager_update_edit_texts(mgr, text_pt, color);
  SDL_StartTextInput();
}

void camera_manager_backspace(char *buf, size_t cap) {
  (void)cap;
  if (!buf) return;
  size_t n = strlen(buf);
  if (n == 0) return;
  buf[n - 1] = 0;
}

void camera_manager_append(char *buf, size_t cap, const char *text) {
  if (!buf || cap == 0 || !text) return;
  size_t n = strlen(buf);
  size_t m = strlen(text);
  if (n + m + 1 > cap) return;
  memcpy(buf + n, text, m + 1);
}

void camera_manager_set_status(CameraManager *mgr, const char *text, int text_pt, SDL_Color color) {
  if (!mgr || !mgr->is_open || !mgr->ren) return;
  snprintf(mgr->status, sizeof(mgr->status), "%s", text ? text : "");
  text_set(mgr->ren, &mgr->status_text, mgr->status, text_pt, color);
}

void camera_manager_render(CameraManager *mgr, int text_pt, SDL_Color text_color) {
  if (!mgr || !mgr->is_open || !mgr->ren) return;

  int win_w = 0, win_h = 0;
  if (!mgr->host_win) return;
  SDL_GetWindowSize(mgr->host_win, &win_w, &win_h);
  if (win_w <= 0 || win_h <= 0) return;

  SDL_SetRenderDrawColor(mgr->ren, 0, 0, 0, 180);
  SDL_Rect shade = {0, 0, win_w, win_h};
  SDL_RenderFillRect(mgr->ren, &shade);

  if (mgr->header.tex) {
    SDL_Rect r = {(win_w - mgr->header.w) / 2, 15, mgr->header.w, mgr->header.h};
    SDL_RenderCopy(mgr->ren, mgr->header.tex, NULL, &r);
  }

  const int pad = 18;
  const int row_h = 34;
  const int btn_h = 44;
  const int btn_w = 140;
  const int btn_gap = 12;

  SDL_Rect main_rect = {pad, 60, win_w - 2 * pad, win_h - 60 - btn_h - pad - 10};

  SDL_SetRenderDrawColor(mgr->ren, 15, 15, 15, 255);
  SDL_RenderFillRect(mgr->ren, &main_rect);
  SDL_SetRenderDrawColor(mgr->ren, 120, 120, 120, 255);
  SDL_RenderDrawRect(mgr->ren, &main_rect);

  if (mgr->mode == 0) {
    int visible = (main_rect.h / row_h);
    if (visible < 1) visible = 1;
    int max_scroll = (mgr->list_count > visible) ? (mgr->list_count - visible) : 0;
    if (mgr->scroll < 0) mgr->scroll = 0;
    if (mgr->scroll > max_scroll) mgr->scroll = max_scroll;

    for (int i = 0; i < visible; i++) {
      int idx = mgr->scroll + i;
      if (idx >= mgr->list_count) break;

      int y = main_rect.y + i * row_h;
      SDL_Rect row = {main_rect.x + 2, y + 2, main_rect.w - 4, row_h - 4};
      if (idx == mgr->selected) {
        SDL_SetRenderDrawColor(mgr->ren, 40, 120, 40, 220);
      } else {
        SDL_SetRenderDrawColor(mgr->ren, 35, 35, 35, 255);
      }
      SDL_RenderFillRect(mgr->ren, &row);

      if (mgr->list_ui && mgr->list_ui[idx].text_tex) {
        SDL_Rect tr = {row.x + 10, row.y + (row.h - mgr->list_ui[idx].text_h) / 2,
                       mgr->list_ui[idx].text_w, mgr->list_ui[idx].text_h};
        SDL_RenderCopy(mgr->ren, mgr->list_ui[idx].text_tex, NULL, &tr);
      }
    }

    int total_btns = 4;
    int bar_w = total_btns * btn_w + (total_btns - 1) * btn_gap;
    int x0 = (win_w - bar_w) / 2;
    int y0 = win_h - btn_h - pad;

    SDL_Rect btn_add = {x0 + 0 * (btn_w + btn_gap), y0, btn_w, btn_h};
    SDL_Rect btn_edit = {x0 + 1 * (btn_w + btn_gap), y0, btn_w, btn_h};
    SDL_Rect btn_del = {x0 + 2 * (btn_w + btn_gap), y0, btn_w, btn_h};
    SDL_Rect btn_close = {x0 + 3 * (btn_w + btn_gap), y0, btn_w, btn_h};

    SDL_SetRenderDrawColor(mgr->ren, 45, 45, 45, 255);
    SDL_RenderFillRect(mgr->ren, &btn_add);
    SDL_RenderFillRect(mgr->ren, &btn_edit);
    SDL_RenderFillRect(mgr->ren, &btn_del);
    SDL_RenderFillRect(mgr->ren, &btn_close);

    SDL_SetRenderDrawColor(mgr->ren, 160, 160, 160, 255);
    SDL_RenderDrawRect(mgr->ren, &btn_add);
    SDL_RenderDrawRect(mgr->ren, &btn_edit);
    SDL_RenderDrawRect(mgr->ren, &btn_del);
    SDL_RenderDrawRect(mgr->ren, &btn_close);

    TextUiItem *labels[] = {&mgr->btn_add, &mgr->btn_edit, &mgr->btn_delete, &mgr->btn_close};
    SDL_Rect *btns[] = {&btn_add, &btn_edit, &btn_del, &btn_close};
    for (int i = 0; i < total_btns; i++) {
      if (labels[i]->tex) {
        SDL_Rect tr = {btns[i]->x + (btns[i]->w - labels[i]->w) / 2,
                       btns[i]->y + (btns[i]->h - labels[i]->h) / 2, labels[i]->w, labels[i]->h};
        SDL_RenderCopy(mgr->ren, labels[i]->tex, NULL, &tr);
      }
    }
  } else {
    CamMgrLayout lay;
    camera_manager_layout(mgr, win_w, win_h, &lay);

    SDL_SetRenderDrawColor(mgr->ren, 45, 45, 45, 255);
    for (int f = 0; f < CAMMGR_FIELD_COUNT; f++) {
      if (lay.visible[f]) SDL_RenderFillRect(mgr->ren, &lay.boxes[f]);
    }

    SDL_Color focus = {120, 200, 120, 255};
    for (int f = 0; f < CAMMGR_FIELD_COUNT; f++) {
      if (!lay.visible[f]) continue;
      SDL_Color c = (mgr->edit_field == f) ? focus : (SDL_Color){140, 140, 140, 255};
      SDL_SetRenderDrawColor(mgr->ren, c.r, c.g, c.b, c.a);
      SDL_RenderDrawRect(mgr->ren, &lay.boxes[f]);
    }

    // Indexed by CAMMGR_FIELD_*.
    TextUiItem *labels[CAMMGR_FIELD_COUNT] = {
        &mgr->lbl_type,     &mgr->lbl_title,    &mgr->lbl_url,
        &mgr->lbl_preview_url, &mgr->lbl_onvif_url, &mgr->lbl_ptz_on,
        &mgr->lbl_ptz_url,  &mgr->lbl_ptz_user, &mgr->lbl_ptz_pass,
        &mgr->lbl_enabled};
    TextUiItem *values[CAMMGR_FIELD_COUNT] = {
        &mgr->edit_type_text,     &mgr->edit_title_text,    &mgr->edit_url_text,
        &mgr->edit_preview_url_text, &mgr->edit_onvif_url_text, &mgr->edit_ptz_on_text,
        &mgr->edit_ptz_url_text,  &mgr->edit_ptz_user_text, &mgr->edit_ptz_pass_text,
        &mgr->edit_enabled_text};
    for (int f = 0; f < CAMMGR_FIELD_COUNT; f++) {
      if (!lay.visible[f]) continue;
      SDL_Rect box = lay.boxes[f];
      if (labels[f]->tex) {
        SDL_Rect r = {box.x, box.y - labels[f]->h - lay.label_gap, labels[f]->w, labels[f]->h};
        SDL_RenderCopy(mgr->ren, labels[f]->tex, NULL, &r);
      }
      if (values[f]->tex) {
        SDL_Rect r = {box.x + 10, box.y + (box.h - values[f]->h) / 2, values[f]->w, values[f]->h};
        SDL_RenderCopy(mgr->ren, values[f]->tex, NULL, &r);
      }
    }

    // Read-only ONVIF metadata collected from the device (below the last row).
    if (mgr->edit_type == CAMERA_TYPE_ONVIF) {
      int info_y = lay.content_bottom;
      if (mgr->edit_info1_text.tex) {
        SDL_Rect r = {lay.main_rect.x + 20, info_y, mgr->edit_info1_text.w,
                      mgr->edit_info1_text.h};
        SDL_RenderCopy(mgr->ren, mgr->edit_info1_text.tex, NULL, &r);
        info_y += mgr->edit_info1_text.h + 6;
      }
      if (mgr->edit_info2_text.tex) {
        SDL_Rect r = {lay.main_rect.x + 20, info_y, mgr->edit_info2_text.w,
                      mgr->edit_info2_text.h};
        SDL_RenderCopy(mgr->ren, mgr->edit_info2_text.tex, NULL, &r);
      }
    }

    if (((SDL_GetTicks() / 500) % 2) == 0) {
      int cursors_px[CAMMGR_FIELD_COUNT] = {0,
                                            mgr->title_cursor_px,
                                            mgr->url_cursor_px,
                                            mgr->preview_cursor_px,
                                            mgr->onvif_cursor_px,
                                            0,
                                            mgr->ptz_url_cursor_px,
                                            mgr->ptz_user_cursor_px,
                                            mgr->ptz_pass_cursor_px,
                                            0};
      int f = mgr->edit_field;
      // Type, PTZ on/off and Enabled are toggles, not text fields.
      int is_text = (f >= 0 && f < CAMMGR_FIELD_COUNT && f != CAMMGR_FIELD_TYPE &&
                     f != CAMMGR_FIELD_PTZ_ON && f != CAMMGR_FIELD_ENABLED);
      if (is_text && lay.visible[f]) {
        SDL_Rect box = lay.boxes[f];
        SDL_SetRenderDrawColor(mgr->ren, 240, 240, 240, 255);
        int cx = box.x + 10 + cursors_px[f];
        if (cx < box.x + 6) cx = box.x + 6;
        if (cx > box.x + box.w - 6) cx = box.x + box.w - 6;
        SDL_RenderDrawLine(mgr->ren, cx, box.y + 8, cx, box.y + box.h - 8);
      }
    }

    int total_btns = 2;
    int bar_w = total_btns * btn_w + (total_btns - 1) * btn_gap;
    int x0 = (win_w - bar_w) / 2;
    int y0 = win_h - btn_h - pad;

    SDL_Rect btn_save = {x0, y0, btn_w, btn_h};
    SDL_Rect btn_cancel = {x0 + (btn_w + btn_gap), y0, btn_w, btn_h};

    SDL_SetRenderDrawColor(mgr->ren, 45, 45, 45, 255);
    SDL_RenderFillRect(mgr->ren, &btn_save);
    SDL_RenderFillRect(mgr->ren, &btn_cancel);
    SDL_SetRenderDrawColor(mgr->ren, 160, 160, 160, 255);
    SDL_RenderDrawRect(mgr->ren, &btn_save);
    SDL_RenderDrawRect(mgr->ren, &btn_cancel);

    if (mgr->btn_save.tex) {
      SDL_Rect tr = {btn_save.x + (btn_save.w - mgr->btn_save.w) / 2,
                     btn_save.y + (btn_save.h - mgr->btn_save.h) / 2, mgr->btn_save.w,
                     mgr->btn_save.h};
      SDL_RenderCopy(mgr->ren, mgr->btn_save.tex, NULL, &tr);
    }
    if (mgr->btn_cancel.tex) {
      SDL_Rect tr = {btn_cancel.x + (btn_cancel.w - mgr->btn_cancel.w) / 2,
                     btn_cancel.y + (btn_cancel.h - mgr->btn_cancel.h) / 2, mgr->btn_cancel.w,
                     mgr->btn_cancel.h};
      SDL_RenderCopy(mgr->ren, mgr->btn_cancel.tex, NULL, &tr);
    }
  }

  // Status line (ONVIF probe progress/result) pinned to the panel's bottom edge.
  if (mgr->status_text.tex) {
    SDL_Rect r = {main_rect.x + 12, main_rect.y + main_rect.h - mgr->status_text.h - 8,
                  mgr->status_text.w, mgr->status_text.h};
    SDL_RenderCopy(mgr->ren, mgr->status_text.tex, NULL, &r);
  }

  (void)text_color;
}
