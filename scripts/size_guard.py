# Fails the build when firmware.bin no longer fits an OTA slot (0x1F0000 in
# partitions_ota.csv). esptool happily writes an oversized image and the
# bootloader then rejects it, leaving the board in a SW_RESET loop that
# needs a BOOT-button flash to recover.
Import("env")

import os

OTA_SLOT_BYTES = 0x1F0000

def check_size(source, target, env):
    path = str(target[0])
    size = os.path.getsize(path)
    if size > OTA_SLOT_BYTES:
        print("ERROR: %s is %d bytes, %d over the %d-byte OTA slot"
              % (path, size, size - OTA_SLOT_BYTES, OTA_SLOT_BYTES))
        env.Exit(1)
    print("firmware.bin: %d bytes, %d bytes of OTA slot headroom"
          % (size, OTA_SLOT_BYTES - size))

env.AddPostAction("$BUILD_DIR/firmware.bin", check_size)
