#pragma once
#include "raylib.h"

// City owns both imported decorative buildings and procedural large buildings.
// Call City_Init once after Terrain_Init; City_Unload releases model resources.
void City_Init(unsigned int seed);
void City_SetLighting(Shader shader);
void City_Generate(unsigned int seed);
void City_Draw(bool debug); // debug exposes procedural plan diagnostics
// True only for walls. Procedural supports intentionally have no collision.
bool City_Collides(float x, float z, float radius, float feetY, float height);
// Merge terrain with raised procedural floors and ceilings for the player.
float City_GroundHeight(float x, float z, float terrainHeight);
float City_CeilingHeight(float x, float z, float minimumHeight);
void City_Unload(void);
int  City_Count(void);
int  City_LargeBuildingCount(void);
unsigned int City_FirstLargeBuildingSeed(void);
