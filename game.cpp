#include "raylib.h"
#include "terrain.h"
#include "player.h"
#include "city.h"
#include "config.h"
#include "viewer.h"
#include <stdlib.h>
#include <string.h>

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
    Model terrain = Terrain_BuildModel();
    City_Init(seed);

    Player player;
    Player_Init(&player, 0.0f, 0.0f);

    bool panelOpen       = false;  // Tab toggles the settings panel
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
        }

        if (rebuildTerrain) {
            UnloadModel(terrain);
            terrain = Terrain_BuildModel();
            City_Generate(seed);
            rebuildTerrain = false;
        }

        // While the panel is open the player ignores input but still follows the ground,
        // so you don't end up inside a hill you just made taller.
        bool controls = !panelOpen && skipMouseFrames == 0;
        if (!panelOpen && skipMouseFrames > 0) skipMouseFrames--;
        Player_Update(&player, dt, controls);
        Camera3D cam = Player_GetCamera(&player);

        BeginDrawing();
            ClearBackground(SKYBLUE);       // also clears the depth buffer
            DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(),
                                   Color{ 60, 120, 210, 255 },     // top
                                   Color{ 185, 218, 245, 255 });   // bottom

            BeginMode3D(cam);
                DrawModel(terrain, Vector3{ 0, 0, 0 }, 1.0f, WHITE);
                City_Draw();
            EndMode3D();

            DrawText(TextFormat("seed %u   %d settlements   %d fps   [Tab] settings",
                                 seed, City_Count(), GetFPS()), 10, 10, 20, WHITE);
            if (panelOpen && Config_Draw()) rebuildTerrain = true;
        EndDrawing();
    }

    UnloadModel(terrain);
    City_Unload();
    CloseWindow();
    return 0;
}
