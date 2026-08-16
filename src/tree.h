#ifndef TREE_H
#define TREE_H

#include <pebble.h>
#include "common.h"

// ---------------------------------------------------------------------------
// Feed tree model. Nodes arrive streamed from the phone (one per AppMessage);
// the collect phase buffers them, then recomputes folder sums, persists the
// cache and notifies the UI. Row-mapping helpers feed the root and folder
// menus (folders always before feeds among children).
// ---------------------------------------------------------------------------

void tree_reset(void);
void tree_add_node(int32_t kind, const char *id, const char *name,
                   int32_t unread, const char *parent);

//! Streaming collect: begin_collect resets and records the expected node
//! count; collect_node appends and fires tree_fetch_done() once the expected
//! count is reached (also fires immediately for a 0-node tree).
void tree_begin_collect(int32_t count);
void tree_collect_node(int32_t kind, const char *id, const char *name,
                       int32_t unread, const char *parent);
void tree_fetch_done(void);

//! Stable menu order: specials pinned at top (arrival order), then folders
//! (arrival order), then feeds — grouped by parent — sorted by unread desc,
//! title asc. Runs on fetch-done and cache load.
void tree_sort(void);

//! Folder unread = sum of the subtree feeds; the reading-list special keeps
//! the server count; starred is forced to 0.
void tree_compute_unread(void);
//! Optimistic badge update after a mark-read: decrement the feed and every
//! ancestor (folders + the reading-list special), flooring at 0.
void tree_feed_decrement(const char *feed_id);
//! Optimistic Starred-badge adjustment (star on: +1, off: -1, floored 0).
void tree_starred_adjust(int delta);
//! Optimistic mark-all-read: zero the stream (+ subtree/ancestors) locally.
void tree_mark_all_read(const char *stream);

//! Row mapping for the root menu: specials + top-level folders + top-level
//! feeds, in tree_sort() order (specials pinned, folders, then feeds sorted
//! by unread desc / title asc within their group).
int tree_root_count(void);
const FeedNode *tree_root_node(int row);
//! Row mapping for a folder menu: child folders first, then child feeds
//! (feeds sorted by unread desc / title asc).
int tree_child_count(const char *folder_id);
const FeedNode *tree_child_node(const char *folder_id, int row);

const FeedNode *tree_find(const char *id);

//! Load the persisted tree cache into s_nodes (instant start). Returns count.
int tree_load_cache(void);

//! Defined in main.c: refresh root/folder menus and hide the working dialog.
void ui_tree_updated(void);

#endif
