#pragma once

#include <string.h>
#if defined(_MSC_VER)
#define strcasecmp _stricmp
#elif !defined(ARDUINO)
#include <strings.h>
#endif

// TLS trust anchor for the host OpenOS fetches signed content from.
//
// This is Let's Encrypt YR1, the intermediate that issues the certificate
// raw.githubusercontent.com presents (valid to 2028-09-02, SHA-256
// 13949634D99CD6FD6AA80BC034FEFACCEB1969FEEF986586713ECDBB05758D3F).
//
// Only one certificate is pinned, and deliberately not a root. Every
// certificate in the trust store is parsed and held for the whole handshake,
// and on this board that memory competes with mbedTLS's two 16 KB record
// buffers; pinning the ISRG roots as well cost several KB and made the
// handshake fail in exactly the situations where an update matters — from
// inside a running application. Trusting the leaf's own issuer also makes
// verification a single RSA-2048 check instead of a walk through two
// RSA-4096 signatures.
//
// This is hardening, not the basis of trust: SecureHttp only attempts a
// pinned handshake when there is enough headroom for it to succeed, and
// falls back to an unpinned session otherwise. Authenticity always comes
// from the ECDSA release signature on the manifest and catalog and from the
// SHA-256 those documents carry for every image and package. When YR1 is
// rotated the pin simply stops matching and every fetch takes the unpinned
// path until OpenOS is republished with the new certificate.
namespace OpenOSTrustAnchors {

static const char GITHUB_ROOTS[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIE2zCCAsOgAwIBAgIRAKICU/FfJpHAXcHOE7m8yk4wDQYJKoZIhvcNAQELBQAw\n"
    "LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw\n"
    "HhcNMjUwOTAzMDAwMDAwWhcNMjgwOTAyMjM1OTU5WjAzMQswCQYDVQQGEwJVUzEW\n"
    "MBQGA1UEChMNTGV0J3MgRW5jcnlwdDEMMAoGA1UEAxMDWVIxMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAoVi8X2xCYgMXvJxNPKp/oF13UMgmPABB07VC\n"
    "LNDtoXmt9luEZNJSBV10VyT1Pz6LD8Zq1d2gc43WNl1AdRrj4sEnazbOiz0nPpmG\n"
    "Bp2hui49oZtDIY6wdKeZAi5BbNU20CH6RSBBMLSQ9cXrH8dxdv4PAJ45ssGML68U\n"
    "SE3BsjC2a6cAN9L5CgXVIQi5tfNiTPoFZZ3S0OlXqLmmtdV95udWAb5b6e/F49Di\n"
    "CsH0Y00Ag72BVIb1hzynmKe+X0mERBTtsb3BwmpV9ipeBjMLoR/D9cHxHQCWoi5l\n"
    "TmXwY015J5rGelz1nZjJuxc2kioaX29XJBnhMkP531rSdG5uMwIDAQABo4HuMIHr\n"
    "MA4GA1UdDwEB/wQEAwIBhjATBgNVHSUEDDAKBggrBgEFBQcDATASBgNVHRMBAf8E\n"
    "CDAGAQH/AgEAMB0GA1UdDgQWBBQfLzW+RhSCzUCxrnksVXj699Ro+zAfBgNVHSME\n"
    "GDAWgBTe51tg0CJtQCh9Pw0B/qS1UrRRlDAyBggrBgEFBQcBAQQmMCQwIgYIKwYB\n"
    "BQUHMAKGFmh0dHA6Ly95ci5pLmxlbmNyLm9yZy8wEwYDVR0gBAwwCjAIBgZngQwB\n"
    "AgEwJwYDVR0fBCAwHjAcoBqgGIYWaHR0cDovL3lyLmMubGVuY3Iub3JnLzANBgkq\n"
    "hkiG9w0BAQsFAAOCAgEA0+zvMq3kHig1ddTmmm+RibTr9/RpX7k4buanMMRqbV/y\n"
    "IvP82zAHN3mvaw+cASuVsdpd0ikjhr4hnhJQLQOzOp2ccKrsdGOAgo0vddeISFAq\n"
    "EWEV4lmUM3vFF796up+bSgmJ1u6RupDCMxDgF8M3eLvGuj6L0lu3zkQ0KuQLnKxL\n"
    "tB0oQqn1Idg5CuuGpMvQzk29Pa3D/qHurc0EIM9SxukQuJqq63lxsYyRQFU8yMBO\n"
    "hq1w5LbfaWNRrz1uklOfI/pYkAb2E2MTZrAMQkBIE2S8Jt1F8gRc96o/xOsrgvSk\n"
    "a84AisX6xq1lz1Z7jGvrnXc4TMcjxZTjiTaihcYI1JIXZiLtEMSCa5l3cu8YWd6z\n"
    "dLRQlqRdclVjuQfNHawRJ6GWlkK0QJosivTKwdBw3KxEtzGo8yMHERbsy57gP1UX\n"
    "HOMcmZYQC0gtyR3SxfenIM/MxC3Ia2Ypab/kQ/CTnlIn2KQ5JUC6NYrGCbhFN9bp\n"
    "5lKJStEwCUnLpntcrXk5XVDCNv/5RyWpRThkGOV7GetKkQ0qAY8hCzWK6oqnAhDZ\n"
    "cjlYVdWfqOw3DIOX6EDNBgAqHarRVxyF9QZdOaXSyPJ0ueD2BYJEBgaCGQ8rAaU/\n"
    "Qc123V5LTXDZW4CcsPBDyhy4v+c8hClAyw/IkJlfBqxB9D+/wvIMHgECZ4ptP6o=\n"
    "-----END CERTIFICATE-----\n";

struct Anchor {
    // Matched against the end of the host name, so "githubusercontent.com"
    // covers raw.githubusercontent.com and objects.githubusercontent.com.
    const char* hostSuffix;
    const char* pem;
};

static const Anchor ANCHORS[] = {
    { "githubusercontent.com", GITHUB_ROOTS },
};

// Returns the PEM to pass to WiFiClientSecure::setCACert(), or nullptr when
// the host is not pinned.
inline const char* forHost(const char* host) {
    if (!host) return nullptr;
    size_t hostLength = strlen(host);
    for (const Anchor& anchor : ANCHORS) {
        size_t suffixLength = strlen(anchor.hostSuffix);
        if (hostLength < suffixLength) continue;
        const char* tail = host + hostLength - suffixLength;
        if (strcasecmp(tail, anchor.hostSuffix) != 0) continue;
        // The whole host or a subdomain boundary, never "evilgithubusercontent.com".
        if (hostLength == suffixLength || tail[-1] == '.') return anchor.pem;
    }
    return nullptr;
}

} // namespace OpenOSTrustAnchors
