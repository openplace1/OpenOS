#pragma once

#include <stddef.h>

// One contiguous block, reserved at boot while the heap is still unfragmented,
// that mbedTLS allocates from and that the few other large consumers can
// borrow or have released to them.
//
// On this no-PSRAM board the free heap after boot is ~78 KB in one 59 KB
// block plus change. Every script that loads sprinkles small, long-lived
// Strings across that block, and after that nothing larger than 20-30 KB can
// be allocated even with 70 KB free. mbedTLS needs two 16.4 KB record
// buffers for one HTTPS session, a 3D game needs a 25-35 KB sprite, and the
// OPK extractor needs a 32 KB inflate window; all of them used to fail (or
// silently downgrade) as soon as an application was running.
//
// TLS: mbedTLS routes every allocation through a function pointer pair
// (mbedtls_platform_set_calloc_free). begin() points them at a small
// first-fit allocator inside this block, so a whole session — the two
// record buffers, the handshake, the peer chain and the hundreds of bignum
// limbs — lives here and never depends on the general heap. Anything that
// does not fit falls through to the ordinary heap. Two field failures drove
// this:
// - After one transfer the Wi-Fi stack had left ~8 KB of buffers inside the
//   freed region, the reserve could only be re-taken at 37 KB, and the next
//   handshake failed inside RSA with "memory allocation failed" even with
//   60 KB free.
// - Serving only the large structures from here left mbedTLS's ~9 KB of
//   small pieces to chew the general heap into fragments, until the Wi-Fi
//   driver could no longer get a 1.6 KB receive buffer and the transfer
//   stalled for ever at "Loading catalog".
//
// The same allocator serves OpenOS's own large, short-lived needs through
// allocate()/deallocate(): a script's source text while it compiles, and
// the Control Center's second runtime (its inline arrays are 20-30 KB, which
// no application-time heap offers in one piece). Sprites and the OPK inflate
// window cannot be pointed at the arena, so for them release() still hands
// the whole block back to the allocator — but only while nothing is live
// inside it — and reclaim() takes it again afterwards, growing back to BYTES
// whenever the heap allows.
namespace HeapReserve {

// A pinned session peaks at about 50 KB in here (measured: 41 KB of
// structures over 1 KB plus the small pieces).
static constexpr size_t BYTES = 52U * 1024U;
// Once an application has run, the heap rarely offers BYTES in one piece
// again. reclaim() settles for the largest block it can get down to this
// floor, which still covers both TLS record buffers (2 x ~16.7 KB).
static constexpr size_t MIN_BYTES = 35U * 1024U;
// makeRoom() keeps at least this much: enough for a 3D scene list or a
// script's source text while a sprite holds the rest.
static constexpr size_t MIN_KEEP = 20U * 1024U;

void begin();

// Hand the whole block back to the allocator. Use this for a consumer that
// needs the *allocator* to hand out the space (TFT_eSprite, the OPK inflate
// window); reclaim() takes it again afterwards. Refused, with a log line,
// while mbedTLS still holds memory inside the block.
void release(const char* reason);
// The general heap cannot serve `bytes` in one block: give up just enough
// of the reserve's tail for it (the block is shrunk in place, so the freed
// tail is one contiguous piece), or the whole block when that would leave
// less than MIN_KEEP. Refused while anything is live inside. reclaim() grows
// the block back once the consumer is gone.
bool makeRoom(size_t bytes, const char* reason);
bool reclaim();
bool held();
// Size of the block currently held, 0 when none.
size_t size();

// Arena bookkeeping for diagnostics: bytes mbedTLS currently holds inside
// the block, the high-water mark since the last resetPeak(), the number of
// live allocations and how many large requests had to fall through to the
// general heap because the block was full.
size_t arenaUsed();
size_t arenaPeak();
int    arenaLive();
int    arenaOverflows();
void   resetPeak();

// Allocate inside the reserved block, without freeing it. A consumer that
// would otherwise allocate and then leave long-lived debris inside the freed
// region must use this instead: loading a script used to release the block,
// and the compiler's string pools then settled inside it, so the region
// could never be reclaimed whole again. Returns nullptr when the block is
// missing or nothing large enough is free inside it. Memory obtained here
// blocks release() until it is given back.
void* allocate(size_t bytes, const char* reason);
void deallocate(void* pointer);

} // namespace HeapReserve
