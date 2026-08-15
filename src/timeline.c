#include <pebble.h>

#include "common.h"
#include "storage.h"
#include "tree.h"
#include "proto.h"
#include "timeline.h"

// ---------------------------------------------------------------------------
// Timeline reading view (see timeline.h). Article headings + summaries
// stream in from the phone into a ring buffer. The reader is a paged
// full-screen view: a black top bar (stream name in accent), one article per
// page — an accent header bar (heading + feed·time), a scrollable summary
// body — and a thick accent SIDEBAR on the right holding the read/unread dot
// and the star icon. Page changes slide with a continuous two-page
// transition (the outgoing page leaves while the incoming one enters — no
// teleport cut).
// ---------------------------------------------------------------------------

#define TOP_BAR_H 24      // black top bar with the stream name
#define SIDEBAR_W 26      // thick accent sidebar holding the icons
#define HEADER_FLOOR 52   // minimum header height
#define HEADER_META_H 22  // feed·time line + padding below the heading
#define HEADING_MAX_H 42  // two GOTHIC_18 lines

static Article s_articles[MAX_ARTICLES];
static int32_t s_count;
static char s_stream[48]; // current stream id
static char s_cont[24];   // next continuation; "" = all loaded
static bool s_loading;
static bool s_loaded_all;
static char s_title[24]; // stream title (top bar)

static int32_t s_idx;        // current article (ring-buffer index)
static bool s_advancing;     // true while a page transition runs
static bool s_advance_guard; // at most one advance per article
static GPoint s_last_offset; // last scroll offset seen (current page)

static int16_t s_win_w;
static int16_t s_win_h;
static int16_t s_view_h; // page height (window minus the top bar)

static Window *s_tl_window;
static Layer *s_root;
static Layer *s_page_area; // holds the pages; added BEFORE the sidebar so the
                           // accent icon bar always stays on top
static Layer *s_top_bar;   // black bar, accent text (static)
static TextLayer *s_top_text;
static Layer *s_sidebar;   // accent bar with the read/unread + star icons
static TextLayer *s_status;   // full-screen "Loading..." / "All caught up"
static Layer *s_status_check; // accent GPath check above the status text
static TextLayer *s_status_hint; // small hint under the status text

// One article page: everything that slides during a transition. Two pages
// exist so the transition can animate them against each other.
typedef struct {
  Layer *root;        // container layer (slides)
  Layer *header;      // accent header (dynamic height)
  ScrollLayer *scroll; // summary body
  TextLayer *body;    // wrapped summary, recreated per page
  int32_t idx;        // ring index this page shows
} Page;

static Page s_pages[2];
static int s_cur; // index of the current page in s_pages

static Page *cur_page(void) { return &s_pages[s_cur]; }
static Page *spare_page(void) { return &s_pages[1 - s_cur]; }

// Transition state: direction, target index, the two page-frame animations
// (from/to frames must outlive the animations).
static int8_t s_dir;
static int32_t s_target_idx;
static Animation *s_anim_a; // current page slides out
static Animation *s_anim_b; // spare page slides in
static GRect s_from_a, s_to_a, s_from_b, s_to_b;

// Shared draw path: a 5-point star (~16 px) drawn in the sidebar.
static const GPathInfo STAR_PATH_INFO = {
  .num_points = 10,
  .points = (GPoint[10]){
    { 0, -8 }, { 2, -2 }, { 8, -2 }, { 5, 1 }, { 6, 8 },
    { 0, 4 }, { -6, 8 }, { -5, 1 }, { -8, -2 }, { -2, -2 },
  },
};

static GPath *s_star_path;

// Shared draw path: a closed check mark (~16 px) for the all-caught-up
// status; drawn in accent above the status text.
static const GPathInfo CHECK_PATH_INFO = {
  .num_points = 6,
  .points = (GPoint[6]){
    { -7, 2 }, { -1, 8 }, { 7, -3 },
    { 5, -5 }, { -4, 2 }, { -8, 0 },
  },
};

static GPath *s_status_check_path;

static void timeline_prefetch_check(void);
static void timeline_content_offset_changed(ScrollLayer *scroll, void *context);
static void article_mark_read(int32_t idx);
static void maybe_advance(void);
static void maybe_regress(void);
static void transition_to(int8_t dir);
static void page_build(Page *p, int32_t idx);
static void page_destroy(Page *p);
static void status_update(void);
static void status_check_update(Layer *layer, GContext *ctx);
static void transition_anim_stopped(Animation *anim, bool finished, void *context);

//! The article currently under the reader, or NULL when the buffer is empty.
static const Article *current_article(void) {
  if (s_idx < 0 || s_idx >= s_count) {
    return NULL;
  }
  return &s_articles[s_idx];
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
// Static chrome: top bar + sidebar
// ---------------------------------------------------------------------------

//! Black top bar; the stream name is a TextLayer in accent. A 2 px accent
//! progress line along the bar's bottom edge tracks the current article
//! position within the loaded stream (gated by the progress setting).
static void top_bar_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  if (setting_progress() && s_count > 0) {
    int16_t w = (int16_t)((int32_t)(s_idx + 1) * b.size.w / s_count);
    if (w > 0) {
      graphics_context_set_fill_color(ctx, s_accent);
      graphics_fill_rect(ctx, GRect(0, b.size.h - 2, w, 2), 0, GCornerNone);
    }
  }
}

//! Thick accent sidebar holding the icons: a white dot (filled = unread,
//! outline = read) and, when starred, the yellow star above it.
static void sidebar_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  const Article *a = current_article();
  if (!a) {
    return;
  }
  int16_t cx = b.size.w / 2;
  int16_t cy = b.size.h / 2;

  if (a->star && s_star_path) {
    gpath_move_to(s_star_path, GPoint(cx, cy - 18));
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 2);
    gpath_draw_outline(ctx, s_star_path);
    graphics_context_set_fill_color(ctx, GColorYellow);
    gpath_draw_filled(ctx, s_star_path);
  }

  // Read/unread dot: filled = unread, outline = read.
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 2);
  if (a->read) {
    graphics_draw_circle(ctx, GPoint(cx, cy + (a->star ? 16 : 0)), 5);
  } else {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, GPoint(cx, cy + (a->star ? 16 : 0)), 5);
  }
}

// ---------------------------------------------------------------------------
// Page surfaces
// ---------------------------------------------------------------------------

//! Which ring index a header layer belongs to (there are only two pages).
static int32_t header_article_idx(Layer *header) {
  for (int i = 0; i < 2; i++) {
    if (s_pages[i].header == header) {
      return s_pages[i].idx;
    }
  }
  return -1;
}

//! Accent header bar: always accent background with black text — the read
//! state lives in the sidebar icons, not the colors.
static void header_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  int32_t idx = header_article_idx(layer);
  const Article *a = (idx >= 0 && idx < s_count) ? &s_articles[idx] : NULL;
  if (!a) {
    return;
  }

  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, a->title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(4, 2, b.size.w - SIDEBAR_W - 8, HEADING_MAX_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  char meta[48];
  char t[16];
  format_reltime(t, sizeof(t), a->published);
  snprintf(meta, sizeof(meta), "%s · %s", a->feed, t);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, meta, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(4, b.size.h - 18, b.size.w - SIDEBAR_W - 8, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

//! Dynamic header height: heading (capped at two GOTHIC_18 lines) plus room
//! for the feed·time line, floored at 52 px.
static int16_t header_height_for(const Article *a) {
  GSize sz = graphics_text_layout_get_content_size(
      a->title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(4, 0, s_win_w - SIDEBAR_W - 8, 1000),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  int32_t h = sz.h;
  if (h > HEADING_MAX_H) {
    h = HEADING_MAX_H;
  }
  h += HEADER_META_H;
  return (int16_t)(h < HEADER_FLOOR ? HEADER_FLOOR : h);
}

//! Build one article page (header + scrollable summary) into a fresh set of
//! layers; the body text stays clear of the sidebar.
static void page_build(Page *p, int32_t idx) {
  const Article *a = &s_articles[idx];
  p->idx = idx;

  p->root = layer_create(GRect(0, TOP_BAR_H, s_win_w, s_view_h));
  layer_add_child(s_page_area, p->root);

  int16_t hh = header_height_for(a);
  p->header = layer_create(GRect(0, 0, s_win_w, hh));
  layer_set_update_proc(p->header, header_update);
  layer_add_child(p->root, p->header);

  p->scroll = scroll_layer_create(GRect(0, hh, s_win_w, s_view_h - hh));
  scroll_layer_set_callbacks(p->scroll, (ScrollLayerCallbacks){
    .content_offset_changed_handler = timeline_content_offset_changed,
  });
  scroll_layer_set_shadow_hidden(p->scroll, true);
  layer_add_child(p->root, scroll_layer_get_layer(p->scroll));

  GSize sz = graphics_text_layout_get_content_size(
      a->summary, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(4, 2, s_win_w - SIDEBAR_W - 8, 1000),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  int32_t body_h = sz.h + 6; // 2 px top pad + 4 px air at the bottom

  p->body = text_layer_create(GRect(4, 2, s_win_w - SIDEBAR_W - 8, sz.h));
  text_layer_set_font(p->body, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(p->body, GTextAlignmentLeft);
  text_layer_set_overflow_mode(p->body, GTextOverflowModeWordWrap);
  text_layer_set_background_color(p->body, theme_bg());
  text_layer_set_text_color(p->body, theme_fg());
  text_layer_set_text(p->body, a->summary);
  layer_add_child(scroll_layer_get_layer(p->scroll), text_layer_get_layer(p->body));

  scroll_layer_set_content_size(p->scroll, GSize(s_win_w - SIDEBAR_W, body_h));
  scroll_layer_set_content_offset(p->scroll, GPointZero, false);
}

static void page_destroy(Page *p) {
  if (p->body) {
    text_layer_destroy(p->body);
    p->body = NULL;
  }
  if (p->scroll) {
    scroll_layer_destroy(p->scroll);
    p->scroll = NULL;
  }
  if (p->header) {
    layer_destroy(p->header);
    p->header = NULL;
  }
  if (p->root) {
    layer_destroy(p->root);
    p->root = NULL;
  }
  p->idx = -1;
}

// ---------------------------------------------------------------------------
// Page transition (continuous two-page slide)
// ---------------------------------------------------------------------------

//! Scroll tracking: when a scroll (button paging or touch fling) lands the
//! body at its bottom, advance to the next article.
static void timeline_content_offset_changed(ScrollLayer *scroll, void *context) {
  if (s_count == 0 || s_advancing || s_advance_guard) {
    return;
  }
  GPoint offset = scroll_layer_get_content_offset(scroll);
  if (offset.y == s_last_offset.y) {
    return; // unchanged
  }
  s_last_offset = offset;
  GRect frame = layer_get_frame(scroll_layer_get_layer(scroll));
  GSize content = scroll_layer_get_content_size(scroll);
  if (content.h <= frame.size.h + 2 || offset.y >= content.h - frame.size.h - 2) {
    maybe_advance();
  }
}

//! Transition finished: the target page is fully on screen. Commit the new
//! index, mark read on advances, drain the triage stream (unstar the article
//! we landed on when advancing inside Starred), drop the old page and
//! release the locks.
static void transition_finalize(void) {
  s_idx = s_target_idx;
  if (s_dir > 0 && s_mark_detail) {
    article_mark_read(s_idx);
  }
  if (s_dir > 0 && setting_triage() &&
      strcmp(s_stream, "user/-/state/com.google/starred") == 0) {
    if (s_idx >= 0 && s_idx < s_count) {
      Article *a = &s_articles[s_idx];
      a->star = 0;
      proto_star(a->id, 0);
    }
  }
  page_destroy(&s_pages[s_cur]);
  s_cur = 1 - s_cur;
  s_advancing = false;
  s_advance_guard = false;
  s_last_offset = GPointZero;
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar);
  }
  if (s_top_bar) {
    layer_mark_dirty(s_top_bar); // progress line follows the new index
  }
  timeline_prefetch_check();
}

//! The current page's out-animation stopped: finalize the swap. On an
//! interrupted stop (teardown unschedule) the pages are torn down by the
//! caller instead.
static void transition_anim_stopped(Animation *anim, bool finished, void *context) {
  if (anim != s_anim_a) {
    return;
  }
  s_anim_a = NULL;
  s_anim_b = NULL;
  if (finished) {
    transition_finalize();
  }
}

//! Start a transition in the given direction (+1 next, -1 previous): build
//! the target page, park it off-screen on the entry side, then slide the
//! current page out while the target slides in — both 260 ms ease-in-out,
//! one continuous sheet, no cut.
static void transition_to(int8_t dir) {
  if (s_advancing) {
    return;
  }
  int32_t nidx = s_idx + dir;
  if (nidx < 0 || nidx >= s_count) {
    vibes_short_pulse(); // first/last article: stay
    return;
  }
  s_advancing = true;
  s_advance_guard = (dir > 0);
  s_dir = dir;
  s_target_idx = nidx;

  Page *sp = spare_page();
  page_build(sp, nidx);
  layer_set_frame(sp->root, GRect(0, TOP_BAR_H + dir * s_view_h, s_win_w, s_view_h));

  Page *cp = cur_page();

  s_from_a = layer_get_frame(cp->root);
  s_to_a = GRect(0, TOP_BAR_H - dir * s_view_h, s_win_w, s_view_h);
  s_anim_a = (Animation *)property_animation_create_layer_frame(cp->root, &s_from_a, &s_to_a);
  animation_set_duration(s_anim_a, 260);
  animation_set_curve(s_anim_a, AnimationCurveEaseInOut);
  animation_set_handlers(s_anim_a, (AnimationHandlers){
    .stopped = transition_anim_stopped,
  }, NULL);
  animation_schedule(s_anim_a);

  s_from_b = layer_get_frame(sp->root);
  s_to_b = GRect(0, TOP_BAR_H, s_win_w, s_view_h);
  s_anim_b = (Animation *)property_animation_create_layer_frame(sp->root, &s_from_b, &s_to_b);
  animation_set_duration(s_anim_b, 260);
  animation_set_curve(s_anim_b, AnimationCurveEaseInOut);
  animation_schedule(s_anim_b);
}

//! Advance to the next article (guarded).
static void maybe_advance(void) {
  if (s_advancing || s_advance_guard) {
    return;
  }
  if (s_count == 0) {
    return;
  }
  transition_to(1);
}

//! Go back to the previously read article.
static void maybe_regress(void) {
  if (s_advancing || s_advance_guard) {
    return;
  }
  if (s_count == 0) {
    return;
  }
  transition_to(-1);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

//! UP scrolls the body up; past the top it goes back to the previously
//! read article (still in the ring from this session).
static void timeline_up_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0 || s_advancing) {
    return;
  }
  ScrollLayer *scroll = cur_page()->scroll;
  if (scroll_layer_get_content_offset(scroll).y > 0) {
    scroll_layer_scroll_up_click_handler(rec, scroll);
  } else {
    maybe_regress();
  }
}

//! DOWN scrolls the body; at the bottom (or when it all fits) it advances
//! to the next article instead.
static void timeline_down_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0 || s_advancing || s_advance_guard) {
    return;
  }
  ScrollLayer *scroll = cur_page()->scroll;
  GRect frame = layer_get_frame(scroll_layer_get_layer(scroll));
  GSize content = scroll_layer_get_content_size(scroll);
  GPoint offset = scroll_layer_get_content_offset(scroll);
  if (content.h <= frame.size.h + 2 || offset.y >= content.h - frame.size.h - 2) {
    maybe_advance();
  } else {
    scroll_layer_scroll_down_click_handler(rec, scroll);
  }
}

//! SELECT advances to the next article; on an empty (all-caught-up) stream
//! it re-fetches the first page (the status hint points at this).
static void timeline_select_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0) {
    if (!s_loading) {
      s_loading = true;
      proto_request_items(s_stream, "");
      status_update();
    }
    return;
  }
  if (s_advancing) {
    return;
  }
  maybe_advance();
}

//! Long SELECT toggles the star of the current article (fire-and-forget);
//! the star icon lives in the sidebar.
static void timeline_star_long_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0 || s_advancing) {
    return;
  }
  Article *a = &s_articles[s_idx];
  a->star = !a->star;
  proto_star(a->id, a->star);
  vibes_short_pulse();
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar);
  }
}

static void timeline_back_click(ClickRecognizerRef rec, void *ctx) {
  window_stack_pop(true);
}

//! Custom click config: UP/DOWN are subscribed here (never
//! scroll_layer_set_click_config_onto_window) so the reader controls the
//! advance semantics; scrolling delegates to the scroll layer's handlers.
static void timeline_click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, timeline_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, timeline_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, timeline_select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, timeline_star_long_click, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, timeline_back_click);
}

// ---------------------------------------------------------------------------
// Timeline window
// ---------------------------------------------------------------------------

//! Accent check mark drawn above the status text when the stream came up
//! empty (all caught up).
static void status_check_update(Layer *layer, GContext *ctx) {
  if (!s_status_check_path) {
    return;
  }
  GRect b = layer_get_bounds(layer);
  gpath_move_to(s_status_check_path, GPoint(b.size.w / 2, b.size.h / 2));
  graphics_context_set_stroke_color(ctx, s_accent);
  graphics_context_set_stroke_width(ctx, 3);
  gpath_draw_outline(ctx, s_status_check_path);
}

//! Full-screen status while the stream loads or comes up empty; hidden as
//! soon as the first article arrives. An empty, fully-loaded stream shows
//! the all-caught-up state: accent check + "All caught up" + a hint.
static void status_update(void) {
  if (!s_status) {
    return;
  }
  bool empty = (s_count == 0);
  layer_set_hidden(text_layer_get_layer(s_status), !empty);
  if (s_status_check) {
    layer_set_hidden(s_status_check, !empty || s_loading);
  }
  if (s_status_hint) {
    layer_set_hidden(text_layer_get_layer(s_status_hint), !empty || s_loading);
  }
  if (empty) {
    if (s_loading) {
      text_layer_set_text(s_status, "Loading...");
    } else {
      text_layer_set_text(s_status, "All caught up");
      if (s_status_hint) {
        text_layer_set_text(s_status_hint, "SELECT to refresh");
      }
    }
  }
}

static void timeline_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_win_w = bounds.size.w;
  s_win_h = bounds.size.h;
  s_view_h = s_win_h - TOP_BAR_H;
  s_root = root;

  window_set_background_color(window, theme_bg());

  s_star_path = gpath_create(&STAR_PATH_INFO);
  s_status_check_path = gpath_create(&CHECK_PATH_INFO);
  s_last_offset = GPointZero;
  s_cur = 0;
  s_pages[0].idx = -1;
  s_pages[1].idx = -1;

  // Black top bar with the stream name in accent.
  s_top_bar = layer_create(GRect(0, 0, s_win_w, TOP_BAR_H));
  layer_set_update_proc(s_top_bar, top_bar_update);
  layer_add_child(root, s_top_bar);

  s_top_text = text_layer_create(GRect(4, 1, s_win_w - SIDEBAR_W - 8, TOP_BAR_H - 2));
  text_layer_set_font(s_top_text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_top_text, GTextAlignmentLeft);
  text_layer_set_overflow_mode(s_top_text, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_top_text, GColorClear);
  text_layer_set_text_color(s_top_text, s_accent);
  text_layer_set_text(s_top_text, s_title);
  layer_add_child(root, text_layer_get_layer(s_top_text));

  s_status = text_layer_create(bounds);
  text_layer_set_font(s_status, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_status, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_status, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_status, GColorClear);
  text_layer_set_text_color(s_status, theme_muted());
  text_layer_set_text(s_status, s_loading ? "Loading..." : "All caught up");
  layer_add_child(root, text_layer_get_layer(s_status));

  // All-caught-up chrome: accent check above the text, hint below it. Both
  // are hidden by status_update() unless the stream is empty and loaded.
  s_status_check = layer_create(GRect(s_win_w / 2 - 12, s_win_h / 2 - 46, 24, 18));
  layer_set_update_proc(s_status_check, status_check_update);
  layer_add_child(root, s_status_check);

  s_status_hint = text_layer_create(GRect(0, s_win_h / 2 + 12, s_win_w, 16));
  text_layer_set_font(s_status_hint, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_status_hint, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_status_hint, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_status_hint, GColorClear);
  text_layer_set_text_color(s_status_hint, theme_muted());
  layer_add_child(root, text_layer_get_layer(s_status_hint));

  // Page area (below the sidebar in z-order) + the accent sidebar on top.
  s_page_area = layer_create(GRect(0, TOP_BAR_H, s_win_w, s_view_h));
  layer_add_child(root, s_page_area);

  s_sidebar = layer_create(GRect(s_win_w - SIDEBAR_W, TOP_BAR_H, SIDEBAR_W, s_view_h));
  layer_set_update_proc(s_sidebar, sidebar_update);
  layer_add_child(root, s_sidebar);

  window_set_click_config_provider(window, timeline_click_config_provider);

  status_update();
}

//! Teardown on window unload: flush any pending mark-read batch, stop the
//! in-flight animations (so no stopped handler touches destroyed layers)
//! and release the layer pointers.
static void timeline_close(void) {
  proto_flush_now();
  if (s_anim_a) {
    Animation *old = s_anim_a;
    s_anim_a = NULL;
    animation_unschedule(old);
  }
  if (s_anim_b) {
    Animation *old = s_anim_b;
    s_anim_b = NULL;
    animation_unschedule(old);
  }
  page_destroy(&s_pages[0]);
  page_destroy(&s_pages[1]);
  s_root = NULL;
  s_page_area = NULL;
  s_top_bar = NULL;
  s_top_text = NULL;
  s_sidebar = NULL;
}

static void timeline_window_unload(Window *window) {
  timeline_close();
  if (s_star_path) {
    gpath_destroy(s_star_path);
    s_star_path = NULL;
  }
  if (s_status_check_path) {
    gpath_destroy(s_status_check_path);
    s_status_check_path = NULL;
  }
  if (s_status) {
    text_layer_destroy(s_status);
    s_status = NULL;
  }
  if (s_status_check) {
    layer_destroy(s_status_check);
    s_status_check = NULL;
  }
  if (s_status_hint) {
    text_layer_destroy(s_status_hint);
    s_status_hint = NULL;
  }
  if (s_top_text) {
    text_layer_destroy(s_top_text);
    s_top_text = NULL;
  }
  if (s_top_bar) {
    layer_destroy(s_top_bar);
    s_top_bar = NULL;
  }
  if (s_sidebar) {
    layer_destroy(s_sidebar);
    s_sidebar = NULL;
  }
  if (s_page_area) {
    layer_destroy(s_page_area);
    s_page_area = NULL;
  }
  window_destroy(s_tl_window);
  s_tl_window = NULL;
}

//! Open (or reset and re-open) the timeline for a stream. State is reset and
//! the first page is requested; a full-screen "Loading..." is shown until
//! the first article arrives.
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
  s_idx = 0;
  s_advancing = false;
  s_advance_guard = false;
  s_last_offset = GPointZero;

  s_tl_window = window_create();
  window_set_window_handlers(s_tl_window, (WindowHandlers){
    .load = timeline_window_load,
    .unload = timeline_window_unload,
  });
  window_stack_push(s_tl_window, true);

  proto_request_items(s_stream, "");
}

// ---------------------------------------------------------------------------
// Item-page collect hooks (called from proto_handle_inbox)
// ---------------------------------------------------------------------------

//! A new page is starting: make room in the ring buffer by dropping the
//! oldest entries from the front when the page would overflow, keeping the
//! current article under the reader.
void timeline_page_begin(int32_t n) {
  int32_t need = n < 0 ? 0 : n;
  if (s_count + need > MAX_ARTICLES) {
    int32_t drop = s_count + need - MAX_ARTICLES;
    if (drop >= s_count) {
      s_count = 0;
      s_idx = 0;
    } else {
      memmove(s_articles, &s_articles[drop],
              (size_t)(s_count - drop) * sizeof(Article));
      s_count -= drop;
      s_idx -= drop;
      if (s_idx < 0) {
        s_idx = 0;
      }
    }
  }
}

//! Append one article (heading + summary) from the current message
//! (bounds-checked; overflow drops the oldest from the front). The first
//! article of the stream brings the reader up.
void timeline_collect_article(DictionaryIterator *iter) {
  bool first = (s_count == 0);
  if (s_count >= MAX_ARTICLES) {
    memmove(s_articles, &s_articles[1], (size_t)(s_count - 1) * sizeof(Article));
    s_count--;
    if (s_idx > 0) {
      s_idx--;
    }
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

  if (first && s_tl_window) {
    // First article of the stream: build page 0, hide the status and mark
    // it per the list toggle.
    s_cur = 0;
    page_build(&s_pages[0], 0);
    status_update();
    if (s_sidebar) {
      layer_mark_dirty(s_sidebar);
    }
    if (s_top_bar) {
      layer_mark_dirty(s_top_bar); // progress line appears with the first article
    }
    if (s_mark_list) {
      article_mark_read(0);
    }
    timeline_prefetch_check();
  }
}

//! The page ended: store the continuation ("", = no more items), release the
//! loading flag and show the all-caught-up state on an empty stream.
void timeline_page_end(const char *cont) {
  snprintf(s_cont, sizeof(s_cont), "%s", cont ? cont : "");
  s_loaded_all = (s_cont[0] == '\0');
  s_loading = false;

  if (!s_tl_window) {
    return;
  }
  status_update();
  if (s_top_bar) {
    layer_mark_dirty(s_top_bar); // progress line grows with the loaded count
  }
  if (s_count > 0) {
    timeline_prefetch_check();
  }
}

//! Prefetch the next page when the reader enters the last 6 articles.
static void timeline_prefetch_check(void) {
  if (s_loading || s_loaded_all || s_count == 0) {
    return;
  }
  if (s_idx >= s_count - 6) {
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
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar); // the read/unread dot flips
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
  if (s_status) {
    text_layer_set_text_color(s_status, theme_muted());
  }
  if (s_status_hint) {
    text_layer_set_text_color(s_status_hint, theme_muted());
  }
  if (s_status_check) {
    layer_mark_dirty(s_status_check); // accent check
  }
  if (s_top_text) {
    text_layer_set_text_color(s_top_text, s_accent);
  }
  if (s_top_bar) {
    layer_mark_dirty(s_top_bar); // accent progress line
  }
  for (int i = 0; i < 2; i++) {
    Page *p = &s_pages[i];
    if (p->body) {
      text_layer_set_background_color(p->body, theme_bg());
      text_layer_set_text_color(p->body, theme_fg());
    }
    if (p->header) {
      layer_mark_dirty(p->header);
    }
  }
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar);
  }
}
