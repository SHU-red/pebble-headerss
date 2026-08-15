#include <pebble.h>

#include "timeline.h"
#include "storage.h"
#include "proto.h"
#include "tree.h"
#include "common.h"

// ---------------------------------------------------------------------------
// Timeline reading view (see timeline.h). Article headings stream in from
// the phone into a ring buffer; pages are dropped from the head when the
// buffer fills and re-fetched via continuation. The visual centerpiece is a
// native-Timeline-style list: a permanent accent bar on the right edge, one
// dot per entry on the bar (accent = unread, muted = read), a filled star
// for starred entries, and a pin notch on the selected row.
// ---------------------------------------------------------------------------

#define TIMELINE_HEADER_H 26 // custom title bar (SDK3 has no window title)

static Article s_articles[MAX_ARTICLES];
static int32_t s_count;
static char s_stream[48]; // current stream id
static char s_cont[24];   // next continuation; "" = all loaded
static bool s_loading;
static bool s_loaded_all;
static char s_title[24];

static Window *s_tl_window;
static MenuLayer *s_tl_menu;
static TextLayer *s_tl_header;
static bool s_tl_visible;

// Shared draw paths: a 5-point star (10 vertices, ~8 px) and the pin notch
// (right-pointing triangle, 10x8). Repositioned per row with gpath_move_to.
static const GPathInfo STAR_PATH_INFO = {
  .num_points = 10,
  .points = (GPoint[]) {
    { 0, -4 }, { -1, 2 }, { -4, 1 }, { -2, -1 }, { -2, -3 },
    { 0, -2 }, { 2, -3 }, { 2, -1 }, { 4, 1 }, { 1, 2 },
  },
};

static const GPathInfo PIN_PATH_INFO = {
  .num_points = 3,
  .points = (GPoint[]) { { 0, 0 }, { -10, -4 }, { -10, 4 } },
};

static GPath *s_star_path;
static GPath *s_pin_path;

// ---------------------------------------------------------------------------
// Article detail window: title + feed·time header over a scrolling summary.
// The summary is a single-slot cache (id + text) so revisits don't re-fetch.
// ---------------------------------------------------------------------------

static Window *s_detail_window;
static TextLayer *s_detail_title;
static TextLayer *s_detail_meta;
static ScrollLayer *s_detail_scroll;
static TextLayer *s_summary_text;
static int32_t s_detail_idx;
static char s_summary_id[24];
static char s_summary[300];

static void detail_open(int32_t idx);
static void detail_render(void);
static void article_mark_read(int32_t idx);
static void detail_select_click(ClickRecognizerRef rec, void *ctx);
static void detail_star_long_click(ClickRecognizerRef rec, void *ctx);
static void detail_back_click(ClickRecognizerRef rec, void *ctx);

// ---------------------------------------------------------------------------
// Small formatters
// ---------------------------------------------------------------------------

//! Compact relative time: "now", "5m", "3h", "2d", else "dd.mm.".
static void format_reltime(char *buf, size_t len, int32_t published) {
  int32_t now = (int32_t)time(NULL);
  int32_t diff = now - published;
  if (diff < 0) {
    diff = 0;
  }
  if (diff < 60) {
    snprintf(buf, len, "now");
  } else if (diff < 3600) {
    snprintf(buf, len, "%ldm", (long)(diff / 60));
  } else if (diff < 86400) {
    snprintf(buf, len, "%ldh", (long)(diff / 3600));
  } else if (diff < 604800) {
    snprintf(buf, len, "%ldd", (long)(diff / 86400));
  } else {
    struct tm *lt = localtime(&published);
    if (lt) {
      strftime(buf, len, "%d.%m.", lt);
    } else {
      snprintf(buf, len, "%s", "");
    }
  }
}

//! Absolute date+time for the detail card: "dd.mm. hh:mm".
static void format_datetime(char *buf, size_t len, int32_t published) {
  struct tm *lt = localtime(&published);
  if (lt) {
    strftime(buf, len, "%d.%m. %H:%M", lt);
  } else {
    snprintf(buf, len, "%s", "");
  }
}

// ---------------------------------------------------------------------------
// Timeline window
// ---------------------------------------------------------------------------

static void timeline_prefetch_check(void);

static void timeline_up_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  if (idx.row > 0) {
    idx.row--;
    menu_layer_set_selected_index(s_tl_menu, idx, MenuRowAlignCenter, true);
    timeline_prefetch_check();
  }
}

static void timeline_down_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  uint16_t rows = (uint16_t)(s_count > 0 ? s_count : 1);
  if (idx.row + 1 < rows) {
    idx.row++;
    menu_layer_set_selected_index(s_tl_menu, idx, MenuRowAlignCenter, true);
    timeline_prefetch_check();
  }
}

//! SELECT opens the article detail; the list-level mark-read toggle applies
//! on open (the detail flow applies its own toggle on advance).
static void timeline_select_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  if (idx.row >= (uint16_t)s_count) {
    return;
  }
  if (s_mark_list) {
    article_mark_read((int32_t)idx.row);
  }
  detail_open((int32_t)idx.row);
}

//! Long SELECT toggles the star of the selected entry (fire-and-forget).
static void timeline_star_long_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  if (idx.row >= (uint16_t)s_count) {
    return;
  }
  Article *a = &s_articles[idx.row];
  a->star = a->star ? 0 : 1;
  proto_star(a->id, a->star != 0);
  vibes_short_pulse();
  menu_layer_reload_data(s_tl_menu);
}

static void timeline_back_click(ClickRecognizerRef rec, void *ctx) {
  window_stack_pop(true);
}

static void timeline_click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, timeline_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, timeline_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, timeline_select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, timeline_star_long_click, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, timeline_back_click);
}

static uint16_t timeline_get_num_rows(MenuLayer *menu_layer, uint16_t section_index,
                                      void *callback_context) {
  return (uint16_t)(s_count > 0 ? s_count : 1); // hint row when empty
}

static int16_t timeline_get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index,
                                        void *callback_context) {
  return TIMELINE_ROW_H;
}

//! Timeline cell: right-edge accent bar, dot per entry on the bar, star for
//! starred entries, title + feed·reltime, pin notch on the selected row.
static void timeline_draw_row(GContext *ctx, const Layer *cell_layer,
                              MenuIndex *cell_index, void *callback_context) {
  GRect b = layer_get_bounds(cell_layer);
  bool selected = menu_layer_is_index_selected(s_tl_menu, (MenuIndex *)cell_index);

  // Row background: accent when selected, theme otherwise.
  graphics_context_set_fill_color(ctx, selected ? s_accent : theme_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  if (cell_index->row >= (uint16_t)s_count) {
    // Empty/loading state: a single centered hint row.
    graphics_context_set_text_color(ctx, selected ? GColorWhite : theme_muted());
    graphics_draw_text(ctx, s_loading ? "Loading..." : "No articles",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(0, (b.size.h - 22) / 2, b.size.w, 22),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  Article *a = &s_articles[cell_index->row];
  const int16_t bar_x = b.size.w - 4; // left edge of the accent bar
  const int16_t cy = b.size.h / 2;    // row center (dot + pin anchor)

  // Permanent accent-colored vertical bar on the right edge, full cell height.
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, GRect(bar_x, 0, 3, b.size.h), 0, GCornerNone);

  // Dot centered on the bar: accent = unread, muted = read.
  graphics_context_set_fill_color(ctx, a->read ? theme_muted() : s_accent);
  graphics_fill_circle(ctx, GPoint(bar_x + 2, cy), 2);

  // Starred entries: small filled star just left of the bar.
  if (a->star && s_star_path) {
    graphics_context_set_fill_color(ctx, GColorYellow);
    gpath_move_to(s_star_path, GPoint(bar_x - 10, cy));
    gpath_draw_filled(ctx, s_star_path);
  }

  // Title: bold while unread; one line + feed·reltime line, or two wrapped
  // title lines when the title does not fit (measured, not guessed).
  const int16_t text_w = bar_x - 22; // x from 4 to bar_x-18
  GFont title_font = fonts_get_system_font(
      a->read ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_18_BOLD);
  GSize ts = graphics_text_layout_get_content_size(
      a->title, title_font, GRect(0, 0, text_w, 100), GTextOverflowModeWordWrap,
      GTextAlignmentLeft);
  graphics_context_set_text_color(ctx, selected ? GColorWhite : theme_fg());
  if (ts.h > 22) {
    graphics_draw_text(ctx, a->title, title_font, GRect(4, 1, text_w, 44),
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  } else {
    graphics_draw_text(ctx, a->title, title_font, GRect(4, 3, text_w, 22),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    char rel[12];
    format_reltime(rel, sizeof(rel), a->published);
    char meta[48];
    snprintf(meta, sizeof(meta), "%s \xc2\xb7 %s", a->feed[0] ? a->feed : "?", rel);
    graphics_context_set_text_color(ctx, selected ? GColorWhite : theme_muted());
    graphics_draw_text(ctx, meta, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(4, 27, text_w, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }

  // Selected row: pin notch — right-pointing triangle with its apex at the
  // bar, drawn at the row center (static per-row draw: no scroll math).
  if (selected && s_pin_path) {
    graphics_context_set_fill_color(ctx, s_accent);
    gpath_move_to(s_pin_path, GPoint(bar_x, cy));
    gpath_draw_filled(ctx, s_pin_path);
  }
}

static void timeline_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  // Custom title bar: this SDK has no window title/fullscreen API — windows
  // are full-bleed by default (no status bar), so the header sits at the top.
  window_set_background_color(window, theme_bg());

  s_tl_header = text_layer_create(GRect(4, 2, bounds.size.w - 8, 24));
  text_layer_set_font(s_tl_header, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_tl_header, GTextAlignmentLeft);
  text_layer_set_overflow_mode(s_tl_header, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_tl_header, GColorClear);
  text_layer_set_text_color(s_tl_header, theme_fg());
  text_layer_set_text(s_tl_header, s_title);
  layer_add_child(root, text_layer_get_layer(s_tl_header));

  s_tl_menu = menu_layer_create(
      GRect(0, TIMELINE_HEADER_H, bounds.size.w, bounds.size.h - TIMELINE_HEADER_H));
  menu_layer_set_callbacks(s_tl_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = timeline_get_num_rows,
    .get_cell_height = timeline_get_cell_height,
    .draw_row = timeline_draw_row,
  });
  window_set_click_config_provider(window, timeline_click_config_provider);
  menu_layer_pad_bottom_enable(s_tl_menu, true);
  menu_layer_set_normal_colors(s_tl_menu, theme_bg(), theme_fg());
  menu_layer_set_highlight_colors(s_tl_menu, s_accent, GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_tl_menu));
}

//! Teardown on window unload: flush any pending mark-read batch and release
//! every layer/path so a later re-open builds fresh.
static void timeline_close(void) {
  proto_flush_now(); // never leave a mark-read batch half-flushed
  menu_layer_destroy(s_tl_menu);
  text_layer_destroy(s_tl_header);
  s_tl_menu = NULL;
  s_tl_header = NULL;
  if (s_star_path) {
    gpath_destroy(s_star_path);
    s_star_path = NULL;
  }
  if (s_pin_path) {
    gpath_destroy(s_pin_path);
    s_pin_path = NULL;
  }
}

static void timeline_window_unload(Window *window) {
  timeline_close();
  window_destroy(s_tl_window);
  s_tl_window = NULL;
}

static void timeline_window_appear(Window *window) {
  s_tl_visible = true;
  if (s_tl_menu) {
    menu_layer_reload_data(s_tl_menu); // read/star flags may have changed
  }
}

static void timeline_window_disappear(Window *window) {
  s_tl_visible = false;
}

//! Open (or reset and re-open) the timeline for a stream. State is reset and
//! the first page is requested; the menu shows a "Loading..." hint row until
//! the first page arrives.
void timeline_open(const char *stream, const char *title) {
  if (!s_tl_window) {
    s_tl_window = window_create();
    window_set_window_handlers(s_tl_window, (WindowHandlers){
      .load = timeline_window_load,
      .appear = timeline_window_appear,
      .disappear = timeline_window_disappear,
      .unload = timeline_window_unload,
    });
  }
  if (!s_star_path) {
    s_star_path = gpath_create(&STAR_PATH_INFO);
  }
  if (!s_pin_path) {
    s_pin_path = gpath_create(&PIN_PATH_INFO);
  }
  s_count = 0;
  s_loading = true;
  s_loaded_all = false;
  s_cont[0] = '\0';
  snprintf(s_stream, sizeof(s_stream), "%s", stream ? stream : "");
  snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
  if (s_tl_header) {
    text_layer_set_text(s_tl_header, s_title);
  }
  if (s_tl_menu) {
    menu_layer_reload_data(s_tl_menu);
  }
  if (!s_tl_visible) {
    window_stack_push(s_tl_window, true);
  }
  proto_request_items(s_stream, "");
}

// ---------------------------------------------------------------------------
// Item-page collect hooks (called from proto_handle_inbox)
// ---------------------------------------------------------------------------

//! A new page is starting: make room in the ring buffer by shifting out the
//! oldest 24 entries when the buffer is nearly full.
void timeline_page_begin(int32_t n) {
  (void)n;
  if (s_count >= MAX_ARTICLES - 24) {
    memmove(&s_articles[0], &s_articles[24],
            (size_t)(s_count - 24) * sizeof(Article));
    s_count -= 24;
  }
}

//! Append one article heading from the current message (bounds-checked).
void timeline_collect_article(DictionaryIterator *iter) {
  if (s_count >= MAX_ARTICLES) {
    return;
  }
  Article *a = &s_articles[s_count];
  memset(a, 0, sizeof(Article));
  Tuple *t;
  if ((t = dict_find(iter, MESSAGE_KEY_ItemId))) {
    snprintf(a->id, sizeof(a->id), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemTitle))) {
    snprintf(a->title, sizeof(a->title), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemFeed))) {
    snprintf(a->feed, sizeof(a->feed), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemFeedId))) {
    snprintf(a->feed_id, sizeof(a->feed_id), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemTime))) {
    a->published = t->value->int32;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemRead))) {
    a->read = (uint8_t)(t->value->int32 != 0);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemStar))) {
    a->star = (uint8_t)(t->value->int32 != 0);
  }
  s_count++;
}

//! The page ended: store the continuation ("", = no more items), release the
//! loading flag and redraw.
void timeline_page_end(const char *cont) {
  snprintf(s_cont, sizeof(s_cont), "%s", cont ? cont : "");
  if (s_cont[0] == '\0') {
    s_loaded_all = true;
  }
  s_loading = false;
  if (s_tl_menu) {
    menu_layer_reload_data(s_tl_menu);
    layer_mark_dirty(menu_layer_get_layer(s_tl_menu));
  }
}

//! Prefetch the next page when the selection enters the last 6 rows.
static void timeline_prefetch_check(void) {
  if (!s_tl_menu || s_loading || s_loaded_all || s_count <= 0) {
    return;
  }
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  if ((int32_t)idx.row + 6 >= s_count) {
    s_loading = true;
    proto_request_items(s_stream, s_cont);
  }
}

// ---------------------------------------------------------------------------
// Mark-read helper
// ---------------------------------------------------------------------------

//! Mark an article read: flip the flag, queue the id for the mark-read batch
//! and decrement the tree badges optimistically.
static void article_mark_read(int32_t idx) {
  if (idx < 0 || idx >= s_count) {
    return;
  }
  Article *a = &s_articles[idx];
  if (a->read) {
    return;
  }
  a->read = 1;
  if (s_tl_menu) {
    menu_layer_reload_data(s_tl_menu);
  }
  proto_mark_push(a->id);
  tree_feed_decrement(a->feed_id);
}

// ---------------------------------------------------------------------------
// Detail window
// ---------------------------------------------------------------------------

static void detail_click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, detail_select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, detail_star_long_click, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, detail_back_click);
}

//! SELECT advances to the next article (marks read per the detail toggle);
//! at the end the pulse says "no more" and the card stays.
static void detail_select_click(ClickRecognizerRef rec, void *ctx) {
  if (s_detail_idx + 1 >= s_count) {
    vibes_short_pulse();
    return;
  }
  s_detail_idx++;
  if (s_mark_detail) {
    article_mark_read(s_detail_idx);
  }
  detail_render();
}

//! Long SELECT toggles the star of the shown article.
static void detail_star_long_click(ClickRecognizerRef rec, void *ctx) {
  if (s_detail_idx >= s_count) {
    return;
  }
  Article *a = &s_articles[s_detail_idx];
  a->star = a->star ? 0 : 1;
  proto_star(a->id, a->star != 0);
  vibes_short_pulse();
  if (s_tl_menu) {
    menu_layer_reload_data(s_tl_menu); // keep the timeline star marker fresh
  }
}

static void detail_back_click(ClickRecognizerRef rec, void *ctx) {
  window_stack_pop(true);
}

//! Fill the summary text layer and size the scroll content to fit it.
static void detail_set_summary(const char *text) {
  text_layer_set_text(s_summary_text, text);
  GSize ss = graphics_text_layout_get_content_size(
      text, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(0, 0, layer_get_bounds(window_get_root_layer(s_detail_window)).size.w - 8,
            2000),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  scroll_layer_set_content_size(s_detail_scroll,
                                GSize(layer_get_bounds(window_get_root_layer(
                                          s_detail_window)).size.w, ss.h + 4));
}

//! Render the current article: title (up to ~4 wrapped lines), feed·time,
//! and the summary from the single-slot cache or a fresh request.
static void detail_render(void) {
  if (!s_detail_window || !s_detail_title || !s_summary_text ||
      s_detail_idx < 0 || s_detail_idx >= s_count) {
    return;
  }
  Article *a = &s_articles[s_detail_idx];
  Layer *root = window_get_root_layer(s_detail_window);
  GRect bounds = layer_get_bounds(root);

  const char *title = a->title[0] ? a->title : a->feed;
  GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GSize ts = graphics_text_layout_get_content_size(
      title, title_font, GRect(0, 0, bounds.size.w - 8, 200),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  int16_t title_h = ts.h > 116 ? 116 : (ts.h < 26 ? 26 : ts.h);
  text_layer_set_text(s_detail_title, title);
  layer_set_frame(text_layer_get_layer(s_detail_title),
                  GRect(4, 2, bounds.size.w - 8, title_h));

  char meta[64];
  format_datetime(meta, sizeof(meta), a->published);
  size_t l = strlen(meta);
  snprintf(meta + l, sizeof(meta) - l, " \xc2\xb7 %s", a->feed[0] ? a->feed : "?");
  text_layer_set_text(s_detail_meta, meta);
  int16_t meta_y = 2 + title_h + 2;
  layer_set_frame(text_layer_get_layer(s_detail_meta),
                  GRect(4, meta_y, bounds.size.w - 8, 18));

  int16_t scroll_y = meta_y + 18 + 4;
  layer_set_frame(scroll_layer_get_layer(s_detail_scroll),
                  GRect(0, scroll_y, bounds.size.w, bounds.size.h - scroll_y));

  if (strcmp(s_summary_id, a->id) == 0) {
    // Cache hit — or the request for this id is already in flight and the
    // placeholder ("Loading...") will be replaced when the answer arrives.
    detail_set_summary(s_summary);
  } else {
    snprintf(s_summary_id, sizeof(s_summary_id), "%s", a->id);
    snprintf(s_summary, sizeof(s_summary), "Loading...");
    detail_set_summary(s_summary);
    proto_request_summary(a->id);
  }
}

//! The requested summary arrived: cache it and render if it matches the
//! article currently shown. Stale responses (user moved on) are dropped —
//! the single-slot cache only ever holds the current request.
void timeline_summary_arrived(DictionaryIterator *iter) {
  Tuple *sum_t = dict_find(iter, MESSAGE_KEY_ItemSummary);
  if (!sum_t) {
    return;
  }
  Tuple *id_t = dict_find(iter, MESSAGE_KEY_ItemId);
  const char *sid = id_t ? id_t->value->cstring : "";
  if (strcmp(s_summary_id, sid) != 0) {
    return; // stale: the user already moved to another article
  }
  snprintf(s_summary, sizeof(s_summary), "%s", sum_t->value->cstring);
  if (s_detail_window && s_summary_text && s_detail_idx >= 0 &&
      s_detail_idx < s_count && strcmp(s_articles[s_detail_idx].id, sid) == 0) {
    detail_set_summary(s_summary);
  }
}

static void detail_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  // Full-bleed window (no status bar by default on this SDK).
  window_set_background_color(window, theme_bg());

  s_detail_title = text_layer_create(GRect(4, 2, bounds.size.w - 8, 88));
  text_layer_set_font(s_detail_title, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_overflow_mode(s_detail_title, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_detail_title, GColorClear);
  text_layer_set_text_color(s_detail_title, theme_fg());
  layer_add_child(root, text_layer_get_layer(s_detail_title));

  s_detail_meta = text_layer_create(GRect(4, 94, bounds.size.w - 8, 18));
  text_layer_set_font(s_detail_meta, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_overflow_mode(s_detail_meta, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_detail_meta, GColorClear);
  text_layer_set_text_color(s_detail_meta, theme_muted());
  layer_add_child(root, text_layer_get_layer(s_detail_meta));

  s_detail_scroll = scroll_layer_create(GRect(0, 116, bounds.size.w, bounds.size.h - 116));
  scroll_layer_set_shadow_hidden(s_detail_scroll, true);
  // The scroll layer's internal provider handles UP/DOWN scrolling and then
  // calls our provider for SELECT / long-SELECT / BACK (documented chain).
  scroll_layer_set_callbacks(s_detail_scroll, (ScrollLayerCallbacks){
    .click_config_provider = detail_click_config_provider,
  });
  s_summary_text = text_layer_create(GRect(4, 0, bounds.size.w - 8, 100));
  text_layer_set_font(s_summary_text, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_summary_text, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_summary_text, GColorClear);
  text_layer_set_text_color(s_summary_text, theme_fg());
  scroll_layer_add_child(s_detail_scroll, text_layer_get_layer(s_summary_text));
  scroll_layer_set_click_config_onto_window(s_detail_scroll, window);
  layer_add_child(root, scroll_layer_get_layer(s_detail_scroll));
}

static void detail_window_unload(Window *window) {
  scroll_layer_destroy(s_detail_scroll);
  text_layer_destroy(s_detail_meta);
  text_layer_destroy(s_detail_title);
  s_detail_scroll = NULL;
  s_detail_meta = NULL;
  s_detail_title = NULL;
  s_summary_text = NULL;
  window_destroy(s_detail_window);
  s_detail_window = NULL;
}

//! Open the detail card for a given article index.
static void detail_open(int32_t idx) {
  if (idx < 0 || idx >= s_count) {
    return;
  }
  s_detail_idx = idx;
  if (!s_detail_window) {
    s_detail_window = window_create();
    window_set_window_handlers(s_detail_window, (WindowHandlers){
      .load = detail_window_load,
      .unload = detail_window_unload,
    });
  }
  window_stack_push(s_detail_window, true);
  detail_render(); // safe whether load already ran or runs on push
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

//! Re-apply accent/theme to the timeline + detail windows.
void timeline_apply_settings(void) {
  if (s_tl_window) {
    window_set_background_color(s_tl_window, theme_bg());
  }
  if (s_tl_header) {
    text_layer_set_text_color(s_tl_header, theme_fg());
  }
  if (s_tl_menu) {
    menu_layer_set_normal_colors(s_tl_menu, theme_bg(), theme_fg());
    menu_layer_set_highlight_colors(s_tl_menu, s_accent, GColorWhite);
  }
  if (s_detail_window) {
    window_set_background_color(s_detail_window, theme_bg());
  }
  if (s_detail_title) {
    text_layer_set_text_color(s_detail_title, theme_fg());
  }
  if (s_detail_meta) {
    text_layer_set_text_color(s_detail_meta, theme_muted());
  }
  if (s_summary_text) {
    text_layer_set_text_color(s_summary_text, theme_fg());
  }
}
