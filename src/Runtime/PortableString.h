#pragma once

// Parsers that must stay testable on a desktop include this instead of
// <Arduino.h>. The firmware build gets the real Arduino String; the host
// test harness (test/host) provides a small std::string-backed stand-in.
#ifdef OPENOS_HOST_TEST
#include "ArduinoStringShim.h"
#else
#include <Arduino.h>
#endif
