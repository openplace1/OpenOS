# OpenOS — SD card contents

Drag the **contents** of this folder onto the root of your SD card so the
filesystem ends up like this:

```
SD root/
├─ system/
│  └─ apps/
│     ├─ settings.osa
│     ├─ clock.osa
│     ├─ calculator.osa
│     ├─ files.osa
│     ├─ notes.osa
│     ├─ bounce.osa
│     ├─ controlcenter.osa
│     └─ lockscreen.osa
└─ tap_game.osa
```

## What lives where

| Path                          | What it does                                |
|-------------------------------|---------------------------------------------|
| `system/apps/*.osa`           | Built-in apps. Privileged — they get the full SDK (`sys.*`, `cfg.*`, `fs.*`, `crypto.*`, `wifi.*`, `bt.*`, NTP, app launch, home mutations). |
| `system/apps/lockscreen.osa`  | Boot script; runs first at power-on. `exit()` unlocks to home. |
| `system/apps/controlcenter.osa` | Swipe-down-from-top overlay. |
| `tap_game.osa`                | Demo mini-game in the root for testing. Not a system app. |

Anything you drop into `system/apps/` becomes privileged (because the runtime
checks path prefix). Anything dropped anywhere else (root, subfolders one
level deep) is a normal user app — sandboxed reads/writes only inside
`/apps/<scriptname>/`.

## How the firmware finds your scripts

At boot the firmware scans:

1. `/` (root) — one level deep — for any `*.osa` with `#isApp true`
2. `/system/apps/` — privileged location, same scan

Each match becomes a home-screen tile (icon color from `#appColor "#RRGGBB"`,
name from `#app "..."`). The firmware **no longer ships these scripts in
flash** — that's why the firmware image is small. If you delete a system
script from the SD card the corresponding tile disappears.

## Editing

Just edit the `.osa` files in any text editor and copy them back. No
recompile needed — the firmware reads from SD on every launch.
