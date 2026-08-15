#ifndef TIMELINE_H
#define TIMELINE_H

#include <pebble.h>

// ---------------------------------------------------------------------------
// Timeline reading view: a native-Timeline-style list (accent bar, dots,
// pin notch, star markers) over a ring buffer of article headings, plus the
// article detail card with a single-slot summary cache.
// ---------------------------------------------------------------------------

//! Open (or reset and re-open) the timeline for a stream; requests page 1.
void timeline_open(const char *stream, const char *title);

//! Item-page collect hooks, driven by proto_handle_inbox.
void timeline_page_begin(int32_t n);
void timeline_collect_article(DictionaryIterator *iter);
void timeline_page_end(const char *cont);
void timeline_summary_arrived(DictionaryIterator *iter);

//! Re-apply accent/theme to the timeline + detail windows (from settings).
void timeline_apply_settings(void);

#endif
