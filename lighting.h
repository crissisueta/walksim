#pragma once
#include "raylib.h"

// raylib's default shader is unlit, so building models would look flat.
// This is a tiny shader (sun + ambient, same sun direction as the terrain)
// that we attach to loaded models.

Shader Lighting_LoadShader(void);                 // call once, after InitWindow
void   Lighting_Attach(Model *m, Shader shader);  // use this shader on all materials of m
void   Lighting_Detach(Model *m);                 // call BEFORE UnloadModel (model must not own the shader)
