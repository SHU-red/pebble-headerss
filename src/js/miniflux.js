/**
 * HeadeRSS — Miniflux Google Reader API client.
 *
 * ES5-only, callback-style client matching freshrss.js' public interface:
 *   ensureAuth, getTree, getItems, getSummary,
 *   markRead, markUnread, star, markAllRead
 *
 * Miniflux compatibility notes:
 * - API root is BASE_URL (no /api/greader.php)
 * - GET auth uses Authorization: GoogleLogin auth=<token>
 * - POST auth uses form field T=<token>
 * - unread-count is not implemented by Miniflux
 * - GET stream/contents/<stream> is not implemented by Miniflux
 * - label streams are not accepted by stream/items/ids
 *
 * Consequently:
 * - tree totals are derived from stream/items/ids
 * - per-feed unread badges are left at 0 to avoid N extra HTTP calls on launch
 * - stream pages use IDs first, then POST stream/items/contents
 * - folder pages aggregate the IDs of feeds in that folder
 */
var DEFAULT_TIMEOUT_MS = 15000;

var READING_LIST = 'user/-/state/com.google/reading-list';
var STARRED = 'user/-/state/com.google/starred';
var READ_TAG = 'user/-/state/com.google/read';
var LABEL_PREFIX = 'user/-/label/';
var FALLBACK_LABEL = 'user/-/label/Uncategorized';

var AUTH_CACHE = {};

function makeError(code, text) {
  return { code: code, text: text };
}

function normalizeBaseUrl(url) {
  if (!url) {
    return '';
  }

  url = String(url).trim();

  if (!/^https?:\/\//i.test(url)) {
    var authority = url.split('/')[0];
    var host = authority;

    /* Strip a port for IPv4/hostname checks. IPv6 literals keep brackets. */
    if (host.charAt(0) !== '[') {
      host = host.split(':')[0];
    }

    var isLocal =
      host === 'localhost' ||
      /^127\./.test(host) ||
      /^10\./.test(host) ||
      /^192\.168\./.test(host) ||
      /^172\.(1[6-9]|2\d|3[01])\./.test(host) ||
      /^\[::1\]$/i.test(host) ||
      /^\[f[cd][0-9a-f]{2}:/i.test(host);

    url = (isLocal ? 'http://' : 'https://') + url;
  }

  while (url.charAt(url.length - 1) === '/') {
    url = url.slice(0, -1);
  }

  return url;
}

function stripHtml(html) {
  var s = String(html || '');
  s = s.replace(/<script[\s\S]*?<\/script>/gi, ' ');
  s = s.replace(/<style[\s\S]*?<\/style>/gi, ' ');
  s = s.replace(/<[^>]+>/g, ' ');
  s = s.replace(/&amp;/g, '&')
       .replace(/&lt;/g, '<')
       .replace(/&gt;/g, '>')
       .replace(/&quot;/g, '"')
       .replace(/&#39;/g, "'")
       .replace(/&nbsp;/g, ' ')
       .replace(/&#x27;/gi, "'");
  return s.replace(/\s+/g, ' ').trim();
}

function hasCategory(item, needle) {
  var cats = item && item.categories;
  if (!cats) {
    return false;
  }
  for (var i = 0; i < cats.length; i++) {
    if (String(cats[i]).indexOf(needle) !== -1) {
      return true;
    }
  }
  return false;
}

/* Miniflux emits user/<numeric-id>/label/Foo. Canonicalize to user/-/label/Foo
 * so the existing watch-side tree logic can treat FreshRSS and Miniflux alike.
 */
function canonicalLabel(labelId) {
  var s = String(labelId || '');
  var marker = '/label/';
  var i = s.indexOf(marker);
  if (i === -1) {
    return s;
  }
  return LABEL_PREFIX + s.slice(i + marker.length);
}

function lastLabelSegment(labelId) {
  var s = canonicalLabel(labelId);
  var i = s.lastIndexOf('/');
  return i === -1 ? s : s.slice(i + 1);
}

function parentLabel(labelId) {
  var s = canonicalLabel(labelId);
  if (s.indexOf(LABEL_PREFIX) !== 0) {
    return '';
  }
  var inner = s.slice(LABEL_PREFIX.length);
  var i = inner.lastIndexOf('/');
  if (i === -1) {
    return '';
  }
  return LABEL_PREFIX + inner.slice(0, i);
}

function authCacheKey(base, user) {
  return base + '\u0001' + user;
}

/* Convert a Google Reader long-form hex item ID to a decimal string without
 * BigInt, because PebbleKit JS must remain ES5-compatible.
 */
function hexToDecimalString(hex) {
  var digits = [0];
  var clean = String(hex || '').toLowerCase().replace(/^0+/, '') || '0';

  for (var i = 0; i < clean.length; i++) {
    var nibble = parseInt(clean.charAt(i), 16);
    if (isNaN(nibble)) {
      return '';
    }

    var carry = nibble;
    for (var j = digits.length - 1; j >= 0; j--) {
      var v = digits[j] * 16 + carry;
      digits[j] = v % 10;
      carry = Math.floor(v / 10);
    }
    while (carry > 0) {
      digits.unshift(carry % 10);
      carry = Math.floor(carry / 10);
    }
  }

  return digits.join('');
}

function itemDecimalId(item) {
  var id = String(item && item.id || '');
  var marker = 'tag:google.com,2005:reader/item/';
  if (id.indexOf(marker) === 0) {
    return hexToDecimalString(id.slice(marker.length));
  }

  if (/^[0-9]+$/.test(id)) {
    return id;
  }

  if (/^[0-9a-fA-F]{16}$/.test(id)) {
    return hexToDecimalString(id);
  }

  return '';
}

function createClient(baseUrl, username, apiPass, opts) {
  opts = opts || {};

  var base = normalizeBaseUrl(baseUrl);
  var user = String(username || '');
  var pass = String(apiPass || '');
  var token = AUTH_CACHE[authCacheKey(base, user)] || null;
  var subscriptionsCache = null;

  var requestFactory = opts.request || function () {
    return new XMLHttpRequest();
  };

  function login(cb) {
    var xhr = requestFactory();
    xhr.open('POST', base + '/accounts/ClientLogin', true);
    xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    xhr.timeout = DEFAULT_TIMEOUT_MS;

    xhr.onload = function () {
      if (xhr.status === 200) {
        var m = String(xhr.responseText || '').match(/Auth=([^\r\n]+)/);
        if (m && m[1]) {
          token = m[1];
          AUTH_CACHE[authCacheKey(base, user)] = token;
          cb(null, token);
          return;
        }
        cb(makeError(1, 'Login failed — bad Miniflux GReader response'));
        return;
      }

      if (xhr.status === 401) {
        cb(makeError(1, 'Login failed — check Google Reader credentials'));
        return;
      }

      cb(makeError(xhr.status, 'HTTP ' + xhr.status));
    };

    xhr.onerror = function () {
      cb(makeError(-1, 'Network error'));
    };

    xhr.ontimeout = function () {
      cb(makeError(-1, 'Timeout'));
    };

    xhr.send(
      'Email=' + encodeURIComponent(user) +
      '&Passwd=' + encodeURIComponent(pass)
    );
  }

  function ensureAuth(cb) {
    if (token) {
      cb(null, token);
      return;
    }
    login(cb);
  }

  function request(method, path, body, cb) {
    ensureAuth(function (authErr) {
      if (authErr) {
        cb(authErr);
        return;
      }
      doRequest(method, path, body, false, cb);
    });
  }

  function doRequest(method, path, body, retried, cb) {
    var xhr = requestFactory();
    var upper = String(method || 'GET').toUpperCase();
    var actualBody = body || '';

    xhr.open(upper, base + path, true);

    if (upper === 'GET') {
      xhr.setRequestHeader('Authorization', 'GoogleLogin auth=' + token);
    } else {
      xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
      actualBody =
        'T=' + encodeURIComponent(token) +
        (actualBody ? '&' + actualBody : '');
    }

    xhr.timeout = DEFAULT_TIMEOUT_MS;

    xhr.onload = function () {
      if (xhr.status === 401 && !retried) {
        token = null;
        AUTH_CACHE[authCacheKey(base, user)] = null;

        login(function (loginErr) {
          if (loginErr) {
            cb(loginErr);
            return;
          }
          doRequest(method, path, body, true, cb);
        });
        return;
      }

      if (xhr.status >= 200 && xhr.status < 300) {
        cb(null, {
          status: xhr.status,
          text: xhr.responseText
        });
        return;
      }

      cb(makeError(xhr.status, 'HTTP ' + xhr.status));
    };

    xhr.onerror = function () {
      cb(makeError(-1, 'Network error'));
    };

    xhr.ontimeout = function () {
      cb(makeError(-1, 'Timeout'));
    };

    xhr.send(upper === 'GET' ? null : actualBody);
  }

  function parseJsonResponse(resp, label, cb) {
    try {
      cb(null, JSON.parse(resp.text));
    } catch (e) {
      cb(makeError(2, 'Bad ' + label + ' response'));
    }
  }

  function getSubscriptions(cb) {
    if (subscriptionsCache) {
      cb(null, subscriptionsCache);
      return;
    }

    request(
      'GET',
      '/reader/api/0/subscription/list?output=json',
      null,
      function (err, resp) {
        if (err) {
          cb(err);
          return;
        }

        parseJsonResponse(resp, 'subscription list', function (parseErr, parsed) {
          if (parseErr) {
            cb(parseErr);
            return;
          }
          subscriptionsCache = (parsed && parsed.subscriptions) || [];
          cb(null, subscriptionsCache);
        });
      }
    );
  }

  function fetchIdsDirect(stream, cont, n, unreadOnly, cb) {
    var count = n || 50;
    var path =
      '/reader/api/0/stream/items/ids?output=json' +
      '&s=' + encodeURIComponent(String(stream)) +
      '&n=' + encodeURIComponent(String(count));

    if (cont) {
      path += '&c=' + encodeURIComponent(String(cont));
    }

    if (
      String(stream) === READING_LIST ||
      (unreadOnly && String(stream) !== STARRED)
    ) {
      path += '&xt=' + encodeURIComponent(READ_TAG);
    }

    request('GET', path, null, function (err, resp) {
      if (err) {
        cb(err);
        return;
      }

      parseJsonResponse(resp, 'item ids', function (parseErr, parsed) {
        if (parseErr) {
          cb(parseErr);
          return;
        }

        var refs = (parsed && parsed.itemRefs) || [];
        var ids = [];
        for (var i = 0; i < refs.length; i++) {
          if (refs[i] && refs[i].id !== undefined) {
            ids.push(String(refs[i].id));
          }
        }

        cb(null, {
          ids: ids,
          continuation: parsed && parsed.continuation
            ? String(parsed.continuation)
            : ''
        });
      });
    });
  }

  function fetchContents(ids, cb) {
    if (!ids || !ids.length) {
      cb(null, []);
      return;
    }

    var body = 'output=json';
    for (var i = 0; i < ids.length; i++) {
      body += '&i=' + encodeURIComponent(String(ids[i]));
    }

    request(
      'POST',
      '/reader/api/0/stream/items/contents',
      body,
      function (err, resp) {
        if (err) {
          cb(err);
          return;
        }

        parseJsonResponse(resp, 'item contents', function (parseErr, parsed) {
          if (parseErr) {
            cb(parseErr);
            return;
          }

          cb(null, (parsed && parsed.items) || []);
        });
      }
    );
  }

  function mapItem(item) {
    var summaryHtml = '';
    if (item && item.summary && item.summary.content) {
      summaryHtml = item.summary.content;
    } else if (item && item.content && item.content.content) {
      summaryHtml = item.content.content;
    }

    return {
      id: itemDecimalId(item),
      title: String(item && item.title || '(no title)').slice(0, 96),
      feed: (item && item.origin && item.origin.title) || '',
      feedId: (item && item.origin && item.origin.streamId) || '',
      summary: stripHtml(summaryHtml).slice(0, 80),
      time: item && item.published ? (item.published | 0) : 0,
      read: hasCategory(item, 'com.google/read') ? 1 : 0,
      star: hasCategory(item, 'com.google/starred') ? 1 : 0,
      _fullSummary: stripHtml(summaryHtml)
    };
  }

  function getTree(cb) {
    var subs = null;
    var allUnread = 0;
    var starred = 0;
    var firstErr = null;
    var pending = 3;

    function finish() {
      pending -= 1;
      if (pending > 0) {
        return;
      }

      if (firstErr) {
        cb(firstErr);
        return;
      }

      cb(null, mergeTree(subs, allUnread, starred));
    }

    getSubscriptions(function (err, result) {
      if (err) {
        firstErr = firstErr || err;
      } else {
        subs = result;
      }
      finish();
    });

    fetchIdsDirect(READING_LIST, '', 10000, true, function (err, result) {
      if (err) {
        firstErr = firstErr || err;
      } else {
        allUnread = result.ids.length;
      }
      finish();
    });

    fetchIdsDirect(STARRED, '', 10000, false, function (err, result) {
      if (!err) {
        starred = result.ids.length;
      }
      finish();
    });
  }

  function mergeTree(subs, allUnread, starred) {
    var nodes = [
      {
        type: 0,
        id: READING_LIST,
        name: 'All unread',
        unread: allUnread || 0,
        parent: ''
      },
      {
        type: 0,
        id: STARRED,
        name: 'Starred',
        unread: starred || 0,
        parent: ''
      }
    ];

    var folderById = {};
    var folderNodes = [];
    var feedNodes = [];

    function ensureFolder(labelId) {
      var id = canonicalLabel(labelId);
      if (!id || id.indexOf(LABEL_PREFIX) !== 0) {
        return;
      }

      if (Object.prototype.hasOwnProperty.call(folderById, id)) {
        return;
      }

      var parent = parentLabel(id);
      if (parent) {
        ensureFolder(parent);
      }

      folderById[id] = true;
      folderNodes.push({
        type: 1,
        id: id,
        name: lastLabelSegment(id),
        unread: 0,
        parent: parent
      });
    }

    subs = subs || [];

    for (var i = 0; i < subs.length; i++) {
      var sub = subs[i] || {};
      var parent = '';

      if (sub.categories && sub.categories.length && sub.categories[0].id) {
        parent = canonicalLabel(sub.categories[0].id);
        ensureFolder(parent);
      } else {
        parent = FALLBACK_LABEL;
        ensureFolder(parent);
      }

      feedNodes.push({
        type: 2,
        id: String(sub.id || ''),
        name: String(sub.title || sub.id || ''),
        unread: 0,
        parent: parent
      });
    }

    function depthOf(labelId) {
      var inner = String(labelId || '').replace(LABEL_PREFIX, '');
      var d = 0;
      for (var j = 0; j < inner.length; j++) {
        if (inner.charAt(j) === '/') {
          d += 1;
        }
      }
      return d;
    }

    function byName(a, b) {
      return a.name < b.name ? -1 : (a.name > b.name ? 1 : 0);
    }

    folderNodes.sort(function (a, b) {
      var da = depthOf(a.id);
      var db = depthOf(b.id);
      if (da !== db) {
        return da - db;
      }
      return byName(a, b);
    });

    feedNodes.sort(function (a, b) {
      if (a.parent !== b.parent) {
        return a.parent < b.parent ? -1 : 1;
      }
      return byName(a, b);
    });

    return nodes.concat(folderNodes, feedNodes);
  }

  function folderFeeds(labelStream, cb) {
    var wanted = canonicalLabel(labelStream);

    getSubscriptions(function (err, subs) {
      if (err) {
        cb(err);
        return;
      }

      var feeds = [];
      for (var i = 0; i < subs.length; i++) {
        var sub = subs[i] || {};
        var categories = sub.categories || [];

        for (var j = 0; j < categories.length; j++) {
          var category = canonicalLabel(categories[j] && categories[j].id);
          if (
            category === wanted ||
            category.indexOf(wanted + '/') === 0
          ) {
            feeds.push(String(sub.id || ''));
            break;
          }
        }
      }

      cb(null, feeds);
    });
  }

  function folderItems(stream, cont, n, unreadOnly, cb) {
    var count = n || 50;
    var offset = 0;

    if (cont && String(cont).indexOf('mf:') === 0) {
      offset = parseInt(String(cont).slice(3), 10) || 0;
    }

    folderFeeds(stream, function (feedsErr, feeds) {
      if (feedsErr) {
        cb(feedsErr);
        return;
      }

      if (!feeds.length) {
        cb(null, { items: [], continuation: '' });
        return;
      }

      var perFeed = offset + count;
      var pending = feeds.length;
      var firstErr = null;
      var ids = [];

      function finishIds() {
        pending -= 1;
        if (pending > 0) {
          return;
        }

        if (firstErr) {
          cb(firstErr);
          return;
        }

        /* Deduplicate before requesting contents. */
        var seen = {};
        var unique = [];
        for (var k = 0; k < ids.length; k++) {
          var id = String(ids[k]);
          if (!seen[id]) {
            seen[id] = true;
            unique.push(id);
          }
        }

        fetchContents(unique, function (contentsErr, rawItems) {
          if (contentsErr) {
            cb(contentsErr);
            return;
          }

          var mapped = [];
          for (var m = 0; m < rawItems.length; m++) {
            var mappedItem = mapItem(rawItems[m]);
            if (mappedItem.id) {
              mapped.push(mappedItem);
            }
          }

          mapped.sort(function (a, b) {
            return b.time - a.time;
          });

          var page = mapped.slice(offset, offset + count);
          var next = mapped.length > offset + count
            ? 'mf:' + String(offset + count)
            : '';

          cb(null, {
            items: page,
            continuation: next
          });
        });
      }

      for (var i = 0; i < feeds.length; i++) {
        fetchIdsDirect(
          feeds[i],
          '',
          perFeed,
          unreadOnly,
          function (err, result) {
            if (err) {
              firstErr = firstErr || err;
            } else {
              ids = ids.concat(result.ids);
            }
            finishIds();
          }
        );
      }
    });
  }

  function getItems(stream, cont, n, unreadOnly, cb) {
    if (canonicalLabel(stream).indexOf(LABEL_PREFIX) === 0) {
      folderItems(stream, cont, n, unreadOnly, cb);
      return;
    }

    fetchIdsDirect(stream, cont, n || 50, unreadOnly, function (idsErr, result) {
      if (idsErr) {
        cb(idsErr);
        return;
      }

      fetchContents(result.ids, function (contentsErr, rawItems) {
        if (contentsErr) {
          cb(contentsErr);
          return;
        }

        var items = [];
        for (var i = 0; i < rawItems.length; i++) {
          var mapped = mapItem(rawItems[i]);
          if (mapped.id) {
            items.push(mapped);
          }
        }

        cb(null, {
          items: items,
          continuation: result.continuation
        });
      });
    });
  }

  function getSummary(id, cb) {
    fetchContents([String(id)], function (err, items) {
      if (err) {
        cb(err);
        return;
      }

      if (!items.length) {
        cb(makeError(2, 'Article not found'));
        return;
      }

      cb(null, mapItem(items[0])._fullSummary);
    });
  }

  function editTag(body, cb) {
    request('POST', '/reader/api/0/edit-tag', body, function (err, resp) {
      if (err) {
        cb(err);
        return;
      }

      if (String(resp.text || '').indexOf('OK') !== -1) {
        cb(null, 'OK');
        return;
      }

      cb(makeError(2, 'Edit-tag failed'));
    });
  }

  function markRead(ids, cb) {
    var body = 'a=' + encodeURIComponent(READ_TAG);
    var list = Array.isArray(ids) ? ids : String(ids || '').split(',');

    for (var i = 0; i < list.length; i++) {
      var id = String(list[i]).trim();
      if (id) {
        body += '&i=' + encodeURIComponent(id);
      }
    }

    editTag(body, cb);
  }

  function markUnread(id, cb) {
    editTag(
      'r=' + encodeURIComponent(READ_TAG) +
      '&i=' + encodeURIComponent(String(id)),
      cb
    );
  }

  function star(id, on, cb) {
    var op = on ? 'a' : 'r';
    editTag(
      op + '=' + encodeURIComponent(STARRED) +
      '&i=' + encodeURIComponent(String(id)),
      cb
    );
  }

  function markAllRead(stream, cb) {
    request(
      'POST',
      '/reader/api/0/mark-all-as-read',
      's=' + encodeURIComponent(String(stream)) + '&ts=0',
      function (err) {
        if (err) {
          cb(err);
          return;
        }
        cb(null, 'OK');
      }
    );
  }

  return {
    ensureAuth: ensureAuth,
    getTree: getTree,
    getItems: getItems,
    getSummary: getSummary,
    markRead: markRead,
    markUnread: markUnread,
    star: star,
    markAllRead: markAllRead
  };
}

module.exports = {
  createClient: createClient,
  stripHtml: stripHtml
};
