#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <IPAddress.h>
#include <WiFiClientSecure.h>
#include <esp_wifi_types.h>

// Shared HTTPS transport for OpenStore and firmware OTA on the no-PSRAM ESP32.
//
// arduino-esp32 2.0.x ships mbedTLS with two fixed 16 KB record buffers, so a
// single handshake needs roughly 45-50 KB of heap and two contiguous blocks of
// at least ~16.4 KB. When either is missing, WiFiClientSecure::connect() fails
// and HTTPClient reports only "connection refused" — the same text it uses for
// a DNS miss or a TCP timeout. This helper frees what it can first, resolves
// the host up front, retries transient failures and turns the final failure
// into a message that says what actually went wrong.
//
// Server identity: hosts listed in OpenOSTrustAnchors.h (the GitHub raw CDN
// that serves the official OTA and OpenStore feeds) are verified against the
// pinned root CA, including the host name. Any other host is used without
// certificate verification, which is why every document fetched from a custom
// feed must still carry a release signature.
namespace SecureHttp {

// Heap floor for one TLS session: 2 x 16 KB record buffers, handshake state,
// the parsed peer chain (GitHub sends three certificates) and lwIP receive
// buffers. Below this the handshake fails inside mbedTLS with an allocation
// error long before any byte reaches the application.
static constexpr size_t TLS_MIN_FREE_BYTES  = 46U * 1024U;
// mbedTLS allocates MBEDTLS_SSL_IN_BUFFER_LEN (16384 + record overhead) twice.
static constexpr size_t TLS_MIN_BLOCK_BYTES = 17U * 1024U;
// Verification runs while both record buffers are held, so for a pinned host
// the *largest single region* has to cover both of them plus room to work.
// Two separate 17 KB blocks pass a naive probe and then leave the
// verification with nothing, which mbedTLS reports as "certificate is not
// correctly signed by the trusted CA". Since the leaf's own issuer is pinned
// (see OpenOSTrustAnchors.h) that work is a single RSA-2048 signature check
// rather than a walk through two RSA-4096 certificates, so the margin here is
// small — but it must not be zero.
static constexpr size_t TLS_PINNED_EXTRA_BYTES = 3U * 1024U;
static constexpr size_t TLS_PINNED_REGION_BYTES =
    2U * TLS_MIN_BLOCK_BYTES + TLS_PINNED_EXTRA_BYTES;

// Keeps the Wi-Fi modem out of power save while the object lives. Modem
// sleep adds up to ~100 ms per round trip and, with some access points,
// loses the retransmits a TLS handshake depends on. Transfers are
// latency-bound, so callers hold one of these for the whole download.
class RadioAwake {
public:
    RadioAwake();
    ~RadioAwake();
    RadioAwake(const RadioAwake&) = delete;
    RadioAwake& operator=(const RadioAwake&) = delete;
private:
    wifi_ps_type_t previous;
};

struct Request {
    uint32_t connectTimeoutMs  = 10000;
    uint32_t readTimeoutMs     = 15000;
    uint32_t handshakeTimeoutS = 20;
    int      attempts          = 3;
};

// Suspends Classic Bluetooth (main.cpp restores it after returning Home) and
// logs the heap that the following TLS session will have to work with.
void prepareMemory(const char* tag, const char* operation);

// True when a TLS handshake has a realistic chance. Otherwise `why` receives
// a message with the current free/largest-block figures. `pinned` adds the
// headroom certificate verification needs.
bool memoryAvailable(String& why, bool pinned = false);

// PEM of the root CA that `host` is pinned to, or nullptr when the host is
// not in OpenOSTrustAnchors.h.
const char* trustAnchorForHost(const String& host);

// Connects to `host`:443 without verification, prints the certificate chain
// the server presents and the result of verifying it against the pinned roots
// (mbedTLS verify flags) — once with a free heap and once while two blocks
// the size of the TLS record buffers are held, which is the situation the
// real handshake verifies in. Available over the USB serial command
// OPENOS:TLSDIAG; never run automatically, because it opens a second TLS
// session.
void diagnoseCertificateChain(const String& host);

// Host part of an https:// URL without userinfo or port. Empty when invalid.
String hostFromUrl(const String& url);

// Resolves `host` with a few attempts. lwIP caches the answer, so the
// connect() that follows does not repeat the lookup.
bool resolveHost(const String& host, IPAddress& address, String& why,
                 int attempts = 3);

// Opens `http` on `client` and performs GET, retrying transport failures.
// Returns the HTTP status (> 0) with the connection open for reading, or a
// negative HTTPClient error after the last attempt. `why` describes the last
// transport failure in user-facing terms.
int get(HTTPClient& http, WiFiClientSecure& client, const String& url,
        const Request& request, String& why);

} // namespace SecureHttp
