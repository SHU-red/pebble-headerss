#include <pebble.h>

#include "common.h"
#include "storage.h"
#include "tree.h"
#include "proto.h"
#include "timeline.h"

// ---------------------------------------------------------------------------
// Timeline reading view (see timeline.h). Article headings + summaries
// stream in from the phone into a ring buffer. The reader is a paged
// full-screen view: a 2 px accent progress line along the very top, a theme
// top bar (stream name) below it, one article per page — an editorial
// header (GOTHIC_24_BOLD white heading on the page background, accent feed·time
// meta, a 2 px accent rule), a scrollable summary body — and a slim accent
// SIDEBAR on the right holding the clock chip, the read/unread dot, the
// star and the highlight-word M badge. Page changes slide with a
// continuous two-page transition (the outgoing page leaves while the
// incoming one enters — no teleport cut).
// ---------------------------------------------------------------------------

#define PROGRESS_H 3      // accent progress line along the very top (y=0..3)
#define TOP_BAR_H 24      // theme top bar with the stream name (starts y=2)
#define DIVIDER_H 1       // hairline divider under the top bar (P6)
#define SIDEBAR_W 20      // slim accent sidebar holding the icons (P14: 26 -> 20)
#define HEADER_META_H 18  // feed·time line + padding below the heading
#define END_BAR_H 16      // bottom hint band (reserved for long articles)

// Sidebar icons: monochrome (inactive = black, active = white — except the
// highlight M chip, which uses the shared alarm color), stacked in a
// vertically-centered column beside the physical SELECT button. The stack
// (22+12+16+12+18 = 78 px) starts below the plain clock digits (two 17 px
// rows from y=1 -> bottom 35) on every target: the P3 overlap on 144×168
// is gone (stack top there = (168-78)/2 = 45 > 35).
#define SIDEBAR_ICON_GAP 12 // vertical gap between the indicators
#define SIDEBAR_DISC_D 16   // read/unread disc diameter
#define SIDEBAR_STAR_H 22   // fav star height
#define SIDEBAR_MAG_H 18    // M badge chip height
#define SIDEBAR_ICON_TOP(s_win_h) (((s_win_h) - 78) / 2)
// Clock at the sidebar's very top: 2 rows of GOTHIC_14_BOLD digits, plain
// black text on the accent bar — no chip, no border (0.3.35).
#define SIDEBAR_CLOCK_W 18 // digit box width (2 GOTHIC_14 digits fit)
#define SIDEBAR_CLOCK_TOP 1 // first digit row starts at the very top

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
static int32_t s_total;    // stream total at open (feed/folder unread count);
                           // 0 = unknown (falls back to the loaded window)
static int32_t s_dropped;  // articles evicted from the ring front since open
                           // (the ring drops the oldest as pages stream in)
static char s_stream[48]; // current stream id
static char s_cont[24];   // next continuation; "" = all loaded
static bool s_loading;
static bool s_loaded_all;
static bool s_waiting_advance; // DOWN pressed at the buffer end: the pending
                               // advance fires when the next page's first
                               // article lands (seamless lazy loading)
static char s_title[24]; // stream title (top bar)

static int32_t s_idx;        // current article (ring-buffer index)
static bool s_advancing;     // true while a page transition runs
static bool s_advance_guard; // at most one advance per article
static bool s_scroll_mode;   // long article: entered by HOLD DOWN, tap then
                             // scrolls; reset on every article change

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

// Shared draw path: a chunky star (~18 px) — the favourite indicator in the
// sidebar. Bright chrome-yellow with a black outline when starred (the
// outline keeps it visible on yellow/green accents), black otherwise.
static const GPathInfo STAR_ICON_INFO = {
  .num_points = 10,
  .points = (GPoint[10]){
    { 0, -10 }, { 3, -3 }, { 9, -3 }, { 5, 2 }, { 8, 10 },
    { 0, 6 }, { -8, 10 }, { -5, 2 }, { -9, -3 }, { -3, -3 },
  },
};

static GPath *s_star_path;

// The all-caught-up check mark uses the shared UI_CHECK_PATH_INFO from
// common.h (the dialogs draw the same mark for success).
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

//! Gated by the progress setting. The denominator is the stream total at
//! open (the feed/folder unread count — the REAL size of what the user is
//! reading, so a 300-article folder never shows 100% at article 50); the
//! numerator is the GLOBAL position (articles evicted from the ring front
//! + the ring index), so the bar climbs monotonically instead of resetting
//! at every page boundary. Unknown totals (e.g. the Important stream) fall
//! back to the loaded window: the larger of the announced page size and
//! the actual loaded count.
static void progress_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (setting_progress() && s_count > 0) {
    int32_t denom;
    int32_t pos;
    if (s_total > 0) {
      denom = s_total;
      pos = s_dropped + s_idx + 1;
      if (pos > denom) {
        pos = denom;
      }
    } else {
      denom = s_count > s_page_announced ? s_count : s_page_announced;
      pos = s_idx + 1;
    }
    if (denom > 0) {
      // Cap at the sidebar's left edge: the last article reaches exactly
      // the accent icon area, never hidden under it.
      int16_t max_w = (int16_t)(b.size.w - SIDEBAR_W);
      int16_t w = (int16_t)(pos * max_w / denom);
      if (w < 0) {
        w = 0;
      }
      if (w > max_w) {
        w = max_w;
      }
      // The muted track (the unread remainder, untouched) — rounded caps
      // (radius 1) so the bar reads as a modern scrollbar.
      graphics_context_set_fill_color(ctx, theme_muted());
      graphics_fill_rect(ctx, GRect(0, 0, max_w, PROGRESS_H), 1, GCornersAll);
      // The read portion (left of the position) in the full accent.
      if (w > 0) {
        graphics_context_set_fill_color(ctx, s_accent);
        graphics_fill_rect(ctx, GRect(0, 0, w, PROGRESS_H), 1, GCornersAll);
      }
    }
  }
}
//! Top bar with the stream name (starts right below the progress line).
//! Theme-aware (P5): black crown in dark mode, white bar in light mode —
//! the hairline divider below separates it from the page either way.
static void top_bar_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, theme_bg());
  graphics_fill_rect(ctx, b, 0, GCornerNone);
}

//! Thin divider between the top bar and the scrollable page: white in dark
//! mode (a visible seam under the black bar over the black page), dark gray
//! in light mode — a white line would vanish against the white page.
static void divider_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, theme_dark() ? GColorWhite : GColorDarkGray);
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

//! 2 x 2 1-bit checkerboard for the sidebar's dithered seam (P12): the
//! strip where the accent bar meets the page gets a 2 px checkerboard. The
//! firmware's bitblt TILES the source vertically, so drawing this 2x2 in a
//! 2 x height rect covers the whole seam 1:1 (no scaling). One pattern
//! serves both themes: GCompOpAnd paints the UNSET (white) pixels black
//! (page in dark mode) and keeps the set pixels (accent); GCompOpOr paints
//! the SET (black) pixels white (page in light mode) and keeps the unset.
//! Pixel 0 of a row is the LSB (bitblt word order); row 0 has pixel 0 set.
static const uint8_t s_dither_pbi[] = {
  // row_size_bytes=4, info_flags = version 1 << 12, origin (0,0), size 2x2
  0x04, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
  // row 0: pixel0 set (black), pixel1 unset (white)
  0x01, 0x00, 0x00, 0x00,
  // row 1: pixel0 unset, pixel1 set
  0x02, 0x00, 0x00, 0x00,
};
static GBitmap *s_dither_bitmap;

//! Accent sidebar (full screen height, y = 0..s_win_h) holding three
//! indicators VERTICALLY CENTERED beside the SELECT button, in the order
//! star, circle, M badge: favourite star (chrome-yellow + black outline
//! when starred, black when not — P2), read/unread disc (filled white =
//! unread, nothing when read — the classic unread-dot idiom, P2) and the
//! highlight match (alarm-red rounded chip with a white "M" when the
//! current article matches a highlight word, red chip + white M otherwise
//! otherwise — P1).
static void sidebar_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // 2 px dithered seam at the inner (left) edge: the accent checkerboards
  // into the page instead of cutting a hard line (the 2x2 pattern tiles
  // vertically over the whole bar).
  if (s_dither_bitmap) {
    graphics_context_set_compositing_mode(ctx,
                                          theme_dark() ? GCompOpAnd
                                                       : GCompOpOr);
    graphics_draw_bitmap_in_rect(ctx, s_dither_bitmap,
                                 (GRect){ .origin = { 0, 0 },
                                          .size = { 2, b.size.h } });
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
  }

  // 2-row clock at the sidebar's very top: hours over minutes, plain black
  // digits on the accent bar — no chip, no border (0.3.35).
  int16_t ccx = b.size.w / 2;
  time_t now = time(NULL);
  struct tm *lt = localtime(&now);
  if (lt) {
    char tbuf[4];
    graphics_context_set_text_color(ctx, GColorBlack);
    snprintf(tbuf, sizeof(tbuf), "%d", lt->tm_hour);
    graphics_draw_text(ctx, tbuf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(ccx - SIDEBAR_CLOCK_W / 2, SIDEBAR_CLOCK_TOP,
                             SIDEBAR_CLOCK_W, 17),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                       NULL);
    snprintf(tbuf, sizeof(tbuf), "%02d", lt->tm_min);
    graphics_draw_text(ctx, tbuf, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(ccx - SIDEBAR_CLOCK_W / 2, SIDEBAR_CLOCK_TOP + 17,
                             SIDEBAR_CLOCK_W, 17),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                       NULL);
  }

  const Article *a = current_article();
  if (!a) {
    return;
  }
  int16_t cx = b.size.w / 2;
  int16_t y = SIDEBAR_ICON_TOP(s_win_h);

  // 1. Favourite: a star — bright chrome-yellow with a black outline when
  // starred (visible on any accent), plain black otherwise.
  if (s_star_path) {
    GPoint sc = GPoint(cx, y + SIDEBAR_STAR_H / 2);
    gpath_move_to(s_star_path, sc);
    graphics_context_set_fill_color(ctx,
                                    a->star ? GColorChromeYellow : GColorBlack);
    gpath_draw_filled(ctx, s_star_path);
    if (a->star) {
      graphics_context_set_stroke_color(ctx, GColorBlack);
      graphics_context_set_stroke_width(ctx, 2);
      gpath_draw_outline(ctx, s_star_path);
    }
  }
  y += SIDEBAR_STAR_H + SIDEBAR_ICON_GAP;

  // 2. Read/unread: a plain filled white disc when unread; nothing when
  // read (the classic unread-dot idiom — a black disc vanished on dark
  // accents). The slot stays so the star and M never jump.
  if (!a->read) {
    GPoint c = GPoint(cx, y + SIDEBAR_DISC_D / 2);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, c, SIDEBAR_DISC_D / 2);
  }
  y += SIDEBAR_DISC_D + SIDEBAR_ICON_GAP;

  // 3. Match: the badge is always an "M" — accent M on a black rounded
  // chip by default (the chip is the "area" that turns alarm-red when the
  // current article matches a highlight word; the M then flips to white
  // for contrast on the red).
  bool matched = false;
  for (int i = 0; i < 2; i++) {
    const Page *pg = &s_pages[i];
    if (pg->idx == s_idx) {
      matched = pg->hl_match;
      break;
    }
  }
  GRect chip = GRect(cx - 8, y + 1, 16, 16);
  graphics_context_set_fill_color(ctx,
                                  matched ? HL_ALARM_COLOR : GColorBlack);
  graphics_fill_rect(ctx, chip, 4, GCornersAll);
  graphics_context_set_text_color(ctx, matched ? GColorWhite : s_accent);
  graphics_draw_text(ctx, "M", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(chip.origin.x, chip.origin.y - 2,
                           chip.size.w, chip.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                     NULL);
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

//! Engine scratch: the layout engine is single-threaded and re-layouts
//! happen one at a time. Keeping these off the stack matters — the app
//! stack is only 2 KB on basalt-class watches and this frame was 584 B.
static char s_hl_scratch[HL_TOKEN_MAX + 1]; // longest slice: one hard-broken token
static HlSpan s_hl_spans[HL_SPANS_MAX];

//! End offset of the longest UTF-8-safe prefix of T[poff..pend) that fits
//! `width` px, preferring to break right AFTER a hyphen inside the fitting
//! prefix: long hyphenated compounds ("state-of-the-art") wrap at the
//! hyphens instead of hard-breaking mid-word. Falls back to a character
//! boundary when no hyphen fits. Always advances at least one full char.
static size_t hl_piece_end(const char *t, const HlSpan *spans, int nspans,
                           size_t poff, size_t pend, GFont base, GFont hl,
                           int16_t width) {
  size_t best = poff;
  size_t k = poff;
  while (k < pend) {
    size_t next = k + 1;
    while (next < pend && ((unsigned char)t[next] & 0xC0) == 0x80) {
      next++; // UTF-8 continuation bytes: never split a character
    }
    int16_t w = 0;
    HlSeg segs[HL_SEG_MAX];
    int msegs = hl_token_segments(spans, nspans, poff, next, segs,
                                  HL_SEG_MAX);
    for (int s = 0; s < msegs; s++) {
      w += hl_measure_slice(t, segs[s].off, segs[s].len,
                            segs[s].style ? hl : base, s_hl_scratch);
    }
    if (w > width) {
      break;
    }
    best = next;
    k = next;
  }
  // Prefer the last hyphen inside the fitting prefix: "foo-bar-baz" wraps
  // after the hyphen ("foo-", "bar-", "baz") instead of a hard character
  // break, so the compound stays readable.
  for (size_t j = best; j > poff; j--) {
    if (t[j - 1] == '-') {
      return j;
    }
  }
  return best;
}

//! Emit the styled sub-segments of T[poff..pend) into the run table at the
//! current line position, extending the open run when the style matches
//! (else opening a new run, growing the heap table on demand). `gap` is the
//! inter-word gap before the piece (0 at line start); `line_y` is the
//! current line's top. Shared by the whole-token and wide-token-piece paths.
static void hl_emit_range(HlLayout *lo, const char *t, const HlSpan *spans,
                          int nspans, size_t poff, size_t pend,
                          const HlBuildParams *p, int16_t gap,
                          int16_t *line_x, bool *have_word, int *cur,
                          int16_t *cur_w, int16_t line_y) {
  HlSeg segs[HL_SEG_MAX];
  int msegs = hl_token_segments(spans, nspans, poff, pend, segs, HL_SEG_MAX);
  for (int s = 0; s < msegs; s++) {
    size_t soff = segs[s].off;
    size_t slen = segs[s].len;
    uint8_t sstyle = segs[s].style;
    GFont sf = sstyle ? p->hl_font : p->base_font;
    int16_t sw = hl_measure_slice(t, soff, slen, sf, s_hl_scratch);
    int16_t g = (s == 0) ? gap : 0;
    if (*cur >= 0 && sstyle == lo->runs[*cur].style) {
      lo->runs[*cur].len = (uint16_t)(soff + slen - lo->runs[*cur].off);
      *line_x += g + sw;
      *cur_w += g + sw;
    } else {
      if (*cur >= 0) {
        lo->runs[*cur].w = *cur_w;
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
          *cur = -1;
          *cur_w = 0;
          *line_x += g + sw;
          *have_word = true;
          continue; // heap exhausted: keep the static truncation
        }
      }
      {
        *cur = lo->n;
        HlRun *r = &lo->runs[*cur];
        r->x = *line_x + g;
        r->y = line_y;
        r->off = (uint16_t)soff;
        r->len = (uint16_t)slen;
        r->w = 0;
        r->style = sstyle;
        lo->n++;
        *cur_w = sw;
      }
      *line_x += g + sw;
      *have_word = true;
    }
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
  // Engine scratch lives at file scope (s_hl_scratch / s_hl_spans): the
  // engine is single-threaded and re-layouts happen one at a time, and
  // keeping them off the stack matters — the app stack is only 2 KB on
  // basalt-class watches and this frame was 584 B.
  char *scratch = s_hl_scratch;
  HlSpan *spans = s_hl_spans;

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
    if (have_word && line_x + gap_w + tw > p->width && tw <= p->width) {
      // The token fits on a fresh line: wrap it there whole.
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
    if (tw <= p->width) {
      // Normal token: emit the segments whole.
      hl_emit_range(lo, t, spans, nspans, woff, wend, p,
                    have_word ? gap_w : 0, &line_x, &have_word, &cur,
                    &cur_w, line_y);
    } else {
      // A token WIDER than the line (a long word, or several words joined
      // by hyphens): split it into width-fitting pieces so the whole text
      // stays visible — long hyphenated compounds wrap at the hyphens
      // ("state-of-the-" / "art"), everything else hard-breaks at a
      // character boundary. The first piece may sit on the current line
      // when it fits after the gap; the rest each start a fresh line.
      // Nothing is ever clipped at the right edge.
      size_t poff = woff;
      while (poff < wend) {
        size_t pend = hl_piece_end(t, spans, nspans, poff, wend,
                                   p->base_font, p->hl_font, p->width);
        if (pend <= poff) {
          // One glyph is wider than the line: still advance a full char
          // (the piece will overhang by design; it must not loop forever).
          pend = poff + 1;
          while (pend < wend && ((unsigned char)t[pend] & 0xC0) == 0x80) {
            pend++;
          }
        }
        HlSeg psegs[HL_SEG_MAX];
        int mpsegs = hl_token_segments(spans, nspans, poff, pend, psegs,
                                       HL_SEG_MAX);
        int16_t pw = 0;
        for (int s = 0; s < mpsegs; s++) {
          pw += hl_measure_slice(t, psegs[s].off, psegs[s].len,
                                 psegs[s].style ? p->hl_font : p->base_font,
                                 scratch);
        }
        int16_t pgap = have_word ? gap_w : 0;
        if (have_word && line_x + pgap + pw > p->width) {
          if (p->max_lines > 0 &&
              line_y + lo->line_h >= (int16_t)(p->max_lines * lo->line_h)) {
            truncated = true; // the piece would exceed the cap
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
          pgap = 0;
        }
        hl_emit_range(lo, t, spans, nspans, poff, pend, p, pgap,
                      &line_x, &have_word, &cur, &cur_w, line_y);
        poff = pend;
      }
      if (truncated) {
        break; // the cap cut the token mid-word
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
//! line). Highlighted runs draw their text in hl_text_c (usually base_c;
//! the editorial header uses black text on the accent fill).
static void hl_draw(GContext *ctx, const char *text, const HlLayout *lo,
                    int16_t ox, int16_t oy, GFont base_font, GFont hl_font,
                    GColor base_c, GColor hl_c, GColor hl_text_c,
                    int16_t y_limit) {
  char scratch[HL_TOKEN_MAX + 1];
  for (int k = 0; k < lo->n; k++) {
    const HlRun *r = &lo->runs[k];
    if (y_limit > 0 && oy + r->y + lo->line_h > y_limit) {
      continue; // this line would collide with the feed·time line: skip it
    }
    if (r->style) {
      // Text-marker highlight: fill the line box behind the run in the
      // alarm color, then draw the words BOLD in the highlight text color.
      graphics_context_set_fill_color(ctx, hl_c);
      graphics_fill_rect(ctx, GRect(ox + r->x, oy + r->y + 1, r->w,
                                    lo->line_h - 2),
                         0, GCornerNone);
    }
    GFont f = r->style ? hl_font : base_font;
    graphics_context_set_text_color(ctx, r->style ? hl_text_c : base_c);
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
//! the 80-char preview; while a fetch is in flight a subtle muted hint
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
          theme_fg(), HL_ALARM_COLOR, theme_fg(), 0);
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

//! The visible height of a page's scroll area. In the skim view (not
//! scroll mode) a long article ends early so the reserved band stays clear
//! for the "HOLD ▼: Scroll" hint. In scroll mode the article takes the
//! FULL page — no empty bar while scrolling — and the "HOLD ▼: Next" line
//! acts as the article's last line (the scroll range leaves room for it).
static int16_t page_view_h(const Page *p) {
  bool long_article = (p->content_h > s_view_h + 8);
  if (long_article && !s_scroll_mode) {
    return (int16_t)(s_view_h - END_BAR_H);
  }
  return s_view_h;
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
  // Keep the clip in sync: a short article can grow long when the full
  // summary lands, and vice versa — the bottom band appears/disappears
  // with the hint.
  layer_set_frame(p->root, GRect(0, 0, s_win_w, page_view_h(p)));
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

//! The furthest the content can scroll. Long articles leave room for the
//! "HOLD ▼: Next" line at the very end — it reads like the article's own
//! last line, never over text. Short articles keep the plain bottom
//! (content bottom == view bottom).
static int32_t page_scroll_min(const Page *p) {
  int32_t min_y = s_view_h - p->content_h;
  if (p->content_h > s_view_h + 8) {
    min_y -= END_BAR_H;
  }
  return min_y;
}

//! Manual scroll: move the page's content wrapper by frame — the only
//! movement mechanism proven to render on the user's emery (settles move
//! page roots with layer_set_frame; the ScrollLayer's bounds-origin path
//! never redraws there). Single clamp authority for the scroll offset.
static void page_set_offset(Page *p, int32_t y) {
  int32_t min_y = page_scroll_min(p);
  if (y < min_y) {
    y = min_y;
  }
  if (y > 0) {
    y = 0;
  }
  layer_set_frame(p->content, GRect(0, y, s_win_w, p->content_h));
  layer_mark_dirty(p->content);
  if (s_end_bar) {
    layer_mark_dirty(s_end_bar); // the HOLD DOWN hint follows the bottom state
  }
}

//! Filled down triangle (no arrow glyph exists in the system fonts): the
//! wide base on TOP tapering to the point at the bottom (a ▼ — the DOWN
//! button), centered on cx, 1 px rows.
static void triangle_down(GContext *ctx, int16_t cx, int16_t y, int16_t w,
                          int16_t h) {
  for (int16_t r = 0; r < h; r++) {
    int16_t half = (int16_t)((int32_t)(h - 1 - r) * (w / 2) / (h - 1));
    graphics_draw_line(ctx, GPoint(cx - half, y + r), GPoint(cx + half, y + r));
  }
}

//! Centered "HOLD [▼]: <what>" — HOLD + the down-triangle + the action, so
//! the hint only ever says what holding does (never what a single tap
//! does). The triangle is drawn (no glyph in the fonts), measured into the
//! layout so the whole string reads as one centered unit.
static void hint_draw(GContext *ctx, GRect b, const char *what, GFont f,
                      GColor color, int16_t font_h) {
  char suffix[16];
  snprintf(suffix, sizeof(suffix), ": %s", what);
  GSize s_pre = graphics_text_layout_get_content_size(
      "HOLD ", f, GRect(0, 0, 200, 32), GTextOverflowModeFill,
      GTextAlignmentLeft);
  GSize s_suf = graphics_text_layout_get_content_size(
      suffix, f, GRect(0, 0, 200, 32), GTextOverflowModeFill,
      GTextAlignmentLeft);
  int16_t tri_w = (font_h >= 18) ? 7 : 6;
  int16_t tri_h = (font_h >= 18) ? 5 : 4;
  int16_t total = s_pre.w + 2 + tri_w + 2 + s_suf.w;
  int16_t x = b.origin.x + (b.size.w - total) / 2;
  if (x < b.origin.x) {
    x = b.origin.x;
  }
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, "HOLD ", f, GRect(x, b.origin.y, s_pre.w, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_context_set_stroke_color(ctx, color);
  triangle_down(ctx, x + s_pre.w + 2 + tri_w / 2,
                b.origin.y + font_h / 2 - 2, tri_w, tri_h);
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, suffix, f,
                     GRect(x + s_pre.w + 2 + tri_w + 2, b.origin.y,
                           s_suf.w, b.size.h),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

//! Hint at the bottom of the view, two states, both in the accent color:
//! - initial view of a LONG article (not in scroll mode): "HOLD ▼: Scroll"
//!   on the reserved band — the prompt that holding enters scroll mode
//!   (tap still skips);
//! - scrolled to the very end (scroll mode): "HOLD ▼: Next" — it reads as
//!   the article's last line (the scroll range leaves the band's room), the
//!   tap there is held back, the advance needs the explicit HOLD.
//! Mid-scroll the line stays EMPTY — the user knows a single tap scrolls,
//! so nothing hints at it; the article fills the full page while scrolling.
static void end_bar_update(Layer *layer, GContext *ctx) {
  if (s_count == 0 || !cur_page() || !cur_page()->content) {
    return;
  }
  GRect b = layer_get_bounds(layer);
  if (s_waiting_advance) {
    // A boundary DOWN started the next-page fetch: feedback that the press
    // was received and the reader will continue as soon as data arrives.
    graphics_context_set_text_color(ctx, s_accent);
    graphics_draw_text(ctx, "Loading...",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(b.origin.x, b.origin.y, b.size.w, b.size.h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter,
                       NULL);
    return;
  }
  const Page *p = cur_page();
  bool long_article = (p->content_h > s_view_h + 8); // a real scroll area
  if (!long_article) {
    return;
  }
  if (s_scroll_mode) {
    int32_t off = layer_get_frame(p->content).origin.y;
    if (off > page_scroll_min(p) + 2) {
      return; // mid-scroll: nothing — the tap needs no hint
    }
    hint_draw(ctx, b, "Next", fonts_get_system_font(FONT_KEY_GOTHIC_14),
              s_accent, 14);
    return;
  }
  hint_draw(ctx, b, "Scroll", fonts_get_system_font(FONT_KEY_GOTHIC_14),
            s_accent, 14);
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
//! The 80-char preview stays in the body until the chunks assemble; the
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
//! self-heal): put the 80-char preview layout back.
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

//! Editorial header (P13/P4): page background — no accent band. The heading
//! reads in theme_fg at GOTHIC_24_BOLD with highlighted words on an
//! accent fill in black text (the app's accent-surface treatment — the
//! heading went back to white after the accent experiment of 0.3.35), the
//! feed·time meta sits at the bottom in the ACCENT color, and a 2 px
//! accent rule closes the header like a newspaper dateline. The read
//! state lives in the sidebar icons, not the colors.
static void header_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, theme_bg());
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
  GFont heading_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  int16_t text_w = (int16_t)(b.size.w - SIDEBAR_W - 8);
  // The feed·time line lives at the header's bottom, above the accent rule;
  // a clamped long title must not draw under it.
  char meta[48];
  char t[16];
  format_reltime(t, sizeof(t), a->published);
  snprintf(meta, sizeof(meta), "%s · %s", a->feed, t);
  graphics_context_set_text_color(ctx, s_accent);
  graphics_draw_text(ctx, meta, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(4, b.size.h - HEADER_META_H, text_w, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  // The 2 px accent rule under the meta (editorial dateline).
  graphics_context_set_fill_color(ctx, s_accent);
  graphics_fill_rect(ctx, GRect(4, b.size.h - 3, text_w, 2), 0, GCornerNone);
  // The heading: theme_fg (white in dark) bold text, highlighted words on
  // an accent fill with black text. The LAST line's bottom is
  // 2 + head_layout.height = b.size.h - HEADER_META_H + 2; the y_limit
  // guards the meta line below.
  hl_draw(ctx, a->title, &p->head_layout, 4, 2, heading_font, heading_font,
          theme_fg(), s_accent, GColorBlack,
          (int16_t)(b.size.h - HEADER_META_H + 2));
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
  // ellipsis cap; GOTHIC_24_BOLD throughout — smaller than the 28 of the
  // first overhaul, and drawn in the accent color by header_update;
  // highlighted words sit on a theme_fg chip) and summary body (unlimited
  // lines, base GOTHIC_18 with GOTHIC_18_BOLD for highlighted words).
  GFont gothic18 = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  GFont gothic18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont gothic24b = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  int16_t text_w = (int16_t)(s_win_w - SIDEBAR_W - 8);
  hl_build_layout(&(HlBuildParams){
    .text = a->title,
    .base_font = gothic24b,
    .hl_font = gothic24b,
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
  // The root's height is the page's clip. Long articles end the scroll area
  // early (the layouts already measure the content), so the reserved bottom
  // band stays empty for the Scroll/Next hint.
  int16_t hh = (int16_t)(p->head_layout.height + HEADER_META_H);
  int16_t body_h = p->body_layout.height + 4;
  if (p->idx == s_full_idx && s_full_fetching && !s_full_done) {
    body_h += FULL_HINT_H;
  }
  bool long_article = ((int32_t)hh + 2 + body_h) > s_view_h + 8;
  p->root = layer_create(GRect(0, 0, s_win_w,
                               long_article ? s_view_h - END_BAR_H
                                            : s_view_h));
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
//! cancelled when the transition settles; a healthy 300 ms slide never
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
//! index, drop the old page, release the locks, then arm the auto-mark
//! timer and fetch the full summary for the settled article.
static void transition_finalize(void) {
  transition_watchdog_cancel();
  s_idx = s_target_idx;
  APP_LOG(APP_LOG_LEVEL_INFO, "nav: settle idx=%ld dir=%d", (long)s_idx,
          (int)s_dir);
  page_destroy(&s_pages[s_cur]);
  s_cur = 1 - s_cur;
  s_advancing = false;
  s_advance_guard = false;
  s_scroll_mode = false; // every article starts in the tap-to-skip view
  if (cur_page()) {
    page_resize(cur_page()); // the skim band returns on the settled page
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
    layer_set_frame(cur_page()->root,
                    GRect(0, 0, s_win_w, page_view_h(cur_page())));
  }
}

//! Start a transition in the given direction (+1 next, -1 previous): build
//! the target page, park it off-screen on the entry side, then slide the
//! current page out while the target slides in — both 300 ms ease-out
//! (P15: a slightly softer landing than the old 260 ms ease-in-out),
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
  Page *cp = cur_page();
  int16_t sh = page_view_h(sp); // per-page clip heights: long articles end
  int16_t ch = page_view_h(cp); // early, so the hint band never carries text
  layer_set_frame(sp->root, GRect(0, top + dir * s_view_h, s_win_w, sh));

  s_from_a = layer_get_frame(cp->root);
  s_to_a = GRect(0, top - dir * s_view_h, s_win_w, ch);
  s_anim_a = (Animation *)property_animation_create_layer_frame(cp->root, &s_from_a, &s_to_a);
  animation_set_duration(s_anim_a, 300);
  animation_set_curve(s_anim_a, AnimationCurveEaseOut);
  animation_set_handlers(s_anim_a, (AnimationHandlers){
    .stopped = transition_anim_stopped,
  }, NULL);
  animation_schedule(s_anim_a);

  s_from_b = layer_get_frame(sp->root);
  s_to_b = GRect(0, top, s_win_w, sh);
  s_anim_b = (Animation *)property_animation_create_layer_frame(sp->root, &s_from_b, &s_to_b);
  animation_set_duration(s_anim_b, 300);
  animation_set_curve(s_anim_b, AnimationCurveEaseOut);
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
    // Buffer end. With more pages coming, fetch the next page on demand and
    // hold the advance until its first article lands (timeline_collect_article
    // delivers it) — the reader never dead-ends at a page boundary, so the
    // user never has to back out and re-enter to keep reading. At the true
    // end the pulse confirms there is nothing after.
    if (!s_loaded_all) {
      if (!s_loading) {
        s_loading = true;
        proto_request_items(s_stream, s_cont);
      }
      s_waiting_advance = true;
      vibes_short_pulse(); // the press was received: more is on the way
      if (s_end_bar) {
        layer_mark_dirty(s_end_bar); // "Loading..." hint
      }
      return;
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "nav: advance blocked (last article %ld/%ld)",
            (long)s_idx, (long)s_count);
    vibes_short_pulse();
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
  s_waiting_advance = false; // going back: drop the pending boundary advance
  transition_to(-1);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

//! UP: in the initial view it goes back to the previously read article
//! (mirroring DOWN = next). Inside scroll mode it scrolls the article up by
//! one viewport; at the top it exits scroll mode back to the initial view
//! (a further UP then regresses). The SDK's offset is the content origin:
//! negative when scrolled, 0 at the top.
static void timeline_up_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0 || s_advancing) {
    return;
  }
  Page *p = cur_page();
  if (!s_scroll_mode) {
    APP_LOG(APP_LOG_LEVEL_INFO, "nav: UP (initial) -> previous");
    maybe_regress();
    return;
  }
  int32_t off = layer_get_frame(p->content).origin.y;
  APP_LOG(APP_LOG_LEVEL_INFO, "nav: UP off=%ld scroll-mode", (long)off);
  if (off < 0) {
    int32_t target = off + (s_view_h - 24);
    if (target > 0) {
      target = 0;
    }
    page_set_offset(p, target);
    if (target == 0) {
      s_scroll_mode = false; // at the top: back to the initial view
      if (cur_page()) {
        page_resize(cur_page()); // the hint band returns
      }
      if (s_end_bar) {
        layer_mark_dirty(s_end_bar); // the prompt re-appears
      }
    }
  } else {
    s_scroll_mode = false; // already at the top: just exit scroll mode
    if (cur_page()) {
      page_resize(cur_page()); // the hint band returns
    }
    if (s_end_bar) {
      layer_mark_dirty(s_end_bar);
    }
  }
}

//! DOWN: the initial view ALWAYS skips to the next article (fast skimming —
//! a long article's "- HOLD DOWN -" prompt invites a hold to read it).
//! Inside scroll mode it page-scrolls ~3/4 viewport; at the true bottom of
//! the text the tap is held back (the advance needs the explicit HOLD, the
//! pulse confirms the press). No fetch-hold: pressing DOWN while the full
//! summary is still loading proceeds anyway (fast reading).
static void timeline_down_click(ClickRecognizerRef rec, void *ctx) {
  if (s_count == 0 || s_advancing || s_advance_guard) {
    APP_LOG(APP_LOG_LEVEL_INFO, "nav: DOWN ignored (count=%ld adv=%d guard=%d)",
            (long)s_count, (int)s_advancing, (int)s_advance_guard);
    return;
  }
  Page *p = cur_page();
  if (!s_scroll_mode) {
    APP_LOG(APP_LOG_LEVEL_INFO, "nav: DOWN (initial) -> next");
    maybe_advance();
    return;
  }
  int32_t off = layer_get_frame(p->content).origin.y;
  int32_t content_h = p->content_h;
  int32_t min_y = page_scroll_min(p);
  // The offset is clamped to [min_y, 0] — the content origin: 0 at the
  // top, NEGATIVE when scrolled, min_y at the very bottom (long articles
  // leave one line clear of the HOLD DOWN hint). "At the bottom" is
  // off <= min_y.
  bool at_bottom = (off <= min_y + 2);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "nav: DOWN off=%ld frame=%d cont=%ld bottom=%d",
          (long)off, s_view_h, (long)content_h, (int)at_bottom);
  if (at_bottom) {
    // End of the scroll: one fast tap must not throw the reader past the
    // article — the scroll state is lost. Advance only on an explicit HOLD
    // (the "- HOLD DOWN -" hint at the bottom says so); the pulse confirms
    // the press.
    APP_LOG(APP_LOG_LEVEL_INFO,
            "nav: scroll end — hold DOWN to advance");
    vibes_short_pulse();
    if (s_end_bar) {
      layer_mark_dirty(s_end_bar);
    }
    return;
  }
  // Page-down: ~3/4 viewport per press (a small overlap keeps the previous
  // screen's last line visible for context); the final press lands exactly
  // on the bottom so the last word is clearly visible.
  int32_t step = (s_view_h * 3) / 4;
  int32_t target = off - step;
  if (target < min_y) {
    target = min_y;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "nav: page-down %ld -> %ld", (long)off, (long)target);
  page_set_offset(p, target);
}

//! Toggle the current article's read state (unread -> mark read via the
//! batch path, read -> unread via proto_mark_unread) and cancel any pending
//! auto-mark timer. On an empty (all-caught-up) stream it re-fetches the
//! first page (the status hint points at this). Shared by the SELECT button
//! and the touch tap (which defers the call through the double-tap window).
static void timeline_toggle_read(void) {
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

//! SELECT toggles the current article's read state (see timeline_toggle_read).
static void timeline_select_click(ClickRecognizerRef rec, void *ctx) {
  timeline_toggle_read();
}

//! Toggle the star of the current article (fire-and-forget); the star icon
//! lives in the sidebar. Shared by the long SELECT button and the touch hold.
static void timeline_toggle_star(void) {
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

//! Long SELECT toggles the star of the current article (see timeline_toggle_star).
static void timeline_star_long_click(ClickRecognizerRef rec, void *ctx) {
  timeline_toggle_star();
}

//! HOLD DOWN: on a LONG article's initial view it ENTERS scroll mode (tap
//! then scrolls; the "- HOLD DOWN -" prompt at the bottom invites this).
//! In scroll mode — or on a short article — it jumps to the next article.
//! The advance guard (cleared at the settle) keeps one jump per hold.
static void timeline_down_hold_click(ClickRecognizerRef rec, void *ctx) {
  Page *p = cur_page();
  bool long_article = (p && p->content_h > s_view_h + 8);
  if (!s_scroll_mode && long_article) {
    APP_LOG(APP_LOG_LEVEL_INFO, "nav: hold DOWN -> scroll mode");
    s_scroll_mode = true;
    vibes_short_pulse();
    if (cur_page()) {
      page_resize(cur_page()); // the article takes the full page now
    }
    if (s_end_bar) {
      layer_mark_dirty(s_end_bar); // the prompt hides until the end
    }
    return;
  }
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
// Touch gestures (reader). The reader is a custom paged view — the system
// touch bridge (menus) does nothing for it, so the window disables the
// bridge and consumes the raw touch stream directly (no recognizers: the
// SDK's tap/swipe recognizers proved unreliable and directionally inverted
// on the target firmware, while the raw stream worked):
//   skim view:    swipe up = next article, swipe down = previous
//                 (natural: up = later, same direction as the scroll drag)
//                 tap = read/unread, tap hold (500 ms) = favourite,
//                 double tap = enter article scroll
//   scroll mode:  the article is locked in — the content follows the finger
//                 (drag up = article scrolls down = later text), a fast
//                 flick page-steps in the flick direction, both ends are
//                 clamped no-ops (no advance, no exit), and ONLY a double
//                 tap leaves scroll mode, staying on the same article.
//                 Tap/hold keep their skim-mode actions.
//   left edge:    a swipe from the left screen border to the inside goes
//                 back (window_stack_pop), exactly like touch in the menus.
// The single-tap action is deferred by the double-tap window; a second tap
// inside the window turns it into the double-tap action instead. The hold is
// a 500 ms still press on the raw stream; a fired hold suppresses the tap
// that would otherwise fire on liftoff.
// Platform scope: the touch-service API is real on emery/gabbro (SDK 4.33)
// and a compile-time no-op on the 64 KB class — the whole section is
// compiled out there.
// Debugging: set TOUCH_DEBUG to 1 for a debug build that logs every raw
// event (type, x, y, ms) and every classified gesture — capture with
// `pebble logs` on the phone to trace the firmware's real touch behavior.
// ---------------------------------------------------------------------------

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)

#define TOUCH_DEBUG 0 // 1 = verbose raw-event + gesture logging (debug build)

#define TOUCH_TAP_DEBOUNCE_MS 350  // double-tap window = deferred single tap
#define TOUCH_HOLD_MS 500          // hold threshold (matches the buttons)
#define TOUCH_MOVE_DIST 12         // px of travel that voids a hold / starts a drag
#define TOUCH_SWIPE_MIN 18         // min travel for a skim-mode navigation swipe
#define TOUCH_FLICK_MIN 15         // min travel for a scroll-mode flick
#define TOUCH_FLICK_VEL 300        // px/s above which a drag counts as a flick
#define TOUCH_EDGE_X 12            // left-edge zone width for the back swipe
#define TOUCH_EDGE_MIN 30          // rightward travel that triggers back

static AppTimer *s_tap_timer;     // deferred single tap / double-tap window
static AppTimer *s_hold_timer;    // 500 ms still press -> favourite
static bool s_hold_fired;         // the hold fired during the current touch
static bool s_touch_active;       // raw subscription is live (attach idempotency)
static bool s_dragging;           // this touch travelled past the move distance
static bool s_gest_active;        // a Touchdown was seen for the current touch
static GPoint s_gest_down;        // touchdown point
static uint64_t s_gest_t0;        // touchdown time (ms)
static int32_t s_drag_base;       // scroll-mode content offset at touchdown

static void touch_handle_drag(int16_t dy, int16_t dist, uint32_t dur);

//! Full millisecond clock: time_ms() only returns the sub-second portion,
//! so the seconds part comes from the same call's tloc out-parameter.
static uint64_t touch_now_ms(void) {
  time_t sec = 0;
  uint16_t ms = 0;
  time_ms(&sec, &ms);
  return (uint64_t)sec * 1000u + ms;
}

//! Cancel the deferred single tap (double-tap window). Called when a gesture
//! proves not to be a tap (drag/flick starts) or the window closes.
static void touch_cancel_tap(void) {
  if (s_tap_timer) {
    app_timer_cancel(s_tap_timer);
    s_tap_timer = NULL;
  }
}

//! Enter article scroll mode (double tap on a long article — the same gate
//! as the HOLD DOWN button: short articles have nothing to scroll).
static void touch_enter_scroll(void) {
  if (s_advancing || s_count == 0) {
    return;
  }
  Page *p = cur_page();
  if (!p || p->content_h <= s_view_h + 8) {
    return;
  }
  s_scroll_mode = true;
  vibes_short_pulse();
  if (cur_page()) {
    page_resize(cur_page()); // the article takes the full page now
  }
  if (s_end_bar) {
    layer_mark_dirty(s_end_bar); // the prompt hides until the end
  }
}

//! Leave article scroll mode (double tap again / swipe up at the top).
static void touch_exit_scroll(void) {
  if (!s_scroll_mode) {
    return;
  }
  s_scroll_mode = false;
  if (cur_page()) {
    page_resize(cur_page()); // the hint band returns
  }
  if (s_end_bar) {
    layer_mark_dirty(s_end_bar); // the prompt re-appears
  }
}

//! The deferred single tap fired: toggle read/unread (or refresh on an empty
//! stream). Re-checks the reader state — the article may have changed since
//! the tap (a swipe cancelled the timer in that case, but be safe).
static void touch_tap_cb(void *data) {
  s_tap_timer = NULL;
  if (!s_tl_window || !s_touch) {
    return;
  }
  timeline_toggle_read();
}

//! A tap completed (short press, no travel): the first tap defers the
//! read-toggle by the double-tap window; a second tap inside the window
//! enters/exits article scroll mode instead. A hold that fired during this
//! touch is consumed by the liftoff path before this is reached.
static void touch_handle_tap(void) {
  if (s_advancing) {
    return;
  }
  if (s_count == 0) {
    timeline_toggle_read(); // empty stream: refresh immediately (no double tap)
    return;
  }
  if (s_tap_timer) {
    // Second tap inside the window: a double tap.
    touch_cancel_tap();
    APP_LOG(APP_LOG_LEVEL_INFO, "touch: double tap -> %s scroll mode",
            s_scroll_mode ? "exit" : "enter");
    if (s_scroll_mode) {
      touch_exit_scroll();
    } else {
      touch_enter_scroll();
    }
    return;
  }
  s_tap_timer = app_timer_register(TOUCH_TAP_DEBOUNCE_MS, touch_tap_cb, NULL);
}

//! The 500 ms hold fired: favourite the current article (the same action as
//! long SELECT). The flag suppresses the tap that fires when the finger
//! finally lifts.
static void touch_hold_cb(void *data) {
  s_hold_timer = NULL;
  if (!s_tl_window || !s_touch) {
    return;
  }
  s_hold_fired = true;
  timeline_toggle_star();
}

//! Raw touch stream — the whole gesture layer lives here (no recognizers).
//! Touchdown records the anchor and arms the 500 ms hold; PositionUpdate
//! cancels the hold and drags the scroll-mode content live; Liftoff
//! classifies the gesture: hold (already fired) -> nothing, left-edge
//! rightward swipe -> back, no travel -> tap (deferred / double tap),
//! travel -> skim navigation or scroll-mode flick/edges.
//! The s_gest_active guard drops ORPHAN events: the subscription can start
//! mid-gesture (the reader opens on the liftoff of the menu tap that pushed
//! it, and the tap's own liftoff is delivered to the freshly-subscribed
//! handler) — without a seen Touchdown the anchor is stale and a stray
//! liftoff would read as a phantom edge-swipe (dx = tap position) and pop
//! the reader instantly.
static void touch_raw_handler(const TouchEvent *event, void *context) {
  if (!s_tl_window || !s_touch) {
    return;
  }
#if TOUCH_DEBUG
  APP_LOG(APP_LOG_LEVEL_INFO, "touch: %s x=%d y=%d t=%llu",
          event->type == TouchEvent_Touchdown ? "DOWN" :
          event->type == TouchEvent_Liftoff ? "UP" : "MOVE",
          (int)event->x, (int)event->y,
          (unsigned long long)touch_now_ms());
#endif
  if (event->type == TouchEvent_Touchdown) {
    s_gest_down = GPoint(event->x, event->y);
    s_gest_t0 = touch_now_ms();
    s_hold_fired = false;
    s_dragging = false;
    s_gest_active = true;
    if (s_count > 0 && !s_advancing && !s_hold_timer) {
      s_hold_timer = app_timer_register(TOUCH_HOLD_MS, touch_hold_cb, NULL);
    }
    if (s_scroll_mode && cur_page()) {
      s_drag_base = layer_get_frame(cur_page()->content).origin.y;
    }
    return;
  }
  if (event->type == TouchEvent_PositionUpdate) {
    if (!s_gest_active) {
      return;
    }
    int16_t dx = (int16_t)(event->x - s_gest_down.x);
    int16_t dy = (int16_t)(event->y - s_gest_down.y);
    if (dx < 0) {
      dx = (int16_t)-dx;
    }
    if (dy < 0) {
      dy = (int16_t)-dy;
    }
    if (dx > TOUCH_MOVE_DIST || dy > TOUCH_MOVE_DIST) {
      if (!s_dragging) {
        s_dragging = true;
        touch_cancel_tap(); // a drag is not the tap we deferred
      }
      if (s_hold_timer) {
        app_timer_cancel(s_hold_timer);
        s_hold_timer = NULL;
      }
    }
    // Live drag: the content follows the finger — dragging up reveals the
    // article's later text (the direction the user expects).
    if (s_scroll_mode && cur_page() && s_dragging) {
      page_set_offset(cur_page(), s_drag_base + (int32_t)(event->y - s_gest_down.y));
    }
    return;
  }
  // Liftoff: classify.
  if (!s_gest_active) {
    return; // orphan liftoff (subscribed mid-gesture): not our touch
  }
  s_gest_active = false;
  if (s_hold_timer) {
    app_timer_cancel(s_hold_timer);
    s_hold_timer = NULL;
  }
  if (s_hold_fired) {
    s_hold_fired = false; // the hold already starred; not a tap
    return;
  }
  int16_t dx = (int16_t)(event->x - s_gest_down.x);
  int16_t dy = (int16_t)(event->y - s_gest_down.y);
  int16_t dist = dy < 0 ? (int16_t)-dy : dy;
  uint64_t dur = touch_now_ms() - s_gest_t0;
  // Left-edge swipe inward: go back, like touch in the menus.
  if (s_gest_down.x <= TOUCH_EDGE_X && dx >= TOUCH_EDGE_MIN && dx >= dist) {
    APP_LOG(APP_LOG_LEVEL_INFO, "touch: edge-back (dx=%d)", (int)dx);
    timeline_back_click(NULL, NULL);
    return;
  }
  if (!s_dragging) {
    if (dist <= TOUCH_MOVE_DIST) {
      touch_handle_tap();
    }
    return;
  }
  if (s_advancing || s_count == 0) {
    return;
  }
  touch_handle_drag(dy, dist, (uint32_t)dur);
}

//! Page-step in scroll mode: ~3/4 viewport per flick (like the DOWN/UP
//! buttons); `forward` = toward the article's later text. Clamped at both
//! ends — scroll mode is locked to the article, so reaching an edge is a
//! no-op, never an exit or an advance (only a double tap leaves).
static void scroll_page_step(bool forward) {
  Page *p = cur_page();
  if (!p) {
    return;
  }
  int32_t off = layer_get_frame(p->content).origin.y;
  int32_t min_y = page_scroll_min(p);
  int32_t step = (s_view_h * 3) / 4;
  if (forward) {
    int32_t target = off - step;
    if (target < min_y) {
      target = min_y;
    }
    page_set_offset(p, target);
  } else {
    int32_t target = off + step;
    if (target > 0) {
      target = 0;
    }
    page_set_offset(p, target);
  }
}

//! A drag/flick ended (travel past TOUCH_MOVE_DIST, no hold). Skim view: any
//! swipe of TOUCH_SWIPE_MIN+ travels navigates articles — natural direction,
//! finger up = next (the same "up = later" as the scroll drag). Scroll mode:
//! the drag already scrolled live; a fast flick adds a page-step. The
//! article is locked in: nothing here advances or exits — only a double tap
//! leaves scroll mode (staying on the same article).
static void touch_handle_drag(int16_t dy, int16_t dist, uint32_t dur) {
  if (!s_scroll_mode) {
    if (dist >= TOUCH_SWIPE_MIN) {
      if (dy < 0) {
        APP_LOG(APP_LOG_LEVEL_INFO, "touch: swipe up (dy=%d) -> next", (int)dy);
        maybe_advance();
      } else {
        APP_LOG(APP_LOG_LEVEL_INFO, "touch: swipe down (dy=%d) -> prev", (int)dy);
        maybe_regress();
      }
    }
    return;
  }
  Page *p = cur_page();
  if (!p) {
    return;
  }
  uint32_t vel = dist * 1000 / (dur > 0 ? dur : 1);
  bool flick = (dist >= TOUCH_FLICK_MIN) && (vel >= TOUCH_FLICK_VEL);
#if TOUCH_DEBUG
  int32_t off = layer_get_frame(p->content).origin.y;
  APP_LOG(APP_LOG_LEVEL_INFO,
          "touch: lift dy=%d dist=%d dur=%u vel=%u flick=%d off=%ld",
          (int)dy, (int)dist, (unsigned)dur, (unsigned)vel, (int)flick,
          (long)off);
#endif
  if (flick) {
    scroll_page_step(dy < 0); // finger up = forward (later text)
  }
}

//! Attach the reader's touch layer: disable the window's touch bridge (the
//! global recognizer set would otherwise swallow the stream) and subscribe
//! to the raw touch stream. Idempotent (s_touch_active). No-op when touch is
//! disabled or the platform stubs the API out (touch_service_is_enabled
//! compiles to false there).
static void touch_attach(void) {
  if (!s_touch || !touch_service_is_enabled() || s_touch_active) {
    return;
  }
  window_set_touch_bridge_disabled(s_tl_window, true);
  touch_service_subscribe(touch_raw_handler, NULL);
  s_touch_active = true;
  s_gest_active = false; // a fresh subscription sees no in-flight gesture
  APP_LOG(APP_LOG_LEVEL_INFO, "touch: reader gestures armed");
}

//! Detach the reader's touch layer: cancel the pending timers, unsubscribe
//! from the raw stream and re-enable the window's touch bridge. Idempotent.
static void touch_detach(void) {
  touch_cancel_tap();
  if (s_hold_timer) {
    app_timer_cancel(s_hold_timer);
    s_hold_timer = NULL;
  }
  if (s_touch_active) {
    touch_service_unsubscribe();
    s_touch_active = false;
    APP_LOG(APP_LOG_LEVEL_INFO, "touch: reader gestures disarmed");
  }
  if (s_tl_window) {
    window_set_touch_bridge_disabled(s_tl_window, false);
  }
}

//! The TouchEnabled setting changed while the reader is open: (re)attach or
//! tear the touch layer down to match. Called from proto.c's settings path.
void timeline_touch_apply(void) {
  if (!s_tl_window) {
    return;
  }
  if (s_touch && touch_service_is_enabled()) {
    touch_attach();
  } else {
    touch_detach();
  }
}

#else // !(EMERY || GABBRO): the SDK stubs the touch APIs out on the 64 KB class.

//! No touch API on this platform: the setting hook is a no-op.
void timeline_touch_apply(void) {
}

#endif // touch-capable platforms

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
  s_status_check_path = gpath_create(&UI_CHECK_PATH_INFO);
  s_dither_bitmap = gbitmap_create_with_data(s_dither_pbi);
  s_cur = 0;
  s_scroll_mode = false;
  s_pages[0].idx = -1;
  s_pages[1].idx = -1;

  // Black top bar with the stream name in accent (y = 2..2 + TOP_BAR_H).
  s_top_bar = layer_create(GRect(0, PROGRESS_H, s_win_w, TOP_BAR_H));
  layer_set_update_proc(s_top_bar, top_bar_update);
  layer_add_child(root, s_top_bar);

  // Thin theme divider line between the top bar and the scrollable page.
  s_divider = layer_create(GRect(0, TOP_BAR_H + PROGRESS_H, s_win_w, DIVIDER_H));
  layer_set_update_proc(s_divider, divider_update);
  layer_add_child(root, s_divider);

  // Top bar: stream name in GOTHIC_18_BOLD (0.3.36: smaller than the
  // ROBOTO_CONDENSED_21 of the overhaul) in theme_fg (white on the black
  // crown in dark, black on white in light).
  s_top_text = text_layer_create(GRect(4, PROGRESS_H, s_win_w - SIDEBAR_W - 8, TOP_BAR_H));
  text_layer_set_font(s_top_text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_top_text, GTextAlignmentLeft);
  text_layer_set_overflow_mode(s_top_text, GTextOverflowModeTrailingEllipsis);
  text_layer_set_background_color(s_top_text, GColorClear);
  text_layer_set_text_color(s_top_text, theme_fg());
  text_layer_set_text(s_top_text, s_title);
  layer_add_child(root, text_layer_get_layer(s_top_text));

  // Progress line along the very top (y = 0..PROGRESS_H). The old position
  // dot is gone (0.3.35); the layer is exactly the bar height now.
  s_prog_line = layer_create(GRect(0, 0, s_win_w, PROGRESS_H));
  layer_set_update_proc(s_prog_line, progress_update);
  layer_add_child(root, s_prog_line);

  s_status = text_layer_create(GRect(8, s_win_h / 2 - 30, s_win_w - 16, 48));
  text_layer_set_font(s_status, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_status, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_status, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_status, GColorClear);
  text_layer_set_text_color(s_status, theme_muted());
  text_layer_set_text(s_status, s_loading ? "Loading..." : "All caught up");
  layer_add_child(root, text_layer_get_layer(s_status));

  // All-caught-up chrome: accent check above the text, hint below it. Both
  // are hidden by status_update() unless the stream is empty and loaded.
  s_status_check = layer_create(GRect(s_win_w / 2 - 12, s_win_h / 2 - 52, 24, 18));
  layer_set_update_proc(s_status_check, status_check_update);
  layer_add_child(root, s_status_check);

  s_status_hint = text_layer_create(GRect(0, s_win_h / 2 + 24, s_win_w, 16));
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
  // A ROOT child added AFTER the page area: the pages are built later and
  // would otherwise paint over it (the page roots stack on top of anything
  // added to the page area first).
  s_end_bar = layer_create(GRect(0, TOP_BAR_H + PROGRESS_H + DIVIDER_H +
                                     s_view_h - END_BAR_H,
                                 s_win_w, END_BAR_H));
  layer_set_update_proc(s_end_bar, end_bar_update);
  layer_add_child(root, s_end_bar);

  s_sidebar = layer_create(GRect(s_win_w - SIDEBAR_W, 0, SIDEBAR_W, s_win_h));
  layer_set_update_proc(s_sidebar, sidebar_update);
  layer_add_child(root, s_sidebar);
  s_clock_timer = app_timer_register(60000, clock_tick_cb, NULL);

  window_set_click_config_provider(window, timeline_click_config_provider);

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  touch_attach(); // touch gestures, when enabled and the platform supports them
#endif

  status_update();
}

//! Teardown on window unload: flush any pending mark-read batch, stop the
//! in-flight animations (so no stopped handler touches destroyed layers)
//! and release the layer pointers.
static void timeline_close(void) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
  touch_detach(); // cancel touch timers, drop recognizers + raw subscription
#endif
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

//! the first article arrives. `total` is the stream's article count at open
//! (the feed/folder unread count — the progress bar's real denominator);
//! pass 0 when unknown (the bar then falls back to the loaded window).
void timeline_open(const char *stream, const char *title, int32_t total) {
  if (s_tl_window) {
    window_stack_remove(s_tl_window, true);
  }
  snprintf(s_stream, sizeof(s_stream), "%s", stream ? stream : "");
  snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
  s_count = 0;
  s_page_announced = 0; // the next page_begin pins the progress denominator
  s_total = total > 0 ? total : 0;
  s_dropped = 0;
  s_waiting_advance = false;
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
      s_dropped += s_count; // every buffered article moved past the reader
      s_count = 0;
      s_idx = 0;
      full_summary_reset(); // everything dropped: the buffered text is gone
      // The pages' articles were evicted too: mark them inert so they stop
      // referencing dropped indices (they draw nothing until rebuilt).
      for (int i = 0; i < 2; i++) {
        s_pages[i].idx = -1;
      }
    } else {
      s_dropped += drop; // the evicted oldest are read-and-passed articles
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
    s_dropped++; // one article moved past the reader (the ring's front)
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
  // The user pressed DOWN at the end of the loaded buffer while the next
  // page streamed in: the moment a fresh article is in the ring, deliver
  // the held advance — exactly one step, no fast-forward through the page.
  if (s_waiting_advance && !s_advancing && s_idx + 1 < s_count) {
    s_waiting_advance = false;
    transition_to(1);
  }
}

//! The page ended: store the continuation ("", = no more items), release the
//! loading flag and show the all-caught-up state on an empty stream.
void timeline_page_end(const char *cont) {
  snprintf(s_cont, sizeof(s_cont), "%s", cont ? cont : "");
  s_loaded_all = (s_cont[0] == '\0');
  s_loading = false;
  if (s_waiting_advance) {
    // The on-demand page delivered nothing new (empty/errored fetch): drop
    // the wait so the "Loading..." hint clears; a further DOWN re-fetches.
    s_waiting_advance = false;
    if (s_end_bar) {
      layer_mark_dirty(s_end_bar);
    }
  }

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
    text_layer_set_text_color(s_top_text, theme_fg()); // theme bar text
  }
  if (s_top_bar) {
    layer_mark_dirty(s_top_bar);
  }
  if (s_divider) {
    layer_mark_dirty(s_divider); // theme divider color
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
  GFont gothic24b = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  int16_t text_w = (int16_t)(s_win_w - SIDEBAR_W - 8);
  for (int i = 0; i < 2; i++) {
    Page *p = &s_pages[i];
    if (p->idx < 0 || p->idx >= s_count) {
      continue;
    }
    const Article *a = &s_articles[p->idx];
    // The heading re-layout must use the same font as page_build
    // (GOTHIC_24_BOLD) — it used GOTHIC_18_BOLD before 0.3.36, so a
    // word change while reading shrank the heading.
    hl_build_layout(&(HlBuildParams){
      .text = a->title, .base_font = gothic24b, .hl_font = gothic24b,
      .width = text_w, .max_lines = 0, .out = &p->head_layout,
    });
    // The body text depends on the full-summary state: the heap buffer when
    // the article's full text is complete, else the 80-char preview.
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
