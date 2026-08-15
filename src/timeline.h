#ifndef TIMELINE_H
#define TIMELINE_H

#include <pebble.h>

// ---------------------------------------------------------------------------
// Timeline reading view: a native-Timeline-style list (accent spine, dots,
// pin notch, animated selection wash, star markers) over a ring buffer of
// article headings + summaries. No detail view — rows always show heading
// and summary.
// ---------------------------------------------------------------------------

//! Open (or reset and re-open) the timeline for a stream; requests page 1.
void timeline_open(const char *stream, const char *title);

//! Item-page collect hooks, driven by proto_handle_inbox.
void timeline_page_begin(int32_t n);
void timeline_collect_article(DictionaryIterator *iter);
void timeline_page_end(const char *cont);

//! Re-apply accent/theme to the timeline window (from settings).
void timeline_apply_settings(void);

//! A new highlight-word list arrived (Clay -> watch): re-layout the open
//! reader's current article (body + header) in place and mark it dirty, so
//! the words highlight live while reading. No-op when no reader is open.
void timeline_highlight_words_changed(void);

//! One chunk of the current article's full summary arrived from the phone
//! (FullSummary reply to a proto_request_summary fetch). Chunks are handed
//! through straight from the inbox buffer — NOT copied — so this hook must
//! copy `text` into its own heap buffer before returning. `last` marks the
//! final chunk; once it arrives the assembled full text replaces the
//! 140-char preview in the scrollable body. A ("", true) call (SummaryLast
//! alone) closes an empty/errored fetch and keeps the preview. `id` is the
//! article id the phone put on the chunk ("" when absent): a chunk whose id
//! does not match the settled article is dropped (stale pipe protection).
//! No-op when no reader is open or the chunks are stale (the reader moved
//! on).
void timeline_full_summary_chunk(const char *text, bool last, const char *id);

#endif
