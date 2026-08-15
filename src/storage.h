#ifndef STORAGE_H
#define STORAGE_H

#include <pebble.h>
#include "common.h"

// ---------------------------------------------------------------------------
// Durable settings + tree cache. The watch is the source of truth for the
// settings (launcher pattern): Clay values land here, RequestConfig re-sends
// them, and every change is written to flash immediately.
// ---------------------------------------------------------------------------

extern GColor s_accent;      // GColor8, default GColorCobaltBlue
extern bool s_dark;
extern bool s_touch;
extern bool s_unread_only;   // "Unread only" — hide read articles from the server
extern bool s_important;     // "Important row" — root menu special row (default ON)
extern bool s_newdot;        // "NEW-dot" — unseen-feed marker (default ON)
extern bool s_progress;      // "Progress line" — timeline top bar (default ON)
extern bool s_triage;        // "Triage drain" — auto-unstar from Starred (default OFF)

void storage_load(void);
void storage_save_settings(void);
void storage_save_tree(const FeedNode *nodes, int count);
int storage_load_tree(FeedNode *nodes, int max);

//! Per-feed last-seen (seconds, derived from the FeedNewest of the last items
//! page that arrived for the feed). Used by the NEW-dot: a feed row shows the
//! marker when tree newest (seconds) > this value. Missing entry reads 0.
int64_t storage_feed_last_seen(const char *feed_id);
//! Upsert the last-seen seconds for a feed (keyed by feed id, persisted at
//! PERSIST_KEY_LASTSEEN_BASE + i, see storage.c for the key map).
void storage_feed_set_last_seen(const char *feed_id, int64_t seconds);

//! Theme palette by s_dark (mirror launcher).
GColor theme_bg(void);
GColor theme_fg(void);
GColor theme_muted(void);

//! Persist a new highlight-word CSV (Clay -> watch, comma-separated, max
//! HL_WORDS_MAX entries of HL_WORD_MAX bytes) and re-parse it in place.
//! Getter accessors are declared in common.h.
void storage_highlight_set_words(const char *csv);

#endif
