#include "HeapReserve.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

namespace HeapReserve {
namespace {

static void* s_block = nullptr;

static void log(const char* what) {
    Serial.printf("[HEAP] reserve %s free=%u maxBlock=%u\n", what,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

} // namespace

void begin() {
    reclaim();
    log(s_block ? "taken at boot" : "not available at boot");
}

void release(const char* reason) {
    if (!s_block) return;
    free(s_block);
    s_block = nullptr;
    Serial.printf("[HEAP] reserve released for %s free=%u maxBlock=%u\n",
                  reason ? reason : "large allocation",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool reclaim() {
    if (s_block) return true;
    s_block = heap_caps_malloc(BYTES, MALLOC_CAP_8BIT);
    if (s_block) log("reclaimed");
    return s_block != nullptr;
}

bool held() { return s_block != nullptr; }

} // namespace HeapReserve
