#include <pebble.h>

#include "common.h"
#include "storage.h"
#include "tree.h"
#include "proto.h"
#include "timeline.h"

// ---------------------------------------------------------------------------
// Timeline reading view (see timeline.h). Article headings + summaries
// stream in from the phone into a ring buffer; pages are dropped from the
// head when the buffer fills and re-fetched via continuation. The reader is
// a paged full-screen view in the style of the native Timeline app: one
// article per page — an accent header bar (heading + feed·time, yellow star
// when starred), a scrollable summary body, a permanent accent spine with a
// gliding progress pin, and a two-phase slide transition between articles.
// ---------------------------------------------------------------------------

#define SPINE_W 4        // accent spine width
#define PIN_W 11         // pin notch: base..apex (apex sits on the spine)
#define PIN_H 10
#define TOP_BAR_H 24     // accent top bar showing the folder/feed name
#define HEADER_FLOOR 52  // minimum header height
#define HEADER_META_H 22 // feed·time line + padding below the heading
#define HEADING_MAX_H 42 // two GOTHIC_18 lines
#define STAR_PAD 26      // right-side room for the star icon in the heading box

static Article s_articles[MAX_ARTICLES];
static int32_t s_count;
static char s_stream[48]; // current stream id
static char s_cont[24];   // next continuation; "" = all loaded
static bool s_loading;
static bool s_loaded_all;
static char s_title[24]; // stream title (kept for the open() contract)

static int32_t s_idx;        // current article (ring-buffer index)
static bool s_advancing;     // true while the slide transition runs
static bool s_advance_guard; // at most one advance per article
static GPoint s_last_offset; // last scroll offset seen

static int16_t s_win_w;
static int16_t s_win_h;
static int16_t s_view_h; // container height (window minus the top bar)
static int16_t s_header_h; // dynamic per article

static Window *s_tl_window;
static Layer *s_top_bar;   // accent top bar with the stream name (static)
static TextLayer *s_top_text;
static Layer *s_container;   // holds every page surface; slides on advance
static Layer *s_header_bar;  // accent header (dynamic height)
static ScrollLayer *s_scroll; // summary body
static TextLayer *s_body;    // wrapped summary, recreated per article
static Layer *s_spine;       // 4 px accent bar, full window height
static Layer *s_pin;         // progress notch on the spine
static TextLayer *s_status;  // full-screen "Loading..." / "No articles"

// Shared draw path: a 5-point star (~16 px), drawn in the header when the
// current article is starred.
static const GPathInfo STAR_PATH_INFO = {
  .num_points = 10,
  .points = (GPoint[10]){
    { 0, -8 }, { 2, -2 }, { 8, -2 }, { 5, 1 }, { 6, 8 },
    { 0, 4 }, { -6, 8 }, { -5, 1 }, { -8, -2 }, { -2, -2 },
  },
};

static GPath *s_star_path;

//! The pin notch: right-pointing triangle (base left, apex right, at the
//! spine), repositioned with gpath_move_to per draw.
static const GPathInfo PIN_PATH_INFO = {
  .num_points = 3,
  .points = (GPoint[3]){ { 0, -4 }, { 8, 0 }, { 0, 4 } },
};

static GPath *s_pin_gpath;

// ---------------------------------------------------------------------------
// Advance transition: a two-phase slide of the container (header + body +
// spine + pin). Phase 1 slides the current page up and out (ease-in); on
// stopped, the next article is loaded and the container, parked below the
// window, slides back up (ease-out). Property animations of the container
// frame; the from/to frames live in statics for the animation's lifetime.
// ---------------------------------------------------------------------------

static Animation *s_slide_anim;
static GRect s_slide_from;
static GRect s_slide_to;
static int8_t s_slide_dir; // +1 = next (slide up), -1 = previous (slide down)

// Pin glide: an int-lerp of the pin's y within the container (launcher
// idiom), marking the pin dirty per frame — no float math.
static int16_t s_pin_cur; // current pin y
static int16_t s_pin_from;
static int16_t s_pin_to;
static Animation *s_pin_anim;

static void timeline_prefetch_check(void);
static void article_mark_read(int32_t idx);
static void maybe_advance(void);
static void maybe_regress(void);
static void slide_start(int8_t dir);
static void load_article_content(void);
static void pin_reposition(bool glide);
static void status_update(void);
static void slide_phase2_stopped(Animation *anim, bool finished, void *context);

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
// Accent surfaces
// ---------------------------------------------------------------------------

//! The permanent 4 px accent spine along the right edge (full window height).
static void spine_update(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

//! The accent top bar: solid accent fill (the stream name is a TextLayer).
static void top_bar_update(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

//! The pin: a small notch at the current article's progress, base to the
//! left, apex on the spine. Its color carries the read state: accent =
//! unread, dark gray = read.
static void pin_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (!s_pin_gpath) {
    return;
  }
  const Article *a = current_article();
  graphics_context_set_fill_color(ctx, (a && a->read) ? GColorDarkGray : s_accent);
  gpath_move_to(s_pin_gpath, GPoint(0, b.size.h / 2));
  gpath_draw_filled(ctx, s_pin_gpath);
}

//! Accent header bar: heading (bold, up to 2 lines, ellipsized) and the
//! feed·time line. The bar color carries the read state: accent + black
//! text for UNREAD, dark gray + white text for READ; a yellow star with a
//! black outline on the right when the article is starred.
static void header_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  const Article *a = current_article();
  if (!a) {
    graphics_context_set_fill_color(ctx, s_accent);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
    return;
  }

  GColor bar = a->read ? GColorDarkGray : s_accent;
  GColor bar_text = a->read ? GColorWhite : GColorBlack;
  graphics_context_set_fill_color(ctx, bar);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  graphics_context_set_text_color(ctx, bar_text);
  graphics_draw_text(ctx, a->title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(4, 2, b.size.w - 8 - STAR_PAD, HEADING_MAX_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  char meta[48];
  char t[16];
  format_reltime(t, sizeof(t), a->published);
  snprintf(meta, sizeof(meta), "%s · %s", a->feed, t);
  graphics_context_set_text_color(ctx, bar_text);
  graphics_draw_text(ctx, meta, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(4, b.size.h - 18, b.size.w - 8, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  if (a->star && s_star_path) {
    // Yellow star with a black outline so it reads on any bar color.
    gpath_move_to(s_star_path, GPoint(b.size.w - 18, b.size.h / 2));
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 2);
    gpath_draw_outline(ctx, s_star_path);
    graphics_context_set_fill_color(ctx, GColorYellow);
    gpath_draw_filled(ctx, s_star_path);
  }
}

//! Dynamic header height for an article: the heading (capped at two
//! GOTHIC_18 lines) plus room for the feed·time line, floored at 52 px.
static int16_t header_height_for(const Article *a) {
  GSize sz = graphics_text_layout_get_content_size(
      a->title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(4, 0, s_win_w - 8 - STAR_PAD, 1000),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  int32_t h = sz.h;
  if (h > HEADING_MAX_H) {
    h = HEADING_MAX_H;
  }
  h += HEADER_META_H;
  return (int16_t)(h < HEADER_FLOOR ? HEADER_FLOOR : h);
}

// ---------------------------------------------------------------------------
// Pin glide
// ---------------------------------------------------------------------------

static void pin_anim_update(Animation *anim, const AnimationProgress progress) {
  s_pin_cur = s_pin_from +
              (int16_t)(((int32_t)(s_pin_to - s_pin_from) * (int32_t)progress) /
                        (int32_t)ANIMATION_NORMALIZED_MAX);
  if (s_pin) {
    layer_set_frame(s_pin, GRect(s_win_w - PIN_W - 3, s_pin_cur, PIN_W, PIN_H));
    layer_mark_dirty(s_pin);
  }
}

static void pin_anim_stopped(Animation *anim, bool finished, void *context) {
  if (anim != s_pin_anim) {
    return; // superseded by a newer glide
  }
  s_pin_anim = NULL;
  s_pin_cur = s_pin_to;
  if (s_pin) {
    layer_set_frame(s_pin, GRect(s_win_w - PIN_W - 3, s_pin_cur, PIN_W, PIN_H));
    layer_mark_dirty(s_pin);
  }
}

static const AnimationImplementation PIN_ANIM_IMPL = {
  .update = pin_anim_update,
};

//! Reposition the pin for the current article's progress within the loaded
//! window; glide (220 ms ease-in-out) or snap. Called when the article
//! changes and when pages append.
static void pin_reposition(bool glide) {
  if (!s_pin || s_count == 0) {
    return;
  }
  int32_t den = s_count > 0 ? s_count : 1;
  int32_t body_h = s_view_h - s_header_h;
  int32_t y = s_header_h + (body_h - 14) * (s_idx + 1) / den;
  if (y < s_header_h) {
    y = s_header_h;
  }
  if (y > s_view_h - PIN_H) {
    y = s_view_h - PIN_H;
  }

  if (s_pin_anim) {
    Animation *old = s_pin_anim;
    s_pin_anim = NULL;
    animation_unschedule(old);
  }
  if (!glide) {
    s_pin_cur = (int16_t)y;
    layer_set_frame(s_pin, GRect(s_win_w - PIN_W - 3, s_pin_cur, PIN_W, PIN_H));
    layer_mark_dirty(s_pin);
    return;
  }
  s_pin_from = s_pin_cur;
  s_pin_to = (int16_t)y;
  if (s_pin_to == s_pin_from) {
    return;
  }
  s_pin_anim = animation_create();
  animation_set_duration(s_pin_anim, 220);
  animation_set_curve(s_pin_anim, AnimationCurveEaseInOut);
  animation_set_implementation(s_pin_anim, &PIN_ANIM_IMPL);
  animation_set_handlers(s_pin_anim, (AnimationHandlers){
    .stopped = pin_anim_stopped,
  }, NULL);
  animation_schedule(s_pin_anim);
}

// ---------------------------------------------------------------------------
// Article page
// ---------------------------------------------------------------------------

//! Rebuild header + body for the current article: dynamic header height, a
//! fresh wrapped summary TextLayer (destroy/create resets the scroll state)
//! and the measured content height on the scroll layer.
static void load_article_content(void) {
  const Article *a = current_article();
  if (!a) {
    return;
  }

  s_header_h = header_height_for(a);
  layer_set_frame(s_header_bar, GRect(0, 0, s_win_w, s_header_h));
  layer_mark_dirty(s_header_bar);

  scroll_layer_set_frame(s_scroll, GRect(0, s_header_h, s_win_w, s_view_h - s_header_h));

  if (s_body) {
    text_layer_destroy(s_body);
    s_body = NULL;
  }

  // Measured wrapped height of the summary in the same box the layer uses
  // (window width minus 12 px so the spine stays clear).
  GSize sz = graphics_text_layout_get_content_size(
      a->summary, fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(4, 2, s_win_w - 12, 1000), GTextOverflowModeWordWrap, GTextAlignmentLeft);
  int32_t body_h = sz.h + 6; // 2 px top pad + 4 px air at the bottom

  s_body = text_layer_create(GRect(4, 2, s_win_w - 12, sz.h));
  text_layer_set_font(s_body, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_body, GTextAlignmentLeft);
  text_layer_set_overflow_mode(s_body, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_body, theme_bg());
  text_layer_set_text_color(s_body, theme_fg());
  text_layer_set_text(s_body, a->summary);
  layer_add_child(scroll_layer_get_layer(s_scroll), text_layer_get_layer(s_body));

  scroll_layer_set_content_size(s_scroll, GSize(s_win_w, body_h));
  scroll_layer_set_content_offset(s_scroll, GPointZero, false);
  s_last_offset = GPointZero;
}

// ---------------------------------------------------------------------------
// Advance transition
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

//! Commit an advance: guard the article, then slide the page up and bring
//! the next article in from below. The last loaded article pulses and stays.
static void maybe_advance(void) {
  if (s_advancing || s_advance_guard) {
    return;
  }
  if (s_count == 0) {
    return;
  }
  if (s_idx + 1 >= s_count) {
    vibes_short_pulse(); // last article: stay
    return;
  }
  s_advance_guard = true;
  s_advancing = true;
  slide_start(1);
}

//! Go back to the previously read article (scroll up past the top); the
//! newest article has nothing above it.
static void maybe_regress(void) {
  if (s_advancing || s_advance_guard) {
    return;
  }
  if (s_count == 0) {
    return;
  }
  if (s_idx <= 0) {
    vibes_short_pulse(); // already at the newest: nothing above
    return;
  }
  s_advancing = true;
  slide_start(-1);
}

//! Phase 2 done: the new article is fully on screen; release the transition
//! lock and the per-article advance guard.
static void slide_phase2_stopped(Animation *anim, bool finished, void *context) {
  if (anim != s_slide_anim) {
    return;
  }
  s_slide_anim = NULL;
  s_advancing = false;
  s_advance_guard = false;
  layer_set_frame(s_container, GRect(0, TOP_BAR_H, s_win_w, s_view_h));
}

//! Phase 1 done: the old article has slid out. Load the next/previous
//! article's data into the header/body, park the container off-screen on
//! the opposite side and slide it in (phase 2).
static void slide_phase1_stopped(Animation *anim, bool finished, void *context) {
  if (anim != s_slide_anim) {
    return;
  }
  s_slide_anim = NULL;

  s_idx += s_slide_dir;
  load_article_content();
  if (s_slide_dir > 0) {
    // Advancing into an article marks it read per the detail toggle;
    // going back never marks anything.
    if (s_mark_detail) {
      article_mark_read(s_idx);
    }
  }
  pin_reposition(true); // glide to the new progress
  timeline_prefetch_check();

  layer_set_frame(s_container, GRect(0, TOP_BAR_H + s_slide_dir * s_view_h, s_win_w, s_view_h));

  s_slide_from = layer_get_frame(s_container);
  s_slide_to = GRect(0, TOP_BAR_H, s_win_w, s_view_h);
  s_slide_anim = (Animation *)property_animation_create_layer_frame(s_container, &s_slide_to, &s_slide_from);
  animation_set_duration(s_slide_anim, 260);
  animation_set_curve(s_slide_anim, AnimationCurveEaseOut);
  animation_set_handlers(s_slide_anim, (AnimationHandlers){
    .stopped = slide_phase2_stopped,
  }, NULL);
  animation_schedule(s_slide_anim);
}

//! Phase 1 of a transition: slide the current page out (ease-in, 220 ms)
//! in the given direction (+1 up/next, -1 down/previous).
static void slide_start(int8_t dir) {
  if (s_slide_anim) {
    Animation *old = s_slide_anim;
    s_slide_anim = NULL;
    animation_unschedule(old);
  }
  s_slide_dir = dir;
  s_slide_from = layer_get_frame(s_container);
  s_slide_to = GRect(0, TOP_BAR_H - dir * s_view_h, s_win_w, s_view_h);
  s_slide_anim = (Animation *)property_animation_create_layer_frame(s_container, &s_slide_to, &s_slide_from);
  animation_set_duration(s_slide_anim, 260);
  animation_set_curve(s_slide_anim, AnimationCurveEaseIn);
  animation_set_handlers(s_slide_anim, (AnimationHandlers){
    .stopped = slide_phase1_stopped,
  }, NULL);
  animation_schedule(s_slide_anim);
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
  if (scroll_layer_get_content_offset(s_scroll).y > 0) {
    scroll_layer_scroll_up_click_handler(rec, s_scroll);
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
  GRect frame = layer_get_frame(scroll_layer_get_layer(s_scroll));
  GSize content = scroll_layer_get_content_size(s_scroll);
  GPoint offset = scroll_layer_get_content_offset(s_scroll);
  if (content.h <= frame.size.h + 2 || offset.y >= content.h - frame.size.h - 2) {
    maybe_advance();
  } else {
    scroll_layer_scroll_down_click_handler(rec, s_scroll);
  }
}

//! SELECT advances to the next article (marks per the detail toggle when the
//! new article becomes current).
static void timeline_select_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0 || s_advancing) {
    return;
  }
  maybe_advance();
}

//! Long SELECT toggles the star of the current article (fire-and-forget).
static void timeline_star_long_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0 || s_advancing) {
    return;
  }
  Article *a = &s_articles[s_idx];
  a->star = !a->star;
  proto_star(a->id, a->star);
  vibes_short_pulse();
  if (s_header_bar) {
    layer_mark_dirty(s_header_bar);
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

//! Full-screen status while the stream loads or comes up empty; hidden as
//! soon as the first article arrives.
static void status_update(void) {
  if (!s_status) {
    return;
  }
  if (s_count > 0) {
    layer_set_hidden(text_layer_get_layer(s_status), true);
  } else {
    layer_set_hidden(text_layer_get_layer(s_status), false);
    text_layer_set_text(s_status, s_loading ? "Loading..." : "No articles");
  }
}

static void timeline_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_win_w = bounds.size.w;
  s_win_h = bounds.size.h;
  s_view_h = s_win_h - TOP_BAR_H;

  window_set_background_color(window, theme_bg());

  s_star_path = gpath_create(&STAR_PATH_INFO);
  s_pin_gpath = gpath_create(&PIN_PATH_INFO);
  s_header_h = HEADER_FLOOR;
  s_pin_cur = HEADER_FLOOR;
  s_last_offset = GPointZero;

  // Accent top bar: the folder/feed currently showing its articles. Stays
  // put while the page below slides.
  s_top_bar = layer_create(GRect(0, 0, s_win_w, TOP_BAR_H));
  layer_set_update_proc(s_top_bar, top_bar_update);
  layer_add_child(root, s_top_bar);

  s_top_text = text_layer_create(GRect(4, 1, s_win_w - 8, TOP_BAR_H - 2));
  text_layer_set_font(s_top_text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_top_text, GTextAlignmentLeft);
  text_layer_set_overflow_mode(s_top_text, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_top_text, GColorClear);
  text_layer_set_text_color(s_top_text, GColorBlack);
  text_layer_set_text(s_top_text, s_title);
  layer_add_child(root, text_layer_get_layer(s_top_text));

  // The sliding container holds every page surface; children are positioned
  // relative to it so the whole page moves together during a transition.
  s_container = layer_create(GRect(0, TOP_BAR_H, s_win_w, s_view_h));
  layer_add_child(root, s_container);

  s_header_bar = layer_create(GRect(0, 0, s_win_w, s_header_h));
  layer_set_update_proc(s_header_bar, header_update);
  layer_add_child(s_container, s_header_bar);

  s_scroll = scroll_layer_create(GRect(0, s_header_h, s_win_w, s_view_h - s_header_h));
  scroll_layer_set_callbacks(s_scroll, (ScrollLayerCallbacks){
    .content_offset_changed_handler = timeline_content_offset_changed,
  });
  scroll_layer_set_shadow_hidden(s_scroll, true);
  layer_add_child(s_container, scroll_layer_get_layer(s_scroll));

  s_spine = layer_create(GRect(s_win_w - SPINE_W - 2, 0, SPINE_W, s_view_h));
  layer_set_update_proc(s_spine, spine_update);
  layer_add_child(s_container, s_spine);

  s_pin = layer_create(GRect(s_win_w - PIN_W - 3, s_pin_cur, PIN_W, PIN_H));
  layer_set_update_proc(s_pin, pin_update);
  layer_add_child(s_container, s_pin);

  s_status = text_layer_create(bounds);
  text_layer_set_font(s_status, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_status, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_status, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_status, GColorClear);
  text_layer_set_text_color(s_status, theme_muted());
  text_layer_set_text(s_status, s_loading ? "Loading..." : "No articles");
  layer_add_child(root, text_layer_get_layer(s_status));

  window_set_click_config_provider(window, timeline_click_config_provider);

  // No articles yet: hide the reader and show the full-screen status.
  layer_set_hidden(s_container, true);
  status_update();
}

//! Teardown on window unload: flush any pending mark-read batch, stop the
//! in-flight animations (so no stopped handler touches destroyed layers)
//! and release the layer pointers.
static void timeline_close(void) {
  proto_flush_now();
  if (s_slide_anim) {
    Animation *old = s_slide_anim;
    s_slide_anim = NULL;
    animation_unschedule(old);
  }
  if (s_pin_anim) {
    Animation *old = s_pin_anim;
    s_pin_anim = NULL;
    animation_unschedule(old);
  }
  s_container = NULL;
  s_top_bar = NULL;
  s_top_text = NULL;
  s_header_bar = NULL;
  s_scroll = NULL;
  s_spine = NULL;
  s_pin = NULL;
}

static void timeline_window_unload(Window *window) {
  timeline_close();
  if (s_star_path) {
    gpath_destroy(s_star_path);
    s_star_path = NULL;
  }
  if (s_pin_gpath) {
    gpath_destroy(s_pin_gpath);
    s_pin_gpath = NULL;
  }
  if (s_body) {
    text_layer_destroy(s_body);
    s_body = NULL;
  }
  if (s_scroll) {
    scroll_layer_destroy(s_scroll);
    s_scroll = NULL;
  }
  if (s_status) {
    text_layer_destroy(s_status);
    s_status = NULL;
  }
  if (s_header_bar) {
    layer_destroy(s_header_bar);
    s_header_bar = NULL;
  }
  if (s_top_text) {
    text_layer_destroy(s_top_text);
    s_top_text = NULL;
  }
  if (s_top_bar) {
    layer_destroy(s_top_bar);
    s_top_bar = NULL;
  }
  if (s_spine) {
    layer_destroy(s_spine);
    s_spine = NULL;
  }
  if (s_pin) {
    layer_destroy(s_pin);
    s_pin = NULL;
  }
  if (s_container) {
    layer_destroy(s_container);
    s_container = NULL;
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

  if (first && s_tl_window && s_container) {
    // First article of the stream: bring the reader up (article 0 becomes
    // current) and mark it per the list toggle.
    layer_set_hidden(s_container, false);
    status_update();
    load_article_content();
    pin_reposition(false);
    if (s_mark_list) {
      article_mark_read(0);
    }
    timeline_prefetch_check();
  }
}

//! The page ended: store the continuation ("", = no more items), release the
//! loading flag, show "No articles" on an empty stream and re-span the pin.
void timeline_page_end(const char *cont) {
  snprintf(s_cont, sizeof(s_cont), "%s", cont ? cont : "");
  s_loaded_all = (s_cont[0] == '\0');
  s_loading = false;

  if (!s_tl_window) {
    return;
  }
  status_update();
  if (s_count > 0) {
    pin_reposition(true); // pages appended: glide the pin to the new span
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
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

//! Re-apply accent/theme to the timeline window (from settings). The header,
//! spine and pin stay accent; the body, scroll and status re-theme.
void timeline_apply_settings(void) {
  if (!s_tl_window) {
    return;
  }
  window_set_background_color(s_tl_window, theme_bg());
  if (s_status) {
    text_layer_set_text_color(s_status, theme_muted());
  }
  if (s_body) {
    text_layer_set_background_color(s_body, theme_bg());
    text_layer_set_text_color(s_body, theme_fg());
  }
  if (s_header_bar) {
    layer_mark_dirty(s_header_bar);
  }
  if (s_top_bar) {
    layer_mark_dirty(s_top_bar);
  }
  if (s_spine) {
    layer_mark_dirty(s_spine);
  }
  if (s_pin) {
    layer_mark_dirty(s_pin);
  }
}
