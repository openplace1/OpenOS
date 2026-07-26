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
- [OpenStore and OPK packages](#openstore-and-opk-packages)
- [SDK reference](#sdk-reference)
  - [Screen drawing](#screen-drawing)
  - [Colours](#colours)
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
  - [Lightweight pseudo-3D](#lightweight-pseudo-3d)
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
- [Specs](#specs)
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
│  └─ apps/                  ← legacy system entry paths (see Permissions)
│     ├─ home.osa            ← required: rendered as the home screen
│     ├─ lockscreen.osa      ← required: boot script
│     ├─ controlcenter.osa   ← swipe-down overlay
│     ├─ settings.osa
│     └─ openstore.osa       ← online OPK catalog/install UI
├─ user/
│  └─ config.ini             ← key/value store (Config::get/set)
├─ apps/                     ← per-script sandboxes (auto-created)
│  ├─ <appname>/             ← legacy loose script data
│  └─ pkg_<package_id>/      ← OPK data, stable across app updates/renames
│     ├─ _kv.ini             ← kv.set / kv.get
│     └─ … fwrite() output
├─ wallpapers/               ← BMP files; Settings picks active one
└─ <anything>.osa            ← user scripts with #isApp true become tiles
```

The runtime scans the root one level deep and `/system/apps/` for `.osa`
files with `#isApp true`. Each one becomes a home tile.

Installed OpenStore packages use two additional roots:

| Path | Purpose |
|---|---|
| `/packages/<id>/` | Sandboxed user OPK packages |
| `/system/packages/<id>/` | Approved system-app updates |
| `/apps/pkg_<id>/` | Persistent writable OPK data; kept across updates |

## OpenStore and OPK packages

OpenStore can download its catalog and packages from a separate repository,
web server or CDN. The compiled fallback points at `openplace1/OpenStore`, and
`store_catalog_url` in `/user/config.ini` can override it without rebuilding
the kernel. An `.opk` file is an ordinary ZIP renamed to `.opk`; it contains a
root `manifest.json`, one `.osa` or `.osac` entry point and optional assets.

The bottom navigation separates normal `Apps` from `System Apps`. System
updates require an official `openos.*` package, a `system` manifest scope and a
URL under the configured trusted system-package prefix.

```ini
store_catalog_url=https://example.com/openstore/catalog.json
store_system_prefix=https://example.com/openstore/packages/
```

The second setting is needed only when privileged system OPKs are hosted away
from the compiled default. User-package URLs may point to any valid HTTPS host.

Packages are streamed to SD, checked against the catalog SHA-256, validated,
extracted with strict size/path limits and activated atomically. Reinstalling a
package with a higher `versionCode` updates it while preserving `/apps/` data.
System OPKs update OSA system applications only; they cannot replace the C++
kernel or flash firmware.

The installer rejects package downgrades, duplicate/case-colliding paths,
reserved `openos.*` IDs in user packages, ZIP traversal, unsupported ZIP
features and scope changes. An interrupted install is recovered on the next
boot from staging/backup directories.

> **Security:** catalog SHA-256 detects corruption and a package that differs
> from its catalog entry. The current catalog is not digitally signed yet and
> HTTPS uses the device's insecure/no-CA mode. Do not treat remote system OPKs
> as secure against an active network attacker until release-key signatures are
> added. User OPKs remain unprivileged and permission-gated.

Build packages and regenerate the catalog with `tools/build_opk.py`. The full
format and publishing workflow are documented in `store/README.md`.

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

Limits per script: 128 KB source, 512 lines, 768 bytes per source line,
96 variables, 24 user functions and 10 nested calls. Compiled bytecode is
limited to 8192 bytes, 64 numeric constants, 48 string constants, 64 names and
a 48-value operand stack. Exceeding a compiler pool is a hard compile error;
it never falls through to an invalid `-1` bytecode index.

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

The full **privileged** SDK is granted only to fixed legacy OpenOS entry paths
and recognized entry points of allowlisted `/system/packages/<id>/` packages.
`#exception true` by itself — including in an arbitrary file copied into
`/system/apps/` — does not grant privilege.

Permission decisions are keyed by normalized script/package path, not by the
displayed `#app` name. Two store apps with the same visible name therefore do
not share grants.

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
| `bg565(c)` | Fill the screen or active sprite with packed RGB565 |
| `setcolor(r, g, b)` | Active draw colour for rect / circle / line / triangle |
| `setcolor565(c)` | Same, but packed 16-bit |
| `rect(x, y, w, h)` | Filled rectangle |
| `frame(x, y, w, h)` | Outline rectangle |
| `rrect(x, y, w, h, r)` | Filled rounded rect, radius `r` |
| `rframe(x, y, w, h, r)` | Outline rounded rect |
| `circle(x, y, r)` | Filled circle |
| `ring(x, y, r)` | Outline circle |
| `line(x1, y1, x2, y2)` | Line |
| `hline(x, y, w)` / `vline(x, y, h)` | Fast horizontal / vertical line |
| `thickline(x1,y1,x2,y2,width)` | Anti-aliased wide line |
| `pixel(x, y)` | Single pixel |
| `ellipse(x,y,rx,ry)` / `eframe(x,y,rx,ry)` | Filled / outlined ellipse |
| `triangle(x1,y1,x2,y2,x3,y3)` | Filled triangle |
| `tframe(x1,y1,x2,y2,x3,y3)` | Outline triangle |
| `quad(x1,y1,x2,y2,x3,y3,x4,y4)` | Filled quadrilateral |
| `qframe(x1,y1,x2,y2,x3,y3,x4,y4)` | Outline quadrilateral |
| `arc(x,y,r,width,start,end[,bg565])` | Arc in degrees |
| `gradient(x,y,w,h,r1,g1,b1,r2,g2,b2)` | Vertical RGB gradient |
| `gradienth(x,y,w,h,r1,g1,b1,r2,g2,b2)` | Horizontal RGB gradient |
| `screenw()` / `screenh()` | Returns `240` / `320` |

### Colours

| Call | Returns |
|---|---|
| `color.rgb(r,g,b)` / `color.gray(v)` | Packed RGB565 |
| `color.r(c)` / `color.g(c)` / `color.b(c)` | Approximate 8-bit channel |
| `color.hsv(h,s,v)` | RGB565; hue in degrees, saturation/value in `0..1` |
| `color.lerp(a,b,t)` | RGB565 interpolation |
| `color.lighten(c,t)` / `color.darken(c,t)` | Mix with white / black |
| `color.contrast(c)` | Black or white for readable foreground text |

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
| `textfit(str, width)` | One line shortened with `...` when needed |
| `textblock(x,y,w,str,lineH,scroll,clipTop,clipBottom,[maxLines])` | Draw wrapped, vertically clipped text; returns full content height |

### Touch and gestures

| Call | Returns |
|---|---|
| `touch.down()` | `1` if any finger is on the screen |
| `touch.x()` / `touch.y()` | Current touch position |
| `touch.pressed()` | One-shot when a new touch starts |
| `touch.startX()` / `touch.startY()` | Position when current gesture began |
| `touch.endX()` / `touch.endY()` | Last release position |
| `touch.dx()` / `touch.dy()` | Delta from start |
| `touch.duration()` | ms since gesture began (0 if no touch) |
| `touch.held(ms)` | `1` once the current hold reaches `ms` |
| `touch.moved([px])` | `1` after movement exceeds the threshold |
| `touch.in(x,y,w,h)` | Current finger is inside a rectangle |
| `touch.tap(x,y,w,h)` | One-shot short tap released inside a rectangle |
| `touch.clearTap()` | Discard an unmatched pending tap |
| `touch.released()` | One-shot: `1` once on the frame after release |
| `gesture.swipeUp()` / `swipeDown()` / `swipeLeft()` / `swipeRight()` | One-shot after release with > 40 px travel in that direction |

### Off-screen sprites

For flicker-free animation. Drawing builtins are routed to the sprite
when one is active; `gfx.stash` lets you keep a sprite around while
drawing to the screen, then blit it back on demand.

| Call | Effect |
|---|---|
| `gfx.begin(w, h[, depth])` | Allocate sprite (depth 1, 8 or 16; default 16). Returns `1` ok / `0` failed |
| `gfx.auto(w,h[,maxDepth])` | Safely choose 16, 8 or 1-bit depth; returns selected depth or `0` |
| `gfx.push(x, y)` | Blit active sprite to TFT |
| `gfx.pushClip(x, y, clipX, clipY, clipW, clipH)` | Blit only inside a TFT clipping rectangle |
| `gfx.origin(x, y)` | Translate active-sprite drawing coordinates; useful for reusable scroll stripes |
| `gfx.end()` | Free the sprite and any stashed sprite |
| `gfx.active()` | `1` if a sprite is currently the draw target |
| `gfx.width()` / `gfx.height()` / `gfx.depth()` | Current target properties |
| `gfx.bytes()` | Pixel-buffer bytes used by the active sprite |
| `gfx.stash()` | Detach active sprite, drawing returns to screen, sprite stays in memory |
| `gfx.show(x, y)` | Blit the stashed sprite to TFT |
| `gfx.unstash()` | Re-activate the stashed sprite as draw target |

`gfx.begin` and `gfx.auto` reject allocations that would leave less than 8 KB
of heap or cannot fit in the largest contiguous heap block. For animation on a
non-PSRAM ESP32, prefer a 160×160 8-bit sprite (25.6 KB) over a full-screen
buffer.

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
| `sin(x)`, `cos(x)`, `tan(x)` | Trig (radians) |
| `asin(x)`, `acos(x)`, `atan(x)`, `atan2(y,x)` | Inverse trig |
| `hypot(x,y)`, `dist(x1,y1,x2,y2)`, `angle(x1,y1,x2,y2)` | 2D geometry |
| `log(x)`, `exp(x)` | Natural log / exp |
| `floor(x)`, `ceil(x)`, `round(x)` | Rounding |
| `int(x)` | Truncate to int |
| `random(lo, hi)` | Random int in `[lo, hi)` |
| `randomf(lo,hi)` | Random floating-point value |
| `sign(x)`, `fract(x)`, `finite(x)` | Numeric helpers |
| `radians(deg)` / `degrees(rad)` | Angle conversion |
| `map(v,inLo,inHi,outLo,outHi)` | Remap a range |
| `smoothstep(edge0,edge1,x)` | Smooth `0..1` interpolation |
| `wrap(v,lo,hi)` | Wrap into a repeating range |
| `approach(current,target,amount)` | Move towards a target without overshoot |
| `noise(x)` / `noise2(x,y)` | Deterministic smooth value noise |
| `pi()` / `tau()` | π / 2π |

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
| `indexof(s, sub)` / `lastindexof(s,sub)` | Index or `-1` |
| `left(s,n)` / `right(s,n)` | First / last `n` bytes |
| `slice(s,start,end)` | Slice; negative indexes count from the end |
| `count(s,sub)` | Number of non-overlapping occurrences |
| `char(n)` | One-char string from ASCII code |
| `code(s)` | ASCII code of first char |
| `split(s, delim, n)` | Returns the `n`-th piece, or `""` |
| `splitcount(s,delim)` | Number of pieces |
| `isnumber(s)` | `1` when the whole string is numeric |
| `hex(n[,digits])` / `unhex(s)` | Hex formatting and parsing |
| `repeat(s, n)` | Concatenate `n` copies |
| `padleft(s, n, ch)` / `padright(s, n, ch)` | Pad to length `n` |

### File I/O — sandboxed

Loose-script paths are relative to `/apps/<scriptname>/`; OPK paths use the
collision-safe `/apps/pkg_<package_id>/` directory.

| Call | Returns |
|---|---|
| `fread(path)` | File contents as a string, or `""` |
| `fread(path, offset, length)` | Read one bounded file slice |
| `fsize(path)` | File size in bytes, or `-1` |
| `freadline(path, n)` | n-th line, or `""` |
| `fwrite(path, data)` | `1` ok / `0` fail (overwrites) |
| `fappend(path, data)` | `1` ok / `0` fail (appends a line) |
| `fexists(path)` | 0/1 |
| `fremove(path)` | 0/1 |
| `io.error()` | Last file/asset error |

`fread` is capped at 32 KB per call. Use `fsize` and the offset/length form for
larger files.

Package assets are read-only and remain confined to the active OPK directory.

| Call | Returns |
|---|---|
| `asset.path(path)` | Absolute package path for APIs such as `bmp.thumb` |
| `asset.read(path[, offset, length])` | Asset data, capped at 32 KB |
| `asset.size(path)` | Asset size or `-1` |
| `asset.exists(path)` | 0/1 |

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
| `http.error()` | Last network error text |
| `url_encode(s)` / `url_decode(s)` | URL component conversion |
| `json.get(json, path)` | Value at dotted path, e.g. `"data.0.name"` |
| `json.raw(json, path)` | Raw JSON sub-tree as a string |
| `json.has(json, path)` | 0/1 |
| `json.size(json, path)` | Array length |
| `json.escape(s)` | JSON-safe string content without outer quotes |
| `json.quote(s)` | Escaped JSON string including outer quotes |

HTTP request and response bodies are capped at 24 KB. Both fixed-length and
chunked responses use the same limit; requests also have connect/read timeouts.
Bearer tokens are capped at 4096 bytes, require HTTPS and are consumed by one
request so they cannot leak into a later call to another host. URLs are capped
at 2048 bytes.

### Animation helpers

| Call | Returns |
|---|---|
| `lerp(a, b, t)` | Linear interpolation, `t` ∈ `[0, 1]` |
| `clamp(v, lo, hi)` | `v` clipped to range |
| `ease(t, type)` | Eased value; types: `0` linear, `1` ease-in (quad), `2` ease-out (quad), `3` ease-in-out (cubic), `4` cubic-in, `5` cubic-out |
| `perf.frame([fps])` | Cooperative frame limiter, clamped to 1–60 FPS; returns measured FPS |
| `perf.fps()` | Smoothed measured FPS |
| `perf.delta()` / `perf.frameMs()` | Last frame duration in seconds / ms |

`perf.frame()` resets the VM execution slice and processes system gestures while
waiting. It should be called once per animation loop instead of a busy wait.

### Lightweight pseudo-3D

The 3D API uses perspective projection, back-face culling, simple directional
lighting and a painter-sort for cube faces. It allocates no vertex heap and uses
32-bit float math. Rendering a cube is one native SDK call.

| Call | Effect / return |
|---|---|
| `d3.reset()` | Reset camera, timing and adaptive quality |
| `d3.camera(cx,cy,focal,distance)` | Set projection; camera looks towards positive Z |
| `d3.projectX(x,y,z)` / `d3.projectY(x,y,z)` | Project one coordinate; `-32768` if behind camera |
| `d3.visible(x,y,z)` | Point can be projected |
| `d3.line(x1,y1,z1,x2,y2,z2)` | Projected line in current draw colour |
| `d3.point(x,y,z[,radius])` | Projected point |
| `d3.triangle(x1,y1,z1,x2,y2,z2,x3,y3,z3[,filled])` | Projected triangle |
| `d3.cube(x,y,z,size,rx,ry,rz[,mode[,edge565]])` | Cube; mode `0` wire, `1` solid, `2` solid + edges |
| `d3.grid(y,halfSize,step)` | XZ reference grid, capped at 66 lines |
| `d3.axes(size)` | RGB X/Y/Z axes |
| `d3.frame([fps])` | 3D frame boundary; target is hard-clamped to 12–20 FPS |
| `d3.fps()` / `d3.delta()` | Smoothed FPS / frame delta in seconds |
| `d3.renderMs()` / `d3.faces()` | Last cube render time / visible face count |
| `d3.adaptive(enabled)` | Enable/disable automatic quality fallback |
| `d3.quality()` | `1` full quality, `0` temporary wireframe fallback |

If two consecutive frames exceed the 12 FPS budget, filled cubes temporarily
switch to wireframe. Full quality returns after ten fast frames. The hard 20 FPS
cap prevents a tight animation from monopolising the ESP32. The included
[`cube3d.osa`](sd_content/cube3d.osa) sample uses a 160×160 8-bit sprite and is
the reference workload for the 12–20 FPS target.

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
| `ui.alert(title, body)` | OK popup; text wraps automatically |
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
| `app.launch(path)` | Unload current script, load and run another `.osa` / `.osac` |
| `exit()` | End the script; main router returns to home |
| `wait(ms)` | Sleep, while still processing the universal swipe-up gesture |
| `yield()` | Cooperatively feed the system and reset the execution slice |
| `millis()` | ms since boot |
| `micros()` | µs counter since boot |
| `elapsed(startMs)` | Wrap-safe milliseconds elapsed since `startMs` |
| `sdk.version()` | Numeric SDK compatibility level (currently `2`) |
| `sdk.has(feature)` | Capability check: `d3`, `sprite`, `touch`, `perf`, `http`, `json`, `opk` |

### Privileged — system

`#exception true` does not grant privilege by itself. The kernel grants the
privileged SDK only to fixed legacy system paths and recognized entry points of
allowlisted system OPKs. Cryptographic remote authenticity is a separate,
not-yet-implemented signed-catalog layer.

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
| `heap.free()` / `heap.total()` | Current free / total heap |
| `heap.maxBlock()` | Largest contiguous allocation currently possible |
| `heap.lowWater()` | Lowest free heap observed since boot |
| `heap.fragmentation()` | Approximate fragmentation percentage |
| `uptime()` | Seconds since boot |
| `sdready()` | `1` if SD mounted |
| `battery()` | Mock value (`97`) — CYD has no fuel gauge |

### Privileged — file system

Any absolute path on SD. Use carefully.

| Call | Effect |
|---|---|
| `fs.list(absPath)` | `\|`-separated entries; directories end with `/` |
| `fs.read(path[, offset, length])` | File contents or a bounded slice |
| `fs.size(path)` | File size in bytes |
| `fs.write(path, data)` | Overwrite |
| `fs.append(path, data)` | Append line |
| `fs.exists(path)` / `fs.delete(path)` | 0/1 |
| `fs.mkdir(path)` / `fs.rmdir(path)` | 0/1 |
| `fs.wipe()` | Erase whole SD (after confirmation in the calling script) |

`fs.read` is capped at 32 KB per call. `io.error()` reports limit, seek and
allocation failures.

### Privileged — OpenStore

| Call | Returns |
|---|---|
| `store.catalog()` | Raw catalog JSON (compatibility API; prefer indexed calls to save RAM) |
| `store.source()` / `store.setSource(url)` | Read/change the catalog URL; empty URL restores the default |
| `store.systemSource()` / `store.setSystemSource(prefix)` | Read/change the approved system-package directory |
| `store.refresh()` | Download/validate catalog; returns item count or `-1` |
| `store.count()` | Number of entries in the cached native catalog |
| `store.visibleCount(tab)` | Fast count for `tab`: `0` user apps, `1` system apps |
| `store.visibleItem(tab, slot)` | Catalog index for a visible slot, or `-1` |
| `store.state(i)` | `0` GET, `1` UPDATE, `2` INSTALLED, `3` local version is newer |
| `store.canUninstall(i)` | `1` for an installed user package |
| `store.id(i)` / `store.name(i)` | Catalog identity/display name |
| `store.remoteVersion(i)` / `store.remoteVersionCode(i)` | Published version |
| `store.scope(i)` / `store.summary(i)` | `user`/`system` and short description |
| `store.developer(i)` / `store.owner(i)` | Publisher shown by OpenStore |
| `store.description(i)` | Wrapped long description, maximum 10,000 UTF-8 bytes |
| `store.color(i)` | App icon colour as packed RGB565 |
| `store.url(i)` / `store.sha256(i)` | Selected OPK download metadata |
| `store.install(url, sha256, id, scope, [direct])` | `1` after verified install/update; `direct=1` skips confirmation only for user apps |
| `store.versionCode(id)` | Installed numeric version or `0` |
| `store.version(id)` | Installed display version |
| `store.remove(id, [label])` | Remove a user package after confirmation; optional label is shown to the user |
| `store.error()` | Last catalog/package error |
| `store.restartRequired()` | `1` when Home must be refreshed by reboot |

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
| `bt.enable()` / `bt.disable()` / `bt.enabled()` | Bluetooth Classic toggle; enable/disable return `1` on success |
| `bt.error()` | Last Bluetooth initialization error |
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
| `home.canUninstall(i)` | `1` for a removable user app; system apps and folders return `0` |
| `home.folderCount(i)` | Children count |
| `home.folderAppName(i, j)` / `folderAppColor(i, j)` / `folderAppPath(i, j)` | Child fields |
| `home.swap(i, j)` | Swap two tiles |
| `home.makeFolder(i)` | Wrap tile in a new folder; returns `1` on success |
| `home.deleteFolder(i)` | Restore all children and remove folder; returns `0` if Home has too few free slots |
| `home.uninstall(i)` | Confirm and uninstall a removable app; app data is preserved |
| `home.addToFolder(folderIdx, appIdx)` | Move app into folder; returns `0` if full or allocation fails |
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

| | **Sandbox** (default) | **Privileged** (kernel-approved system entry) |
|---|---|---|
| File I/O | `fread/fwrite` under `/apps/<scriptname>/` | `fs.*` anywhere on SD |
| KV store | `kv.get/set/del` per script | Plus `cfg.get/set/del` system-wide |
| Crypto | — | `crypto.encrypt/decrypt` |
| Wi-Fi / BT control | — | `wifi.*`, `bt.*`, `ntp.sync` |
| System | — | `sys.brightness/theme/setTime/reboot`, `setbright`, `setwallpaper` |
| Home | Read tile data | Mutate (`swap`, `makeFolder`, `deleteFolder`, `addToFolder`, `uninstall`) |
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

## Specs

| Metric | Value |
|---|---|
| C++ source | ~8 000 lines |
| OSA scripts | Loaded from SD / OPK packages |
| Flash | 1.99 MB / 3 MB partition |
| RAM (static) | 95.7 KB |
| Heap headroom at boot | ~60 KB |
| Wallpaper cache | 150 KB (lazy) |
| Per-runtime overhead | ~25 KB (vars + lines + funcs arrays) |

---

## Dependencies

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) — touch
- arduino-esp32 (WiFi, Bluetooth, SD, HTTPClient)

---

## License

MIT. See `LICENSE`.
