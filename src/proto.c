#include <pebble.h>

#include "proto.h"
#include "storage.h"
#include "tree.h"
#include "timeline.h"
#include "common.h"

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

//! Send the queued ids as one CSV MarkRead payload, then reset the queue.
static void mark_send_batch(void) {
  if (s_mark_count == 0) {
    s_mark_timer = NULL;
    return;
  }
  char csv[MARK_BATCH_MAX * 24]; // 12 x 23 chars + 11 commas + NUL
  csv[0] = '\0';
  for (uint8_t i = 0; i < s_mark_count; i++) {
    size_t l = strlen(csv);
    snprintf(csv + l, sizeof(csv) - l, "%s%s", i ? "," : "", s_mark_ids[i]);
  }
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
  snprintf(s_mark_ids[s_mark_count++], sizeof(s_mark_ids[0]), "%s", id);
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
  }
}

//! Request one page of a stream; cont "" asks for the first page.
void proto_request_items(const char *stream, const char *cont) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_OK) {
    dict_write_int32(iter, MESSAGE_KEY_FetchItems, 1);
    dict_write_cstring(iter, MESSAGE_KEY_ItemStream, stream);
    dict_write_cstring(iter, MESSAGE_KEY_ItemCont, cont ? cont : "");
    dict_write_int32(iter, MESSAGE_KEY_FetchN, PAGE_SIZE);
    res = app_message_outbox_send();
  }
  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send FetchItems (%d)", (int)res);
  }
}

//! Ask for the stripped HTML summary of one article.
void proto_request_summary(const char *id) {
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_OK) {
    dict_write_int32(iter, MESSAGE_KEY_FetchSummary, 1);
    dict_write_cstring(iter, MESSAGE_KEY_ItemId, id ? id : "");
    res = app_message_outbox_send();
  }
  if (res != APP_MSG_OK) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send FetchSummary (%d)", (int)res);
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

//! Answer the phone's RequestConfig with the durable watch-bound settings.
//! The connection fields (server/user/password) are phone-side only and
//! never round-trip (agreed with the JS agent).
void proto_reply_config(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_int32(iter, MESSAGE_KEY_AccentColor, (int32_t)s_accent.argb);
    dict_write_int32(iter, MESSAGE_KEY_DarkMode, s_dark ? 1 : 0);
    dict_write_int32(iter, MESSAGE_KEY_TouchEnabled, s_touch ? 1 : 0);
    dict_write_int32(iter, MESSAGE_KEY_MarkOnOpenList, s_mark_list ? 1 : 0);
    dict_write_int32(iter, MESSAGE_KEY_MarkOnOpenDetail, s_mark_detail ? 1 : 0);
    dict_write_end(iter);
    app_message_outbox_send();
  }
}

// ---------------------------------------------------------------------------
// Phone -> watch dispatch (launcher chain style: first matching key wins)
// ---------------------------------------------------------------------------

void proto_handle_inbox(DictionaryIterator *iter) {
  Tuple *t;

  // Generic result/error channel.
  if ((t = dict_find(iter, MESSAGE_KEY_ResultCode))) {
    int32_t code = t->value->int32;
    Tuple *text_t = dict_find(iter, MESSAGE_KEY_ResultText);
    ui_result(code, text_t ? text_t->value->cstring : "");
    return;
  }

  // Tree stream: FeedCount announces the node count, then one message per
  // node carries FeedType + FeedId + FeedName + FeedUnread + FeedParent.
  if ((t = dict_find(iter, MESSAGE_KEY_FeedCount))) {
    tree_begin_collect(t->value->int32);
    return;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_FeedType))) {
    const char *id = "", *name = "", *parent = "";
    int32_t kind = t->value->int32;
    int32_t unread = 0;
    if ((t = dict_find(iter, MESSAGE_KEY_FeedId))) {
      id = t->value->cstring;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_FeedName))) {
      name = t->value->cstring;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_FeedUnread))) {
      unread = t->value->int32;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_FeedParent))) {
      parent = t->value->cstring;
    }
    tree_collect_node(kind, id, name, unread, parent);
    return;
  }

  // Item page: ItemCount announces the page, then one message per article
  // (ItemTitle present), then a final ItemCont with the next continuation.
  if ((t = dict_find(iter, MESSAGE_KEY_ItemCount))) {
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

  // Summary response: ItemSummary + ItemId echo.
  if ((t = dict_find(iter, MESSAGE_KEY_ItemSummary))) {
    timeline_summary_arrived(iter);
    return;
  }

  // Settings from Clay: the phone delivers every watch-bound key in one
  // message; persist, re-theme and re-arm touch navigation.
  bool settings = false;
  if ((t = dict_find(iter, MESSAGE_KEY_AccentColor))) {
    s_accent = (GColor){ .argb = (uint8_t)t->value->int32 };
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
  if ((t = dict_find(iter, MESSAGE_KEY_MarkOnOpenList))) {
    s_mark_list = t->value->int32 != 0;
    settings = true;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_MarkOnOpenDetail))) {
    s_mark_detail = t->value->int32 != 0;
    settings = true;
  }
  if (settings) {
    storage_save_settings();
    apply_settings();
    (void)app_touch_navigation_enable(s_touch);
    return;
  }

  // Config request from the JS (on 'ready'): reply with the durable copy.
  if (dict_find(iter, MESSAGE_KEY_RequestConfig)) {
    proto_reply_config();
    return;
  }

  APP_LOG(APP_LOG_LEVEL_INFO, "Unrecognized AppMessage payload");
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
