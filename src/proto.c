#include <pebble.h>
#include <stdlib.h>

#include "proto.h"
#include "storage.h"
#include "tree.h"
#include "timeline.h"
#include "common.h"

// Shallow stack helpers (defined at the bottom): used by proto_handle_inbox,
// which must avoid deep libc frames (strtoll/vfprintf) on the 2 KB stack.
static int64_t parse_decimal(const char *s);
static void copy_str(char *dst, size_t cap, const char *src);

// ---------------------------------------------------------------------------
// AppMessage codec (see proto.h). Every send is fire-and-forget with a log
// on failure; the mark-read path is batched so rapid reading does not flood
// the BLE link (one edit-tag POST per batch on the phone side).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Mark-read batch queue
// ---------------------------------------------------------------------------

static char s_mark_ids[MARK_BATCH_MAX][24];
static uint8_t s_mark_count;
static AppTimer *s_mark_timer;

//! Stream the watch last asked for; an incoming ItemCount + FeedNewest
//! updates that stream's last-seen (NEW-dot support).
static char s_fetch_stream[48];

//! Send the queued ids as one CSV MarkRead payload, then reset the queue.
static void mark_send_batch(void) {
  if (s_mark_count == 0) {
    s_mark_timer = NULL;
    return;
  }
  char csv[MARK_BATCH_MAX * 24]; // 12 x 23 chars + 11 commas + NUL
  size_t l = 0;
  for (uint8_t i = 0; i < s_mark_count && l + 1 < sizeof(csv); i++) {
    if (i > 0 && l + 1 < sizeof(csv)) {
      csv[l++] = ',';
    }
    size_t k = 0;
    while (k + 1 < sizeof(csv) - l && s_mark_ids[i][k]) {
      csv[l++] = s_mark_ids[i][k++];
    }
  }
  csv[l] = '\0';
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_MarkRead, csv);
    dict_write_end(iter);
    app_message_outbox_send();
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Mark-read batch dropped (outbox busy)");
  }
  s_mark_count = 0;
  s_mark_timer = NULL;
}

static void mark_flush_cb(void *data) {
  mark_send_batch();
}

//! Queue one article id; flush when the batch is full or the timer fires.
void proto_mark_push(const char *id) {
  if (!id || !id[0] || s_mark_count >= MARK_BATCH_MAX) {
    if (s_mark_count >= MARK_BATCH_MAX) {
      mark_send_batch();
    }
    return;
  }
  copy_str(s_mark_ids[s_mark_count++], sizeof(s_mark_ids[0]), id);
  if (s_mark_count >= MARK_BATCH_MAX) {
    if (s_mark_timer) {
      app_timer_cancel(s_mark_timer);
      s_mark_timer = NULL;
    }
    mark_send_batch();
  } else if (!s_mark_timer) {
    s_mark_timer = app_timer_register(MARK_FLUSH_MS, mark_flush_cb, NULL);
  }
}

//! Force-flush any queued ids (window close / app exit).
void proto_flush_now(void) {
  if (s_mark_timer) {
    app_timer_cancel(s_mark_timer);
    s_mark_timer = NULL;
  }
  mark_send_batch();
}

// ---------------------------------------------------------------------------
// Watch -> phone senders
// ---------------------------------------------------------------------------

//! Ask the phone to (re)fetch the feed tree and stream it back.
void proto_request_tree(void) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_OK) {
    dict_write_int32(iter, MESSAGE_KEY_FetchTree, 1);
    res = app_message_outbox_send();
  }
  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send FetchTree (%d)", (int)res);
  } else {
    APP_LOG(APP_LOG_LEVEL_INFO, "startup: tree requested");
  }
}

//! Request one page of a stream; cont "" asks for the first page.
void proto_request_items(const char *stream, const char *cont) {
  if (stream && stream[0]) {
    copy_str(s_fetch_stream, sizeof(s_fetch_stream), stream);
  }
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_OK) {
    dict_write_int32(iter, MESSAGE_KEY_FetchItems, 1);
    dict_write_cstring(iter, MESSAGE_KEY_ItemStream, stream);
    dict_write_cstring(iter, MESSAGE_KEY_ItemCont, cont ? cont : "");
    dict_write_int32(iter, MESSAGE_KEY_FetchN, PAGE_SIZE);
    dict_write_int32(iter, MESSAGE_KEY_UnreadOnly, s_unread_only ? 1 : 0);
    res = app_message_outbox_send();
  }
  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send FetchItems (%d)", (int)res);
  }
}

//! Ask the phone for account info (Connection screen); the reply arrives as
//! ResultCode/ResultText with a multiline "Account: ..." block.
void proto_request_user_info(void) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_OK) {
    dict_write_int32(iter, MESSAGE_KEY_FetchUserInfo, 1);
    res = app_message_outbox_send();
  }
  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send FetchUserInfo (%d)", (int)res);
  }
}

//! Toggle the star flag of one article on the server.
void proto_star(const char *id, bool on) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_StarItem, id ? id : "");
    dict_write_int32(iter, MESSAGE_KEY_StarOn, on ? 1 : 0);
    res = app_message_outbox_send();
  }
  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send StarItem (%d)", (int)res);
  }
}

//! Mark every article of a stream as read (stream id from the tree).
void proto_mark_all_read(const char *stream) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_MarkAllRead, stream ? stream : "");
    res = app_message_outbox_send();
  }
  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send MarkAllRead (%d)", (int)res);
  }
}

//! Ask the phone for the full summary of one article. The reply streams
//! back as FullSummary chunks (+ SummaryLast on the final one) which
//! proto_handle_inbox hands to the timeline reader. `id` is the article's
//! decimal microsecond id (as kept in Article.id).
void proto_request_summary(const char *id) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_FetchSummary, id ? id : "");
    res = app_message_outbox_send();
  }
  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send FetchSummary (%d)", (int)res);
  }
}

//! Toggle one article back to unread on the server (removes the read tag).
//! `id` is the article's decimal microsecond id.
void proto_mark_unread(const char *id) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_MarkUnread, id ? id : "");
    res = app_message_outbox_send();
  }
  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send MarkUnread (%d)", (int)res);
  }
}

//! Answer the phone's RequestConfig with the durable watch-bound settings.
//! The connection fields (server/user/password) are phone-side only and
//! never round-trip (agreed with the JS agent). The smart-surface toggles
//! are reported W->P too (the JS may ignore them, like the other replies).
void proto_reply_config(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_int32(iter, MESSAGE_KEY_AccentColor, (int32_t)s_accent.argb);
    dict_write_int32(iter, MESSAGE_KEY_DarkMode, s_dark ? 1 : 0);
    dict_write_int32(iter, MESSAGE_KEY_TouchEnabled, s_touch ? 1 : 0);
    dict_write_int32(iter, MESSAGE_KEY_ImportantRow, setting_important() ? 1 : 0);
    dict_write_int32(iter, MESSAGE_KEY_TriageDrain, setting_triage() ? 1 : 0);
    dict_write_int32(iter, MESSAGE_KEY_NewDot, setting_newdot() ? 1 : 0);
    dict_write_int32(iter, MESSAGE_KEY_ProgressLine, setting_progress() ? 1 : 0);
    dict_write_end(iter);
    app_message_outbox_send();
  }
}

// ---------------------------------------------------------------------------
// Phone -> watch dispatch (launcher chain style: first matching key wins)
// ---------------------------------------------------------------------------

void proto_handle_inbox(DictionaryIterator *iter) {
  Tuple *t;

  // Generic result/error channel. Copy the text out of the inbox buffer:
  // ui_result can build a dialog (allocations) and the inbox pointer must
  // not outlive the callback handling.
  if ((t = dict_find(iter, MESSAGE_KEY_ResultCode))) {
    int32_t code = t->value->int32;
    APP_LOG(APP_LOG_LEVEL_INFO, "startup: result %ld", (long)code);
    char text_buf[96];
    Tuple *text_t = dict_find(iter, MESSAGE_KEY_ResultText);
    copy_str(text_buf, sizeof(text_buf),
             text_t ? text_t->value->cstring : "");
    ui_result(code, text_buf);
    return;
  }

  // Tree stream: FeedCount announces the node count, then one message per
  // node carries FeedType + FeedId + FeedName + FeedUnread + FeedParent.
  if ((t = dict_find(iter, MESSAGE_KEY_FeedCount))) {
    APP_LOG(APP_LOG_LEVEL_INFO, "startup: tree count %ld",
            (long)t->value->int32);
    tree_begin_collect(t->value->int32);
    return;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_FeedType))) {
    const char *id = "", *name = "", *parent = "";
    int32_t kind = t->value->int32;
    int32_t unread = 0;
    int64_t newest = 0;
    if ((t = dict_find(iter, MESSAGE_KEY_FeedId))) {
      id = t->value->cstring;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_FeedName))) {
      name = t->value->cstring;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_FeedUnread))) {
      unread = t->value->int32;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_FeedNewest))) {
      // Decimal microseconds string; 0/absent when the feed has no items.
      newest = parse_decimal(t->value->cstring);
    }
    if ((t = dict_find(iter, MESSAGE_KEY_FeedParent))) {
      parent = t->value->cstring;
    }
    tree_collect_node(kind, id, name, unread, parent, newest);
    return;
  }

  // Item page: ItemCount announces the page (carrying FeedNewest, the newest
  // article timestampUsec of the page), then one message per article
  // (ItemTitle present), then a final ItemCont with the next continuation.
  if ((t = dict_find(iter, MESSAGE_KEY_ItemCount))) {
    // Opening a stream stores its last-seen: seconds = FeedNewest / 1000000.
    // The NEW-dot then compares the tree's per-feed newest against this.
    Tuple *newest_t = dict_find(iter, MESSAGE_KEY_FeedNewest);
    if (newest_t) {
      int64_t usec = parse_decimal(newest_t->value->cstring);
      if (usec > 0) {
        storage_feed_set_last_seen(s_fetch_stream, div_million(usec));
      }
    }
    timeline_page_begin(t->value->int32);
    return;
  }
  bool item = false;
  if ((t = dict_find(iter, MESSAGE_KEY_ItemTitle))) {
    timeline_collect_article(iter);
    item = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_ItemCont))) {
    timeline_page_end(t->value->cstring);
    return;
  }
  if (item) {
    return;
  }

  // Full summary stream (fetch-by-id reply): the phone sends FullSummary
  // text chunks with SummaryLast=1 on the final chunk. The chunk is handed
  // to the timeline reader directly, NOT copied — the inbox buffer stays
  // alive for the whole callback and the reader copies into its own heap
  // buffer. Shallow-stack diet: no copy_str/snprintf on this path.
  if ((t = dict_find(iter, MESSAGE_KEY_FullSummary))) {
    bool last = dict_find(iter, MESSAGE_KEY_SummaryLast) != NULL;
    timeline_full_summary_chunk(t->value->cstring, last);
    return;
  }
  // SummaryLast alone (finalize an empty/errored fetch): close the reader's
  // buffer so the preview/partial text settles.
  if ((t = dict_find(iter, MESSAGE_KEY_SummaryLast))) {
    timeline_full_summary_chunk("", true);
    return;
  }

  // Settings from Clay: the phone delivers every watch-bound key in one
  // message; persist, re-theme and re-arm touch navigation.
  bool settings = false;
  if ((t = dict_find(iter, MESSAGE_KEY_AccentColor))) {
    // Clay delivers a 24-bit RGB int (launcher convention); the argb
    // truncation bug turned every non-default accent into gray.
    s_accent = GColorFromHEX((uint32_t)t->value->int32);
    settings = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_DarkMode))) {
    s_dark = t->value->int32 != 0;
    settings = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_TouchEnabled))) {
    s_touch = t->value->int32 != 0;
    settings = true;
  }
  if (settings) {
    storage_save_settings();
    apply_settings();
    (void)app_touch_navigation_enable(s_touch);
    return;
  }

  // Highlight words from Clay: persist the new list and re-layout an open
  // reader so the change applies to the current article immediately.
  if ((t = dict_find(iter, MESSAGE_KEY_HighlightWords))) {
    storage_highlight_set_words(t->value->cstring);
    timeline_highlight_words_changed();
    return;
  }

  // Config request from the JS (on 'ready'): reply with the durable copy.
  if (dict_find(iter, MESSAGE_KEY_RequestConfig)) {
    proto_reply_config();
    return;
  }

  APP_LOG(APP_LOG_LEVEL_INFO, "Unrecognized AppMessage payload");
}

//! Bounded decimal parse for the FeedNewest microseconds string. Deliberately
//! NOT libc strtoll: newlib's strtoll/_strtoll_l have deep stack frames, and
//! the app stack is only 2 KB on basalt-class platforms — the parse runs
//! inside the AppMessage inbox callback, whose chain already overflows.
static int64_t parse_decimal(const char *s) {
  if (!s) {
    return 0;
  }
  int64_t v = 0;
  bool neg = false;
  if (*s == '-') {
    neg = true;
    s++;
  }
  while (*s >= '0' && *s <= '9') {
    int64_t nv = v * 10 + (*s - '0');
    if (nv < v) {
      return neg ? INT64_MIN : INT64_MAX; // clamp on overflow
    }
    v = nv;
    s++;
  }
  return neg ? -v : v;
}

//! Bounded string copy with forced NUL. Replaces snprintf("%s") in the
//! inbox path: newlib's vfprintf machinery is the deepest frame there.
static void copy_str(char *dst, size_t cap, const char *src) {
  if (!cap) {
    return;
  }
  if (!src) {
    src = "";
  }
  size_t i = 0;
  while (i + 1 < cap && src[i]) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

// ---------------------------------------------------------------------------
// AppMessage lifecycle
// ---------------------------------------------------------------------------

static void inbox_received(DictionaryIterator *iter, void *context) {
  proto_handle_inbox(iter);
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage inbox dropped (%d)", (int)reason);
}

static void outbox_sent(DictionaryIterator *iter, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "AppMessage sent");
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason,
                          void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage outbox failed (%d)", (int)reason);
  // Surface the failure only when the user is waiting on a dialog.
  if (ui_result_active()) {
    ui_result(1, "Send failed");
  }
}

//! Register handlers and open the buffers (launcher-verified sizes).
void proto_init(void) {
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(4096, 1024);
}
