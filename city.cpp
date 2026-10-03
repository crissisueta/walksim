#include "city.h"
#include "terrain.h"
#include "building.h"
#include "lighting.h"
#include <float.h>
#include <math.h>

// City generation chooses deterministic locations, installs terrain pads, then
// places procedural plans. Terrain-dependent support data is finalized last.
static const int CITY_LIMIT = 5;
static const int BUILDING_LIMIT = 20;
static const int MODEL_LIMIT = 3;
static const int WALL_LIMIT = 4096;
static const int LARGE_BUILDING_LIMIT = 4;

struct WallSegment {
    float x1;
    float z1;
    float x2;
    float z2;
    float minY;
    float maxY;
};

struct CityBuilding {
    Vector3 position;
    float rotation;
    float scale;
    float groundY;
    int model;
    Color tint;
};

struct LargeBuildingInstance {
    Building plan;
    Vector3 position;
};

struct City {
    Vector2 position;
    float radius;
    unsigned int seed;
    int buildingCount;
    CityBuilding buildings[BUILDING_LIMIT];
    int largeBuildingCount;
    LargeBuildingInstance largeBuildings[LARGE_BUILDING_LIMIT];
};

static Model g_models[MODEL_LIMIT];
static float g_modelBaseY[MODEL_LIMIT];
static float g_modelCenterX[MODEL_LIMIT];
static float g_modelCenterZ[MODEL_LIMIT];
static float g_modelHalfWidth[MODEL_LIMIT];
static float g_modelHalfDepth[MODEL_LIMIT];
static WallSegment g_modelWalls[MODEL_LIMIT][WALL_LIMIT];
static int g_modelWallCount[MODEL_LIMIT];
static Texture2D g_finishTextures[MODEL_LIMIT];
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

static unsigned int DeriveSeed(unsigned int seed, unsigned int stream)
{
    unsigned int value = seed ^ (stream * 0x9E3779B9u);
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value ? value : 1;
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

static void LargeBuildingPad(const Building &building, float *halfWidth, float *halfDepth)
{
    float minX = 0.0f, maxX = 0.0f, minZ = 0.0f, maxZ = 0.0f;
    for (size_t i = 0; i < building.rooms.size(); i++) {
        const Room &room = building.rooms[i];
        float angle = room.rotation * DEG2RAD;
        float roomHalfWidth = (room.width * fabsf(cosf(angle)) +
                               room.depth * fabsf(sinf(angle))) * 0.5f;
        float roomHalfDepth = (room.depth * fabsf(cosf(angle)) +
                               room.width * fabsf(sinf(angle))) * 0.5f;
        float roomMinX = room.position.x - roomHalfWidth;
        float roomMaxX = room.position.x + roomHalfWidth;
        float roomMinZ = room.position.z - roomHalfDepth;
        float roomMaxZ = room.position.z + roomHalfDepth;
        if (i == 0 || roomMinX < minX) minX = roomMinX;
        if (i == 0 || roomMaxX > maxX) maxX = roomMaxX;
        if (i == 0 || roomMinZ < minZ) minZ = roomMinZ;
        if (i == 0 || roomMaxZ > maxZ) maxZ = roomMaxZ;
    }
    // Keep a small flat apron beyond the complete room footprint. Supports
    // handle any remaining terrain transition rather than enlarging this pad.
    *halfWidth = (maxX - minX) * 0.5f + 0.50f;
    *halfDepth = (maxZ - minZ) * 0.5f + 0.50f;
}

static float LargeBuildingFoundationHeight(float x, float z, float halfWidth, float halfDepth)
{
    // Average a 5x5 footprint grid. This is stable, reflects the terrain the
    // building actually occupies, and avoids choosing a single extreme point.
    float total = 0.0f;
    const int samples = 5;
    for (int iz = 0; iz < samples; iz++) {
        for (int ix = 0; ix < samples; ix++) {
            float sx = x + halfWidth * (2.0f * ix / (samples - 1) - 1.0f);
            float sz = z + halfDepth * (2.0f * iz / (samples - 1) - 1.0f);
            total += Terrain_BaseHeight(sx, sz);
        }
    }
    return total / (samples * samples);
}

static Vector3 MeshVertex(const Mesh &mesh, int index)
{
    return Vector3{
        mesh.vertices[index * 3],
        mesh.vertices[index * 3 + 1],
        mesh.vertices[index * 3 + 2]
    };
}

static float HorizontalDistanceSquared(Vector3 a, Vector3 b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

static void AddCollisionWall(int model, Vector3 a, Vector3 b, Vector3 c,
                             float baseY, float normalY, float normalLength)
{
    if (normalLength == 0.0f || fabsf(normalY) > normalLength * 0.35f ||
        g_modelWallCount[model] >= WALL_LIMIT) return;

    Vector3 first = a;
    Vector3 second = b;
    float longest = HorizontalDistanceSquared(a, b);
    float length = HorizontalDistanceSquared(b, c);
    if (length > longest) { first = b; second = c; longest = length; }
    length = HorizontalDistanceSquared(c, a);
    if (length > longest) { first = c; second = a; longest = length; }
    if (longest < 0.0001f) return;

    WallSegment &wall = g_modelWalls[model][g_modelWallCount[model]++];
    wall.x1 = first.x - g_modelCenterX[model];
    wall.z1 = first.z - g_modelCenterZ[model];
    wall.x2 = second.x - g_modelCenterX[model];
    wall.z2 = second.z - g_modelCenterZ[model];
    wall.minY = fminf(a.y, fminf(b.y, c.y)) - baseY;
    wall.maxY = fmaxf(a.y, fmaxf(b.y, c.y)) - baseY;
}

static void BuildCollisionWalls(Model source, int model, float baseY)
{
    g_modelWallCount[model] = 0;
    for (int meshIndex = 0; meshIndex < source.meshCount; meshIndex++) {
        const Mesh &mesh = source.meshes[meshIndex];
        if (mesh.vertices == NULL) continue;

        for (int triangle = 0; triangle < mesh.triangleCount; triangle++) {
            int ia = mesh.indices ? mesh.indices[triangle * 3] : triangle * 3;
            int ib = mesh.indices ? mesh.indices[triangle * 3 + 1] : triangle * 3 + 1;
            int ic = mesh.indices ? mesh.indices[triangle * 3 + 2] : triangle * 3 + 2;
            Vector3 a = MeshVertex(mesh, ia);
            Vector3 b = MeshVertex(mesh, ib);
            Vector3 c = MeshVertex(mesh, ic);

            float abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
            float acx = c.x - a.x, acy = c.y - a.y, acz = c.z - a.z;
            float nx = aby * acz - abz * acy;
            float ny = abz * acx - abx * acz;
            float nz = abx * acy - aby * acx;
            float normalLength = sqrtf(nx * nx + ny * ny + nz * nz);
            AddCollisionWall(model, a, b, c, baseY, ny, normalLength);
        }
    }
}

static Texture2D BuildConcreteTexture(int finish)
{
    const int size = 256;
    Image image = GenImageColor(size, size, WHITE);
    Color *pixels = (Color *)image.data;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            unsigned int noise = (unsigned int)(x * 374761393u + y * 668265263u + finish * 1013904223u);
            noise = (noise ^ (noise >> 13)) * 1274126177u;
            int grain = (int)((noise >> 24) % 13) - 6;
            int shade = 154 + grain;

            if (finish == 0) {
                int row = y / 4;
                int brickX = (x + ((row & 1) ? 4 : 0)) % 8;
                if ((y % 4) == 0 || brickX == 0) shade = 190 + grain / 2;
                else shade += ((row * 17 + x / 8 * 11) % 13) - 6;
            } else if (finish == 1) {
                if ((x % 8) == 0 || (y % 8) == 0) shade = 194 + grain / 2;
                else shade += (((x / 8) * 7 + (y / 8) * 11) % 11) - 5;
            } else if ((noise & 0x3FFu) == 0) {
                shade -= 24;
            }

            if (shade < 0) shade = 0;
            if (shade > 255) shade = 255;
            pixels[y * size + x] = Color{
                (unsigned char)shade,
                (unsigned char)(shade + (finish == 2 ? 1 : 0)),
                (unsigned char)(shade + (finish == 1 ? 2 : 0)),
                255
            };
        }
    }

    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
    return texture;
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
        int modelIndex = g_modelCount++;
        g_models[modelIndex] = model;
        g_modelBaseY[modelIndex] = bounds.min.y;
        g_modelCenterX[modelIndex] = (bounds.min.x + bounds.max.x) * 0.5f;
        g_modelCenterZ[modelIndex] = (bounds.min.z + bounds.max.z) * 0.5f;
        g_modelHalfWidth[modelIndex] = (bounds.max.x - bounds.min.x) * 0.5f;
        g_modelHalfDepth[modelIndex] = (bounds.max.z - bounds.min.z) * 0.5f;
        BuildCollisionWalls(g_models[modelIndex], modelIndex, bounds.min.y);
        g_finishTextures[modelIndex] = BuildConcreteTexture(modelIndex);
        for (int material = 0; material < model.materialCount; material++)
            SetMaterialTexture(&g_models[modelIndex].materials[material],
                               MATERIAL_MAP_DIFFUSE, g_finishTextures[modelIndex]);
    }
}

static void GenerateBuildings(City *city)
{
    const int wanted = LARGE_BUILDING_LIMIT;
    city->buildingCount = 0;
    city->largeBuildingCount = 0;

    for (int attempt = 0; attempt < wanted * 120 &&
         city->buildingCount + city->largeBuildingCount < wanted; attempt++) {
        float angle = RandomRange(0.0f, 2.0f * PI);
        float distance = sqrtf(Random01()) * city->radius;
        float x = city->position.x + cosf(angle) * distance;
        float z = city->position.y + sinf(angle) * distance;
        if (TerrainSlope(x, z) > 0.28f) continue;

        bool spaced = true;
        for (int i = 0; i < city->largeBuildingCount; i++) {
            const LargeBuildingInstance &other = city->largeBuildings[i];
            if (Distance(x, z, other.position.x, other.position.z) < 55.0f)
                spaced = false;
        }
        if (!spaced) continue;

        const float worldLimit = TERRAIN_SIZE * 0.5f - 5.0f;
        if (fabsf(x) + 35.0f > worldLimit || fabsf(z) + 35.0f > worldLimit) continue;
        unsigned int buildingSeed = DeriveSeed(city->seed,
                                               (unsigned int)city->largeBuildingCount + 1u);
        LargeBuildingInstance &building = city->largeBuildings[city->largeBuildingCount++];
        building.plan = Building_GenerateTestBuilding(buildingSeed);
        float halfWidth, halfDepth;
        LargeBuildingPad(building.plan, &halfWidth, &halfDepth);
        float foundationY = LargeBuildingFoundationHeight(x, z, halfWidth, halfDepth);
        Terrain_AddBuildingPad(x, z, halfWidth, halfDepth, foundationY, 8.0f);
        building.position = Vector3{ x, foundationY, z };
        building.plan.position = building.position;
        Building_GenerateSupports(&building.plan);
    }
}

void City_Generate(unsigned int seed)
{
    unsigned int worldSeed = seed ? seed : 1;
    g_randomState = worldSeed;
    g_cityCount = 0;
    Terrain_ClearPads();

    const float edge = TERRAIN_SIZE * 0.5f - 65.0f;
    for (int attempt = 0; attempt < 2500 && g_cityCount < CITY_LIMIT; attempt++) {
        float x = RandomRange(-edge, edge);
        float z = RandomRange(-edge, edge);
        float radius = RandomRange(44.0f, 58.0f);

        if (Distance(x, z, 0.0f, 0.0f) < 120.0f || TerrainSlope(x, z) > 0.20f) continue;

        bool separated = true;
        for (int i = 0; i < g_cityCount; i++) {
            const City &other = g_cities[i];
            if (Distance(x, z, other.position.x, other.position.y) < 190.0f) {
                separated = false;
                break;
            }
        }
        if (!separated) continue;

        City &city = g_cities[g_cityCount++];
        city = City{};
        city.position = Vector2{ x, z };
        city.radius = radius;
        city.seed = DeriveSeed(worldSeed, (unsigned int)g_cityCount);
        GenerateBuildings(&city);
    }
}

void City_Init(unsigned int seed)
{
    LoadBuildingModels();
    City_Generate(seed);
}

void City_SetLighting(Shader shader)
{
    for (int i = 0; i < g_modelCount; i++) Lighting_Attach(&g_models[i], shader);
}

void City_Draw(bool debug)
{
    for (int i = 0; i < g_cityCount; i++) {
        const City &city = g_cities[i];
        for (int j = 0; j < city.buildingCount; j++) {
            const CityBuilding &building = city.buildings[j];
            float angle = building.rotation * DEG2RAD;
            float centerX = g_modelCenterX[building.model] * building.scale;
            float centerZ = g_modelCenterZ[building.model] * building.scale;
            Vector3 origin = {
                building.position.x - (centerX * cosf(angle) + centerZ * sinf(angle)),
                building.position.y,
                building.position.z - (-centerX * sinf(angle) + centerZ * cosf(angle))
            };
            DrawModelEx(g_models[building.model], origin,
                        Vector3{ 0.0f, 1.0f, 0.0f }, building.rotation,
                        Vector3{ building.scale, building.scale, building.scale },
                        building.tint);
        }
        for (int j = 0; j < city.largeBuildingCount; j++)
            Building_Draw(city.largeBuildings[j].plan, debug);
    }
}

bool City_Collides(float x, float z, float radius, float feetY, float height)
{
    for (int i = 0; i < g_cityCount; i++) {
        const City &city = g_cities[i];
        for (int j = 0; j < city.buildingCount; j++) {
            const CityBuilding &building = city.buildings[j];
            float dx = x - building.position.x;
            float dz = z - building.position.z;
            float broadRadius = sqrtf(g_modelHalfWidth[building.model] * g_modelHalfWidth[building.model] +
                                      g_modelHalfDepth[building.model] * g_modelHalfDepth[building.model]) *
                                building.scale + radius;
            if (dx * dx + dz * dz > broadRadius * broadRadius) continue;

            float angle = building.rotation * DEG2RAD;
            float localX = dx * cosf(angle) - dz * sinf(angle);
            float localZ = dx * sinf(angle) + dz * cosf(angle);
            localX /= building.scale;
            localZ /= building.scale;
            float localRadius = radius / building.scale;
            float localFeet = (feetY - building.groundY) / building.scale;
            float localTop = localFeet + height / building.scale;

            for (int wallIndex = 0; wallIndex < g_modelWallCount[building.model]; wallIndex++) {
                const WallSegment &wall = g_modelWalls[building.model][wallIndex];
                if (localFeet >= wall.maxY || localTop <= wall.minY) continue;

                float sx = wall.x2 - wall.x1;
                float sz = wall.z2 - wall.z1;
                float lengthSquared = sx * sx + sz * sz;
                float t = ((localX - wall.x1) * sx + (localZ - wall.z1) * sz) / lengthSquared;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                float nearestX = wall.x1 + sx * t;
                float nearestZ = wall.z1 + sz * t;
                float outsideX = localX - nearestX;
                float outsideZ = localZ - nearestZ;
                if (outsideX * outsideX + outsideZ * outsideZ < localRadius * localRadius) return true;
            }
        }
        for (int j = 0; j < city.largeBuildingCount; j++) {
            if (Building_Collides(city.largeBuildings[j].plan, x, z, radius,
                                  feetY, height)) return true;
        }
    }
    return false;
}

float City_GroundHeight(float x, float z, float terrainHeight)
{
    float ground = terrainHeight;
    for (int i = 0; i < g_cityCount; i++) {
        const City &city = g_cities[i];
        for (int j = 0; j < city.largeBuildingCount; j++)
            ground = Building_FloorHeight(city.largeBuildings[j].plan, x, z, ground);
    }
    return ground;
}

float City_CeilingHeight(float x, float z, float minimumHeight)
{
    float ceiling = FLT_MAX;
    for (int i = 0; i < g_cityCount; i++) {
        const City &city = g_cities[i];
        for (int j = 0; j < city.largeBuildingCount; j++) {
            float candidate = Building_CeilingHeight(city.largeBuildings[j].plan, x, z,
                                                      minimumHeight);
            if (candidate < ceiling) ceiling = candidate;
        }
    }
    return ceiling;
}

void City_Unload(void)
{
    for (int i = 0; i < g_modelCount; i++) {
        Lighting_Detach(&g_models[i]);
        UnloadModel(g_models[i]);
        UnloadTexture(g_finishTextures[i]);
    }
    g_modelCount = 0;
    g_cityCount = 0;
}

int City_Count(void)
{
    return g_cityCount;
}

int City_LargeBuildingCount(void)
{
    int count = 0;
    for (int i = 0; i < g_cityCount; i++) count += g_cities[i].largeBuildingCount;
    return count;
}

unsigned int City_FirstLargeBuildingSeed(void)
{
    for (int i = 0; i < g_cityCount; i++)
        if (g_cities[i].largeBuildingCount > 0)
            return g_cities[i].largeBuildings[0].plan.seed;
    return 0;
}
