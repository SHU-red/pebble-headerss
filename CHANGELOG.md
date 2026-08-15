# Changelog

## 0.2.2

- **Startup crash fixed (root cause)**: basalt/chalk/diorite run on a 2 KB
  app stack (PebbleOS: `APP_STACK_NORMAL_SIZE` 4 KB on emery/gabbro, 2 KB
  elsewhere — verified in `src/fw/process_management/app_manager.c`). The
  startup AppMessage chain (inbox callback → `ui_result` → dialog build →
  `text_layer_set_text` → SDK text layout, plus libc `strstr`'s deep
  two-way algorithm frame) overflowed it → corrupted return address → hard
  fault with a corrupted PC (crash logs showed PC mid-instruction inside
  `strstr` and a constant RAM LR = the stack region).
  Fix: replaced `strstr` with a bounded case-insensitive matcher
  (`contains_ci`, no deep libc frame), shrank `ui_result`'s buffer
  192→96 B, copied ResultText out of the AppMessage inbox before dialog
  work, and moved the highlight engine's 269 B of scratch (layout spans +
  slice buffer) off the stack. `ui_result` frame 208→112 B,
  `hl_build_layout` 584→304 B; the `strstr` crash site no longer exists
  in the binary.
- The earlier pulse-timer use-after-free fix (0.2.1) stays — it was a real
  bug ("Timer does not exist") but not this crash.

## 0.2.1

- **Word highlighting**: enter up to 10 words/phrases in the phone app
  settings (Clay) — matched words render in accent + bold + underline in
  the article summary and white + bold + underline in the heading. Whole
  words, case-insensitive; hyphens are word boundaries so "nuclear" matches
  inside "Nuclear-Fusion" while "ai" never matches inside "said". Matching
  is a plain bounded substring scan on the watch (no regex, microseconds);
  the hand-rolled layout engine (SDK 4.33 has no per-character positioning)
  caches a run table per article so scrolling stays cheap. Words apply to
  the open article immediately when saved.
- **Startup crash fix**: the working-dialog pulse timer ran without a
  liveness guard — when the dialog was popped while the timer was already
  fired ("Timer does not exist"), the queued callback touched the freed text
  layer (use-after-free → App fault at startup after the tree fetch). The
  pulse callback now re-checks dialog state, reschedules itself only while
  alive, and the dialog pop is guarded against a double dismiss racing the
  asynchronous unload.
- **Resource budget**: the highlight engine costs ~5 KB of
  .text/.bss; `.text+.data+.bss` must stay ≤ 65535 B (uint16
  `virtual_size`). Emery ring trimmed 96 → 72 articles; the 64 KB-class
  platforms (basalt/chalk/diorite) run 56 articles and 48 feed nodes so
  runtime heap (app_message buffers + windows) stays ~10 KB free. Fixed a
  `PBL_PLATFORM_GABBRO` typo that silently shrank gabbro to the small
  config — gabbro keeps 64/64.

## 0.2.0 — smart surface

- **Important row**: dedicated root-menu entry for the FreshRSS priority
  stream (`user/-/state/org.freshrss/important`), toggleable
- **NEW-dots**: feeds with articles newer than your last visit get an accent
  NEW pill; per-feed last-seen persisted, updated when the feed is opened
  (JS sends per-feed newest timestamps from unread-count and per-page newest
  from stream/contents)
- **Newest-first sorting**: feeds sort by newest activity (then unread, then
  name) within their folder; sub-folders stay above feeds, specials pinned
- **Per-feed context menu**: long-press SELECT on a feed row → Mark all read
  (with confirm) or Refresh (re-fetch, open at newest)
- **Connection info**: shows account name, email, server host and total
  unread via the GReader user-info + unread-count endpoints
- **Triage drain** (toggle, default OFF): inside Starred, advancing un-stars
  the article — star = keep, reading drains the list
- **Progress line**: static 2 px accent position bar in the reader top bar
  (toggle, default ON)
- **All caught up**: empty streams show an accent checkmark screen instead
  of bare "No articles"
- **App icon**: new 25 px menu icon (design from resources/store, 48/144 px
  store assets added); dead `getSummary`/`FetchSummary` removed

## 0.1.7

- Fixed the page transition: `property_animation_create_layer_frame` takes
  (from, to) — the arguments were swapped, so the outgoing page animated from
  its target position (no fly-out) and the incoming page ended parked below
  the screen; the swap now slides properly in both directions
- Fixed z-order: pages live in a dedicated area added before the accent
  sidebar, so the icon bar is always on top of the article page
- Interrupted transitions (teardown) no longer finalize the swap

## 0.1.6

- Reader redesign per spec: black top bar with the stream name in accent;
  heading bar is ALWAYS accent with black text (no more grey read headers);
  summary on the theme background; thick (26 px) accent sidebar holding the
  icons — read/unread dot (white filled = unread, white outline = read) and
  the yellow star
- Page transition rebuilt as a continuous two-page slide: the outgoing page
  leaves while the incoming page enters (both 260 ms ease-in-out, one sheet,
  no teleport cut)
- Sidebar is static (no more sliding pin — it behaved weird during
  transitions); icons update per article

## 0.1.5

- Modern look: dark theme by default; accent color made prominent — root
  menu's top row is a full accent strip, every menu has a permanent accent
  right spine, unread counts are filled accent pills (black count), selected
  rows stay accent-filled
- Reader now shows the read state: the header bar and the progress pin are
  accent for unread articles, dark gray for read ones
- Page slide transition smoothed (260 ms)

## 0.1.4

- Fixed: Clay-delivered accent color was truncated to gray (24-bit RGB was
  read as a raw GColor8 byte) — now converted properly, the accent shows on
  the top bar, header, spine and pin
- Re-introduced the accent top bar: shows the folder/feed currently showing
  its articles; the page slides beneath it
- Scrolling up past the top now goes BACK to the previously read article
  (still in the ring from this session); going back never marks anything
- Star icon: bigger (16 px) with a black outline so it reads on any accent
- New watch toggle "Unread only" (sub-menu, default ON): feed/folder streams
  exclude read articles from the server; "All unread" always filters
- Article ring raised to 96 on the Time 2 (emery, 128 KB RAM) — 64 on the
  64 KB platforms — so more read articles stay re-openable in a session

## 0.1.3

- Reading view is now a paged full-screen article reader in the style of the
  native Pebble Timeline: accent header bar (heading + feed·time), scrollable
  summary body; scrolling past the bottom advances to the next article with
  a 220 ms slide transition (ease-in out / ease-out in); permanent accent
  right-side bar with a gliding progress pin; SELECT also advances, long-press
  SELECT stars (yellow star in the header)
- Mechanics modelled on the open-source PebbleOS Timeline app
  (coredevices/PebbleOS: src/fw/apps/system/timeline/ layer.c + relbar.c +
  animations.c, src/fw/services/timeline/timeline_layout.c) — reimplemented
  with public SDK APIs (ScrollLayer, PropertyAnimation, Animation, GPath)
- Mark-read: first article per the list toggle, advance-reached articles per
  the detail toggle (both default on)

## 0.1.2

- Timeline reading view redesigned as the modern native-Timeline look:
  permanent accent spine, per-article dots (accent unread / muted read),
  yellow star icons, animated accent wash + pin notch + popping dot on the
  selected row, accent underline header
- No detail view anymore: every row always shows heading + summary
  (summaries stream with the articles, stripped to 140 chars)
- SELECT marks read (per the list toggle) and advances to the next article
  (per the detail toggle); long-press SELECT stars; starred rows show a star
- Ring buffer trimmed to 64 articles (fits the summary field; ~22 KB heap
  free on basalt-class platforms)

## 0.1.1

- Reading options moved from the phone settings to the watch: "Mark read on
  list" and "Mark read on detail" are now flat toggle rows in the watch
  sub-menu (UP from the root) — no submenus; Clay page keeps connection +
  appearance only

## 0.1.0

First release.

- Feed tree from FreshRSS GReader API: All unread / Starred streams, nested
  folders (label names split on `/`), unread badges (recursive sums), tree
  cached in watch flash
- Folder view: "All articles" runs the whole recursive subtree, or pick a
  single feed
- Timeline-style reading view: accent bar on the right edge, per-article
  dots (accent unread / muted read), star glyphs, pin notch on the selected
  row; lazy 50-title pages via continuation tokens, prefetched near the end
- Article detail card: title, feed · time, stripped summary (fetched on
  demand, 300 chars), SELECT advances to the next article
- Mark read on open: separate toggles for list and detail (default on);
  optimistic badge updates; batched edit-tag POSTs (12 ids / 500 ms flush)
- Star via long-press SELECT in list and detail; Starred stream in the tree
- Mark all read (orange confirm) from the sub-menu
- Clay settings page: FreshRSS URL, username, API password; mark-read
  toggles; dark/light theme, accent color, touch toggle
- Built with AI, maintained with love
