#ifndef COMMON_H
#define COMMON_H

#include <pebble.h>

// ---------------------------------------------------------------------------
// Shared limits and wire types. These are the single source of truth for the
// buffer budgets of the whole app (tree cache, article ring buffer, AppMessage
// batching) — keep them in sync with the phone-side protocol in package.json.
// ---------------------------------------------------------------------------

#define MAX_FEED_NODES 64
//! Article ring-buffer budget: the Time 2 (emery) has 128 KB RAM, the
//! basalt-class platforms 64 KB — bigger window on the target watch keeps
//! more read articles re-openable in a session.
#if defined(PBL_PLATFORM_EMERY)
#define MAX_ARTICLES 96
#else
#define MAX_ARTICLES 64
#endif
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
  int64_t newest;  // FeedNewest (decimal microseconds) from the phone; 0 if absent
} FeedNode;

// ---------------------------------------------------------------------------
// Smart-surface settings (watch sub-menu toggles). Declared here so any
// translation unit can read them; implemented in storage.c. C->JS direction:
// the values are also reported on RequestConfig (keys ImportantRow /
// TriageDrain / NewDot / ProgressLine), the JS may ignore them.
// ---------------------------------------------------------------------------

bool setting_important(void); // Important row in the root menu — default ON
bool setting_triage(void);    // Triage drain (auto-unstar from Starred) — default OFF
bool setting_newdot(void);    // NEW-dot on feeds with unseen items — default ON
bool setting_progress(void);  // Progress line on the timeline top bar — default ON

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
