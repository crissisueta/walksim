#pragma once
#include "raylib.h"

// The terrain is a single square mesh centred on the origin.
// RES*RES cells -> (RES+1)^2 vertices. raylib meshes use 16-bit indices,
// so (RES+1)^2 must stay below 65536 (RES <= 254).
#define TERRAIN_SIZE 600.0f   // metres, edge to edge
#define TERRAIN_RES  240      // cells per side (cell = 2.5 m)

// Everything the in-game settings panel can change about the terrain.
// Defaults live in Terrain_ResetParams() in terrain.cpp.
struct TerrainParams {
    float amp[3];         // height of each noise octave in metres (big, medium, small)
    float wavelength[3];  // size of each octave in metres
    float valley[3];      // grass RGB (0..255) in low areas
    float hill[3];        // grass RGB (0..255) on hilltops
};
extern TerrainParams g_terrainParams;

void  Terrain_ResetParams(void);         // restore default TerrainParams
void  Terrain_Init(unsigned int seed);   // call once, before anything else
float Terrain_BaseHeight(float x, float z); // natural terrain, without building pads
float Terrain_Height(float x, float z);  // ground height at world (x, z)
void  Terrain_ClearPads(void);            // remove all local building terrain edits
// Registers a flat rectangular foundation. blendWidth softly transitions it
// back into natural terrain; pads must be added before Terrain_BuildModel().
void  Terrain_AddBuildingPad(float x, float z, float halfWidth, float halfDepth,
                             float height, float blendWidth);
Model Terrain_BuildModel(void);          // after params change: UnloadModel, then call this again
// Releases the shared procedural ground-detail texture after the final terrain model unload.
void  Terrain_Unload(void);
