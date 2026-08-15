/**
 * HeadeRSS — PebbleKit JavaScript.
 *
 * Lifecycle, config handling and AppMessage dispatch for the FreshRSS feed
 * reader. ES5 only (the phone's old JS engine); HTTP runs through
 * ./freshrss.js; streaming results are chunked one entry per AppMessage,
 * chained on outbox ack (launcher pattern) with generation counters so a
 * stale chain is dropped when a newer fetch supersedes it.
 *
 * Wire protocol (messageKeys in package.json):
 *   watch->phone: FetchTree | FetchItems(+ItemStream/ItemCont/FetchN) |
 *     MarkRead (CSV ids) | StarItem(+StarOn) | MarkAllRead | FetchSummary
 *     (+ItemId) | RequestConfig
 *   phone->watch: ResultCode/ResultText (0 = success), FeedCount + per-node
 *     FeedType/FeedId/FeedName/FeedUnread/FeedParent, ItemCount + per-item
 *     ItemId/ItemTitle/ItemFeed/ItemFeedId/ItemTime/ItemRead/ItemStar,
 *     ItemCont (page continuation), ItemSummary(+ItemId echo).
 *
 * Clay auto-handles the config webview: on save it writes 'clay-settings'
 * for page prefill and sends every watch-bound messageKey value (AccentColor,
 * DarkMode, TouchEnabled, MarkOnOpenList, MarkOnOpenDetail) to the WATCH via
 * AppMessage; the watch persists them durably. The connection fields
 * (ServerUrl, User, ApiPass) are phone-side only — 'clay-settings' is the
 * prefill cache and 'headerssConfig' (CONFIG_KEY) our working copy; on
 * 'ready' we pull the watch's durable settings back and rewrite the prefill.
 */

var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var messageKeys = require('message_keys');
var freshrss = require('./freshrss');

var CONFIG_KEY = 'headerssConfig';
var CLAY_SETTINGS_KEY = 'clay-settings';
var TREE_FLOW_TIMEOUT_MS = 25000; // whole-tree guard; XHRs themselves time out at 15s
var DEFAULT_FETCH_N = 50;

// Clay handles showConfiguration/webviewclosed itself (the documented
// default): on save it normalizes the response ({value: ...} wrapping),
// writes 'clay-settings' for page prefill, and sends every watch-bound
// messageKey value to the WATCH via AppMessage. The watch persists them
// durably in its flash; this JS pulls the durable copy back on 'ready'.
// No manual event handling needed.
var clay = new Clay(clayConfig);

// Monotonic generation counters for the streaming flows. If a flow is
// re-triggered while a previous chain is still streaming, the stale chain
// is dropped instead of interleaving with the newer one.
var treeGeneration = 0;
var itemsGeneration = 0;

/**
 * Normalize a stored server URL: trim whitespace, strip a stray trailing
 * '/api/greader.php' suffix and a single trailing slash.
 * @param {string} url
 * @return {string}
 */
function normalizeBaseUrl(url) {
  if (!url) {
    return '';
  }
  url = String(url).trim();
  var suffix = '/api/greader.php';
  if (url.length > suffix.length && url.slice(-suffix.length) === suffix) {
    url = url.slice(0, url.length - suffix.length);
  }
  if (url.charAt(url.length - 1) === '/') {
    url = url.slice(0, -1);
  }
  return url;
}

/**
 * Load the phone-side configuration from localStorage.
 * @return {{serverUrl: string, user: string, apiPass: string}}
 */
function loadConfig() {
  var config = { serverUrl: '', user: '', apiPass: '' };
  try {
    var raw = localStorage.getItem(CONFIG_KEY);
    if (raw) {
      var parsed = JSON.parse(raw);
      config.serverUrl = normalizeBaseUrl(parsed.serverUrl || '');
      config.user = parsed.user || '';
      config.apiPass = parsed.apiPass || '';
    }
  } catch (err) {
    console.log('loadConfig: failed to read stored config: ' + err);
  }
  return config;
}

/**
 * Persist the phone-side configuration to localStorage.
 * @param {{serverUrl: string, user: string, apiPass: string}} config
 */
function saveConfig(config) {
  try {
    localStorage.setItem(CONFIG_KEY, JSON.stringify(config));
  } catch (err) {
    console.log('saveConfig: failed to persist config: ' + err);
  }
}

/**
 * Build a client from the stored config, or null when not configured.
 * @return {Object|null}
 */
function makeClient() {
  var config = loadConfig();
  if (!config.serverUrl || !config.user || !config.apiPass) {
    return null;
  }
  return freshrss.createClient(config.serverUrl, config.user, config.apiPass);
}

/**
 * Send a result to the watch over ResultCode/ResultText.
 * @param {number} code 0 = success, nonzero = failure
 * @param {string} text
 */
function sendResult(code, text) {
  var dict = {};
  dict.ResultCode = code;
  dict.ResultText = text;
  Pebble.sendAppMessage(dict, function () {
    console.log('sendResult: ' + code + ' ' + text);
  }, function (err) {
    console.log('sendResult: failed to deliver (' + code + '): ' + JSON.stringify(err));
  });
}

/**
 * Fetch the feed tree: auth, then getTree, then stream it to the watch
 * (FeedCount first, then one Feed* node per message, chained on ack).
 * A 25s guard reports failure if the HTTP phase stalls.
 */
function treeFlow() {
  var client = makeClient();
  if (!client) {
    sendResult(1, 'Set server in phone settings');
    return;
  }
  var generation = ++treeGeneration;
  var timedOut = false;
  var guard = setTimeout(function () {
    if (generation !== treeGeneration) {
      return; // superseded by a newer fetch
    }
    timedOut = true;
    sendResult(2, 'Tree timeout');
  }, TREE_FLOW_TIMEOUT_MS);

  client.ensureAuth(function (authErr) {
    if (generation !== treeGeneration) {
      clearTimeout(guard);
      return;
    }
    if (authErr) {
      clearTimeout(guard);
      if (!timedOut) {
        sendResult(authErr.code, authErr.text);
      }
      return;
    }
    client.getTree(function (err, nodes) {
      clearTimeout(guard);
      if (generation !== treeGeneration || timedOut) {
        return; // stale chain or the guard already reported
      }
      if (err) {
        sendResult(err.code, err.text);
        return;
      }
      sendResult(0, 'OK');
      sendTreeNodes(nodes, generation);
    });
  });
}

/**
 * Send the tree: {FeedCount: n} first, then one node per message, chained
 * through the ack callback to respect the app message queue.
 * @param {Array<Object>} nodes
 * @param {number} generation - tree generation; a stale chain aborts
 */
function sendTreeNodes(nodes, generation) {
  if (generation !== treeGeneration) {
    console.log('tree: stale chain dropped');
    return;
  }
  var dict = {};
  dict.FeedCount = nodes.length;
  Pebble.sendAppMessage(dict, function () {
    sendTreeNode(nodes, 0, generation);
  }, function (err) {
    console.log('tree: failed to send FeedCount: ' + JSON.stringify(err));
  });
}

/**
 * Send one tree node, then the next, chained on success.
 * @param {Array<Object>} nodes
 * @param {number} index
 * @param {number} generation - tree generation; a stale chain aborts
 */
function sendTreeNode(nodes, index, generation) {
  if (generation !== treeGeneration || index >= nodes.length) {
    return;
  }
  var node = nodes[index];
  var dict = {};
  dict.FeedType = node.type;
  dict.FeedId = node.id;
  dict.FeedName = node.name;
  dict.FeedUnread = node.unread;
  dict.FeedParent = node.parent;
  Pebble.sendAppMessage(dict, function () {
    sendTreeNode(nodes, index + 1, generation);
  }, function (err) {
    console.log('tree: failed to send node ' + node.id + ': ' + JSON.stringify(err));
  });
}

/**
 * Fetch one item page. On any failure the watch is unblocked with
 * {ItemCont: ''} plus a ResultCode/ResultText error.
 */
function itemsFlow(stream, cont, n) {
  var client = makeClient();
  if (!client) {
    sendItemCont('', itemsGeneration);
    sendResult(1, 'Set server in phone settings');
    return;
  }
  var generation = ++itemsGeneration;
  var timedOut = false;
  var guard = setTimeout(function () {
    if (generation !== itemsGeneration) {
      return; // superseded by a newer fetch
    }
    timedOut = true;
    sendItemCont('', generation);
    sendResult(2, 'Items timeout');
  }, TREE_FLOW_TIMEOUT_MS);

  client.ensureAuth(function (authErr) {
    if (generation !== itemsGeneration) {
      clearTimeout(guard);
      return;
    }
    if (authErr) {
      clearTimeout(guard);
      if (timedOut) {
        return;
      }
      sendItemCont('', generation);
      sendResult(authErr.code, authErr.text);
      return;
    }
    client.getItems(stream, cont, n, function (err, data) {
      clearTimeout(guard);
      if (generation !== itemsGeneration || timedOut) {
        return; // stale chain or the guard already reported
      }
      if (err) {
        sendItemCont('', generation);
        sendResult(err.code, err.text);
        return;
      }
      sendItems(data.items, data.continuation, generation);
    });
  });
}

/**
 * Send one item page: {ItemCount: n} first, then one item per message,
 * chained through the ack callback, then {ItemCont: continuation} so the
 * watch knows the page is complete.
 * @param {Array<Object>} items
 * @param {string} continuation
 * @param {number} generation - items generation; a stale chain aborts
 */
function sendItems(items, continuation, generation) {
  if (generation !== itemsGeneration) {
    return;
  }
  var dict = {};
  dict.ItemCount = items.length;
  Pebble.sendAppMessage(dict, function () {
    sendItemChain(items, 0, continuation, generation);
  }, function (err) {
    console.log('items: failed to send ItemCount: ' + JSON.stringify(err));
  });
}

/**
 * Send one item, then the next, chained on success; the continuation is
 * sent after the last item.
 * @param {Array<Object>} items
 * @param {number} index
 * @param {string} continuation
 * @param {number} generation - items generation; a stale chain aborts
 */
function sendItemChain(items, index, continuation, generation) {
  if (generation !== itemsGeneration) {
    return;
  }
  if (index >= items.length) {
    sendItemCont(continuation, generation);
    return;
  }
  var item = items[index];
  var dict = {};
  dict.ItemId = item.id;
  dict.ItemTitle = item.title;
  dict.ItemFeed = item.feed;
  dict.ItemFeedId = item.feedId;
  dict.ItemTime = item.time;
  dict.ItemRead = item.read;
  dict.ItemStar = item.star;
  Pebble.sendAppMessage(dict, function () {
    sendItemChain(items, index + 1, continuation, generation);
  }, function (err) {
    console.log('items: failed to send item ' + item.id + ': ' + JSON.stringify(err));
  });
}

/**
 * Send the page continuation token ('' = no more pages).
 * @param {string} cont
 * @param {number} generation - items generation; a stale chain aborts
 */
function sendItemCont(cont, generation) {
  if (generation !== itemsGeneration) {
    return;
  }
  var dict = {};
  dict.ItemCont = cont || '';
  Pebble.sendAppMessage(dict, function () {
    // best effort
  }, function (err) {
    console.log('items: failed to send ItemCont: ' + JSON.stringify(err));
  });
}

/**
 * Mark a CSV list of item ids read, then report the result.
 * @param {string} csv - comma-separated decimal µs ids (<= 12 per message)
 */
function markReadFlow(csv) {
  var client = makeClient();
  if (!client) {
    sendResult(1, 'Set server in phone settings');
    return;
  }
  var parts = String(csv || '').split(',');
  var ids = [];
  for (var i = 0; i < parts.length; i++) {
    var id = parts[i].trim();
    if (id) {
      ids.push(id);
    }
  }
  client.markRead(ids, function (err) {
    if (err) {
      sendResult(err.code, err.text);
      return;
    }
    sendResult(0, 'OK');
  });
}

/**
 * Star or unstar one item, then report the result.
 * @param {string} id
 * @param {number} on - 1 = star, 0 = unstar
 */
function starFlow(id, on) {
  var client = makeClient();
  if (!client) {
    sendResult(1, 'Set server in phone settings');
    return;
  }
  client.star(id, on ? 1 : 0, function (err) {
    if (err) {
      sendResult(err.code, err.text);
      return;
    }
    sendResult(0, 'OK');
  });
}

/**
 * Mark an entire stream read, then report the result.
 * @param {string} stream - 'feed/N' | 'user/-/label/...' | reading-list | starred
 */
function markAllReadFlow(stream) {
  var client = makeClient();
  if (!client) {
    sendResult(1, 'Set server in phone settings');
    return;
  }
  client.markAllRead(stream, function (err) {
    if (err) {
      sendResult(err.code, err.text);
      return;
    }
    sendResult(0, 'OK');
  });
}

/**
 * Fetch an item summary and send it back with the id echoed.
 * @param {string} id
 */
function summaryFlow(id) {
  var client = makeClient();
  if (!client) {
    sendResult(1, 'Set server in phone settings');
    return;
  }
  client.getSummary(id, function (err, summary) {
    if (err) {
      sendResult(err.code, err.text);
      return;
    }
    var dict = {};
    dict.ItemSummary = summary;
    dict.ItemId = id;
    Pebble.sendAppMessage(dict, function () {
      console.log('summary: sent summary for ' + id);
    }, function (sendErr) {
      console.log('summary: failed to send: ' + JSON.stringify(sendErr));
    });
  });
}

/**
 * Read a payload field by messageKey name, tolerating both the string-name
 * form (multi-JS) and the numeric-key form.
 * @param {Object} payload
 * @param {string} keyName
 * @return {*}
 */
function payloadValue(payload, keyName) {
  if (payload[keyName] !== undefined) {
    return payload[keyName];
  }
  return payload[messageKeys[keyName]];
}

/**
 * Recover the config from Clay's persisted settings (written by Clay's own
 * getSettings when a save is delivered). Acts as a prefill/cache layer for
 * the phone-side connection fields; the watch's flash is the source of
 * truth for the watch-bound settings.
 */
function importClaySettings() {
  try {
    var raw = localStorage.getItem(CLAY_SETTINGS_KEY);
    if (!raw) {
      return;
    }
    var s = JSON.parse(raw);
    function plain(k) {
      var v = s[k];
      return (v && typeof v === 'object' && 'value' in v) ? v.value : v;
    }
    var cfg = loadConfig();
    var changed = false;
    if (plain('ServerUrl')) {
      cfg.serverUrl = normalizeBaseUrl(plain('ServerUrl'));
      changed = true;
    }
    if (plain('User')) {
      cfg.user = plain('User');
      changed = true;
    }
    if (plain('ApiPass')) {
      cfg.apiPass = plain('ApiPass');
      changed = true;
    }
    if (changed) {
      saveConfig(cfg);
      console.log('ready: recovered config from clay-settings');
    }
  } catch (err) {
    console.log('ready: clay-settings import failed: ' + err);
  }
}

/**
 * Config reply from the watch (its durable copy of the Clay-bound settings,
 * triggered by our {RequestConfig: 1} on 'ready'). Rewrite the
 * 'clay-settings' prefill so the next Clay open reflects what the watch
 * actually has, keeping the phone-side connection fields.
 * @param {Object} payload
 */
function handleConfigReply(payload) {
  var cfg = loadConfig();
  var settings = {
    ServerUrl: cfg.serverUrl,
    User: cfg.user,
    ApiPass: cfg.apiPass
  };
  function copyIn(key) {
    var v = payloadValue(payload, key);
    if (v !== undefined && v !== null) {
      settings[key] = v;
    }
  }
  copyIn('AccentColor');
  copyIn('DarkMode');
  copyIn('TouchEnabled');
  copyIn('MarkOnOpenList');
  copyIn('MarkOnOpenDetail');
  try {
    localStorage.setItem(CLAY_SETTINGS_KEY, JSON.stringify(settings));
  } catch (err) {
    // best effort — the prefill is only a cache
  }
  console.log('appmessage: config from watch saved');
}

Pebble.addEventListener('ready', function () {
  console.log('JS ready');
  importClaySettings();
  // Pull the durable watch-bound settings from the watch (it persists what
  // Clay sent) and rewrite the 'clay-settings' prefill from the reply.
  var msg = {};
  msg.RequestConfig = 1;
  Pebble.sendAppMessage(msg, function () {
    console.log('ready: requested config from watch');
  }, function (err) {
    console.log('ready: config request failed: ' + JSON.stringify(err));
  });
});

Pebble.addEventListener('appmessage', function (e) {
  var payload = e.payload || {};

  // Config reply from the watch (its durable copy of what Clay sent it).
  // AccentColor is the discriminator — no other watch->phone message
  // carries it.
  var accent = payloadValue(payload, 'AccentColor');
  if (accent !== undefined && accent !== null) {
    handleConfigReply(payload);
    return;
  }

  var fetchTree = payloadValue(payload, 'FetchTree');
  if (fetchTree !== undefined && fetchTree !== null && fetchTree !== 0) {
    console.log('appmessage: fetching tree');
    treeFlow();
    return;
  }

  var fetchItems = payloadValue(payload, 'FetchItems');
  if (fetchItems !== undefined && fetchItems !== null && fetchItems !== 0) {
    var stream = payloadValue(payload, 'ItemStream');
    var cont = payloadValue(payload, 'ItemCont');
    var fetchN = payloadValue(payload, 'FetchN');
    console.log('appmessage: fetching items');
    itemsFlow(
      (stream === undefined || stream === null) ? '' : String(stream),
      (cont === undefined || cont === null) ? '' : String(cont),
      (fetchN === undefined || fetchN === null) ? DEFAULT_FETCH_N : fetchN
    );
    return;
  }

  var markRead = payloadValue(payload, 'MarkRead');
  if (markRead !== undefined && markRead !== null && markRead !== '') {
    console.log('appmessage: marking read');
    markReadFlow(String(markRead));
    return;
  }

  var starItem = payloadValue(payload, 'StarItem');
  if (starItem !== undefined && starItem !== null && starItem !== '') {
    var starOn = payloadValue(payload, 'StarOn');
    console.log('appmessage: starring');
    starFlow(String(starItem), starOn ? 1 : 0);
    return;
  }

  var markAll = payloadValue(payload, 'MarkAllRead');
  if (markAll !== undefined && markAll !== null && markAll !== '') {
    console.log('appmessage: marking all read');
    markAllReadFlow(String(markAll));
    return;
  }

  var fetchSummary = payloadValue(payload, 'FetchSummary');
  if (fetchSummary !== undefined && fetchSummary !== null && fetchSummary !== 0) {
    var itemId = payloadValue(payload, 'ItemId');
    if (itemId === undefined || itemId === null || itemId === '') {
      console.log('appmessage: summary request without ItemId');
      sendResult(1, 'Missing item id');
      return;
    }
    console.log('appmessage: fetching summary');
    summaryFlow(String(itemId));
    return;
  }

  console.log('appmessage: unrecognized payload');
});
