#pragma once

struct VegetationAvoidArea {
    float x;
    float z;
    float halfWidth;
    float halfDepth;
};

void Vegetation_Init(void);
void Vegetation_Generate(unsigned int seed, const VegetationAvoidArea *avoided, int avoidedCount);
void Vegetation_Draw(void);
bool Vegetation_TreeCollides(float x, float z, float radius);
void Vegetation_Unload(void);
int Vegetation_TreeCount(void);
int Vegetation_FlowerCount(void);
int Vegetation_GrassCount(void);
