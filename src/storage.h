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
extern bool s_mark_list;     // "Mark read on open (list)" — watch sub-menu toggle
extern bool s_mark_detail;   // "Mark read on open (detail)" — watch sub-menu toggle

void storage_load(void);
void storage_save_settings(void);
void storage_save_tree(const FeedNode *nodes, int count);
int storage_load_tree(FeedNode *nodes, int max);

//! Theme palette by s_dark (mirror launcher).
GColor theme_bg(void);
GColor theme_fg(void);
GColor theme_muted(void);

#endif
