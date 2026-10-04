#include "vegetation.h"
#include "terrain.h"
#include "raylib.h"
#include "rlgl.h"
#include <math.h>

static const int TREE_LIMIT = 45;
static const int FLOWER_LIMIT = 30;
static const int GRASS_LIMIT = 120;
// Covers the pad apron and its 8 m terrain blend, so foliage does not grow
// on a foundation or immediately beside a building entrance.
static const float BUILDING_CLEARANCE = 10.0f;
static const float TREE_SPACING = 13.0f;

// Visual proportion controls (metres). Adjust these ranges to make a species
// broader/narrower or taller/shorter without touching placement or collision.
static const float TREE_WIDTH_MIN = 3.2f;
static const float TREE_WIDTH_MAX = 5.2f;
static const float TREE_HEIGHT_MIN = 5.5f;
static const float TREE_HEIGHT_MAX = 8.5f;
static const float FLOWER_WIDTH_MIN = 0.65f;
static const float FLOWER_WIDTH_MAX = 1.15f;
static const float FLOWER_HEIGHT_MIN = 0.45f;
static const float FLOWER_HEIGHT_MAX = 0.85f;
static const float GRASS_WIDTH_MIN = 0.45f;
static const float GRASS_WIDTH_MAX = 0.90f;
static const float GRASS_HEIGHT_MIN = 0.45f;
static const float GRASS_HEIGHT_MAX = 0.95f;

struct VegetationInstance {
    Vector3 position;
    float width;
    float height;
    float rotation;
    Color tint;
};
static VegetationInstance g_trees[TREE_LIMIT], g_flowers[FLOWER_LIMIT], g_grass[GRASS_LIMIT];
static int g_treeCount, g_flowerCount, g_grassCount;
static Texture2D g_treeTexture, g_flowerTexture, g_grassTexture;
static bool g_loaded = false;
static unsigned int g_randomState = 1;

static unsigned int RandomU32(void) { g_randomState ^= g_randomState << 13; g_randomState ^= g_randomState >> 17; g_randomState ^= g_randomState << 5; return g_randomState; }
static float Random01(void) { return (RandomU32() >> 8) / 16777216.0f; }
static float RandomRange(float minimum, float maximum) { return minimum + (maximum - minimum) * Random01(); }

static float TerrainSlope(float x, float z)
{
    const float sample = 2.5f;
    float dx = (Terrain_BaseHeight(x + sample, z) - Terrain_BaseHeight(x - sample, z)) / (2.0f * sample);
    float dz = (Terrain_BaseHeight(x, z + sample) - Terrain_BaseHeight(x, z - sample)) / (2.0f * sample);
    return sqrtf(dx * dx + dz * dz);
}

static bool NearBuilding(float x, float z, const VegetationAvoidArea *avoided, int count)
{
    for (int i = 0; i < count; i++)
        if (fabsf(x - avoided[i].x) <= avoided[i].halfWidth + BUILDING_CLEARANCE &&
            fabsf(z - avoided[i].z) <= avoided[i].halfDepth + BUILDING_CLEARANCE) return true;
    return false;
}

static bool FarFromTrees(float x, float z)
{
    for (int i = 0; i < g_treeCount; i++) {
        float dx = x - g_trees[i].position.x, dz = z - g_trees[i].position.z;
        if (dx * dx + dz * dz < TREE_SPACING * TREE_SPACING) return false;
    }
    return true;
}

static void Place(VegetationInstance *items, int *count, int limit, float minWidth, float maxWidth,
                  float minHeight, float maxHeight, const VegetationAvoidArea *avoided,
                  int avoidedCount, bool trees)
{
    const float edge = TERRAIN_SIZE * 0.5f - 6.0f;
    for (int attempt = 0; attempt < limit * 60 && *count < limit; attempt++) {
        float x = RandomRange(-edge, edge), z = RandomRange(-edge, edge);
        if (TerrainSlope(x, z) > 0.36f || NearBuilding(x, z, avoided, avoidedCount) ||
            (trees && !FarFromTrees(x, z))) continue;
        float tint = RandomRange(0.86f, 1.12f);
        items[(*count)++] = VegetationInstance{
            Vector3{ x, Terrain_Height(x, z), z },
            RandomRange(minWidth, maxWidth), RandomRange(minHeight, maxHeight),
            RandomRange(0.0f, PI),
            Color{ (unsigned char)(255.0f * tint), (unsigned char)(255.0f * tint),
                   (unsigned char)(255.0f * tint), 255 }
        };
    }
}

void Vegetation_Init(void)
{
    if (g_loaded) return;
    g_treeTexture = LoadTexture("assets/tree.png");
    g_flowerTexture = LoadTexture("assets/flower1.png");
    g_grassTexture = LoadTexture("assets/grass.png");
    SetTextureFilter(g_treeTexture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_flowerTexture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_grassTexture, TEXTURE_FILTER_BILINEAR);
    g_loaded = true;
}

void Vegetation_Generate(unsigned int seed, const VegetationAvoidArea *avoided, int avoidedCount)
{
    g_randomState = seed ? seed : 1;
    g_treeCount = g_flowerCount = g_grassCount = 0;
    Place(g_trees, &g_treeCount, TREE_LIMIT, TREE_WIDTH_MIN, TREE_WIDTH_MAX,
          TREE_HEIGHT_MIN, TREE_HEIGHT_MAX, avoided, avoidedCount, true);
    Place(g_flowers, &g_flowerCount, FLOWER_LIMIT, FLOWER_WIDTH_MIN, FLOWER_WIDTH_MAX,
          FLOWER_HEIGHT_MIN, FLOWER_HEIGHT_MAX, avoided, avoidedCount, false);
    Place(g_grass, &g_grassCount, GRASS_LIMIT, GRASS_WIDTH_MIN, GRASS_WIDTH_MAX,
          GRASS_HEIGHT_MIN, GRASS_HEIGHT_MAX, avoided, avoidedCount, false);
}

static void DrawBillboardPlane(const VegetationInstance &item, float angle)
{
    float half = item.width * 0.5f, bottom = item.position.y, top = bottom + item.height;
    float dx = cosf(angle) * half, dz = sinf(angle) * half;
    rlBegin(RL_QUADS);
        rlColor4ub(item.tint.r, item.tint.g, item.tint.b, item.tint.a);
        rlTexCoord2f(0, 1); rlVertex3f(item.position.x - dx, bottom, item.position.z - dz);
        rlTexCoord2f(1, 1); rlVertex3f(item.position.x + dx, bottom, item.position.z + dz);
        rlTexCoord2f(1, 0); rlVertex3f(item.position.x + dx, top, item.position.z + dz);
        rlTexCoord2f(0, 0); rlVertex3f(item.position.x - dx, top, item.position.z - dz);
        // Reverse winding is an explicit back face. This stays visible even
        // if another renderer enables back-face culling before our draw call.
        rlTexCoord2f(0, 1); rlVertex3f(item.position.x - dx, bottom, item.position.z - dz);
        rlTexCoord2f(0, 0); rlVertex3f(item.position.x - dx, top, item.position.z - dz);
        rlTexCoord2f(1, 0); rlVertex3f(item.position.x + dx, top, item.position.z + dz);
        rlTexCoord2f(1, 1); rlVertex3f(item.position.x + dx, bottom, item.position.z + dz);
    rlEnd();
}

static void DrawVegetationBillboard(const VegetationInstance &item, Texture2D texture, int planes)
{
    rlSetTexture(texture.id);
    for (int plane = 0; plane < planes; plane++)
        DrawBillboardPlane(item, item.rotation + PI * plane / planes);
    rlSetTexture(0);
}

void Vegetation_Draw(void)
{
    if (!g_loaded) return;
    // Crossed billboards provide their own front and back faces above.
    rlEnableBackfaceCulling();
    BeginBlendMode(BLEND_ALPHA);
    for (int i = 0; i < g_treeCount; i++) DrawVegetationBillboard(g_trees[i], g_treeTexture, 2);
    for (int i = 0; i < g_flowerCount; i++) DrawVegetationBillboard(g_flowers[i], g_flowerTexture, 2);
    // Three planes make grass clumps fuller from every viewing direction.
    for (int i = 0; i < g_grassCount; i++) DrawVegetationBillboard(g_grass[i], g_grassTexture, 3);
    EndBlendMode();
}

bool Vegetation_TreeCollides(float x, float z, float radius)
{
    const float total = radius + 0.42f;
    for (int i = 0; i < g_treeCount; i++) {
        float dx = x - g_trees[i].position.x, dz = z - g_trees[i].position.z;
        if (dx * dx + dz * dz < total * total) return true;
    }
    return false;
}

void Vegetation_Unload(void)
{
    if (!g_loaded) return;
    UnloadTexture(g_treeTexture); UnloadTexture(g_flowerTexture); UnloadTexture(g_grassTexture);
    g_loaded = false;
}
int Vegetation_TreeCount(void) { return g_treeCount; }
int Vegetation_FlowerCount(void) { return g_flowerCount; }
int Vegetation_GrassCount(void) { return g_grassCount; }
