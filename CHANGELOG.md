# Changelog

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
