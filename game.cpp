#include "raylib.h"
#include "terrain.h"
#include "player.h"
#include "world.h"
#include "vegetation.h"
#include "config.h"
#include "viewer.h"
#include "lighting.h"
#include <stdlib.h>
#include <string.h>

// Application lifecycle: initialize world services, run the frame loop, then
// release GPU resources in the reverse order of their dependencies.
int main(int argc, char **argv)
{
    // World seed:   ./game 839271
    // Model viewer: ./game --view assets/house_01.glb
    unsigned int seed = 839271;
    const char *viewPath = NULL;
    if (argc > 2 && strcmp(argv[1], "--view") == 0) viewPath = argv[2];
    else if (argc > 1) seed = (unsigned int)strtoul(argv[1], NULL, 10);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "walksim");
    SetTargetFPS(60);
    DisableCursor();   // capture the mouse for looking around

    if (viewPath) {    // viewer mode: look at one model, then quit
        Viewer_Run(viewPath);
        CloseWindow();
        return 0;
    }

    Terrain_Init(seed);
    Shader lighting = Lighting_LoadShader();
    World_Init(seed);
    World_SetLighting(lighting);
    Model terrain = Terrain_BuildModel();

    Player player;
    Player_Init(&player, 0.0f, 0.0f);

    bool panelOpen       = false;  // Tab toggles the settings panel
    bool buildingDebug   = false;
    Config_Load(&panelOpen, &buildingDebug);
    if (panelOpen) EnableCursor();
    bool rebuildTerrain  = false;  // set by the panel when a terrain setting changes
    int  skipMouseFrames = 2;      // ignore the mouse briefly after capturing it (avoids a view jump)

    while (!WindowShouldClose()) {          // ESC quits
        float dt = GetFrameTime();
        if (dt > 0.05f) dt = 0.05f;         // avoid huge steps after a hitch

        if (IsKeyPressed(KEY_TAB)) {
            panelOpen = !panelOpen;
            if (panelOpen) {
                EnableCursor();             // free the mouse to use the sliders
            } else {
                DisableCursor();            // back to mouse look
                skipMouseFrames = 2;
            }
            Config_Save(panelOpen, buildingDebug);
        }
        if (IsKeyPressed(KEY_F3)) {
            buildingDebug = !buildingDebug;
            Config_Save(panelOpen, buildingDebug);
        }

        if (rebuildTerrain) {
            World_Generate(seed);
            UnloadModel(terrain);
            terrain = Terrain_BuildModel();
            rebuildTerrain = false;
        }

        // While the panel is open the player ignores input but still follows the ground,
        // so you don't end up inside a hill you just made taller.
        bool controls = !panelOpen && skipMouseFrames == 0;
        if (!panelOpen && skipMouseFrames > 0) skipMouseFrames--;
        World_UpdateDoors(dt);
        Player_Update(&player, dt, controls);
        Camera3D cam = Player_GetCamera(&player);
        if (controls && IsKeyPressed(KEY_E)) World_InteractDoor(cam);
        const char *doorPrompt = controls ? World_DoorPrompt(cam) : NULL;

        BeginDrawing();
            ClearBackground(SKYBLUE);       // also clears the depth buffer
            DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(),
                                   Color{ 60, 120, 210, 255 },     // top
                                   Color{ 185, 218, 245, 255 });   // bottom

            BeginMode3D(cam);
                DrawModel(terrain, Vector3{ 0, 0, 0 }, 1.0f, WHITE);
                World_Draw(buildingDebug);
            EndMode3D();

            if (buildingDebug) World_DrawDebugLabels(cam);

            DrawText(TextFormat("seed %u   %d buildings   %d trees   %d flower groups   %d grass clumps   %d fps",
                                 seed, World_BuildingCount(), Vegetation_TreeCount(),
                                 Vegetation_FlowerCount(), Vegetation_GrassCount(), GetFPS()),
                     10, 10, 20, WHITE);
            DrawText("WASD move   Space jump   Ctrl/C crouch   Shift sprint   E interact   [Tab] settings   [F3] building debug",
                     10, 34, 16, WHITE);
            if (buildingDebug)
                DrawText(TextFormat("M3 debug   first building seed %u   %d trees   %d flowers   %d grass",
                                    World_FirstBuildingSeed(), Vegetation_TreeCount(),
                                    Vegetation_FlowerCount(), Vegetation_GrassCount()),
                         10, 54, 16, WHITE);
            if (doorPrompt)
                DrawText(doorPrompt, GetScreenWidth() / 2 - MeasureText(doorPrompt, 20) / 2,
                         GetScreenHeight() / 2 + 36, 20, WHITE);
            if (panelOpen && Config_Draw()) rebuildTerrain = true;
            if (Config_ConsumeChanged()) Config_Save(panelOpen, buildingDebug);
        EndDrawing();
    }

    UnloadModel(terrain);
    World_Unload();
    UnloadShader(lighting);
    CloseWindow();
    return 0;
}
