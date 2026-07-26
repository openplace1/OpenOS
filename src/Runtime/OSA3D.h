#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Lightweight immediate-mode pseudo-3D renderer for OSA.
//
// It intentionally keeps no vertex heap and uses float math. A complete cube
// is transformed and rendered inside one native call, avoiding dozens of VM
// crossings per frame on classic ESP32 boards.
class OSA3DRenderer {
public:
    struct Vec3 {
        float x;
        float y;
        float z;
    };

    struct Projected {
        int16_t x;
        int16_t y;
        float z;
    };

    void reset();
    void setCamera(float centerX, float centerY, float focalLength,
                   float cameraDistance);

    bool project(float x, float y, float z, int16_t& screenX,
                 int16_t& screenY) const;
    bool drawLine(TFT_eSPI* tft, TFT_eSprite* sprite,
                  float x1, float y1, float z1,
                  float x2, float y2, float z2,
                  uint16_t color) const;
    bool drawPoint(TFT_eSPI* tft, TFT_eSprite* sprite,
                   float x, float y, float z, int radius,
                   uint16_t color) const;
    bool drawTriangle(TFT_eSPI* tft, TFT_eSprite* sprite,
                      float x1, float y1, float z1,
                      float x2, float y2, float z2,
                      float x3, float y3, float z3,
                      bool filled, uint16_t color) const;
    int drawCube(TFT_eSPI* tft, TFT_eSprite* sprite,
                 float x, float y, float z, float size,
                 float rotationX, float rotationY, float rotationZ,
                 int mode, uint16_t baseColor, uint16_t edgeColor);
    int drawGrid(TFT_eSPI* tft, TFT_eSprite* sprite,
                 float y, float halfSize, float step,
                 uint16_t color) const;
    void drawAxes(TFT_eSPI* tft, TFT_eSprite* sprite, float size) const;

    float centerX() const { return cameraCenterX; }
    float centerY() const { return cameraCenterY; }
    float focal() const { return focalLength; }
    float distance() const { return cameraDistance; }
    uint32_t lastRenderMicros() const { return renderMicros; }
    int lastFaceCount() const { return renderedFaces; }

    static uint16_t shade565(uint16_t color, float amount);

private:
    static Vec3 rotate(const Vec3& value,
                       float sinX, float cosX,
                       float sinY, float cosY,
                       float sinZ, float cosZ);
    bool projectVec(const Vec3& value, Projected& output) const;
    static void line2D(TFT_eSPI* tft, TFT_eSprite* sprite,
                       int x1, int y1, int x2, int y2, uint16_t color);
    static void point2D(TFT_eSPI* tft, TFT_eSprite* sprite,
                        int x, int y, int radius, uint16_t color);
    static void triangle2D(TFT_eSPI* tft, TFT_eSprite* sprite,
                           const Projected& a, const Projected& b,
                           const Projected& c, bool filled, uint16_t color);

    float cameraCenterX = 120.0f;
    float cameraCenterY = 160.0f;
    float focalLength = 110.0f;
    float cameraDistance = 5.0f;
    uint32_t renderMicros = 0;
    int renderedFaces = 0;
};
