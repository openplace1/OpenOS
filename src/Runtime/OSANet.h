#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Low-level building blocks for OSA: byte buffers, raw TCP/UDP sockets and
// pixel access to the screen and the active sprite. Together they let a
// script implement things like a remote display, a screen cast or a custom
// binary protocol without the SDK having to know about any of them.
//
// One script runs at a time, so everything here is static; reset() at
// script unload closes every socket and frees every buffer.
namespace OSANet {

static constexpr int MAX_BUFFERS = 8;
static constexpr size_t MAX_BUFFER_BYTES = 64U * 1024U;
static constexpr int MAX_SOCKETS = 4;

// ── Buffers ────────────────────────────────────────────────────────────────
int      bufAlloc(size_t bytes);           // id or -1
bool     bufFree(int id);
size_t   bufLength(int id);                // 0 when unknown
uint8_t* bufData(int id);                  // nullptr when unknown

// ── TCP ────────────────────────────────────────────────────────────────────
int  tcpConnect(const String& host, uint16_t port, uint32_t timeoutMs);   // socket id or -1
bool tcpListen(uint16_t port);
void tcpStopListening();
int  tcpAccept();                          // socket id or -1 when nobody is waiting
bool tcpConnected(int sock);
int  tcpAvailable(int sock);
int  tcpSend(int sock, const uint8_t* data, size_t count);   // bytes written or -1
int  tcpReceive(int sock, uint8_t* out, size_t maximum);     // bytes read, 0 when none, -1 closed
void tcpClose(int sock);
String tcpRemoteIP(int sock);

// ── UDP ────────────────────────────────────────────────────────────────────
bool udpBegin(uint16_t port);
void udpEnd();
int  udpSend(const String& host, uint16_t port, const uint8_t* data, size_t count);
int  udpReceive(uint8_t* out, size_t maximum);   // one datagram or 0
String udpRemoteIP();
uint16_t udpRemotePort();

// ── Pixels ─────────────────────────────────────────────────────────────────
// Screen pixels are RGB565, two bytes each in memory order.
size_t screenRead(TFT_eSPI* tft, int x, int y, int w, int h, uint8_t* out, size_t capacity);
size_t screenWrite(TFT_eSPI* tft, int x, int y, int w, int h, const uint8_t* in, size_t available);
// Sprite pixels in the sprite's own depth (1, 8 or 16 bits per pixel).
size_t spriteRead(TFT_eSprite* sprite, uint8_t* out, size_t capacity);
size_t spriteWrite(TFT_eSprite* sprite, const uint8_t* in, size_t available);

void reset();

} // namespace OSANet
