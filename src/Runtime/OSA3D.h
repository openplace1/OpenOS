#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Pseudo-3D renderer for OSA.
//
// Two ways to use it. Immediate calls (drawLine, drawCube, ...) transform and
// draw inside one native call, as before. A *scene* collects every primitive
// of a frame into one depth-sorted list first (beginScene ... endScene), so
// several shapes occlude each other correctly, and presentScene() can render
// that list through a sprite that is much smaller than the viewport, band by
// band — full 240x320 at 16-bit colour from a 23 KB buffer. Nothing here
// keeps a vertex heap for the built-in shapes: a sphere, cylinder, torus or
// box is generated and transformed on the fly into the list.
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

    // Render mode bits for shapes: 0 wireframe, 1 solid, 2 solid + edges,
    // +4 two-sided (no back-face culling, for planes and open meshes).
    static constexpr int MODE_WIRE = 0;
    static constexpr int MODE_SOLID = 1;
    static constexpr int MODE_EDGES = 2;
    static constexpr int MODE_TWO_SIDED = 4;

    void reset();
    void setCamera(float centerX, float centerY, float focalLength,
                   float cameraDistance);

    bool project(float x, float y, float z, int16_t& screenX,
                 int16_t& screenY) const;
    bool drawLine(TFT_eSPI* tft, TFT_eSprite* sprite,
                  float x1, float y1, float z1,
                  float x2, float y2, float z2,
                  uint16_t color);
    bool drawPoint(TFT_eSPI* tft, TFT_eSprite* sprite,
                   float x, float y, float z, int radius,
                   uint16_t color);
    bool drawTriangle(TFT_eSPI* tft, TFT_eSprite* sprite,
                      float x1, float y1, float z1,
                      float x2, float y2, float z2,
                      float x3, float y3, float z3,
                      bool filled, uint16_t color);
    int drawCube(TFT_eSPI* tft, TFT_eSprite* sprite,
                 float x, float y, float z, float size,
                 float rotationX, float rotationY, float rotationZ,
                 int mode, uint16_t baseColor, uint16_t edgeColor);
    int drawGrid(TFT_eSPI* tft, TFT_eSprite* sprite,
                 float y, float halfSize, float step,
                 uint16_t color);
    void drawAxes(TFT_eSPI* tft, TFT_eSprite* sprite, float size);

    // ── Scene ───────────────────────────────────────────────────────────
    // beginScene() allocates the list on first use (from the heap reserve
    // when it is free, else the general heap) and clears it. Between begin
    // and end every draw call above and every shape below is collected
    // instead of drawn. endScene() sorts far-to-near and draws into the
    // active target; presentScene() renders through `sprite` in horizontal
    // bands and pushes each band at the viewport position.
    bool beginScene();
    bool sceneActive() const { return collecting; }
    int  endScene(TFT_eSPI* tft, TFT_eSprite* sprite);
    int  presentScene(TFT_eSPI* tft, TFT_eSprite* sprite);
    void releaseScene();
    int  sceneCapacity() const { return capacity; }
    int  sceneCount() const { return count; }
    int  droppedCount() const { return dropped; }

    // Global orbit rotation applied after each shape's own transform.
    void setView(float rx, float ry, float rz);
    void setLight(float x, float y, float z, float ambient);
    void setBackground(uint16_t color) { background = color; }
    void setViewport(int x, int y, int w, int h);

    // Model transform for the shapes below: position, rotation, uniform scale.
    void setTransform(float x, float y, float z, float rx, float ry, float rz,
                      float scale);

    int drawBox(TFT_eSPI* tft, TFT_eSprite* sprite, float sx, float sy, float sz,
                int mode, uint16_t color, uint16_t edge);
    int drawSphere(TFT_eSPI* tft, TFT_eSprite* sprite, float radius, int segments,
                   int mode, uint16_t color, uint16_t edge);
    int drawCylinder(TFT_eSPI* tft, TFT_eSprite* sprite, float radiusBottom,
                     float radiusTop, float height, int sides,
                     int mode, uint16_t color, uint16_t edge);
    int drawTorus(TFT_eSPI* tft, TFT_eSprite* sprite, float ringRadius,
                  float tubeRadius, int segments, int rings,
                  int mode, uint16_t color, uint16_t edge);
    int drawPlane(TFT_eSPI* tft, TFT_eSprite* sprite, float width, float depth,
                  int mode, uint16_t color, uint16_t edge);

    // Custom mesh: up to MESH_MAX_VERTICES / MESH_MAX_FACES, faces are
    // triangles or quads listed clockwise as seen from outside (the order the
    // built-in cube uses; +Y is up and +Z points away from the viewer).
    bool meshBegin();
    int  meshVertex(float x, float y, float z);
    bool meshFace(int a, int b, int c, int d);
    void meshEnd() {}
    int  meshVertexCount() const { return meshVerts; }
    int  meshFaceCount() const { return meshFaces; }
    int  drawMesh(TFT_eSPI* tft, TFT_eSprite* sprite, int mode, uint16_t color,
                  uint16_t edge);

    float centerX() const { return cameraCenterX; }
    float centerY() const { return cameraCenterY; }
    float focal() const { return focalLength; }
    float distance() const { return cameraDistance; }
    uint32_t lastRenderMicros() const { return renderMicros; }
    int lastFaceCount() const { return renderedFaces; }

    static uint16_t shade565(uint16_t color, float amount);
    static constexpr int MESH_MAX_VERTICES = 256;
    static constexpr int MESH_MAX_FACES = 512;

private:
    enum : uint8_t { ENTRY_FILL = 1, ENTRY_EDGES = 2, ENTRY_LINE = 4, ENTRY_POINT = 8 };
    // 24 bytes. For a triangle `radius` holds the edge mask instead: bit 0
    // draws a-b, bit 1 b-c, bit 2 c-a — so the two halves of a quad show
    // its outline without the shared diagonal, and a quad in edge mode
    // costs two entries rather than six.
    struct Entry {
        int16_t x[3];
        int16_t y[3];
        float depth;
        uint16_t color;
        uint16_t edge;
        uint8_t flags;
        uint8_t radius;
    };
    struct MeshFace { uint16_t v[4]; };   // v[3] == 0xFFFF for a triangle

    struct Basis {
        float sinX, cosX, sinY, cosY, sinZ, cosZ;
    };

    static Vec3 rotate(const Vec3& value,
                       float sinX, float cosX,
                       float sinY, float cosY,
                       float sinZ, float cosZ);
    static Vec3 rotate(const Vec3& value, const Basis& basis) {
        return rotate(value, basis.sinX, basis.cosX, basis.sinY, basis.cosY,
                      basis.sinZ, basis.cosZ);
    }
    static Basis basisOf(float rx, float ry, float rz);
    bool projectVec(const Vec3& value, Projected& output) const;
    static void line2D(TFT_eSPI* tft, TFT_eSprite* sprite,
                       int x1, int y1, int x2, int y2, uint16_t color);
    static void point2D(TFT_eSPI* tft, TFT_eSprite* sprite,
                        int x, int y, int radius, uint16_t color);
    static void triangle2D(TFT_eSPI* tft, TFT_eSprite* sprite,
                           const Projected& a, const Projected& b,
                           const Projected& c, bool filled, uint16_t color);

    // Applies the model transform and the view rotation.
    Vec3 toWorld(const Vec3& local) const;
    // Lit colour for a face with this world-space normal.
    uint16_t litColor(const Vec3& normal, uint16_t base) const;
    // Emits one face (tri) either into the scene or straight to the target.
    // Returns 1 when drawn/collected, 0 when culled or dropped.
    int emitFace(TFT_eSPI* tft, TFT_eSprite* sprite, const Vec3& a,
                 const Vec3& b, const Vec3& c, int mode, uint16_t color,
                 uint16_t edge);
    int emitLine(TFT_eSPI* tft, TFT_eSprite* sprite, const Projected& a,
                 const Projected& b, float depth, uint16_t color);
    int emitPoint(TFT_eSPI* tft, TFT_eSprite* sprite, const Projected& p,
                  float depth, int radius, uint16_t color);
    void drawEntry(TFT_eSPI* tft, TFT_eSprite* sprite, const Entry& e,
                   int offsetX, int offsetY) const;
    void sortEntries();
    // Shared by box/sphere/cylinder/torus/mesh: vertices in local space,
    // faces as index lists.
    int emitQuad(TFT_eSPI* tft, TFT_eSprite* sprite, const Vec3& a,
                 const Vec3& b, const Vec3& c, const Vec3& d, int mode,
                 uint16_t color, uint16_t edge);

    float cameraCenterX = 120.0f;
    float cameraCenterY = 160.0f;
    float focalLength = 110.0f;
    float cameraDistance = 5.0f;
    uint32_t renderMicros = 0;
    int renderedFaces = 0;

    Entry* entries = nullptr;
    bool entriesInReserve = false;
    int capacity = 0;
    int count = 0;
    int dropped = 0;
    bool collecting = false;

    Basis view = {0, 1, 0, 1, 0, 1};
    Basis model = {0, 1, 0, 1, 0, 1};
    Vec3 modelPos = {0, 0, 0};
    float modelScale = 1.0f;
    Vec3 light = {-0.365f, -0.548f, -0.752f};
    float ambient = 0.36f;
    uint16_t background = 0x0000;
    int viewX = 0, viewY = 0, viewW = 240, viewH = 320;

    Vec3* meshVertex_ = nullptr;
    MeshFace* meshFace_ = nullptr;
    bool meshInReserve = false;
    int meshVerts = 0;
    int meshFaces = 0;
};
