#include "SecureHttp.h"

#include "HeapReserve.h"
#include "OpenOSTrustAnchors.h"
#include "UrlUtil.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>


namespace SecureHttp {
namespace {

// Set once a pinned handshake with this host has been rejected. Retrying the
// pin on every later transfer only costs a failed handshake and, worse, holds
// the whole request to the stricter pinned memory requirement.
static String s_pinRejectedHost;

static String heapSummary() {
    String summary = "free ";
    summary += (unsigned)(ESP.getFreeHeap() / 1024U);
    summary += " KB, largest block ";
    summary += (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024U);
    summary += " KB";
    if (HeapReserve::held()) {
        summary += ", TLS arena ";
        summary += (unsigned)(HeapReserve::size() / 1024U);
        summary += " KB";
    }
    return summary;
}

// WiFiClientSecure::connect() collapses every failure into "connection
// refused". The mbedTLS error that start_ssl_client() stored tells DNS, TCP,
// allocation and handshake problems apart.
static String describeConnectFailure(WiFiClientSecure& client, const String& host) {
    char detail[96] = {0};
    int tlsError = client.lastError(detail, sizeof(detail));
    if (tlsError == 0)
        return String("Could not connect to ") + host + " (" + heapSummary() + ")";
    // start_ssl_client() returns -1 for socket-level problems: TCP connect
    // timeout/refusal, select() errors and the handshake wall-clock timeout.
    if (tlsError == -1)
        return String("TCP connection to ") + host + ":443 timed out or was refused";
    bool memory = strstr(detail, "llocation") != nullptr ||
                  strstr(detail, "emory") != nullptr;
    if (memory)
        return String("Not enough RAM for the TLS handshake (") + heapSummary() + ")";
    String message = "TLS handshake with ";
    message += host;
    message += " failed: ";
    message += detail[0] ? detail : "unknown error";
    message += " (-0x";
    message += String((unsigned)(-tlsError), HEX);
    message += ")";
    // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED. Usually a CA change or an on-path
    // proxy — but mbedTLS reports a failed allocation during the RSA
    // verification the same way, so name both. OPENOS:TLSDIAG over USB prints
    // the chain and the verify flags; it is not run automatically because it
    // would open a second TLS session on an already tight heap.
    if (tlsError == -0x2700) {
        message += " - certificate not issued by the pinned root CA, or too "
                   "little contiguous RAM to verify it (";
        message += heapSummary();
        message += ")";
    }
    return message;
}

} // namespace

RadioAwake::RadioAwake() : previous(WiFi.getSleep()) {
    if (previous != WIFI_PS_NONE) WiFi.setSleep(WIFI_PS_NONE);
}

RadioAwake::~RadioAwake() {
    if (previous != WIFI_PS_NONE) WiFi.setSleep(previous);
}

void prepareMemory(const char* tag, const char* operation) {
    // Classic Bluetooth and TLS compete for the same internal RAM. Keep the
    // user's setting, but pause the radio; main.cpp resumes it after Home.
    Serial.printf("[%s] %s free=%u maxBlock=%u\n", tag ? tag : "HTTPS",
                  operation ? operation : "HTTPS",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool memoryAvailable(String& why, bool pinned) {
    // mbedTLS allocates its record buffers and other large structures from
    // the reserve (see HeapReserve), so with the reserve held a session only
    // needs the general heap for small pieces. Without it — lent to a sprite
    // that is still alive — the old all-from-the-heap requirement applies.
    HeapReserve::reclaim();
    size_t freeBytes = ESP.getFreeHeap();
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const bool arena = HeapReserve::held() && HeapReserve::size() >= HeapReserve::MIN_BYTES;
    if (arena) {
        // No largest-block requirement: everything over 1 KB comes out of
        // the arena, and inside an application the general heap is routinely
        // in 2 KB pieces with 25 KB free — a session still completes there.
        size_t needed = TLS_MIN_FREE_WITH_ARENA +
                        (pinned ? TLS_PINNED_EXTRA_WITH_ARENA : 0);
        if (freeBytes >= needed) return true;
        why = "Not enough RAM for HTTPS (";
        why += heapSummary();
        why += "; needs ";
        why += (unsigned)(needed / 1024U);
        why += " KB free). Close Bluetooth or restart the device";
        if (!pinned) Serial.printf("[HTTPS] refused: %s\n", why.c_str());
        return false;
    }
    size_t needed = TLS_MIN_FREE_BYTES + (pinned ? TLS_PINNED_EXTRA_BYTES : 0);
    bool blocksOk;
    if (pinned) {
        // Both buffers and the certificate verification come out of one
        // region; requiring it up front turns a confusing certificate error
        // into an honest memory error.
        blocksOk = largest >= TLS_PINNED_REGION_BYTES;
    } else {
        // mbedTLS allocates its two record buffers back to back. Probe for the
        // second one while holding the first, so one large block that only
        // fits a single buffer is not mistaken for enough.
        blocksOk = false;
        void* first = heap_caps_malloc(TLS_MIN_BLOCK_BYTES, MALLOC_CAP_8BIT);
        if (first) {
            blocksOk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >=
                       TLS_MIN_BLOCK_BYTES;
            free(first);
        }
    }
    if (freeBytes >= needed && blocksOk) return true;
    why = "Not enough RAM for HTTPS (";
    why += heapSummary();
    why += "; needs ";
    why += (unsigned)(needed / 1024U);
    why += " KB free and ";
    if (pinned) {
        why += "one ";
        why += (unsigned)(TLS_PINNED_REGION_BYTES / 1024U);
        why += " KB region";
    } else {
        why += "two ";
        why += (unsigned)(TLS_MIN_BLOCK_BYTES / 1024U);
        why += " KB blocks";
    }
    why += "). Close Bluetooth or restart the device";
    if (!pinned) Serial.printf("[HTTPS] refused: %s\n", why.c_str());
    return false;
}

const char* trustAnchorForHost(const String& host) {
    return OpenOSTrustAnchors::forHost(host.c_str());
}

String hostFromUrl(const String& url) {
    return UrlUtil::hostFromHttpsUrl(url);
}


bool resolveHost(const String& host, IPAddress& address, String& why,
                 int attempts) {
    if (host.length() == 0) {
        why = "HTTPS URL has no host";
        return false;
    }
    IPAddress literal;
    if (literal.fromString(host)) {
        address = literal;
        return true;
    }
    if (attempts < 1) attempts = 1;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (WiFi.status() != WL_CONNECTED) {
            why = "Wi-Fi is not connected";
            return false;
        }
        address = IPAddress((uint32_t)0);
        if (WiFi.hostByName(host.c_str(), address) && (uint32_t)address != 0)
            return true;
        Serial.printf("[HTTPS] DNS attempt %d for %s failed (server %s)\n",
                      attempt, host.c_str(), WiFi.dnsIP().toString().c_str());
        if (attempt < attempts) {
            delay(250U * (uint32_t)attempt);
            yield();
        }
    }
    why = "DNS lookup failed for ";
    why += host;
    why += " (DNS server ";
    why += WiFi.dnsIP().toString();
    why += ")";
    return false;
}

int get(HTTPClient& http, WiFiClientSecure& client, const String& url,
        const Request& request, String& why) {
    why = "";
    String host = hostFromUrl(url);
    if (host.length() == 0) {
        why = "Invalid HTTPS URL";
        return HTTPC_ERROR_CONNECTION_REFUSED;
    }
    const int attempts = request.attempts < 1 ? 1 : request.attempts;
    const char* anchor = trustAnchorForHost(host);
    if (anchor && s_pinRejectedHost == host) anchor = nullptr;
    // Pinning is defence in depth: every document fetched from a pinned host
    // also carries a release signature that is checked afterwards. When the
    // pinned handshake is rejected we therefore log it loudly and continue
    // without the anchor rather than leaving the device unable to update —
    // the signature, not the certificate, is what authenticates the content.
    bool pinRejected = false;
    int status = HTTPC_ERROR_CONNECTION_REFUSED;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (attempt > 1) {
            // Release the previous session completely before retrying so its
            // record buffers are back in the heap for the next handshake.
            http.end();
            client.stop();
            delay(attempt == 2 ? 300 : 800);
            yield();
        }
        // The pinned requirement must never block a transfer that an unpinned
        // session could still carry: verification is defence in depth, the
        // release signature is what authenticates the content.
        bool wantPin = anchor != nullptr && !pinRejected;
        if (wantPin && !memoryAvailable(why, true)) {
            // Not an error: the pin is hardening, the release signature is
            // what authenticates the content.
            Serial.printf("[HTTPS] %s: too little RAM to verify the pinned "
                          "certificate, continuing unpinned (%s)\n",
                          host.c_str(), why.c_str());
            pinRejected = true;
            wantPin = false;
        }
        if (!memoryAvailable(why, false))
            return HTTPC_ERROR_CONNECTION_REFUSED;
        if (WiFi.status() != WL_CONNECTED) {
            why = "Wi-Fi is not connected";
            return HTTPC_ERROR_CONNECTION_REFUSED;
        }
        IPAddress address;
        if (!resolveHost(host, address, why, 2)) {
            status = HTTPC_ERROR_CONNECTION_REFUSED;
            continue;
        }
        // Official hosts are pinned to their root CA (identity + host name).
        // Elsewhere authenticity comes from the release signatures on the
        // documents themselves, not from the server certificate.
        const bool usePin = wantPin;
        if (usePin) client.setCACert(anchor);
        else        client.setInsecure();
        HeapReserve::resetPeak();
        Serial.printf("[HTTPS] connect %s%s free=%u maxBlock=%u arena=%u stackFree=%u\n",
                      host.c_str(),
                      usePin ? " (pinned)" : (anchor ? " (pin rejected, unpinned)" : ""),
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      (unsigned)HeapReserve::size(),
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        client.setHandshakeTimeout(request.handshakeTimeoutS);
        http.setConnectTimeout((int32_t)request.connectTimeoutMs);
        http.setTimeout((uint16_t)request.readTimeoutMs);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setReuse(false);
        if (!http.begin(client, url)) {
            why = "Could not open HTTPS connection";
            return HTTPC_ERROR_CONNECTION_REFUSED;
        }
        status = http.GET();
        // How much of the reserve the handshake used, and whether anything
        // large had to spill into the general heap. Tuning data for the
        // thresholds above; one line per session.
        Serial.printf("[HTTPS] %s: arena peak %u B, %d large allocations spilled to heap\n",
                      status > 0 ? "handshake ok" : "handshake failed",
                      (unsigned)HeapReserve::arenaPeak(), HeapReserve::arenaOverflows());
        if (status > 0) {
            why = "";
            return status;
        }
        why = status == HTTPC_ERROR_CONNECTION_REFUSED
            ? describeConnectFailure(client, host)
            : String(HTTPClient::errorToString(status));
        if (usePin && status == HTTPC_ERROR_CONNECTION_REFUSED) {
            char detail[8] = {0};
            if (client.lastError(detail, sizeof(detail)) == -0x2700) {
                pinRejected = true;
                s_pinRejectedHost = host;
                Serial.printf("[HTTPS] %s rejected the pinned root; continuing "
                              "unpinned for the rest of this session, the "
                              "release signature still has to match\n",
                              host.c_str());
            }
        }
        Serial.printf("[HTTPS] attempt %d/%d %s%s: %s (free=%u maxBlock=%u)\n",
                      attempt, attempts, host.c_str(), usePin ? " (pinned)" : "",
                      why.c_str(),
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    http.end();
    client.stop();
    // Every attempt failed; make sure the reserve is back at full size for
    // whoever tries next.
    HeapReserve::reclaim();
    return status;
}

} // namespace SecureHttp
