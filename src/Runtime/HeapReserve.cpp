#include "HeapReserve.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <mbedtls/platform.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace HeapReserve {
namespace {

static void* s_block = nullptr;
static size_t s_size = 0;

// mbedTLS also runs in the Wi-Fi task (the supplicant derives keys with it),
// so a calloc can arrive from there while the main task is mid-handshake.
// Every touch of the arena and of s_block happens under this lock.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static size_t s_used = 0;
static size_t s_peak = 0;
static int    s_live = 0;
static int    s_overflows = 0;

// ─── Arena allocator ─────────────────────────────────────────────────────────
// A chain of blocks with an 8-byte header, first fit, split on allocate and
// coalesce both ways on free. Payload sizes are multiples of 8 so the low
// bit of `size` can carry the in-use flag; prevSize is 0 only for the first
// block (a payload is never smaller than 8).

struct Header {
    uint32_t size;      // payload bytes | used flag in bit 0
    uint32_t prevSize;  // payload bytes of the block before this one
};

static constexpr uint32_t USED = 1U;

static inline uint32_t payloadOf(const Header* h) { return h->size & ~7U; }
static inline bool     usedOf(const Header* h)    { return (h->size & USED) != 0; }
static inline void     set(Header* h, uint32_t payload, bool used) {
    h->size = payload | (used ? USED : 0U);
}
static inline Header*  nextOf(Header* h) {
    return (Header*)((uint8_t*)h + sizeof(Header) + payloadOf(h));
}
static inline Header*  arenaEnd() { return (Header*)((uint8_t*)s_block + s_size); }

static void arenaFormat() {
    Header* first = (Header*)s_block;
    set(first, (uint32_t)(s_size - sizeof(Header)), false);
    first->prevSize = 0;
    s_used = 0;
    s_live = 0;
}

static void* arenaAlloc(size_t bytes) {
    uint32_t wanted = (uint32_t)((bytes + 7U) & ~7U);
    if (wanted == 0) wanted = 8;
    Header* h = (Header*)s_block;
    Header* end = arenaEnd();
    while (h < end) {
        uint32_t payload = payloadOf(h);
        if (!usedOf(h) && payload >= wanted) {
            uint32_t remainder = payload - wanted;
            if (remainder >= sizeof(Header) + 8U) {
                Header* split = (Header*)((uint8_t*)h + sizeof(Header) + wanted);
                set(split, remainder - (uint32_t)sizeof(Header), false);
                split->prevSize = wanted;
                Header* after = nextOf(split);
                if (after < end) after->prevSize = payloadOf(split);
                set(h, wanted, true);
            } else {
                set(h, payload, true);
            }
            s_used += payloadOf(h) + sizeof(Header);
            if (s_used > s_peak) s_peak = s_used;
            ++s_live;
            return (uint8_t*)h + sizeof(Header);
        }
        h = nextOf(h);
    }
    return nullptr;
}

static void arenaRelease(void* pointer) {
    Header* h = (Header*)((uint8_t*)pointer - sizeof(Header));
    Header* end = arenaEnd();
    s_used -= payloadOf(h) + sizeof(Header);
    --s_live;
    set(h, payloadOf(h), false);
    // Merge with the block after, then let the block before absorb both.
    Header* next = nextOf(h);
    if (next < end && !usedOf(next)) {
        set(h, payloadOf(h) + (uint32_t)sizeof(Header) + payloadOf(next), false);
        Header* after = nextOf(h);
        if (after < end) after->prevSize = payloadOf(h);
    }
    if (h->prevSize != 0) {
        Header* prev = (Header*)((uint8_t*)h - sizeof(Header) - h->prevSize);
        if (!usedOf(prev)) {
            set(prev, payloadOf(prev) + (uint32_t)sizeof(Header) + payloadOf(h), false);
            Header* after = nextOf(prev);
            if (after < end) after->prevSize = payloadOf(prev);
        }
    }
}

static bool inArena(const void* pointer) {
    return s_block && pointer >= s_block &&
           pointer < (const void*)((const uint8_t*)s_block + s_size);
}

// ─── mbedTLS hooks ───────────────────────────────────────────────────────────

static void* tlsCalloc(size_t count, size_t size) {
    size_t bytes = count * size;
    if (count != 0 && bytes / count != size) return nullptr;
    void* pointer = nullptr;
    portENTER_CRITICAL(&s_mux);
    if (s_block) {
        pointer = arenaAlloc(bytes);
        if (!pointer) ++s_overflows;
    }
    portEXIT_CRITICAL(&s_mux);
    if (pointer) {
        memset(pointer, 0, bytes);
        return pointer;
    }
    return calloc(count, size);
}

static void tlsFree(void* pointer) {
    if (!pointer) return;
    bool inside;
    portENTER_CRITICAL(&s_mux);
    inside = inArena(pointer);
    if (inside) arenaRelease(pointer);
    portEXIT_CRITICAL(&s_mux);
    if (!inside) free(pointer);
}

static void log(const char* what) {
    Serial.printf("[HEAP] reserve %s free=%u maxBlock=%u\n", what,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// Takes the largest block available between BYTES and MIN_BYTES and formats
// it as an empty arena. Caller holds no lock; s_block must be null.
static bool take() {
    for (size_t wanted = BYTES; wanted >= MIN_BYTES; wanted -= 1024U) {
        void* block = heap_caps_malloc(wanted, MALLOC_CAP_8BIT);
        if (!block) continue;
        portENTER_CRITICAL(&s_mux);
        s_block = block;
        s_size = wanted;
        arenaFormat();
        portEXIT_CRITICAL(&s_mux);
        Serial.printf("[HEAP] reserve reclaimed %u B free=%u maxBlock=%u\n",
                      (unsigned)wanted, (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return true;
    }
    return false;
}

} // namespace

void begin() {
    take();
    // From here on every mbedTLS allocation goes through the arena hooks.
    // Memory mbedTLS obtained earlier came from calloc(), which is exactly
    // what tlsFree() hands non-arena pointers to.
    mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
    log(s_block ? "taken at boot" : "not available at boot");
}

void release(const char* reason) {
    void* block = nullptr;
    int live = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_block) {
        live = s_live;
        if (live == 0) {
            block = s_block;
            s_block = nullptr;
            s_size = 0;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (live > 0) {
        // Freeing the region under mbedTLS would corrupt the heap; the
        // caller has to make do with what the general heap offers.
        Serial.printf("[HEAP] reserve kept for %s: %d allocations live inside it\n",
                      reason ? reason : "large allocation", live);
        return;
    }
    if (!block) return;
    free(block);
    Serial.printf("[HEAP] reserve released for %s free=%u maxBlock=%u\n",
                  reason ? reason : "large allocation",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool makeRoom(size_t bytes, const char* reason) {
    size_t need = bytes + 512U;   // allocator headers and rounding
    size_t current = 0;
    int live = 0;
    portENTER_CRITICAL(&s_mux);
    current = s_block ? s_size : 0;
    live = s_live;
    portEXIT_CRITICAL(&s_mux);
    if (!current) return false;
    if (live > 0) {
        Serial.printf("[HEAP] reserve kept for %s: %d allocations live inside it\n",
                      reason ? reason : "large allocation", live);
        return false;
    }
    if (current < need + MIN_KEEP) {
        release(reason);
        return true;
    }
    size_t keep = (current - need) & ~(size_t)7U;
    portENTER_CRITICAL(&s_mux);
    void* shrunk = heap_caps_realloc(s_block, keep, MALLOC_CAP_8BIT);
    if (shrunk) {
        s_block = shrunk;
        s_size = keep;
        arenaFormat();
    }
    portEXIT_CRITICAL(&s_mux);
    if (!shrunk) {
        release(reason);
        return true;
    }
    Serial.printf("[HEAP] reserve shrunk to %u B for %s free=%u maxBlock=%u\n",
                  (unsigned)keep, reason ? reason : "large allocation",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

bool reclaim() {
    if (s_block) {
        if (s_size >= BYTES) return true;
        // A smaller block was all the heap offered last time. Whatever sat
        // next to it may be gone by now, so try once more for the full size;
        // failing that, keep what we have.
        bool idle;
        void* old = nullptr;
        size_t oldSize = 0;
        portENTER_CRITICAL(&s_mux);
        idle = s_live == 0;
        if (idle) {
            old = s_block;
            oldSize = s_size;
            s_block = nullptr;
            s_size = 0;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!idle) return true;
        free(old);
        size_t got = BYTES;
        void* bigger = heap_caps_malloc(BYTES, MALLOC_CAP_8BIT);
        if (!bigger) {
            got = oldSize;
            bigger = heap_caps_malloc(oldSize, MALLOC_CAP_8BIT);
        }
        if (bigger) {
            portENTER_CRITICAL(&s_mux);
            s_block = bigger;
            s_size = got;
            arenaFormat();
            portEXIT_CRITICAL(&s_mux);
            if (got > oldSize)
                Serial.printf("[HEAP] reserve grew back to %u B free=%u maxBlock=%u\n",
                              (unsigned)got, (unsigned)ESP.getFreeHeap(),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            return true;
        }
        return take();
    }
    return take();
}

size_t size() { return s_block ? s_size : 0; }

bool held() { return s_block != nullptr; }

size_t arenaUsed()     { return s_used; }
size_t arenaPeak()     { return s_peak; }
int    arenaLive()     { return s_live; }
int    arenaOverflows(){ return s_overflows; }
void   resetPeak() {
    portENTER_CRITICAL(&s_mux);
    s_peak = s_used;
    s_overflows = 0;
    portEXIT_CRITICAL(&s_mux);
}

void* allocate(size_t bytes, const char* reason) {
    if (!s_block && !reclaim()) return nullptr;
    void* pointer = nullptr;
    portENTER_CRITICAL(&s_mux);
    if (s_block) pointer = arenaAlloc(bytes);
    portEXIT_CRITICAL(&s_mux);
    if (!pointer) return nullptr;
    Serial.printf("[HEAP] reserve lent %u B to %s (%u B in use) free=%u maxBlock=%u\n",
                  (unsigned)bytes, reason ? reason : "caller", (unsigned)s_used,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return pointer;
}

void deallocate(void* pointer) {
    if (!pointer) return;
    portENTER_CRITICAL(&s_mux);
    if (inArena(pointer)) arenaRelease(pointer);
    portEXIT_CRITICAL(&s_mux);
}

} // namespace HeapReserve
