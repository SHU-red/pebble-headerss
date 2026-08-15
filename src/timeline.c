#include <pebble.h>

#include "common.h"
#include "storage.h"
#include "tree.h"
#include "proto.h"
#include "timeline.h"

// ---------------------------------------------------------------------------
// Timeline reading view (see timeline.h). Article headings + summaries
// stream in from the phone into a ring buffer; pages are dropped from the
// head when the buffer fills and re-fetched via continuation. The visual
// centerpiece is a native-Timeline-style list: a permanent accent spine on
// the right edge, one dot per entry on the spine (accent = unread, muted =
// read), a yellow star on starred entries, and on the selected row an
// accent wash + pin notch + popping dot. There is no detail view — every
// row always shows heading + summary.
// ---------------------------------------------------------------------------

#define TIMELINE_HEADER_H 26 // custom title bar (SDK3 has no window title)
#define SPINE_W 4            // accent spine width

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
static Layer *s_tl_underline; // 2px accent underline under the header
static bool s_tl_visible;

// Shared draw paths: a 5-point star (~12 px) and the pin notch
// (right-pointing triangle, 9x10). Repositioned per row with gpath_move_to.
static const GPathInfo STAR_PATH_INFO = {
  .num_points = 10,
  .points = (GPoint[10]){
    { 0, -6 }, { 2, -2 }, { 6, -2 }, { 3, 1 }, { 4, 6 },
    { 0, 3 }, { -4, 6 }, { -3, 1 }, { -6, -2 }, { -2, -2 },
  },
};
static const GPathInfo PIN_PATH_INFO = {
  .num_points = 3,
  .points = (GPoint[3]){ { 0, -5 }, { 9, 0 }, { 0, 5 } }, // apex right, at the spine
};

static GPath *s_star_path;
static GPath *s_pin_path;

// ---------------------------------------------------------------------------
// Selected-row animations: the accent wash tint (GColor8 lerp, launcher
// idiom) and the dot pop (radius). Both are plain AnimationImplementations
// marking the menu dirty per frame — no float math.
// ---------------------------------------------------------------------------

static GColor s_tint_cur;
static GColor s_tint_from;
static GColor s_tint_to;
static Animation *s_tint_anim;
static int16_t s_dot_cur; // selected dot radius
static int16_t s_dot_from;
static int16_t s_dot_to;
static Animation *s_dot_anim;

//! GColor8 channel lerp (the 2-bit channels interpolate per frame).
static GColor color8_lerp(GColor from, GColor to, uint32_t num, uint32_t den) {
  if (den == 0) {
    return to;
  }
  GColor c = GColorFromRGB(
      (uint8_t)(from.r + (((int16_t)to.r - from.r) * (int16_t)num) / (int16_t)den),
      (uint8_t)(from.g + (((int16_t)to.g - from.g) * (int16_t)num) / (int16_t)den),
      (uint8_t)(from.b + (((int16_t)to.b - from.b) * (int16_t)num) / (int16_t)den));
  c.a = from.a;
  return c;
}

//! Accent washed toward the theme background — the selection wash color.
static GColor selection_tint(void) {
  return color8_lerp(s_accent, theme_bg(), 164, 200);
}

static void tint_anim_update(Animation *anim, const AnimationProgress progress) {
  s_tint_cur = color8_lerp(s_tint_from, s_tint_to, progress, ANIMATION_NORMALIZED_MAX);
  if (s_tl_menu) {
    layer_mark_dirty(menu_layer_get_layer(s_tl_menu));
  }
}

static void tint_anim_stopped(Animation *anim, bool finished, void *context) {
  if (anim != s_tint_anim) {
    return; // superseded by a newer animation
  }
  s_tint_anim = NULL;
  s_tint_cur = s_tint_to;
  if (s_tl_menu) {
    layer_mark_dirty(menu_layer_get_layer(s_tl_menu));
  }
}

static const AnimationImplementation TINT_ANIM_IMPL = {
  .update = tint_anim_update,
};

static void dot_anim_update(Animation *anim, const AnimationProgress progress) {
  s_dot_cur = s_dot_from +
              (int16_t)(((int32_t)(s_dot_to - s_dot_from) * (int32_t)progress) /
                        (int32_t)ANIMATION_NORMALIZED_MAX);
  if (s_tl_menu) {
    layer_mark_dirty(menu_layer_get_layer(s_tl_menu));
  }
}

static void dot_anim_stopped(Animation *anim, bool finished, void *context) {
  if (anim != s_dot_anim) {
    return;
  }
  s_dot_anim = NULL;
  s_dot_cur = s_dot_to;
  if (s_tl_menu) {
    layer_mark_dirty(menu_layer_get_layer(s_tl_menu));
  }
}

static const AnimationImplementation DOT_ANIM_IMPL = {
  .update = dot_anim_update,
};

//! Kick both selection animations off (tint wash 220 ms, dot pop 180 ms).
static void selection_animate(void) {
  if (s_tint_anim) {
    animation_unschedule(s_tint_anim);
  }
  s_tint_from = s_tint_cur;
  s_tint_to = selection_tint();
  s_tint_anim = animation_create();
  animation_set_duration(s_tint_anim, 220);
  animation_set_curve(s_tint_anim, AnimationCurveEaseInOut);
  animation_set_implementation(s_tint_anim, &TINT_ANIM_IMPL);
  animation_set_handlers(s_tint_anim, (AnimationHandlers){
    .stopped = tint_anim_stopped,
  }, NULL);
  animation_schedule(s_tint_anim);

  if (s_dot_anim) {
    animation_unschedule(s_dot_anim);
  }
  s_dot_from = s_dot_cur;
  s_dot_to = 4;
  s_dot_anim = animation_create();
  animation_set_duration(s_dot_anim, 180);
  animation_set_curve(s_dot_anim, AnimationCurveEaseOut);
  animation_set_implementation(s_dot_anim, &DOT_ANIM_IMPL);
  animation_set_handlers(s_dot_anim, (AnimationHandlers){
    .stopped = dot_anim_stopped,
  }, NULL);
  animation_schedule(s_dot_anim);
}

// ---------------------------------------------------------------------------
// Small formatters
// ---------------------------------------------------------------------------

//! Compact relative time: "now", "5m", "3h", "2d", else "dd.mm.".
static void format_reltime(char *buf, size_t len, int32_t published) {
  time_t now = time(NULL);
  int32_t diff = (int32_t)(now - published);
  if (diff < 0) {
    diff = 0;
  }
  if (diff < 60) {
    snprintf(buf, len, "now");
  } else if (diff < 3600) {
    snprintf(buf, len, "%ldm", (long)(diff / 60));
  } else if (diff < 86400) {
    snprintf(buf, len, "%ldh", (long)(diff / 3600));
  } else if (diff < 172800) {
    snprintf(buf, len, "2d");
  } else {
    struct tm *tm = localtime(&published);
    if (tm) {
      snprintf(buf, len, "%02d.%02d.", tm->tm_mday, tm->tm_mon + 1);
    } else {
      snprintf(buf, len, "?");
    }
  }
}

// ---------------------------------------------------------------------------
// Timeline window
// ---------------------------------------------------------------------------

static void timeline_prefetch_check(void);
static void article_mark_read(int32_t idx);

static void timeline_selection_changed(void) {
  selection_animate();
  timeline_prefetch_check();
}

static void timeline_up_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  if (idx.row > 0) {
    idx.row--;
    menu_layer_set_selected_index(s_tl_menu, idx, MenuRowAlignCenter, true);
    timeline_selection_changed();
  }
}

static void timeline_down_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  if (idx.row + 1 < (uint16_t)s_count) {
    idx.row++;
    menu_layer_set_selected_index(s_tl_menu, idx, MenuRowAlignCenter, true);
    timeline_selection_changed();
  }
}

//! SELECT: mark the article read (per the list toggle), then advance to the
//! next article (marking it per the detail toggle). The list IS the reader —
//! there is no detail view.
static void timeline_select_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  if (s_count == 0 || idx.row >= (uint16_t)s_count) {
    return;
  }
  if (s_mark_list) {
    article_mark_read((int32_t)idx.row);
  }
  if (idx.row + 1 < (uint16_t)s_count) {
    idx.row++;
    menu_layer_set_selected_index(s_tl_menu, idx, MenuRowAlignCenter, true);
    timeline_selection_changed();
    if (s_mark_detail) {
      article_mark_read((int32_t)idx.row);
    }
  } else {
    vibes_short_pulse(); // end of the list
  }
}

//! Long SELECT toggles the star of the selected entry (fire-and-forget).
static void timeline_star_long_click(ClickRecognizerRef rec, void *ctx) {
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  if (s_count == 0 || idx.row >= (uint16_t)s_count) {
    return;
  }
  Article *a = &s_articles[idx.row];
  a->star = !a->star;
  proto_star(a->id, a->star);
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

//! Timeline cell: accent wash on the selected row, right-edge accent spine,
//! dot per entry (accent unread / muted read, popping on selection), yellow
//! star for starred entries, pin notch on the selected row, and the
//! heading + summary text block.
static void timeline_draw_row(GContext *ctx, const Layer *cell_layer,
                              MenuIndex *cell_index, void *callback_context) {
  GRect b = layer_get_bounds(cell_layer);

  if (s_count == 0) {
    graphics_context_set_text_color(ctx, theme_muted());
    graphics_draw_text(ctx, s_loading ? "Loading..." : "No articles",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(4, 0, b.size.w - 8, b.size.h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  uint16_t row = cell_index->row;
  if (row >= (uint16_t)s_count) {
    return;
  }
  const Article *a = &s_articles[row];
  bool selected = menu_layer_is_index_selected(s_tl_menu, cell_index);

  // Selection accent wash (animated).
  if (selected) {
    graphics_context_set_fill_color(ctx, s_tint_cur);
    graphics_fill_rect(ctx, GRect(0, 0, b.size.w, b.size.h), 0, GCornerNone);
  }

  // Permanent accent spine on the right edge.
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, GRect(b.size.w - SPINE_W - 2, 0, SPINE_W, b.size.h), 0, GCornerNone);

  int32_t cx = b.size.w - SPINE_W - 2 + SPINE_W / 2; // spine center x
  int32_t cy = b.size.h / 2;

  // Dot on the spine: accent = unread, muted = read; popping on selection.
  int32_t dot_r = selected ? s_dot_cur : 3;
  graphics_context_set_fill_color(ctx, a->read ? theme_muted() : s_accent);
  graphics_fill_circle(ctx, GPoint(cx, cy), (int16_t)dot_r);

  // Yellow star for starred entries, left of the spine.
  if (a->star && s_star_path) {
    graphics_context_set_fill_color(ctx, GColorYellow);
    gpath_move_to(s_star_path, GPoint(cx - 14, cy));
    gpath_draw_filled(ctx, s_star_path);
  }

  // Pin notch on the selected row, apex at the spine.
  if (selected && s_pin_path) {
    graphics_context_set_fill_color(ctx, s_accent);
    gpath_move_to(s_pin_path, GPoint(cx - 7, cy));
    gpath_draw_filled(ctx, s_pin_path);
  }

  // Heading (1 line, bold when unread) + summary (2 lines) + feed · time.
  int16_t text_right = b.size.w - SPINE_W - 2 - 16;
  graphics_context_set_text_color(ctx, a->read ? theme_muted() : theme_fg());
  graphics_draw_text(ctx, a->title,
                     fonts_get_system_font(a->read ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_18_BOLD),
                     GRect(4, 3, text_right - 4, 19),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  graphics_context_set_text_color(ctx, theme_muted());
  graphics_draw_text(ctx, a->summary,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(4, 24, text_right - 4, 30),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  char meta[48];
  char t[16];
  format_reltime(t, sizeof(t), a->published);
  snprintf(meta, sizeof(meta), "%s · %s", a->feed, t);
  graphics_draw_text(ctx, meta,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(4, 56, text_right - 4, 14),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void header_underline_update(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

static void timeline_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  // Custom title bar: this SDK has no window title/fullscreen API — windows
  // are full-bleed by default, so the header sits at the top with a 2px
  // accent underline.
  window_set_background_color(window, theme_bg());

  s_star_path = gpath_create(&STAR_PATH_INFO);
  s_pin_path = gpath_create(&PIN_PATH_INFO);

  s_tl_header = text_layer_create(GRect(4, 2, bounds.size.w - 8, 22));
  text_layer_set_font(s_tl_header, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_tl_header, GTextAlignmentLeft);
  text_layer_set_overflow_mode(s_tl_header, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_tl_header, GColorClear);
  text_layer_set_text_color(s_tl_header, theme_fg());
  text_layer_set_text(s_tl_header, s_title);
  layer_add_child(root, text_layer_get_layer(s_tl_header));

  s_tl_underline = layer_create(GRect(0, TIMELINE_HEADER_H - 2, bounds.size.w, 2));
  layer_set_update_proc(s_tl_underline, header_underline_update);
  layer_add_child(root, s_tl_underline);

  s_tl_menu = menu_layer_create(
      GRect(0, TIMELINE_HEADER_H, bounds.size.w, bounds.size.h - TIMELINE_HEADER_H));
  menu_layer_set_callbacks(s_tl_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = timeline_get_num_rows,
    .get_cell_height = timeline_get_cell_height,
    .draw_row = timeline_draw_row,
  });
  // Native highlight is disabled (normal == highlight colors); the accent
  // wash + pin are drawn by draw_row so they can be animated.
  menu_layer_set_normal_colors(s_tl_menu, theme_bg(), theme_fg());
  menu_layer_set_highlight_colors(s_tl_menu, theme_bg(), theme_fg());
  menu_layer_pad_bottom_enable(s_tl_menu, true);
  layer_add_child(root, menu_layer_get_layer(s_tl_menu));

  window_set_click_config_provider(window, timeline_click_config_provider);

  s_tint_cur = selection_tint();
  s_dot_cur = 4;
  s_tl_visible = true;
}

//! Teardown on window unload: flush any pending mark-read batch and release
//! every layer/path so a later re-open builds fresh.
static void timeline_close(void) {
  proto_flush_now();
  s_tl_menu = NULL;
  s_tl_header = NULL;
  s_tl_underline = NULL;
}

static void timeline_window_unload(Window *window) {
  timeline_close();
  if (s_star_path) {
    gpath_destroy(s_star_path);
    s_star_path = NULL;
  }
  if (s_pin_path) {
    gpath_destroy(s_pin_path);
    s_pin_path = NULL;
  }
  menu_layer_destroy(s_tl_menu);
  text_layer_destroy(s_tl_header);
  layer_destroy(s_tl_underline);
  window_destroy(s_tl_window);
  s_tl_window = NULL;
}

static void timeline_window_appear(Window *window) {
  s_tl_visible = true;
}

static void timeline_window_disappear(Window *window) {
  s_tl_visible = false;
}

//! Open (or reset and re-open) the timeline for a stream. State is reset and
//! the first page is requested; the menu shows a "Loading..." hint row until
//! the first page arrives.
void timeline_open(const char *stream, const char *title) {
  if (s_tl_window) {
    window_stack_remove(s_tl_window, true);
  }
  snprintf(s_stream, sizeof(s_stream), "%s", stream ? stream : "");
  snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
  s_count = 0;
  s_cont[0] = '\0';
  s_loading = true;
  s_loaded_all = false;
  s_tint_cur = selection_tint();
  s_dot_cur = 4;

  s_tl_window = window_create();
  window_set_window_handlers(s_tl_window, (WindowHandlers){
    .load = timeline_window_load,
    .unload = timeline_window_unload,
    .appear = timeline_window_appear,
    .disappear = timeline_window_disappear,
  });
  window_stack_push(s_tl_window, true);

  proto_request_items(s_stream, "");
}

// ---------------------------------------------------------------------------
// Item-page collect hooks (called from proto_handle_inbox)
// ---------------------------------------------------------------------------

//! A new page is starting: make room in the ring buffer by dropping the
//! newest entries (the user has already scrolled past them) when the page
//! would overflow.
void timeline_page_begin(int32_t n) {
  int32_t need = n < 0 ? 0 : n;
  if (s_count + need > MAX_ARTICLES) {
    int32_t drop = s_count + need - MAX_ARTICLES;
    if (drop >= s_count) {
      s_count = 0;
    } else {
      memmove(s_articles, &s_articles[drop],
              (size_t)(s_count - drop) * sizeof(Article));
      s_count -= drop;
    }
  }
}

//! Append one article (heading + summary) from the current message
//! (bounds-checked; overflow drops the oldest from the front).
void timeline_collect_article(DictionaryIterator *iter) {
  if (s_count >= MAX_ARTICLES) {
    memmove(s_articles, &s_articles[1], (size_t)(s_count - 1) * sizeof(Article));
    s_count--;
  }
  Article *a = &s_articles[s_count++];
  memset(a, 0, sizeof(*a));

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
  if ((t = dict_find(iter, MESSAGE_KEY_ItemSummary))) {
    snprintf(a->summary, sizeof(a->summary), "%s", t->value->cstring);
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemTime))) {
    a->published = t->value->int32;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemRead))) {
    a->read = t->value->int32 ? 1 : 0;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemStar))) {
    a->star = t->value->int32 ? 1 : 0;
  }
}

//! The page ended: store the continuation ("", = no more items), release the
//! loading flag and redraw.
void timeline_page_end(const char *cont) {
  snprintf(s_cont, sizeof(s_cont), "%s", cont ? cont : "");
  s_loaded_all = (s_cont[0] == '\0');
  s_loading = false;
  if (s_tl_menu) {
    menu_layer_reload_data(s_tl_menu);
  }
}

//! Prefetch the next page when the selection enters the last 6 rows.
static void timeline_prefetch_check(void) {
  if (!s_tl_menu || s_loading || s_loaded_all || s_count == 0) {
    return;
  }
  MenuIndex idx = menu_layer_get_selected_index(s_tl_menu);
  if (idx.row + 6 >= (uint16_t)s_count) {
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
  proto_mark_push(a->id);
  tree_feed_decrement(a->feed_id);
  if (s_tl_menu) {
    menu_layer_reload_data(s_tl_menu);
  }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

//! Re-apply accent/theme to the timeline window (from settings).
void timeline_apply_settings(void) {
  if (!s_tl_window) {
    return;
  }
  window_set_background_color(s_tl_window, theme_bg());
  if (s_tl_header) {
    text_layer_set_text_color(s_tl_header, theme_fg());
  }
  if (s_tl_underline) {
    layer_mark_dirty(s_tl_underline);
  }
  if (s_tl_menu) {
    menu_layer_set_normal_colors(s_tl_menu, theme_bg(), theme_fg());
    menu_layer_set_highlight_colors(s_tl_menu, theme_bg(), theme_fg());
    layer_mark_dirty(menu_layer_get_layer(s_tl_menu));
  }
  s_tint_cur = selection_tint();
}
