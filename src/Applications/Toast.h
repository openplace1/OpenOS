#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Non-blocking notification banner behind notify() / sys.notify().
//
// show() paints a slim strip across the top of the panel and keeps it there
// for a moment; poll() — called from the main loop and from every place the
// runtime already polls touch — repaints it if the script drew over it and,
// once it expires, puts back the pixels that were underneath. The strip is
// only 22 px tall so on Home it covers the status bar (which never changes
// while a toast is up) and nothing of the icon grid.
namespace Toast {

static constexpr int HEIGHT = 22;
static constexpr uint32_t DURATION_MS = 1500;

void show(TFT_eSPI* tft, const String& text);
void poll(TFT_eSPI* tft);
bool active();

} // namespace Toast
