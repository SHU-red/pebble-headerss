#include <pebble.h>

#include "common.h"
#include "storage.h"
#include "tree.h"
#include "proto.h"
#include "timeline.h"

// ---------------------------------------------------------------------------
// Timeline reading view (see timeline.h). Article headings + summaries
// stream in from the phone into a ring buffer. The reader is a paged
// full-screen view: a 2 px accent progress line along the very top, a black
// top bar (stream name in accent) below it, one article per page — an accent
// header bar (full multi-line heading + feed·time), a scrollable summary
// body — and a thick accent SIDEBAR on the right holding the read/unread
// dot, the star and the highlight-word M badge. Page changes slide with a
// continuous two-page transition (the outgoing page leaves while the
// incoming one enters — no teleport cut).
// ---------------------------------------------------------------------------

#define PROGRESS_H 2      // accent progress line along the very top (y=0..2)
#define TOP_BAR_H 24      // black top bar with the stream name (starts y=2)
#define DIVIDER_H 2       // accent divider line under the top bar (y=26..28)
#define SIDEBAR_W 26      // thick accent sidebar holding the icons
#define HEADER_META_H 18  // feed·time line + padding below the heading

// Sidebar icons: monochrome (inactive = black, active = white — except the
// highlight M, which uses the shared alarm color), stacked at the top of the
// full-height sidebar ~10 px apart, starting ~8 px from the very top.
#define SIDEBAR_ICON_GAP 12 // vertical gap between the indicators
#define SIDEBAR_DISC_D 20   // read/unread disc diameter
#define SIDEBAR_STAR_H 30   // fav star height
#define SIDEBAR_MAG_H 24    // match magnifier glyph height
// The indicator column is vertically centered beside the physical SELECT
// button (right edge, mid-screen): total stack = 30+12+20+12+24 = 98 px.
#define SIDEBAR_ICON_TOP(s_win_h) (((s_win_h) - 98) / 2)

// Highlight alarm color: the highlight M badge and ALL matched words (body
// and title) share this one color so the connection is obvious.
#define HL_ALARM_COLOR GColorRed

// Full-summary heap buffer: one buffer for the whole reader, never
// per-article. malloc'd when the first chunk of a fetch arrives, freed on
// article change / stream close / new fetch start.
// The 64 KB-class app heap is tiny (app bank 65536 − ~56 KB static image =
// ~9.5 KB, ~2.3 KB free with the reader open): a 4095-byte buffer plus the
// heap-grown run table cannot fit, so malloc() failed silently and the full
// summary never assembled (the watchdog kept the preview). Cap the assembly
// at 2048 bytes there — the common case (up to ~120 runs, no highlight
// blow-up) fits with the run table. emery/gabbro (128 KB banks) keep 4095.
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define FULL_SUMMARY_CAP 4095   // max assembled bytes (cap)
#else
#define FULL_SUMMARY_CAP 2048
#endif
#define FULL_SUMMARY_BUF (FULL_SUMMARY_CAP + 1) // + NUL
#define FULL_HINT_H 16          // "Loading full text..." line height

static Article s_articles[MAX_ARTICLES];
static int32_t s_count;
static int32_t s_page_announced; // page size announced by the phone; pins the
                                 // progress denominator until s_count passes it
static char s_stream[48]; // current stream id
static char s_cont[24];   // next continuation; "" = all loaded
static bool s_loading;
static bool s_loaded_all;
static char s_title[24]; // stream title (top bar)

static int32_t s_idx;        // current article (ring-buffer index)
static bool s_advancing;     // true while a page transition runs
static bool s_advance_guard; // at most one advance per article

static int16_t s_win_w;
static int16_t s_win_h;
static int16_t s_view_h; // page height (window minus progress + top bar)

static Window *s_tl_window;
static Layer *s_root;
static Layer *s_prog_line;  // 2 px accent progress line at the very top
static Layer *s_page_area; // holds the pages; added BEFORE the sidebar so the
                           // accent icon bar always stays on top
static Layer *s_top_bar;   // black bar, accent text (static)
static Layer *s_divider;   // thin accent line under the top bar
static TextLayer *s_top_text;
static Layer *s_sidebar;   // accent bar with the eye/star/M icons
static Layer *s_end_bar;   // grey "LONG" hint at the bottom of a long article
static AppTimer *s_clock_timer; // 1-minute tick: redraws the sidebar clock
static TextLayer *s_status;   // full-screen "Loading..." / "All caught up"
static Layer *s_status_check; // accent GPath check above the status text
static TextLayer *s_status_hint; // small hint under the status text

//! Full-summary fetch state: one heap buffer for the whole reader (malloc'd
//! on the first chunk, freed on article change / unload / new fetch start).
static char *s_full_summary; // assembled full text; NULL = none
static size_t s_full_len;    // bytes currently stored (<= FULL_SUMMARY_CAP)
static int32_t s_full_idx;   // article index the fetch/buffer belongs to
static bool s_full_done;     // the final chunk arrived (text complete)
static bool s_full_fetching; // a full-summary fetch is in flight
static AppTimer *s_full_watchdog; // stalls must never lock DOWN forever

// Auto-mark timer: marks the settled article read after mark_mode()'s delay
// (MARK_NOW marks immediately; MARK_NEVER arms nothing). Cancelled on
// advance / regress / manual read-toggle / window unload.
static AppTimer *s_mark_timer;
static int32_t s_mark_timer_idx; // article the pending timer is for

// ---------------------------------------------------------------------------
// Highlight layout engine — types (the engine itself lives below, before the
// page surfaces). The reader draws the summary body and the article heading
// from cached run tables instead of TextLayers, so words from the Clay
// highlight list render in the shared alarm color + bold + underline (body
// and heading alike — the M badge uses the same color). The layout is
// computed ONCE per article (and again on words-change); scroll frames only
// replay the cached runs — no measurement on draw.
// ---------------------------------------------------------------------------

#define HL_SPANS_MAX 32   // matched spans per text
#define HL_RUNS_MAX 24    // runs per layout (12 B each); tail folds when full
#define HL_MEASURE_W 4000 // one-line measurement box (never wraps)
#define HL_TOKEN_MAX 140  // longest wrap-token; longer tokens hard-break
                          // (keeps the measure scratch at 141 B)

//! One cached run: a contiguous styled slice of the source text.
typedef struct {
  int16_t x, y;  // run box top-left; y = line top (relative to text origin)
  int16_t w;     // run box width (px)
  uint16_t off;  // byte offset of the slice in the source text
  uint16_t len;  // byte length of the slice; 0 = literal ellipsis "…"
  uint8_t style; // 0 = base font, 1 = highlight font
} HlRun;

//! Cached layout: a wrapped, run-annotated text. The run table is a small
//! static array (covers previews and headings); when a FULL summary needs
//! more runs the layout grows a heap array on demand (freed on re-layout /
//! teardown), so long texts are never silently truncated.
typedef struct {
  int16_t height; // total layout height (px), a multiple of line_h
  int16_t line_h; // single text line height (px)
  uint16_t n;     // runs in use (a full summary can need hundreds)
  uint16_t cap;   // current run capacity (runs[])
  bool dyn;       // runs[] is heap-allocated
  HlRun *runs;    // points at static_runs or a heap array
  HlRun static_runs[HL_RUNS_MAX];
} HlLayout;

//! A matched span of the source text (byte range).
typedef struct {
  uint16_t off;
  uint16_t len;
} HlSpan;

//! Parameters for one layout pass.
typedef struct {
  const char *text; // source text (summary or title)
  GFont base_font;  // style-0 font
  GFont hl_font;    // style-1 font
  int16_t width;    // wrap width (px)
  int16_t max_lines; // 0 = unlimited; >0 = cap with a trailing ellipsis
  HlLayout *out;    // destination layout
} HlBuildParams;

//! One styled piece of a wrap-token (a token splits at span boundaries, so
//! "Nuclear-Fusion" with the pattern "nuclear" yields an hl "Nuclear" piece
//! and a base "-Fusion" piece).
typedef struct {
  size_t off;   // byte offset in the source text
  size_t len;   // byte length
  uint8_t style; // 0 = base font, 1 = highlight font
} HlSeg;

#define HL_SEG_MAX 16 // pieces per wrap-token (pathological tail folds)

// One article page: everything that slides during a transition. Two pages
// exist so the transition can animate them against each other.
typedef struct {
  Layer *root;        // container layer (slides during transitions)
  Layer *content;     // scroll wrapper: header+body, moved by FRAME
  int16_t content_h;  // content wrapper height (hh + 2 + body_h)
  Layer *header;      // accent header (dynamic height)
  Layer *body;        // custom highlight body layer, recreated per page
  HlLayout body_layout; // cached summary runs (per article / words-change)
  HlLayout head_layout; // cached heading runs (full title, multi-line)
  int32_t idx;        // ring index this page shows
  bool hl_match;      // any highlight-word match in title or summary
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
static AppTimer *s_transition_watchdog; // failsafe: releases a wedged transition

// Shared draw path: a chunky star (~30 px) — the favourite indicator in the
// sidebar. Bright chrome-yellow when starred (pops on the accent bar),
// black otherwise.
static const GPathInfo STAR_ICON_INFO = {
  .num_points = 10,
  .points = (GPoint[10]){
    { 0, -12 }, { 4, -4 }, { 12, -4 }, { 6, 2 }, { 10, 12 },
    { 0, 7 }, { -10, 12 }, { -6, 2 }, { -12, -4 }, { -4, -4 },
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
static void article_mark_read(int32_t idx);
static void mark_timer_start(int32_t idx);
static void mark_timer_cancel(void);
static void full_summary_request(int32_t idx);
static void full_summary_reset(void);
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
// Static chrome: progress line + top bar + sidebar
// ---------------------------------------------------------------------------

//! 2 px accent progress line along the very top of the screen (y = 0..2),
//! above the black top bar. Tracks the current article position within the
//! loaded stream; gated by the progress setting. The denominator is the
//! larger of the announced page size (s_page_announced, set by
//! timeline_page_begin) and the actual loaded count, so with s_count == 1 on
//! entry the bar starts near 0 (1/50 of the page) instead of jumping to 100%.
static void progress_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (setting_progress() && s_count > 0) {
    int32_t denom = s_count > s_page_announced ? s_count : s_page_announced;
    if (denom > 0) {
      // Cap the bar at the sidebar's left edge: at the last article (100%)
      // it reaches exactly the accent icon area, never hidden under it.
      int16_t max_w = (int16_t)(b.size.w - SIDEBAR_W);
      int16_t w = (int16_t)((int32_t)(s_idx + 1) * max_w / denom);
      if (w < 0) {
        w = 0;
      }
      if (w > max_w) {
        w = max_w;
      }
      if (w > 0) {
        graphics_context_set_fill_color(ctx, s_accent);
        graphics_fill_rect(ctx, GRect(0, 0, w, b.size.h), 0, GCornerNone);
      }
    }
  }
}

//! Black top bar with the stream name in accent (starts right below the
//! progress line).
static void top_bar_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
}

//! Thin white divider between the top bar and the scrollable page (matches
//! the menu group dividers).
static void divider_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
}

//! Once a minute: redraw the sidebar so the clock chip shows the new time.
static void clock_tick_cb(void *data) {
  s_clock_timer = NULL;
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar);
  }
  s_clock_timer = app_timer_register(60000, clock_tick_cb, NULL);
}

//! Accent sidebar (full screen height, y = 0..s_win_h) holding three
//! indicators VERTICALLY CENTERED beside the SELECT button, in the order
//! star, circle, magnifier: favourite star (orange = starred, black = not),
//! read/unread disc (filled white = unread, black = read) and the
//! highlight-match magnifier (the alarm color when the current article
//! matches a highlight word, plain black otherwise).
static void sidebar_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // 2-row clock in the sidebar's otherwise-wasted top: hours over minutes,
  // accent digits on a black rounded chip.
  int16_t ccx = b.size.w / 2;
  time_t now = time(NULL);
  struct tm *lt = localtime(&now);
  if (lt) {
    char tbuf[4];
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, GRect(ccx - 11, 6, 22, 42), 4, GCornersAll);
    graphics_context_set_text_color(ctx, s_accent);
    snprintf(tbuf, sizeof(tbuf), "%d", lt->tm_hour);
    graphics_draw_text(ctx, tbuf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(ccx - 11, 7, 22, 20),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                       NULL);
    snprintf(tbuf, sizeof(tbuf), "%02d", lt->tm_min);
    graphics_draw_text(ctx, tbuf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(ccx - 11, 27, 22, 20),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                       NULL);
  }

  const Article *a = current_article();
  if (!a) {
    return;
  }
  int16_t cx = b.size.w / 2;
  int16_t y = SIDEBAR_ICON_TOP(s_win_h);

  // 1. Favourite: a star — bright chrome-yellow when starred, black otherwise.
  if (s_star_path) {
    GPoint sc = GPoint(cx, y + SIDEBAR_STAR_H / 2);
    gpath_move_to(s_star_path, sc);
    graphics_context_set_fill_color(ctx,
                                    a->star ? GColorChromeYellow : GColorBlack);
    gpath_draw_filled(ctx, s_star_path);
  }
  y += SIDEBAR_STAR_H + SIDEBAR_ICON_GAP;

  // 2. Read/unread: a plain filled disc — white = unread, black = read.
  GPoint c = GPoint(cx, y + SIDEBAR_DISC_D / 2);
  graphics_context_set_fill_color(ctx, a->read ? GColorBlack : GColorWhite);
  graphics_fill_circle(ctx, c, SIDEBAR_DISC_D / 2);
  y += SIDEBAR_DISC_D + SIDEBAR_ICON_GAP;

  // 3. Match: a magnifying glass — alarm color when the current article has
  // highlight-word matches, black otherwise.
  bool matched = false;
  for (int i = 0; i < 2; i++) {
    const Page *pg = &s_pages[i];
    if (pg->idx == s_idx) {
      matched = pg->hl_match;
      break;
    }
  }
  GColor mag_c = matched ? HL_ALARM_COLOR : GColorBlack;
  graphics_context_set_stroke_color(ctx, mag_c);
  graphics_context_set_stroke_width(ctx, 2);
  GPoint center = GPoint(cx, y + SIDEBAR_MAG_H / 2 - 3);
  graphics_draw_circle(ctx, center, 8);
  graphics_draw_line(ctx, GPoint(center.x + 6, center.y + 6),
                     GPoint(center.x + 11, center.y + 11));
}

// ---------------------------------------------------------------------------
// Highlight layout engine (word highlighting)
// ---------------------------------------------------------------------------
//
// Matching rule: a pattern P matches at byte offset i iff P equals
// T[i..i+len) ASCII-case-insensitively ([A-Z] -> [a-z]) and both neighbours
// are boundaries (word chars are [A-Za-z0-9]; any other byte — hyphen,
// apostrophe, dot, space, non-ASCII — is a boundary). First match wins,
// overlapping matches are skipped, at most HL_SPANS_MAX spans per text.
//
// Runs: per line, adjacent same-style words (and the gaps between them) merge
// into one run; style 0 = base font, 1 = highlight font. A run with len == 0
// draws a literal ellipsis glyph ("…") — the heading's trailing-ellipsis
// truncation. A word wider than the line is placed anyway and clipped by the
// layer bounds (body), or ellipsized (heading).

//! ASCII fold: [A-Z] -> [a-z], everything else compared as raw bytes.
static char hl_fold_char(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

//! Word chars are [A-Za-z0-9]; any other byte (incl. non-ASCII) is a boundary.
static bool hl_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

//! Does the pattern match T at byte offset i (with boundary checks)?
static bool hl_match_at(const char *t, size_t tlen, const char *pat,
                        size_t plen, size_t i) {
  if (i + plen > tlen) {
    return false;
  }
  for (size_t k = 0; k < plen; k++) {
    if (hl_fold_char(t[i + k]) != hl_fold_char(pat[k])) {
      return false;
    }
  }
  bool before = (i == 0) || !hl_word_char(t[i - 1]);
  bool after = (i + plen == tlen) || !hl_word_char(t[i + plen]);
  return before && after;
}

//! Collect up to `cap` spans: every highlight word, first match wins,
//! overlapping spans skipped (a later word never re-claims a matched range).
static int hl_collect_spans(const char *text, HlSpan *spans, int cap) {
  int n = 0;
  size_t tlen = strlen(text);
  for (int wi = 0; wi < highlight_word_count() && n < cap; wi++) {
    const char *pat = highlight_word(wi);
    size_t plen = strlen(pat);
    if (plen == 0) {
      continue;
    }
    for (size_t i = 0; i + plen <= tlen && n < cap;) {
      if (hl_match_at(text, tlen, pat, plen, i)) {
        bool overlap = false;
        for (int k = 0; k < n; k++) {
          size_t a0 = spans[k].off, a1 = (size_t)spans[k].off + spans[k].len;
          if (i < a1 && a0 < i + plen) {
            overlap = true;
            break;
          }
        }
        if (!overlap) {
          spans[n].off = (uint16_t)i;
          spans[n].len = (uint16_t)plen;
          n++;
        }
        i += plen; // skip overlapping matches of the same word
      } else {
        i++;
      }
    }
  }
  return n;
}

//! Is byte position `pos` inside a matched span?
static bool hl_span_contains(const HlSpan *spans, int n, size_t pos) {
  for (int k = 0; k < n; k++) {
    if ((size_t)spans[k].off <= pos &&
        pos < (size_t)spans[k].off + spans[k].len) {
      return true;
    }
  }
  return false;
}

//! Split a wrap-token [woff, wend) into styled segments (contiguous pieces
//! sharing a style). Matched spans start/end at boundaries, so they never cut
//! a word-char run in half and the piece boundaries always sit between
//! word-char runs / separators. Beyond `cap` pieces the tail folds into the
//! previous piece (pathological, still consistent for drawing).
static int hl_token_segments(const HlSpan *spans, int n, size_t woff,
                             size_t wend, HlSeg *segs, int cap) {
  int m = 0;
  size_t pos = woff;
  while (pos < wend) {
    bool hl = hl_span_contains(spans, n, pos);
    size_t end = wend;
    if (hl) {
      // The piece ends at the covering span's end.
      for (int k = 0; k < n; k++) {
        if ((size_t)spans[k].off <= pos &&
            pos < (size_t)spans[k].off + spans[k].len) {
          size_t e = (size_t)spans[k].off + spans[k].len;
          if (e < end) {
            end = e;
          }
        }
      }
    } else {
      // The piece ends where the next span starts.
      for (int k = 0; k < n; k++) {
        size_t s = (size_t)spans[k].off;
        if (s > pos && s < end) {
          end = s;
        }
      }
    }
    if (m >= cap) {
      segs[m - 1].len = wend - segs[m - 1].off; // fold the tail in
      return m;
    }
    segs[m].off = pos;
    segs[m].len = end - pos;
    segs[m].style = hl ? 1 : 0;
    m++;
    pos = end;
  }
  return m;
}

//! Pixel width of a NUL-terminated string in the given font (one line).
static int16_t hl_measure_text(const char *text, GFont font) {
  GSize sz = graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, HL_MEASURE_W, 200),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  return (int16_t)sz.w;
}

//! Pixel width of the byte slice T[off..off+len) (scratch >= len + 1).
static int16_t hl_measure_slice(const char *t, size_t off, size_t len,
                                GFont font, char *scratch) {
  if (len == 0) {
    return 0;
  }
  memcpy(scratch, t + off, len);
  scratch[len] = '\0';
  return hl_measure_text(scratch, font);
}

//! Longest UTF-8-safe prefix of T[off..off+len) that fits in `allowed` px.
//! Returns the byte length (0 when even the first character does not fit —
//! the ellipsis caller then drops the run instead); *out_w receives the
//! measured width of that prefix.
static size_t hl_prefix_fit(const char *t, size_t off, size_t len,
                            int16_t allowed, GFont font, char *scratch,
                            int16_t *out_w) {
  size_t best = 0;
  int16_t best_w = 0;
  size_t k = 0;
  while (k < len) {
    k++;
    while (k < len && ((unsigned char)t[off + k] & 0xC0) == 0x80) {
      k++; // step over continuation bytes (never split a UTF-8 char)
    }
    int16_t w = hl_measure_slice(t, off, k, font, scratch);
    if (w > allowed) {
      break;
    }
    best = k;
    best_w = w;
  }
  *out_w = best_w;
  return best;
}

//! Append a trailing-ellipsis run to the last line of the layout, shrinking
//! or dropping the line's trailing runs until the ellipsis fits the width.
static void hl_add_ellipsis(HlLayout *lo, const char *text, GFont base_font,
                            GFont hl_font, int16_t width, int16_t line_y,
                            char *scratch) {
  int last = lo->n - 1;
  if (last < 0 || lo->runs[last].y != line_y) {
    // The last line is empty: a lone ellipsis marks the cut.
    if (lo->n < lo->cap) {
      HlRun *r = &lo->runs[lo->n];
      r->x = 0;
      r->y = line_y;
      r->w = hl_measure_text("\xE2\x80\xA6", base_font); // UTF-8 "…"
      r->off = 0;
      r->len = 0;
      r->style = 0;
      lo->n++;
    }
    return;
  }
  int first = last;
  for (int k = last; k >= 0 && lo->runs[k].y == line_y; k--) {
    first = k;
  }
  // True right edge of the line (runs tile; inter-style gaps are between the
  // run boxes, so the extent is the last run's x + w, not the width sum).
  int16_t ell_x = (int16_t)(lo->runs[last].x + lo->runs[last].w);
  int k = last;
  while (k >= first) {
    // Measure the ellipsis in the style of the run it will follow; the final
    // run is re-measured after the loop, once the style is certain.
    int16_t ew = hl_measure_text("\xE2\x80\xA6",
                                 lo->runs[k].style ? hl_font : base_font);
    if (ell_x + ew <= width) {
      break; // the ellipsis fits after run k
    }
    int16_t need = (int16_t)(ell_x + ew - width); // px to free
    HlRun *r = &lo->runs[k];
    if (r->w > need) {
      int16_t kept_w = 0;
      size_t keep = hl_prefix_fit(text, r->off, r->len,
                                  (int16_t)(r->w - need),
                                  r->style ? hl_font : base_font, scratch,
                                  &kept_w);
      if (keep > 0) {
        r->len = (uint16_t)keep;
        r->w = kept_w;
        ell_x = (int16_t)(r->x + r->w);
        break;
      }
      // Even one character is too wide: drop the run below.
    }
    // Shrinking cannot free enough: drop run k (frees its width plus the gap
    // before it — the ellipsis will sit right after the previous run).
    lo->n = k;
    ell_x = (k > first) ? (int16_t)(lo->runs[k - 1].x + lo->runs[k - 1].w)
                        : 0;
    k--;
  }
  if (lo->n < lo->cap) {
    uint8_t estyle = (lo->n > 0) ? lo->runs[lo->n - 1].style : 0;
    int16_t ew = hl_measure_text("\xE2\x80\xA6",
                                 estyle ? hl_font : base_font);
    HlRun *r = &lo->runs[lo->n];
    r->x = ell_x;
    r->y = line_y;
    r->w = ew;
    r->off = 0;
    r->len = 0;
    r->style = estyle;
    lo->n++;
  }
}

//! Layout pass: word-wrap the text into a cached run table. Hard newlines
//! break lines; whitespace collapses to single-space gaps; with max_lines >
//! 0 overflowing text is cut with a trailing ellipsis.
static void hl_build_layout(const HlBuildParams *p) {
  HlLayout *lo = p->out;
  const char *t = p->text;
  size_t tlen = strlen(t);
  if (lo->dyn) {
    free(lo->runs);
  }
  lo->runs = lo->static_runs;
  lo->dyn = false;
  lo->cap = HL_RUNS_MAX;
  lo->n = 0;
  lo->height = 0;

  GSize lh = graphics_text_layout_get_content_size(
      "Ag", p->base_font, GRect(0, 0, HL_MEASURE_W, 200),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  lo->line_h = (int16_t)lh.h;

  int16_t base_space = hl_measure_text(" ", p->base_font);
  int16_t hl_space = hl_measure_text(" ", p->hl_font);
  // Static scratch: the engine is single-threaded and re-layouts happen one
  // at a time. Keeping these off the stack matters — the app stack is only
  // 2 KB on basalt-class watches and this frame was 584 B.
  static char scratch[HL_TOKEN_MAX + 1]; // longest slice: one hard-broken token
  static HlSpan spans[HL_SPANS_MAX];

  int nspans = hl_collect_spans(t, spans, HL_SPANS_MAX);

  int16_t line_x = 0;     // width consumed on the current line
  int16_t line_y = 0;     // top of the current line
  bool have_word = false; // current line holds at least one word
  bool truncated = false; // max_lines cut the text short
  int cur = -1;           // open run index, -1 = none
  int16_t cur_w = 0;      // width accumulated into the open run

  size_t i = 0;
  while (i < tlen) {
    // Collapse horizontal whitespace; count gap chars before the next word.
    size_t gap_chars = 0;
    while (i < tlen && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r')) {
      i++;
      gap_chars++;
    }
    // Newlines are hard line breaks.
    while (i < tlen && t[i] == '\n') {
      if (have_word) {
        if (cur >= 0) {
          lo->runs[cur].w = cur_w;
          cur = -1;
          cur_w = 0;
        }
        have_word = false;
      }
      line_x = 0;
      line_y += lo->line_h;
      i++;
      while (i < tlen && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r')) {
        i++; // leading whitespace of the next line is dropped
      }
      gap_chars = 0;
    }
    if (i >= tlen) {
      break;
    }
    if (p->max_lines > 0 && line_y >= (int16_t)(p->max_lines * lo->line_h)) {
      truncated = true; // a word would land beyond the cap
      break;
    }
    size_t woff = i;
    while (i < tlen && t[i] != ' ' && t[i] != '\t' && t[i] != '\r' &&
           t[i] != '\n') {
      i++;
    }
    size_t wend = i;
    if (wend - woff > HL_TOKEN_MAX) {
      // Pathological tokens (a full summary can carry a huge unbroken
      // string): hard-break so the measure scratch (HL_TOKEN_MAX + NUL)
      // never overflows. The remainder re-tokens on the next pass.
      wend = woff + HL_TOKEN_MAX;
      i = wend;
    }

    // Wrap-token [woff, wend): split into styled segments (a token never
    // splits across lines — wrap on the sum of the segment widths).
    HlSeg segs[HL_SEG_MAX];
    int msegs = hl_token_segments(spans, nspans, woff, wend, segs,
                                  HL_SEG_MAX);
    int16_t tw = 0;
    for (int s = 0; s < msegs; s++) {
      tw += hl_measure_slice(t, segs[s].off, segs[s].len,
                             segs[s].style ? p->hl_font : p->base_font,
                             scratch);
    }
    int16_t gap_w = 0;
    if (have_word) {
      gap_w = (int16_t)(gap_chars * (segs[0].style ? hl_space : base_space));
    }
    if (have_word && line_x + gap_w + tw > p->width) {
      if (p->max_lines > 0 &&
          line_y + lo->line_h >= (int16_t)(p->max_lines * lo->line_h)) {
        truncated = true; // wrapping would exceed the cap
        break;
      }
      if (cur >= 0) {
        lo->runs[cur].w = cur_w;
        cur = -1;
        cur_w = 0;
      }
      have_word = false;
      line_x = 0;
      line_y += lo->line_h;
      gap_w = 0;
    }
    // Emit the segments: extend the open run when the style matches, else
    // open a new run (or fold into nothing when the table is full).
    for (int s = 0; s < msegs; s++) {
      size_t soff = segs[s].off;
      size_t slen = segs[s].len;
      uint8_t sstyle = segs[s].style;
      GFont sf = sstyle ? p->hl_font : p->base_font;
      int16_t sw = hl_measure_slice(t, soff, slen, sf, scratch);
      int16_t gap = (s == 0 && have_word) ? gap_w : 0;
      if (cur >= 0 && sstyle == lo->runs[cur].style) {
        lo->runs[cur].len = (uint16_t)(soff + slen - lo->runs[cur].off);
        line_x += gap + sw;
        cur_w += gap + sw;
      } else {
        if (cur >= 0) {
          lo->runs[cur].w = cur_w;
        }
        if (lo->n >= lo->cap) {
          // The static table is full (a full summary needs many more runs
          // than a preview): grow a heap array on demand. Doubling keeps
          // the reallocation count low; the memory is freed on re-layout.
          uint16_t new_cap = (uint16_t)(lo->cap * 2);
          if (new_cap < 64) {
            new_cap = 64;
          }
          HlRun *arr = malloc((size_t)new_cap * sizeof(HlRun));
          if (arr) {
            memcpy(arr, lo->runs, (size_t)lo->n * sizeof(HlRun));
            if (lo->dyn) {
              free(lo->runs);
            }
            lo->runs = arr;
            lo->dyn = true;
            lo->cap = new_cap;
          } else {
            cur = -1;
            cur_w = 0;
            line_x += gap + sw;
            have_word = true;
            continue; // heap exhausted: keep the static truncation
          }
        }
        {
          cur = lo->n;
          HlRun *r = &lo->runs[cur];
          r->x = line_x + gap;
          r->y = line_y;
          r->off = (uint16_t)soff;
          r->len = (uint16_t)slen;
          r->w = 0;
          r->style = sstyle;
          lo->n++;
          cur_w = sw;
        }
        line_x += gap + sw;
        have_word = true;
      }
    }
  }
  if (cur >= 0) {
    lo->runs[cur].w = cur_w; // close the final run
  }
  if (tlen > 0 && t[tlen - 1] == '\n') {
    line_y += lo->line_h; // trailing newline: one empty line
  }
  if (have_word) {
    line_y += lo->line_h; // the current line's own height
  }
  if (p->max_lines > 0 && line_y > (int16_t)(p->max_lines * lo->line_h)) {
    line_y = (int16_t)(p->max_lines * lo->line_h); // cap empty overhang
  }
  lo->height = line_y;

  if (p->max_lines > 0 && (truncated || (have_word && line_x > p->width))) {
    int16_t ell_y = line_y;
    if (ell_y >= (int16_t)(p->max_lines * lo->line_h)) {
      ell_y = (int16_t)((p->max_lines - 1) * lo->line_h);
    }
    hl_add_ellipsis(lo, t, p->base_font, p->hl_font, p->width, ell_y,
                    scratch);
  }
}

//! Replay a cached layout. ox/oy offset the text origin (the layer position);
//! y_limit > 0 skips runs whose line ENDS at or below that absolute y (used
//! by the header so a clamped long title never draws under the feed·time
//! line).
static void hl_draw(GContext *ctx, const char *text, const HlLayout *lo,
                    int16_t ox, int16_t oy, GFont base_font, GFont hl_font,
                    GColor base_c, GColor hl_c, int16_t y_limit) {
  char scratch[HL_TOKEN_MAX + 1];
  for (int k = 0; k < lo->n; k++) {
    const HlRun *r = &lo->runs[k];
    if (y_limit > 0 && oy + r->y + lo->line_h > y_limit) {
      continue; // this line would collide with the feed·time line: skip it
    }
    if (r->style) {
      // Text-marker highlight: fill the line box behind the run in the
      // alarm color, then draw the words BOLD in the base text color.
      graphics_context_set_fill_color(ctx, hl_c);
      graphics_fill_rect(ctx, GRect(ox + r->x, oy + r->y + 1, r->w,
                                    lo->line_h - 2),
                         0, GCornerNone);
    }
    GFont f = r->style ? hl_font : base_font;
    graphics_context_set_text_color(ctx, base_c);
    // A very wide one-line box: glyphs start at the box origin and are never
    // wrapped; the layer bounds supply the real clip.
    GRect box = GRect(ox + r->x, oy + r->y, HL_MEASURE_W, lo->line_h);
    if (r->len == 0) {
      graphics_draw_text(ctx, "\xE2\x80\xA6", f, box,
                         GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    } else {
      memcpy(scratch, text + r->off, r->len);
      scratch[r->len] = '\0';
      graphics_draw_text(ctx, scratch, f, box,
                         GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    }
  }
}

//! Which page a body layer belongs to (there are only two pages).
static int body_page_idx(Layer *body) {
  for (int i = 0; i < 2; i++) {
    if (s_pages[i].body == body) {
      return i;
    }
  }
  return -1;
}

//! Summary body: paints the page background and replays the cached runs
//! (base words in theme_fg, highlighted words in the alarm color + bold +
//! underline). When the assembled full summary of the page's article is
//! complete, the runs (rebuilt from the heap buffer) draw from it instead of
//! the 140-char preview; while a fetch is in flight a subtle muted hint
//! trails the text.
static void body_update(Layer *layer, GContext *ctx) {
  int pi = body_page_idx(layer);
  if (pi < 0) {
    return;
  }
  const Page *p = &s_pages[pi];
  const Article *a =
      (p->idx >= 0 && p->idx < s_count) ? &s_articles[p->idx] : NULL;
  if (!a) {
    return;
  }
  graphics_context_set_fill_color(ctx, theme_bg());
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  const char *text = a->summary;
  if (s_full_done && s_full_summary && p->idx == s_full_idx) {
    text = s_full_summary;
  }
  hl_draw(ctx, text, &p->body_layout, 0, 0,
          fonts_get_system_font(FONT_KEY_GOTHIC_18),
          fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
          theme_fg(), HL_ALARM_COLOR, 0);
  if (p->idx == s_full_idx && s_full_fetching && !s_full_done) {
    graphics_context_set_text_color(ctx, theme_muted());
    graphics_draw_text(ctx, "Loading full text...",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(0, p->body_layout.height + 4,
                             layer_get_bounds(layer).size.w, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft,
                       NULL);
  }
}

//! Size the page's scrollable unit: the accent header (full wrapped heading
//! + feed·time line) with the summary body stacked below it; the content
//! wrapper covers both so they scroll together. Adds room for the
//! "Loading full text..." hint when a fetch is in flight. The wrapper is
//! moved by FRAME (page_set_offset) — never by a ScrollLayer's bounds
//! origin, which advances the offset state but does not render on emery.
static void page_resize(Page *p) {
  int16_t hh = (int16_t)(p->head_layout.height + HEADER_META_H);
  int16_t body_h = p->body_layout.height + 4; // 2 px pad + 2 px air
  if (p->idx == s_full_idx && s_full_fetching && !s_full_done) {
    body_h += FULL_HINT_H;
  }
  layer_set_frame(p->header, GRect(0, 0, s_win_w, hh));
  layer_set_frame(p->body, GRect(4, hh + 1, s_win_w - SIDEBAR_W - 8, body_h));
  int32_t total = hh + 2 + body_h;
  int16_t old_h = p->content_h;
  if (old_h != total) {
    p->content_h = (int16_t)total;
    APP_LOG(APP_LOG_LEVEL_INFO, "layout: page idx=%ld content %d -> %d (head %d body %d)",
            (long)p->idx, old_h, (int)total, hh,
            (int)p->body_layout.height);
  }
  // Preserve the current offset; re-clamp when the content grew/shrunk so a
  // resize can never strand the offset past the new bottom.
  int32_t off = layer_get_frame(p->content).origin.y;
  int32_t min_y = s_view_h - total;
  if (off < min_y) {
    off = min_y;
  }
  if (off > 0) {
    off = 0;
  }
  layer_set_frame(p->content, GRect(0, off, s_win_w, (int16_t)total));
  layer_mark_dirty(p->header);
  layer_mark_dirty(p->body);
  if (s_end_bar) {
    layer_mark_dirty(s_end_bar); // the LONG hint follows the content size
  }
}

//! Manual scroll: move the page's content wrapper by frame — the only
//! movement mechanism proven to render on the user's emery (settles move
//! page roots with layer_set_frame; the ScrollLayer's bounds-origin path
//! never redraws there). Single clamp authority for the scroll offset.
static void page_set_offset(Page *p, int32_t y) {
  int32_t min_y = s_view_h - p->content_h;
  if (y < min_y) {
    y = min_y;
  }
  if (y > 0) {
    y = 0;
  }
  layer_set_frame(p->content, GRect(0, y, s_win_w, p->content_h));
  layer_mark_dirty(p->content);
  if (s_end_bar) {
    layer_mark_dirty(s_end_bar); // the LONG hint follows the bottom state
  }
}

#define END_BAR_H 16

//! Grey "LONG" hint pinned to the bottom of the view while the reader sits
//! at the very end of a long article: a tap there is held back (the
//! advance needs an explicit HOLD), so the hint tells the user why.
//! Transparent (no fill) at every other position/article.
static void end_bar_update(Layer *layer, GContext *ctx) {
  if (s_count == 0 || !cur_page() || !cur_page()->content) {
    return;
  }
  const Page *p = cur_page();
  int32_t off = layer_get_frame(p->content).origin.y;
  bool long_article = (p->content_h > s_view_h + 8); // a real scroll area
  bool at_bottom = (off <= s_view_h - p->content_h + 2);
  if (!long_article || !at_bottom) {
    return;
  }
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, theme_muted());
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, theme_bg());
  graphics_draw_text(ctx, "LONG",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(0, 0, b.size.w, b.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     NULL);
}

// ---------------------------------------------------------------------------
// Full summary (fetch + assembly + render)
// ---------------------------------------------------------------------------

//! Did the cached layout contain any highlight-style run?
static bool layout_has_hl(const HlLayout *lo) {
  for (int k = 0; k < lo->n; k++) {
    if (lo->runs[k].style) {
      return true;
    }
  }
  return false;
}

//! Re-layout one page's body from the given text and resize to match.
static void body_relayout(Page *p, const char *text) {
  GFont gothic18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont gothic18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  hl_build_layout(&(HlBuildParams){
    .text = text, .base_font = gothic18, .hl_font = gothic18b,
    .width = (int16_t)(s_win_w - SIDEBAR_W - 8), .max_lines = 0,
    .out = &p->body_layout,
  });
  p->hl_match = layout_has_hl(&p->head_layout) ||
                layout_has_hl(&p->body_layout);
  page_resize(p);
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar); // the M badge follows the new matches
  }
}

//! Drop the full-summary buffer + fetch state (article change, unload, new
//! fetch start). The body keeps whatever layout it currently has.
static void full_summary_reset(void) {
  if (s_full_summary) {
    free(s_full_summary);
    s_full_summary = NULL;
  }
  s_full_len = 0;
  s_full_idx = -1;
  s_full_done = false;
  s_full_fetching = false;
  if (s_full_watchdog) {
    app_timer_cancel(s_full_watchdog);
    s_full_watchdog = NULL;
  }
}

//! A full-summary fetch that never completes (dropped chunk chain) must not
//! keep the DOWN button blocked on the short preview: drop the fetch state,
//! the preview stays and DOWN advances again.
static void full_summary_watchdog_cb(void *data) {
  s_full_watchdog = NULL;
  APP_LOG(APP_LOG_LEVEL_INFO, "fetch: WATCHDOG fired (idx %ld len %u)",
          (long)s_full_idx, (unsigned)s_full_len);
  full_summary_reset();
  if (s_count > 0) {
    Page *p = cur_page();
    if (p && p->body) {
      page_resize(p); // the hint line disappears with the fetch state
      layer_mark_dirty(p->body);
    }
  }
}

//! The current page's fetch state changed in a way that affects its body
//! geometry (hint on/off): re-size in place.
static void full_summary_hint_update(void) {
  if (s_count == 0 || !cur_page()->body) {
    return;
  }
  page_resize(cur_page());
}

//! An article settled under the reader: ask the phone for its full summary.
//! The 140-char preview stays in the body until the chunks assemble; the
//! fetch is skipped when the preview already is the full summary.
static void full_summary_request(int32_t idx) {
  full_summary_reset(); // new fetch start: free the previous buffer
  if (idx < 0 || idx >= s_count) {
    return;
  }
  const Article *a = &s_articles[idx];
  if (strlen(a->summary) < (size_t)(sizeof(a->summary) - 1)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "summary: idx %ld preview is full, no fetch",
            (long)idx);
    return; // the preview wasn't truncated: it already is the full summary
  }
  s_full_idx = idx;
  s_full_fetching = true;
  proto_request_summary(a->id);
  full_summary_hint_update();
  s_full_watchdog = app_timer_register(8000, full_summary_watchdog_cb, NULL);
  APP_LOG(APP_LOG_LEVEL_INFO, "summary: fetch idx %ld id %s", (long)idx,
          a->id);
}

//! The assembled full text replaced the preview: re-layout the current page
//! from the heap buffer and grow the scroll content.
static void full_summary_apply(void) {
  if (!s_full_done || !s_full_summary) {
    APP_LOG(APP_LOG_LEVEL_INFO, "apply: skipped (done=%d buf=%d)",
            (int)s_full_done, s_full_summary != NULL);
    return;
  }
  if (s_full_idx != s_idx || s_count == 0) {
    APP_LOG(APP_LOG_LEVEL_INFO, "apply: skipped (idx %ld vs s_idx %ld)",
            (long)s_full_idx, (long)s_idx);
    return; // the reader moved on; the buffer is freed at the next settle
  }
  Page *p = cur_page();
  if (!p->body || p->idx != s_idx) {
    APP_LOG(APP_LOG_LEVEL_INFO, "apply: skipped (page idx %ld mid-transition)",
            (long)(p ? p->idx : -1));
    return; // mid-transition: the settled page isn't current yet
  }
  body_relayout(p, s_full_summary);
  APP_LOG(APP_LOG_LEVEL_INFO, "apply: full text %u bytes -> layout h=%d runs=%u",
          (unsigned)s_full_len, p->body_layout.height, p->body_layout.n);
}

//! The full-summary text under the current page was thrown away (stale-race
//! self-heal): put the 140-char preview layout back.
static void full_summary_revert_to_preview(void) {
  if (s_count == 0 || s_idx < 0 || s_idx >= s_count) {
    return;
  }
  Page *p = cur_page();
  if (!p->body || p->idx != s_idx) {
    return;
  }
  body_relayout(p, s_articles[s_idx].summary);
}

//! One FullSummary chunk arrived (called from the AppMessage inbox path —
//! shallow stack only: no snprintf/strtoll/strstr; the chunk text lives in
//! the inbox buffer and is copied here). Assembles into the one heap buffer;
//! on the final chunk the full text replaces the preview in the body.
//! `id` is the article id the phone put on the chunk ("" when absent):
//! chunks are attributed by id so a stale chunk of the PREVIOUS article
//! still in the BLE pipe when the reader advanced cannot pollute the new
//! article's buffer (the positional s_full_idx==s_idx guard alone passes
//! once the new fetch reset s_full_idx to the new index).
void timeline_full_summary_chunk(const char *text, bool last, const char *id) {
  if (!text) {
    return;
  }
  size_t tlen = strlen(text);

  // Chunks of a fetch the reader has left: ignore. A non-empty stream
  // arriving AFTER a completed buffer is the previous fetch's tail racing
  // past an article change: restart the assembly so the real stream wins,
  // then fall through to assemble this chunk.
  if (s_full_idx != s_idx) {
    return;
  }
  if (id && id[0] && (s_idx < 0 || s_idx >= s_count ||
                      strcmp(id, s_articles[s_idx].id) != 0)) {
    return; // a chunk for a different article (stale pipe): drop it
  }
  if (s_full_done) {
    if (tlen == 0) {
      return; // trailing finalize must not clobber a completed buffer
    }
    full_summary_reset();
    s_full_idx = s_idx;
    s_full_fetching = true;
    full_summary_revert_to_preview();
  }

  if (!s_full_summary) {
    s_full_summary = malloc(FULL_SUMMARY_BUF);
    if (!s_full_summary) {
      return; // heap exhausted: keep the preview
    }
    s_full_summary[0] = '\0';
    s_full_len = 0;
  }
  if (s_full_len < FULL_SUMMARY_CAP) {
    size_t room = FULL_SUMMARY_CAP - s_full_len;
    size_t take = tlen < room ? tlen : room;
    memcpy(s_full_summary + s_full_len, text, take);
    s_full_len += take;
    s_full_summary[s_full_len] = '\0';
  }

  if (last) {
    s_full_done = true;
    s_full_fetching = false;
    if (s_full_watchdog) {
      app_timer_cancel(s_full_watchdog);
      s_full_watchdog = NULL;
    }
    if (!s_full_summary || s_full_len == 0) {
      // Empty/errored fetch (SummaryLast alone): keep the preview, close
      // the fetch and drop the hint.
      APP_LOG(APP_LOG_LEVEL_INFO, "summary: fetch empty/errored, keep preview");
      full_summary_reset();
      full_summary_hint_update();
      return;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "summary: complete, %u bytes", 
            (unsigned)s_full_len);
    full_summary_apply();
    return;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "summary: chunk %u bytes",
          (unsigned)tlen);
}

// ---------------------------------------------------------------------------
// Page surfaces
// ---------------------------------------------------------------------------

//! Which page a header layer belongs to (there are only two pages).
static int header_page_idx(Layer *header) {
  for (int i = 0; i < 2; i++) {
    if (s_pages[i].header == header) {
      return i;
    }
  }
  return -1;
}

//! Accent header bar: always accent background with black text — the read
//! state lives in the sidebar icons, not the colors. The heading is drawn
//! from the cached highlight layout: the whole title keeps its bold look,
//! highlighted words render in the alarm color + underlined (shared with the
//! sidebar M badge and the body's matched words).
static void header_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  int pi = header_page_idx(layer);
  if (pi < 0) {
    return;
  }
  const Page *p = &s_pages[pi];
  const Article *a =
      (p->idx >= 0 && p->idx < s_count) ? &s_articles[p->idx] : NULL;
  if (!a) {
    return;
  }
  GFont heading_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  // The feed·time line lives at the header's bottom; a clamped long title
  // must not draw under it (lines ending in the meta zone are skipped).
  char meta[48];
  char t[16];
  format_reltime(t, sizeof(t), a->published);
  snprintf(meta, sizeof(meta), "%s · %s", a->feed, t);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, meta, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(4, b.size.h - 18, b.size.w - SIDEBAR_W - 8, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  // The heading is never clamped (max_lines=0), so the y_limit only guards
  // the meta line below. The LAST line's bottom is 2 + head_layout.height =
  // b.size.h - 16; the old limit (b.size.h - 20) sat above it and dropped
  // the last line of every heading (a 1-line title never rendered at all).
  hl_draw(ctx, a->title, &p->head_layout, 4, 2, heading_font, heading_font,
          GColorBlack, HL_ALARM_COLOR, (int16_t)(b.size.h - 16));
}

//! Build one article page (header + scrollable summary) into a fresh set of
//! layers; the body text stays clear of the sidebar. Any layers the page
//! already holds are destroyed first, so a parked spare (e.g. from an
//! interrupted transition) is always safely overwritten with no dangling
//! state.
static void page_build(Page *p, int32_t idx) {
  page_destroy(p); // drop stale layers (no-op for a fresh/empty page)
  const Article *a = &s_articles[idx];
  p->idx = idx;

  // Cached highlight layouts: heading (the FULL title, multi-line, no
  // ellipsis cap; GOTHIC_18_BOLD throughout, highlighted words drawn white)
  // and summary body (unlimited lines, base GOTHIC_18 with GOTHIC_18_BOLD
  // for highlighted words).
  GFont gothic18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont gothic18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  int16_t text_w = (int16_t)(s_win_w - SIDEBAR_W - 8);
  hl_build_layout(&(HlBuildParams){
    .text = a->title,
    .base_font = gothic18b,
    .hl_font = gothic18b,
    .width = text_w,
    .max_lines = 0,
    .out = &p->head_layout,
  });
  hl_build_layout(&(HlBuildParams){
    .text = a->summary,
    .base_font = gothic18,
    .hl_font = gothic18b,
    .width = text_w,
    .max_lines = 0,
    .out = &p->body_layout,
  });
  p->hl_match = layout_has_hl(&p->head_layout) ||
                layout_has_hl(&p->body_layout);

  // The page area already sits below the progress line + top bar + divider;
  // the page root starts at its origin. The page is ONE scrollable unit: the
  // accent heading and the summary scroll together (heading first, body
  // below), so a long article's last word is reachable by scrolling and only
  // a further DOWN at the very end advances.
  p->root = layer_create(GRect(0, 0, s_win_w, s_view_h));
  layer_add_child(s_page_area, p->root);

  // Manual scroll wrapper (no ScrollLayer): a plain layer holding the
  // header + body, moved by FRAME via page_set_offset. The ScrollLayer was
  // abandoned because it moves its internal content sub-layer by BOUNDS
  // ORIGIN — the offset state advances but the screen never redraws on the
  // user's emery; layer_set_frame is the mechanism the settles prove works.
  // Clipping is the layer default (clips=true), identical to the old view.
  p->content = layer_create(GRect(0, 0, s_win_w, 1));
  layer_add_child(p->root, p->content);

  p->header = layer_create(GRect(0, 0, s_win_w, 1));
  layer_set_update_proc(p->header, header_update);
  layer_add_child(p->content, p->header);

  p->body = layer_create(GRect(4, 1, s_win_w - SIDEBAR_W - 8, 1));
  layer_set_update_proc(p->body, body_update);
  layer_add_child(p->content, p->body);
  p->content_h = 1;
  page_resize(p); // sizes header + body + the content wrapper (+ hint)
}

static void page_destroy(Page *p) {
  // Free heap-grown run tables (full summaries) before the layers go.
  if (p->head_layout.dyn) {
    free(p->head_layout.runs);
    p->head_layout.runs = p->head_layout.static_runs;
    p->head_layout.dyn = false;
  }
  if (p->body_layout.dyn) {
    free(p->body_layout.runs);
    p->body_layout.runs = p->body_layout.static_runs;
    p->body_layout.dyn = false;
  }
  if (p->content) {
    layer_destroy(p->content); // destroys the header + body children too
    p->content = NULL;
    p->header = NULL;
    p->body = NULL;
  }
  if (p->root) {
    layer_destroy(p->root);
    p->root = NULL;
  }
  p->idx = -1;
  p->hl_match = false;
}

// ---------------------------------------------------------------------------
// Page transition (continuous two-page slide)
// ---------------------------------------------------------------------------

//! Failsafe: if a page transition ever wedges (s_advancing stuck — e.g. an
//! animation the OS never completes or never reports), release the locks so
//! DOWN/UP/SELECT keep working. The watchdog is armed in transition_to and
//! cancelled when the transition settles; a healthy 260 ms slide never
//! reaches it.
static void transition_watchdog_cb(void *data) {
  s_transition_watchdog = NULL;
  if (!s_advancing) {
    return;
  }
  // A wedged transition left its animations live on the page roots; the
  // next transition destroys those layers (page_destroy). Unschedule both
  // animations first — NULL the pointers so their stopped handlers no-op
  // (they fire with finished=false and would otherwise compare against the
  // cleared s_anim_a).
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
  s_advancing = false;
  s_advance_guard = false;
  layer_set_frame(cur_page()->root, GRect(0, 0, s_win_w, s_view_h));
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar);
  }
}

static void transition_watchdog_cancel(void) {
  if (s_transition_watchdog) {
    app_timer_cancel(s_transition_watchdog);
    s_transition_watchdog = NULL;
  }
}

//! Transition finished: the target page is fully on screen. Commit the new
//! index, drain the triage stream (unstar the article we landed on when
//! advancing inside Starred), drop the old page, release the locks, then
//! arm the auto-mark timer and fetch the full summary for the settled
//! article.
static void transition_finalize(void) {
  transition_watchdog_cancel();
  s_idx = s_target_idx;
  APP_LOG(APP_LOG_LEVEL_INFO, "nav: settle idx=%ld dir=%d", (long)s_idx,
          (int)s_dir);
  page_destroy(&s_pages[s_cur]);
  s_cur = 1 - s_cur;
  s_advancing = false;
  s_advance_guard = false;
  if (s_dir > 0 && setting_triage() &&
      strcmp(s_stream, "user/-/state/com.google/starred") == 0) {
    if (s_idx >= 0 && s_idx < s_count) {
      Article *a = &s_articles[s_idx];
      a->star = 0;
      proto_star(a->id, 0);
      tree_starred_adjust(-1); // drained: the Starred badge follows
    }
  }
  mark_timer_start(s_idx);
  full_summary_request(s_idx);
  full_summary_apply(); // heal: apply a completed fetch if the chunk path missed it
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar);
  }
  if (s_top_bar) {
    layer_mark_dirty(s_top_bar);
  }
  if (s_prog_line) {
    layer_mark_dirty(s_prog_line); // progress line follows the new index
  }
  if (s_end_bar) {
    layer_mark_dirty(s_end_bar); // the LONG hint follows the new article
  }
  timeline_prefetch_check();
}

//! The current page's out-animation stopped. A finished stop finalizes the
//! swap. An interrupted stop (the OS unscheduled the animation mid-flight,
//! e.g. a notification peek) leaves s_advancing && s_advance_guard set —
//! which would lock the reader out of every further advance ("sometimes
//! can't go to the next article"). Release the locks without the page swap,
//! pull the current page back to its settled frame, and let the next
//! transition rebuild the parked spare (page_build destroys any stale
//! layers first).
static void transition_anim_stopped(Animation *anim, bool finished, void *context) {
  if (anim != s_anim_a) {
    return;
  }
  s_anim_a = NULL;
  s_anim_b = NULL;
  if (finished) {
    transition_finalize();
  } else {
    transition_watchdog_cancel();
    s_advancing = false;
    s_advance_guard = false;
    layer_set_frame(cur_page()->root, GRect(0, 0, s_win_w, s_view_h));
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
  mark_timer_cancel(); // leaving the current article: drop its auto-mark
  transition_watchdog_cancel();
  s_transition_watchdog = app_timer_register(2000, transition_watchdog_cb, NULL);

  int16_t top = 0; // settled y inside the page area (which itself sits below
                   // the progress line + top bar)
  Page *sp = spare_page();
  page_build(sp, nidx);
  layer_set_frame(sp->root, GRect(0, top + dir * s_view_h, s_win_w, s_view_h));

  Page *cp = cur_page();

  s_from_a = layer_get_frame(cp->root);
  s_to_a = GRect(0, top - dir * s_view_h, s_win_w, s_view_h);
  s_anim_a = (Animation *)property_animation_create_layer_frame(cp->root, &s_from_a, &s_to_a);
  animation_set_duration(s_anim_a, 260);
  animation_set_curve(s_anim_a, AnimationCurveEaseInOut);
  animation_set_handlers(s_anim_a, (AnimationHandlers){
    .stopped = transition_anim_stopped,
  }, NULL);
  animation_schedule(s_anim_a);

  s_from_b = layer_get_frame(sp->root);
  s_to_b = GRect(0, top, s_win_w, s_view_h);
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
  if (s_idx + 1 >= s_count) {
    APP_LOG(APP_LOG_LEVEL_INFO, "nav: advance blocked (last article %ld/%ld)",
            (long)s_idx, (long)s_count);
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

//! UP scrolls the article up by one viewport; at the top it goes back to
//! the previously read article (still in the ring from this session). The
//! SDK's offset is the content origin: negative when scrolled, 0 at the top.
static void timeline_up_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0 || s_advancing) {
    return;
  }
  Page *p = cur_page();
  int32_t off = layer_get_frame(p->content).origin.y;
  APP_LOG(APP_LOG_LEVEL_INFO, "nav: UP off=%ld", (long)off);
  if (off < 0) {
    int32_t target = off + (s_view_h - 24);
    if (target > 0) {
      target = 0;
    }
    page_set_offset(p, target);
  } else {
    maybe_regress();
  }
}

//! DOWN scrolls the article by ~3/4 viewport; at the true bottom of the
//! text it advances to the next article. No fetch-hold: pressing DOWN while
//! the full summary is still loading proceeds anyway (fast reading). Any
//! overflow — even a few pixels of a cut last line — scrolls first, so the
//! bottom of the summary is always revealed before the next article.
static void timeline_down_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0 || s_advancing || s_advance_guard) {
    APP_LOG(APP_LOG_LEVEL_INFO, "nav: DOWN ignored (count=%ld adv=%d guard=%d)",
            (long)s_count, (int)s_advancing, (int)s_advance_guard);
    return;
  }
  Page *p = cur_page();
  int32_t off = layer_get_frame(p->content).origin.y;
  int32_t content_h = p->content_h;
  // The offset is clamped to [s_view_h - content_h, 0] — the content
  // origin: 0 at the top, NEGATIVE when scrolled, s_view_h - content_h at
  // the very bottom. "At the bottom" is off <= s_view_h - content_h.
  bool at_bottom = (off <= s_view_h - content_h + 2);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "nav: DOWN off=%ld frame=%d cont=%ld bottom=%d",
          (long)off, s_view_h, (long)content_h, (int)at_bottom);
  if (at_bottom) {
    if (content_h > s_view_h + 8) {
      // End of a LONG article: one fast tap must not throw the reader past
      // it — the scroll state is lost and getting back means re-scrolling
      // from the heading. Advance only on an explicit HOLD (the grey LONG
      // bar at the bottom says so); the pulse confirms the press.
      APP_LOG(APP_LOG_LEVEL_INFO,
              "nav: long-article end — hold DOWN to advance");
      vibes_short_pulse();
      if (s_end_bar) {
        layer_mark_dirty(s_end_bar);
      }
      return;
    }
    maybe_advance();
    return;
  }
  // The chrome is tight now: near-fit articles (content up to ~8 px over the
  // viewport — a sub-perceptual sliver of the last line) advance on ONE
  // press instead of performing an invisible 8 px scroll. Genuinely long
  // text (a full line or more over) keeps the visible page-scroll.
  // EXCEPT while the full summary of this article is still loading: the
  // preview is short BECAUSE the fetch is in flight, not because the article
  // is short — advancing then would skip the article the user is reading.
  // Hold; the (repeating) press scrolls once the full text lands.
  if (content_h <= s_view_h + 8) {
    bool fetching_here = (s_full_idx == s_idx && s_full_fetching &&
                          !s_full_done);
    if (fetching_here) {
      APP_LOG(APP_LOG_LEVEL_INFO,
              "nav: fetch in flight, preview fits — hold");
      return;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "nav: fits (cont=%ld frame=%d), advance",
            (long)content_h, s_view_h);
    maybe_advance();
    return;
  }
  // Page-down: ~3/4 viewport per press (a small overlap keeps the previous
  // screen's last line visible for context); the final press lands exactly
  // on the bottom so the last word is clearly visible.
  int32_t step = (s_view_h * 3) / 4;
  int32_t target = off - step;
  int32_t min_y = s_view_h - content_h;
  if (target < min_y) {
    target = min_y;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "nav: page-down %ld -> %ld", (long)off, (long)target);
  page_set_offset(p, target);
}

//! SELECT toggles the current article's read state (unread -> mark read via
//! the batch path, read -> unread via proto_mark_unread) and cancels any
//! pending auto-mark timer. On an empty (all-caught-up) stream it re-fetches
//! the first page (the status hint points at this).
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
  Article *a = &s_articles[s_idx];
  if (a->read) {
    a->read = 0;
    proto_mark_unread(a->id);
  } else {
    article_mark_read(s_idx);
  }
  mark_timer_cancel(); // the manual toggle supersedes the auto-mark timer
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar); // the dot flips
  }
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
  tree_starred_adjust(a->star ? 1 : -1); // the root menu's Starred badge
  vibes_short_pulse();
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar);
  }
}

//! HOLD DOWN: jump to the next article without scrolling through the text
//! (a long article used to need ~19 page-downs). The tap still page-scrolls;
//! the advance guard (cleared at the settle) keeps one jump per hold.
static void timeline_down_hold_click(ClickRecognizerRef rec, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_INFO, "nav: hold DOWN -> next");
  maybe_advance();
}

//! HOLD UP: jump to the previously read article (no scrolling back).
static void timeline_up_hold_click(ClickRecognizerRef rec, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_INFO, "nav: hold UP -> previous");
  maybe_regress();
}

static void timeline_back_click(ClickRecognizerRef rec, void *ctx) {
  window_stack_pop(true);
}

//! Custom click config: UP/DOWN are subscribed here (never
//! scroll_layer_set_click_config_onto_window) so the reader controls the
//! advance semantics; scrolling delegates to the scroll layer's handlers.
//! Tap = page-scroll; HOLD (500 ms) = jump to the next/previous article,
//! so a long article needs no ~19 taps to move on.
static void timeline_click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, timeline_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, timeline_down_click);
  window_long_click_subscribe(BUTTON_ID_UP, 500, timeline_up_hold_click, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 500, timeline_down_hold_click, NULL);
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
  s_view_h = s_win_h - TOP_BAR_H - PROGRESS_H - DIVIDER_H;
  s_root = root;

  window_set_background_color(window, theme_bg());

  s_star_path = gpath_create(&STAR_ICON_INFO);
  s_status_check_path = gpath_create(&CHECK_PATH_INFO);
  s_cur = 0;
  s_pages[0].idx = -1;
  s_pages[1].idx = -1;

  // 2 px accent progress line along the very top (y = 0..2), above the bar.
  s_prog_line = layer_create(GRect(0, 0, s_win_w, PROGRESS_H));
  layer_set_update_proc(s_prog_line, progress_update);
  layer_add_child(root, s_prog_line);

  // Black top bar with the stream name in accent (y = 2..2 + TOP_BAR_H).
  s_top_bar = layer_create(GRect(0, PROGRESS_H, s_win_w, TOP_BAR_H));
  layer_set_update_proc(s_top_bar, top_bar_update);
  layer_add_child(root, s_top_bar);

  // Thin accent divider line between the top bar and the scrollable page.
  s_divider = layer_create(GRect(0, TOP_BAR_H + PROGRESS_H, s_win_w, DIVIDER_H));
  layer_set_update_proc(s_divider, divider_update);
  layer_add_child(root, s_divider);

  s_top_text = text_layer_create(GRect(4, PROGRESS_H + 1, s_win_w - SIDEBAR_W - 8, TOP_BAR_H - 2));
  text_layer_set_font(s_top_text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_top_text, GTextAlignmentLeft);
  text_layer_set_overflow_mode(s_top_text, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_top_text, GColorClear);
  text_layer_set_text_color(s_top_text, GColorWhite); // matches the divider
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
  // The sidebar spans the FULL screen height (y = 0..s_win_h): its accent
  // bar reaches the very top of the screen, drawn over the progress line,
  // the top bar and the header's right edge. The page area stays below the
  // top bar.
  s_page_area = layer_create(GRect(0, TOP_BAR_H + PROGRESS_H + DIVIDER_H,
                                   s_win_w, s_view_h));
  layer_add_child(root, s_page_area);

  // Grey "LONG" hint at the very bottom of the view: visible while the
  // reader sits at the end of a long article (tap is held back there).
  s_end_bar = layer_create(GRect(0, s_view_h - END_BAR_H, s_win_w, END_BAR_H));
  layer_set_update_proc(s_end_bar, end_bar_update);
  layer_add_child(s_page_area, s_end_bar);

  s_sidebar = layer_create(GRect(s_win_w - SIDEBAR_W, 0, SIDEBAR_W, s_win_h));
  layer_set_update_proc(s_sidebar, sidebar_update);
  layer_add_child(root, s_sidebar);
  s_clock_timer = app_timer_register(60000, clock_tick_cb, NULL);

  window_set_click_config_provider(window, timeline_click_config_provider);

  status_update();
}

//! Teardown on window unload: flush any pending mark-read batch, stop the
//! in-flight animations (so no stopped handler touches destroyed layers)
//! and release the layer pointers.
static void timeline_close(void) {
  mark_timer_cancel(); // the pending auto-mark must not fire on dead pages
  if (s_clock_timer) {
    app_timer_cancel(s_clock_timer);
    s_clock_timer = NULL;
  }
  full_summary_reset(); // free the assembled full text
  transition_watchdog_cancel();
  proto_flush_now();
  // Free heap-grown run tables (full-summary layouts) on teardown.
  for (int i = 0; i < 2; i++) {
    if (s_pages[i].head_layout.dyn) {
      free(s_pages[i].head_layout.runs);
      s_pages[i].head_layout.runs = s_pages[i].head_layout.static_runs;
      s_pages[i].head_layout.dyn = false;
    }
    if (s_pages[i].body_layout.dyn) {
      free(s_pages[i].body_layout.runs);
      s_pages[i].body_layout.runs = s_pages[i].body_layout.static_runs;
      s_pages[i].body_layout.dyn = false;
    }
  }
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
  s_prog_line = NULL;
  s_top_bar = NULL;
  s_divider = NULL;
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
  if (s_divider) {
    layer_destroy(s_divider);
    s_divider = NULL;
  }
  if (s_prog_line) {
    layer_destroy(s_prog_line);
    s_prog_line = NULL;
  }
  if (s_sidebar) {
    layer_destroy(s_sidebar);
    s_sidebar = NULL;
  }
  if (s_page_area) {
    layer_destroy(s_page_area); // destroys the end bar too
    s_page_area = NULL;
    s_end_bar = NULL;
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
  s_page_announced = 0; // the next page_begin pins the progress denominator
  s_cont[0] = '\0';
  s_loading = true;
  s_loaded_all = false;
  s_idx = 0;
  s_advancing = false;
  s_advance_guard = false;
  mark_timer_cancel(); // stale auto-mark must not fire into the new stream
  full_summary_reset(); // stale full text must not leak across streams

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
  s_page_announced = need; // pins the progress denominator until s_count passes it
  if (s_count + need > MAX_ARTICLES) {
    int32_t drop = s_count + need - MAX_ARTICLES;
    if (drop >= s_count) {
      s_count = 0;
      s_idx = 0;
      full_summary_reset(); // everything dropped: the buffered text is gone
      // The pages' articles were evicted too: mark them inert so they stop
      // referencing dropped indices (they draw nothing until rebuilt).
      for (int i = 0; i < 2; i++) {
        s_pages[i].idx = -1;
      }
    } else {
      memmove(s_articles, &s_articles[drop],
              (size_t)(s_count - drop) * sizeof(Article));
      s_count -= drop;
      s_idx -= drop;
      bool regressed_below_drop = (s_idx < 0);
      if (s_idx < 0) {
        s_idx = 0;
      }
      // The live pages follow the ring too (their article moved down by
      // `drop`; a page whose article was evicted goes inert).
      for (int i = 0; i < 2; i++) {
        if (s_pages[i].idx >= drop) {
          s_pages[i].idx -= drop;
        } else if (s_pages[i].idx >= 0) {
          s_pages[i].idx = -1;
        }
      }
      if (s_full_idx >= 0) {
        s_full_idx -= drop; // the buffered article shifted with the ring
        if (s_full_idx < 0) {
          full_summary_reset(); // the buffered article was dropped
        }
      }
      if (regressed_below_drop && s_count > 0 && cur_page()->body) {
        // The reader had gone back below the drop line when the prefetched
        // page arrived, so its article was evicted and the pages went inert
        // (blank screen). Rebuild the current page on the new head so the
        // reader shows a real article instead of nothing.
        page_build(cur_page(), s_idx);
      }
    }
  }
  if (s_prog_line) {
    layer_mark_dirty(s_prog_line); // the announced size pinned the denominator
  }
}

//! Append one article (heading + summary) from the current message
//! (bounds-checked; overflow drops the oldest from the front). The first
//! article of the stream brings the reader up.
void timeline_collect_article(DictionaryIterator *iter) {
  // Dedup by id: the phone may re-send an item after a lost ack (send retry)
  // or the server may repeat an id across a continuation boundary. A
  // duplicate must not take a ring slot or shift the reader's position.
  Tuple *id_t = dict_find(iter, MESSAGE_KEY_ItemId);
  const char *new_id = id_t ? id_t->value->cstring : "";
  if (new_id[0]) {
    for (int32_t i = 0; i < s_count; i++) {
      if (strcmp(s_articles[i].id, new_id) == 0) {
        return; // already collected: skip
      }
    }
  }
  bool first = (s_count == 0);
  if (s_count >= MAX_ARTICLES) {
    memmove(s_articles, &s_articles[1], (size_t)(s_count - 1) * sizeof(Article));
    s_count--;
    if (s_idx > 0) {
      s_idx--;
    }
    for (int i = 0; i < 2; i++) {
      if (s_pages[i].idx > 0) {
        s_pages[i].idx--;
      } else if (s_pages[i].idx == 0) {
        s_pages[i].idx = -1; // the page's article was evicted
      }
    }
    if (s_full_idx > 0) {
      s_full_idx--; // the buffered article shifted with the ring
    } else if (s_full_idx == 0) {
      full_summary_reset(); // the buffered article was dropped
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
    // First article of the stream: build page 0, hide the status, then
    // settle it like any article (auto-mark timer + full-summary fetch).
    s_cur = 0;
    page_build(&s_pages[0], 0);
    status_update();
    if (s_sidebar) {
      layer_mark_dirty(s_sidebar);
    }
    if (s_top_bar) {
      layer_mark_dirty(s_top_bar);
    }
    if (s_prog_line) {
      layer_mark_dirty(s_prog_line); // progress line appears with the article
    }
    mark_timer_start(0);
    full_summary_request(0);
    full_summary_apply(); // heal: apply a completed fetch if missed earlier
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
    layer_mark_dirty(s_top_bar);
  }
  if (s_prog_line) {
    layer_mark_dirty(s_prog_line); // progress line grows with the loaded count
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
// Auto-mark timer (mark_mode())
// ---------------------------------------------------------------------------

//! The delayed auto-mark fired: mark the article read only if the reader is
//! still on it and it is still unread (article_mark_read re-checks).
static void mark_timer_cb(void *data) {
  s_mark_timer = NULL;
  if (s_idx == s_mark_timer_idx) {
    article_mark_read(s_idx);
  }
}

//! Drop any pending auto-mark timer (advance, regress, manual read/unread
//! toggle, window unload).
static void mark_timer_cancel(void) {
  if (s_mark_timer) {
    app_timer_cancel(s_mark_timer);
    s_mark_timer = NULL;
  }
}

//! Arm the auto-mark for the article that just settled under the reader:
//! MARK_NEVER arms nothing, MARK_NOW marks immediately, the delayed modes
//! register an app timer whose callback re-checks the article.
static void mark_timer_start(int32_t idx) {
  mark_timer_cancel();
  int mode = mark_mode();
  if (mode <= MARK_NEVER || mode > MARK_10S) {
    return;
  }
  if (mode == MARK_NOW) {
    article_mark_read(idx);
    return;
  }
  static const int32_t mark_delay_ms[] = { 0, 0, 1000, 2000, 3000, 5000, 10000 };
  s_mark_timer_idx = idx;
  s_mark_timer = app_timer_register(mark_delay_ms[mode], mark_timer_cb, NULL);
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
    text_layer_set_text_color(s_top_text, GColorWhite); // matches the divider
  }
  if (s_top_bar) {
    layer_mark_dirty(s_top_bar);
  }
  if (s_prog_line) {
    layer_mark_dirty(s_prog_line); // accent progress line
  }
  for (int i = 0; i < 2; i++) {
    Page *p = &s_pages[i];
    if (p->body) {
      layer_mark_dirty(p->body); // colors are read at draw time
    }
    if (p->header) {
      layer_mark_dirty(p->header);
    }
  }
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar);
  }
}

// ---------------------------------------------------------------------------
// Highlight words live update
// ---------------------------------------------------------------------------

//! A new highlight-word list arrived from Clay (proto.c): re-layout the open
//! reader's pages (body + heading) in place, resize what the layout dictates
//! and mark everything dirty. No-op while no reader is open.
void timeline_highlight_words_changed(void) {
  if (!s_tl_window || s_count == 0) {
    return;
  }
  GFont gothic18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont gothic18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  int16_t text_w = (int16_t)(s_win_w - SIDEBAR_W - 8);
  for (int i = 0; i < 2; i++) {
    Page *p = &s_pages[i];
    if (p->idx < 0 || p->idx >= s_count) {
      continue;
    }
    const Article *a = &s_articles[p->idx];
    hl_build_layout(&(HlBuildParams){
      .text = a->title, .base_font = gothic18b, .hl_font = gothic18b,
      .width = text_w, .max_lines = 0, .out = &p->head_layout,
    });
    // The body text depends on the full-summary state: the heap buffer when
    // the article's full text is complete, else the 140-char preview.
    const char *body_text = a->summary;
    if (s_full_done && s_full_summary && p->idx == s_full_idx) {
      body_text = s_full_summary;
    }
    hl_build_layout(&(HlBuildParams){
      .text = body_text, .base_font = gothic18, .hl_font = gothic18b,
      .width = text_w, .max_lines = 0, .out = &p->body_layout,
    });
    p->hl_match = layout_has_hl(&p->head_layout) ||
                  layout_has_hl(&p->body_layout);

    page_resize(p); // header + body + scroll content follow the layout

    layer_mark_dirty(p->body);
    layer_mark_dirty(p->header);
  }
  if (s_sidebar) {
    layer_mark_dirty(s_sidebar); // the M badge reflects the new word list
  }
}
