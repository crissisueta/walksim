#include "terrain.h"
#include <math.h>

static unsigned int g_seed = 0;

// Pads are registered by city generation before the terrain mesh is built.
// A fixed-size list keeps the terrain API simple and matches the city limits.
static const int TERRAIN_PAD_LIMIT = 20;
struct TerrainPad {
    float x, z;
    float halfWidth, halfDepth;
    float height;
    float blendWidth;
};
static TerrainPad g_pads[TERRAIN_PAD_LIMIT];
static int g_padCount = 0;

// ---------------------------------------------------------------------------
// Seeded value noise. No external library, fully deterministic for a given seed.
// ---------------------------------------------------------------------------

// Pseudo-random value in [0,1) for an integer lattice point.
static float Hash(int x, int z, int layer)
{
    unsigned int h = (unsigned int)x * 374761393u
                   + (unsigned int)z * 668265263u
                   + (unsigned int)layer * 1013904223u
                   + g_seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = (h ^ (h >> 16)) * 2654435761u;
    h ^= (h >> 15);
    return (h & 0xFFFFFF) / (float)0x1000000;
}

static float Lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Smooth noise in roughly [-1, 1]. One lattice cell = 1 unit of input.
static float Noise(float x, float z, int layer)
{
    int   xi = (int)floorf(x), zi = (int)floorf(z);
    float fx = x - xi,         fz = z - zi;
    float u = fx * fx * (3.0f - 2.0f * fx);   // smoothstep
    float v = fz * fz * (3.0f - 2.0f * fz);
    float a = Hash(xi,     zi,     layer);
    float b = Hash(xi + 1, zi,     layer);
    float c = Hash(xi,     zi + 1, layer);
    float d = Hash(xi + 1, zi + 1, layer);
    return Lerpf(Lerpf(a, b, u), Lerpf(c, d, u), v) * 2.0f - 1.0f;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

TerrainParams g_terrainParams;

void Terrain_ResetParams(void)
{
    g_terrainParams.amp[0] = 22.0f;  g_terrainParams.wavelength[0] = 220.0f;  // big rolling hills
    g_terrainParams.amp[1] =  6.0f;  g_terrainParams.wavelength[1] =  70.0f;  // medium bumps
    g_terrainParams.amp[2] =  0.8f;  g_terrainParams.wavelength[2] =  20.0f;  // small ripples

    g_terrainParams.valley[0] =  45.0f; g_terrainParams.valley[1] = 135.0f; g_terrainParams.valley[2] = 35.0f;
    g_terrainParams.hill[0]   = 125.0f; g_terrainParams.hill[1]   = 195.0f; g_terrainParams.hill[2]   = 50.0f;
}

void Terrain_Init(unsigned int seed)
{
    g_seed = seed;
    Terrain_ResetParams();
    Terrain_ClearPads();
}

float Terrain_BaseHeight(float x, float z)
{
    // Three octaves: big hills, medium bumps, small ripples (see TerrainParams).
    const TerrainParams &p = g_terrainParams;
    float h = 0.0f;
    for (int i = 0; i < 3; i++)
        h += Noise(x / p.wavelength[i], z / p.wavelength[i], i) * p.amp[i];
    return h;
}

void Terrain_ClearPads(void)
{
    g_padCount = 0;
}

void Terrain_AddBuildingPad(float x, float z, float halfWidth, float halfDepth,
                            float height, float blendWidth)
{
    if (g_padCount >= TERRAIN_PAD_LIMIT) return;
    g_pads[g_padCount++] = TerrainPad{ x, z, halfWidth, halfDepth, height, blendWidth };
}

float Terrain_Height(float x, float z)
{
    float height = Terrain_BaseHeight(x, z);
    for (int i = 0; i < g_padCount; i++) {
        const TerrainPad &pad = g_pads[i];
        float dx = fabsf(x - pad.x) - pad.halfWidth;
        float dz = fabsf(z - pad.z) - pad.halfDepth;
        if (dx < 0.0f) dx = 0.0f;
        if (dz < 0.0f) dz = 0.0f;
        float distance = sqrtf(dx * dx + dz * dz);
        if (distance >= pad.blendWidth) continue;

        // Cubic smoothstep holds the complete footprint level, then blends
        // into the untouched height function without a visible hard edge.
        float t = distance / pad.blendWidth;
        float blend = t * t * (3.0f - 2.0f * t);
        height = Lerpf(pad.height, height, blend);
    }
    return height;
}

// Grass colour for one vertex, with sun shading baked in (no shaders needed).
static Color GrassColor(float x, float z, float h, float nx, float ny, float nz)
{
    // Valleys: deeper green. Hilltops: lighter, yellower green.
    float t = (h + 15.0f) / 40.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const TerrainParams &p = g_terrainParams;
    float r = Lerpf(p.valley[0], p.hill[0], t);
    float g = Lerpf(p.valley[1], p.hill[1], t);
    float b = Lerpf(p.valley[2], p.hill[2], t);

    // Low-frequency patchiness so the grass isn't one flat colour.
    float patch = 1.0f + Noise(x / 9.0f, z / 9.0f, 3) * 0.07f;

    // Fixed sun direction. Flat ground -> lit ~ 1.0, sun-facing slopes brighter,
    // slopes facing away darker.
    const float lx = 0.5f, ly = 0.7f, lz = 0.35f;
    float len  = sqrtf(lx * lx + ly * ly + lz * lz);
    float diff = (nx * lx + ny * ly + nz * lz) / len;
    if (diff < 0.0f) diff = 0.0f;
    float lit = (0.55f + 0.60f * diff) * patch;

    r *= lit; g *= lit; b *= lit;
    if (r > 255.0f) r = 255.0f;
    if (g > 255.0f) g = 255.0f;
    if (b > 255.0f) b = 255.0f;
    return Color{ (unsigned char)r, (unsigned char)g, (unsigned char)b, 255 };
}

Model Terrain_BuildModel(void)
{
    const int   N    = TERRAIN_RES + 1;
    const float half = TERRAIN_SIZE * 0.5f;
    const float cell = TERRAIN_SIZE / TERRAIN_RES;
    const int   vc   = N * N;
    const int   tc   = TERRAIN_RES * TERRAIN_RES * 2;

    Mesh mesh = {};
    mesh.vertexCount   = vc;
    mesh.triangleCount = tc;
    mesh.vertices  = (float *)MemAlloc(vc * 3 * sizeof(float));
    mesh.normals   = (float *)MemAlloc(vc * 3 * sizeof(float));
    mesh.texcoords = (float *)MemAlloc(vc * 2 * sizeof(float));
    mesh.colors    = (unsigned char *)MemAlloc(vc * 4);
    mesh.indices   = (unsigned short *)MemAlloc(tc * 3 * sizeof(unsigned short));

    // Vertices
    for (int j = 0; j < N; j++) {
        for (int i = 0; i < N; i++) {
            float x = -half + i * cell;
            float z = -half + j * cell;
            float h = Terrain_Height(x, z);

            // Normal from the height gradient (central differences).
            float e  = cell;
            float hx = (Terrain_Height(x + e, z) - Terrain_Height(x - e, z)) / (2.0f * e);
            float hz = (Terrain_Height(x, z + e) - Terrain_Height(x, z - e)) / (2.0f * e);
            float nx = -hx, ny = 1.0f, nz = -hz;
            float nl = sqrtf(nx * nx + ny * ny + nz * nz);
            nx /= nl; ny /= nl; nz /= nl;

            int v = j * N + i;
            mesh.vertices[v * 3 + 0] = x;
            mesh.vertices[v * 3 + 1] = h;
            mesh.vertices[v * 3 + 2] = z;
            mesh.normals[v * 3 + 0] = nx;
            mesh.normals[v * 3 + 1] = ny;
            mesh.normals[v * 3 + 2] = nz;
            mesh.texcoords[v * 2 + 0] = (float)i / TERRAIN_RES;
            mesh.texcoords[v * 2 + 1] = (float)j / TERRAIN_RES;

            Color c = GrassColor(x, z, h, nx, ny, nz);
            mesh.colors[v * 4 + 0] = c.r;
            mesh.colors[v * 4 + 1] = c.g;
            mesh.colors[v * 4 + 2] = c.b;
            mesh.colors[v * 4 + 3] = 255;
        }
    }

    // Triangles: two per grid cell, counter-clockwise when seen from above.
    int k = 0;
    for (int j = 0; j < TERRAIN_RES; j++) {
        for (int i = 0; i < TERRAIN_RES; i++) {
            unsigned short v00 = (unsigned short)(j * N + i);
            unsigned short v10 = (unsigned short)(j * N + i + 1);
            unsigned short v01 = (unsigned short)((j + 1) * N + i);
            unsigned short v11 = (unsigned short)((j + 1) * N + i + 1);
            mesh.indices[k++] = v00; mesh.indices[k++] = v01; mesh.indices[k++] = v10;
            mesh.indices[k++] = v10; mesh.indices[k++] = v01; mesh.indices[k++] = v11;
        }
    }

    UploadMesh(&mesh, false);
    return LoadModelFromMesh(mesh);
}
