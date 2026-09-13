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
- [Memory on a board without PSRAM](#memory-on-a-board-without-psram)
- [Firmware OTA updates](#firmware-ota-updates)
- [SDK reference](#sdk-reference)
  - [Screen drawing](#screen-drawing)
  - [Custom shapes](#custom-shapes)
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
  - [Immediate-mode widgets](#immediate-mode-widgets)
  - [Notifications](#notifications)
  - [Buffers, sockets and pixels](#buffers-sockets-and-pixels)
  - [App control](#app-control)
  - [Privileged — system](#privileged--system)
  - [Privileged — file system](#privileged--file-system)
  - [Privileged — OpenStore](#privileged--openstore)
  - [Privileged — firmware OTA](#privileged--firmware-ota)
  - [Privileged — Wi-Fi](#privileged--wi-fi)
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
| Display | 240×320 TFT, ILI9341_2 over VSPI: MISO 12, MOSI 13, SCLK 14, CS 15, DC 2, reset not connected |
| Backlight | GPIO 21, active high |
| Touch | XPT2046 resistive on a separate remapped SPI bus: IRQ 36, MOSI 32, MISO 39, CLK 25, CS 33 |
| Storage | microSD over HSPI: CS 5, MOSI 23, MISO 19, SCLK 18 |
| Optional | Wi-Fi (built into ESP32; Bluetooth is not used — its stack was dropped in 1.6 to free ~180 KB of flash and 80 KB of RAM) |

Tested on the "Cheap Yellow Display" (CYD) board.
The TFT_eSPI setup is pinned in `platformio.ini`, so no global library
`User_Setup.h` needs to be edited.

---

## Installation

1. **Flash the complete firmware layout.** Use PlatformIO
   (`pio run --target upload`). OpenOS 1.1.0 changes the flash from the legacy
   single application slot to two OTA slots, so an existing device needs this
   one-time USB flash including `partitions.bin`. Writing only `firmware.bin`
   at `0x10000` does not migrate the partition table.
2. **Prepare the SD card.** Copy the contents of `sd_content/` from this
   repo to the root of the card. The expected layout is described
   [below](#sd-card-layout).
3. **Insert and power on.** Boot loads `/system/apps/lockscreen.osa`; on
   unlock it transitions to `/system/apps/home.osa`. If either is missing
   the screen shows a red error.
4. **Update Settings.** Open OpenStore's **System Apps** tab and install
   Settings 1.1.0. Its Software Update page requires OpenOS code 2 / OSA SDK 3.

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
The optional integer fields `minSdk` and `minOpenOS` declare the oldest
supported OSA SDK and OpenOS version code. Missing fields mean `1` for
compatibility with existing packages.

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

> **Security:** the catalog is signed with the same ECDSA P-256 release key
> as firmware updates (`keyId` + `signature`, verified by
> `PackageManager::verifyCatalogSignature` before any URL or hash is read),
> and every OPK must match the SHA-256 recorded in that signed catalog. The
> official host is additionally pinned to ISRG Root X1 (see
> `Runtime/OpenOSTrustAnchors.h`). An unsigned or re-signed catalog — from
> the official feed or a custom `store_catalog_url` — is rejected with
> "Store catalog is not signed". User OPKs remain unprivileged and
> permission-gated.

Build packages and regenerate the catalog with `tools/build_opk.py`. The full
format and publishing workflow are documented in `store/README.md`.

## Memory on a board without PSRAM

One HTTPS session is the largest thing OpenOS does. mbedTLS here has fixed
16 KB record buffers and allocates two of them, so a handshake needs roughly
45 KB of heap *in one piece* — more than the free heap contains in one piece
once an application is running. Three rules keep that possible, and getting
any of them wrong produces errors that name the wrong culprit.

**One reserved block.** `Runtime/HeapReserve` claims the largest block it can
(45 KB, down to a 35 KB floor) at boot, while the heap is still unbroken, and
lends it to the few consumers that need contiguous memory: a TLS session, a
3D sprite, the OPK inflate window, a script's source text. Small allocations
never land in it, so it stays whole.

**Borrow, do not release, when debris would be left behind.** `release()`
returns the block to the allocator; whatever the consumer allocates next
comes out of that region, and anything still alive when the consumer
finishes leaves a hole that can never be reclaimed. Script loading therefore
uses `borrow()`, which hands out the block without freeing it, and a
downloaded manifest or catalog reserves its buffer *before* the block is
released. Both were real bugs: compiling one application, or one update
check, used to shrink the reserve permanently and the next transfer failed.

**Verification runs while the buffers are held.** The trust store is parsed
and kept for the whole handshake, and the chain is checked with both record
buffers still allocated. On this board that fits with roughly 72 KB free and
does not at 66 KB, which is what an update check from inside a running
application has. So pinning is attempted only when the headroom is there:
one certificate is pinned rather than a chain of roots
(`Runtime/OpenOSTrustAnchors.h`), and when it does not fit — or the chain is
rejected — `SecureHttp` logs why and continues unpinned. The release
signature is what authenticates content; a hardening layer must not be able
to stop updates.

Failures in this area are reported with the free heap and the largest block
at the moment they happened, because "certificate verification failed" and
"connection refused" are both what a shortage of contiguous memory looks
like from the outside.

---

## Firmware OTA updates

`partitions_ota.csv` provides `ota_0` and `ota_1`, each `0x1F0000`
(2,031,616 bytes). An update is streamed into the inactive slot and activated
only after its exact signed size and SHA-256 are verified. `Update.end(false)`
performs the ESP image checks without bypassing validation.

The default manifest is
`https://raw.githubusercontent.com/openplace1/OpenStore/main/update/info.json`.
It is a flat, maximum-4-KB JSON object signed with ECDSA P-256. The matching
release public key is compiled into OpenOS, so authenticity comes from that
signature and the signed firmware hash. On top of that the official host is
pinned: `Runtime/OpenOSTrustAnchors.h` carries the three certificates that
issue the GitHub raw CDN chain (Let's Encrypt YR1, ISRG Root YR, ISRG Root
X1), and mbedTLS verifies the chain and host name of
`*.githubusercontent.com` against them. Pinning the leaf's own issuer matters
on this board: it makes verification a single RSA-2048 check instead of a
walk through two RSA-4096 certificates, which does not fit in the heap left
over once mbedTLS holds both record buffers. Pinning is defence in depth — if
the pinned handshake is rejected the device logs it and retries unpinned,
because the release signature, not the certificate, is what authenticates the
content. The official URL accepts only the `stable` channel.

Settings can store another HTTPS `info.json` URL in NVS, like OpenStore can
change its catalog. Custom feeds may publish `stable`, `beta` or `dev`, but
they must still be signed by the release key embedded in this firmware; hosts
outside the pinned list are reached without certificate verification, which
is exactly why the documents themselves stay signed. A URL change, install
and manual rollback each require a native on-device confirmation; those
mutating calls are restricted to Settings.

After the new slot starts, OpenOS waits until display, touch, SD/application
startup completes and the main loop remains healthy for eight seconds before
marking the image valid. A failed probation boot is rolled back by the ESP32
bootloader. Settings also exposes manual restore while the previous slot is
still bootable.

Manifest and firmware transfers go through `Runtime/SecureHttp`, shared with
OpenStore. Before a handshake it keeps the Wi-Fi
modem out of power save, resolves the host up front and verifies that the
heap can hold mbedTLS's two 16 KB record buffers (about 46 KB free with two
17 KB blocks, 52 KB when a pinned root has to be parsed). `Runtime/HeapReserve`
keeps a 45 KB block claimed from boot and lends it out here: mbedTLS carves
both record buffers out of one region and then verifies an RSA-4096 signature
while holding them, so a fragmented heap makes the handshake fail with a
misleading "certificate verification failed". Transport failures
are retried and reported by cause — DNS, TCP, TLS allocation, handshake or a
certificate that does not chain to the pinned root — instead of HTTPClient's
generic "connection refused"; the serial log prints the same detail with the
heap figures at each attempt.

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
| For | `for i = 0 to 10 do … end` (legacy `for i in 0..10 do … end` is also accepted) |
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

Runtime limits per script: 128 KB source, 768 lines, 4096 bytes per source line,
96 variables, 24 user functions and 10 nested calls. Compiled bytecode is
limited to 12288 bytes, 192 numeric constants, 384 string constants, 320 names and
a 48-value operand stack. Exceeding a compiler pool is a hard compile error;
it never falls through to an invalid `-1` bytecode index.
The OpenStore publisher applies a stricter 768-byte line limit to packaged OSA
source so packages remain within the supported publishing profile.

Compiled bytecode is cached on the SD card (`/system/cache/<hash>.osac`, one
entry per script path, validated against the source's size, modification time
and the firmware version), so a script only pays for parsing and compiling the
first time it runs on a given firmware. Privilege still comes from the script's
path, never from anything inside a cache file.

---

## Permissions

| Bit | Name | Grants |
|----:|------|--------|
| 1 | `notify` | `notify()` |
| 2 | `network` | `http.get`, `http.post` |
| 4 | `system` | `setbright`, `setwallpaper` |

Bit 8 is reserved (an `overlay` permission was planned but never implemented)
so stored grant/deny masks keep their meaning.

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
| `arc(x,y,r,width,start,end[,bg565])` | Arc in degrees, clockwise from 6 o'clock |
| `pie(x,y,r,start,end)` | Filled sector, same angle convention as `arc` |
| `pill(x, y, w, h)` / `pillframe(x, y, w, h)` | Fully rounded rectangle — the shape of a modern button |
| `rrect4(x,y,w,h,tl,tr,br,bl)` | Filled rounded rect with one radius per corner |
| `rframe4(x,y,w,h,tl,tr,br,bl)` | Outline of the same |
| `star(x,y,points,outerR,innerR[,rot])` | Filled star; `rot` in degrees clockwise |
| `sframe(x,y,points,outerR,innerR[,rot[,width]])` | Outline star |
| `ngon(x,y,sides,r[,rot])` | Filled regular polygon |
| `nframe(x,y,sides,r[,rot[,width]])` | Outline regular polygon |
| `gradient(x,y,w,h,r1,g1,b1,r2,g2,b2)` | Vertical RGB gradient |
| `gradienth(x,y,w,h,r1,g1,b1,r2,g2,b2)` | Horizontal RGB gradient |
| `smooth(on)` | `1` makes `rrect`, `rframe`, `circle`, `ring`, `pill` and `pie` blend their edges against what is already on screen. Off by default: every blended pixel is a read-back over SPI when drawing straight to the panel, so use it for chrome, not for per-frame sprites |
| `screenw()` / `screenh()` | Returns `240` / `320` |

### Custom shapes

One path of up to 48 points, filled under the even-odd rule so concave and
self-intersecting outlines render the way a vector tool would draw them.
Points are kept as floats, so a shape can be built once and transformed every
frame without drift.

| Call | Effect |
|---|---|
| `path.begin()` | Start a new path |
| `path.to(x, y)` | Append a point; returns the point count |
| `path.count()` | Number of points |
| `path.x(i)` / `path.y(i)` | Read a point back, e.g. for hit tests |
| `path.fill()` | Fill the polygon with the draw colour |
| `path.stroke([width], [closed])` | Outline it; `width > 1` is anti-aliased, `closed` defaults to `1` |
| `path.move(dx, dy)` | Translate every point |
| `path.rotate(cx, cy, deg)` | Rotate every point about a centre, clockwise |
| `path.scale(cx, cy, fx, [fy])` | Scale about a centre |

```
# A spaceship that turns with the finger
path.begin()
path.to(0, -14)
path.to(10, 12)
path.to(0, 6)
path.to(-10, 12)
path.move(120, 160)
loop
  cls()
  path.rotate(120, 160, 3)
  setcolor(255, 149, 0)
  path.fill()
  setcolor(255, 255, 255)
  path.stroke(2)
  wait(30)
end
```

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

Persisted to `/apps/<scriptname>/_kv.ini`. Up to 24 entries per script.

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

The 3D API uses perspective projection, back-face culling, directional
lighting and a painter's sort. Immediate calls (`d3.cube`, `d3.line`, ...)
transform and draw inside one native call. A *scene* (`d3.begin` … `d3.end` /
`d3.present`) collects every primitive of a frame into one depth-sorted list
first, so several shapes occlude each other correctly, and `d3.present` can
render that list through a sprite much smaller than the viewport, band by
band — the whole 240×320 at 16-bit colour from a 240×40 sprite (19 KB).
Built-in shapes are generated on the fly; nothing keeps a vertex heap except
a custom mesh.

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
| `d3.frame([fps])` | 3D frame boundary; target is clamped to 12–40 FPS (default 20) |
| `d3.fps()` / `d3.delta()` | Smoothed FPS / frame delta in seconds |
| `d3.renderMs()` / `d3.faces()` | Last cube render time / visible face count |
| `d3.adaptive(enabled)` | Enable/disable automatic quality fallback |
| `d3.quality()` | `1` full quality, `0` temporary wireframe fallback |
| `d3.view(rx, ry, rz)` | Orbit rotation applied to everything, radians |
| `d3.light(x, y, z, [ambient])` | Direction *towards* the light and the ambient level `0..1` |

**Scene**

| Call | Effect / return |
|---|---|
| `d3.begin()` | Start collecting; `1` when the list exists (768 entries from the heap reserve, fewer on a tight heap) |
| `d3.end()` | Sort far-to-near and draw into the active sprite or the screen; returns the entry count |
| `d3.present()` | Sort and render through the active sprite in horizontal bands of its height, pushing each band at the viewport; the sprite must be as wide as the viewport |
| `d3.viewport(x, y, w, h)` | Screen rectangle `d3.present` fills (default full screen) |
| `d3.background(c565)` | Colour each band is cleared to |
| `d3.tris()` / `d3.dropped()` / `d3.capacity()` | Entries collected / dropped for lack of room / list size |

**Shapes** — placed with `d3.at`, coloured with `setcolor`; `mode` is `0` wire,
`1` solid, `2` solid + edges, and `+4` makes a shape two-sided (no culling).
The optional `edge565` overrides the edge colour.

| Call | Effect / return |
|---|---|
| `d3.at(x, y, z, [rx, ry, rz], [scale])` | Position, rotation and uniform scale for the shapes that follow |
| `d3.box(sx, sy, sz, [mode], [edge565])` | Box with those side lengths |
| `d3.sphere(r, [segments], [mode], [edge565])` | UV sphere, 4–24 segments (default 12) |
| `d3.cylinder(rBottom, [rTop], h, [sides], [mode], [edge565])` | Cylinder, or a frustum when the radii differ |
| `d3.cone(r, h, [sides], [mode], [edge565])` | Cone |
| `d3.torus(R, r, [segments], [rings], [mode], [edge565])` | Torus of ring radius `R` and tube radius `r` |
| `d3.plane(w, d, [mode], [edge565])` | Two-sided rectangle in the XZ plane |
| `d3.meshBegin()` / `d3.vertex(x, y, z)` / `d3.face(a, b, c, [d])` / `d3.meshEnd()` | Custom mesh of up to 256 vertices and 512 faces; `vertex` returns the index. List each face clockwise as seen from outside |
| `d3.mesh([mode], [edge565])` | Draw the custom mesh under the current `d3.at` |

```
gfx.auto(240, 40, 16)          # a band buffer, not a full screen
d3.viewport(0, 24, 240, 296)   # keep a strip for a HUD
loop
  d3.begin()
  d3.view(0.35, t, 0)
  setcolor(255, 149, 0)
  d3.at(-1.2, 0, 0, 0, t, 0)
  d3.sphere(0.9, 12, 2)
  setcolor(52, 199, 120)
  d3.at(1.2, 0, 0, t, t, 0)
  d3.torus(0.8, 0.3, 12, 8, 1)
  d3.present()
  t = t + d3.delta()
  d3.frame(30)
end
```

If two consecutive frames take longer than ~1.67× the requested frame time,
filled cubes temporarily switch to wireframe. Full quality returns after ten
frames close to target. The frame limiter sleeps in 1 ms steps up to the
deadline and keeps polling system gestures, so a 40 FPS target never starves
touch or Wi-Fi; a 160×160 8-bit sprite pushed over 40 MHz SPI costs about
12 ms per frame, which is what the 40 FPS ceiling is sized for. The
[`3D Cube`](https://github.com/openplace1/OpenStore/blob/main/apps/cube3d.osa)
sample is distributed through OpenStore and is the reference workload for
immediate mode; [`3D Shapes`](https://github.com/openplace1/OpenStore/blob/main/apps/shapes3d.osa)
is the reference for a full-screen scene.

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
| `confirm(title, body, [danger])` | `1` OK / `0` Cancel; `danger=1` styles OK as destructive |
| `ui.dialog(title, body, left, right, [danger])` | System popup with your own labels; `1` right / `0` left or swiped away. Empty `left` gives a single button |
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

Popups are drawn as a rounded card with a left-aligned heading, a muted body
and pill buttons. When the right button is destructive (`danger=1`) the safe
button is the filled one; otherwise the action is.

### Immediate-mode widgets

These only draw — into the sprite when one is open — and never block. Pair
them with `touch.tap(x, y, w, h)` or `touch.in()` for input, and redraw a
widget when its state changes rather than every frame. Colours follow the
theme; the accent is the system blue.

| Call | Effect |
|---|---|
| `ui.button(x, y, w, h, label, [style], [pressed])` | Pill button. `style` 0 filled accent (default), 1 soft grey, 2 soft grey with red text, 3 outlined, 4 filled red; `pressed=1` darkens it |
| `ui.switch(x, y, on, [w], [h])` | Toggle switch, 46×28 by default |
| `ui.checkbox(x, y, size, checked, [label], [bg565])` | Rounded checkbox with an optional label to the right |
| `ui.radio(cx, cy, r, selected, [label], [bg565])` | Radio button centred on `cx, cy` |
| `ui.progress(x, y, w, h, value, [max])` | Pill progress track; `value/max` clamped to `0..1` |
| `ui.card(x, y, w, h, [r])` | Raised surface for grouping content |
| `ui.chip(x, y, label, [selected])` | Pill tag; returns its width so a row can be laid out in one pass |
| `ui.icon(name, cx, cy, size, [bg565])` | One of the built-in icons in the draw colour; `1` if the name is known |
| `ui.iconButton(cx, cy, d, icon, [style], [pressed])` | Round button with an icon; styles as `ui.button`, default `1` |
| `ui.tabbar("icon:Label\|icon:Label\|…", selected)` | Floating pill tab bar along the bottom edge, up to 6 tabs; returns one tab's width |
| `ui.tabbarTap(count)` | Index of the tab just tapped, or `-1` |

Icons: `clock`, `share`, `folder`, `grid`/`apps`, `gear`/`settings`, `search`,
`home`, `star`, `download`, `list`, `info`, `heart`, `wifi`, `bt`, `sun`,
`moon`/`theme`, `sync`, `power`, `back`, `plus`, `check`, `close`, `more`,
`play`, `music`, `cube`, `note`. Any other name draws its first character in a
ring. The tab bar floats over the content, so leave the bottom 70 px of a
scrolling list free — Control Center and OpenStore are built from these.

`bg565` is the colour behind a checkbox or radio, needed to clear the mark
when it turns off; it defaults to `theme.surface()`.

```
var on = 0
ui.header("Wi-Fi")
ui.card(12, 60, 216, 56)
fontsize(2)
textcolor565(theme.text())
textml(28, 88, "Enabled")
ui.switch(170, 74, on)
ui.button(20, 260, 200, 44, "Forget network", 2)
loop
  if touch.tap(150, 60, 78, 56) == 1 then
    on = 1 - on
    ui.switch(170, 74, on)
  end
  if touch.tap(20, 260, 200, 44) == 1 then
    if ui.dialog("Forget this network?", "You will need the password again.", "Cancel", "Forget", 1) == 1 then exit() end
  end
  wait(30)
end
```

### Notifications

| Call | Effect |
|---|---|
| `notify(msg)` | Non-blocking 22-px banner along the top edge for 1.5 s; stays visible over script drawing and restores the panel when it expires. Needs the `notify` permission |

### Buffers, sockets and pixels

Building blocks rather than features: with these a script can stream its
screen, show a remote one, or speak any binary protocol. Sockets need the
`network` permission (the same one as `http.*`). A script owns at most 8
buffers of up to 64 KB (large ones come from the heap reserve), 4 TCP sockets
and one UDP socket; everything is closed and freed when the script ends.
[`Cast Receiver`](https://github.com/openplace1/OpenStore/blob/main/apps/castreceiver.osa)
with `tools/cast_sender.py` is the worked example — a computer screen on the
device over plain TCP.

| Call | Effect / return |
|---|---|
| `buf.alloc(bytes)` / `buf.free(id)` / `buf.len(id)` | Byte buffer; `alloc` returns an id or `-1` |
| `buf.get(id, i)` / `buf.set(id, i, v)` | One byte |
| `buf.u16(id, i)` / `buf.setU16(id, i, v)` / `buf.u32(id, i)` / `buf.setU32(id, i, v)` | Little-endian integers |
| `buf.fill(id, v, [from], [count])` / `buf.copy(dst, dstOff, src, srcOff, count)` | Bulk fill / copy |
| `buf.str(id, [off], [count])` / `buf.write(id, off, string)` | Bytes as a string and back |
| `screen.read(x, y, w, h, id, [off])` / `screen.write(x, y, w, h, id, [off])` | RGB565 pixels, two bytes each in memory order, rows top to bottom; returns the byte count |
| `gfx.read(id, [off])` / `gfx.write(id, [off])` | The active sprite's pixel buffer in its own depth |
| `net.connect(host, port, [timeoutMs])` | Outbound TCP; socket id or `-1` |
| `net.listen(port)` / `net.accept()` / `net.stopListening()` | Inbound TCP; `accept` returns a socket id or `-1` without waiting |
| `net.connected(s)` / `net.available(s)` / `net.remoteIP(s)` / `net.close(s)` | Socket state |
| `net.send(s, id, [off], [count])` / `net.recv(s, id, [off], [max])` | Bytes through a buffer; `recv` returns what was waiting (`0` none, `-1` closed) |
| `net.sendStr(s, string)` / `net.recvStr(s, [max])` | The same with strings |
| `udp.begin(port)` / `udp.end()` / `udp.send(host, port, id, [off], [count])` / `udp.recv(id, [off])` / `udp.remoteIP()` / `udp.remotePort()` | One UDP socket |

### App control

| Call | Effect |
|---|---|
| `exit()` | End the script; main router returns to home |
| `wait(ms)` | Sleep, while still processing the universal swipe-up gesture |
| `yield()` | Cooperatively feed the system and reset the execution slice |
| `millis()` | ms since boot |
| `micros()` | µs counter since boot |
| `elapsed(startMs)` | Wrap-safe milliseconds elapsed since `startMs` |
| `sdk.version()` | Numeric SDK compatibility level (currently `6`) |
| `sdk.has(feature)` | Capability check, including `d3`, `d3.scene`, `sprite`, `touch`, `perf`, `http`, `json`, `opk`, `ota`, `shapes`, `path`, `widgets`, `icons`, `tabbar`, `smooth`, `net`, `buffers`, `pixels`, `store.compatibility` and `store.updateAll` |
| `sys.info(key)` | Hardware and build facts: `chip`, `cores`, `cpu` (MHz), `flash`, `sketch`, `slot`, `ram`, `psram`, `idf`, `mac`, `board`, `partition`, `display`, `touch`, `sdtotal`, `sdused`, `sdtype`, `reset`, `reserve` |
| `openos.version()` | Display version (currently `1.6.1`) |
| `openos.versionCode()` | Numeric OpenOS compatibility level (currently `25`) |

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
| `sys.setTime(hour, minute, second, day, month, year)` | Set the system clock |
| `sys.reboot()` | `ESP.restart()` |
| `sys.notify(msg)` | Same banner as `notify()` without the permission prompt |
| `setbright(n)` | Requires `#perm system` |
| `getbright()` | Read current backlight |
| `freeram()` | `ESP.getFreeHeap()` |
| `heap.free()` / `heap.total()` | Current free / total heap |
| `heap.maxBlock()` | Largest contiguous allocation currently possible |
| `heap.lowWater()` | Lowest free heap observed since boot |
| `heap.fragmentation()` | Approximate fragmentation percentage |
| `uptime()` | Seconds since boot |
| `sdready()` | `1` if SD mounted |
| `battery()` | Charge percentage from an optional ADC divider, `-1` when no gauge is configured (the stock CYD has none) |
| `battery.available()` / `battery.mv()` | `1` when `battery_pin` is set; measured pack voltage in mV |

### Privileged — file system

Any absolute path on SD. Use carefully.

| Call | Effect |
|---|---|
| `fs.list(absPath)` | `\|`-separated entries; directories end with `/` |
| `fs.usage(absPath, [maxDepth])` | Bytes under a directory (or a file's size), walking up to `maxDepth` levels (default 4) |
| `fs.read(path[, offset, length])` | File contents or a bounded slice |
| `fs.size(path)` | File size in bytes |
| `fs.write(path, data)` | Overwrite |
| `fs.append(path, data)` | Append line |
| `fs.exists(path)` / `fs.delete(path)` | 0/1 |
| `fs.mkdir(path)` / `fs.rmdir(path)` | 0/1 |
| `fs.wipe(path)` | Recursively erase the contents below an absolute directory; use `/` for the whole SD |

`fs.read` is capped at 32 KB per call. `io.error()` reports limit, seek and
allocation failures.
`fs.wipe` does not show a confirmation prompt; the calling privileged script
must obtain confirmation before invoking it.

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
| `store.minSdk(i)` / `store.minOpenOS(i)` | Minimum package compatibility levels |
| `store.compatible(i)` | `1` when the current kernel can run the package |
| `store.requirement(i)` | Human-readable compatibility requirement |
| `store.updateCount()` | Number of compatible updates currently available |
| `store.updateAll()` | Confirm once and install every compatible update; returns count or `-1` |
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

### Privileged — firmware OTA

Read/check calls require a trusted privileged system entry. Mutating calls are
additionally restricted to Settings and always show a native confirmation.

| Call | Returns |
|---|---|
| `ota.supported()` | `1` only when the running and inactive partitions are the expected dual OTA slots |
| `ota.check()` | `1` newer release, `0` current/newer local version, `-1` error |
| `ota.available()` | `1` after a successful check found a newer signed release |
| `ota.checked()` | `1` once a check has run in this session; survives leaving the app |
| `ota.name()` / `ota.version()` / `ota.versionCode()` | Signed release identity |
| `ota.channel()` / `ota.type()` | `stable|beta|dev` and `major|minor|patch|security` |
| `ota.description()` / `ota.notes()` | Signed release notes |
| `ota.publishedAt()` / `ota.size()` | Signed publication label and byte size |
| `ota.source()` | Current HTTPS `info.json` URL |
| `ota.setSource(url)` | Change source; empty string restores the official feed |
| `ota.install()` | Confirm, stream, verify and activate the checked release; restarts on success |
| `ota.canRollback()` / `ota.rollback()` | Query/confirm restoration of the previous bootable slot |
| `ota.error()` | Last OTA error |

### Privileged — Wi-Fi

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
| `bt.enable()` / `bt.disable()` / `bt.enabled()` / `bt.error()` | Kept as no-ops returning `0` / an explanation: Bluetooth left OpenOS in 1.6 |
| `ntp.sync()` | Sync RTC via SNTP |

### Privileged — config

Persisted to `/user/config.ini`. Up to 48 keys system-wide.

| Call | Returns |
|---|---|
| `cfg.get(key, default)` | Value or default |
| `cfg.set(key, value)` | Save |
| `cfg.del(key)` | Remove |

### Privileged — crypto

AES-256-GCM with a key that never leaves the device: SHA-256 over a domain
tag, the factory Wi-Fi MAC and a 32-byte random secret generated on first
boot and kept in NVS (internal flash). Wi-Fi credentials (`wifi.save`) and
the lockscreen passcode use it, so a copy of the SD card is not enough to
read them. Values look like `v2:<base64 nonce‖ciphertext‖tag>`; older
XOR-obfuscated values are still readable and are re-sealed automatically on
the first boot of OpenOS 1.2.

| Call | Returns |
|---|---|
| `crypto.encrypt(plaintext)` | `v2:` + base64 sealed value (up to 1 KB of text) |
| `crypto.decrypt(value)` | Original string, or `""` when the value was tampered with or sealed by another device |

### Privileged — apps

Used by Settings to list installed scripts and toggle permissions.

| Call | Returns |
|---|---|
| `app.launch(path)` | Unload the current script and run another `.osa` / `.osac` |
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
| `home.move(from, to)` | Reorder by insertion (used when a tile is carried to another page) |
| `anim.openAt(x, y, color565)` | Where the tile being launched sits on screen; the router zooms from there and back into it when the app closes |
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
| Wi-Fi control | — | `wifi.*`, `ntp.sync` |
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

`platformio.ini` is preconfigured for the `denky32` board, the dual-slot
`partitions_ota.csv` table and the CYD display through TFT_eSPI build flags.
Both OTA app slots are 2,031,616 bytes. LTO is enabled because the complete
kernel must fit independently in either slot. The display remains on VSPI;
touch uses its own remapped SPI bus and the SD card uses HSPI.

The first upload from a legacy `huge_app.csv` installation must include the new
partition table over USB. After that migration, signed releases may be
installed from Settings without a cable.

If the upload port differs from the configured `COM3`, change `upload_port` or
override it with `pio run --target upload --upload-port <port>`.

The C++ sources live under `src/`. The runtime is in `src/Runtime/`; the
host kernel in `src/main.cpp` plus a small `Applications/` layer for the
data store (home grid, wallpaper cache, theme palette).

---

## Specs

| Metric | Value |
|---|---|
| C++ source | ~12 500 lines |
| OSA scripts | Loaded from SD / OPK packages |
| Flash | 2,000,529 B (98.5% of either 1.9375 MiB OTA slot); `firmware.bin` is 2,007,104 B |
| RAM (static) | 106,088 B (103.6 KiB; 32.4% of 320 KiB) |
| Heap headroom at boot | ~60 KB |
| Wallpaper cache | 150 KB (lazy) |
| Per-runtime overhead | ~31 KB (vars + lines + funcs + bytecode/pools) |

---

## Dependencies

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) — touch
- arduino-esp32 (WiFi, SD, HTTPClient)

---

## License

MIT. See `LICENSE`.
