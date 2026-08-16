# Changelog

## Store release notes

**HeadeRSS** — FreshRSS on your wrist: browse your feed tree, read
articles full-screen, mark read, star favourites, highlight words.

**v0.3.31**
- Articles scroll smoothly on every watch (incl. Time 2); hold DOWN jumps
  to the next article, hold UP goes back — long articles are a breeze
- Full article text streamed on demand — nothing is ever shortened
- Scrollbar progress bar with a flash on every article change
- Dark & light themes + your accent color; word highlighting with an M
  indicator; unread-only mode; auto-mark-read; per-feed mark-all-read and
  refresh

## 0.3.31

- **Progress bar: scrollbar thumb with a settle flash** — the 2 px line is
  now a full-width muted track with a 2 px-tall × 3 px-wide accent thumb at
  the current article position; the thumb flashes bright for ~160 ms on
  every article change (including the first article of a stream).

## 0.3.30

- **Connection info removed**: the sub-menu entry and its whole chain are
  gone — `proto_request_user_info`, the JS `userInfoFlow`/`getUserInfo`,
  the `FetchUserInfo` wire key and the account dialog. The sub-menu now
  lists 6 rows (Refresh / Mark all read / Auto mark read / Unread only /
  Important row / Progress line).

## 0.3.29

- **"HOLD DOWN" end hint redesigned**: the hint at the end of a long
  article is now centered text (`- HOLD DOWN -`) in the muted theme color
  instead of a grey bar — nothing is painted over the article. The content
  scrolls one extra line so the article's last line is never hidden behind
  the hint; the tap-hold-back logic is unchanged.

## 0.3.28

- **NEW-dot removed**: the "newer than your last visit" marker is gone —
  the per-feed last-seen table, the FeedNewest wire key and the toggle were
  removed cleanly (feeds are still sorted by newest activity).
- **No automatic un-star**: advancing inside Starred never un-stars an
  article anymore. Un-starring happens only when you explicitly hold SELECT
  on an article, and the Starred badge updates then. The Triage-drain
  setting is removed.
- **README**: screenshots now sit inline under their feature descriptions
  (feed tree, article, highlight matches, sub-menu) instead of a table.

## 0.3.27

- **Light mode fully implemented**: audit of every colorized surface (menus,
  dialogs, reader chrome, sidebar, badges, status) with the dark/light
  toggle now live-repaints everything. The one real defect was the reader
  divider (white-on-white in light mode) — it now renders dark gray there
  and repaints on a theme change; every other surface was already
  theme-driven or both-mode-readable by design (black top bar + accent
  chrome stay, per design).
- **README screenshots**: feed tree, sub-menu, article and highlight-match
  captures added.
- **Cleanup**: dead `highlight_words_csv()` removed (12 B of binary),
  unused `MARK_MODE_DELAY_MS`/`TIMELINE_ROW_H` macros dropped, retired
  `MarkOnOpenList`/`MarkOnOpenDetail` messageKeys removed, dead JS `login`
  export dropped, stale "140-char preview" comments corrected to 80.

## 0.3.26

- **Long articles must be left with a HOLD** (new): one fast tap at the end
  of a long article no longer throws you past it — an accidental tap used to
  advance and the scroll state was gone (getting back meant re-scrolling
  from the heading). At the article's true bottom a tap is held back (with
  a pulse) and a grey **LONG** bar at the bottom of the view indicates that
  holding DOWN is what advances. Short articles still advance on a tap.
- **The article scrolls by layer frame — proven to render on the Time 2**:
  the ScrollLayer was abandoned. It moves its content sub-layer by mutating
  the sub-layer's *bounds origin*; on the user's emery that path advances
  the offset state (logs: -150 → -2668, bottom=1, settle) but never redraws
  the screen, while app-owned `layer_set_frame` animations (settles) do
  render. The reader now scrolls manually: header + summary live in a plain
  content wrapper layer moved by `layer_set_frame` — the mechanism the
  device already proves renders. Offset clamping, fit/bottom/advance logic
  and the full-summary resize re-clamp are unchanged; ScrollLayer objects
  are gone (~400–500 B heap per page on the 64 KB class).
- **Build identity in the log**: startup now logs `build: HeadeRSS commit
  <hash>` so a device log can prove which binary is running.

## 0.3.25

- **HOLD UP/DOWN jumps to the previous/next article** (tap still
  page-scrolls): a long article used to need ~19 taps to move on; holding
  the button now advances immediately, skipping the text.
- **The article finally scrolls on screen — the reader's core bug**: the
  heading + summary page never moved visually. The page's header and body
  layers were attached with `layer_add_child()` onto the scroll layer's own
  layer, but a ScrollLayer only moves its internal "content" sub-layer
  (children must be added via `scroll_layer_add_child()`). The scroll offset
  state advanced exactly as logged (page-down -150 → -2668 → bottom → next
  article) while the drawn frames never changed, so the screen showed the
  same top of the article for every press. Verified: after one page-down the
  framebuffer diff went from ~10 pixels (nothing moved) to ~9,500 (the text
  scrolled); the bottom-advance still settles on the next article.

## 0.3.24

- **Long headings fully readable**: the heading's last line was never drawn
  — the highlight engine's y-limit guard dropped it for every multi-line
  title (a 1-line title rendered nothing at all, only feed·time). The guard
  now allows the full heading; the feed·time line stays clear.
- **Full summaries load on the 64 KB class (basalt/diorite/chalk)**: the app
  heap (~9.3 KB) could not fit the 4095-byte summary buffer plus the grown
  run table, so malloc failed silently and long articles stayed 80-char
  previews. The app_message buffers shrink to 2048/512 (phone chunks are
  capped at 1500 bytes to fit), the assembly cap is 2048 there, and the full
  text now loads and scrolls as one unit. emery/gabbro keep 4095/4096.
- **Headings up to 96 chars** on the Time 2 class (was 80); the 64 KB class
  keeps 80 to protect the heap.
- **Summary reliability**: every chunk now carries the article id (a stale
  chunk can no longer mix into the next article's buffer), a dropped chunk
  retries twice then finalizes, and the phone caches its auth token (no
  per-fetch ClientLogin round trip racing the 8 s watchdog).
- **Edge hardening**: the transition watchdog unschedules the wedged
  animations before the next transition destroys their layers; regressing
  below a ring drop during a prefetch rebuilds the reader page instead of
  showing a blank screen.
- **Item stream survives dropped acks**: a lost AppMessage ack used to kill
  the item send chain silently — the ring stayed partial (e.g. 1 of 33
  articles) while the count showed the full page, so the reader appeared
  stuck on the first entry ("can't advance although 33 articles are
  shown"). Item sends now retry twice and the watch dedups by id, so a
  re-send after a lost ack cannot duplicate and the page always completes.

## 0.3.23

- **Hold DOWN/UP = fast scroll**: UP/DOWN are now repeating clicks — a
  single press scrolls one page, holding repeats every 100 ms. A very long
  article (the 4095-char full-text cap can reach ~2800 px of text, ~18
  presses) scrolls through in a couple of seconds of holding, and holding
  past the end auto-advances for fast reading.

## 0.3.22

- **Starred stream never unread-filters**: the "Unread only" toggle was
  being applied to the Starred stream too, so only the unread subset of
  starred articles loaded (e.g. 2 of 33 — hence a 50% progress bar and
  nowhere to navigate). Starred is a curated list: ALL starred articles now
  load and you can hop through them one by one. The reading list and
  feed/folder streams keep their unread filtering.

## 0.3.21

- **Sidebar clock**: a 2-row clock chip (hours over minutes, e.g. 14 / 30)
  in the accent sidebar's top — accent digits on a black rounded chip,
  refreshed once a minute. Uses the previously wasted sidebar space.
- **Scroll circle closed**: the chrome is tighter (heading meta 22 → 18 px,
  body padding 6 → 4 px) so near-fit articles now actually FIT the viewport;
  content up to 8 px over advances on ONE press (no invisible micro-scroll),
  while genuinely long text keeps the visible ~3/4-viewport page-scroll and
  advances only at the real last word.

## 0.3.20

- Star icon one size bigger (30 px chunky path), column recentered

## 0.3.19

- **No fetch-hold on DOWN**: pressing DOWN while the full summary is still
  loading proceeds immediately (fast readers can skip ahead); the fetch for
  the skipped article is dropped and the next article's settles normally
- **Any overflow scrolls**: the ≤1-line "fits" heuristic is gone — if the
  article's text exceeds the viewport by even a few pixels (a cut last
  line), DOWN scrolls first so the bottom of the summary is always revealed
  before the next article appears; content that fits advances on one press

## 0.3.18

- **One press per article again**: the scroll air (0.3.16) made every
  article require two DOWN presses (a meaningless ~100 px scroll through
  empty margin first). The air is removed; articles whose text fits the
  viewport (or grazes it by ≤1 line) advance on ONE press, while genuinely
  long text keeps the visible ~3/4-viewport page-scroll. Short articles:
  one clean press to the next article — long articles: scroll, then
  advance at the real last word.

## 0.3.17

- **Progress bar reaches the sidebar exactly**: the bar's width is capped
  at the accent icon area's left edge — at the last article (100%) it spans
  right up to the sidebar instead of disappearing under it
- **Star redesigned**: chunkier, wider 26 px star (was narrow 24 px); the
  active colour is now bright chrome-yellow (was dark orange) — it pops
  clearly against the accent bar

## 0.3.16

- **Every article scrolls**: the body now carries ~160 px of scroll air
  below the text (a real reader margin). Even a two-line summary under a
  tall heading has a visible scroll range, so the last line is always
  scrolled fully into view before DOWN advances — no more half-cut final
  lines and no "advance as if it were short". The one-screen heuristic
  (0.3.15) is removed; DOWN page-scrolls (~3/4 viewport) and advances at
  the true bottom.

## 0.3.15

- **DOWN behavior for one-screen articles**: articles whose full text just
  grazes the viewport bottom (≤32 px of scrollable range) are treated as a
  single screen — the first DOWN advances cleanly instead of performing an
  invisible ~10 px micro-scroll that read as "nothing happened, then next
  article". Genuinely long articles keep the ~3/4-viewport page-scroll per
  press. (Diagnostics confirmed the reader logic was correct: the tested
  articles' full summaries were 113–126 bytes — one screen tall — so there
  was nothing to scroll.)

## 0.3.14

- **THE full-text cutoff — root cause fixed**: the highlight layout's run
  table was capped at 24 runs (~8 lines). A full summary needs hundreds of
  runs; once the table filled, every further token was silently DROPPED —
  the geometry kept counting (the "text grows" glimpse) but the text was
  never drawn below the cap (the cutoff) and the scroll range never covered
  the real content. The run table now grows a heap array on demand
  (doubling from 24, freed on re-layout/teardown; static 24-run fallback if
  the heap is exhausted). Full summaries are now laid out and drawn in
  their entirety — the page scrolls through the real text and DOWN advances
  only at the actual last word. Run count widened to uint16_t (a full text
  can exceed 255 runs).
- No .bss growth (the static table is unchanged; the grown arrays live on
  the heap).

## 0.3.12

- **Full-summary fetch reliability**: the FetchSummary request could hit a
  busy AppMessage outbox (the auto-mark batch flushes ~500 ms after every
  article settles — exactly when the summary request fires); a dropped
  request left long articles as short previews, so DOWN appeared to jump
  article-to-article without ever scrolling. The request now retries on a
  busy outbox (up to 3×) and the fetch watchdog was extended 3 s → 8 s to
  cover slower BLE chunk streams. The phone-side chunk flow was verified
  end-to-end (1604-char summary streams in one chunk + SummaryLast).
- Diagnostics: `summary:` log lines (request sent/retried, chunk bytes,
  complete/empty, preview-is-full skip) — if any article still fails, the
  next log names the exact link.

## 0.3.11

- **Page-scroll**: DOWN now scrolls the article by a full viewport per press
  (animated, with a small overlap so the previous screen's last line stays
  in view) and only advances at the true bottom of the text — a proper
  "scroll to the end, then next" reading flow instead of 32 px nudges and
  confusing no-ops. The final press lands exactly on the bottom so the last
  word is clearly visible. UP page-scrolls back symmetrically.
- **Full text reliability**: if a completed full-summary fetch ever missed
  its apply (article changed mid-stream), the settle now re-applies it —
  long articles reliably show their full text instead of the short preview.
- **Star and magnifier bigger** (24 px vs the 20 px circle), star outline
  thicker; the indicator column recentered.
- **Divider line under the top bar is white** (matches the menu group
  dividers).

## 0.3.9

- **The stuck-at-the-end bug, root-caused at the SDK level**: Pebble's
  scroll content offset is the content *origin* — 0 at the top, NEGATIVE
  when scrolled, `frame.h − content.h` at the very bottom. The DOWN
  handler compared it as a positive offset, so at the end of a long
  article the "at the bottom" condition never fired: DOWN kept calling the
  scroll handler (which clamped to nothing) and never advanced. The
  comparison is now `offset.y <= frame.h − content.h + 2` — DOWN scrolls
  the whole heading+summary unit, reaches the last word, and a further
  DOWN advances. The UP handler had the same convention bug (it could
  never scroll the body back up — only regress); fixed too.

## 0.3.8

- **Group dividers** in the menus: a thin muted line below "All unread" and
  below the Important row in the root menu (the specials are separated from
  the folder/feed area), and below "All articles" in the folder window
- **Sidebar indicator order**: star, circle, magnifier (was circle first) —
  the favourite star sits at the top
- (The favourite icon is the orange star since 0.3.7 — no heart remains)

## 0.3.7 — scrollable article unit, orange star, marker highlight

- **The whole heading + summary scroll as one unit**: the accent heading
  (full title, never clamped) and the summary body are now inside a single
  scroll layer — DOWN scrolls the article from the first line of the title
  to the last word of the text, and only a further DOWN at the very end
  advances. A thin accent divider line separates the top bar from the
  scrollable page. The scroll limit is the real end of the text, so the
  last word is always reachable.
- **Stuck-while-loading eliminated**: a full-summary fetch that never
  completes (e.g. a dropped chunk chain) timed out after 3 s and released
  the DOWN block — the preview stays and navigation resumes. (The stuck
  reports came from DOWN being blocked forever on the short preview.)
- **Favourite indicator is a star again, in orange** (was a white heart).
- **Highlight = text marker**: matched words are drawn bold with an alarm-red
  background fill (like a highlighter) instead of red text + underline; the
  sidebar magnifier lights up in the same alarm red as the marking.

## 0.3.6

- **Connection info (and every result dialog) fixed**: a pulse-tick callback
  that had already fired before the result arrived ran *after* the result
  text was set and overwrote it with "Loading…" again (then rescheduled) —
  so final results never stayed visible. A final-state flag now stops the
  pulse from clobbering results. Multiline results (the Connection account
  block) additionally drop to GOTHIC_18_BOLD so all lines fit the dialog.

## 0.3.5

- **Context menu is full screen** (long-press SELECT on a feed/folder) —
  it was a two-row bottom sheet, now it looks like every other menu
- **Mark all read updates the badges immediately**: the counts zero
  locally the moment the confirm is pressed (whole list, one feed, or a
  folder + its subtree; ancestor badges decrement; the Starred counter is
  untouched), instead of waiting for a re-fetch. The phone syncs the
  server in the background and a later Refresh re-verifies.

## 0.3.4 — navigation

- The accent right spine is gone from every menu (root, folder, sub-menu,
  context) — it carried no information next to the unread badges
- Leading nav icons in the root and folder menus, drawn in the row color:
  Important = **pin**, Starred = **star**, folders = **folder**, feeds =
  **news** (a document glyph); "All unread" / "All articles" get no icon.
  The old folder triangle marker is replaced by the folder icon

## 0.3.3

- **Starred badge now shows the real star count** (it was hardcoded 0): the
  GReader unread-count/tag-list endpoints have no starred breakdown, so the
  phone counts the starred stream directly (`stream/items/ids`, one extra
  parallel request in the tree fetch) and the watch no longer force-zeroes
  the Starred row. In-session star toggles (long-press, and the Starred
  triage drain) adjust the badge optimistically.

## 0.3.2

- **Stuck reader fixed (three causes)**:
  1. While the full summary of an article is still loading, the 80-char
     preview does not fill the screen — DOWN previously *advanced past* the
     article instead of scrolling. DOWN now never advances while the fetch
     is in flight for the current article; it scrolls (no-op on the short
     preview) until the full text lands.
  2. A transition that wedges (animation never completes/reports) left
     `s_advancing` locked — DOWN/UP/SELECT all dead. Added a 2 s watchdog:
     armed per transition, cancelled on settle, force-releases the locks if
     it ever fires. The reader can no longer lock up.
  3. The whole-ring-drop path (a page larger than the ring) left the live
     pages referencing evicted articles — now marked inert instead.
- **Sidebar indicators redesigned + repositioned**: a column of three
  monochrome glyphs vertically centered beside the physical SELECT button
  (right edge, mid-screen): read/unread disc (white = unread, black = read),
  favourite **heart** (white = starred), match **magnifying glass** (alarm
  red when highlight words match, black otherwise)

## 0.3.1 — optimization + reader polish

- **Resource optimization** (from the audit): summary preview 140 → 80
  chars (−59 B/article, full text still fetched on demand), last-seen
  entries 56 → 24 B (keys moved 12/100+i → 15/200+i, retired blobs swept),
  hand-rolled `div_million` removes the 754 B 64-bit division libcall.
  Result: emery 64.3 → 57.4 KB of the 65,535 B budget (margin 1.2 → 8.2 KB),
  basalt 57.6 → 51.8 KB, basalt heap free 7.9 → 13.7 KB (full summaries now
  fit on the 64 KB class too)
- **No black gap**: the page root had a double offset (page area y=26 plus
  an additional y=26 inside) — the heading now starts flush at y=26
- **Sidebar to the upper edge**: the accent bar spans the full screen
  height (y=0) with the icons at the top
- **Monochrome icons, bigger**: read/unread = filled 16 px circle (white =
  unread, black = read, no mixed eye), star = 18 px GPath (white =
  favourited), M = 18 px bold glyph — inactive all black; active "M" and
  every matched word light up in the same alarm red (GColorRed) so the
  sidebar M ↔ match connection is obvious
- **Underline fixed**: the match underline sat at the baseline/through the
  x-height (read as a strikethrough) — it now hugs the bottom of the line
  box
- **DOWN = scroll first, advance at the limit**: scrolling/flinging to the
  article bottom no longer auto-advances; a further DOWN press at the limit
  opens the next article
- **Stuck advance fixed**: an interrupted page transition left
  `s_advancing` locked forever ("sometimes can't go to the next article") —
  interrupted stops now release the locks and rebuild the spare page safely
- **Progress starts at 0**: the bar used the live article count (1/1 = 100%
  on entry); it now divides by the announced page size, so it starts at ~2%
  and grows with the loaded window
- Heading already rendered bold (kept)

## 0.3.0 — reading overhaul

- **Auto mark as read** replaces the "on list"/"on detail" toggles: one
  setting with Never / Immediately / 1s / 2s / 3s / 5s / 10s — the time an
  article must be shown before it is marked read (watch sub-menu opens a
  selector; persisted, default Immediately)
- **SELECT toggles read/unread** on the current article and cancels the
  pending auto-mark timer; long-press SELECT still toggles the star
- **Sidebar icons**, always visible at the top of the (now full-height)
  right bar: eye (white = unread, black = read), star (yellow = favourited,
  black otherwise), M (accent on a black capsule when the article has
  highlight-word matches, black otherwise); inactive icons are black
- **Full text**: the heading renders the complete title (multi-line, never
  truncated) and the summary is fetched in full on demand — the 140-char
  preview streams with the article, the phone returns the whole text
  (chunked over AppMessage into one heap buffer, shown in the scrollable
  body) — nothing is shortened
- **Layout**: the 2 px accent progress line sits at the very top of the
  screen (above the black top bar, which now starts at y=2); the gap
  between the top bar and the accent heading is gone; the heading text
  stops before the sidebar
- **Root menu**: the top row is now a black bar with accent dots
- Emery ring trimmed 72 → 68 to keep `.text+.data+.bss` ≤ 65535 B after
  the overhaul

## 0.2.4

- **Startup crash — deep libc frames removed from the inbox path.** The
  0.2.3 markers pinned the fault to the first tree-node processing, and the
  faulting PC (0x5c80) sat directly past the `strtoll` symbol in rodata:
  newlib's `strtoll`/`_strtoll_l` and the `snprintf`→`vfprintf` machinery
  have deep stack frames, and the AppMessage inbox callback runs on the
  2 KB basalt-class app stack — the FeedNewest parse plus three node
  `snprintf`s overflowed it, corrupting a return address into the rodata
  right after `strtoll`.
  Fix: hand-rolled `parse_decimal` (20 B frame, replaces `strtoll`) and
  `copy_str` (bounded, replaces `snprintf("%s")`) in the inbox path;
  `tree_add_node` now uses `strncpy`+NUL; the mark-read CSV join, mark id
  store and fetch-stream copy no longer use `snprintf` either. The inbox
  chain dropped from ~1.4 KB to ~300 B of stack. `snprintf` remains only
  in render/dialog/persist paths.
- Startup markers kept (`startup: tree requested / result / count /
  menu reload`).

## 0.2.3

- **Startup crash — menu animation / dialog interplay removed.** Crash
  forensics (faulting PC executed inside a rodata string literal — corrupted
  control flow; constant RAM LR = PebbleOS dispatch) pointed at two startup
  hazards:
  1. `menu_layer_set_selected_index(..., animated=true)` at window load: the
     tree arrives immediately after and `reload_data()` rebuilds the rows
     while the selection scroll-animation is still running — the menu's
     animation callback fires on the rebuilt menu (use-after-free →
     corrupted callback pointer → jump into rodata). Selection is now
     non-animated at load.
  2. The startup working dialog (shown when the tree cache is invalid —
     e.g. right after the 0.2.0 FeedNode cache-format change, which
     self-perpetuated because the crash prevented the cache from ever
     saving): the initial fetch no longer opens a dialog; the menu's empty
     state is the feedback and the cached tree still renders instantly
     (`tree_load_cache()` kept). Explicit user actions (Refresh,
     Connection, Mark all read) keep their dialogs.
- Startup step markers added (`startup: tree requested / result / count /
  menu reload`) so any remaining failure pinpoints its step in the log.

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
