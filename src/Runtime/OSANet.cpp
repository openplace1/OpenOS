#include "OSANet.h"

#include "HeapReserve.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <string.h>

namespace OSANet {
namespace {

struct Buffer {
    uint8_t* data = nullptr;
    size_t   size = 0;
    bool     inReserve = false;
};
static Buffer s_buffers[MAX_BUFFERS];

static WiFiClient* s_sockets[MAX_SOCKETS] = {nullptr, nullptr, nullptr, nullptr};
static WiFiServer* s_server = nullptr;
static WiFiUDP*    s_udp = nullptr;

static bool validSocket(int sock) {
    return sock >= 0 && sock < MAX_SOCKETS && s_sockets[sock] != nullptr;
}

static int freeSocketSlot() {
    for (int i = 0; i < MAX_SOCKETS; ++i)
        if (!s_sockets[i]) return i;
    return -1;
}

} // namespace

// ── Buffers ────────────────────────────────────────────────────────────────

int bufAlloc(size_t bytes) {
    if (bytes == 0 || bytes > MAX_BUFFER_BYTES) return -1;
    int id = -1;
    for (int i = 0; i < MAX_BUFFERS; ++i)
        if (!s_buffers[i].data) { id = i; break; }
    if (id < 0) return -1;
    Buffer& b = s_buffers[id];
    // Large buffers come from the reserve when it has room (a 16 KB tile
    // buffer would otherwise rarely find one block in an application-time
    // heap); small ones from the general heap.
    b.inReserve = false;
    b.data = nullptr;
    if (bytes >= 2048) {
        b.data = (uint8_t*)HeapReserve::allocate(bytes, "buffer");
        b.inReserve = b.data != nullptr;
    }
    if (!b.data) b.data = (uint8_t*)malloc(bytes);
    if (!b.data) return -1;
    memset(b.data, 0, bytes);
    b.size = bytes;
    return id;
}

bool bufFree(int id) {
    if (id < 0 || id >= MAX_BUFFERS || !s_buffers[id].data) return false;
    Buffer& b = s_buffers[id];
    if (b.inReserve) HeapReserve::deallocate(b.data);
    else             free(b.data);
    b.data = nullptr;
    b.size = 0;
    b.inReserve = false;
    return true;
}

size_t bufLength(int id) {
    if (id < 0 || id >= MAX_BUFFERS) return 0;
    return s_buffers[id].size;
}

uint8_t* bufData(int id) {
    if (id < 0 || id >= MAX_BUFFERS) return nullptr;
    return s_buffers[id].data;
}

// ── TCP ────────────────────────────────────────────────────────────────────

int tcpConnect(const String& host, uint16_t port, uint32_t timeoutMs) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    int slot = freeSocketSlot();
    if (slot < 0) return -1;
    WiFiClient* client = new (std::nothrow) WiFiClient();
    if (!client) return -1;
    client->setTimeout((uint16_t)min(timeoutMs / 1000U, 60U));
    if (!client->connect(host.c_str(), port, (int32_t)timeoutMs)) {
        delete client;
        return -1;
    }
    client->setNoDelay(true);
    s_sockets[slot] = client;
    return slot;
}

bool tcpListen(uint16_t port) {
    if (WiFi.status() != WL_CONNECTED) return false;
    tcpStopListening();
    s_server = new (std::nothrow) WiFiServer(port, 2);
    if (!s_server) return false;
    s_server->begin();
    s_server->setNoDelay(true);
    return true;
}

void tcpStopListening() {
    if (!s_server) return;
    s_server->stop();
    delete s_server;
    s_server = nullptr;
}

int tcpAccept() {
    if (!s_server) return -1;
    WiFiClient incoming = s_server->available();
    if (!incoming) return -1;
    int slot = freeSocketSlot();
    if (slot < 0) { incoming.stop(); return -1; }
    WiFiClient* client = new (std::nothrow) WiFiClient(incoming);
    if (!client) { incoming.stop(); return -1; }
    client->setNoDelay(true);
    s_sockets[slot] = client;
    return slot;
}

bool tcpConnected(int sock) {
    if (!validSocket(sock)) return false;
    return s_sockets[sock]->connected() || s_sockets[sock]->available() > 0;
}

int tcpAvailable(int sock) {
    if (!validSocket(sock)) return 0;
    return s_sockets[sock]->available();
}

int tcpSend(int sock, const uint8_t* data, size_t count) {
    if (!validSocket(sock) || !data) return -1;
    if (!s_sockets[sock]->connected()) return -1;
    size_t sent = s_sockets[sock]->write(data, count);
    return (int)sent;
}

int tcpReceive(int sock, uint8_t* out, size_t maximum) {
    if (!validSocket(sock) || !out || maximum == 0) return -1;
    WiFiClient* client = s_sockets[sock];
    int waiting = client->available();
    if (waiting <= 0) return client->connected() ? 0 : -1;
    size_t take = min((size_t)waiting, maximum);
    int got = client->read(out, take);
    return got < 0 ? -1 : got;
}

void tcpClose(int sock) {
    if (!validSocket(sock)) return;
    s_sockets[sock]->stop();
    delete s_sockets[sock];
    s_sockets[sock] = nullptr;
}

String tcpRemoteIP(int sock) {
    if (!validSocket(sock)) return String();
    return s_sockets[sock]->remoteIP().toString();
}

// ── UDP ────────────────────────────────────────────────────────────────────

bool udpBegin(uint16_t port) {
    if (WiFi.status() != WL_CONNECTED) return false;
    udpEnd();
    s_udp = new (std::nothrow) WiFiUDP();
    if (!s_udp) return false;
    if (!s_udp->begin(port)) { delete s_udp; s_udp = nullptr; return false; }
    return true;
}

void udpEnd() {
    if (!s_udp) return;
    s_udp->stop();
    delete s_udp;
    s_udp = nullptr;
}

int udpSend(const String& host, uint16_t port, const uint8_t* data, size_t count) {
    if (!s_udp || !data) return -1;
    if (!s_udp->beginPacket(host.c_str(), port)) return -1;
    size_t written = s_udp->write(data, count);
    if (!s_udp->endPacket()) return -1;
    return (int)written;
}

int udpReceive(uint8_t* out, size_t maximum) {
    if (!s_udp || !out) return 0;
    int size = s_udp->parsePacket();
    if (size <= 0) return 0;
    int got = s_udp->read(out, min((size_t)size, maximum));
    // Anything that did not fit is dropped with the datagram.
    return got < 0 ? 0 : got;
}

String udpRemoteIP() { return s_udp ? s_udp->remoteIP().toString() : String(); }
uint16_t udpRemotePort() { return s_udp ? s_udp->remotePort() : 0; }

// ── Pixels ─────────────────────────────────────────────────────────────────

size_t screenRead(TFT_eSPI* tft, int x, int y, int w, int h, uint8_t* out, size_t capacity) {
    if (!tft || !out || w <= 0 || h <= 0) return 0;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 240) w = 240 - x;
    if (y + h > 320) h = 320 - y;
    if (w <= 0 || h <= 0) return 0;
    size_t bytes = (size_t)w * (size_t)h * 2U;
    if (bytes > capacity) return 0;
    tft->readRect(x, y, w, h, (uint16_t*)out);
    return bytes;
}

size_t screenWrite(TFT_eSPI* tft, int x, int y, int w, int h, const uint8_t* in, size_t available) {
    if (!tft || !in || w <= 0 || h <= 0) return 0;
    size_t bytes = (size_t)w * (size_t)h * 2U;
    if (bytes > available) return 0;
    tft->pushImage(x, y, w, h, (const uint16_t*)in);
    return bytes;
}

static size_t spriteBytes(TFT_eSprite* sprite) {
    if (!sprite) return 0;
    size_t pixels = (size_t)sprite->width() * (size_t)sprite->height();
    switch (sprite->getColorDepth()) {
        case 16: return pixels * 2U;
        case 8:  return pixels;
        case 1:  return (pixels + 7U) / 8U;
        default: return 0;
    }
}

size_t spriteRead(TFT_eSprite* sprite, uint8_t* out, size_t capacity) {
    size_t bytes = spriteBytes(sprite);
    if (!bytes || !out || bytes > capacity) return 0;
    const void* pixels = sprite->getPointer();
    if (!pixels) return 0;
    memcpy(out, pixels, bytes);
    return bytes;
}

size_t spriteWrite(TFT_eSprite* sprite, const uint8_t* in, size_t available) {
    size_t bytes = spriteBytes(sprite);
    if (!bytes || !in || bytes > available) return 0;
    void* pixels = sprite->getPointer();
    if (!pixels) return 0;
    memcpy(pixels, in, bytes);
    return bytes;
}

void reset() {
    for (int i = 0; i < MAX_SOCKETS; ++i) tcpClose(i);
    tcpStopListening();
    udpEnd();
    for (int i = 0; i < MAX_BUFFERS; ++i) bufFree(i);
}

} // namespace OSANet
