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
#define STARRED_STREAM "user/-/state/com.google/starred"
#define IMPORTANT_STREAM "user/-/state/org.freshrss/important"

#define RESULT_DISMISS_MS 1500 // final result auto-dismiss
#define PULSE_INTERVAL_MS 250  // working dialog animated ellipsis
#define LONG_SELECT_MS 700     // context menu long-press threshold

// ---------------------------------------------------------------------------
// Result dialog (built from scratch, launcher pattern: colored background +
// centered white text + animated ellipsis while working; auto-dismiss for
// finals; orange confirm mode waits for input).
// ---------------------------------------------------------------------------

static Window *s_dialog_window;
static Layer *s_dialog_bg;
static TextLayer *s_dialog_text;
static AppTimer *s_timeout_timer;
static AppTimer *s_dismiss_timer;
static AppTimer *s_pulse_timer;
static bool s_dialog_active;
static bool s_dialog_dismissing; // async pop in flight (unload not yet run)
static bool s_dialog_final;      // a final result is shown; stop pulsing
static bool s_dialog_confirm;
static bool s_confirm_markall; // the confirm dialog is about Mark all read
static char s_markall_stream[48]; // stream the confirm targets ("" = reading list)
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
static Window *s_mode_window; // auto-mark mode selection (sub-menu -> window)
static MenuLayer *s_mode_menu;

// Nav icons: leading glyphs for menu rows (pin = Important, star = Starred,
// folder = folders, news = feeds; "All articles"/"All unread" get none).
static const GPathInfo PIN_PATH_INFO = {
  .num_points = 6,
  .points = (GPoint[6]){
    { 0, -7 }, { 4, -3 }, { 3, 2 }, { 0, 7 }, { -3, 2 }, { -4, -3 },
  },
};
static const GPathInfo STAR_ICON_INFO = {
  .num_points = 10,
  .points = (GPoint[10]){
    { 0, -7 }, { 2, -2 }, { 7, -2 }, { 3, 1 }, { 5, 7 },
    { 0, 4 }, { -5, 7 }, { -3, 1 }, { -7, -2 }, { -2, -2 },
  },
};
static const GPathInfo FOLDER_ICON_INFO = {
  .num_points = 8,
  .points = (GPoint[8]){
    { -9, -3 }, { -2, -3 }, { 0, -1 }, { 9, -1 }, { 9, 6 }, { -9, 6 },
    { -9, -3 }, { -2, -1 },
  },
};
static const GPathInfo NEWS_ICON_INFO = {
  .num_points = 6,
  .points = (GPoint[6]){
    { -5, -7 }, { 5, -7 }, { 7, -5 }, { 7, 7 }, { -7, 7 }, { -7, -5 },
  },
};
static GPath *s_icon_pin;
static GPath *s_icon_star;
static GPath *s_icon_folder;
static GPath *s_icon_news;

static void push_folder_window(const char *id, const char *name);
static void push_submenu_window(void);
static void push_mode_window(void);
static void open_context_menu(const FeedNode *feed);

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
  if (s_confirm_markall) {
    // Mark all read: confirm in place — the working dialog repaints this
    // very window, so there is no pop/re-create race on the async unload.
    const char *stream = s_markall_stream[0] ? s_markall_stream
                                             : READING_LIST_STREAM;
    s_confirm_markall = false;
    s_dialog_confirm = false;
    dialog_show_working("Marking read");
    proto_mark_all_read(stream);
    // Optimistic badge update: the counts hit 0 immediately (the phone
    // syncs the server in the background; a later refresh re-verifies).
    tree_mark_all_read(stream);
    ui_tree_updated();
    return;
  }
  if (!s_dialog_confirm) {
    return;
  }
  s_dialog_confirm = false;
  dialog_dismiss_cb(NULL);
}

static void dialog_confirm_cancel(ClickRecognizerRef rec, void *ctx) {
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
  if (s_dialog_window && !s_dialog_dismissing) {
    s_dialog_dismissing = true;
    // Pop only the dialog: the app stays alive (exiting mid-AppMessage-stream
    // crashed on use-after-free of windows/animation objects). The unload is
    // asynchronous, so block a second dismiss from racing the pop.
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
//! Reschedules itself while the dialog is alive. MUST re-check the dialog
//! state: the dismiss/unload path is asynchronous (window_stack_remove ->
//! unload), so this callback can be queued behind the cancel that popped
//! the dialog — touching the freed text layer then faults the app.
static void pulse_tick_cb(void *data) {
  if (!s_dialog_active || !s_dialog_text || s_dialog_final) {
    return;
  }
  static const char *dots[] = { "", ".", "..", "..." };
  s_pulse_phase = (uint8_t)((s_pulse_phase + 1) % 4);
  char buf[40];
  snprintf(buf, sizeof(buf), "%s%s", s_working_label, dots[s_pulse_phase]);
  text_layer_set_text(s_dialog_text, buf);
  if (s_dialog_active) {
    s_pulse_timer = app_timer_register(PULSE_INTERVAL_MS, pulse_tick_cb, NULL);
  }
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

  s_dialog_text = text_layer_create(GRect(8, (bounds.size.h - 120) / 2,
                                          bounds.size.w - 16, 120));
  text_layer_set_font(s_dialog_text, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_dialog_text, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_dialog_text, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_dialog_text, GColorClear);
  text_layer_set_text_color(s_dialog_text, GColorWhite);
  layer_add_child(s_dialog_bg, text_layer_get_layer(s_dialog_text));

  s_dialog_active = true;
  s_dialog_dismissing = false;
  s_dialog_final = false;
  window_stack_push(s_dialog_window, false);
}

//! Shared dialog setup: cancel timers, reset the interaction modes, paint
//! the background color, show white text and pulse.
static void dialog_prepare(GColor color, const char *text) {
  dialog_cancel_timers();
  s_dialog_confirm = false;
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
  s_dialog_final = false;
  text_layer_set_font(s_dialog_text,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
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
  s_dialog_final = true;
  // Multiline results (e.g. the Connection account block) get the smaller
  // font so all lines fit the dialog's text area.
  if (text && strchr(text, '\n')) {
    text_layer_set_font(s_dialog_text,
                        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
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

static void dialog_unload(Window *window) {
  dialog_cancel_timers();
  text_layer_destroy(s_dialog_text);
  layer_destroy(s_dialog_bg);
  window_destroy(s_dialog_window);
  s_dialog_window = NULL;
  s_dialog_active = false;
  s_dialog_dismissing = false;
  s_dialog_final = false;
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
//! Bounded case-insensitive substring test. Deliberately NOT strstr: libc's
//! strstr (two-way algorithm) has a deep stack frame, and the app stack is
//! only 2 KB on the basalt-class platforms — a stack overflow there faulted
//! with a corrupted PC inside strstr's internals at startup.
static bool contains_ci(const char *hay, const char *needle) {
  size_t hl = strlen(hay);
  size_t nl = strlen(needle);
  if (nl == 0 || nl > hl) {
    return false;
  }
  for (size_t i = 0; i + nl <= hl; i++) {
    size_t k = 0;
    while (k < nl && ((hay[i + k] | 0x20) == (needle[k] | 0x20))) {
      k++;
    }
    if (k == nl) {
      return true;
    }
  }
  return false;
}

void ui_result(int code, const char *text) {
  if (code == 0) {
    if (s_dialog_active) {
      // Success: show the phone's ResultText when present (e.g. the
      // multiline account block from FetchUserInfo), else the generic note.
      dialog_show_final(true, (text && text[0]) ? text : "Done!");
    }
    return;
  }
  if (!s_dialog_active) {
    dialog_create();
  }
  char msg[96];
  // Bounded copy (no snprintf): the vfprintf machinery is a deep frame on
  // the 2 KB basalt-class stack, and this runs in the inbox callback.
  {
    size_t i = 0;
    const char *src = (text && text[0]) ? text : "Error";
    while (i + 1 < sizeof(msg) && src[i]) {
      msg[i] = src[i];
      i++;
    }
    msg[i] = '\0';
  }
  if (contains_ci(msg, "login") || contains_ci(msg, "401") ||
      contains_ci(msg, "unauthorized")) {
    const char *hint = "\n\nSet API password in phone settings";
    size_t l = strlen(msg);
    size_t k = 0;
    while (l + k + 1 < sizeof(msg) && hint[k]) {
      msg[l + k] = hint[k];
      k++;
    }
    msg[l + k] = '\0';
  }
  dialog_show_final(false, msg);
}

//! A fresh tree arrived: refresh the visible tree menus and hide any
//! working dialog ("Loading..." / "Refreshing...").
void ui_tree_updated(void) {
  APP_LOG(APP_LOG_LEVEL_INFO, "startup: menu reload");
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
  if (s_mode_window) {
    window_set_background_color(s_mode_window, theme_bg());
  }
  if (s_mode_menu) {
    menu_layer_set_normal_colors(s_mode_menu, theme_bg(), theme_fg());
    menu_layer_set_highlight_colors(s_mode_menu, s_accent, GColorBlack);
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
  // Row 0 is the accent strip; the Important special row (after Starred)
  // adds one data row when enabled (skipped while the tree is empty).
  int data = root > 0 ? root : 1;
  if (setting_important() && root > 0) {
    data++;
  }
  return (uint16_t)(1 + data);
}

//! Map a root data row (0-based, row 0 = the first tree/special row) to a
//! tree row index, or -1 for the synthetic Important row (inserted right
//! after the Starred special; appended at the end when Starred is absent).
//! Callers must handle the empty tree (root == 0) before using this.
static int root_data_to_tree(int row) {
  int root = tree_root_count();
  if (!setting_important()) {
    return row;
  }
  int insert_at = root; // default: after all tree rows
  for (int i = 0; i < root; i++) {
    if (strcmp(tree_root_node(i)->id, STARRED_STREAM) == 0) {
      insert_at = i + 1;
      break;
    }
  }
  if (row < insert_at) {
    return row;
  }
  if (row == insert_at) {
    return -1; // the Important row itself
  }
  return row - 1;
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

//! Right-aligned unread badge as a filled accent pill (black count); on the

//! Width of the unread badge pill for a count (digits * 8 + padding); 0 when
//! nothing would be drawn.
static int16_t badge_width(int32_t unread) {
  if (unread <= 0) {
    return 0;
  }
  int d = 1;
  int32_t v = unread;
  while ((v /= 10) > 0) {
    d++;
  }
  return (int16_t)(14 + d * 8);
}

//! Right-aligned unread badge as a filled accent pill (black count); on the
//! accent selection row the pill inverts (black pill, white count). `shift`
//! moves it left to make room for the NEW-dot marker.
static void draw_badge(GContext *ctx, GRect b, int32_t unread, bool selected,
                       int16_t shift) {
  int16_t pw = badge_width(unread);
  if (pw <= 0) {
    return;
  }
  char num[12];
  snprintf(num, sizeof(num), "%ld", (long)unread);
  GRect pill = GRect(b.size.w - 12 - pw - shift, (b.size.h - 16) / 2, pw, 16);
  graphics_context_set_fill_color(ctx, selected ? GColorBlack : s_accent);
  graphics_fill_rect(ctx, pill, 8, GCornersAll);
  graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorBlack);
  graphics_draw_text(ctx, num, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(pill.origin.x, pill.origin.y - 1, pill.size.w, pill.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

//! NEW-dot: a feed row with unread > 0 whose tree newest (seconds) is newer
//! than the stored last-seen for that feed shows an accent "NEW" pill.
#define NEW_PILL_W 36 // "NEW" marker pill width

static bool new_pill_active(const FeedNode *node) {
  if (!setting_newdot() || !node || node->kind != 2 || node->unread <= 0) {
    return false;
  }
  int64_t newest_s = div_million(node->newest);
  return newest_s > storage_feed_last_seen(node->id);
}

//! Small accent "NEW" pill, left of the unread badge, same style as the
//! badge (inverts on the selected row). `shift` = badge width + gap.
static void draw_new_pill(GContext *ctx, GRect b, bool selected,
                          const FeedNode *node, int16_t shift) {
  if (!new_pill_active(node)) {
    return;
  }
  GRect pill = GRect(b.size.w - 12 - NEW_PILL_W - shift,
                     (b.size.h - 16) / 2, NEW_PILL_W, 16);
  graphics_context_set_fill_color(ctx, selected ? GColorBlack : s_accent);
  graphics_fill_rect(ctx, pill, 8, GCornersAll);
  graphics_context_set_text_color(ctx, selected ? GColorWhite : GColorBlack);
  graphics_draw_text(ctx, "NEW", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(pill.origin.x, pill.origin.y - 1, pill.size.w, pill.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

//! Leading nav icon: pin = Important (NULL node = the synthetic row), star =
//! Starred, folder = folders, news = feeds; "All unread"/"All articles" get
//! none. Returns the text offset (icon + gap when drawn, else the base x).
//! The news glyph gets two text lines "cut out" in the row background color.
static int16_t draw_nav_icon(GContext *ctx, const FeedNode *node,
                             int16_t text_x, int16_t cy, GColor color,
                             GColor bg) {
  GPath *p = NULL;
  if (!node) {
    p = s_icon_pin;
  } else if (node->kind == 2) {
    p = s_icon_news;
  } else if (node->kind == 1) {
    p = s_icon_folder;
  } else if (strcmp(node->id, "user/-/state/com.google/starred") == 0) {
    p = s_icon_star;
  }
  if (!p) {
    return text_x;
  }
  GPoint c = GPoint(text_x + 8, cy);
  graphics_context_set_fill_color(ctx, color);
  gpath_move_to(p, c);
  gpath_draw_filled(ctx, p);
  if (p == s_icon_news) {
    graphics_context_set_stroke_color(ctx, bg);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(c.x - 4, c.y - 1), GPoint(c.x + 4, c.y - 1));
    graphics_draw_line(ctx, GPoint(c.x - 4, c.y + 2), GPoint(c.x + 4, c.y + 2));
  }
  return text_x + 24;
}

static void main_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                          void *callback_context) {
  uint16_t row = cell_index->row;
  GRect bounds = layer_get_bounds(cell_layer);

  if (row == 0) {
    // Accent strip: the UP entry row doubles as the app's color header.
    // Black bar with accent dots (inverted from the old solid-accent bar).
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    int16_t cx = bounds.size.w / 2;
    int16_t cy = bounds.size.h / 2;
    graphics_context_set_fill_color(ctx, s_accent);
    for (int i = -1; i <= 1; i++) {
      graphics_fill_circle(ctx, GPoint(cx + i * 6, cy), 2);
    }
    return;
  }

  int root = tree_root_count();
  if (root == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "No feeds yet",
                         "Open the phone app settings", NULL);
    return;
  }

  int tree_row = root_data_to_tree(row - 1);
  GRect b = layer_get_bounds(cell_layer);
  bool selected = menu_layer_is_index_selected(s_main_menu, (MenuIndex *)cell_index);

  // Row background: accent when selected, theme otherwise.
  graphics_context_set_fill_color(ctx, selected ? s_accent : theme_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // tree_row < 0 is the synthetic Important row (pin icon, no badge).
  const FeedNode *node = tree_row >= 0 ? tree_root_node(tree_row) : NULL;
  int16_t text_x = draw_nav_icon(ctx, node, 8, b.size.h / 2,
                                 selected ? GColorBlack : theme_fg(),
                                 selected ? s_accent : theme_bg());

  const char *label = "Important";
  if (node) {
    label = node->name[0] ? node->name : node->id;
  }
  bool np = node && new_pill_active(node);
  graphics_context_set_text_color(ctx, selected ? GColorBlack : theme_fg());
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(text_x, (b.size.h - 22) / 2,
                           b.size.w - text_x - 38 - (np ? NEW_PILL_W + 4 : 0), 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (node) {
    draw_new_pill(ctx, b, selected, node, badge_width(node->unread) + 4);
    draw_badge(ctx, b, node->unread, selected, np ? NEW_PILL_W + 4 : 0);
  }
}

static void main_select_cb(MenuLayer *menu_layer, MenuIndex *cell_index,
                           void *callback_context) {
  uint16_t row = cell_index->row;
  if (row == 0) {
    push_submenu_window();
    return;
  }
  int root = tree_root_count();
  if (root == 0) {
    push_submenu_window(); // empty state routes to the sub-menu too
    return;
  }
  int tree_row = root_data_to_tree(row - 1);
  if (tree_row < 0) {
    timeline_open(IMPORTANT_STREAM, "Important");
    return;
  }
  const FeedNode *node = tree_root_node(tree_row);
  if (node->kind == 1) {
    push_folder_window(node->id, node->name);
  } else {
    // Specials ("All unread", "Starred", "Important") and feeds open the
    // timeline.
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

//! Long-press SELECT: context menu on feed rows only (Mark all read /
//! Refresh). Folders, specials and the accent strip do nothing.
static void main_long_select_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_main_menu);
  if (idx.row == 0) {
    return;
  }
  int root = tree_root_count();
  if (root == 0) {
    return;
  }
  int tree_row = root_data_to_tree(idx.row - 1);
  if (tree_row < 0) {
    return; // the Important special
  }
  const FeedNode *node = tree_root_node(tree_row);
  if (node && node->kind == 2) {
    open_context_menu(node);
  }
}

static void main_click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, main_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, main_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, main_select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, LONG_SELECT_MS,
                              main_long_select_click, NULL);
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
  // Always start on the first tree row. NOT animated: the tree arrives
  // right after load and reload_data() rebuilds the rows while a scroll
  // animation would still be running — PebbleOS's menu animation callback
  // then fires on the rebuilt menu (use-after-free -> corrupted callback
  // pointer -> the startup crash).
  MenuIndex first = { .section = 0, .row = 1 };
  menu_layer_set_selected_index(s_main_menu, first, MenuRowAlignCenter, false);
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

  const char *label;
  int32_t unread;
  const FeedNode *node = NULL;

  if (cell_index->row == 0) {
    // "All articles": the folder's own stream id opens the recursive
    // listing. No icon.
    label = "All articles";
    const FeedNode *f = tree_find(s_folder_id);
    unread = f ? f->unread : 0;
  } else {
    node = tree_child_node(s_folder_id, cell_index->row - 1);
    if (!node) {
      return;
    }
    label = node->name[0] ? node->name : node->id;
    unread = node->unread;
  }
  int16_t text_x = draw_nav_icon(ctx, node, 8, b.size.h / 2,
                                 selected ? GColorBlack : theme_fg(),
                                 selected ? s_accent : theme_bg());

  bool np = node && new_pill_active(node);
  graphics_context_set_text_color(ctx, selected ? GColorBlack : theme_fg());
  graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(text_x, (b.size.h - 22) / 2,
                           b.size.w - text_x - 38 - (np ? NEW_PILL_W + 4 : 0), 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (node) {
    draw_new_pill(ctx, b, selected, node, badge_width(node->unread) + 4);
  }
  draw_badge(ctx, b, unread, selected, np ? NEW_PILL_W + 4 : 0);
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

//! Long-press SELECT: context menu on feed rows only ("All articles" and
//! sub-folder rows do nothing).
static void folder_long_select_cb(MenuLayer *menu_layer, MenuIndex *cell_index,
                                  void *callback_context) {
  if (cell_index->row == 0) {
    return;
  }
  const FeedNode *node = tree_child_node(s_folder_id, cell_index->row - 1);
  if (node && node->kind == 2) {
    open_context_menu(node);
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
    .select_long_click = folder_long_select_cb,
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
// Sub-menu (UP from the root): Refresh / Mark all read / Connection / Auto
// mark read (opens the MarkMode selection window) / Unread only / the four
// smart-surface toggles — flat, one submenu (the auto-mark window)
// ---------------------------------------------------------------------------

//! Mode labels for the "Auto mark read" row subtitle and the selection
//! window, indexed by MarkMode (see MARK_MODE_LABELS in common.h).
static const char *const s_mark_mode_labels[MARK_MODE_COUNT] = MARK_MODE_LABELS;

static uint16_t sub_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                 void *callback_context) {
  return 9;
}

static void sub_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                         void *callback_context) {
  if (cell_index->row == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Refresh",
                         "Reload feeds from the server", NULL);
  } else if (cell_index->row == 1) {
    menu_cell_basic_draw(ctx, cell_layer, "Mark all read",
                         "All articles in every feed", NULL);
  } else if (cell_index->row == 2) {
    menu_cell_basic_draw(ctx, cell_layer, "Connection info",
                         "Show the connected account", NULL);
  } else if (cell_index->row == 3) {
    // Auto-mark mode: subtitle shows the current mode label.
    const char *mode = mark_mode() >= MARK_NEVER && mark_mode() < MARK_MODE_COUNT
                           ? s_mark_mode_labels[mark_mode()]
                           : "?";
    menu_cell_basic_draw(ctx, cell_layer, "Auto mark read", mode, NULL);
  } else if (cell_index->row == 4) {
    menu_cell_basic_draw(ctx, cell_layer, "Unread only",
                         s_unread_only ? "ON" : "OFF", NULL);
  } else if (cell_index->row == 5) {
    menu_cell_basic_draw(ctx, cell_layer, "Important row",
                         s_important ? "ON" : "OFF", NULL);
  } else if (cell_index->row == 6) {
    menu_cell_basic_draw(ctx, cell_layer, "NEW-dot",
                         s_newdot ? "ON" : "OFF", NULL);
  } else if (cell_index->row == 7) {
    menu_cell_basic_draw(ctx, cell_layer, "Progress line",
                         s_progress ? "ON" : "OFF", NULL);
  } else {
    menu_cell_basic_draw(ctx, cell_layer, "Triage drain",
                         s_triage ? "ON" : "OFF", NULL);
  }
}

static void sub_select_cb(MenuLayer *menu_layer, MenuIndex *cell_index,
                          void *callback_context) {
  if (cell_index->row == 0) {
    dialog_show_working("Refreshing");
    proto_request_tree();
  } else if (cell_index->row == 1) {
    s_markall_stream[0] = '\0'; // the whole reading list
    s_confirm_markall = true;
    dialog_show_confirm_text("Mark all read?\n\nSELECT: confirm\nBACK: cancel");
  } else if (cell_index->row == 2) {
    // Connection info: ask the phone for the account; the ResultText reply
    // is shown in the dialog ("Account: ...\n...\nServer: ...\nUnread: ...").
    dialog_show_working("Loading");
    proto_request_user_info();
  } else if (cell_index->row == 3) {
    push_mode_window();
  } else if (cell_index->row == 4) {
    s_unread_only = !s_unread_only;
    storage_save_settings();
    vibes_short_pulse();
    menu_layer_reload_data(menu_layer);
  } else if (cell_index->row == 5) {
    s_important = !s_important;
    storage_save_settings();
    vibes_short_pulse();
    menu_layer_reload_data(menu_layer);
  } else if (cell_index->row == 6) {
    s_newdot = !s_newdot;
    storage_save_settings();
    vibes_short_pulse();
    menu_layer_reload_data(menu_layer);
  } else if (cell_index->row == 7) {
    s_progress = !s_progress;
    storage_save_settings();
    vibes_short_pulse();
    menu_layer_reload_data(menu_layer);
  } else {
    s_triage = !s_triage;
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

static void sub_window_appear(Window *window) {
  // The auto-mark label may have changed in the mode window underneath.
  menu_layer_reload_data(s_sub_menu);
}

static void push_submenu_window(void) {
  s_sub_window = window_create();
  window_set_window_handlers(s_sub_window, (WindowHandlers){
    .load = sub_window_load,
    .appear = sub_window_appear,
    .unload = sub_window_unload,
  });
  window_stack_push(s_sub_window, true);
}

// ---------------------------------------------------------------------------
// Auto-mark mode window (opened from the sub-menu "Auto mark read" row): a
// 7-row selection list over the MarkMode enum, folder-window style. SELECT
// picks the highlighted mode (persisted via mark_mode_set, then pops);
// BACK pops without changing anything.
// ---------------------------------------------------------------------------

static uint16_t mode_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                  void *callback_context) {
  return MARK_MODE_COUNT;
}

static int16_t mode_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index,
                                    void *callback_context) {
  return 44;
}

static void mode_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                          void *callback_context) {
  GRect b = layer_get_bounds(cell_layer);
  bool selected = menu_layer_is_index_selected(s_mode_menu, (MenuIndex *)cell_index);
  bool current = (int)cell_index->row == mark_mode();

  graphics_context_set_fill_color(ctx, selected ? s_accent : theme_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  graphics_context_set_text_color(ctx, selected ? GColorBlack : theme_fg());
  graphics_draw_text(ctx, s_mark_mode_labels[cell_index->row],
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(8, (b.size.h - 22) / 2, b.size.w - 40, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (current) {
    // Checkmark on the active mode (drawn in the inverted row color).
    GPoint mid = GPoint(b.size.w - 22, b.size.h / 2);
    graphics_context_set_stroke_color(ctx, selected ? GColorBlack : s_accent);
    graphics_draw_line(ctx, GPoint(mid.x - 5, mid.y + 1), GPoint(mid.x - 1, mid.y + 5));
    graphics_draw_line(ctx, GPoint(mid.x - 1, mid.y + 5), GPoint(mid.x + 6, mid.y - 3));
  }
}

static void mode_select_cb(MenuLayer *menu_layer, MenuIndex *cell_index,
                           void *callback_context) {
  mark_mode_set((int)cell_index->row);
  vibes_short_pulse();
  if (s_mode_window) {
    window_stack_remove(s_mode_window, true);
  }
}

static void mode_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  window_set_background_color(window, theme_bg());

  s_mode_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_mode_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = mode_get_num_rows,
    .get_cell_height = mode_get_cell_height,
    .draw_row = mode_draw_row,
    .select_click = mode_select_cb,
  });
  menu_layer_set_click_config_onto_window(s_mode_menu, window);
  menu_layer_pad_bottom_enable(s_mode_menu, true);
  menu_layer_set_normal_colors(s_mode_menu, theme_bg(), theme_fg());
  menu_layer_set_highlight_colors(s_mode_menu, s_accent, GColorBlack);
  layer_add_child(root, menu_layer_get_layer(s_mode_menu));
}

static void mode_window_unload(Window *window) {
  menu_layer_destroy(s_mode_menu);
  s_mode_menu = NULL;
  window_destroy(s_mode_window);
  s_mode_window = NULL;
}

static void push_mode_window(void) {
  s_mode_window = window_create();
  window_set_window_handlers(s_mode_window, (WindowHandlers){
    .load = mode_window_load,
    .unload = mode_window_unload,
  });
  window_stack_push(s_mode_window, true);
}

// ---------------------------------------------------------------------------
// Context menu (long-press SELECT on a feed row): a small two-row menu —
// "Mark all read" (orange confirm + mark-all-as-read for that feed) and
// "Refresh" (re-fetch items and open the reader at the newest).
// ---------------------------------------------------------------------------

static Window *s_ctx_window;
static MenuLayer *s_ctx_menu;
static char s_ctx_feed_id[48];
static char s_ctx_feed_name[48];

static uint16_t ctx_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                 void *callback_context) {
  return 2;
}

static int16_t ctx_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index,
                                   void *callback_context) {
  return 44;
}

static void ctx_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                         void *callback_context) {
  if (cell_index->row == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Mark all read",
                         "Mark every article in this feed", NULL);
  } else {
    menu_cell_basic_draw(ctx, cell_layer, "Refresh",
                         "Re-fetch this feed", NULL);
  }
}

static void ctx_select_cb(MenuLayer *menu_layer, MenuIndex *cell_index,
                          void *callback_context) {
  // Snapshot the target feed before the window is destroyed by the pop.
  char id[48];
  char name[48];
  snprintf(id, sizeof(id), "%s", s_ctx_feed_id);
  snprintf(name, sizeof(name), "%s", s_ctx_feed_name);
  if (s_ctx_window) {
    window_stack_remove(s_ctx_window, true);
  }
  if (cell_index->row == 0) {
    // Mark all read for this feed: reuse the orange confirm machinery.
    snprintf(s_markall_stream, sizeof(s_markall_stream), "%s", id);
    s_confirm_markall = true;
    dialog_show_confirm_text("Mark all read?\n\nSELECT: confirm\nBACK: cancel");
  } else {
    // Refresh: timeline_open re-requests page 1 (newest first) and shows
    // the reader.
    timeline_open(id, name);
  }
}

static void ctx_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, theme_bg());

  // Full-screen menu, like every other menu in the app.
  GRect menu_bounds = GRect(0, 0, bounds.size.w, bounds.size.h);
  s_ctx_menu = menu_layer_create(menu_bounds);
  menu_layer_set_callbacks(s_ctx_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = ctx_get_num_rows,
    .get_cell_height = ctx_get_cell_height,
    .draw_row = ctx_draw_row,
    .select_click = ctx_select_cb,
  });
  menu_layer_set_click_config_onto_window(s_ctx_menu, window);
  menu_layer_pad_bottom_enable(s_ctx_menu, true);
  menu_layer_set_normal_colors(s_ctx_menu, theme_bg(), theme_fg());
  menu_layer_set_highlight_colors(s_ctx_menu, s_accent, GColorBlack);
  layer_add_child(root, menu_layer_get_layer(s_ctx_menu));
}

static void ctx_window_unload(Window *window) {
  menu_layer_destroy(s_ctx_menu);
  s_ctx_menu = NULL;
  window_destroy(s_ctx_window);
  s_ctx_window = NULL;
}

//! Open the context menu for a feed row (no-op for anything else).
static void open_context_menu(const FeedNode *feed) {
  if (!feed || feed->kind != 2) {
    return;
  }
  snprintf(s_ctx_feed_id, sizeof(s_ctx_feed_id), "%s", feed->id);
  snprintf(s_ctx_feed_name, sizeof(s_ctx_feed_name), "%s",
           feed->name[0] ? feed->name : feed->id);
  s_ctx_window = window_create();
  window_set_window_handlers(s_ctx_window, (WindowHandlers){
    .load = ctx_window_load,
    .unload = ctx_window_unload,
  });
  window_stack_push(s_ctx_window, true);
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

static void init(void) {
  storage_load();
  proto_init(); // registers handlers + app_message_open(4096, 1024)

  s_icon_pin = gpath_create(&PIN_PATH_INFO);
  s_icon_star = gpath_create(&STAR_ICON_INFO);
  s_icon_folder = gpath_create(&FOLDER_ICON_INFO);
  s_icon_news = gpath_create(&NEWS_ICON_INFO);

  // Native touch navigation, only when the user enables it in settings.
  (void)app_touch_navigation_enable(s_touch);

  push_main_window();

  // Cached tree = instant start; refresh in the background. NEVER show the
  // working dialog here: on a first run (or after a cache-format change)
  // the fetch races the dialog's push/pop against the menu render — that
  // interplay crashed at startup. The menu's empty state is the feedback;
  // explicit user actions (Refresh, Connection, Mark all read) still use
  // the dialog.
  tree_load_cache();
  proto_request_tree();
}

static void deinit(void) {
  if (s_icon_pin) {
    gpath_destroy(s_icon_pin);
    s_icon_pin = NULL;
  }
  if (s_icon_star) {
    gpath_destroy(s_icon_star);
    s_icon_star = NULL;
  }
  if (s_icon_folder) {
    gpath_destroy(s_icon_folder);
    s_icon_folder = NULL;
  }
  if (s_icon_news) {
    gpath_destroy(s_icon_news);
    s_icon_news = NULL;
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
