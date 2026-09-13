#pragma once

#include <stddef.h>

// One contiguous block, reserved at boot while the heap is still unfragmented
// and lent out to the few consumers that need a large allocation.
//
// On this no-PSRAM board the free heap after boot is ~78 KB in one 59 KB
// block plus change. Every script that loads sprinkles small, long-lived
// Strings across that block, and after that nothing larger than 20-30 KB can
// be allocated even with 70 KB free. mbedTLS needs two 16.4 KB record
// buffers for one HTTPS session, a 3D game needs a 25-35 KB sprite, and the
// OPK extractor needs a 32 KB inflate window; all of them used to fail (or
// silently downgrade) as soon as an application was running.
//
// Holding the block keeps small allocations out of it. release() hands the
// space back to the heap immediately before a large allocation and reclaim()
// takes it again once the consumer is done (idempotent, best effort: if
// something else grabbed the space meanwhile the next reclaim() retries).
//
// Size: a pinned HTTPS session is the worst case. mbedTLS takes two 16.4 KB
// record buffers out of this block and then verifies an RSA-4096 signature
// *while holding them*, so the block has to be large enough to leave a usable
// remainder; with only ~1 KB left the verification fails and mbedTLS reports
// it as "certificate not signed by the trusted CA".
namespace HeapReserve {

static constexpr size_t BYTES = 45U * 1024U;
// Once an application has run, the heap rarely offers BYTES in one piece
// again — a single long-lived String left in the middle of the released
// region is enough. Reclaiming all-or-nothing therefore lost the reserve for
// the rest of the session, and the next transfer found whatever fragments
// were left. reclaim() now settles for the largest block it can get down to
// this floor, which still covers both TLS record buffers.
static constexpr size_t MIN_BYTES = 35U * 1024U;

void begin();

// Hand the whole block back to the allocator. Use this for a consumer that
// needs the *allocator* to hand out the space (mbedTLS, TFT_eSprite, the OPK
// inflate window); reclaim() takes it again afterwards.
void release(const char* reason);
bool reclaim();
bool held();
// Size of the block currently held, 0 when none.
size_t size();

// Use the reserved block directly, without freeing it. A consumer that would
// otherwise allocate and then leave long-lived debris inside the freed region
// must borrow instead: loading a script used to release the block, and the
// compiler's string pools then settled inside it, so the region could never
// be reclaimed whole again and the next HTTPS handshake found only ~38 KB
// contiguous — enough for both TLS record buffers but not for the
// certificate verification that runs while they are held.
// Returns nullptr when the block is missing, already lent or too small.
void* borrow(size_t bytes, const char* reason);
void giveBack(void* pointer);

} // namespace HeapReserve
