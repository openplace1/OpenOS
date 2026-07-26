#include "OSA3D.h"

#include <math.h>

static_assert(sizeof(OSA3DRenderer) <= 32,
              "OSA3DRenderer must stay heap-free and lightweight");

namespace {

struct CubeFace {
    uint8_t vertex[4];
    OSA3DRenderer::Vec3 normal;
};

// Vertices use a right-handed system. The camera sits on negative Z and looks
// towards positive Z, hence faces whose rotated normal has negative Z point
// towards the viewer.
static const CubeFace CUBE_FACES[6] = {
    {{0, 3, 2, 1}, { 0.0f,  0.0f, -1.0f}},
    {{4, 5, 6, 7}, { 0.0f,  0.0f,  1.0f}},
    {{0, 4, 7, 3}, {-1.0f,  0.0f,  0.0f}},
    {{1, 2, 6, 5}, { 1.0f,  0.0f,  0.0f}},
    {{0, 1, 5, 4}, { 0.0f, -1.0f,  0.0f}},
    {{3, 7, 6, 2}, { 0.0f,  1.0f,  0.0f}},
};

static const uint8_t CUBE_EDGES[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static float clampFloat(float value, float minimum, float maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

} // namespace

void OSA3DRenderer::reset() {
    cameraCenterX = 120.0f;
    cameraCenterY = 160.0f;
    focalLength = 110.0f;
    cameraDistance = 5.0f;
    renderMicros = 0;
    renderedFaces = 0;
}

void OSA3DRenderer::setCamera(float centerXValue, float centerYValue,
                              float focalLengthValue,
                              float cameraDistanceValue) {
    if (isfinite(centerXValue))
        cameraCenterX = clampFloat(centerXValue, -4096.0f, 4096.0f);
    if (isfinite(centerYValue))
        cameraCenterY = clampFloat(centerYValue, -4096.0f, 4096.0f);
    if (isfinite(focalLengthValue))
        focalLength = clampFloat(focalLengthValue, 8.0f, 2048.0f);
    if (isfinite(cameraDistanceValue))
        cameraDistance = clampFloat(cameraDistanceValue, 0.25f, 4096.0f);
}

OSA3DRenderer::Vec3 OSA3DRenderer::rotate(
        const Vec3& value,
        float sinX, float cosX,
        float sinY, float cosY,
        float sinZ, float cosZ) {
    const float x1 = value.x;
    const float y1 = value.y * cosX - value.z * sinX;
    const float z1 = value.y * sinX + value.z * cosX;

    const float x2 = x1 * cosY + z1 * sinY;
    const float y2 = y1;
    const float z2 = -x1 * sinY + z1 * cosY;

    Vec3 output;
    output.x = x2 * cosZ - y2 * sinZ;
    output.y = x2 * sinZ + y2 * cosZ;
    output.z = z2;
    return output;
}

bool OSA3DRenderer::projectVec(const Vec3& value, Projected& output) const {
    const float depth = cameraDistance + value.z;
    if (depth <= 0.08f) return false;

    const float scale = focalLength / depth;
    const float projectedX = cameraCenterX + value.x * scale;
    const float projectedY = cameraCenterY - value.y * scale;
    if (!isfinite(projectedX) || !isfinite(projectedY) ||
        projectedX < -32760.0f || projectedX > 32760.0f ||
        projectedY < -32760.0f || projectedY > 32760.0f) {
        return false;
    }

    output.x = (int16_t)lroundf(projectedX);
    output.y = (int16_t)lroundf(projectedY);
    output.z = value.z;
    return true;
}

bool OSA3DRenderer::project(float x, float y, float z,
                            int16_t& screenX, int16_t& screenY) const {
    Projected projected;
    if (!projectVec({x, y, z}, projected)) return false;
    screenX = projected.x;
    screenY = projected.y;
    return true;
}

void OSA3DRenderer::line2D(TFT_eSPI* tft, TFT_eSprite* sprite,
                           int x1, int y1, int x2, int y2,
                           uint16_t color) {
    if (sprite) sprite->drawLine(x1, y1, x2, y2, color);
    else        tft->drawLine(x1, y1, x2, y2, color);
}

void OSA3DRenderer::point2D(TFT_eSPI* tft, TFT_eSprite* sprite,
                            int x, int y, int radius, uint16_t color) {
    radius = constrain(radius, 1, 32);
    if (sprite) sprite->fillCircle(x, y, radius, color);
    else        tft->fillCircle(x, y, radius, color);
}

void OSA3DRenderer::triangle2D(TFT_eSPI* tft, TFT_eSprite* sprite,
                               const Projected& a, const Projected& b,
                               const Projected& c, bool filled,
                               uint16_t color) {
    if (filled) {
        if (sprite) sprite->fillTriangle(a.x, a.y, b.x, b.y, c.x, c.y, color);
        else        tft->fillTriangle(a.x, a.y, b.x, b.y, c.x, c.y, color);
        return;
    }
    line2D(tft, sprite, a.x, a.y, b.x, b.y, color);
    line2D(tft, sprite, b.x, b.y, c.x, c.y, color);
    line2D(tft, sprite, c.x, c.y, a.x, a.y, color);
}

bool OSA3DRenderer::drawLine(TFT_eSPI* tft, TFT_eSprite* sprite,
                             float x1, float y1, float z1,
                             float x2, float y2, float z2,
                             uint16_t color) const {
    Projected a;
    Projected b;
    if (!projectVec({x1, y1, z1}, a) || !projectVec({x2, y2, z2}, b))
        return false;
    line2D(tft, sprite, a.x, a.y, b.x, b.y, color);
    return true;
}

bool OSA3DRenderer::drawPoint(TFT_eSPI* tft, TFT_eSprite* sprite,
                              float x, float y, float z, int radius,
                              uint16_t color) const {
    Projected projected;
    if (!projectVec({x, y, z}, projected)) return false;
    point2D(tft, sprite, projected.x, projected.y, radius, color);
    return true;
}

bool OSA3DRenderer::drawTriangle(TFT_eSPI* tft, TFT_eSprite* sprite,
                                 float x1, float y1, float z1,
                                 float x2, float y2, float z2,
                                 float x3, float y3, float z3,
                                 bool filled, uint16_t color) const {
    Projected a;
    Projected b;
    Projected c;
    if (!projectVec({x1, y1, z1}, a) ||
        !projectVec({x2, y2, z2}, b) ||
        !projectVec({x3, y3, z3}, c)) {
        return false;
    }
    triangle2D(tft, sprite, a, b, c, filled, color);
    return true;
}

uint16_t OSA3DRenderer::shade565(uint16_t color, float amount) {
    if (!isfinite(amount)) amount = 0.0f;
    amount = clampFloat(amount, 0.0f, 2.0f);
    int red = ((color >> 11) & 0x1F) * 255 / 31;
    int green = ((color >> 5) & 0x3F) * 255 / 63;
    int blue = (color & 0x1F) * 255 / 31;
    red = constrain((int)lroundf(red * amount), 0, 255);
    green = constrain((int)lroundf(green * amount), 0, 255);
    blue = constrain((int)lroundf(blue * amount), 0, 255);
    return ((uint16_t)(red & 0xF8) << 8) |
           ((uint16_t)(green & 0xFC) << 3) |
           ((uint16_t)blue >> 3);
}

int OSA3DRenderer::drawCube(TFT_eSPI* tft, TFT_eSprite* sprite,
                            float x, float y, float z, float size,
                            float rotationX, float rotationY, float rotationZ,
                            int mode, uint16_t baseColor,
                            uint16_t edgeColor) {
    const uint32_t started = micros();
    renderedFaces = 0;
    if (!tft || !isfinite(size) || size <= 0.0f) {
        renderMicros = micros() - started;
        return 0;
    }

    size = clampFloat(size, 0.001f, 2048.0f);
    mode = constrain(mode, 0, 2);
    const float half = size * 0.5f;
    const float sinX = sinf(rotationX);
    const float cosX = cosf(rotationX);
    const float sinY = sinf(rotationY);
    const float cosY = cosf(rotationY);
    const float sinZ = sinf(rotationZ);
    const float cosZ = cosf(rotationZ);

    static const int8_t signs[8][3] = {
        {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
        {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
    };

    Vec3 vertices[8];
    Projected projected[8];
    for (int i = 0; i < 8; ++i) {
        Vec3 local = {
            signs[i][0] * half,
            signs[i][1] * half,
            signs[i][2] * half,
        };
        Vec3 rotated = rotate(local, sinX, cosX, sinY, cosY, sinZ, cosZ);
        vertices[i] = {rotated.x + x, rotated.y + y, rotated.z + z};
        if (!projectVec(vertices[i], projected[i])) {
            renderMicros = micros() - started;
            return 0;
        }
    }

    if (mode == 0) {
        for (int i = 0; i < 12; ++i) {
            const Projected& a = projected[CUBE_EDGES[i][0]];
            const Projected& b = projected[CUBE_EDGES[i][1]];
            line2D(tft, sprite, a.x, a.y, b.x, b.y, edgeColor);
        }
        renderMicros = micros() - started;
        return 12;
    }

    int visible[6];
    float depth[6];
    Vec3 rotatedNormals[6];
    int visibleCount = 0;
    for (int face = 0; face < 6; ++face) {
        rotatedNormals[face] = rotate(CUBE_FACES[face].normal,
                                      sinX, cosX, sinY, cosY, sinZ, cosZ);
        if (rotatedNormals[face].z >= -0.015f) continue;
        float average = 0.0f;
        for (int corner = 0; corner < 4; ++corner)
            average += vertices[CUBE_FACES[face].vertex[corner]].z;
        visible[visibleCount] = face;
        depth[visibleCount] = average * 0.25f;
        ++visibleCount;
    }

    // Painter's algorithm. At most three cube faces are visible, so a tiny
    // insertion sort is cheaper than pulling in a general sorting routine.
    for (int i = 1; i < visibleCount; ++i) {
        int face = visible[i];
        float faceDepth = depth[i];
        int j = i - 1;
        while (j >= 0 && depth[j] < faceDepth) {
            visible[j + 1] = visible[j];
            depth[j + 1] = depth[j];
            --j;
        }
        visible[j + 1] = face;
        depth[j + 1] = faceDepth;
    }

    static const Vec3 light = {-0.365f, -0.548f, -0.752f};
    for (int order = 0; order < visibleCount; ++order) {
        const int face = visible[order];
        const CubeFace& definition = CUBE_FACES[face];
        const Vec3& normal = rotatedNormals[face];
        float diffuse = normal.x * light.x +
                        normal.y * light.y +
                        normal.z * light.z;
        if (diffuse < 0.0f) diffuse = 0.0f;
        const uint16_t faceColor = shade565(baseColor, 0.36f + diffuse * 0.72f);
        const Projected& a = projected[definition.vertex[0]];
        const Projected& b = projected[definition.vertex[1]];
        const Projected& c = projected[definition.vertex[2]];
        const Projected& d = projected[definition.vertex[3]];
        triangle2D(tft, sprite, a, b, c, true, faceColor);
        triangle2D(tft, sprite, a, c, d, true, faceColor);
        if (mode == 2) {
            line2D(tft, sprite, a.x, a.y, b.x, b.y, edgeColor);
            line2D(tft, sprite, b.x, b.y, c.x, c.y, edgeColor);
            line2D(tft, sprite, c.x, c.y, d.x, d.y, edgeColor);
            line2D(tft, sprite, d.x, d.y, a.x, a.y, edgeColor);
        }
        ++renderedFaces;
    }

    renderMicros = micros() - started;
    return renderedFaces;
}

int OSA3DRenderer::drawGrid(TFT_eSPI* tft, TFT_eSprite* sprite,
                            float y, float halfSize, float step,
                            uint16_t color) const {
    if (!tft || !isfinite(halfSize) || !isfinite(step) ||
        halfSize <= 0.0f || step <= 0.0f) {
        return 0;
    }
    halfSize = clampFloat(halfSize, 0.01f, 2048.0f);
    step = clampFloat(step, halfSize / 16.0f, halfSize * 2.0f);
    int halfLines = min(16, (int)floorf(halfSize / step));
    int drawn = 0;
    for (int i = -halfLines; i <= halfLines; ++i) {
        const float position = i * step;
        if (drawLine(tft, sprite, -halfSize, y, position,
                     halfSize, y, position, color)) ++drawn;
        if (drawLine(tft, sprite, position, y, -halfSize,
                     position, y, halfSize, color)) ++drawn;
    }
    return drawn;
}

void OSA3DRenderer::drawAxes(TFT_eSPI* tft, TFT_eSprite* sprite,
                             float size) const {
    size = clampFloat(size, 0.01f, 2048.0f);
    drawLine(tft, sprite, 0, 0, 0, size, 0, 0, TFT_RED);
    drawLine(tft, sprite, 0, 0, 0, 0, size, 0, TFT_GREEN);
    drawLine(tft, sprite, 0, 0, 0, 0, 0, size, TFT_BLUE);
}
