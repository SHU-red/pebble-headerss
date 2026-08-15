#ifndef PROTO_H
#define PROTO_H

#include <pebble.h>

// ---------------------------------------------------------------------------
// AppMessage codec. Exactly mirrors the messageKeys table in package.json;
// the phone-side JS agent implements the same keys and semantics.
// ---------------------------------------------------------------------------

//! Register AppMessage handlers and open the inbound/outbound buffers.
void proto_init(void);

//! Watch -> phone senders.
void proto_request_tree(void);
void proto_request_items(const char *stream, const char *cont);
void proto_request_summary(const char *id);
void proto_star(const char *id, bool on);
void proto_mark_all_read(const char *stream);
void proto_reply_config(void);

//! Mark-read batching: ids accumulate locally and flush as a CSV MarkRead
//! payload when the batch fills (MARK_BATCH_MAX) or MARK_FLUSH_MS elapses.
void proto_mark_push(const char *id);
void proto_flush_now(void);

//! Dispatch one inbound message by key presence (launcher chain style).
void proto_handle_inbox(DictionaryIterator *iter);

//! Defined in main.c: global result/error surface and settings application.
void ui_result(int code, const char *text);
bool ui_result_active(void);
void apply_settings(void);

#endif
