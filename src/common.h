#ifndef COMMON_H
#define COMMON_H

#include <pebble.h>

// ---------------------------------------------------------------------------
// Shared limits and wire types. These are the single source of truth for the
// buffer budgets of the whole app (tree cache, article ring buffer, AppMessage
// batching) — keep them in sync with the phone-side protocol in package.json.
// ---------------------------------------------------------------------------

// 64 on the 128 KB watches (emery/gabbro), 48 on the 64 KB class: the
// 0.2.1 highlight engine costs ~5 KB of .text/.bss, and .text+.data+.bss
// must stay <= 65535 B (uint16 virtual_size) with runtime heap for
// app_message buffers + windows left over.
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define MAX_FEED_NODES 64
#else
#define MAX_FEED_NODES 48
#endif
//! Article ring-buffer budget: the Time 2 (emery) has 128 KB RAM, the
//! basalt-class platforms 64 KB — bigger window on the target watch keeps
//! more read articles re-openable in a session.
#if defined(PBL_PLATFORM_EMERY)
// 68 (was 72): .text+.data+.bss must stay <= 65535 B (uint16 virtual_size).
// The 0.3.0 reader overhaul (full title, sidebar icons, auto-mark timer,
// full-summary assembly) pushed emery to 65512 B; 68 articles keeps a small
// emery advantage over the 64-article platforms with ~1 KB of headroom.
#define MAX_ARTICLES 68
#elif defined(PBL_PLATFORM_GABBRO)
#define MAX_ARTICLES 64
#else
// 56 on the 64 KB class: keeps heap free above ~9 KB so app_message buffers
// (inbox 4096 + outbox 1024) and the window stack still fit at runtime.
#define MAX_ARTICLES 56
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

// ---------------------------------------------------------------------------
// Highlight words (Clay list -> watch). The comma-separated CSV is persisted
// by storage.c (persist key 13) and parsed at load; an empty/absent list
// yields count 0. Words are matched case-insensitively (ASCII fold) in the
// reader's title and summary. Declared here so any translation unit can read
// them; the setter lives in storage.h.
// ---------------------------------------------------------------------------

#define HL_WORDS_MAX 10 // cap from the phone-side Clay config
#define HL_WORD_MAX 32  // bytes per entry (trimmed at parse time)

//! Number of parsed highlight words (0..HL_WORDS_MAX).
int highlight_word_count(void);
//! Word i as a NUL-terminated string; "" when i is out of range.
const char *highlight_word(int i);
//! The normalized comma-separated word list ("" when none).
const char *highlight_words_csv(void);

// ---------------------------------------------------------------------------
// Auto-mark read mode (0.3.0). Replaces the old MarkOnOpenList /
// MarkOnOpenDetail watch toggles: one mode says how eagerly the reader marks
// an opened article read. Persisted by storage.c (persist key 14); the
// reader (timeline.c) polls mark_mode() and arms its dwell timer with
// MARK_MODE_DELAY_MS[mode].
// ---------------------------------------------------------------------------

typedef enum {
  MARK_NEVER = 0, // never auto-mark (manual SELECT toggle only)
  MARK_NOW,       // mark as soon as an article opens (old both-toggles-ON default)
  MARK_1S,
  MARK_2S,
  MARK_3S,
  MARK_5S,
  MARK_10S,
} MarkMode;

#define MARK_MODE_COUNT 7
//! Selection-window labels (main.c), indexed by MarkMode.
#define MARK_MODE_LABELS { "Never", "Immediately", "1s", "2s", "3s", "5s", "10s" }
//! Dwell delay before auto-marking read, indexed by MarkMode (0 = immediately;
//! MARK_NEVER disables auto-marking entirely).
#define MARK_MODE_DELAY_MS { 0, 0, 1000, 2000, 3000, 5000, 10000 }

//! Current auto-mark mode (default MARK_NOW); read by the timeline reader.
int mark_mode(void);
//! Persist a new auto-mark mode (clamped to the valid enum range).
void mark_mode_set(int mode);

//! One article in the timeline ring buffer (heading + summary — there is no
//! separate detail view; the list IS the reader).
typedef struct {
  char id[24];       // decimal microsecond id, kept as a string
  char title[80];    // full article title (the header renders it multi-line, no cap)
  char feed[24];     // feed display name
  char feed_id[16];  // "feed/N"
  char summary[140]; // stripped summary text
  int32_t published; // unix seconds
  uint8_t read;
  uint8_t star;
} Article;

#endif
