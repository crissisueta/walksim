#include "config.h"
#include "terrain.h"
#include "player.h"
#include "raylib.h"

// ---------------------------------------------------------------------------
// Tiny immediate-mode UI: a slider is just a function you call every frame.
// ---------------------------------------------------------------------------

static int g_nextId = 0;    // reset every frame; gives each slider a stable id
static int g_active = -1;   // id of the slider being dragged, or -1

// Draws one slider and updates *value while it's dragged. Returns true if the value changed.
static bool Slider(const char *label, float *value, float lo, float hi, int decimals,
                   float x, float y, float w)
{
    int id = g_nextId++;
    Vector2 m = GetMousePosition();

    Rectangle hit = { x, y + 14, w, 16 };
    if (CheckCollisionPointRec(m, hit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        g_active = id;

    bool changed = false;
    if (g_active == id) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float f = (m.x - x) / w;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            float nv = lo + f * (hi - lo);
            if (nv != *value) { *value = nv; changed = true; }
        } else {
            g_active = -1;   // released
        }
    }

    float t = (*value - lo) / (hi - lo);
    DrawText(TextFormat("%s: %.*f", label, decimals, *value), (int)x, (int)y, 14, WHITE);
    DrawRectangle((int)x, (int)y + 19, (int)w, 6, Color{ 70, 70, 70, 255 });
    DrawRectangle((int)x, (int)y + 19, (int)(w * t), 6, Color{ 120, 190, 60, 255 });
    DrawCircle((int)(x + w * t), (int)y + 22, 7, WHITE);
    return changed;
}

static void Heading(const char *text, int x, int y)
{
    DrawText(text, x, y, 16, Color{ 255, 220, 120, 255 });
}

static void Swatch(const float rgb[3], int x, int y)
{
    DrawRectangle(x, y, 40, 16, Color{ (unsigned char)rgb[0], (unsigned char)rgb[1], (unsigned char)rgb[2], 255 });
    DrawRectangleLines(x, y, 40, 16, WHITE);
}

// ---------------------------------------------------------------------------

bool Config_Draw(void)
{
    g_nextId = 0;
    bool terrainChanged = false;

    const int px = 10, py = 40, pw = 340, pad = 14;
    const int x = px + pad;
    const int w = pw - 2 * pad;
    int y = py + pad;

    DrawRectangle(px, py, pw, 600, Fade(BLACK, 0.7f));

    DrawText("Settings  (Tab to close)", x, y, 16, WHITE);
    y += 26;

    // --- Terrain shape ---
    Heading("Terrain", x, y);
    y += 22;

    static const char *ampName[3]  = { "Big hills height",   "Medium bumps height",   "Small ripples height" };
    static const char *sizeName[3] = { "Big hills size",     "Medium bumps size",     "Small ripples size" };
    static const float ampMax[3]   = { 60.0f, 30.0f, 5.0f };
    static const float sizeMin[3]  = { 80.0f, 30.0f, 8.0f };
    static const float sizeMax[3]  = { 600.0f, 250.0f, 80.0f };

    // Each octave has a height slider and a size slider (6 sliders total).
    // Order here matches the layout height used for the panel background.
    for (int i = 0; i < 3; i++) {
        if (Slider(ampName[i],  &g_terrainParams.amp[i],        0.0f,       ampMax[i],  1, x, y, w)) terrainChanged = true;
        y += 30;
        if (Slider(sizeName[i], &g_terrainParams.wavelength[i], sizeMin[i], sizeMax[i], 0, x, y, w)) terrainChanged = true;
        y += 30;
    }

    // --- Grass colours ---
    static const char *rgb[3] = { "R", "G", "B" };

    Heading("Grass colour: valleys", x, y);
    Swatch(g_terrainParams.valley, x + w - 40, y);
    y += 22;
    for (int i = 0; i < 3; i++) {
        if (Slider(rgb[i], &g_terrainParams.valley[i], 0.0f, 255.0f, 0, x, y, w)) terrainChanged = true;
        y += 30;
    }

    Heading("Grass colour: hilltops", x, y);
    Swatch(g_terrainParams.hill, x + w - 40, y);
    y += 22;
    for (int i = 0; i < 3; i++) {
        if (Slider(rgb[i], &g_terrainParams.hill[i], 0.0f, 255.0f, 0, x, y, w)) terrainChanged = true;
        y += 30;
    }

    // --- Player ---
    Heading("Player", x, y);
    y += 22;
    Slider("Walk speed (m/s)",   &g_playerParams.walkSpeed,   1.0f, 30.0f, 1, x, y, w);
    y += 30;
    Slider("Sprint speed (m/s)", &g_playerParams.sprintSpeed, 2.0f, 60.0f, 1, x, y, w);
    y += 40;

    // --- Reset button ---
    Rectangle br = { (float)x, (float)y, (float)w, 28.0f };
    bool hover = CheckCollisionPointRec(GetMousePosition(), br);
    DrawRectangleRec(br, hover ? Color{ 110, 110, 110, 255 } : Color{ 80, 80, 80, 255 });
    DrawText("Reset to defaults", x + 10, y + 6, 16, WHITE);
    if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Terrain_ResetParams();
        Player_ResetParams();
        terrainChanged = true;
    }

    return terrainChanged;
}
