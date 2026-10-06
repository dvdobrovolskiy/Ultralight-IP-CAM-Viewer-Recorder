#pragma once

#include <SDL.h>

#include "app/camera.h"
#include "ui/text.h"
#include "ui/ui_render.h"

// Logical edit-form fields. The numbering is the identity used by edit_field,
// the per-field buffers and every input handler; the on-screen order is given
// by camera_manager_field_order().
enum {
  CAMMGR_FIELD_TYPE = 0,
  CAMMGR_FIELD_TITLE = 1,
  CAMMGR_FIELD_URL = 2,
  CAMMGR_FIELD_PREVIEW = 3,
  CAMMGR_FIELD_ONVIF_URL = 4,
  CAMMGR_FIELD_PTZ_ON = 5,
  CAMMGR_FIELD_PTZ_URL = 6,
  CAMMGR_FIELD_PTZ_USER = 7,
  CAMMGR_FIELD_PTZ_PASS = 8,
  CAMMGR_FIELD_ENABLED = 9,
  CAMMGR_FIELD_COUNT = 10
};

typedef struct CameraManager {
  SDL_Window *host_win;
  SDL_Renderer *ren;

  int is_open;
  int selected;
  int scroll;

  int mode; // 0 list, 1 edit
  int edit_index; // -1 add, >=0 edit
  int edit_field; // 0 type, 1 title, 2 url, 3 preview url, 4 onvif device url (ONVIF only)
  CameraType edit_type;
  char edit_title[256];
  char edit_url[2048];
  char edit_preview_url[2048];
  char edit_onvif_url[2048];
  int edit_enabled;
  int edit_ptz_on;
  char edit_ptz_url[2048];
  char edit_ptz_user[128];
  char edit_ptz_pass[128];
  int title_cursor;
  int url_cursor;
  int preview_cursor;
  int onvif_cursor;
  int ptz_url_cursor;
  int ptz_user_cursor;
  int ptz_pass_cursor;
  int title_cursor_px;
  int url_cursor_px;
  int preview_cursor_px;
  int onvif_cursor_px;
  int ptz_url_cursor_px;
  int ptz_user_cursor_px;
  int ptz_pass_cursor_px;

  TextUiItem header;
  TextUiItem btn_add;
  TextUiItem btn_edit;
  TextUiItem btn_delete;
  TextUiItem btn_close;
  TextUiItem btn_save;
  TextUiItem btn_cancel;

  TextUiItem lbl_type;
  TextUiItem lbl_title;
  TextUiItem lbl_url;
  TextUiItem lbl_preview_url;
  TextUiItem lbl_onvif_url;
  TextUiItem lbl_enabled;
  TextUiItem lbl_ptz_on;
  TextUiItem lbl_ptz_url;
  TextUiItem lbl_ptz_user;
  TextUiItem lbl_ptz_pass;

  // Read-only ONVIF metadata lines shown under the fields when editing an
  // ONVIF camera that has been probed (built in camera_manager_begin_edit).
  char edit_info1[256];
  char edit_info2[256];
  TextUiItem edit_info1_text;
  TextUiItem edit_info2_text;

  // One-line status at the bottom of the panel (ONVIF probe progress/result).
  char status[256];
  TextUiItem status_text;

  TextUiItem edit_type_text;
  TextUiItem edit_title_text;
  TextUiItem edit_url_text;
  TextUiItem edit_preview_url_text;
  TextUiItem edit_onvif_url_text;
  TextUiItem edit_enabled_text;
  TextUiItem edit_ptz_on_text;
  TextUiItem edit_ptz_url_text;
  TextUiItem edit_ptz_user_text;
  TextUiItem edit_ptz_pass_text;  // rendered masked

  CameraUiItem *list_ui;
  int list_count;
} CameraManager;

void camera_manager_free_list(CameraManager *mgr);
int camera_manager_rebuild_list(CameraManager *mgr, const CameraConfig *cams, int count,
                                int text_pt, SDL_Color color);
void camera_manager_update_edit_texts(CameraManager *mgr, int text_pt, SDL_Color color);
int camera_manager_open(CameraManager *mgr, SDL_Window *host_win, SDL_Renderer *renderer,
                        int selected, const CameraConfig *cams, int count, int text_pt,
                        SDL_Color color);
void camera_manager_close(CameraManager *mgr);
void camera_manager_begin_edit(CameraManager *mgr, int edit_index, const CameraConfig *cams,
                               int count, int text_pt, SDL_Color color);
void camera_manager_backspace(char *buf, size_t cap);
void camera_manager_append(char *buf, size_t cap, const char *text);
void camera_manager_set_status(CameraManager *mgr, const char *text, int text_pt, SDL_Color color);

// Which fields are shown, and where, depends on the camera type (ONVIF adds a
// device-URL field) and on the PTZ toggle (which reveals endpoint/credentials).
// The layout is computed in one place and used by both the renderer and the
// mouse hit-testing in main.c, so the two can never drift apart.
typedef struct CamMgrLayout {
  SDL_Rect main_rect;             // panel body
  SDL_Rect boxes[CAMMGR_FIELD_COUNT];  // per-field box; only valid where visible
  int visible[CAMMGR_FIELD_COUNT];
  int row_count;                  // number of visible rows
  int box_h;
  int label_gap;
  int content_bottom;             // y below the last row (info/status area)
} CamMgrLayout;

// Next camera type when the Type field is toggled. ONVIF is intentionally left
// out of the cycle — the type is hidden from the picker while all of its logic
// (probe, stream discovery, persistence, PTZ auto-config) stays live for
// cameras already configured as ONVIF. An ONVIF camera toggles out to RTSP.
// Re-enable by adding ONVIF back to the rotation here.
CameraType camera_manager_cycle_type(CameraType t);

// Visible fields, top to bottom, for the form's current type/PTZ state.
// Returns the count and fills `order` (must hold CAMMGR_FIELD_COUNT ints).
int camera_manager_field_order(CameraType type, int ptz_on, int *order);
// Tab order over the visible fields; follows the visual order.
int camera_manager_next_field(CameraType type, int ptz_on, int f);
// Geometry for the edit form. `mgr` supplies type + PTZ state.
void camera_manager_layout(const CameraManager *mgr, int win_w, int win_h, CamMgrLayout *out);
void camera_manager_render(CameraManager *mgr, int text_pt, SDL_Color text_color);
