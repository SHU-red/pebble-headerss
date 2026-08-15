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
                   int32_t unread, const char *parent, int64_t newest) {
  if (s_node_count >= MAX_FEED_NODES) {
    return;
  }
  FeedNode *n = &s_nodes[s_node_count++];
  // strncpy + forced NUL (NOT snprintf: newlib's vfprintf has a deep stack
  // frame and this runs inside the AppMessage inbox callback on the 2 KB
  // basalt-class stack).
  strncpy(n->id, id ? id : "", sizeof(n->id) - 1);
  n->id[sizeof(n->id) - 1] = '\0';
  strncpy(n->name, name ? name : "", sizeof(n->name) - 1);
  n->name[sizeof(n->name) - 1] = '\0';
  strncpy(n->parent, parent ? parent : "", sizeof(n->parent) - 1);
  n->parent[sizeof(n->parent) - 1] = '\0';
  n->kind = (uint8_t)(kind < 0 ? 0 : (kind > 2 ? 2 : kind));
  n->unread = unread;
  n->newest = newest;
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
                       int32_t unread, const char *parent, int64_t newest) {
  tree_add_node(kind, id, name, unread, parent, newest);
  s_collected++;
  if (s_collected >= s_collect_expected) {
    tree_fetch_done();
  }
}

//! The whole tree has arrived: recompute folder sums, persist the cache,
//! sort into menu order and let main.c refresh the menus (and dismiss any
//! working dialog).
void tree_fetch_done(void) {
  tree_compute_unread();
  storage_save_tree(s_nodes, s_node_count);
  tree_sort();
  ui_tree_updated();
}

// ---------------------------------------------------------------------------
// Unread accounting
// ---------------------------------------------------------------------------

void tree_compute_unread(void) {
  // Folders are recomputed from their subtree; the starred special keeps
  // the phone-reported star count (the GReader API has no starred breakdown,
  // so the JS counts the starred stream itself); the reading-list special
  // keeps the server-reported count.
  for (int i = 0; i < s_node_count; i++) {
    FeedNode *n = &s_nodes[i];
    if (n->kind == 1) {
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

//! Optimistic star-count adjustment (star on: +1, off: -1, floored at 0) so
//! the root menu's Starred badge follows in-session star toggles.
void tree_starred_adjust(int delta) {
  FeedNode *n = find_node(STARRED_ID);
  if (!n) {
    return;
  }
  int32_t v = n->unread + delta;
  n->unread = v < 0 ? 0 : v;
}

//! Walk a feed's ancestor chain (folders) plus the reading-list special,
//! decrementing each by `dec` (floored at 0).
static void tree_dec_ancestors(const char *pid, int32_t dec) {
  int guard = 0;
  while (pid && pid[0] && guard++ < MAX_FEED_NODES) {
    FeedNode *p = find_node(pid);
    if (!p) {
      break;
    }
    if (p->unread > 0) {
      p->unread -= dec;
      if (p->unread < 0) {
        p->unread = 0;
      }
    }
    pid = p->parent;
  }
  FeedNode *all = find_node(READING_LIST_ID);
  if (all && all->unread > 0) {
    all->unread -= dec;
    if (all->unread < 0) {
      all->unread = 0;
    }
  }
}

//! Is `child` (a feed's parent chain) inside the folder `folder`?
static bool tree_in_folder(const char *pid, const char *folder) {
  int guard = 0;
  while (pid && pid[0] && guard++ < MAX_FEED_NODES) {
    if (strcmp(pid, folder) == 0) {
      return true;
    }
    const FeedNode *p = find_node(pid);
    if (!p) {
      break;
    }
    pid = p->parent;
  }
  return false;
}

//! Optimistic mark-all-read: zero the target stream locally (the whole
//! reading list, one feed, or a folder + its subtree) and decrement the
//! ancestor badges, so the menus hit 0 IMMEDIATELY. The phone syncs the
//! server in the background; a later refresh re-verifies. The Starred
//! counter is untouched (stars are not an unread state).
void tree_mark_all_read(const char *stream) {
  if (!stream || !stream[0]) {
    // Whole reading list: zero everything except the Starred counter.
    for (int i = 0; i < s_node_count; i++) {
      FeedNode *n = &s_nodes[i];
      if (n->kind != 0 || strcmp(n->id, STARRED_ID) != 0) {
        n->unread = 0;
      }
    }
    return;
  }
  FeedNode *n = find_node(stream);
  if (!n) {
    return;
  }
  if (n->kind == 2) {
    // One feed: its folders + the reading list lose the feed's count.
    tree_dec_ancestors(n->parent, n->unread);
    n->unread = 0;
  } else if (n->kind == 1) {
    // A folder: zero every feed inside it, then its own ancestors lose the
    // folder's whole count.
    for (int i = 0; i < s_node_count; i++) {
      FeedNode *f = &s_nodes[i];
      if (f->kind == 2 && tree_in_folder(f->parent, n->id)) {
        f->unread = 0;
      }
    }
    tree_dec_ancestors(n->parent, n->unread);
    n->unread = 0;
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

// ---------------------------------------------------------------------------
// Menu ordering
// ---------------------------------------------------------------------------

//! Rank for the stable menu sort: specials (kind 0) stay pinned at the top,
//! then folders (kind 1), then feeds (kind 2) — sub-folders stay above feeds.
static int sort_rank(const FeedNode *n) {
  return n->kind >= 2 ? 2 : (int)n->kind;
}

//! Compare two nodes for the stable menu sort. Specials and folders keep
//! their (stable) arrival order; feeds sort within their parent group by
//! newest desc, unread desc, title asc.
static int node_sort_cmp(const FeedNode *a, const FeedNode *b) {
  int ra = sort_rank(a);
  int rb = sort_rank(b);
  if (ra != rb) {
    return ra - rb;
  }
  if (ra != 2) {
    return 0; // specials / folders: stable arrival order
  }
  int p = strcmp(a->parent, b->parent); // keep each folder's feeds together
  if (p != 0) {
    return p;
  }
  if (a->newest != b->newest) {
    return a->newest > b->newest ? -1 : 1; // newest desc
  }
  if (a->unread != b->unread) {
    return a->unread > b->unread ? -1 : 1; // unread desc
  }
  return strcmp(a->name, b->name); // title asc
}

//! Stable insertion sort of the live array (64 nodes max; O(n^2) is trivial
//! here and keeps equal keys in arrival order).
void tree_sort(void) {
  for (int i = 1; i < s_node_count; i++) {
    FeedNode key = s_nodes[i];
    int j = i - 1;
    while (j >= 0 && node_sort_cmp(&s_nodes[j], &key) > 0) {
      s_nodes[j + 1] = s_nodes[j];
      j--;
    }
    s_nodes[j + 1] = key;
  }
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
  tree_sort();
  return s_node_count;
}
