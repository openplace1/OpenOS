#include "OSA3D.h"

#include "HeapReserve.h"

#include <math.h>
#include <stdlib.h>

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

static const int8_t BOX_SIGNS[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};

static float clampFloat(float value, float minimum, float maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static inline OSA3DRenderer::Vec3 sub(const OSA3DRenderer::Vec3& a,
                                      const OSA3DRenderer::Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline OSA3DRenderer::Vec3 cross(const OSA3DRenderer::Vec3& a,
                                        const OSA3DRenderer::Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static inline float dot(const OSA3DRenderer::Vec3& a, const OSA3DRenderer::Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

// ─── Camera and transforms ───────────────────────────────────────────────────

void OSA3DRenderer::reset() {
    cameraCenterX = 120.0f;
    cameraCenterY = 160.0f;
    focalLength = 110.0f;
    cameraDistance = 5.0f;
    renderMicros = 0;
    renderedFaces = 0;
    view = basisOf(0.0f, 0.0f, 0.0f);
    model = view;
    modelPos = {0.0f, 0.0f, 0.0f};
    modelScale = 1.0f;
    light = {-0.365f, -0.548f, -0.752f};
    ambient = 0.36f;
    background = 0x0000;
    viewX = 0; viewY = 0; viewW = 240; viewH = 320;
    releaseScene();
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

OSA3DRenderer::Basis OSA3DRenderer::basisOf(float rx, float ry, float rz) {
    if (!isfinite(rx)) rx = 0.0f;
    if (!isfinite(ry)) ry = 0.0f;
    if (!isfinite(rz)) rz = 0.0f;
    return {sinf(rx), cosf(rx), sinf(ry), cosf(ry), sinf(rz), cosf(rz)};
}

void OSA3DRenderer::setView(float rx, float ry, float rz) {
    view = basisOf(rx, ry, rz);
}

void OSA3DRenderer::setLight(float x, float y, float z, float ambientValue) {
    float length = sqrtf(x * x + y * y + z * z);
    if (isfinite(length) && length > 0.0001f)
        light = {x / length, y / length, z / length};
    if (isfinite(ambientValue)) ambient = clampFloat(ambientValue, 0.0f, 1.0f);
}

void OSA3DRenderer::setViewport(int x, int y, int w, int h) {
    viewX = constrain(x, 0, 239);
    viewY = constrain(y, 0, 319);
    viewW = constrain(w, 1, 240 - viewX);
    viewH = constrain(h, 1, 320 - viewY);
}

void OSA3DRenderer::setTransform(float x, float y, float z, float rx, float ry,
                                 float rz, float scale) {
    modelPos = {isfinite(x) ? x : 0.0f, isfinite(y) ? y : 0.0f, isfinite(z) ? z : 0.0f};
    model = basisOf(rx, ry, rz);
    modelScale = isfinite(scale) ? clampFloat(scale, 0.0001f, 4096.0f) : 1.0f;
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

OSA3DRenderer::Vec3 OSA3DRenderer::toWorld(const Vec3& local) const {
    Vec3 scaled = {local.x * modelScale, local.y * modelScale, local.z * modelScale};
    Vec3 rotated = rotate(scaled, model);
    Vec3 placed = {rotated.x + modelPos.x, rotated.y + modelPos.y, rotated.z + modelPos.z};
    return rotate(placed, view);
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
    if (!projectVec(rotate({x, y, z}, view), projected)) return false;
    screenX = projected.x;
    screenY = projected.y;
    return true;
}

// ─── 2D helpers ──────────────────────────────────────────────────────────────

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

uint16_t OSA3DRenderer::litColor(const Vec3& normal, uint16_t base) const {
    float length = sqrtf(dot(normal, normal));
    if (!(length > 0.0f)) return shade565(base, ambient);
    float diffuse = dot(normal, light) / length;
    if (diffuse < 0.0f) diffuse = 0.0f;
    return shade565(base, ambient + diffuse * (1.0f - ambient) * 1.15f);
}

// ─── Scene list ──────────────────────────────────────────────────────────────

bool OSA3DRenderer::beginScene() {
    if (!entries) {
        // The reserve first: a 3D app usually holds its sprite in the general
        // heap, and the reserve is otherwise idle while it runs.
        static const int reserveSizes[] = {768, 512, 384, 256};
        for (int size : reserveSizes) {
            entries = (Entry*)HeapReserve::allocate((size_t)size * sizeof(Entry), "3D scene");
            if (entries) { capacity = size; entriesInReserve = true; break; }
        }
        if (!entries) {
            static const int heapSizes[] = {512, 384, 256, 128};
            for (int size : heapSizes) {
                entries = (Entry*)malloc((size_t)size * sizeof(Entry));
                if (entries) { capacity = size; entriesInReserve = false; break; }
            }
        }
        if (!entries) { capacity = 0; return false; }
        Serial.printf("[3D] scene list %d entries (%u B) %s\n", capacity,
                      (unsigned)((size_t)capacity * sizeof(Entry)),
                      entriesInReserve ? "in reserve" : "on heap");
    }
    count = 0;
    dropped = 0;
    collecting = true;
    return true;
}

void OSA3DRenderer::releaseScene() {
    if (entries) {
        if (entriesInReserve) HeapReserve::deallocate(entries);
        else                  free(entries);
    }
    entries = nullptr;
    capacity = count = dropped = 0;
    collecting = false;
    if (meshVertex_) {
        if (meshInReserve) HeapReserve::deallocate(meshVertex_);
        else               free(meshVertex_);
    }
    meshVertex_ = nullptr;
    meshFace_ = nullptr;
    meshVerts = meshFaces = 0;
}

void OSA3DRenderer::sortEntries() {
    // Far to near. Shell sort: in place, no recursion, fine for a few hundred
    // 24-byte records.
    for (int gap = count / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < count; ++i) {
            Entry temp = entries[i];
            int j = i;
            while (j >= gap && entries[j - gap].depth < temp.depth) {
                entries[j] = entries[j - gap];
                j -= gap;
            }
            entries[j] = temp;
        }
    }
}

void OSA3DRenderer::drawEntry(TFT_eSPI* tft, TFT_eSprite* sprite,
                              const Entry& e, int offsetX, int offsetY) const {
    if (e.flags & ENTRY_POINT) {
        point2D(tft, sprite, e.x[0] - offsetX, e.y[0] - offsetY, e.radius, e.color);
        return;
    }
    if (e.flags & ENTRY_LINE) {
        line2D(tft, sprite, e.x[0] - offsetX, e.y[0] - offsetY,
               e.x[1] - offsetX, e.y[1] - offsetY, e.color);
        return;
    }
    Projected a = {(int16_t)(e.x[0] - offsetX), (int16_t)(e.y[0] - offsetY), 0.0f};
    Projected b = {(int16_t)(e.x[1] - offsetX), (int16_t)(e.y[1] - offsetY), 0.0f};
    Projected c = {(int16_t)(e.x[2] - offsetX), (int16_t)(e.y[2] - offsetY), 0.0f};
    if (e.flags & ENTRY_FILL) triangle2D(tft, sprite, a, b, c, true, e.color);
    if (e.flags & ENTRY_EDGES) {
        if (e.radius & 1) line2D(tft, sprite, a.x, a.y, b.x, b.y, e.edge);
        if (e.radius & 2) line2D(tft, sprite, b.x, b.y, c.x, c.y, e.edge);
        if (e.radius & 4) line2D(tft, sprite, c.x, c.y, a.x, a.y, e.edge);
    }
}

int OSA3DRenderer::endScene(TFT_eSPI* tft, TFT_eSprite* sprite) {
    const uint32_t started = micros();
    collecting = false;
    if (!entries || !tft) { renderMicros = micros() - started; return 0; }
    sortEntries();
    for (int i = 0; i < count; ++i) drawEntry(tft, sprite, entries[i], 0, 0);
    renderedFaces = count;
    renderMicros = micros() - started;
    return count;
}

int OSA3DRenderer::presentScene(TFT_eSPI* tft, TFT_eSprite* sprite) {
    const uint32_t started = micros();
    collecting = false;
    if (!entries || !tft) { renderMicros = micros() - started; return 0; }
    sortEntries();
    if (!sprite) {
        // No band buffer: draw straight to the panel.
        for (int i = 0; i < count; ++i) drawEntry(tft, nullptr, entries[i], 0, 0);
        renderedFaces = count;
        renderMicros = micros() - started;
        return count;
    }
    const int bandH = sprite->height();
    if (bandH <= 0) { renderMicros = micros() - started; return 0; }
    // Clip pushes to the viewport so the last band never spills below it.
    tft->setViewport(viewX, viewY, viewW, viewH, false);
    for (int bandY = viewY; bandY < viewY + viewH; bandY += bandH) {
        const int bandEnd = bandY + bandH;
        sprite->fillSprite(background);
        for (int i = 0; i < count; ++i) {
            const Entry& e = entries[i];
            int minY, maxY;
            if (e.flags & ENTRY_POINT) {
                minY = e.y[0] - e.radius; maxY = e.y[0] + e.radius;
            } else if (e.flags & ENTRY_LINE) {
                minY = min(e.y[0], e.y[1]); maxY = max(e.y[0], e.y[1]);
            } else {
                minY = min(e.y[0], min(e.y[1], e.y[2]));
                maxY = max(e.y[0], max(e.y[1], e.y[2]));
            }
            if (maxY < bandY || minY >= bandEnd) continue;
            // Entries are in screen coordinates; the band sprite lands at
            // (viewX, bandY), so that is what comes off.
            drawEntry(tft, sprite, e, viewX, bandY);
        }
        sprite->pushSprite(viewX, bandY);
    }
    tft->resetViewport();
    renderedFaces = count;
    renderMicros = micros() - started;
    return count;
}

int OSA3DRenderer::emitLine(TFT_eSPI* tft, TFT_eSprite* sprite,
                            const Projected& a, const Projected& b,
                            float depth, uint16_t color) {
    if (!collecting) {
        line2D(tft, sprite, a.x, a.y, b.x, b.y, color);
        return 1;
    }
    if (count >= capacity) { ++dropped; return 0; }
    Entry& e = entries[count++];
    e.x[0] = a.x; e.y[0] = a.y; e.x[1] = b.x; e.y[1] = b.y; e.x[2] = 0; e.y[2] = 0;
    e.depth = depth; e.color = color; e.edge = color; e.flags = ENTRY_LINE; e.radius = 0;
    return 1;
}

int OSA3DRenderer::emitPoint(TFT_eSPI* tft, TFT_eSprite* sprite,
                             const Projected& p, float depth, int radius,
                             uint16_t color) {
    radius = constrain(radius, 1, 32);
    if (!collecting) {
        point2D(tft, sprite, p.x, p.y, radius, color);
        return 1;
    }
    if (count >= capacity) { ++dropped; return 0; }
    Entry& e = entries[count++];
    e.x[0] = p.x; e.y[0] = p.y; e.x[1] = e.x[2] = 0; e.y[1] = e.y[2] = 0;
    e.depth = depth; e.color = color; e.edge = color; e.flags = ENTRY_POINT;
    e.radius = (uint8_t)radius;
    return 1;
}

// a, b, c are already in view space. Faces turned away from the camera are
// skipped unless the mode is two-sided; lighting comes from the face normal,
// flipped to face the camera for two-sided faces.
int OSA3DRenderer::emitFace(TFT_eSPI* tft, TFT_eSprite* sprite, const Vec3& a,
                            const Vec3& b, const Vec3& c, int mode,
                            uint16_t color, uint16_t edge) {
    Projected pa, pb, pc;
    if (!projectVec(a, pa) || !projectVec(b, pb) || !projectVec(c, pc)) return 0;
    Vec3 normal = cross(sub(b, a), sub(c, a));
    // Camera position in view space is (0, 0, -distance).
    Vec3 toFace = {a.x, a.y, a.z + cameraDistance};
    float facing = dot(normal, toFace);
    if (facing >= 0.0f) {
        if (!(mode & MODE_TWO_SIDED)) return 0;
        normal = {-normal.x, -normal.y, -normal.z};
    }
    const int style = mode & 3;
    const bool filled = style != MODE_WIRE;
    uint16_t fill = filled ? litColor(normal, color) : color;
    uint8_t flags = 0;
    if (filled) flags |= ENTRY_FILL;
    if (style == MODE_WIRE || style == MODE_EDGES) flags |= ENTRY_EDGES;
    uint16_t edgeColor = style == MODE_WIRE ? color : edge;
    if (!collecting) {
        if (flags & ENTRY_FILL) triangle2D(tft, sprite, pa, pb, pc, true, fill);
        if (flags & ENTRY_EDGES) triangle2D(tft, sprite, pa, pb, pc, false, edgeColor);
        ++renderedFaces;
        return 1;
    }
    if (count >= capacity) { ++dropped; return 0; }
    Entry& e = entries[count++];
    e.x[0] = pa.x; e.y[0] = pa.y; e.x[1] = pb.x; e.y[1] = pb.y; e.x[2] = pc.x; e.y[2] = pc.y;
    e.depth = (a.z + b.z + c.z) * (1.0f / 3.0f);
    e.color = fill; e.edge = edgeColor; e.flags = flags; e.radius = 7;
    return 1;
}

int OSA3DRenderer::emitQuad(TFT_eSPI* tft, TFT_eSprite* sprite, const Vec3& a,
                            const Vec3& b, const Vec3& c, const Vec3& d, int mode,
                            uint16_t color, uint16_t edge) {
    // Two triangles sharing the quad's normal, so a flat quad shades evenly,
    // with edge masks that leave the shared diagonal out of the outline.
    Projected pa, pb, pc, pd;
    if (!projectVec(a, pa) || !projectVec(b, pb) || !projectVec(c, pc) || !projectVec(d, pd))
        return 0;
    Vec3 normal = cross(sub(b, a), sub(c, a));
    Vec3 toFace = {a.x, a.y, a.z + cameraDistance};
    if (dot(normal, toFace) >= 0.0f) {
        if (!(mode & MODE_TWO_SIDED)) return 0;
        normal = {-normal.x, -normal.y, -normal.z};
    }
    const int style = mode & 3;
    const bool filled = style != MODE_WIRE;
    const uint16_t fill = filled ? litColor(normal, color) : color;
    const uint16_t edgeColor = style == MODE_WIRE ? color : edge;
    uint8_t flags = 0;
    if (filled) flags |= ENTRY_FILL;
    if (style == MODE_WIRE || style == MODE_EDGES) flags |= ENTRY_EDGES;
    const float depth = (a.z + b.z + c.z + d.z) * 0.25f;
    if (!collecting) {
        if (filled) {
            triangle2D(tft, sprite, pa, pb, pc, true, fill);
            triangle2D(tft, sprite, pa, pc, pd, true, fill);
        }
        if (flags & ENTRY_EDGES) {
            line2D(tft, sprite, pa.x, pa.y, pb.x, pb.y, edgeColor);
            line2D(tft, sprite, pb.x, pb.y, pc.x, pc.y, edgeColor);
            line2D(tft, sprite, pc.x, pc.y, pd.x, pd.y, edgeColor);
            line2D(tft, sprite, pd.x, pd.y, pa.x, pa.y, edgeColor);
        }
        renderedFaces += 2;
        return 2;
    }
    if (count + 2 > capacity) { dropped += 2; return 0; }
    Entry& e1 = entries[count++];
    e1.x[0] = pa.x; e1.y[0] = pa.y; e1.x[1] = pb.x; e1.y[1] = pb.y; e1.x[2] = pc.x; e1.y[2] = pc.y;
    e1.depth = depth; e1.color = fill; e1.edge = edgeColor; e1.flags = flags; e1.radius = 1 | 2;
    Entry& e2 = entries[count++];
    e2.x[0] = pa.x; e2.y[0] = pa.y; e2.x[1] = pc.x; e2.y[1] = pc.y; e2.x[2] = pd.x; e2.y[2] = pd.y;
    e2.depth = depth; e2.color = fill; e2.edge = edgeColor; e2.flags = flags; e2.radius = 2 | 4;
    return 2;
}

// ─── Immediate primitives (world space, view rotation applied) ──────────────

bool OSA3DRenderer::drawLine(TFT_eSPI* tft, TFT_eSprite* sprite,
                             float x1, float y1, float z1,
                             float x2, float y2, float z2,
                             uint16_t color) {
    Vec3 a = rotate({x1, y1, z1}, view);
    Vec3 b = rotate({x2, y2, z2}, view);
    Projected pa, pb;
    if (!projectVec(a, pa) || !projectVec(b, pb)) return false;
    emitLine(tft, sprite, pa, pb, (a.z + b.z) * 0.5f, color);
    return true;
}

bool OSA3DRenderer::drawPoint(TFT_eSPI* tft, TFT_eSprite* sprite,
                              float x, float y, float z, int radius,
                              uint16_t color) {
    Vec3 p = rotate({x, y, z}, view);
    Projected projected;
    if (!projectVec(p, projected)) return false;
    emitPoint(tft, sprite, projected, p.z, radius, color);
    return true;
}

bool OSA3DRenderer::drawTriangle(TFT_eSPI* tft, TFT_eSprite* sprite,
                                 float x1, float y1, float z1,
                                 float x2, float y2, float z2,
                                 float x3, float y3, float z3,
                                 bool filled, uint16_t color) {
    Vec3 a = rotate({x1, y1, z1}, view);
    Vec3 b = rotate({x2, y2, z2}, view);
    Vec3 c = rotate({x3, y3, z3}, view);
    // A free triangle is two-sided and unlit, as it always was.
    Projected pa, pb, pc;
    if (!projectVec(a, pa) || !projectVec(b, pb) || !projectVec(c, pc)) return false;
    if (!collecting) {
        triangle2D(tft, sprite, pa, pb, pc, filled, color);
        return true;
    }
    if (count >= capacity) { ++dropped; return false; }
    Entry& e = entries[count++];
    e.x[0] = pa.x; e.y[0] = pa.y; e.x[1] = pb.x; e.y[1] = pb.y; e.x[2] = pc.x; e.y[2] = pc.y;
    e.depth = (a.z + b.z + c.z) * (1.0f / 3.0f);
    e.color = color; e.edge = color; e.flags = filled ? ENTRY_FILL : ENTRY_EDGES; e.radius = 7;
    return true;
}

int OSA3DRenderer::drawCube(TFT_eSPI* tft, TFT_eSprite* sprite,
                            float x, float y, float z, float size,
                            float rotationX, float rotationY, float rotationZ,
                            int mode, uint16_t baseColor,
                            uint16_t edgeColor) {
    const uint32_t started = micros();
    if (!collecting) renderedFaces = 0;
    if (!tft || !isfinite(size) || size <= 0.0f) {
        renderMicros = micros() - started;
        return 0;
    }
    size = clampFloat(size, 0.001f, 2048.0f);
    mode = constrain(mode, 0, 2);
    const float half = size * 0.5f;
    Basis own = basisOf(rotationX, rotationY, rotationZ);

    Vec3 vertices[8];
    for (int i = 0; i < 8; ++i) {
        Vec3 local = {BOX_SIGNS[i][0] * half, BOX_SIGNS[i][1] * half, BOX_SIGNS[i][2] * half};
        Vec3 rotated = rotate(local, own);
        vertices[i] = rotate({rotated.x + x, rotated.y + y, rotated.z + z}, view);
    }

    if (mode == 0) {
        int drawn = 0;
        for (int i = 0; i < 12; ++i) {
            const Vec3& a = vertices[CUBE_EDGES[i][0]];
            const Vec3& b = vertices[CUBE_EDGES[i][1]];
            Projected pa, pb;
            if (!projectVec(a, pa) || !projectVec(b, pb)) continue;
            drawn += emitLine(tft, sprite, pa, pb, (a.z + b.z) * 0.5f, edgeColor);
        }
        renderMicros = micros() - started;
        return drawn;
    }

    int drawn = 0;
    for (int face = 0; face < 6; ++face) {
        const CubeFace& definition = CUBE_FACES[face];
        drawn += emitQuad(tft, sprite,
                          vertices[definition.vertex[0]], vertices[definition.vertex[1]],
                          vertices[definition.vertex[2]], vertices[definition.vertex[3]],
                          mode, baseColor, edgeColor);
    }
    renderMicros = micros() - started;
    return drawn;
}

int OSA3DRenderer::drawGrid(TFT_eSPI* tft, TFT_eSprite* sprite,
                            float y, float halfSize, float step,
                            uint16_t color) {
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
                             float size) {
    size = clampFloat(size, 0.01f, 2048.0f);
    drawLine(tft, sprite, 0, 0, 0, size, 0, 0, TFT_RED);
    drawLine(tft, sprite, 0, 0, 0, 0, size, 0, TFT_GREEN);
    drawLine(tft, sprite, 0, 0, 0, 0, 0, size, TFT_BLUE);
}

// ─── Shapes under the model transform ───────────────────────────────────────
// Every shape is generated with outward-facing winding in its local frame.
// The engine's screen has +Y up and +Z away from the viewer, so "outward"
// means the vertices of a face run clockwise when looked at from outside —
// the order the built-in cube's faces have always used; cross(b-a, c-a) then
// points out of the solid and the camera test in emitFace() culls the back.

int OSA3DRenderer::drawBox(TFT_eSPI* tft, TFT_eSprite* sprite, float sx, float sy,
                           float sz, int mode, uint16_t color, uint16_t edge) {
    const uint32_t started = micros();
    if (!collecting) renderedFaces = 0;
    sx = clampFloat(isfinite(sx) ? sx : 1.0f, 0.001f, 2048.0f) * 0.5f;
    sy = clampFloat(isfinite(sy) ? sy : 1.0f, 0.001f, 2048.0f) * 0.5f;
    sz = clampFloat(isfinite(sz) ? sz : 1.0f, 0.001f, 2048.0f) * 0.5f;
    Vec3 v[8];
    for (int i = 0; i < 8; ++i)
        v[i] = toWorld({BOX_SIGNS[i][0] * sx, BOX_SIGNS[i][1] * sy, BOX_SIGNS[i][2] * sz});
    int drawn = 0;
    for (int face = 0; face < 6; ++face) {
        const CubeFace& d = CUBE_FACES[face];
        drawn += emitQuad(tft, sprite, v[d.vertex[0]], v[d.vertex[1]],
                          v[d.vertex[2]], v[d.vertex[3]], mode, color, edge);
    }
    renderMicros = micros() - started;
    return drawn;
}

int OSA3DRenderer::drawPlane(TFT_eSPI* tft, TFT_eSprite* sprite, float width,
                             float depth, int mode, uint16_t color, uint16_t edge) {
    const uint32_t started = micros();
    if (!collecting) renderedFaces = 0;
    float hw = clampFloat(isfinite(width) ? width : 1.0f, 0.001f, 4096.0f) * 0.5f;
    float hd = clampFloat(isfinite(depth) ? depth : 1.0f, 0.001f, 4096.0f) * 0.5f;
    // Seen from above (+Y) the corners run counter-clockwise.
    Vec3 a = toWorld({-hw, 0.0f, -hd});
    Vec3 b = toWorld({-hw, 0.0f,  hd});
    Vec3 c = toWorld({ hw, 0.0f,  hd});
    Vec3 d = toWorld({ hw, 0.0f, -hd});
    int drawn = emitQuad(tft, sprite, a, b, c, d, mode | MODE_TWO_SIDED, color, edge);
    renderMicros = micros() - started;
    return drawn;
}

int OSA3DRenderer::drawSphere(TFT_eSPI* tft, TFT_eSprite* sprite, float radius,
                              int segments, int mode, uint16_t color, uint16_t edge) {
    const uint32_t started = micros();
    if (!collecting) renderedFaces = 0;
    radius = clampFloat(isfinite(radius) ? radius : 1.0f, 0.001f, 2048.0f);
    segments = constrain(segments, 4, 24);
    const int rings = max(3, segments / 2);
    // One ring of vertices at a time: the previous ring and the current one
    // are all a strip of quads needs.
    Vec3 previous[25];
    Vec3 current[25];
    int drawn = 0;
    for (int i = 0; i <= rings; ++i) {
        const float theta = (float)M_PI * (float)i / (float)rings;
        const float y = radius * cosf(theta);
        const float rr = radius * sinf(theta);
        for (int j = 0; j < segments; ++j) {
            const float phi = 2.0f * (float)M_PI * (float)j / (float)segments;
            current[j] = toWorld({rr * cosf(phi), y, rr * sinf(phi)});
        }
        if (i > 0) {
            for (int j = 0; j < segments; ++j) {
                const int k = (j + 1) % segments;
                if (i == 1) {
                    drawn += emitFace(tft, sprite, previous[0], current[k], current[j], mode, color, edge);
                } else if (i == rings) {
                    drawn += emitFace(tft, sprite, previous[j], previous[k], current[0], mode, color, edge);
                } else {
                    drawn += emitQuad(tft, sprite, previous[j], previous[k], current[k], current[j],
                                      mode, color, edge);
                }
            }
        }
        for (int j = 0; j < segments; ++j) previous[j] = current[j];
    }
    renderMicros = micros() - started;
    return drawn;
}

int OSA3DRenderer::drawCylinder(TFT_eSPI* tft, TFT_eSprite* sprite,
                                float radiusBottom, float radiusTop, float height,
                                int sides, int mode, uint16_t color, uint16_t edge) {
    const uint32_t started = micros();
    if (!collecting) renderedFaces = 0;
    radiusBottom = clampFloat(isfinite(radiusBottom) ? radiusBottom : 1.0f, 0.0f, 2048.0f);
    radiusTop = clampFloat(isfinite(radiusTop) ? radiusTop : radiusBottom, 0.0f, 2048.0f);
    height = clampFloat(isfinite(height) ? height : 1.0f, 0.001f, 2048.0f);
    sides = constrain(sides, 3, 24);
    const float hh = height * 0.5f;
    Vec3 bottom[25], top[25];
    for (int j = 0; j < sides; ++j) {
        const float phi = 2.0f * (float)M_PI * (float)j / (float)sides;
        bottom[j] = toWorld({radiusBottom * cosf(phi), -hh, radiusBottom * sinf(phi)});
        top[j] = toWorld({radiusTop * cosf(phi), hh, radiusTop * sinf(phi)});
    }
    Vec3 bottomCentre = toWorld({0.0f, -hh, 0.0f});
    Vec3 topCentre = toWorld({0.0f, hh, 0.0f});
    int drawn = 0;
    for (int j = 0; j < sides; ++j) {
        const int k = (j + 1) % sides;
        // Sides, then caps; see the winding note above.
        if (radiusTop <= 0.0f) {
            drawn += emitFace(tft, sprite, bottom[k], bottom[j], topCentre, mode, color, edge);
        } else if (radiusBottom <= 0.0f) {
            drawn += emitFace(tft, sprite, top[j], top[k], bottomCentre, mode, color, edge);
        } else {
            drawn += emitQuad(tft, sprite, bottom[k], bottom[j], top[j], top[k], mode, color, edge);
        }
        // Caps.
        if (radiusBottom > 0.0f)
            drawn += emitFace(tft, sprite, bottom[j], bottom[k], bottomCentre, mode, color, edge);
        if (radiusTop > 0.0f)
            drawn += emitFace(tft, sprite, top[k], top[j], topCentre, mode, color, edge);
    }
    renderMicros = micros() - started;
    return drawn;
}

int OSA3DRenderer::drawTorus(TFT_eSPI* tft, TFT_eSprite* sprite, float ringRadius,
                             float tubeRadius, int segments, int rings,
                             int mode, uint16_t color, uint16_t edge) {
    const uint32_t started = micros();
    if (!collecting) renderedFaces = 0;
    ringRadius = clampFloat(isfinite(ringRadius) ? ringRadius : 1.0f, 0.001f, 2048.0f);
    tubeRadius = clampFloat(isfinite(tubeRadius) ? tubeRadius : 0.3f, 0.001f, 2048.0f);
    segments = constrain(segments, 3, 24);
    rings = constrain(rings, 3, 16);
    Vec3 previous[17];
    Vec3 current[17];
    int drawn = 0;
    for (int i = 0; i <= segments; ++i) {
        const int ii = i % segments;
        const float alpha = 2.0f * (float)M_PI * (float)ii / (float)segments;
        const float ca = cosf(alpha), sa = sinf(alpha);
        for (int j = 0; j < rings; ++j) {
            const float beta = 2.0f * (float)M_PI * (float)j / (float)rings;
            const float rr = ringRadius + tubeRadius * cosf(beta);
            current[j] = toWorld({rr * ca, tubeRadius * sinf(beta), rr * sa});
        }
        if (i > 0) {
            for (int j = 0; j < rings; ++j) {
                const int k = (j + 1) % rings;
                drawn += emitQuad(tft, sprite, previous[j], previous[k], current[k], current[j],
                                  mode, color, edge);
            }
        }
        for (int j = 0; j < rings; ++j) previous[j] = current[j];
    }
    renderMicros = micros() - started;
    return drawn;
}

// ─── Custom mesh ─────────────────────────────────────────────────────────────

bool OSA3DRenderer::meshBegin() {
    if (!meshVertex_) {
        size_t bytes = (size_t)MESH_MAX_VERTICES * sizeof(Vec3) +
                       (size_t)MESH_MAX_FACES * sizeof(MeshFace);
        void* block = HeapReserve::allocate(bytes, "3D mesh");
        meshInReserve = block != nullptr;
        if (!block) block = malloc(bytes);
        if (!block) return false;
        meshVertex_ = (Vec3*)block;
        meshFace_ = (MeshFace*)((uint8_t*)block + (size_t)MESH_MAX_VERTICES * sizeof(Vec3));
    }
    meshVerts = 0;
    meshFaces = 0;
    return true;
}

int OSA3DRenderer::meshVertex(float x, float y, float z) {
    if (!meshVertex_ || meshVerts >= MESH_MAX_VERTICES) return -1;
    meshVertex_[meshVerts] = {isfinite(x) ? x : 0.0f, isfinite(y) ? y : 0.0f, isfinite(z) ? z : 0.0f};
    return meshVerts++;
}

bool OSA3DRenderer::meshFace(int a, int b, int c, int d) {
    if (!meshVertex_ || meshFaces >= MESH_MAX_FACES) return false;
    if (a < 0 || b < 0 || c < 0 || a >= meshVerts || b >= meshVerts || c >= meshVerts) return false;
    if (d >= meshVerts) return false;
    MeshFace& face = meshFace_[meshFaces++];
    face.v[0] = (uint16_t)a; face.v[1] = (uint16_t)b; face.v[2] = (uint16_t)c;
    face.v[3] = d < 0 ? 0xFFFF : (uint16_t)d;
    return true;
}

int OSA3DRenderer::drawMesh(TFT_eSPI* tft, TFT_eSprite* sprite, int mode,
                            uint16_t color, uint16_t edge) {
    const uint32_t started = micros();
    if (!collecting) renderedFaces = 0;
    if (!meshVertex_) return 0;
    int drawn = 0;
    for (int i = 0; i < meshFaces; ++i) {
        const MeshFace& face = meshFace_[i];
        Vec3 a = toWorld(meshVertex_[face.v[0]]);
        Vec3 b = toWorld(meshVertex_[face.v[1]]);
        Vec3 c = toWorld(meshVertex_[face.v[2]]);
        if (face.v[3] == 0xFFFF) {
            drawn += emitFace(tft, sprite, a, b, c, mode, color, edge);
        } else {
            Vec3 d = toWorld(meshVertex_[face.v[3]]);
            drawn += emitQuad(tft, sprite, a, b, c, d, mode, color, edge);
        }
    }
    renderMicros = micros() - started;
    return drawn;
}
