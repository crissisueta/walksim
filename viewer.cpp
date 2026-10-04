#include "viewer.h"
#include "terrain.h"
#include "player.h"
#include "lighting.h"
#include "raylib.h"
#include "rlgl.h"
#include <math.h>

// Standalone inspection mode for checking imported models before world use.
static bool LoadInto(Model *model, const char *path, Shader shader)
{
    *model = LoadModel(path);
    if (model->meshCount == 0) return false;
    Lighting_Attach(model, shader);
    return true;
}

void Viewer_Run(const char *path)
{
    // Flat ground: reuse the terrain code with all hill heights set to zero.
    Terrain_Init(0);
    for (int i = 0; i < 3; i++) g_terrainParams.amp[i] = 0.0f;
    Model ground = Terrain_BuildModel();

    Shader shader = Lighting_LoadShader();
    Model model;
    bool ok = LoadInto(&model, path, shader);
    BoundingBox bb = ok ? GetModelBoundingBox(model)
                        : BoundingBox{ Vector3{ -1, 0, -1 }, Vector3{ 1, 1, 1 } };

    // Start 10 m in front of the model, looking at it.
    Player player;
    Player_Init(&player, 0.0f, bb.min.z - 10.0f);
    int skipMouseFrames = 2;
    bool cull = true;   // C toggles backface culling (diagnostic for missing faces)

    while (!WindowShouldClose()) {          // ESC quits
        float dt = GetFrameTime();
        if (dt > 0.05f) dt = 0.05f;

        // R reloads the file, so you can re-export from Blender and look again.
        if (IsKeyPressed(KEY_R)) {
            Lighting_Detach(&model);
            UnloadModel(model);
            ok = LoadInto(&model, path, shader);
            if (ok) bb = GetModelBoundingBox(model);
        }

        if (IsKeyPressed(KEY_C)) cull = !cull;

        Player_Update(&player, dt, skipMouseFrames == 0);
        if (skipMouseFrames > 0) skipMouseFrames--;
        Camera3D cam = Player_GetCamera(&player);

        float sx = bb.max.x - bb.min.x;
        float sy = bb.max.y - bb.min.y;
        float sz = bb.max.z - bb.min.z;
        float cx = (bb.min.x + bb.max.x) * 0.5f;
        float cz = (bb.min.z + bb.max.z) * 0.5f;

        BeginDrawing();
            ClearBackground(SKYBLUE);
            DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(),
                                   Color{ 60, 120, 210, 255 }, Color{ 185, 218, 245, 255 });

            BeginMode3D(cam);
                DrawModel(ground, Vector3{ 0, 0, 0 }, 1.0f, WHITE);

                rlPushMatrix();                       // 1 m grid, lifted slightly to avoid z-fighting
                    rlTranslatef(0.0f, 0.03f, 0.0f);
                    DrawGrid(40, 1.0f);
                rlPopMatrix();

                if (ok) {
                    if (!cull) rlDisableBackfaceCulling();   // show faces from both sides
                    DrawModel(model, Vector3{ 0, 0, 0 }, 1.0f, WHITE);
                    rlEnableBackfaceCulling();
                }

                // 1.7 m tall "person" next to the model, for scale.
                DrawCylinder(Vector3{ bb.max.x + 2.0f, 0.0f, 0.0f }, 0.25f, 0.25f, 1.7f, 16, ORANGE);
            EndMode3D();

            DrawText(TextFormat("Viewing: %s", path), 10, 10, 20, WHITE);
            if (ok) {
                DrawText(TextFormat("Size: %.1f x %.1f x %.1f m  (width x height x depth)", sx, sy, sz), 10, 36, 18, WHITE);
                DrawText(TextFormat("Base height: %.2f m    Footprint centre: x %.2f, z %.2f", bb.min.y, cx, cz), 10, 58, 18, WHITE);
                if (fabsf(bb.min.y) > 0.05f || fabsf(cx) > 0.5f || fabsf(cz) > 0.5f)
                    DrawText("Origin should be at the centre of the footprint, at ground level (base height 0)", 10, 80, 18, ORANGE);
            } else {
                DrawText("Could not load this model. See the terminal for the reason.", 10, 36, 18, RED);
            }
            DrawText(TextFormat("WASD move   Shift sprint   R reload   C backface culling: %s   Esc quit   (orange post = 1.7 m)",
                                cull ? "ON" : "OFF"),
                     10, GetScreenHeight() - 28, 18, WHITE);
        EndDrawing();
    }

    Lighting_Detach(&model);
    UnloadModel(model);
    UnloadShader(shader);
    UnloadModel(ground);
    Terrain_Unload();
}
