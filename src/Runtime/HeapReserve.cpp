#include "HeapReserve.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

namespace HeapReserve {
namespace {

static void* s_block = nullptr;
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
    Serial.printf("[HEAP] reserve released for %s free=%u maxBlock=%u\n",
                  reason ? reason : "large allocation",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool reclaim() {
    if (s_block) return true;
    if (s_lent) return false;
    s_block = heap_caps_malloc(BYTES, MALLOC_CAP_8BIT);
    if (s_block) log("reclaimed");
    return s_block != nullptr;
}

bool held() { return s_block != nullptr && !s_lent; }

void* borrow(size_t bytes, const char* reason) {
    if (s_lent || bytes > BYTES) return nullptr;
    if (!s_block && !reclaim()) return nullptr;
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
