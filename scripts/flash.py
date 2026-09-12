#!/usr/bin/env python3
"""Flash OpenOS over USB with a few auto-reset attempts.

The CYD's CH340 auto-reset is unreliable on Windows: esptool often reports
"Wrong boot mode detected (0x13)" because the DTR/RTS transitions arrive too
far apart for the EN capacitor. Nothing in the firmware can fix that (the
classic ESP32 has no software path into the ROM download mode; deep-sleep
wake and software resets keep the previously latched strapping value), so
this simply retries esptool and, when the board still will not cooperate,
asks for the BOOT button.

    python scripts/flash.py [COM3] [path/to/firmware.bin]

Also available as `pio run -t flash`.
"""

from __future__ import annotations

import glob
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FIRMWARE = ROOT / ".pio" / "build" / "denky32" / "firmware.bin"
APP_OFFSET = "0x10000"
OTA_SLOT_BYTES = 0x1F0000
ATTEMPTS = 6


def platformio_python() -> str:
    home = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    for candidate in (home / "penv" / "Scripts" / "python.exe", home / "penv" / "bin" / "python"):
        if candidate.exists():
            return str(candidate)
    return sys.executable


def esptool_path() -> str:
    home = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    matches = glob.glob(str(home / "packages" / "tool-esptoolpy*" / "esptool.py"))
    if not matches:
        raise SystemExit("esptool.py not found; run `pio run` once so PlatformIO installs it")
    return matches[0]


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("OPENOS_PORT", "COM3")
    firmware = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_FIRMWARE
    if not firmware.exists():
        raise SystemExit(f"firmware not found: {firmware} (run `pio run` first)")
    size = firmware.stat().st_size
    if size > OTA_SLOT_BYTES:
        # The bootloader rejects an image that does not fit its partition and
        # the board ends up in a reset loop that only a BOOT-button flash fixes.
        raise SystemExit(f"{firmware} is {size} bytes, over the {OTA_SLOT_BYTES}-byte OTA slot")
    command = [
        platformio_python(), esptool_path(), "--chip", "esp32", "--port", port,
        "--baud", "460800", "--before", "default_reset", "--after", "hard_reset",
        "--connect-attempts", "3",
        "write_flash", "-z", APP_OFFSET, str(firmware),
    ]
    for attempt in range(1, ATTEMPTS + 1):
        print(f"attempt {attempt}/{ATTEMPTS}")
        if subprocess.call(command) == 0:
            return
        if attempt == 2:
            print("The board is not entering download mode by itself: hold BOOT now; "
                  "release it once 'Writing at' appears.")
        time.sleep(2)
    raise SystemExit("flashing failed; hold BOOT (next to RST) while running this again")


if __name__ == "__main__":
    main()
