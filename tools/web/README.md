# Web UI build

    src/web/MJPEG2SD.htm   src/web/common.js      <- edit these
    data/MJPEG2SD.htm      data/common.js         <- generated, minified
    data/MJPEG2SD.htm.gz   data/common.js.gz      <- generated, what the boards serve

```
npm install                         once
node tools/web/build.mjs            rebuild data/ after any web change
node tools/web/build.mjs --check    fails if data/ is stale - run before committing or uploading
```

Measured on the 6 Sep 2026 page:

| | source | minified | gzipped |
|---|---|---|---|
| `MJPEG2SD.htm` | 278.7 KB | 125.7 KB | **32.0 KB** |
| `common.js` | 56.1 KB | 28.1 KB | **9.0 KB** |

342 KB on the wire became 42 KB, and the comments stayed.

## Why data/ is committed

`setupAssist.cpp` `checkDataFiles()` re-downloads the web files straight from this repo's `data/`
directory on `raw.githubusercontent.com` whenever they are missing from a card, which is what
happens after a `CFG_VER` bump wipes `DATA_DIR`. **`data/` is a board-facing artefact, not a build
output you can gitignore.**

Both forms are committed. Firmware from before the gzip change asks for the plain names, so dropping
them would leave an un-upgraded board with no web UI and only the built-in OTA page to recover
through.

## How the board serves it

`gzipResolve()` in `webServer.cpp` swaps `<name>.gz` in at serve time, so `INDEX_PAGE_PATH`,
`COMMON_JS_PATH` and the page's own `<script src="/web?common.js">` are all unchanged.

- **The compressed form wins.** An uncompressed leftover of the same name would otherwise shadow a
  freshly uploaded `.gz` for ever, with no way to tell from outside which one was being sent.
- No `Accept-Encoding` header means any coding is acceptable (RFC 7231 5.3.4), which covers curl and
  every bench script. A client that sends the header and leaves gzip out gets the uncompressed file
  if one is there, and a 404 if not.
- `Content-Type` is set from the LOGICAL name and describes the decoded body, which is what it means
  alongside a `Content-Encoding`.
- `/file?path=` never resolves. It serves recordings off the card, where a `.gz` would be a file
  rather than an encoding.
- `fileProbe` resolves too. It opens the index page repeatedly, and probing a name that does not
  exist reports 0 free slots and reads as total exhaustion.

## Uploading to a board

Same recipe as always, with the `.gz` names:

```
curl -s "http://<board>/control?startOTA=MJPEG2SD.htm.gz"
curl --data-binary "@data/MJPEG2SD.htm.gz" "http://<board>/upload"
```

A board upgraded from before this change keeps its uncompressed files. They are inert, because the
`.gz` wins, but `/control?deldata=MJPEG2SD.htm` removes them (it restarts the board).

## Minifier settings, and why each one is pinned

`build.mjs` is deliberately conservative. Each setting is there against a hazard this project has
already measured - read this table before loosening any of them.

| setting | why |
|---|---|
| terser `mangle: {toplevel: false}`, `compress: {toplevel: false}` | both files are plain top-level scripts sharing one global scope. The page calls `sendControl`, `show`, `hide`, `isOn`, `$` and more from `common.js`, and `common.js` calls `processStatus`, `customInit` and `processBuffer` back |
| terser `mangle.properties` off | `handlers[type]` is looked up by a string off the websocket, every `/status` key reaches an element by id, and `processStatus` compares key strings |
| clean-css **level 1** | level 2 merges and reorders rules, and this stylesheet has live cascade-order dependencies - the `max-width: 30rem` block has to stay after the phone block |
| html-minifier leaves ids, classes and attribute values alone | `updateStatus()` finds an element whose id equals a status key, the click dispatch routes on `e.id` and `e.classList.value`, and `showView()` prefix-matches `<option>` TEXT against a frame size name |
| `gzipSync(..., {mtime: 0})` | a gzip header carrying the build time would make every rebuild look like a change to `--check` and to git |

The page has no `eval`, no `new Function` and no `window[name]` dispatch, which is what makes local
renaming safe. It does have 14 regex literals, which is why this uses a real parser rather than a
regex comment stripper - a stripper would also have to survive `"://"` inside a string.
