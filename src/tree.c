#include <pebble.h>

#include "tree.h"
#include "storage.h"
#include "common.h"

// ---------------------------------------------------------------------------
// Feed tree model (see tree.h). Kept as a flat array; the phone streams
// folders before the feeds that reference them, feeds always carry a parent.
// ---------------------------------------------------------------------------

static FeedNode s_nodes[MAX_FEED_NODES];
static int32_t s_node_count;

static uint16_t s_collect_expected; // node count announced by FeedCount
static uint16_t s_collected;        // nodes appended so far

static const char *READING_LIST_ID = "user/-/state/com.google/reading-list";
static const char *STARRED_ID = "user/-/state/com.google/starred";

static FeedNode *find_node(const char *id);

void tree_reset(void) {
  s_node_count = 0;
  memset(s_nodes, 0, sizeof(s_nodes));
}

//! Append one node (bounds-checked; silently drops overflows).
void tree_add_node(int32_t kind, const char *id, const char *name,
                   int32_t unread, const char *parent) {
  if (s_node_count >= MAX_FEED_NODES) {
    return;
  }
  FeedNode *n = &s_nodes[s_node_count++];
  snprintf(n->id, sizeof(n->id), "%s", id ? id : "");
  snprintf(n->name, sizeof(n->name), "%s", name ? name : "");
  snprintf(n->parent, sizeof(n->parent), "%s", parent ? parent : "");
  n->kind = (uint8_t)(kind < 0 ? 0 : (kind > 2 ? 2 : kind));
  n->unread = unread;
}

// ---------------------------------------------------------------------------
// Streaming collect
// ---------------------------------------------------------------------------

void tree_begin_collect(int32_t count) {
  tree_reset();
  s_collect_expected = (uint16_t)(count < 0 ? 0
                               : (count > MAX_FEED_NODES ? MAX_FEED_NODES : count));
  s_collected = 0;
  if (s_collect_expected == 0) {
    // Empty tree: finish immediately so the UI is released.
    tree_fetch_done();
  }
}

void tree_collect_node(int32_t kind, const char *id, const char *name,
                       int32_t unread, const char *parent) {
  tree_add_node(kind, id, name, unread, parent);
  s_collected++;
  if (s_collected >= s_collect_expected) {
    tree_fetch_done();
  }
}

//! The whole tree has arrived: recompute folder sums, persist the cache and
//! let main.c refresh the menus (and dismiss any working dialog).
void tree_fetch_done(void) {
  tree_compute_unread();
  storage_save_tree(s_nodes, s_node_count);
  ui_tree_updated();
}

// ---------------------------------------------------------------------------
// Unread accounting
// ---------------------------------------------------------------------------

void tree_compute_unread(void) {
  // Folders are recomputed from their subtree; the starred special is always
  // 0 (starring is a boolean flag, not a count); the reading-list special
  // keeps the server-reported count.
  for (int i = 0; i < s_node_count; i++) {
    FeedNode *n = &s_nodes[i];
    if (n->kind == 1) {
      n->unread = 0;
    } else if (n->kind == 0 && strcmp(n->id, STARRED_ID) == 0) {
      n->unread = 0;
    }
  }
  // Every feed adds its unread to each folder in its ancestor chain
  // (folders arrive before feeds, so tree_find always resolves parents).
  for (int i = 0; i < s_node_count; i++) {
    const FeedNode *f = &s_nodes[i];
    if (f->kind != 2) {
      continue;
    }
    const char *pid = f->parent;
    int guard = 0;
    while (pid && pid[0] && guard++ < MAX_FEED_NODES) {
      FeedNode *p = find_node(pid);
      if (!p || p->kind != 1) {
        break;
      }
      p->unread += f->unread;
      pid = p->parent;
    }
  }
}

//! Optimistic badge update after a mark-read batch: the feed's own counter
//! and every ancestor (folders + the reading-list special), floored at 0.
void tree_feed_decrement(const char *feed_id) {
  FeedNode *feed = find_node(feed_id);
  if (!feed || feed->kind != 2) {
    return;
  }
  if (feed->unread > 0) {
    feed->unread--;
  }
  const char *pid = feed->parent;
  int guard = 0;
  while (pid && pid[0] && guard++ < MAX_FEED_NODES) {
    FeedNode *p = find_node(pid);
    if (!p) {
      break;
    }
    if (p->unread > 0) {
      p->unread--;
    }
    pid = p->parent;
  }
  FeedNode *all = find_node(READING_LIST_ID);
  if (all && all->unread > 0) {
    all->unread--;
  }
}

// ---------------------------------------------------------------------------
// Row mapping for menus
// ---------------------------------------------------------------------------

int tree_root_count(void) {
  int n = 0;
  for (int i = 0; i < s_node_count; i++) {
    const FeedNode *nd = &s_nodes[i];
    if (nd->kind == 0 || nd->parent[0] == '\0') {
      n++;
    }
  }
  return n;
}

const FeedNode *tree_root_node(int row) {
  int n = 0;
  for (int i = 0; i < s_node_count; i++) {
    const FeedNode *nd = &s_nodes[i];
    if (nd->kind == 0 || nd->parent[0] == '\0') {
      if (n == row) {
        return nd;
      }
      n++;
    }
  }
  return NULL;
}

int tree_child_count(const char *folder_id) {
  int n = 0;
  for (int i = 0; i < s_node_count; i++) {
    const FeedNode *nd = &s_nodes[i];
    if ((nd->kind == 1 || nd->kind == 2) && strcmp(nd->parent, folder_id) == 0) {
      n++;
    }
  }
  return n;
}

const FeedNode *tree_child_node(const char *folder_id, int row) {
  int n = 0;
  // Child folders first.
  for (int i = 0; i < s_node_count; i++) {
    const FeedNode *nd = &s_nodes[i];
    if (nd->kind == 1 && strcmp(nd->parent, folder_id) == 0) {
      if (n == row) {
        return nd;
      }
      n++;
    }
  }
  // Then child feeds.
  for (int i = 0; i < s_node_count; i++) {
    const FeedNode *nd = &s_nodes[i];
    if (nd->kind == 2 && strcmp(nd->parent, folder_id) == 0) {
      if (n == row) {
        return nd;
      }
      n++;
    }
  }
  return NULL;
}

//! Linear lookup by stream id (non-const internal variant for mutation).
static FeedNode *find_node(const char *id) {
  if (!id || !id[0]) {
    return NULL;
  }
  for (int i = 0; i < s_node_count; i++) {
    if (strcmp(s_nodes[i].id, id) == 0) {
      return &s_nodes[i];
    }
  }
  return NULL;
}

//! Linear lookup by stream id.
const FeedNode *tree_find(const char *id) {
  return find_node(id);
}

//! Load the persisted cache (instant start) into the live array.
int tree_load_cache(void) {
  tree_reset();
  s_node_count = storage_load_tree(s_nodes, MAX_FEED_NODES);
  return s_node_count;
}
