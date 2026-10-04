#pragma once
#include "raylib.h"

// World owns standalone procedural buildings and vegetation. Call World_Init
// after Terrain_Init; World_Unload releases vegetation texture resources.
void World_Init(unsigned int seed);
void World_SetLighting(Shader shader);
void World_Generate(unsigned int seed);
void World_Draw(bool debug);
void World_DrawDebugLabels(Camera3D camera);
void World_UpdateDoors(float dt);
const char *World_DoorPrompt(Camera3D camera);
bool World_InteractDoor(Camera3D camera);
bool World_Collides(float x, float z, float radius, float feetY, float height);
float World_GroundHeight(float x, float z, float terrainHeight, float maximumHeight);
float World_CeilingHeight(float x, float z, float minimumHeight);
void World_Unload(void);
int World_BuildingCount(void);
unsigned int World_FirstBuildingSeed(void);
