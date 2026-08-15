/**
 * HeadeRSS — FreshRSS GReader API client.
 *
 * Pure JavaScript client for the FreshRSS Google-Reader-compatible API
 * (/api/greader.php). No Pebble dependency: it can be required and driven
 * from Node for testing. Every method is callback-style:
 *   cb(err, data)   err = {code: number, text: string} | null
 *
 * Live-verified against FreshRSS 1.29.1 (user `test`):
 *  - ClientLogin POST returns "Auth=<user>/<sha1>"; every other call carries
 *    the header "Authorization: GoogleLogin auth=<token>".
 *  - Mutating POSTs accept the constant token "T=x" (no /token round-trip).
 *  - Label stream paths MUST be %2F-encoded ("News/HN" -> "News%2FHN"); a
 *    raw "/" in a label path silently returns 0 items.
 *  - edit-tag returns HTTP 200 "OK" on success.
 *  - Item ids are decimal microsecond timestamps that overflow 32-bit ints;
 *    they must stay strings end-to-end.
 *
 * Error codes are always nonzero on failure (the app-level ResultCode
 * convention is 0 = success): -1 network/timeout, 1 login failure, 2 bad
 * response, HTTP status otherwise.
 */

var API_BASE = '/api/greader.php';
var DEFAULT_TIMEOUT_MS = 15000;

var READING_LIST = 'user/-/state/com.google/reading-list';
var STARRED = 'user/-/state/com.google/starred';
var READ_TAG = 'user/-/state/com.google/read';
var LABEL_PREFIX = 'user/-/label/';
var FALLBACK_LABEL = 'user/-/label/Uncategorized';

function makeError(code, text) {
  return { code: code, text: text };
}

/**
 * Normalize a base URL: trim whitespace, strip trailing slashes.
 * @param {string} url
 * @return {string}
 */
function normalizeBaseUrl(url) {
  if (!url) {
    return '';
  }
  url = String(url).trim();
  while (url.charAt(url.length - 1) === '/') {
    url = url.slice(0, -1);
  }
  return url;
}

/**
 * Label stream paths are %2F-encoded so the server resolves the label as a
 * single path segment ("News/HN" must arrive as "News%2FHN"; a raw slash in
 * a label path silently returns 0 items). Other stream ids are used
 * verbatim.
 * @param {string} stream
 * @return {string}
 */
function encodeStream(stream) {
  var s = String(stream || '');
  if (s.indexOf(LABEL_PREFIX) === 0) {
    // Only the label portion is %2F-encoded; the "user/-/label/" prefix
    // must stay verbatim (verified: encoded prefix returns 0 items).
    return LABEL_PREFIX + s.slice(LABEL_PREFIX.length).split('/').join('%2F');
  }
  return s;
}

/**
 * True when the item carries a category id containing the given tag
 * substring at a tag boundary — the character right after the match must be
 * a '/', the end of the id, or a non-alphanumeric. This keeps
 * 'com.google/read' from matching the unrelated 'com.google/reading-list'
 * stream tag that FreshRSS puts on every item.
 */
function hasCategory(item, needle) {
  var cats = item.categories;
  if (!cats) {
    return false;
  }
  for (var i = 0; i < cats.length; i++) {
    var c = String(cats[i]);
    var idx = c.indexOf(needle);
    if (idx === -1) {
      continue;
    }
    var after = c.charAt(idx + needle.length);
    if (after === '' || after === '/' || !/[a-z0-9]/.test(after)) {
      return true;
    }
  }
  return false;
}

/**
 * Last path segment of a label id ("user/-/label/News/HN" -> "HN").
 */
function lastLabelSegment(labelId) {
  var s = String(labelId);
  var i = s.lastIndexOf('/');
  return i === -1 ? s : s.slice(i + 1);
}

/**
 * Parent label id of a label id ("user/-/label/News/HN" ->
 * "user/-/label/News"); '' for a top-level label. The "user/-/label/"
 * namespace prefix is not itself a label, so it never appears as a parent.
 */
function parentLabel(labelId) {
  var s = String(labelId);
  if (s.indexOf(LABEL_PREFIX) === 0) {
    var inner = s.slice(LABEL_PREFIX.length);
    var i = inner.lastIndexOf('/');
    if (i === -1) {
      return ''; // top-level label
    }
    return LABEL_PREFIX + inner.slice(0, i);
  }
  var j = s.lastIndexOf('/');
  return j <= 0 ? '' : s.slice(0, j);
}

/**
 * Strip HTML for the summary view: remove script/style blocks, strip tags,
 * decode the common basic entities and collapse whitespace.
 * @param {string} html
 * @return {string}
 */
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
  s = s.replace(/\s+/g, ' ').trim();
  return s;
}

/**
 * Create a FreshRSS GReader client.
 * @param {string} baseUrl   server URL (with or without scheme/trailing slash)
 * @param {string} username  FreshRSS username
 * @param {string} apiPass   FreshRSS API password (separate from login password)
 * @param {Object} [opts]    {request: function() -> XHR-like} for testing
 */
function createClient(baseUrl, username, apiPass, opts) {
  opts = opts || {};
  var base = normalizeBaseUrl(baseUrl);
  var user = String(username || '');
  var pass = String(apiPass || '');
  var token = null;
  var requestFactory = opts.request || function () {
    return new XMLHttpRequest();
  };

  /**
   * ClientLogin: POST Email=<user>&Passwd=<apiPass>; store the Auth token.
   * 401 (or a 200 without an Auth line) -> login failure.
   */
  function login(cb) {
    var xhr = requestFactory();
    xhr.open('POST', base + API_BASE + '/accounts/ClientLogin', true);
    xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    xhr.timeout = DEFAULT_TIMEOUT_MS;
    xhr.onload = function () {
      if (xhr.status === 200) {
        var m = String(xhr.responseText || '').match(/Auth=([^\r\n]+)/);
        if (m && m[1]) {
          token = m[1];
          cb(null, token);
        } else {
          cb(makeError(1, 'Login failed — check API password'));
        }
      } else if (xhr.status === 401) {
        cb(makeError(1, 'Login failed — check API password'));
      } else {
        cb(makeError(xhr.status, 'HTTP ' + xhr.status));
      }
    };
    xhr.onerror = function () {
      cb(makeError(-1, 'Network error'));
    };
    xhr.ontimeout = function () {
      cb(makeError(-1, 'Timeout'));
    };
    xhr.send('Email=' + encodeURIComponent(user) + '&Passwd=' + encodeURIComponent(pass));
  }

  /**
   * Reuse the stored token or log in once.
   */
  function ensureAuth(cb) {
    if (token) {
      cb(null, token);
      return;
    }
    login(cb);
  }

  /**
   * Authorized request: Authorization header on every call, 15s timeout, and
   * one 401 retry (discard the token, log in again, replay the request once).
   * Every failure code is nonzero (the watch treats ResultCode 0 as success):
   * {code: -1, net}, {code: HTTP status}, {code: -1, timeout}.
   */
  function request(method, url, body, cb) {
    ensureAuth(function (authErr) {
      if (authErr) {
        cb(authErr);
        return;
      }
      doRequest(method, url, body, false, cb);
    });
  }

  function doRequest(method, url, body, retried, cb) {
    var xhr = requestFactory();
    xhr.open(method, url, true);
    xhr.setRequestHeader('Authorization', 'GoogleLogin auth=' + token);
    if (body) {
      xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    }
    xhr.timeout = DEFAULT_TIMEOUT_MS;
    xhr.onload = function () {
      if (xhr.status === 401 && !retried) {
        token = null;
        login(function (loginErr) {
          if (loginErr) {
            cb(loginErr);
            return;
          }
          doRequest(method, url, body, true, cb);
        });
        return;
      }
      if (xhr.status === 200) {
        cb(null, { status: xhr.status, text: xhr.responseText });
      } else {
        cb(makeError(xhr.status, 'HTTP ' + xhr.status));
      }
    };
    xhr.onerror = function () {
      cb(makeError(-1, 'Network error'));
    };
    xhr.ontimeout = function () {
      cb(makeError(-1, 'Timeout'));
    };
    xhr.send(body || null);
  }

  /**
   * Map one stream/contents item to the wire format. The µs id stays a
   * string (16 digits lose precision as a JS int).
   */
  function mapItem(item) {
    return {
      id: String(item.timestampUsec),
      title: String(item.title || '(no title)').slice(0, 80),
      feed: (item.origin && item.origin.title) || '',
      feedId: (item.origin && item.origin.streamId) || '',
      summary: stripHtml(item.summary && item.summary.content || '').slice(0, 140),
      time: item.published | 0,
      read: hasCategory(item, 'com.google/read') ? 1 : 0,
      star: hasCategory(item, 'com.google/starred') ? 1 : 0
    };
  }

  /**
   * Feed tree: parallel subscription/list + unread-count, merged into an
   * ordered node list {type, id, name, unread, newest, parent}:
   *   specials first, then folders (depth asc, then name), then feeds
   *   (grouped by parent, alphabetical). Folder nodes always precede the
   *   feeds/folders that reference them. `newest` is the per-feed
   *   newestItemTimestampUsec (decimal µs string; '0' when absent) so the
   *   watch can show a NEW-dot per feed.
   */
  function getTree(cb) {
    var subsUrl = base + API_BASE + '/reader/api/0/subscription/list?output=json';
    var countsUrl = base + API_BASE + '/reader/api/0/unread-count?output=json';
    var subs = null;
    var counts = null;
    var firstErr = null;
    var pending = 2;

    function finish() {
      pending -= 1;
      if (pending > 0) {
        return;
      }
      if (firstErr) {
        cb(firstErr);
        return;
      }
      cb(null, mergeTree(subs, counts));
    }

    request('GET', subsUrl, null, function (err, resp) {
      if (err) {
        firstErr = firstErr || err;
      } else {
        try {
          subs = JSON.parse(resp.text);
        } catch (e) {
          firstErr = firstErr || makeError(2, 'Bad subscription list');
        }
      }
      finish();
    });
    request('GET', countsUrl, null, function (err, resp) {
      if (err) {
        firstErr = firstErr || err;
      } else {
        try {
          counts = JSON.parse(resp.text);
        } catch (e) {
          firstErr = firstErr || makeError(2, 'Bad unread counts');
        }
      }
      finish();
    });
  }

  function mergeTree(subsResp, countsResp) {
    var countsMap = {};
    var newestMap = {};
    var unreadCounts = countsResp && countsResp.unreadcounts;
    if (unreadCounts) {
      for (var i = 0; i < unreadCounts.length; i++) {
        var uc = unreadCounts[i];
        if (uc && uc.id) {
          countsMap[uc.id] = uc.count || 0;
          newestMap[uc.id] = String(uc.newestItemTimestampUsec || '0');
        }
      }
    }
    var subs = (subsResp && subsResp.subscriptions) || [];

    var nodes = [];

    // Specials first.
    nodes.push({
      type: 0,
      id: READING_LIST,
      name: 'All unread',
      unread: countsMap[READING_LIST] || 0,
      newest: '0',
      parent: ''
    });
    nodes.push({
      type: 0,
      id: STARRED,
      name: 'Starred',
      unread: 0,
      newest: '0',
      parent: ''
    });

    // Folders derived from subscriptions' categories. Nesting is a client
    // convention ("Parent/Child" label strings): build the full ancestor
    // chain so every referenced parent folder node exists.
    var folderById = {};
    var folderNodes = [];
    function ensureLabelFolder(labelId) {
      if (Object.prototype.hasOwnProperty.call(folderById, labelId)) {
        return;
      }
      var parent = parentLabel(labelId);
      if (parent) {
        ensureLabelFolder(parent);
      }
      folderById[labelId] = true;
      folderNodes.push({
        type: 1,
        id: labelId,
        name: lastLabelSegment(labelId),
        unread: 0,
        newest: '0',
        parent: parent
      });
    }
    for (var s = 0; s < subs.length; s++) {
      var cats = subs[s].categories;
      if (!cats) {
        continue;
      }
      for (var c = 0; c < cats.length; c++) {
        var cid = cats[c] && cats[c].id;
        if (cid && String(cid).indexOf(LABEL_PREFIX) === 0) {
          ensureLabelFolder(String(cid));
        }
      }
    }

    // Feeds: one node per subscription, parented to its first category.
    var feedNodes = [];
    for (var f = 0; f < subs.length; f++) {
      var sub = subs[f];
      var feedId = String(sub.id || '');
      var parent = '';
      if (sub.categories && sub.categories.length && sub.categories[0].id) {
        parent = String(sub.categories[0].id);
      } else {
        parent = FALLBACK_LABEL;
        ensureLabelFolder(FALLBACK_LABEL);
      }
      feedNodes.push({
        type: 2,
        id: feedId,
        name: String(sub.title || feedId),
        unread: countsMap[feedId] || 0,
        newest: newestMap[feedId] || '0',
        parent: parent
      });
    }

    function depthOf(labelId) {
      var s = String(labelId);
      var d = 0;
      for (var i = 0; i < s.length; i++) {
        if (s.charAt(i) === '/') {
          d += 1;
        }
      }
      return d;
    }
    function byName(a, b) {
      return a.name < b.name ? -1 : (a.name > b.name ? 1 : 0);
    }

    // Folders: depth ascending (parents first), then name.
    folderNodes.sort(function (a, b) {
      var da = depthOf(a.id);
      var db = depthOf(b.id);
      if (da !== db) {
        return da - db;
      }
      return byName(a, b);
    });

    // Feeds: grouped by parent, alphabetical within a group.
    feedNodes.sort(function (a, b) {
      if (a.parent !== b.parent) {
        return a.parent < b.parent ? -1 : 1;
      }
      return byName(a, b);
    });

    return nodes.concat(folderNodes, feedNodes);
  }

  /**
   * One page of a stream. Label streams are %2F-encoded; the reading-list
   * additionally excludes already-read items. Returns
   * {items: [...], continuation: string, newest: string} — continuation ''
   * means no more; newest is the raw timestampUsec of the newest article in
   * the page (decimal µs string, '0' when the page has no items), so the
   * watch can store it as the feed's last-seen when the feed is opened.
   */
  function getItems(stream, cont, n, unreadOnly, cb) {
    var enc = encodeStream(stream);
    var count = n || 50;
    var url = base + API_BASE + '/reader/api/0/stream/contents/' + enc +
      '/output=json?n=' + count + '&ck=' + Date.now();
    if (cont) {
      url += '&c=' + cont;
    }
    // Unread-only: the reading-list stream always filters; feed/folder
    // streams filter when the watch's "Unread only" setting is on.
    if (String(stream || '') === READING_LIST || unreadOnly) {
      url += '&xt=' + READ_TAG;
    }
    request('GET', url, null, function (err, resp) {
      if (err) {
        cb(err);
        return;
      }
      var parsed = null;
      try {
        parsed = JSON.parse(resp.text);
      } catch (e) {
        cb(makeError(2, 'Bad stream response'));
        return;
      }
      var items = [];
      var newest = '0';
      var list = parsed.items;
      if (list) {
        for (var i = 0; i < list.length; i++) {
          items.push(mapItem(list[i]));
          var ts = String(list[i].timestampUsec || '');
          if (ts.length > newest.length ||
              (ts.length === newest.length && ts > newest)) {
            newest = ts;
          }
        }
      }
      cb(null, {
        items: items,
        continuation: parsed.continuation || '',
        newest: newest
      });
    });
  }

  /**
   * Full text of one item: POST stream/items/contents with the item id as a
   * single &i= parameter, then strip the HTML from items[0].summary.content.
   * The FULL stripped text is returned (no length cap) — the watch renders
   * it in the scrollable body.
   */
  function getSummary(id, cb) {
    request('POST', base + API_BASE + '/reader/api/0/stream/items/contents',
      'i=' + encodeURIComponent(String(id)), function (err, resp) {
        if (err) {
          cb(err);
          return;
        }
        var parsed = null;
        try {
          parsed = JSON.parse(resp.text);
        } catch (e) {
          cb(makeError(2, 'Bad summary response'));
          return;
        }
        var item = null;
        if (parsed && parsed.items && parsed.items.length) {
          item = parsed.items[0];
        } else if (Array.isArray(parsed)) {
          item = parsed[0];
        } else {
          item = parsed;
        }
        var html = (item && item.summary && item.summary.content) || '';
        cb(null, stripHtml(html));
      });
  }

  /**
   * Mark a batch of item ids read: one edit-tag POST with a repeated &i=
   * parameter per id. Success = HTTP 200 with an "OK" body.
   */
  function markRead(ids, cb) {
    var body = 'T=x&a=' + READ_TAG;
    var list = Array.isArray(ids) ? ids : String(ids || '').split(',');
    for (var i = 0; i < list.length; i++) {
      var id = String(list[i]).trim();
      if (id) {
        body += '&i=' + encodeURIComponent(id);
      }
    }
    editTag(body, cb);
  }

  /**
   * Mark one item unread: an edit-tag POST removing the read tag
   * (r=.../read, so the item returns to the unread list). Success = HTTP 200
   * with an "OK" body.
   */
  function markUnread(id, cb) {
    var body = 'T=x&r=' + READ_TAG + '&i=' + encodeURIComponent(String(id));
    editTag(body, cb);
  }

  /**
   * Star/unstar one item (a=.../starred to set, r=.../starred to clear).
   */
  function star(id, on, cb) {
    var op = on ? 'a' : 'r';
    var body = 'T=x&' + op + '=' + STARRED + '&i=' + encodeURIComponent(String(id));
    editTag(body, cb);
  }

  function editTag(body, cb) {
    request('POST', base + API_BASE + '/reader/api/0/edit-tag', body, function (err, resp) {
      if (err) {
        cb(err);
        return;
      }
      if (String(resp.text || '').indexOf('OK') !== -1) {
        cb(null, 'OK');
      } else {
        cb(makeError(2, 'Edit-tag failed'));
      }
    });
  }

  /**
   * Mark an entire stream read (stream encoding as in getItems).
   */
  function markAllRead(stream, cb) {
    var body = 'T=x&s=' + encodeStream(stream) + '&ts=0';
    request('POST', base + API_BASE + '/reader/api/0/mark-all-as-read', body, function (err, resp) {
      if (err) {
        cb(err);
        return;
      }
      cb(null, 'OK');
    });
  }

  /**
   * Account info for the Connection screen: parallel user-info +
   * unread-count (max field), like getTree's parallel fetch. Returns
   * {userName, userEmail, unread} with raw values ('' / 0 when absent).
   */
  function getUserInfo(cb) {
    var infoUrl = base + API_BASE + '/reader/api/0/user-info?output=json';
    var countsUrl = base + API_BASE + '/reader/api/0/unread-count?output=json';
    var info = null;
    var counts = null;
    var firstErr = null;
    var pending = 2;

    function finish() {
      pending -= 1;
      if (pending > 0) {
        return;
      }
      if (firstErr) {
        cb(firstErr);
        return;
      }
      cb(null, {
        userName: info.userName || '',
        userEmail: info.userEmail || '',
        unread: counts.max || 0
      });
    }

    request('GET', infoUrl, null, function (err, resp) {
      if (err) {
        firstErr = firstErr || err;
      } else {
        try {
          info = JSON.parse(resp.text);
        } catch (e) {
          firstErr = firstErr || makeError(2, 'Bad user info');
        }
      }
      finish();
    });
    request('GET', countsUrl, null, function (err, resp) {
      if (err) {
        firstErr = firstErr || err;
      } else {
        try {
          counts = JSON.parse(resp.text);
        } catch (e) {
          firstErr = firstErr || makeError(2, 'Bad unread counts');
        }
      }
      finish();
    });
  }

  return {
    login: login,
    ensureAuth: ensureAuth,
    getTree: getTree,
    getItems: getItems,
    getSummary: getSummary,
    getUserInfo: getUserInfo,
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
