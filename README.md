# HeadeRSS

FreshRSS feed headings on your Pebble Time 2 — browse the feed tree, run
through articles, mark them read, star favourites. Native Timeline-style
reading view with an accent bar, dots and a pin.

<div style="border:1px solid #f2d7b3; border-left:4px solid #FF813F; background:#fff8f0; border-radius:6px; padding:12px 16px; margin:16px 0;">
<strong>☕ Buy Me A Coffee</strong>
<p style="margin:8px 0 0;">These are small tools, built with AI — on purpose. There isn't enough time to learn every language and dive into every rabbit hole, so AI lets me solve real problems from my daily life and homelab — and that matters more to me than clever code.</p>
<p style="margin:8px 0 0;">The AI writes most of the code; the idea, the tinkering, testing, publishing and maintenance are mine.</p>
<p style="margin:8px 0 0;">Issues answered, features shipped, a few stars and downloads — does that sound like AI slop? Take a look and make your own opinion.</p>
<p style="margin:8px 0 0;">If this project helps you, <a href="https://www.buymeacoffee.com/yffbptmtaa">buy me a coffee</a> ☕</p>
</div>

## Features

- **Feed tree** — All unread / Starred streams, nested folders with unread
  badges; open a folder to play **all articles recursively** or dive into a
  single feed
- **Timeline reading view** — one article full screen, Timeline-style: accent
  top bar showing the folder/feed, accent header bar with the heading +
  feed·time, scrollable summary body; scrolling past the bottom slides to the
  next article (220 ms ease-in/out), scrolling up past the top goes back to
  previously read articles; permanent accent right-side bar with a gliding
  progress pin; yellow star with outline in the header for starred articles
  (long-press SELECT toggles)
- **Read through** — opening a stream shows the first article full screen;
  scroll to the bottom (or press SELECT) to advance; articles are marked read
  on becoming current (list toggle for the first, detail toggle for
  advances); scroll up past the top to re-open articles marked read this
  session
- **Unread only** — watch toggle (sub-menu): hides read articles from the
  server for feed/folder streams; "All unread" always shows only unread
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
