#pragma once
#include "raylib.h"

// City owns both imported decorative buildings and procedural large buildings.
// Call City_Init once after Terrain_Init; City_Unload releases model resources.

void City_Init(unsigned int seed);
void City_SetLighting(Shader shader);
void City_Generate(unsigned int seed);
void City_Draw(bool debug); // debug exposes procedural plan diagnostics
// Draws F3-only room labels after EndMode3D, using projected room centres.
void City_DrawDebugLabels(Camera3D camera);
void City_UpdateDoors(float dt);
// Returns the action for the best door under the camera aim, or NULL.
const char *City_DoorPrompt(Camera3D camera);
bool City_InteractDoor(Camera3D camera);
// True for walls and room floor/ceiling slabs. Supports have no collision.
bool City_Collides(float x, float z, float radius, float feetY, float height);
// Merge terrain with reachable procedural slab tops for the player.
float City_GroundHeight(float x, float z, float terrainHeight, float maximumHeight);
float City_CeilingHeight(float x, float z, float minimumHeight);
void City_Unload(void);
int  City_Count(void);
int  City_LargeBuildingCount(void);
unsigned int City_FirstLargeBuildingSeed(void);
