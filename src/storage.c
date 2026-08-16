#include <pebble.h>

#include "storage.h"
#include "common.h"

// ---------------------------------------------------------------------------
// Persist key map (launcher numbering scheme: small ids for settings, a base
// + index for the tree cache and the per-feed last-seen table).
//
//   1  AccentColor (int32, 24-bit hex)
//   2  DarkMode (int32)
//   3  TouchEnabled (int32)
//   4  (retired: MarkOnOpenList — removed in 0.3.0, replaced by MarkMode 14)
//   5  (retired: MarkOnOpenDetail — removed in 0.3.0, replaced by MarkMode 14)
//   6  UnreadOnly (int32)
//   7  ImportantRow (int32, smart-surface toggle; default ON)
//   8  NewDot (int32, smart-surface toggle; default ON)
//   9  ProgressLine (int32, smart-surface toggle; default ON)
//   10 TreeCount (int32)
//   11 TriageDrain (int32, smart-surface toggle; default OFF)
//   12 (retired: LastSeenCount — 0.3.0 layout, moved to 15; swept on load)
//   13 HighlightWords (string CSV: comma-separated highlight words from Clay,
//      max 10 entries of 32 bytes each, <= 340 B stored)
//   14 MarkMode (int32: MarkMode enum, default MARK_NOW)
//   15 LastSeenCount (int32)
//   20+i  Tree cache: FeedNode blob per node (<= 256 B each)
//   100+i (retired: 0.3.0 last-seen blobs — 56 B {char id[48]; int64_t};
//      moved to 200+i; keys 100..163 swept on load)
//   200+i LastSeen: {char id[16]; int64_t seconds;} per feed (24 B each)
// ---------------------------------------------------------------------------

#define PERSIST_KEY_ACCENT 1     // int32: 24-bit hex of the accent color
#define PERSIST_KEY_DARK 2       // int32: 1 = dark theme
#define PERSIST_KEY_TOUCH 3      // int32: 1 = native touch navigation
// Retired in 0.3.0 (replaced by PERSIST_KEY_MARK_MODE): kept only so
// storage_load can sweep the stale flash entries on first run.
#define PERSIST_KEY_MARK_LIST 4  // int32 (retired): MarkOnOpenList
#define PERSIST_KEY_MARK_DETAIL 5 // int32 (retired): MarkOnOpenDetail
#define PERSIST_KEY_UNREAD_ONLY 6 // int32: 1 = only fetch unread articles
#define PERSIST_KEY_IMPORTANT 7  // int32: 1 = Important row in root menu
#define PERSIST_KEY_NEWDOT 8     // int32: 1 = NEW-dot on unseen feeds
#define PERSIST_KEY_PROGRESS 9   // int32: 1 = progress line on the top bar
#define PERSIST_KEY_TREE_COUNT 10 // int32: number of cached tree nodes
#define PERSIST_KEY_TRIAGE 11    // int32: 1 = triage drain from Starred
#define PERSIST_KEY_LASTSEEN_COUNT 15 // int32: number of last-seen entries
#define PERSIST_KEY_HIGHLIGHT 13 // string CSV: highlight words (<= 340 B)
#define PERSIST_KEY_MARK_MODE 14 // int32: MarkMode enum (default MARK_NOW)
// Retired in 0.3.1 (24 B entries moved to key 15 / base 200): kept only so
// storage_load can sweep the stale flash entries on first run.
#define PERSIST_KEY_LASTSEEN_COUNT_OLD 12 // int32 (retired): 0.3.0 count
#define PERSIST_KEY_LASTSEEN_BASE_OLD 100 // + i (retired): 0.3.0 56 B entries
#define PERSIST_KEY_TREE_BASE 20  // + i: FeedNode blobs (<= 256 B each)
#define PERSIST_KEY_LASTSEEN_BASE 200 // + i: last-seen entries (24 B each)

#define TREE_CACHE_MAX 32 // cap persisted nodes (persist size budget)

#define DEFAULT_ACCENT_HEX 0x0055AA // GColorCobaltBlue (24-bit RGB)

//! One per-feed last-seen entry (seconds granularity).
typedef struct {
  char id[16];     // stream id ("feed/N", ...); 16 B covers "feed/N" + NUL
  int64_t seconds; // last-seen unix seconds, from FeedNewest/1000000
} LastSeenEntry;

static LastSeenEntry s_last_seen[MAX_FEED_NODES];
static int s_last_seen_count;

GColor s_accent;
bool s_dark;
bool s_touch;
bool s_unread_only;
bool s_important;
bool s_newdot;
bool s_progress;
bool s_triage;

//! Auto-mark mode (see common.h MarkMode). Default MARK_NOW matches the old
//! both-toggles-ON behavior; persisted at PERSIST_KEY_MARK_MODE.
static int s_mark_mode = MARK_NOW;

//! 24-bit RGB hex of a GColor8 (2-bit channels scaled up), for flash storage.
static uint32_t accent_to_hex(GColor c) {
  uint8_t r = (c.argb >> 4) & 0x3;
  uint8_t g = (c.argb >> 2) & 0x3;
  uint8_t b = c.argb & 0x3;
  return (((uint32_t)r * 85) << 16) | (((uint32_t)g * 85) << 8) | ((uint32_t)b * 85);
}

// ---------------------------------------------------------------------------
// Smart-surface setting getters (declared in common.h)
// ---------------------------------------------------------------------------

bool setting_important(void) { return s_important; }
bool setting_triage(void) { return s_triage; }
bool setting_newdot(void) { return s_newdot; }
bool setting_progress(void) { return s_progress; }

// ---------------------------------------------------------------------------
// Auto-mark read mode (declared in common.h; persisted at key 14)
// ---------------------------------------------------------------------------

//! Current auto-mark mode; MARK_NOW when nothing valid is stored yet.
int mark_mode(void) {
  return s_mark_mode;
}

//! Set + persist a new mode, clamping anything outside the enum to MARK_NOW.
void mark_mode_set(int mode) {
  if (mode < MARK_NEVER || mode >= MARK_MODE_COUNT) {
    mode = MARK_NOW;
  }
  if (s_mark_mode != mode) {
    s_mark_mode = mode;
    persist_write_int(PERSIST_KEY_MARK_MODE, mode);
  }
}

// ---------------------------------------------------------------------------
// Per-feed last-seen (NEW-dot support). A small id-keyed table persisted at
// PERSIST_KEY_LASTSEEN_BASE + i; populated when items arrive for a feed (the
// phone sends FeedNewest with ItemCount). Missing entries read as 0.
// ---------------------------------------------------------------------------

static void storage_save_last_seen(void) {
  persist_write_int(PERSIST_KEY_LASTSEEN_COUNT, s_last_seen_count);
  for (int i = 0; i < s_last_seen_count; i++) {
    persist_write_data(PERSIST_KEY_LASTSEEN_BASE + i,
                       &s_last_seen[i], sizeof(LastSeenEntry));
  }
  for (int i = s_last_seen_count; i < MAX_FEED_NODES; i++) {
    if (persist_exists(PERSIST_KEY_LASTSEEN_BASE + i)) {
      persist_delete(PERSIST_KEY_LASTSEEN_BASE + i);
    }
  }
}

int64_t storage_feed_last_seen(const char *feed_id) {
  if (!feed_id || !feed_id[0]) {
    return 0;
  }
  for (int i = 0; i < s_last_seen_count; i++) {
    if (strcmp(s_last_seen[i].id, feed_id) == 0) {
      return s_last_seen[i].seconds;
    }
  }
  return 0;
}

void storage_feed_set_last_seen(const char *feed_id, int64_t seconds) {
  if (!feed_id || !feed_id[0]) {
    return;
  }
  for (int i = 0; i < s_last_seen_count; i++) {
    if (strcmp(s_last_seen[i].id, feed_id) == 0) {
      if (s_last_seen[i].seconds != seconds) {
        s_last_seen[i].seconds = seconds;
        // Update just this slot (a page arrival fires once per stream open).
        persist_write_data(PERSIST_KEY_LASTSEEN_BASE + i,
                           &s_last_seen[i], sizeof(LastSeenEntry));
      }
      return;
    }
  }
  if (s_last_seen_count >= MAX_FEED_NODES) {
    return; // table full: keep the newest-writer behaviour simple, drop
  }
  snprintf(s_last_seen[s_last_seen_count].id,
           sizeof(s_last_seen[0].id), "%s", feed_id);
  s_last_seen[s_last_seen_count].seconds = seconds;
  persist_write_data(PERSIST_KEY_LASTSEEN_BASE + s_last_seen_count,
                     &s_last_seen[s_last_seen_count], sizeof(LastSeenEntry));
  s_last_seen_count++;
  persist_write_int(PERSIST_KEY_LASTSEEN_COUNT, s_last_seen_count);
}

// ---------------------------------------------------------------------------
// Highlight words (reader word highlighting). The Clay config delivers a
// comma-separated list; the watch parses it into HL_WORDS_MAX entries of up
// to HL_WORD_MAX bytes each (whitespace-trimmed, empty entries dropped) and
// persists the normalized CSV at PERSIST_KEY_HIGHLIGHT. The reader polls the
// parsed table via highlight_word_count()/highlight_word(); proto.c hands
// new lists to storage_highlight_set_words().
// ---------------------------------------------------------------------------

static char s_hl_words[HL_WORDS_MAX][HL_WORD_MAX + 1];
static int s_hl_count;
// Normalized CSV: entries joined with "," (10*33 + 9 commas + NUL = 340).
static char s_hl_csv[HL_WORDS_MAX * (HL_WORD_MAX + 1) + HL_WORDS_MAX + 1];

//! Parse a comma-separated list into s_hl_words (trimmed, capped, empties
//! dropped). Comma is the only separator; spaces around an entry are trimmed.
static void hl_parse(const char *csv) {
  s_hl_count = 0;
  if (!csv) {
    return;
  }
  const char *p = csv;
  while (*p && s_hl_count < HL_WORDS_MAX) {
    // Skip separators and leading whitespace between entries.
    while (*p == ',' || *p == ' ' || *p == '\t') {
      p++;
    }
    if (!*p) {
      break;
    }
    const char *start = p;
    while (*p && *p != ',') {
      p++;
    }
    const char *end = p; // at a comma or the NUL
    // Trim trailing whitespace (before the comma/end).
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
      end--;
    }
    size_t len = (size_t)(end - start);
    if (len > HL_WORD_MAX) {
      // Cap at 32 bytes without splitting a UTF-8 sequence.
      len = HL_WORD_MAX;
      while (len > 0 && ((unsigned char)start[len] & 0xC0) == 0x80) {
        len--;
      }
      end = start + len;
    }
    if (len > 0) {
      memcpy(s_hl_words[s_hl_count], start, len);
      s_hl_words[s_hl_count][len] = '\0';
      s_hl_count++;
    }
  }
}

//! Rebuild the normalized CSV (entries joined with ",") from the parsed table.
static void hl_rebuild_csv(void) {
  s_hl_csv[0] = '\0';
  for (int i = 0; i < s_hl_count; i++) {
    size_t l = strlen(s_hl_csv);
    snprintf(s_hl_csv + l, sizeof(s_hl_csv) - l, "%s%s",
             i ? "," : "", s_hl_words[i]);
  }
}

//! Accessors (declared in common.h) — read by the timeline highlight engine.
int highlight_word_count(void) {
  return s_hl_count;
}

const char *highlight_word(int i) {
  return (i >= 0 && i < s_hl_count) ? s_hl_words[i] : "";
}

//! Persist a new list (Clay -> watch) and re-parse it in place.
void storage_highlight_set_words(const char *csv) {
  hl_parse(csv ? csv : "");
  hl_rebuild_csv();
  // String payload (strlen + NUL), fits the <= 340 B budget.
  persist_write_data(PERSIST_KEY_HIGHLIGHT, s_hl_csv, strlen(s_hl_csv) + 1);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

//! Load all settings from flash. Missing keys fall back to the defaults:
//! cobalt blue accent, light theme, no touch navigation, MARK_NOW auto-mark.
void storage_load(void) {
  uint32_t accent_hex = (uint32_t)persist_read_int(PERSIST_KEY_ACCENT);
  if (accent_hex <= 255) {
    // 0 = unset; <=255 = a raw GColor8 byte from an older layout -> default.
    accent_hex = DEFAULT_ACCENT_HEX;
  }
  s_accent = GColorFromHEX(accent_hex);
  // Dark theme is the app's look (Timeline-style); missing key = dark.
  s_dark = persist_exists(PERSIST_KEY_DARK) ? persist_read_int(PERSIST_KEY_DARK) != 0 : true;
  s_touch = persist_read_int(PERSIST_KEY_TOUCH) != 0;
  // Auto-mark mode: default MARK_NOW for fresh installs (the old
  // both-toggles-ON default); clamp any persisted garbage to the enum range.
  int mm = (int)persist_read_int(PERSIST_KEY_MARK_MODE);
  if (mm < MARK_NEVER || mm >= MARK_MODE_COUNT) {
    mm = MARK_NOW;
  }
  s_mark_mode = mm;
  // Retire the 0.3.0-predecessor toggles: nothing reads keys 4/5 anymore,
  // so drop the stale flash entries once.
  if (persist_exists(PERSIST_KEY_MARK_LIST)) {
    persist_delete(PERSIST_KEY_MARK_LIST);
  }
  if (persist_exists(PERSIST_KEY_MARK_DETAIL)) {
    persist_delete(PERSIST_KEY_MARK_DETAIL);
  }
  // "Unread only" defaults ON: read articles stay out of the server fetches.
  s_unread_only = persist_exists(PERSIST_KEY_UNREAD_ONLY)
                      ? persist_read_int(PERSIST_KEY_UNREAD_ONLY) != 0
                      : true;
  // Smart-surface toggles: Important row / NEW-dot / Progress line default ON,
  // Triage drain defaults OFF.
  s_important = persist_exists(PERSIST_KEY_IMPORTANT)
                    ? persist_read_int(PERSIST_KEY_IMPORTANT) != 0
                    : true;
  s_newdot = persist_exists(PERSIST_KEY_NEWDOT)
                 ? persist_read_int(PERSIST_KEY_NEWDOT) != 0
                 : true;
  s_progress = persist_exists(PERSIST_KEY_PROGRESS)
                   ? persist_read_int(PERSIST_KEY_PROGRESS) != 0
                   : true;
  s_triage = persist_exists(PERSIST_KEY_TRIAGE)
                 ? persist_read_int(PERSIST_KEY_TRIAGE) != 0
                 : false;

  // Per-feed last-seen table. First retire the 0.3.0 layout: the count lived
  // at key 12 with 56 B entries at 100+i ({char id[48]; int64_t}). Ids are
  // only ever "feed/N", so entries shrank to 24 B and moved to 200+i (count
  // at 15). Sweep the stale flash entries once; key 12's absence means the
  // sweep already ran.
  if (persist_exists(PERSIST_KEY_LASTSEEN_COUNT_OLD)) {
    persist_delete(PERSIST_KEY_LASTSEEN_COUNT_OLD);
    for (int i = 0; i < 64; i++) { // old cap was 64 slots (keys 100..163)
      if (persist_exists(PERSIST_KEY_LASTSEEN_BASE_OLD + i)) {
        persist_delete(PERSIST_KEY_LASTSEEN_BASE_OLD + i);
      }
    }
  }
  s_last_seen_count = (int)persist_read_int(PERSIST_KEY_LASTSEEN_COUNT);
  if (s_last_seen_count < 0 || s_last_seen_count > MAX_FEED_NODES) {
    s_last_seen_count = 0;
  }
  for (int i = 0; i < s_last_seen_count; i++) {
    int got = persist_read_data(PERSIST_KEY_LASTSEEN_BASE + i,
                                &s_last_seen[i], sizeof(LastSeenEntry));
    if (got != (int)sizeof(LastSeenEntry)) {
      s_last_seen_count = i;
      break;
    }
  }

  // Highlight words (CSV string); absent/corrupt -> empty list (count 0).
  s_hl_count = 0;
  s_hl_csv[0] = '\0';
  int got_hl = persist_read_data(PERSIST_KEY_HIGHLIGHT, s_hl_csv,
                                 sizeof(s_hl_csv) - 1);
  if (got_hl > 0) {
    s_hl_csv[got_hl] = '\0';
    hl_parse(s_hl_csv);
    hl_rebuild_csv(); // normalize whatever came back from flash
  }
}

//! Persist every settings toggle. Called after any Clay-delivered change.
void storage_save_settings(void) {
  persist_write_int(PERSIST_KEY_ACCENT, (int32_t)accent_to_hex(s_accent));
  persist_write_int(PERSIST_KEY_DARK, s_dark ? 1 : 0);
  persist_write_int(PERSIST_KEY_TOUCH, s_touch ? 1 : 0);
  persist_write_int(PERSIST_KEY_MARK_MODE, s_mark_mode);
  persist_write_int(PERSIST_KEY_UNREAD_ONLY, s_unread_only ? 1 : 0);
  persist_write_int(PERSIST_KEY_IMPORTANT, s_important ? 1 : 0);
  persist_write_int(PERSIST_KEY_NEWDOT, s_newdot ? 1 : 0);
  persist_write_int(PERSIST_KEY_PROGRESS, s_progress ? 1 : 0);
  persist_write_int(PERSIST_KEY_TRIAGE, s_triage ? 1 : 0);
  storage_save_last_seen();
}

// ---------------------------------------------------------------------------
// Theme palette (mirror launcher)
// ---------------------------------------------------------------------------

GColor theme_bg(void) { return s_dark ? GColorBlack : GColorWhite; }
GColor theme_fg(void) { return s_dark ? GColorWhite : GColorBlack; }
GColor theme_muted(void) { return s_dark ? GColorLightGray : GColorDarkGray; }

// ---------------------------------------------------------------------------
// Tree cache: instant start with the last known feed tree; refreshed in the
// background on launch. Trimmed to TREE_CACHE_MAX nodes.
// ---------------------------------------------------------------------------

void storage_save_tree(const FeedNode *nodes, int count) {
  if (!nodes || count <= 0) {
    persist_write_int(PERSIST_KEY_TREE_COUNT, 0);
    return;
  }
  int n = count > TREE_CACHE_MAX ? TREE_CACHE_MAX : count;
  persist_write_int(PERSIST_KEY_TREE_COUNT, n);
  for (int i = 0; i < n; i++) {
    persist_write_data(PERSIST_KEY_TREE_BASE + i, &nodes[i], sizeof(FeedNode));
  }
  // Delete stale keys when the cache shrank.
  for (int i = n; i < TREE_CACHE_MAX; i++) {
    if (persist_exists(PERSIST_KEY_TREE_BASE + i)) {
      persist_delete(PERSIST_KEY_TREE_BASE + i);
    }
  }
}

int storage_load_tree(FeedNode *nodes, int max) {
  if (!nodes || max <= 0) {
    return 0;
  }
  int n = (int)persist_read_int(PERSIST_KEY_TREE_COUNT);
  if (n < 0 || n > TREE_CACHE_MAX) {
    n = 0;
  }
  if (n > max) {
    n = max;
  }
  for (int i = 0; i < n; i++) {
    int got = persist_read_data(PERSIST_KEY_TREE_BASE + i, &nodes[i], sizeof(FeedNode));
    if (got != (int)sizeof(FeedNode)) {
      // Corrupt/partial blob: stop and keep only what read back cleanly.
      memset(&nodes[i], 0, sizeof(FeedNode));
      n = i;
      break;
    }
  }
  return n;
}
