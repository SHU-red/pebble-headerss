# Changelog

## Store release notes

**HeadeRSS** — FreshRSS on your wrist: browse your feed tree, read
articles full-screen, mark read, star favourites, highlight words.

**v0.3.42**
- Both bottom-line hints in the accent color — "HOLD ▼: Scroll" on
  long articles, "HOLD ▼: Next" at the end of a scroll
- The article fills the whole screen while scrolling — no empty bottom
  bar; the "Next" line reads like the article's own last line
- Fast skimming: DOWN always jumps to the next article; long articles
  invite a hold with "HOLD ▼: Scroll"
- Hold DOWN enters scroll mode: taps scroll page by page, a hold jumps
  to the next article at any time, "HOLD ▼: Next" at the end
- UP mirrors DOWN: previous article in the skim view, scroll-up inside
  scroll mode, exits back to the skim view at the top
- Read articles in full — nothing is ever cut off; long articles scroll
  smoothly on every watch
- Editorial reader: bold white headings with an accent feed·time line,
  word highlighting in your accent color
- Theme on the watch: one tap cycles System → Dark → Light
- Slim sidebar with clock, star, unread dot and match indicator
- Star favourites, unread-only mode, auto-mark-read, per-feed
  mark-all-read
- Highlight words survive reinstalls

## 0.3.42

- **Both hints in the accent color.** "HOLD ▼: Scroll" (skim view) and
  "HOLD ▼: Next" (scrolled to the end) now share the accent color — the
  text-color/muted split is gone.
- **Full-page scroll.** In scroll mode the article takes the whole page
  — no empty bottom bar while scrolling. "HOLD ▼: Next" acts as the
  article's last line: the scroll range leaves the bottom line's room,
  so it only appears at the very end, never over text. The skim view
  keeps its reserved band for "HOLD ▼: Scroll".

## 0.3.41

- **Clean bottom band.** The bottom hint now sits in a reserved strip:
  long articles end their scroll area early, so the "HOLD ▼: Scroll"
  (skim view, muted) and "HOLD ▼: Next" (scrolled to the end, accent)
  lines never draw over article text. Mid-scroll the strip stays empty —
  a single tap scrolling needs no hint. Short articles fill the whole
  page (no band).
- **Entry toast removed.** The brief "HOLD ▼: Scroll" chip is gone — the
  bottom line alone is the indicator.
- **Down-triangle fixed.** The ▼ now points down (wide base on top,
  point at the bottom — the DOWN button), drawn as one measured unit
  with the text.

## 0.3.40

- **Readable entry chip.** The scroll-mode chip is now an inverted box:
  the pill fills with the font color and the text writes in the page
  color — white pill with black text in dark mode, black pill with
  white text in light. No more bleed-through from the article behind.
- **Hold-only hints.** The bottom line now says "HOLD ▼: Scroll" in the
  skim view (muted) and "HOLD ▼: Next" while scrolling (accent) — it
  never mentions what a single tap does. The ▼ is a drawn down
  triangle (no arrow glyph exists in the system fonts), laid out as one
  centered unit with the text.

## 0.3.39

- **Scroll-mode indicator.** The long-press that enters scroll mode now
  shows a centered "TAP to scroll" chip (accent text on a page-colored
  pill) for 1.2 s — the hold flips the tap behavior, and the chip says so.
- **Persistent mode hint.** While in scroll mode the bottom line reads
  "TAP scroll · HOLD next" in the accent color for the whole scroll;
  at the very end it switches back to the muted "- HOLD DOWN -" (tap is
  held back there, the advance is hold-only). The skim view keeps its
  muted "- HOLD DOWN -" invitation.
- **Fix: the bottom hint was never visible.** The end bar sat inside the
  page area below the pages, which are built later and paint over it —
  the "- HOLD DOWN -" and the new mode line were hidden under the
  article. The bar now lives in the window root, drawn above the page.

## 0.3.38

- **New article-read flow.** DOWN now always skips to the next article
  (fast skimming — no more accidentally reading). On long articles a
  centered "- HOLD DOWN -" hint sits at the bottom of the skim view:
  holding enters scroll mode.
- **Scroll mode:** single DOWN taps page-scroll (~3/4 viewport); holding
  DOWN jumps to the next article at any time (the tap at the very end is
  held back with a pulse — advance is hold-only, the "- HOLD DOWN -"
  hint reappears at the end). UP scrolls back up and exits scroll mode
  at the top; a further UP goes to the previous article. Short articles
  never enter scroll mode: hold just jumps.
- Every article still starts in the skim view (scroll mode resets on
  transitions), so skipping through a feed never gets stuck mid-scroll.

## 0.3.37

- **Heading color back to white** (theme color: white in dark mode, black
  in light) — the accent-heading experiment of 0.3.35 is reverted; the
  heading stays GOTHIC_24_BOLD.
- **Feed·time line in the accent color** — the source + time below the
  heading is now your accent color instead of the muted gray.
- Highlight words in the heading are back to the accent-fill + black-text
  treatment (the inverted chips were a stopgap for the accent heading).

## 0.3.36

- **Match badge rework**: the magnifier is gone — the badge is always an
  "M". By default it's an accent "M" on a small black rounded chip; when
  the article matches a highlight word the chip turns alarm-red and the M
  flips to white (so the match is scannable at a glance).
- **Smaller top-bar stream name**: the feed/folder name above the article
  is GOTHIC_18_BOLD instead of ROBOTO_CONDENSED_21.
- **Bugfix**: changing the highlight words while reading re-laid the
  article heading with the wrong font (18-bold instead of 24) — the
  heading jumped in size on every word change. The re-layout now uses the
  same GOTHIC_24_BOLD as the fresh page build.

## 0.3.35

- **Progress bar**: 1 px thicker (2 → 3 px) and the big position dot is gone —
  the accent fill's edge marks the current article. The settle flash is
  removed with it (there was nothing left to flash).
- **Sidebar clock**: sits at the very top now — plain black GOTHIC_14_BOLD
  digits on the accent bar, no chip, no border.
- **Article heading**: smaller (GOTHIC_24_BOLD) and colorized in the accent
  color; highlighted words invert to a theme chip (white with black text in
  dark mode, black with white text in light) so they still pop on the
  accent heading.

## 0.3.34

**Tier A + Tier B design overhaul** (the full idea list, implemented):

- **Editorial reader header**: the saturated accent band is gone — the heading
  reads on the page background in **GOTHIC_28_BOLD** (theme color) with
  accent-filled highlight words, the feed·time meta is muted GOTHIC_14, and a
  2 px accent rule closes the header like a newspaper dateline (P13/P4).
- **Type hierarchy**: article headings 18 → 28 bold, the top-bar stream name
  is ROBOTO_CONDENSED_21, dialogs one-liners are 28 bold, the all-caught-up
  status is 24 bold (P4).
- **Light mode is coherent**: the reader's top bar is now theme-aware —
  white with black text in light mode, the black crown stays in dark (P5).
- **Slimmer sidebar (26 → 20 px)**: the reading column is wider; the clock
  chip got a 1 px white border (visible on any accent) and the 
  star/disc/M stack is re-spaced — the 144×168 clock/star overlap is gone
  (P14/P11/P3).
- **State legibility**: the read disc is now the classic unread-dot idiom
  (white when unread, nothing when read — no more invisible black dot), the
  starred star gets a black outline so it pops on any accent, and the match
  indicator is an alarm-red rounded chip with a white "M" (P2/P1).
- **Micro-refinement**: 1 px inset menu dividers, rounded progress caps,
  chunkier launcher strip (18 px, r3 dots), uniform 46 px menu rows,
  dialog glyphs (check / X / question mark), a custom RSS-fan empty state,
  a 2 px checkerboard dither seam at the sidebar's inner edge, and a softer
  300 ms ease-out page transition (P6–P10/P12/P15).
- **Bugfix**: the launch restore now applies highlight words — it sent
  AccentColor/TouchEnabled/HighlightWords in one message and the settings
  branch returned before the words were processed, so a reinstall never
  restored your matchwords on the watch.

## 0.3.33

- **Theme on the watch**: the sub-menu has a **Theme** row — one tap cycles
  System → Dark → Light and re-themes every screen instantly, no longer
  depending on the phone app for dark/light mode.
- **Settings survive reinstalls**: on launch the watch now restores the
  phone-side settings (accent color, touch, highlight words) from the
  phone, so a fresh install no longer resets your matchword string.

## 0.3.32

- **Progress bar: big position dot** — the current article position is now
  a large accent dot (8 px, extending into the top bar) instead of the tiny
  thumb; the read portion left of it is a full accent fill, the unread
  remainder stays the muted track. The settle flash is unchanged.

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
