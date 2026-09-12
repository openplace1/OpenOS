# Host-side tests

Desktop regression tests for the parsers that decide what the device trusts:

- `Runtime/OtaManifest` — signed `info.json` parsing, field validation,
  canonical signature payload (checked byte-for-byte against the Python
  release tool's output for the published manifest), firmware URL resolution;
- `Runtime/CatalogSignature` — framing of the signed OpenStore catalog;
- `Runtime/UrlUtil` and `Runtime/OpenOSTrustAnchors` — host extraction and
  root-CA pinning lookup.

These modules include `PortableString.h`, which resolves to Arduino's
`String` in the firmware and to `shim/ArduinoStringShim.h` here, so the code
under test is the exact code that runs on the ESP32. Nothing else from the
firmware is stubbed: networking, flash and mbedTLS stay out of the harness.

## Running

Any C++11 compiler works (GCC/MinGW, Clang, MSVC). From this directory:

```bash
./run.sh          # Linux/macOS/Git Bash with g++ or clang++
run.bat           # Windows with g++ (MinGW) on PATH
```

or by hand:

```bash
g++ -std=c++11 -Wall -Wextra -DOPENOS_HOST_TEST -Ishim -I../../src/Runtime \
    test_main.cpp ../../src/Runtime/OtaManifest.cpp -o host_tests && ./host_tests
```

The process exit code is the number of failed checks; every failure prints
its file and line. `manifest_fixture.inc` is generated from the published
`OpenStore/update/info.json` by `python test/host/gen_fixture.py`; rerun it
when that manifest changes.

`pio test` is not used for these (the `denky32` environment ignores this
directory via `test_ignore`), so they never need the ESP32 toolchain.
