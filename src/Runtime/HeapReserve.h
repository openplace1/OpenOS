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

void begin();
void release(const char* reason);
bool reclaim();
bool held();

} // namespace HeapReserve
