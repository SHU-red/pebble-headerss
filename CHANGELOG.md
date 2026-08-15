# Changelog

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
