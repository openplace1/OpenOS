#include "SecureHttp.h"

#include "HeapReserve.h"
#include "OpenOSTrustAnchors.h"
#include "UrlUtil.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/x509_crt.h>
#include <string.h>

extern bool sysBTEnabled;
extern bool osaSuspendBluetoothForMemory(const char* reason);

namespace SecureHttp {
namespace {

static String heapSummary() {
    String summary = "free ";
    summary += (unsigned)(ESP.getFreeHeap() / 1024U);
    summary += " KB, largest block ";
    summary += (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024U);
    summary += " KB";
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
    if (sysBTEnabled) osaSuspendBluetoothForMemory(operation);
    Serial.printf("[%s] %s free=%u maxBlock=%u\n", tag ? tag : "HTTPS",
                  operation ? operation : "HTTPS",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool memoryAvailable(String& why, bool pinned) {
    // The reserved block exists precisely for this moment.
    HeapReserve::release("HTTPS");
    size_t freeBytes = ESP.getFreeHeap();
    size_t needed = TLS_MIN_FREE_BYTES + (pinned ? TLS_PINNED_EXTRA_BYTES : 0);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
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
    return false;
}

const char* trustAnchorForHost(const String& host) {
    return OpenOSTrustAnchors::forHost(host.c_str());
}

String hostFromUrl(const String& url) {
    return UrlUtil::hostFromHttpsUrl(url);
}

void diagnoseCertificateChain(const String& host) {
    const char* anchor = trustAnchorForHost(host);
    Serial.printf("[HTTPS] diag %s (%s) free=%u maxBlock=%u\n", host.c_str(),
                  anchor ? "pinned" : "not pinned", (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[HTTPS] diag: Wi-Fi is not connected");
        return;
    }

    // Copy the presented chain out as DER, then drop the TLS session before
    // verifying: the point of the diagnostic is to see whether verification
    // succeeds when it is *not* competing with the two 16 KB record buffers.
    static constexpr int MAX_CHAIN = 4;
    uint8_t* der[MAX_CHAIN] = {nullptr, nullptr, nullptr, nullptr};
    size_t derLength[MAX_CHAIN] = {0, 0, 0, 0};
    int count = 0;
    {
        WiFiClientSecure probe;
        probe.setInsecure();
        probe.setHandshakeTimeout(20);
        if (!probe.connect(host.c_str(), 443)) {
            char detail[96] = {0};
            int error = probe.lastError(detail, sizeof(detail));
            Serial.printf("[HTTPS] diag: insecure connect failed (%d %s)\n", error, detail);
            return;
        }
        const mbedtls_x509_crt* peer = probe.getPeerCertificate();
        for (const mbedtls_x509_crt* cert = peer; cert && count < MAX_CHAIN;
             cert = cert->next) {
            char subject[128] = {0};
            char issuer[128] = {0};
            char algorithm[64] = {0};
            mbedtls_x509_dn_gets(subject, sizeof(subject), &cert->subject);
            mbedtls_x509_dn_gets(issuer, sizeof(issuer), &cert->issuer);
            mbedtls_x509_sig_alg_gets(algorithm, sizeof(algorithm), &cert->sig_oid,
                                      cert->sig_pk, cert->sig_md, cert->sig_opts);
            Serial.printf("[HTTPS] diag cert %d: %u bits %s\n    subject %s\n    issuer  %s\n",
                          count, (unsigned)mbedtls_pk_get_bitlen(&cert->pk), algorithm,
                          subject, issuer);
            der[count] = (uint8_t*)malloc(cert->raw.len);
            if (!der[count]) break;
            memcpy(der[count], cert->raw.p, cert->raw.len);
            derLength[count] = cert->raw.len;
            ++count;
        }
        if (!peer) Serial.println("[HTTPS] diag: server sent no certificate");
        probe.stop();
    }

    if (anchor && count > 0) {
        Serial.printf("[HTTPS] diag: session closed, free=%u maxBlock=%u\n",
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        mbedtls_x509_crt chain;
        mbedtls_x509_crt_init(&chain);
        mbedtls_x509_crt ca;
        mbedtls_x509_crt_init(&ca);
        bool built = true;
        for (int i = 0; i < count && built; ++i)
            built = mbedtls_x509_crt_parse_der(&chain, der[i], derLength[i]) == 0;
        int parsed = mbedtls_x509_crt_parse(&ca, (const uint8_t*)anchor, strlen(anchor) + 1);
        if (!built || parsed != 0) {
            Serial.printf("[HTTPS] diag: rebuild failed (chain=%d anchors=-0x%04x)\n",
                          built ? 1 : 0, (unsigned)-parsed);
        } else {
            uint32_t flags = 0;
            int result = mbedtls_x509_crt_verify(&chain, &ca, nullptr, host.c_str(),
                                                 &flags, nullptr, nullptr);
            char info[320] = {0};
            mbedtls_x509_crt_verify_info(info, sizeof(info), "    ", flags);
            Serial.printf("[HTTPS] diag: verify with free heap -> ret=-0x%04x flags=0x%08x "
                          "(free=%u maxBlock=%u)\n%s",
                          (unsigned)-result, (unsigned)flags, (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), info);
        }
        mbedtls_x509_crt_free(&ca);
        mbedtls_x509_crt_free(&chain);
    }
    for (int i = 0; i < count; ++i) free(der[i]);
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
        if (!memoryAvailable(why, anchor != nullptr))
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
        if (anchor) client.setCACert(anchor);
        else        client.setInsecure();
        Serial.printf("[HTTPS] connect %s%s free=%u maxBlock=%u\n", host.c_str(),
                      anchor ? " (pinned)" : "", (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
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
        if (status > 0) return status;
        why = status == HTTPC_ERROR_CONNECTION_REFUSED
            ? describeConnectFailure(client, host)
            : String(HTTPClient::errorToString(status));
        Serial.printf("[HTTPS] attempt %d/%d %s%s: %s (free=%u maxBlock=%u)\n",
                      attempt, attempts, host.c_str(), anchor ? " (pinned)" : "",
                      why.c_str(),
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    http.end();
    client.stop();
    return status;
}

} // namespace SecureHttp
