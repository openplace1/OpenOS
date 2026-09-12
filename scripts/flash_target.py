# Adds `pio run -t flash`: build, then flash through scripts/flash.py, which
# retries the flaky CH340 auto-reset and asks for the BOOT button when needed.
Import("env")

import os

def flash(source, target, env):
    port = env.GetProjectOption("upload_port", "COM3")
    script = os.path.join(env["PROJECT_DIR"], "scripts", "flash.py")
    firmware = os.path.join(env.subst("$BUILD_DIR"), "firmware.bin")
    return env.Execute('"$PYTHONEXE" "%s" "%s" "%s"' % (script, port, firmware))

env.AddCustomTarget(
    name="flash",
    dependencies="$BUILD_DIR/firmware.bin",
    actions=[flash],
    title="Flash with retries",
    description="Flash firmware.bin, retrying the CH340 auto-reset (hold BOOT if asked)",
)
