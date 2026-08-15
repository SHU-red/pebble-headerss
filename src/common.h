#ifndef COMMON_H
#define COMMON_H

#include <pebble.h>

// ---------------------------------------------------------------------------
// Shared limits and wire types. These are the single source of truth for the
// buffer budgets of the whole app (tree cache, article ring buffer, AppMessage
// batching) — keep them in sync with the phone-side protocol in package.json.
// ---------------------------------------------------------------------------

#define MAX_FEED_NODES 64
#define MAX_ARTICLES 64
#define PAGE_SIZE 50
#define MARK_BATCH_MAX 12
#define MARK_FLUSH_MS 500
#define REQUEST_TIMEOUT_MS 12000
#define TIMELINE_ROW_H 72

//! One node of the feed tree as streamed by the phone.
//! kind: 0 special (reading-list / starred), 1 folder, 2 feed.
typedef struct {
  char id[48];     // stream id ("feed/N", "user/-/label/...", special ids)
  char name[32];   // display name
  char parent[48]; // parent folder's id, "" = root
  uint8_t kind;
  int32_t unread;
} FeedNode;

//! One article in the timeline ring buffer (heading + summary — there is no
//! separate detail view; the list IS the reader).
typedef struct {
  char id[24];       // decimal microsecond id, kept as a string
  char title[64];
  char feed[24];     // feed display name
  char feed_id[16];  // "feed/N"
  char summary[140]; // stripped summary text
  int32_t published; // unix seconds
  uint8_t read;
  uint8_t star;
} Article;

#endif
