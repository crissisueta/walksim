#include "city.h"
#include "terrain.h"
#include <math.h>

static const int CITY_LIMIT = 5;
static const int BUILDING_LIMIT = 14;
static const int MODEL_LIMIT = 3;

struct Building {
    Vector3 position;
    float rotation;
    float scale;
    float baseY;
    int model;
    Color tint;
};

struct City {
    Vector2 position;
    float radius;
    int buildingCount;
    Building buildings[BUILDING_LIMIT];
};

static Model g_models[MODEL_LIMIT];
static float g_modelBaseY[MODEL_LIMIT];
static int g_modelCount = 0;
static City g_cities[CITY_LIMIT];
static int g_cityCount = 0;
static unsigned int g_randomState = 1;

static unsigned int RandomU32(void)
{
    g_randomState ^= g_randomState << 13;
    g_randomState ^= g_randomState >> 17;
    g_randomState ^= g_randomState << 5;
    return g_randomState;
}

static float Random01(void)
{
    return (RandomU32() >> 8) / 16777216.0f;
}

static float RandomRange(float min, float max)
{
    return min + (max - min) * Random01();
}

static float Distance(float x1, float z1, float x2, float z2)
{
    float dx = x1 - x2;
    float dz = z1 - z2;
    return sqrtf(dx * dx + dz * dz);
}

static float TerrainSlope(float x, float z)
{
    const float sample = 5.0f;
    float dx = (Terrain_Height(x + sample, z) - Terrain_Height(x - sample, z)) / (2.0f * sample);
    float dz = (Terrain_Height(x, z + sample) - Terrain_Height(x, z - sample)) / (2.0f * sample);
    return sqrtf(dx * dx + dz * dz);
}

static void LoadBuildingModels(void)
{
    static const char *paths[MODEL_LIMIT] = {
        "assets/B1.obj", "assets/B2.obj", "assets/B3.obj"
    };

    g_modelCount = 0;
    for (int i = 0; i < MODEL_LIMIT; i++) {
        Model model = LoadModel(paths[i]);
        if (model.meshCount == 0) continue;

        BoundingBox bounds = GetModelBoundingBox(model);
        g_models[g_modelCount] = model;
        g_modelBaseY[g_modelCount] = bounds.min.y;
        g_modelCount++;
    }
}

static void GenerateBuildings(City *city)
{
    static const Color palette[] = {
        { 232, 213, 172, 255 }, { 202, 151, 119, 255 },
        { 205, 211, 194, 255 }, { 190, 197, 207, 255 },
        { 222, 199, 126, 255 }
    };
    const int wanted = 9 + (int)(Random01() * 6.0f);
    city->buildingCount = 0;

    for (int attempt = 0; attempt < wanted * 100 && city->buildingCount < wanted; attempt++) {
        float angle = RandomRange(0.0f, 2.0f * PI);
        float distance = sqrtf(Random01()) * city->radius;
        float x = city->position.x + cosf(angle) * distance;
        float z = city->position.y + sinf(angle) * distance;
        if (TerrainSlope(x, z) > 0.28f) continue;

        bool spaced = true;
        for (int i = 0; i < city->buildingCount; i++) {
            const Building &other = city->buildings[i];
            if (Distance(x, z, other.position.x, other.position.z) < 19.0f) {
                spaced = false;
                break;
            }
        }
        if (!spaced) continue;

        Building &building = city->buildings[city->buildingCount++];
        building.model = (int)(Random01() * g_modelCount);
        building.scale = RandomRange(0.85f, 1.2f);
        building.rotation = RandomRange(0.0f, 360.0f);
        building.baseY = g_modelBaseY[building.model];
        building.position = Vector3{
            x,
            Terrain_Height(x, z) - building.baseY * building.scale,
            z
        };
        building.tint = palette[(int)(Random01() * (sizeof(palette) / sizeof(palette[0])))];
    }
}

void City_Generate(unsigned int seed)
{
    if (g_modelCount == 0) {
        g_cityCount = 0;
        return;
    }

    g_randomState = seed ? seed : 1;
    g_cityCount = 0;

    const float edge = TERRAIN_SIZE * 0.5f - 65.0f;
    for (int attempt = 0; attempt < 2500 && g_cityCount < CITY_LIMIT; attempt++) {
        float x = RandomRange(-edge, edge);
        float z = RandomRange(-edge, edge);
        float radius = RandomRange(38.0f, 52.0f);

        if (Distance(x, z, 0.0f, 0.0f) < 90.0f || TerrainSlope(x, z) > 0.20f) continue;

        bool separated = true;
        for (int i = 0; i < g_cityCount; i++) {
            const City &other = g_cities[i];
            if (Distance(x, z, other.position.x, other.position.y) < 145.0f) {
                separated = false;
                break;
            }
        }
        if (!separated) continue;

        City &city = g_cities[g_cityCount++];
        city.position = Vector2{ x, z };
        city.radius = radius;
        GenerateBuildings(&city);
    }
}

void City_Init(unsigned int seed)
{
    LoadBuildingModels();
    City_Generate(seed);
}

void City_Draw(void)
{
    for (int i = 0; i < g_cityCount; i++) {
        const City &city = g_cities[i];
        for (int j = 0; j < city.buildingCount; j++) {
            const Building &building = city.buildings[j];
            DrawModelEx(g_models[building.model], building.position,
                        Vector3{ 0.0f, 1.0f, 0.0f }, building.rotation,
                        Vector3{ building.scale, building.scale, building.scale },
                        building.tint);
        }
    }
}

void City_Unload(void)
{
    for (int i = 0; i < g_modelCount; i++) UnloadModel(g_models[i]);
    g_modelCount = 0;
    g_cityCount = 0;
}

int City_Count(void)
{
    return g_cityCount;
}