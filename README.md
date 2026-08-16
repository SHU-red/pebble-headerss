# HeadeRSS

FreshRSS feed headings on your Pebble Time 2 — browse the feed tree, run
through articles, mark them read, star favourites. Native Timeline-style
reading view with a black top bar, always-accent heading and an accent icon
sidebar (read/unread dot + star).

> [!NOTE]  
> ☕ **Buy Me A Coffee** — These are small tools, built with AI — on purpose. There isn't enough time to learn every language and dive into every rabbit hole, so AI lets me solve real problems from my daily life and homelab — and that matters more to me than clever code.
> The AI writes most of the code; the idea, the tinkering, testing, publishing and maintenance are mine.
> Issues answered, features shipped, a few stars and downloads — does that sound like AI slop? Take a look and make your own opinion.
> If this project helps you, [buy me a coffee](https://www.buymeacoffee.com/yffbptmtaa) ☕

## Features

- **Feed tree** — All unread / Starred / **Important** (FreshRSS priority
  feeds) streams, nested folders with unread badges; **NEW-dots** on feeds
  with articles newer than your last visit, feeds sorted by newest activity;
  open a folder to play **all articles recursively** or dive into a single
  feed
- **Timeline reading view** — one article full screen: 2 px accent progress
  line at the very top, black top bar with the stream name in accent, accent
  heading bar with the FULL article heading (multi-line, never truncated)
  + feed·time, scrollable summary body — the full summary is fetched on
  demand (the phone streams the complete text, chunked) so nothing is ever
  shortened; scrolling past the bottom slides to the next article
  (continuous two-page slide, 260 ms), scrolling up past the top goes back
  to previously read articles; a full-height accent sidebar holds three
  always-visible icons: eye (white = unread, black = read), star
  (yellow = favourited), M (lights up when highlight words match)
- **Read through** — opening a stream shows the first article full screen;
  scroll to the bottom (or press DOWN) to advance, scroll up past the top to
  go back; **SELECT toggles read/unread** (and cancels the auto-mark timer),
  long-press SELECT stars; articles are marked read after being shown for
  the configured time (see Auto mark read)
- **Unread only** — watch toggle (sub-menu): hides read articles from the
  server for feed/folder streams; "All unread" always shows only unread
- **Star** — long-press SELECT toggles the star; starred articles show a
  yellow star in the sidebar
- **Word highlighting** — set up to 10 words/phrases in the phone settings;
  matches are accent + bold + underlined in the summary and white + bold +
  underlined in the heading (whole words, case-insensitive, hyphens count
  as boundaries); updates live while reading; the sidebar's M lights up on
  articles with matches
- **Per-feed actions** — long-press SELECT on a feed row: **Mark all read**
  (with confirm) or **Refresh** (re-fetch and open at the newest)
- **Triage drain** — optional (sub-menu): inside Starred, advancing past an
  article un-stars it — star = keep, reading drains the list
- **Modern look** — dark theme by default, accent color everywhere: black
  top strip with accent dots on the root menu, permanent accent right spine
  in every menu, unread counts as filled accent pills, accent-selected rows;
  reader = 2 px accent progress line at the very top, black top bar with
  accent stream name, always-accent heading with black text, full-height
  icon sidebar (eye / star / M)
- **Settings** — UP on the top black strip opens the watch sub-menu:
  Refresh, Mark all read, Connection info (account + unread), **Auto mark
  read** (Never / Immediately / 1s / 2s / 3s / 5s / 10s), Unread only,
  Important row / NEW-dot / Progress line / Triage drain toggles; the
  connection/appearance page lives in the phone app settings (Clay):
  FreshRSS URL, username, API password, theme
- **Lean** — bounded article window in RAM (68 on the Time 2, 56 on the
  64 KB class), live-only data, tree cached in flash for instant start

## Screenshots

| | | |
|---|---|---|
| ![Feed tree](resources/store/menu.png) | ![Sub-menu](resources/store/menu_2.png) | |
| ![Article](resources/store/article.png) | ![Highlight matches](resources/store/article_match.png) | |

## API

100% FreshRSS-compatible via the **GReader API** (`/api/greader.php`):
ClientLogin auth (per-user **API password**, not the login password),
subscription list, unread counts (per-feed newest timestamps for NEW-dots),
continuation-paginated streams, edit-tag mark-read/star, mark-all-as-read,
user-info.

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
