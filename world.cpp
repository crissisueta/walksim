#include "world.h"
#include "terrain.h"
#include "building.h"
#include "vegetation.h"
#include <float.h>
#include <math.h>

static const int BUILDING_LIMIT = 10;
struct WorldBuilding { Building plan; Vector3 position; float padHalfWidth; float padHalfDepth; };
static WorldBuilding g_buildings[BUILDING_LIMIT];
static int g_buildingCount = 0;
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
    value ^= value >> 16; value *= 0x7FEB352Du;
    value ^= value >> 15; value *= 0x846CA68Bu;
    value ^= value >> 16;
    return value ? value : 1;
}
static float Random01(void) { return (RandomU32() >> 8) / 16777216.0f; }
static float RandomRange(float minimum, float maximum) { return minimum + (maximum - minimum) * Random01(); }
static float Distance(float x1, float z1, float x2, float z2) { float x = x1 - x2, z = z1 - z2; return sqrtf(x*x + z*z); }
static float TerrainSlope(float x, float z)
{
    const float sample = 5.0f;
    float dx = (Terrain_BaseHeight(x + sample, z) - Terrain_BaseHeight(x - sample, z)) / (2.0f * sample);
    float dz = (Terrain_BaseHeight(x, z + sample) - Terrain_BaseHeight(x, z - sample)) / (2.0f * sample);
    return sqrtf(dx * dx + dz * dz);
}

static void BuildingPadSize(const Building &building, float *halfWidth, float *halfDepth)
{
    float minX = 0, maxX = 0, minZ = 0, maxZ = 0;
    for (size_t i = 0; i < building.rooms.size(); i++) {
        const Room &room = building.rooms[i];
        float angle = room.rotation * DEG2RAD;
        float hw = (room.width * fabsf(cosf(angle)) + room.depth * fabsf(sinf(angle))) * 0.5f;
        float hd = (room.depth * fabsf(cosf(angle)) + room.width * fabsf(sinf(angle))) * 0.5f;
        if (!i || room.position.x - hw < minX) minX = room.position.x - hw;
        if (!i || room.position.x + hw > maxX) maxX = room.position.x + hw;
        if (!i || room.position.z - hd < minZ) minZ = room.position.z - hd;
        if (!i || room.position.z + hd > maxZ) maxZ = room.position.z + hd;
    }
    *halfWidth = (maxX - minX) * 0.5f + 0.50f;
    *halfDepth = (maxZ - minZ) * 0.5f + 0.50f;
}

static float FoundationHeight(float x, float z, float halfWidth, float halfDepth)
{
    float total = 0.0f;
    for (int iz = 0; iz < 5; iz++) for (int ix = 0; ix < 5; ix++)
        total += Terrain_BaseHeight(x + halfWidth * (2.0f * ix / 4.0f - 1.0f),
                                    z + halfDepth * (2.0f * iz / 4.0f - 1.0f));
    return total / 25.0f;
}

static void GenerateBuildings(unsigned int seed)
{
    unsigned int savedState = g_randomState;
    g_randomState = seed;
    g_buildingCount = 0;
    const float edge = TERRAIN_SIZE * 0.5f - 40.0f;
    for (int attempt = 0; attempt < BUILDING_LIMIT * 180 && g_buildingCount < BUILDING_LIMIT; attempt++) {
        float x = RandomRange(-edge, edge), z = RandomRange(-edge, edge);
        if (Distance(x, z, 0, 0) < 35.0f || TerrainSlope(x, z) > 0.28f) continue;
        bool separated = true;
        for (int i = 0; i < g_buildingCount; i++)
            if (Distance(x, z, g_buildings[i].position.x, g_buildings[i].position.z) < 55.0f) separated = false;
        if (!separated) continue;

        WorldBuilding &instance = g_buildings[g_buildingCount];
        instance.plan = Building_Generate(DeriveSeed(seed, (unsigned int)g_buildingCount + 1u));
        BuildingPadSize(instance.plan, &instance.padHalfWidth, &instance.padHalfDepth);
        float y = FoundationHeight(x, z, instance.padHalfWidth, instance.padHalfDepth);
        if (Terrain_FlatZoneOverlaps(x, z, instance.padHalfWidth + 3.0f, instance.padHalfDepth + 3.0f)) continue;
        Terrain_AddBuildingPad(x, z, instance.padHalfWidth, instance.padHalfDepth, y, 8.0f);
        Terrain_AddFlatZone(x, z, instance.padHalfWidth + 2.5f, instance.padHalfDepth + 2.5f, y, 8.0f);
        instance.position = Vector3{ x, y, z };
        instance.plan.position = instance.position;
        Building_GenerateSupports(&instance.plan);
        g_buildingCount++;
    }
    g_randomState = savedState;
}

void World_Generate(unsigned int seed)
{
    unsigned int worldSeed = seed ? seed : 1;
    g_randomState = worldSeed;
    Terrain_ClearPads();
    Terrain_ClearFlatZones();
    GenerateBuildings(worldSeed);
    VegetationAvoidArea avoided[BUILDING_LIMIT];
    for (int i = 0; i < g_buildingCount; i++)
        avoided[i] = VegetationAvoidArea{ g_buildings[i].position.x, g_buildings[i].position.z,
                                          g_buildings[i].padHalfWidth, g_buildings[i].padHalfDepth };
    Vegetation_Generate(DeriveSeed(worldSeed, 0x564547u), avoided, g_buildingCount);
    g_randomState = worldSeed;
}
void World_Init(unsigned int seed) { Vegetation_Init(); World_Generate(seed); }
void World_SetLighting(Shader shader) { (void)shader; }
void World_Draw(bool debug)
{
    for (int i = 0; i < g_buildingCount; i++) Building_Draw(g_buildings[i].plan, debug);
    Vegetation_Draw();
}

void World_DrawDebugLabels(Camera3D camera)
{
    for (int i = 0; i < g_buildingCount; i++) {
        const Building &building = g_buildings[i].plan;
        for (size_t roomIndex = 0; roomIndex < building.rooms.size(); roomIndex++) {
            const Room &room = building.rooms[roomIndex];
            Vector2 screen = GetWorldToScreen(Vector3{ building.position.x + room.position.x,
                building.position.y + room.position.y + room.height * 0.5f, building.position.z + room.position.z }, camera);
            DrawText(Building_RoomTypeName(room.type), (int)screen.x, (int)screen.y, 14, YELLOW);
        }
    }
}
void World_UpdateDoors(float dt) { for (int i = 0; i < g_buildingCount; i++) Building_UpdateDoors(&g_buildings[i].plan, dt); }

static int FindDoor(Camera3D camera, int *doorIndex)
{
    int bestBuilding = -1; float bestScore = -FLT_MAX;
    for (int i = 0; i < g_buildingCount; i++) {
        float score = -FLT_MAX;
        int candidate = Building_FindDoor(g_buildings[i].plan, camera, 3.0f, &score);
        if (candidate >= 0 && score > bestScore) { bestBuilding = i; *doorIndex = candidate; bestScore = score; }
    }
    return bestBuilding;
}
const char *World_DoorPrompt(Camera3D camera)
{
    int doorIndex, buildingIndex = FindDoor(camera, &doorIndex);
    if (buildingIndex < 0) return NULL;
    const BuildingDoor &door = g_buildings[buildingIndex].plan.doors[doorIndex];
    return fabsf(door.openAngle - door.targetAngle) > 0.01f ? NULL : (door.open ? "E - Close" : "E - Open");
}
bool World_InteractDoor(Camera3D camera)
{
    int doorIndex, buildingIndex = FindDoor(camera, &doorIndex);
    if (buildingIndex < 0) return false;
    BuildingDoor &door = g_buildings[buildingIndex].plan.doors[doorIndex];
    if (fabsf(door.openAngle - door.targetAngle) > 0.01f) return false;
    Building_ToggleDoor(&g_buildings[buildingIndex].plan, doorIndex);
    return true;
}
bool World_Collides(float x, float z, float radius, float feetY, float height)
{
    if (Vegetation_TreeCollides(x, z, radius)) return true;
    for (int i = 0; i < g_buildingCount; i++)
        if (Building_Collides(g_buildings[i].plan, x, z, radius, feetY, height)) return true;
    return false;
}
float World_GroundHeight(float x, float z, float terrainHeight, float maximumHeight)
{
    float ground = terrainHeight;
    for (int i = 0; i < g_buildingCount; i++) ground = Building_FloorHeight(g_buildings[i].plan, x, z, ground, maximumHeight);
    return ground;
}
float World_CeilingHeight(float x, float z, float minimumHeight)
{
    float ceiling = FLT_MAX;
    for (int i = 0; i < g_buildingCount; i++) {
        float candidate = Building_CeilingHeight(g_buildings[i].plan, x, z, minimumHeight);
        if (candidate < ceiling) ceiling = candidate;
    }
    return ceiling;
}
void World_Unload(void) { Vegetation_Unload(); g_buildingCount = 0; }
int World_BuildingCount(void) { return g_buildingCount; }
unsigned int World_FirstBuildingSeed(void) { return g_buildingCount ? g_buildings[0].plan.seed : 0; }
