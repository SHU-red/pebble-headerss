#include <pebble.h>

#include "storage.h"
#include "proto.h"
#include "tree.h"
#include "timeline.h"
#include "common.h"

// ---------------------------------------------------------------------------
// HeadeRSS — watchapp (C side). FreshRSS feed reader via the phone-side JS
// bridge (GReader API). The watch has no network: every feed tree and item
// page arrives over AppMessage; mark-read/star are batched and sent
// back. Windows: root menu (tree root), folder menus, the Timeline reading
// view (heading + summary rows, no detail view), the settings sub-menu and
// the shared result dialog — all mirroring the launcher's idioms.
// ---------------------------------------------------------------------------

#define READING_LIST_STREAM "user/-/state/com.google/reading-list"

#define RESULT_DISMISS_MS 1500 // final result auto-dismiss
#define PULSE_INTERVAL_MS 250  // working dialog animated ellipsis

// ---------------------------------------------------------------------------
// Result dialog (built from scratch, launcher pattern: colored background +
// centered white text + animated ellipsis while working; auto-dismiss for
// finals; orange confirm and info modes wait for input).
// ---------------------------------------------------------------------------

static Window *s_dialog_window;
static Layer *s_dialog_bg;
static TextLayer *s_dialog_text;
static AppTimer *s_timeout_timer;
static AppTimer *s_dismiss_timer;
static AppTimer *s_pulse_timer;
static bool s_dialog_active;
static bool s_dialog_confirm;
static bool s_dialog_info;
static bool s_confirm_markall; // the confirm dialog is about Mark all read
static char s_dialog_text_buf[192];
static char s_working_label[32];
static uint8_t s_pulse_phase;
static GColor s_dialog_color;

// ---------------------------------------------------------------------------
// Menus / windows
// ---------------------------------------------------------------------------

static Window *s_main_window;
static MenuLayer *s_main_menu;
static Window *s_folder_window;
static MenuLayer *s_folder_menu;
static char s_folder_id[48];
static char s_folder_name[32];
static Window *s_sub_window;
static MenuLayer *s_sub_menu;

// Folder marker: a small right-pointing triangle (U+25B6 is not covered by
// the system fonts), repositioned with gpath_move_to per draw.
static const GPathInfo ARROW_PATH_INFO = {
  .num_points = 3,
  .points = (GPoint[]) { { 0, -4 }, { 0, 4 }, { 8, 0 } },
};

static GPath *s_arrow_path;

static void push_folder_window(const char *id, const char *name);
static void push_submenu_window(void);

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

static void dialog_dismiss_cb(void *data);
static void dialog_show_working(const char *text);
static void dialog_unload(Window *window);

//! Fill the dialog background with the current color.
static void dialog_bg_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, s_dialog_color);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

static void dialog_confirm_select(ClickRecognizerRef rec, void *ctx) {
  if (s_dialog_info) {
    return; // info screen: SELECT does nothing, BACK leaves
  }
  if (s_confirm_markall) {
    // Mark all read: confirm in place — the working dialog repaints this
    // very window, so there is no pop/re-create race on the async unload.
    s_confirm_markall = false;
    s_dialog_confirm = false;
    dialog_show_working("Marking read");
    proto_mark_all_read(READING_LIST_STREAM);
    return;
  }
  if (!s_dialog_confirm) {
    return;
  }
  s_dialog_confirm = false;
  dialog_dismiss_cb(NULL);
}

static void dialog_confirm_cancel(ClickRecognizerRef rec, void *ctx) {
  if (s_dialog_info) {
    dialog_dismiss_cb(NULL);
    return;
  }
  if (!s_dialog_confirm) {
    return;
  }
  s_dialog_confirm = false;
  dialog_dismiss_cb(NULL);
}

static void dialog_click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, dialog_confirm_select);
  window_single_click_subscribe(BUTTON_ID_BACK, dialog_confirm_cancel);
}

static void dialog_dismiss_cb(void *data) {
  s_dismiss_timer = NULL;
  // Pop only the dialog: the app stays alive (exiting mid-AppMessage-stream
  // crashed on use-after-free of windows/animation objects).
  if (s_dialog_window) {
    window_stack_remove(s_dialog_window, true);
  }
}

//! A request the user is waiting on did not answer within REQUEST_TIMEOUT_MS.
static void request_timeout_cb(void *data) {
  s_timeout_timer = NULL;
  if (s_dialog_active) {
    ui_result(1, "Timeout");
  }
}

//! Animated ellipsis on the working dialog: "<label>", "<label>.", ...
static void pulse_tick_cb(void *data) {
  static const char *dots[] = { "", ".", "..", "..." };
  s_pulse_phase = (uint8_t)((s_pulse_phase + 1) % 4);
  char buf[40];
  snprintf(buf, sizeof(buf), "%s%s", s_working_label, dots[s_pulse_phase]);
  text_layer_set_text(s_dialog_text, buf);
}

static void dialog_cancel_timers(void) {
  if (s_timeout_timer) {
    app_timer_cancel(s_timeout_timer);
    s_timeout_timer = NULL;
  }
  if (s_dismiss_timer) {
    app_timer_cancel(s_dismiss_timer);
    s_dismiss_timer = NULL;
  }
  if (s_pulse_timer) {
    app_timer_cancel(s_pulse_timer);
    s_pulse_timer = NULL;
  }
}

static void dialog_create(void) {
  if (s_dialog_active) {
    return;
  }
  s_dialog_window = window_create();
  window_set_window_handlers(s_dialog_window, (WindowHandlers){
    .unload = dialog_unload,
  });
  window_set_click_config_provider(s_dialog_window, dialog_click_config_provider);

  Layer *root = window_get_root_layer(s_dialog_window);
  GRect bounds = layer_get_bounds(root);

  s_dialog_bg = layer_create(bounds);
  layer_set_update_proc(s_dialog_bg, dialog_bg_update_proc);
  s_dialog_color = GColorGreen;
  layer_add_child(root, s_dialog_bg);

  s_dialog_text = text_layer_create(GRect(8, (bounds.size.h - 100) / 2,
                                          bounds.size.w - 16, 100));
  text_layer_set_font(s_dialog_text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_dialog_text, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_dialog_text, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_dialog_text, GColorClear);
  text_layer_set_text_color(s_dialog_text, GColorWhite);
  layer_add_child(s_dialog_bg, text_layer_get_layer(s_dialog_text));

  s_dialog_active = true;
  window_stack_push(s_dialog_window, false);
}

//! Shared dialog setup: cancel timers, reset the interaction modes, paint
//! the background color, show white text and pulse.
static void dialog_prepare(GColor color, const char *text) {
  dialog_cancel_timers();
  s_dialog_confirm = false;
  s_dialog_info = false;
  s_dialog_color = color;
  layer_mark_dirty(s_dialog_bg);
  text_layer_set_text_color(s_dialog_text, GColorWhite);
  text_layer_set_text(s_dialog_text, text);
  vibes_short_pulse();
}

//! Green working dialog (no auto-dismiss, animated ellipsis, timeout).
static void dialog_show_working(const char *text) {
  if (!s_dialog_active) {
    dialog_create();
  }
  snprintf(s_working_label, sizeof(s_working_label), "%s", text ? text : "");
  dialog_prepare(GColorGreen, s_working_label);
  s_pulse_phase = 3;
  s_pulse_timer = app_timer_register(PULSE_INTERVAL_MS, pulse_tick_cb, NULL);
  s_timeout_timer = app_timer_register(REQUEST_TIMEOUT_MS, request_timeout_cb, NULL);
}

//! Green (success) or red (failure) final dialog, auto-dismissed after 1.5s.
static void dialog_show_final(bool success, const char *text) {
  if (!s_dialog_active) {
    return;
  }
  dialog_prepare(success ? GColorGreen : GColorRed, text);
  if (!success) {
    vibes_double_pulse();
  }
  s_dismiss_timer = app_timer_register(RESULT_DISMISS_MS, dialog_dismiss_cb, NULL);
}

//! Orange approval screen: one more SELECT confirms, BACK cancels.
static void dialog_show_confirm_text(const char *text) {
  if (!s_dialog_active) {
    dialog_create();
  }
  snprintf(s_dialog_text_buf, sizeof(s_dialog_text_buf), "%s", text ? text : "");
  dialog_prepare(GColorOrange, s_dialog_text_buf);
  s_dialog_confirm = true;
}

//! Orange info screen: stays until BACK.
static void dialog_show_info(const char *text) {
  if (!s_dialog_active) {
    dialog_create();
  }
  snprintf(s_dialog_text_buf, sizeof(s_dialog_text_buf), "%s", text ? text : "");
  dialog_prepare(GColorOrange, s_dialog_text_buf);
  s_dialog_info = true;
}

static void dialog_unload(Window *window) {
  dialog_cancel_timers();
  text_layer_destroy(s_dialog_text);
  layer_destroy(s_dialog_bg);
  window_destroy(s_dialog_window);
  s_dialog_window = NULL;
  s_dialog_active = false;
}

// ---------------------------------------------------------------------------
// Global UI hooks (called from proto.c / tree.c)
// ---------------------------------------------------------------------------

bool ui_result_active(void) {
  return s_dialog_active;
}

//! Global result/error surface. A nonzero code shows a red dialog with the
//! text (auth failures get a setup hint); success repaints the working
//! dialog green. The working dialog is repainted in place (launcher idiom)
//! rather than popped and re-created: the unload callback is asynchronous,
//! so popping here and creating a fresh dialog would race on the flag.
void ui_result(int code, const char *text) {
  if (code == 0) {
    if (s_dialog_active) {
      dialog_show_final(true, "Done!");
    }
    return;
  }
  if (!s_dialog_active) {
    dialog_create();
  }
  char msg[192];
  snprintf(msg, sizeof(msg), "%s", (text && text[0]) ? text : "Error");
  if (strstr(msg, "login") || strstr(msg, "401") || strstr(msg, "Unauthorized")) {
    size_t l = strlen(msg);
    snprintf(msg + l, sizeof(msg) - l,
             "\n\nSet API password in phone settings");
  }
  dialog_show_final(false, msg);
}

//! A fresh tree arrived: refresh the visible tree menus and hide any
//! working dialog ("Loading..." / "Refreshing...").
void ui_tree_updated(void) {
  if (s_main_menu) {
    menu_layer_reload_data(s_main_menu);
  }
  if (s_folder_menu) {
    menu_layer_reload_data(s_folder_menu);
  }
  if (s_dialog_active) {
    dialog_dismiss_cb(NULL);
  }
}

//! Apply accent/theme to every live window (launcher apply_accent/apply_theme).
void apply_settings(void) {
  if (s_main_window) {
    window_set_background_color(s_main_window, theme_bg());
  }
  if (s_main_menu) {
    menu_layer_set_normal_colors(s_main_menu, theme_bg(), theme_fg());
    menu_layer_set_highlight_colors(s_main_menu, s_accent, GColorBlack);
  }
  if (s_folder_window) {
    window_set_background_color(s_folder_window, theme_bg());
  }
  if (s_folder_menu) {
    menu_layer_set_normal_colors(s_folder_menu, theme_bg(), theme_fg());
    menu_layer_set_highlight_colors(s_folder_menu, s_accent, GColorBlack);
  }
  if (s_sub_window) {
    window_set_background_color(s_sub_window, theme_bg());
  }
  if (s_sub_menu) {
    menu_layer_set_normal_colors(s_sub_menu, theme_bg(), theme_fg());
    menu_layer_set_highlight_colors(s_sub_menu, s_accent, GColorBlack);
  }
  timeline_apply_settings();
}

// ---------------------------------------------------------------------------
// Root menu (mirrors the launcher main window)
// ---------------------------------------------------------------------------

static uint16_t main_get_num_sections(MenuLayer *menu_layer, void *callback_context) {
  return 1;
}

static uint16_t main_total_rows(void) {
  int root = tree_root_count();
  return (uint16_t)(1 + (root > 0 ? root : 1));
}

static uint16_t main_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                  void *callback_context) {
  return main_total_rows();
}

//! Row 0 is a narrow accent entry row (UP opens the sub-menu); tree rows
//! keep a comfortable touch target.
static int16_t main_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index,
                                    void *callback_context) {
  return cell_index->row == 0 ? 15 : 46;
}

//! Permanent accent spine on the right edge (Timeline style); on the accent
//! selection row it inverts to black so it stays visible.
static void draw_spine(GContext *ctx, GRect b, bool selected) {
  graphics_context_set_fill_color(ctx, selected ? GColorBlack : s_accent);
  graphics_fill_rect(ctx, GRect(b.size.w - 6, 0, 4, b.size.h), 0, GCornerNone);
}

//! Right-aligned unread badge as a filled accent pill (black count); on the
//! accent selection row the pill inverts (black pill, white count).
static void draw_badge(GContext *ctx, GRect b, int32_t unread, bool selected) {
  if (unread <= 0) {
    return;
  }
  char num[12];
  snprintf(num, sizeof(num), "%ld", (long)unread);
  int d = 1;
  int32_t v = unread;
  while ((v /= 10) > 0) {
    d++;
  }
  int16_t pw = 14 + d * 8; // pill width grows with the digit count
  GRect pill = GRect(b.size.w - 12 - pw, (b.size.h - 16) / 2, pw, 16);
  graphics_context_set_fill_color(ctx, selected ? GColorBlack : s_accent);
  graphics_fill_rect(ctx, pill, 8, GCornersAll);
  graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorBlack);
  graphics_draw_text(ctx, num, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(pill.origin.x, pill.origin.y - 1, pill.size.w, pill.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

//! Folder marker: small right-pointing triangle; feeds/specials get none.
static void draw_folder_marker(GContext *ctx, GRect b, bool selected) {
  if (!s_arrow_path) {
    return;
  }
  graphics_context_set_fill_color(ctx, selected ? GColorBlack : s_accent);
  gpath_move_to(s_arrow_path, GPoint(12, b.size.h / 2));
  gpath_draw_filled(ctx, s_arrow_path);
}

static void main_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                          void *callback_context) {
  uint16_t row = cell_index->row;
  GRect bounds = layer_get_bounds(cell_layer);

  if (row == 0) {
    // Accent strip: the UP entry row doubles as the app's color header.
    graphics_context_set_fill_color(ctx, s_accent);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    int16_t cx = bounds.size.w / 2;
    int16_t cy = bounds.size.h / 2;
    graphics_context_set_fill_color(ctx, GColorBlack);
    for (int i = -1; i <= 1; i++) {
      graphics_fill_circle(ctx, GPoint(cx + i * 6, cy), 2);
    }
    return;
  }

  int root = tree_root_count();
  if (row - 1 >= (uint16_t)root) {
    menu_cell_basic_draw(ctx, cell_layer, "No feeds yet",
                         "Open the phone app settings", NULL);
    return;
  }

  const FeedNode *node = tree_root_node(row - 1);
  GRect b = layer_get_bounds(cell_layer);
  bool selected = menu_layer_is_index_selected(s_main_menu, (MenuIndex *)cell_index);

  // Row background: accent when selected, theme otherwise.
  graphics_context_set_fill_color(ctx, selected ? s_accent : theme_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  draw_spine(ctx, b, selected);

  int16_t text_x = 8;
  if (node->kind == 1) {
    draw_folder_marker(ctx, b, selected);
    text_x = 20;
  }

  graphics_context_set_text_color(ctx, selected ? GColorBlack : theme_fg());
  graphics_draw_text(ctx, node->name[0] ? node->name : node->id,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(text_x, (b.size.h - 22) / 2, b.size.w - text_x - 38, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  draw_badge(ctx, b, node->unread, selected);
}

static void main_select_cb(MenuLayer *menu_layer, MenuIndex *cell_index,
                           void *callback_context) {
  uint16_t row = cell_index->row;
  if (row == 0) {
    push_submenu_window();
    return;
  }
  int root = tree_root_count();
  if (row - 1 >= (uint16_t)root) {
    push_submenu_window(); // empty state routes to the sub-menu too
    return;
  }
  const FeedNode *node = tree_root_node(row - 1);
  if (node->kind == 1) {
    push_folder_window(node->id, node->name);
  } else {
    // Specials ("All unread", "Starred") and feeds open the timeline.
    timeline_open(node->id, node->name);
  }
}

// Custom click handling so UP on the first entry opens the sub-menu and the
// selection starts on the first tree row (not the dots row) — launcher idiom.
static void main_up_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_main_menu);
  if (idx.row <= 1) {
    push_submenu_window(); // push upwards on the first entry
    return;
  }
  idx.row--;
  menu_layer_set_selected_index(s_main_menu, idx, MenuRowAlignCenter, true);
}

static void main_down_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_main_menu);
  if (idx.row + 1 < main_total_rows()) {
    idx.row++;
    menu_layer_set_selected_index(s_main_menu, idx, MenuRowAlignCenter, true);
  }
}

static void main_select_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_main_menu);
  main_select_cb(s_main_menu, &idx, NULL);
}

static void main_click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, main_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, main_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, main_select_click);
}

static void main_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  window_set_background_color(window, theme_bg());

  s_main_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_main_menu, NULL, (MenuLayerCallbacks){
    .get_num_sections = main_get_num_sections,
    .get_num_rows = main_get_num_rows,
    .get_cell_height = main_get_cell_height,
    .draw_row = main_draw_row,
    .select_click = main_select_cb,
  });
  window_set_click_config_provider(window, main_click_config_provider);
  menu_layer_pad_bottom_enable(s_main_menu, true);
  // Dark/light rows with accent highlight.
  menu_layer_set_normal_colors(s_main_menu, theme_bg(), theme_fg());
  menu_layer_set_highlight_colors(s_main_menu, s_accent, GColorBlack);
  layer_add_child(root, menu_layer_get_layer(s_main_menu));
  // Always start on the first tree row.
  MenuIndex first = { .section = 0, .row = 1 };
  menu_layer_set_selected_index(s_main_menu, first, MenuRowAlignCenter, true);
}

static void main_window_unload(Window *window) {
  menu_layer_destroy(s_main_menu);
  s_main_menu = NULL;
}

static void main_window_appear(Window *window) {
  menu_layer_reload_data(s_main_menu); // badges may have changed underneath
}

static void push_main_window(void) {
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load = main_window_load,
    .appear = main_window_appear,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
}

// ---------------------------------------------------------------------------
// Folder window: "All articles" + child folders, then child feeds.
// ---------------------------------------------------------------------------

static uint16_t folder_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                    void *callback_context) {
  return (uint16_t)(1 + tree_child_count(s_folder_id));
}

static int16_t folder_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index,
                                      void *callback_context) {
  return 46;
}

static void folder_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                            void *callback_context) {
  GRect b = layer_get_bounds(cell_layer);
  bool selected = menu_layer_is_index_selected(s_folder_menu, (MenuIndex *)cell_index);

  graphics_context_set_fill_color(ctx, selected ? s_accent : theme_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  draw_spine(ctx, b, selected);

  const char *label;
  int32_t unread;
  int16_t text_x = 8;

  if (cell_index->row == 0) {
    // "All articles": the folder's own stream id opens the recursive listing.
    label = "All articles";
    const FeedNode *f = tree_find(s_folder_id);
    unread = f ? f->unread : 0;
    draw_folder_marker(ctx, b, selected);
    text_x = 20;
  } else {
    const FeedNode *node = tree_child_node(s_folder_id, cell_index->row - 1);
    if (!node) {
      return;
    }
    label = node->name[0] ? node->name : node->id;
    unread = node->unread;
    if (node->kind == 1) {
      draw_folder_marker(ctx, b, selected);
      text_x = 20;
    }
  }

  graphics_context_set_text_color(ctx, selected ? GColorBlack : theme_fg());
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(text_x, (b.size.h - 22) / 2, b.size.w - text_x - 38, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  draw_badge(ctx, b, unread, selected);
}

static void folder_select_cb(MenuLayer *menu_layer, MenuIndex *cell_index,
                             void *callback_context) {
  if (cell_index->row == 0) {
    timeline_open(s_folder_id, s_folder_name);
    return;
  }
  const FeedNode *node = tree_child_node(s_folder_id, cell_index->row - 1);
  if (!node) {
    return;
  }
  if (node->kind == 1) {
    push_folder_window(node->id, node->name);
  } else {
    timeline_open(node->id, node->name);
  }
}

static void folder_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  window_set_background_color(window, theme_bg());

  s_folder_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_folder_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = folder_get_num_rows,
    .get_cell_height = folder_get_cell_height,
    .draw_row = folder_draw_row,
    .select_click = folder_select_cb,
  });
  menu_layer_set_click_config_onto_window(s_folder_menu, window);
  menu_layer_pad_bottom_enable(s_folder_menu, true);
  menu_layer_set_normal_colors(s_folder_menu, theme_bg(), theme_fg());
  menu_layer_set_highlight_colors(s_folder_menu, s_accent, GColorBlack);
  layer_add_child(root, menu_layer_get_layer(s_folder_menu));
}

static void folder_window_unload(Window *window) {
  menu_layer_destroy(s_folder_menu);
  s_folder_menu = NULL;
  window_destroy(s_folder_window);
  s_folder_window = NULL;
}

static void folder_window_appear(Window *window) {
  menu_layer_reload_data(s_folder_menu); // badges may have changed underneath
}

//! Fresh window per push (nested folders would collide on one instance).
static void push_folder_window(const char *id, const char *name) {
  snprintf(s_folder_id, sizeof(s_folder_id), "%s", id ? id : "");
  snprintf(s_folder_name, sizeof(s_folder_name), "%s", name ? name : "");
  s_folder_window = window_create();
  window_set_window_handlers(s_folder_window, (WindowHandlers){
    .load = folder_window_load,
    .appear = folder_window_appear,
    .unload = folder_window_unload,
  });
  window_stack_push(s_folder_window, true);
}

// ---------------------------------------------------------------------------
// Sub-menu (UP from the root): Refresh / Mark all read / Connection / the two
// reading toggles / Unread only — flat, no submenus
// ---------------------------------------------------------------------------

static uint16_t sub_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                 void *callback_context) {
  return 6;
}

static void sub_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                         void *callback_context) {
  GRect b = layer_get_bounds(cell_layer);
  bool selected = menu_layer_is_index_selected(s_sub_menu, (MenuIndex *)cell_index);
  draw_spine(ctx, b, selected);
  if (cell_index->row == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Refresh",
                         "Reload feeds from the server", NULL);
  } else if (cell_index->row == 1) {
    menu_cell_basic_draw(ctx, cell_layer, "Mark all read",
                         "All articles in every feed", NULL);
  } else if (cell_index->row == 2) {
    menu_cell_basic_draw(ctx, cell_layer, "Connection",
                         "How to set up the server", NULL);
  } else if (cell_index->row == 3) {
    menu_cell_basic_draw(ctx, cell_layer, "Mark read on list",
                         s_mark_list ? "ON" : "OFF", NULL);
  } else if (cell_index->row == 4) {
    menu_cell_basic_draw(ctx, cell_layer, "Mark read on detail",
                         s_mark_detail ? "ON" : "OFF", NULL);
  } else {
    menu_cell_basic_draw(ctx, cell_layer, "Unread only",
                         s_unread_only ? "ON" : "OFF", NULL);
  }
}

static void sub_select_cb(MenuLayer *menu_layer, MenuIndex *cell_index,
                          void *callback_context) {
  if (cell_index->row == 0) {
    dialog_show_working("Refreshing");
    proto_request_tree();
  } else if (cell_index->row == 1) {
    s_confirm_markall = true;
    dialog_show_confirm_text("Mark all read?\n\nSELECT: confirm\nBACK: cancel");
  } else if (cell_index->row == 2) {
    dialog_show_info("Set server + API password in the phone app settings");
  } else if (cell_index->row == 3) {
    s_mark_list = !s_mark_list;
    storage_save_settings();
    vibes_short_pulse();
    menu_layer_reload_data(menu_layer);
  } else if (cell_index->row == 4) {
    s_mark_detail = !s_mark_detail;
    storage_save_settings();
    vibes_short_pulse();
    menu_layer_reload_data(menu_layer);
  } else {
    s_unread_only = !s_unread_only;
    storage_save_settings();
    vibes_short_pulse();
    menu_layer_reload_data(menu_layer);
  }
}

static void sub_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, theme_bg());
  s_sub_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_sub_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = sub_get_num_rows,
    .draw_row = sub_draw_row,
    .select_click = sub_select_cb,
  });
  menu_layer_set_click_config_onto_window(s_sub_menu, window);
  menu_layer_pad_bottom_enable(s_sub_menu, true);
  menu_layer_set_normal_colors(s_sub_menu, theme_bg(), theme_fg());
  menu_layer_set_highlight_colors(s_sub_menu, s_accent, GColorBlack);
  layer_add_child(root, menu_layer_get_layer(s_sub_menu));
}

static void sub_window_unload(Window *window) {
  menu_layer_destroy(s_sub_menu);
  s_sub_menu = NULL;
  window_destroy(s_sub_window);
  s_sub_window = NULL;
}

static void push_submenu_window(void) {
  s_sub_window = window_create();
  window_set_window_handlers(s_sub_window, (WindowHandlers){
    .load = sub_window_load,
    .unload = sub_window_unload,
  });
  window_stack_push(s_sub_window, true);
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

static void init(void) {
  storage_load();
  proto_init(); // registers handlers + app_message_open(4096, 1024)

  s_arrow_path = gpath_create(&ARROW_PATH_INFO);

  // Native touch navigation, only when the user enables it in settings.
  (void)app_touch_navigation_enable(s_touch);

  push_main_window();

  // Cached tree = instant start; refresh in the background. Otherwise a
  // working dialog covers the wait for the first real tree.
  if (tree_load_cache() > 0) {
    proto_request_tree();
  } else {
    dialog_show_working("Loading");
    proto_request_tree();
  }
}

static void deinit(void) {
  if (s_arrow_path) {
    gpath_destroy(s_arrow_path);
    s_arrow_path = NULL;
  }
  if (s_main_window) {
    window_destroy(s_main_window);
    s_main_window = NULL;
  }
  // Folder/sub/timeline/detail/dialog windows destroy themselves in their
  // unload handlers; anything still on the stack is cleaned up at exit.
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
