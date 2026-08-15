# HeadeRSS

FreshRSS feed headings on your Pebble Time 2 — browse the feed tree, run
through articles, mark them read, star favourites. Native Timeline-style
reading view with an accent bar, dots and a pin.

## Features

- **Feed tree** — All unread / Starred streams, nested folders with unread
  badges; open a folder to play **all articles recursively** or dive into a
  single feed
- **Timeline reading view** — native-Timeline look: permanent accent spine on
  the right edge, one dot per article (accent = unread, muted = read), yellow
  star icon on starred entries, animated accent wash + pin + popping dot on
  the selected row; every row always shows heading + summary; lazy pages
  (50 articles) fetched only when you scroll near the bottom
- **Read through** — SELECT marks the article read (per the list toggle) and
  advances to the next (per the detail toggle); both toggles default on,
  switch them off to only star articles and leave them unread
- **Star** — long-press SELECT toggles the star; starred articles show a
  yellow star icon
- **Settings** — UP on the top dots row opens the watch sub-menu: Refresh,
  Mark all read, Connection hint, and the two reading toggle rows; the
  connection/appearance page lives in the phone app settings (Clay):
  FreshRSS URL, username, API password, theme
- **Lean** — 96-article window in RAM, live-only data, tree cached in flash
  for instant start

## API

100% FreshRSS-compatible via the **GReader API** (`/api/greader.php`):
ClientLogin auth (per-user **API password**, not the login password),
subscription list, unread counts, continuation-paginated streams, edit-tag
mark-read/star, mark-all-as-read.

## Setup

1. Install the app on your watch.
2. In the Pebble app -> HeadeRSS settings: enter your FreshRSS URL
   (e.g. `http://192.168.178.55:8080`), username and **API password**
   (FreshRSS profile -> API password — a separate password, min. 7 chars).
3. Open the app: the feed tree loads; UP on the top row opens the sub-menu.

## Build

Requires the Pebble SDK (v4.17+ for touch support):

```bash
pebble build
```

## Credits

- Inspired by [pebble-ha-launcher](https://github.com/SHU-red/pebble-ha-launcher)
  (architecture, menus, dialogs, Clay config, chunked AppMessage streaming)
- Native Timeline look recreated with the Pebble SDK's MenuLayer, GPath and
  Animation APIs

## License

MIT.
