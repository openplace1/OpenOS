#include "HeapReserve.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

namespace HeapReserve {
namespace {

static void* s_block = nullptr;
static size_t s_size = 0;
static bool s_lent = false;

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
    if (!s_block || s_lent) return;
    free(s_block);
    s_block = nullptr;
    s_size = 0;
    Serial.printf("[HEAP] reserve released for %s free=%u maxBlock=%u\n",
                  reason ? reason : "large allocation",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool reclaim() {
    if (s_block) return true;
    if (s_lent) return false;
    // Largest first, then step down: a smaller reserve still keeps small
    // allocations out of the one region a TLS session needs.
    for (size_t wanted = BYTES; wanted >= MIN_BYTES; wanted -= 1024U) {
        s_block = heap_caps_malloc(wanted, MALLOC_CAP_8BIT);
        if (!s_block) continue;
        s_size = wanted;
        Serial.printf("[HEAP] reserve reclaimed %u B free=%u maxBlock=%u\n",
                      (unsigned)wanted, (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return true;
    }
    return false;
}

size_t size() { return s_block ? s_size : 0; }

bool held() { return s_block != nullptr && !s_lent; }

void* borrow(size_t bytes, const char* reason) {
    if (s_lent) return nullptr;
    if (!s_block && !reclaim()) return nullptr;
    if (bytes > s_size) return nullptr;
    s_lent = true;
    Serial.printf("[HEAP] reserve lent %u B to %s free=%u maxBlock=%u\n",
                  (unsigned)bytes, reason ? reason : "caller",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return s_block;
}

void giveBack(void* pointer) {
    if (!s_lent || pointer != s_block) return;
    s_lent = false;
}

} // namespace HeapReserve
