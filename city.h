#pragma once
#include "raylib.h"

void City_Init(unsigned int seed);
void City_Generate(unsigned int seed);
void City_Draw(bool debug);
bool City_Collides(float x, float z, float radius, float feetY, float height);
void City_Unload(void);
int  City_Count(void);
int  City_LargeBuildingCount(void);
unsigned int City_FirstLargeBuildingSeed(void);