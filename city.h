#pragma once
#include "raylib.h"

void City_Init(unsigned int seed);
void City_Generate(unsigned int seed);
void City_Draw(void);
void City_Unload(void);
int  City_Count(void);