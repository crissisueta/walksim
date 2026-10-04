#include "raylib.h"
#include "terrain.h"
#include "building.h"
#include "player.h"
#include "world.h"
#include "vegetation.h"
#include "config.h"
#include "viewer.h"
#include "lighting.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Application lifecycle: initialize world services, run the frame loop, then
// release GPU resources in the reverse order of their dependencies.
int main(int argc, char **argv)
{
    // World seed:   ./game 839271
    // Model viewer: ./game --view assets/house_01.glb
    // Headless self-test: ./game --check 20000
    // Headless plan dump:  ./game --seedinfo 13
    // Headless stair walk: ./game --stairwalk 2000
    unsigned int seed = 839271;
    const char *viewPath = NULL;
    if (argc > 1 && strcmp(argv[1], "--check") == 0) {
        unsigned int count = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 20000u;
        int failures = Building_RunSeedCheck(count);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "--stairwalk") == 0) {
        unsigned int count = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 2000u;
        int failures = Building_RunStairWalkCheck(count);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 2 && strcmp(argv[1], "--seedinfo") == 0) {
        unsigned int planSeed = (unsigned int)strtoul(argv[2], NULL, 10);
        Building building = Building_Generate(planSeed);
        bool valid = Building_ValidatePlan(building);
        int floors = 1;
        for (size_t i = 0; i < building.rooms.size(); i++)
            if (building.rooms[i].floor + 1 > floors) floors = building.rooms[i].floor + 1;
        printf("seed %u: valid %d rooms %zu floors %d connections %zu doors %zu\n",
               planSeed, valid ? 1 : 0, building.rooms.size(), floors,
               building.connections.size(), building.doors.size());
        for (size_t i = 0; i < building.rooms.size(); i++) {
            const Room &room = building.rooms[i];
            printf("  room %zu: floor %d y %+.2f template '%s' pos (%.1f, %.1f) rot %.0f size %.0fx%.0fx%.0f\n",
                   i, room.floor, room.position.y, Building_RoomName(room),
                   room.position.x, room.position.z, room.rotation,
                   room.width, room.depth, room.height);
        }
        return valid ? 0 : 1;
    }
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
    Terrain_Unload();
    World_Unload();
    UnloadShader(lighting);
    CloseWindow();
    return 0;
}
