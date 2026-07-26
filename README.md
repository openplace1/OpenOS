# OpenOS

ESP32 firmware that runs `.osa` scripts from an SD card. The kernel,
script interpreter and SDK are in C++. The UI — home screen, lockscreen,
settings, every app — is a script. Edit a file, restart, the change is
live. No reflash.

---

## Table of contents

- [Hardware](#hardware)
- [Installation](#installation)
- [Architecture](#architecture)
- [SD card layout](#sd-card-layout)
- [The `.osa` language](#the-osa-language)
  - [Header directives](#header-directives)
  - [Syntax](#syntax)
- [Permissions](#permissions)
- [SDK reference](#sdk-reference)
  - [Screen drawing](#screen-drawing)
  - [Text](#text)
  - [Touch and gestures](#touch-and-gestures)
  - [Off-screen sprites](#off-screen-sprites)
  - [Time](#time)
  - [Math](#math)
  - [Strings](#strings)
  - [File I/O — sandboxed](#file-io--sandboxed)
  - [Key–value storage — sandboxed](#keyvalue-storage--sandboxed)
  - [HTTP and JSON](#http-and-json)
  - [Animation helpers](#animation-helpers)
  - [Wallpaper](#wallpaper)
  - [Theme palette](#theme-palette)
  - [UI widgets](#ui-widgets)
  - [Notifications](#notifications)
  - [App control](#app-control)
  - [Privileged — system](#privileged--system)
  - [Privileged — file system](#privileged--file-system)
  - [Privileged — Wi-Fi and Bluetooth](#privileged--wi-fi-and-bluetooth)
  - [Privileged — config](#privileged--config)
  - [Privileged — crypto](#privileged--crypto)
  - [Privileged — apps](#privileged--apps)
  - [Privileged — home](#privileged--home)
  - [Misc](#misc)
- [Sandbox vs privileged](#sandbox-vs-privileged)
- [Building from source](#building-from-source)
- [Dependencies](#dependencies)
- [License](#license)

---

## Hardware

| Part | Notes |
|---|---|
| MCU | ESP32 (denky32 / generic ESP32, no PSRAM required) |
| Display | 240×320 TFT, ILI9341 over VSPI |
| Touch | XPT2046 resistive (separate bus pins) |
| Storage | microSD over HSPI |
| Optional | Wi-Fi + Bluetooth (built into ESP32) |

Tested on the "Cheap Yellow Display" (CYD) board.

---

## Installation

1. **Flash the firmware.** Use PlatformIO (`pio run --target upload`) or
   `esptool.py`. The image goes to flash offset `0x10000`.
2. **Prepare the SD card.** Copy the contents of `sd_content/` from this
   repo to the root of the card. The expected layout is described
   [below](#sd-card-layout).
3. **Insert and power on.** Boot loads `/system/apps/lockscreen.osa`; on
   unlock it transitions to `/system/apps/home.osa`. If either is missing
   the screen shows a red error.

---

## Architecture

```
┌─ Arduino entry (setup / loop) ───────────────────────────┐
│  ├─ Hardware init (TFT, touch, SD, WiFi, BT)             │
│  ├─ State router (LOCKSCREEN / HOMESCREEN / IN_APP / CC) │
│  └─ Wallpaper cache (~150 KB)                            │
│                                                          │
│  ┌─ OSAApp #1 (active script) ───┐                       │
│  │  OSARuntime — interpreter,    │  ◄── /system/apps/lockscreen.osa
│  │  variables, function calls,   │      /system/apps/home.osa
│  │  100+ SDK builtins            │      any tapped tile
│  └────────────────────────────────┘                      │
│  ┌─ OSAApp #2 (overlay) ─────────┐                       │
│  │  Control Center, allocated on │  ◄── /system/apps/controlcenter.osa
│  │  swipe-down, freed on close   │                       │
│  └────────────────────────────────┘                      │
└──────────────────────────────────────────────────────────┘
```

Only the boxes above are C++. Everything users see is a script.

---

## SD card layout

```
SD root
├─ system/
│  └─ apps/                  ← privileged scripts (see Permissions)
│     ├─ home.osa            ← required: rendered as the home screen
│     ├─ lockscreen.osa      ← required: boot script
│     ├─ controlcenter.osa   ← swipe-down overlay
│     ├─ settings.osa
│     └─ … any other system app
├─ user/
│  └─ config.ini             ← key/value store (Config::get/set)
├─ apps/                     ← per-script sandboxes (auto-created)
│  └─ <appname>/             ← script's own working directory
│     ├─ _kv.ini             ← kv.set / kv.get
│     └─ … fwrite() output
├─ wallpapers/               ← BMP files; Settings picks active one
└─ <anything>.osa            ← user scripts with #isApp true become tiles
```

The runtime scans the root one level deep and `/system/apps/` for `.osa`
files with `#isApp true`. Each one becomes a home tile.

---

## The `.osa` language

### Header directives

Optional, must be near the top of the file.

| Directive | Effect |
|---|---|
| `#app "Name"` | Display name on the home tile and app header |
| `#appColor "#RRGGBB"` | Tile colour (default = system orange) |
| `#isApp true` | Show on the home screen as a tile |
| `#isApp false` | Background / system script, no tile |
| `#perm name,name,…` | Declare required permissions (see [Permissions](#permissions)) |

### Syntax

| Construct | Form |
|---|---|
| Comment | `# anything to end of line` |
| Local declaration | `var x = 5` |
| Assignment | `x = x + 1` |
| If / elif / else | `if cond then … elif cond then … else … end` |
| While | `while cond do … end` |
| For | `for i = 0 to 10 do … end` |
| Main loop | `loop … end` (one per script, top-level only) |
| Break / continue | `break`, `continue` (inside `while` / `for`) |
| Function definition | `def name(arg1, arg2) … end` |
| Function call | `name(arg1, arg2)` |
| Early return | `return value` |
| Numeric literal | `123`, `3.14`, `-5` |
| String literal | `"hello"` (no escape sequences besides `\"`) |
| String concat | `a + b` (auto-coerces) |
| Comparison | `==  !=  <  >  <=  >=` |
| Logic | `and`, `or`, `not` (`!` also accepted) |
| Arithmetic | `+ - * / %` (`/` is float, use `int(a/b)` for integer) |

Limits per script: 512 lines, 96 variables, 24 user functions, 10 deep
call stack.

---

## Permissions

| Bit | Name | Grants |
|----:|------|--------|
| 1 | `notify` | `notify()` |
| 2 | `network` | `http.get`, `http.post` |
| 4 | `system` | `setbright`, `setwallpaper` |
| 8 | `overlay` | `overlay.draw` (planned — draw on top of any app) |

Declare with `#perm notify,network` etc. The runtime prompts the user
the first time a script touches a permission and remembers the choice in
`Config`. Toggle later from **Settings → Applications**.

Scripts located under `/system/apps/` automatically receive the full
**privileged** SDK in addition (anything labelled "privileged" below).
Drop a script there at your own risk — there is no sandbox.

---

## SDK reference

`x, y, w, h` are pixel coordinates, `0,0` is top-left, screen is 240×320.
Colours are 8-bit `r, g, b` channels unless suffixed `565` (packed 16-bit).
Return values: `0` / `1` for booleans, numeric otherwise. Strings are
plain `String` instances.

### Screen drawing

| Call | Effect |
|---|---|
| `cls()` / `clear()` | Fill the screen with theme background |
| `bg(r, g, b)` | Fill the screen with a colour |
| `setcolor(r, g, b)` | Active draw colour for rect / circle / line / triangle |
| `setcolor565(c)` | Same, but packed 16-bit |
| `rect(x, y, w, h)` | Filled rectangle |
| `frame(x, y, w, h)` | Outline rectangle |
| `rrect(x, y, w, h, r)` | Filled rounded rect, radius `r` |
| `rframe(x, y, w, h, r)` | Outline rounded rect |
| `circle(x, y, r)` | Filled circle |
| `ring(x, y, r)` | Outline circle |
| `line(x1, y1, x2, y2)` | Line |
| `pixel(x, y)` | Single pixel |
| `triangle(x1,y1,x2,y2,x3,y3)` | Filled triangle |
| `tframe(x1,y1,x2,y2,x3,y3)` | Outline triangle |
| `gradient(x,y,w,h,r1,g1,b1,r2,g2,b2)` | Vertical RGB gradient |
| `screenw()` / `screenh()` | Returns `240` / `320` |

### Text

| Call | Effect |
|---|---|
| `textcolor(r, g, b)` / `textcolor565(c)` | Active text colour |
| `fontsize(n)` | TFT_eSPI font (1, 2, 4, 6, 7) |
| `text(x, y, str)` | Top-left anchored |
| `textc(x, y, str)` | Middle-centre anchored |
| `textr(x, y, str)` | Top-right anchored |
| `textml(x, y, str)` | Middle-left anchored |
| `textmr(x, y, str)` | Middle-right anchored |
| `textw(str)` | Measured width in px at current font |
| `texth()` | Current font line height in px |

### Touch and gestures

| Call | Returns |
|---|---|
| `touch.down()` | `1` if any finger is on the screen |
| `touch.x()` / `touch.y()` | Current touch position |
| `touch.startX()` / `touch.startY()` | Position when current gesture began |
| `touch.dx()` / `touch.dy()` | Delta from start |
| `touch.duration()` | ms since gesture began (0 if no touch) |
| `touch.released()` | One-shot: `1` once on the frame after release |
| `gesture.swipeUp()` / `swipeDown()` / `swipeLeft()` / `swipeRight()` | One-shot after release with > 40 px travel in that direction |

### Off-screen sprites

For flicker-free animation. Drawing builtins are routed to the sprite
when one is active; `gfx.stash` lets you keep a sprite around while
drawing to the screen, then blit it back on demand.

| Call | Effect |
|---|---|
| `gfx.begin(w, h[, depth])` | Allocate sprite (depth 1, 8 or 16; default 16). Returns `1` ok / `0` failed |
| `gfx.push(x, y)` | Blit active sprite to TFT |
| `gfx.end()` | Free the sprite and any stashed sprite |
| `gfx.active()` | `1` if a sprite is currently the draw target |
| `gfx.stash()` | Detach active sprite, drawing returns to screen, sprite stays in memory |
| `gfx.show(x, y)` | Blit the stashed sprite to TFT |
| `gfx.unstash()` | Re-activate the stashed sprite as draw target |

### Time

`getLocalTime()` after `ntp.sync()` is required for non-zero values.

| Call | Returns |
|---|---|
| `time.hour()` / `min()` / `sec()` | Current local time fields |
| `time.day()` / `month()` / `year()` | Date fields (1-based month, 4-digit year) |
| `time.weekday()` | 0 = Sunday … 6 = Saturday |
| `time.now()` | Unix timestamp (seconds) |
| `time.synced()` | `1` if NTP has run successfully |
| `time.fmtHM()` | `"HH:MM"` |
| `time.fmtHMS()` | `"HH:MM:SS"` |
| `time.fmtDate()` | `"DD.MM.YYYY"` |

### Math

| Call | Description |
|---|---|
| `abs(x)`, `min(a,b)`, `max(a,b)` | Standard |
| `sqrt(x)`, `pow(a, b)` | Powers |
| `sin(x)`, `cos(x)`, `tan(x)`, `atan2(y, x)` | Trig (radians) |
| `log(x)`, `exp(x)` | Natural log / exp |
| `floor(x)`, `ceil(x)`, `round(x)` | Rounding |
| `int(x)` | Truncate to int |
| `random(lo, hi)` | Random int in `[lo, hi)` |
| `pi()` | 3.14159… |

### Strings

| Call | Description |
|---|---|
| `str(x)` / `num(s)` | Cast |
| `len(s)` | Length |
| `upper(s)` / `lower(s)` / `trim(s)` | Transform |
| `substr(s, start, len)` | Substring |
| `replace(s, find, with)` | Replace all |
| `contains(s, sub)` | 0/1 |
| `startswith(s, p)` / `endswith(s, p)` | 0/1 |
| `indexof(s, sub)` | Index or `-1` |
| `char(n)` | One-char string from ASCII code |
| `code(s)` | ASCII code of first char |
| `split(s, delim, n)` | Returns the `n`-th piece, or `""` |
| `repeat(s, n)` | Concatenate `n` copies |
| `padleft(s, n, ch)` / `padright(s, n, ch)` | Pad to length `n` |

### File I/O — sandboxed

All paths are relative to `/apps/<scriptname>/`.

| Call | Returns |
|---|---|
| `fread(path)` | File contents as a string, or `""` |
| `freadline(path, n)` | n-th line, or `""` |
| `fwrite(path, data)` | `1` ok / `0` fail (overwrites) |
| `fappend(path, data)` | `1` ok / `0` fail (appends a line) |
| `fexists(path)` | 0/1 |
| `fremove(path)` | 0/1 |

### Key–value storage — sandboxed

Persisted to `/apps/<scriptname>/_kv.ini`. Up to 32 entries per script.

| Call | Returns |
|---|---|
| `kv.get(key, default)` | Value or default |
| `kv.set(key, value)` | `1` ok |
| `kv.del(key)` | `1` ok |

### HTTP and JSON

Requires `#perm network` and an active Wi-Fi connection.

| Call | Returns |
|---|---|
| `http.bearer(token)` | Set `Authorization: Bearer …` for next call |
| `http.get(url)` | Response body, sets `http.status()` |
| `http.post(url, body)` | Same, with body |
| `http.status()` | HTTP status code from the last call |
| `url_encode(s)` | URL-encoded string |
| `json.get(json, path)` | Value at dotted path, e.g. `"data.0.name"` |
| `json.raw(json, path)` | Raw JSON sub-tree as a string |
| `json.has(json, path)` | 0/1 |
| `json.size(json, path)` | Array length |

### Animation helpers

| Call | Returns |
|---|---|
| `lerp(a, b, t)` | Linear interpolation, `t` ∈ `[0, 1]` |
| `clamp(v, lo, hi)` | `v` clipped to range |
| `ease(t, type)` | Eased value; types: `0` linear, `1` ease-in (quad), `2` ease-out (quad), `3` ease-in-out (cubic), `4` cubic-in, `5` cubic-out |

### Wallpaper

| Call | Effect |
|---|---|
| `wallpaper.draw()` | Full-screen wallpaper from cache |
| `wallpaper.region(x, y, w, h)` | Just that strip |
| `setwallpaper(path)` | **Privileged** — set active wallpaper BMP |
| `getwallpaper()` | Returns current path |

### Theme palette

Returns the current dark/light variant as a packed RGB565 colour. Pair
with `setcolor565` / `textcolor565` to stay theme-consistent.

| Call | Use |
|---|---|
| `theme()` | Returns `0` (light) or `1` (dark) |
| `theme.bg()` | Screen background |
| `theme.surface()` | Card / row background |
| `theme.header()` | Top bar |
| `theme.divider()` / `theme.divider2()` | Lines |
| `theme.text()` | Primary text |
| `theme.subtext()` | Secondary text |
| `theme.hint()` | Tertiary / placeholder text |

### UI widgets

Blocking — they paint full-screen and return when the user picks. All
support the universal swipe-up gesture and return `-1` if the user
swiped away.

| Call | Returns |
|---|---|
| `ui.header(title)` | Paint the standard top header |
| `ui.backHeader(title)` | Header with a `< Back` button |
| `ui.backTapped()` | Non-blocking — `1` if `< Back` zone tapped |
| `ui.alert(title, body)` | OK popup |
| `confirm(title, body, [danger])` | `1` OK / `0` Cancel; red button when `danger=1` |
| `ui.menu(items_pipe, title, [showBack])` | Pick from a `\|`-separated list; index or `-1` |
| `ui.menuStart(title, [showBack])` | Begin a rich Settings-style menu |
| `ui.menuRow(label, letter, r, g, b, value)` | Add a row |
| `ui.menuShow()` | Render + wait for tap; index or `-1`. Scrollable when rows overflow |
| `ui.slider(label, min, max, val)` | New value, or `-1` Cancel |
| `ui.toggle(label, current)` | `0` / `1` |
| `ui.segmented(label, "A\|B\|C", current)` | Selected index |
| `ui.numpad(prompt, maxDigits)` | Entered digits as string, or `""` |
| `input(prompt, default, [multiLine])` | Text input with on-screen keyboard |
| `bmp.thumb(path, x, y, w, h)` | Draw a downsampled 24-bit BMP |

### Notifications

| Call | Effect |
|---|---|
| `notify(msg)` | No-op in current version (NotificationService was removed; the builtin stays for back-compat) |

### App control

| Call | Effect |
|---|---|
| `app.launch(path)` | Unload current script, load and run another `.osa` |
| `exit()` | End the script; main router returns to home |
| `wait(ms)` | Sleep, while still processing the universal swipe-up gesture |
| `millis()` | ms since boot |

### Privileged — system

`#exception` is auto-set for scripts under `/system/apps/`.

| Call | Effect |
|---|---|
| `sys.brightness(n)` | Set backlight (0–255) |
| `sys.theme(n)` | `0` light / `1` dark |
| `sys.wallpaper(path)` | Set wallpaper BMP, invalidate cache |
| `sys.setTime(unix_ts)` | Override RTC |
| `sys.reboot()` | `ESP.restart()` |
| `sys.notify(msg)` | Alias for `notify()` |
| `setbright(n)` | Requires `#perm system` |
| `getbright()` | Read current backlight |
| `freeram()` | `ESP.getFreeHeap()` |
| `uptime()` | Seconds since boot |
| `sdready()` | `1` if SD mounted |
| `battery()` | Mock value (`97`) — CYD has no fuel gauge |

### Privileged — file system

Any absolute path on SD. Use carefully.

| Call | Effect |
|---|---|
| `fs.list(absPath)` | `\|`-separated entries; directories end with `/` |
| `fs.read(path)` | Full file contents |
| `fs.write(path, data)` | Overwrite |
| `fs.append(path, data)` | Append line |
| `fs.exists(path)` / `fs.delete(path)` | 0/1 |
| `fs.mkdir(path)` / `fs.rmdir(path)` | 0/1 |
| `fs.wipe()` | Erase whole SD (after confirmation in the calling script) |

### Privileged — Wi-Fi and Bluetooth

| Call | Description |
|---|---|
| `wifi.enable()` / `wifi.disable()` | Toggle radio |
| `wifi.isEnabled()` / `wifi.connected()` | 0/1 |
| `wifi.ssid()` / `wifi.ip()` / `wifi.rssi()` | Connection info |
| `wifi.scan()` | Number of networks found |
| `wifi.scanSsid(i)` / `wifi.scanRssi(i)` / `wifi.scanSecure(i)` | Per-result |
| `wifi.connect(ssid, pass)` | Returns `1` on success |
| `wifi.disconnect()` | Drops connection |
| `wifi.save(ssid, pass)` | Stores encrypted credentials in Config |
| `bt.enable()` / `bt.disable()` / `bt.enabled()` | Bluetooth Classic toggle |
| `ntp.sync()` | Sync RTC via SNTP |

### Privileged — config

Persisted to `/user/config.ini`. Up to 48 keys system-wide.

| Call | Returns |
|---|---|
| `cfg.get(key, default)` | Value or default |
| `cfg.set(key, value)` | Save |
| `cfg.del(key)` | Remove |

### Privileged — crypto

XOR encryption — *not* cryptographically secure, only obscures.

| Call | Returns |
|---|---|
| `crypto.encrypt(plaintext)` | Hex string |
| `crypto.decrypt(hex)` | Original string |

### Privileged — apps

Used by Settings to list installed scripts and toggle permissions.

| Call | Returns |
|---|---|
| `apps.scan()` | Number of `.osa` apps on SD |
| `apps.name(i)` / `apps.path(i)` | Per-app |
| `apps.needsPerm(i, bit)` | 0/1 — does the manifest declare this perm? |
| `apps.hasPerm(i, bit)` | 0/1 — is it currently granted? |
| `apps.togglePerm(i, bit)` | Flip and persist |

### Privileged — home

Lets `home.osa` read and mutate the tile grid. Folder children are
copies, not references.

| Call | Returns |
|---|---|
| `home.appCount()` | Number of top-level tiles |
| `home.appName(i)` / `home.appColor(i)` / `home.appPath(i)` | Tile fields |
| `home.appIsFolder(i)` | 0/1 |
| `home.folderCount(i)` | Children count |
| `home.folderAppName(i, j)` / `folderAppColor(i, j)` / `folderAppPath(i, j)` | Child fields |
| `home.swap(i, j)` | Swap two tiles |
| `home.makeFolder(i)` | Wrap tile in a new folder |
| `home.deleteFolder(i)` | Remove folder, restore children to end of grid |
| `home.addToFolder(folderIdx, appIdx)` | Move app into folder |
| `home.saveOrder()` | Persist current arrangement |
| `home.iconX(i)` / `home.iconY(i)` | Geometry helpers |
| `anim.openTile(i)` | Record tile for the close-anim coordinates |

### Misc

| Call | Description |
|---|---|
| `print(x)` | `Serial.println(x)` — debug only, doesn't draw |

---

## Sandbox vs privileged

Two execution contexts:

| | **Sandbox** (default) | **Privileged** (`/system/apps/*.osa`) |
|---|---|---|
| File I/O | `fread/fwrite` under `/apps/<scriptname>/` | `fs.*` anywhere on SD |
| KV store | `kv.get/set/del` per script | Plus `cfg.get/set/del` system-wide |
| Crypto | — | `crypto.encrypt/decrypt` |
| Wi-Fi / BT control | — | `wifi.*`, `bt.*`, `ntp.sync` |
| System | — | `sys.brightness/theme/setTime/reboot`, `setbright`, `setwallpaper` |
| Home | Read tile data | Mutate (`swap`, `makeFolder`, `deleteFolder`, `addToFolder`) |
| Apps | — | `apps.scan/needsPerm/hasPerm/togglePerm` |

User scripts opt into specific permissions (`notify`, `network`, …) and
get a prompt the first time they use one. System scripts get the lot.

---

## Building from source

```sh
git clone https://github.com/openplace1/OpenOS
cd OpenOS
pio run --target upload
```

`platformio.ini` is preconfigured for `denky32` board with the
`huge_app.csv` partition (single 3 MB app slot). Switch to
`min_spiffs.csv` (or a custom OTA-capable table) when adding OTA.

The C++ sources live under `src/`. The runtime is in `src/Runtime/`; the
host kernel in `src/main.cpp` plus a small `Applications/` layer for the
data store (home grid, wallpaper cache, theme palette).

---

## Dependencies

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) — touch
- arduino-esp32 (WiFi, Bluetooth, SD, HTTPClient)

---

## License

MIT. See `LICENSE`.