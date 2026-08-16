#ifndef STORAGE_H
#define STORAGE_H

#include <pebble.h>
#include "common.h"

// ---------------------------------------------------------------------------
// Durable settings + tree cache. The watch is the source of truth for the
// settings (launcher pattern): Clay values land here, RequestConfig re-sends
// them, and every change is written to flash immediately.
// ---------------------------------------------------------------------------

//! Theme mode (watch sub-menu toggle, single tap cycles). "System" follows
//! the app's default look (dark, Timeline-style); the PebbleOS exposes no
//! system-theme API, so the app default is the nearest equivalent.
typedef enum {
  THEME_SYSTEM = 0,
  THEME_DARK = 1,
  THEME_LIGHT = 2,
} ThemeMode;

extern GColor s_accent;      // GColor8, default GColorCobaltBlue
extern int8_t s_theme;       // ThemeMode
extern bool s_touch;
extern bool s_unread_only;   // "Unread only" — hide read articles from the server
extern bool s_important;     // "Important row" — root menu special row (default ON)
extern bool s_progress;      // "Progress line" — timeline top bar (default ON)

void storage_load(void);
void storage_save_settings(void);
void storage_save_tree(const FeedNode *nodes, int count);
int storage_load_tree(FeedNode *nodes, int max);

//! Effective dark-mode flag: THEME_SYSTEM and THEME_DARK are dark, only
//! THEME_LIGHT is light.
bool theme_dark(void);

//! Theme palette by theme_dark() (mirror launcher).
GColor theme_bg(void);
GColor theme_fg(void);
GColor theme_muted(void);

//! Persist a new highlight-word CSV (Clay -> watch, comma-separated, max
//! HL_WORDS_MAX entries of HL_WORD_MAX bytes) and re-parse it in place.
//! Getter accessors are declared in common.h.
void storage_highlight_set_words(const char *csv);

#endif
