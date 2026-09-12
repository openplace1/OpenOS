#pragma once

#include "PortableString.h"

namespace UrlUtil {

// Host part of an https:// URL without userinfo or port. Empty when the URL
// is not https.
inline String hostFromHttpsUrl(const String& url) {
    if (!url.startsWith("https://")) return String();
    const int start = 8;
    int end = url.indexOf('/', start);
    if (end < 0) end = url.length();
    String host = url.substring(start, end);
    int at = host.indexOf('@');
    if (at >= 0) host = host.substring(at + 1);
    int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
    host.trim();
    return host;
}

} // namespace UrlUtil
