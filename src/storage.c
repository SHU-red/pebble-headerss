#include <pebble.h>

#include "storage.h"
#include "common.h"

// ---------------------------------------------------------------------------
// Persist keys (launcher numbering scheme: small ids for settings, a base +
// index for the tree cache).
// ---------------------------------------------------------------------------

#define PERSIST_KEY_ACCENT 1     // int32: 24-bit hex of the accent color
#define PERSIST_KEY_DARK 2       // int32: 1 = dark theme
#define PERSIST_KEY_TOUCH 3      // int32: 1 = native touch navigation
#define PERSIST_KEY_MARK_LIST 4  // int32: 1 = mark read on open (timeline)
#define PERSIST_KEY_MARK_DETAIL 5 // int32: 1 = mark read on open (detail)
#define PERSIST_KEY_TREE_COUNT 10 // int32: number of cached tree nodes
#define PERSIST_KEY_TREE_BASE 20  // + i: FeedNode blobs (<= 256 B each)

#define TREE_CACHE_MAX 32 // cap persisted nodes (persist size budget)

#define DEFAULT_ACCENT_HEX 0x0055AA // GColorCobaltBlue (24-bit RGB)

GColor s_accent;
bool s_dark;
bool s_touch;
bool s_mark_list;
bool s_mark_detail;

//! 24-bit RGB hex of a GColor8 (2-bit channels scaled up), for flash storage.
static uint32_t accent_to_hex(GColor c) {
  uint8_t r = (c.argb >> 4) & 0x3;
  uint8_t g = (c.argb >> 2) & 0x3;
  uint8_t b = c.argb & 0x3;
  return (((uint32_t)r * 85) << 16) | (((uint32_t)g * 85) << 8) | ((uint32_t)b * 85);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

//! Load all settings from flash. Missing keys fall back to the defaults:
//! cobalt blue accent, light theme, no touch navigation, mark-read toggles ON.
void storage_load(void) {
  uint32_t accent_hex = (uint32_t)persist_read_int(PERSIST_KEY_ACCENT);
  if (accent_hex <= 255) {
    // 0 = unset; <=255 = a raw GColor8 byte from an older layout -> default.
    accent_hex = DEFAULT_ACCENT_HEX;
  }
  s_accent = GColorFromHEX(accent_hex);
  s_dark = persist_read_int(PERSIST_KEY_DARK) != 0;
  s_touch = persist_read_int(PERSIST_KEY_TOUCH) != 0;
  // persist_read_int returns 0 for missing keys, which would read as OFF;
  // the mark-read toggles default to ON for fresh installs.
  s_mark_list = persist_exists(PERSIST_KEY_MARK_LIST)
                    ? persist_read_int(PERSIST_KEY_MARK_LIST) != 0
                    : true;
  s_mark_detail = persist_exists(PERSIST_KEY_MARK_DETAIL)
                      ? persist_read_int(PERSIST_KEY_MARK_DETAIL) != 0
                      : true;
}

//! Persist every settings toggle. Called after any Clay-delivered change.
void storage_save_settings(void) {
  persist_write_int(PERSIST_KEY_ACCENT, (int32_t)accent_to_hex(s_accent));
  persist_write_int(PERSIST_KEY_DARK, s_dark ? 1 : 0);
  persist_write_int(PERSIST_KEY_TOUCH, s_touch ? 1 : 0);
  persist_write_int(PERSIST_KEY_MARK_LIST, s_mark_list ? 1 : 0);
  persist_write_int(PERSIST_KEY_MARK_DETAIL, s_mark_detail ? 1 : 0);
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
