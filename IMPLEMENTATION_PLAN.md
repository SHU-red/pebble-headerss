# HeadeRSS — Implementation Plan

Pebble Time 2 watchapp reading FreshRSS feed headings, marking them read, starring
favourites. Blueprint phase output; open questions are listed at the end and asked
separately before implementation starts.

Status: PLANNED (2026-08-15). Research live-verified against the test FreshRSS
instance (http://192.168.178.55:8080, FreshRSS 1.29.1, user `test`) and the working
reference app `pebble-ha-launcher`.

---

## 1. Decisions (research-backed)

| Topic | Decision | Evidence |
|---|---|---|
| API standard | **GReader (Google-Reader-compatible) API only** — FreshRSS's first-class API (`/api/greader.php`) | Live-verified all endpoints; docs call it "best"; large client ecosystem |
| Second API (Fever) | **Not supported** — effort is NOT "very low" once data-model work counts; value nil for a watch | flat groups (no tree), no per-feed unread counts, 50-item cursor pagination, no refresh, group titles mangled (`/` → U+FF0F), auth quirk (POST-only key, `auth:0` on HTTP 200) |
| Auth | ClientLogin (per-user **API password**, not the main password) → `Auth=<user>/<sha1>`; `Authorization: GoogleLogin auth=…` header; `T=x` token on mutating POSTs | Live: 401 `Unauthorized!` when API password unset/mismatched; `T=x`/`T=`/`T=<real>` all accepted |
| UI pattern | Reference `pebble-ha-launcher` (SDK3, waf, MenuLayer, Clay, chunked AppMessage streaming) + **native Timeline look** for the reading view (vertical accent bar, dots, animated pin) | Launcher deep-dive; user requirement |
| Mark-read | Auto on open, gated by TWO settings toggles ("Mark read on open (timeline)" / "(detail)"), both default ON; both off → articles stay unread, starring only | user decision |
| Settings scope | FreshRSS URL, username, API password + dark/light, accent color, touch toggle, the two mark-read toggles | user decision |
| Platforms | Same 5 color platforms as launcher: basalt, chalk, diorite, **emery (Time 2)**, gabbro | mirror reference app |
| Toolchain | Pebble Tool v5.0.39 / SDK v4.33 present on this machine; `pebble build`; node 22 for JS dev | local check |

---

## 2. FreshRSS GReader API contract (100% compatibility target)

Base: `{server}/api/greader.php/…` (reverse-proxy subpath simply prefixes).
All calls except ClientLogin carry `Authorization: GoogleLogin auth=<user>/<sha1>`.
Mutating POSTs use `T=x` (no `/token` round-trip needed).

| Purpose | Request | Notes (live-verified) |
|---|---|---|
| Auth | `POST /api/greader.php/accounts/ClientLogin` `Email=<user>&Passwd=<API password>` | 200 → `Auth=test/<sha1>`; 401 plain text `Unauthorized!` = API password unset/wrong |
| Tree | `GET …/reader/api/0/subscription/list?output=json` | `subscriptions[]`: `id: feed/N`, `title`, `categories[{id: user/-/label/<name>, label}]`, `url`, `iconUrl`, `frss:priority` |
| Counts | `GET …/reader/api/0/unread-count?output=json` | `{max, unreadcounts[]}` per feed, per category, per reading-list, with `newestItemTimestampUsec` (µs string) |
| Listing | `GET …/reader/api/0/stream/contents/<stream>?output=json&n=50&xt=user/-/state/com.google/read&c=<cont>` | `items[]`: `id: tag:…/item/<hex>`, `timestampUsec` (µs string == decimal id), `published` (s), `title`, `categories[]` (presence of `…/read` = read; absence = unread), `origin.title`, `summary.content` (HTML); `continuation` returned when more |
| Mark read / star | `POST …/reader/api/0/edit-tag` `T=x&a=user/-/state/com.google/read&i=<decimal id>` (repeatable `i`); unset via `r=`; star via `a=…/starred` | 200 `OK`; verified read + star round-trip |
| Mark all | `POST …/reader/api/0/mark-all-as-read` `T=x&s=<stream>&ts=0` | `s` = `feed/<id>` | `user/-/label/<name>` | reading-list | starred; `ts` in **nanoseconds** |
| Health | `GET /api/greader.php` | 200 `OK`; `GET …/check/compatibility` → `PASS` (no auth) |

Streams: `user/-/state/com.google/reading-list` (all), `…/unread` / `…/read`
(only via `it=`/`xt=` filters on reading-list or `s=` on `stream/items/ids` —
NOT as contents paths; 400 otherwise), `…/starred`, `user/-/label/<name>` (folder),
`feed/<id>`. FreshRSS extras: `user/-/state/org.freshrss/main|important`.

Gotchas (verified):
- **Nested folders are a client convention**: categories are flat in the DB; nesting
  = `Parent/Child` label strings. Rebuild tree by splitting on `/`.
- Slash-label stream paths MUST be URL-encoded: `…/user/-/label/News%2FHN` works;
  `…/user/-/label/News/HN` silently returns 0 items.
- **Folder recursion [VERIFY at implementation]**: does `user/-/label/Parent` include
  feeds in `Parent/Child`? Research did not settle this. Fallback: JS expands a folder
  to the union of its descendant feeds (one listing per feed, merged by µs id).
- Item ids: keep as strings (µs precision lost on 32-bit ints). `published` seconds
  for display.
- Pagination: `n` uncapped server-side; `n=50` per page is the watch budget; follow
  `continuation` strictly (server shifts by one id — no dupes).
- CORS enabled (Origin `*`, GET/POST, `Authorization`); PebbleKit JS is not CORS-bound.

---

## 3. Architecture

```
┌──────────────────────────────┐        ┌───────────────────────────────┐
│  Watch (C, SDK3)             │        │  Phone (PebbleKit JS)         │
│                              │        │                               │
│  main.c      windows/menus   │        │  freshrss.js  GReader client  │
│  tree.c      folder tree     │        │  app.js       lifecycle/msg   │
│  timeline.c  timeline view   │        │  config.js    Clay page       │
│  storage.c   persist + cache │ AppMsg │                               │
│  proto.c     message codec   │◄──────►│  localStorage prefill cache   │
│                              │ (BLE)  │                               │
└──────────────────────────────┘        └───────────────┬───────────────┘
                                                        │ HTTPS
                                                ┌───────▼───────┐
                                                │ FreshRSS      │
                                                │ /api/greader. │
                                                │ php           │
                                                └───────────────┘
```

- **All HTTP runs in PebbleKit JS on the phone** (watch has no network). C only ever
  sees decoded, size-bounded strings.
- **Config source of truth = watch flash** (launcher pattern). Phone localStorage is a
  Clay prefill cache; `ready` handler pulls durable config back from the watch.
- Settings page via `@rebble/clay` (same as launcher): Server URL, Username,
  **API password** (distinct from login password — settings text must say so), plus
  watch-bound appearance (dark/light, accent color, touch toggle, auto-close).

### AppMessage protocol (messageKeys in package.json)

Watch→phone: `FetchTree` (refresh flag), `FetchItems` (stream, continuation, n),
`MarkRead` (batch of ids), `StarItem` (id, 0/1), `MarkAllRead` (stream),
`RequestConfig`. Phone→watch: `ResultCode`/`ResultText` (generic errors),
`FeedCount` + per-feed `FeedName|FeedId|FeedUnread|FeedParent` streamed one per
message, `ItemCount` + per-item `ItemId|ItemTitle|ItemFeed|ItemTime|ItemRead|ItemStarred`
streamed one per message chained on outbox ack (launcher pattern, generation-counter
guarded), `Continuation` (opaque token kept on both sides). Watch-bound settings
keys (Clay → watch, launcher pattern): `AccentColor`, `DarkMode`, `TouchEnabled`,
`AutoClose`, `MarkOnOpenList`, `MarkOnOpenDetail`.

Buffers: `app_message_open(4096, 1024)` (launcher-verified). One article per message
(~200 B incl. title ≤ 64) is safe. Batch mark-read: up to ~12 decimal ids per
`edit-tag` POST (repeatable `i`), throttled to one in-flight POST.

### Watch memory budget (conservative; verify against emery heap at impl)

- Article ring buffer: **96 entries × ~140 B ≈ 13 KB** (`id[24] title[64] feed[24]
  time[8] flags[2]`). Only the window around the selection is kept; older pages are
  dropped, re-fetched on scroll-back via continuation.
- Feed tree (RAM): 64 feeds × 96 B ≈ 6 KB. Persisted tree: ≤ 4 KB flash (persist
  limit ~4 KB/app, 256 B/value [VERIFY]); cap feeds at 64, tree cache trimmed to
  folder names + unread counts.
- No article content cached in flash; summaries shown live from the loaded window.

### Lazy loading ("super efficient" requirement)

1. Tree load: one `subscription/list` + one `unread-count` → both tiny (< 4 KB).
   Cached in flash; re-fetched on app open (cheap) and on manual refresh.
2. Timeline open: JS fetches `n=50` titles of the requested stream (reading-list /
   folder / feed / starred), strips to `id|title|feed|time|read|starred`, streams to
   watch. Continuation kept. "Loading…" pulse dialog while streaming (launcher idiom).
3. Prefetch: when selection enters the last 6 rows, C asks for next page (continuation
   round-trip); page appended to ring. Pages > 3 behind the selection are dropped.
4. Newer data: re-entry to a stream re-fetches page 1 and merges by id (newest first;
   merge = prepend unseen ids, keep read flags).
5. Mark-read/star are fire-and-forget batches; failures surface via `ResultCode`.

---

## 4. UI / UX blueprint

### 4.1 Root menu (mirrors launcher main menu)

```
┌──────────────────────────────┐
│ •••                      (15px accent dots row — UP → settings)  │
│ ▸ All unread          12    │   ← reading-list (xt=read), badge
│ ★ Starred             3     │
│ ▾ Tech                3     │   ← folder, badge = recursive unread
│    ▸ Lobsters         3     │       feed row, badge
│    ▸ HN (News/HN)     5     │       nested folder row
│ ▸ FreshRSS releases   0     │
└──────────────────────────────┘
```

- Row 0 = narrow 15px accent row (three dots, launcher `main_get_cell_height`
  row 0 → 15) — **UP opens the settings sub-menu** (launcher `main_up_click`:
  `idx.row <= 1 → push_submenu_window`), selection starts on row 1.
- Folders render with unread badge (recursive sum), feeds with own badge.
- SELECT on folder → folder view: first row "▶ All articles (n)" (recursive stream),
  then subfolders, then feeds — satisfies "run through all articles of
  folders/recursive subfolders **or** only specific feeds".
- Sub-menu (UP from root): Refresh, Mark all read, About; settings page is the Clay
  config (launcher pattern).

### 4.2 Timeline reading view (user's core requirement)

Replicates native Pebble Timeline: vertical **accent-colored bar on the right edge**,
a **dot per article** on the bar (accent = unread, muted = read, star glyph variant),
and an **animated pin** marking the selected entry; up/down flows through entries.

Implementation (reuse-first, then custom):
- **Candidate open-source bases [VERIFY at implementation]**: PebbleOS source
  (`github.com/pebble/pebbleos`, MIT) timeline drawing code; `pebble-sdk-examples`
  (MIT) for animation idioms; `Neal/Readebble` (Pebble RSS reader) for JS chunked
  fetching; `Wowfunhappy/Pebble-RSS-Reader` for menu-of-titles patterns. If no
  timeline layer is liftable, implement as a custom MenuLayer draw: rows render
  right-aligned against the bar; bar + dots drawn by a full-height sibling Layer
  behind/over the menu; pin = animated notch (GPath) moved via
  `property_animation` on selection change; selection highlight cross-fades via the
  launcher's GColor8-lerp AnimationImplementation (no float math).
- Row: title (1–2 lines) + feed · relative time (muted, small); left indent for
  unread dot? No — timeline bar carries state. Starred rows show a filled star glyph
  (resource ICON_STAR-style, white variant on dark).
- Buttons in timeline: UP/DOWN scroll (MenuLayer native); SELECT opens the article
  detail; long-press SELECT toggles star; BACK pops to tree.
- Mark-read on open: selecting an article from the timeline marks it read only if
  **Mark read on open (timeline)** is on; entering/advancing articles in the detail
  flow marks read only if **Mark read on open (detail)** is on. Both default ON;
  with both off the user can star articles while they stay unread.
- Detail card (native notification-card style, launcher dialog idiom): title, feed,
  time, first ~200 chars of summary (HTML stripped in JS before sending), star
  toggle; SELECT = advance to next article (marks read per the detail toggle),
  BACK = back.
- Animation budget: selection highlight, pin glide, page-append rows fade in — all
  `animation_schedule`-based; no busy loops.

### 4.3 Settings (UP from row 0)

Clay page: FreshRSS URL, username, API password; behaviour: **Mark read on open
(timeline)** + **Mark read on open (detail)** toggles (both default ON);
appearance: dark/light, accent color, touch toggle. Watch-bound
sub-menu: Refresh, Mark all read, Connection hint ("set server in phone app
settings"). Mirrors launcher config split; `RequestConfig` re-sync on launch.

---

## 5. Project structure (in this repo)

```
package.json            pebble block: uuid (new), displayName HeadeRSS, sdkVersion 3,
                        enableMultiJS, capabilities [configurable], 5 platforms,
                        messageKeys, resources (menu icon, star glyphs), @rebble/clay
wscript                 copy of launcher's (glob src/**/*.c, js entry src/js/app.js)
src/main.c              windows, menus, click providers, autoclose
src/tree.c/.h           feed tree model, folder recursion, badges, persist
src/timeline.c/.h       timeline view: bar + dots + pin, ring buffer, prefetch
src/proto.c/.h          AppMessage codec (streamed collect, batch sends)
src/storage.c/.h        persist keys (launcher numbering scheme)
src/js/app.js           lifecycle, dispatch, streaming (generation counter)
src/js/freshrss.js      GReader client: auth, tree, counts, listing, edit-tag, batching
src/js/config.js        Clay page definition
resources/images/       logo_25.png (menu icon), star/star-white glyphs, store shots
README.md / CHANGELOG.md / LICENSE (MIT) / .gitignore (build/, node_modules/, .lock-waf_*)
```

---

## 6. Implementation phases

1. **Scaffold**: copy launcher build files, new uuid/displayName, messageKeys,
   resources, empty C/JS skeleton; `pebble build` green.
2. **JS API client**: `freshrss.js` against test instance (auth → tree → counts →
   listing+continuation → edit-tag); unit-testable in node with a mock Pebble;
   verify with curl-comparable assertions.
3. **Tree navigation**: tree model + root/folder menus + badges + persist; verify in
   emulator against test instance topology.
4. **Timeline view**: bar/dots/pin rendering, ring buffer, page prefetch, detail card,
   mark-read/star batching, mark-all; verify scroll + lazy behavior.
5. **Settings & polish**: Clay page, theme/accent/autoclose/touch, sub-menu, error
   surfacing, store assets, README/CHANGELOG.
6. **E2E on device/emulator** against live FreshRSS; harden timeouts/retries.

---

## 7. Test plan (live instance)

Instance state after research: FreshRSS 1.29.1; user `test`, login password
`testtest`, **API password set to `testtest`**; feeds: `feed/1` FreshRSS releases
(Uncategorized), `feed/2` Hacker News (News/HN), `feed/3` xkcd (Uncategorized),
`feed/4` Lobsters (Tech); 3 unread (Lobsters), 0 starred. Feeds may be added/
restructured freely — test user only.

Scenarios: auth failure (wrong API password) → guidance; tree with nested folder
(News/HN) renders hierarchy + badges; folder "All articles" recursion [resolve
VERIFY item]; pagination over >50 items; mark read removes badge; star → appears in
Starred; mark-all-as-read per feed/folder/global; offline/phone-unreachable errors;
memory: ring buffer drop/re-fetch on long scroll; both mark-read toggles OFF → starring leaves articles unread.

---

## 8. Decisions frozen from user answers (2026-08-15)

- API scope: **GReader API only** (no Fever).
- Article scope: **headings + summary detail** (title, feed, time, ~200 chars stripped summary).
- Mark-read: **auto on open**, gated by two settings toggles — "Mark read on open
  (timeline)" and "Mark read on open (detail)" — both default ON; with both off the
  user can star articles while leaving them unread.
- Star: **long-press SELECT in the timeline** + **toggle in the detail card**.
- Offline: **live-only**, feed tree + badges cached in watch flash.
- Settings scope: FreshRSS URL, username, API password + dark/light theme, accent
  color, touch toggle, the two mark-read toggles.

## 9. Items to verify during implementation

- Folder stream recursion in FreshRSS (parent label includes child-category feeds?).
- emery heap headroom for the 13 KB ring buffer; persist 4 KB/256 B per-value limits.
- Liftability/license of Timeline drawing code from PebbleOS vs clean-room reimplementation.
- `%2F` label paths through the user's real proxy setup.
