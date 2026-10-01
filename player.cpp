#include "player.h"
#include "terrain.h"
#include <math.h>

// Tweak these freely.
static const float EYE_HEIGHT  = 1.7f;    // metres above the feet
static const float GRAVITY     = 30.0f;   // m/s^2
static const float SNAP_DIST   = 0.3f;    // stick to ground within this distance (walking downhill)
static const float MOUSE_SENS  = 0.0025f; // radians per pixel

PlayerParams g_playerParams;

void Player_ResetParams(void)
{
    g_playerParams.walkSpeed   = 6.0f;
    g_playerParams.sprintSpeed = 16.0f;
}

void Player_Init(Player *p, float x, float z)
{
    Player_ResetParams();
    p->pos   = Vector3{ x, Terrain_Height(x, z), z };
    p->velY  = 0.0f;
    p->yaw   = 0.0f;   // 0 = looking toward +Z
    p->pitch = 0.0f;
}

void Player_Update(Player *p, float dt, bool controls)
{
    // --- Mouse look ---
    if (controls) {
        Vector2 md = GetMouseDelta();
        p->yaw   -= md.x * MOUSE_SENS;   // mouse right -> turn right
        p->pitch -= md.y * MOUSE_SENS;   // mouse down  -> look down
        const float lim = 89.0f * DEG2RAD;
        if (p->pitch >  lim) p->pitch =  lim;
        if (p->pitch < -lim) p->pitch = -lim;
    }

    // --- WASD, relative to where we're facing (flat on the ground plane) ---
    float fx = sinf(p->yaw),  fz = cosf(p->yaw);    // forward
    float rx = -cosf(p->yaw), rz = sinf(p->yaw);    // right

    float mx = 0.0f, mz = 0.0f;
    if (controls) {
        if (IsKeyDown(KEY_W)) { mx += fx; mz += fz; }
        if (IsKeyDown(KEY_S)) { mx -= fx; mz -= fz; }
        if (IsKeyDown(KEY_D)) { mx += rx; mz += rz; }
        if (IsKeyDown(KEY_A)) { mx -= rx; mz -= rz; }
    }

    float len = sqrtf(mx * mx + mz * mz);
    if (len > 0.0f) { mx /= len; mz /= len; }       // no faster diagonals

    float speed = IsKeyDown(KEY_LEFT_SHIFT) ? g_playerParams.sprintSpeed
                                            : g_playerParams.walkSpeed;
    p->pos.x += mx * speed * dt;
    p->pos.z += mz * speed * dt;

    // Stay inside the terrain mesh.
    const float limit = TERRAIN_SIZE * 0.5f - 5.0f;
    if (p->pos.x >  limit) p->pos.x =  limit;
    if (p->pos.x < -limit) p->pos.x = -limit;
    if (p->pos.z >  limit) p->pos.z =  limit;
    if (p->pos.z < -limit) p->pos.z = -limit;

    // --- Gravity + ground following ---
    float ground = Terrain_Height(p->pos.x, p->pos.z);
    p->velY  -= GRAVITY * dt;
    p->pos.y += p->velY * dt;
    if (p->pos.y <= ground + SNAP_DIST && p->velY <= 0.0f) {
        p->pos.y = ground;   // on the ground (also handles walking up and down slopes)
        p->velY  = 0.0f;
    }
}

Camera3D Player_GetCamera(const Player *p)
{
    Vector3 eye = { p->pos.x, p->pos.y + EYE_HEIGHT, p->pos.z };
    Vector3 dir = {
        cosf(p->pitch) * sinf(p->yaw),
        sinf(p->pitch),
        cosf(p->pitch) * cosf(p->yaw)
    };

    Camera3D cam = {};
    cam.position   = eye;
    cam.target     = Vector3{ eye.x + dir.x, eye.y + dir.y, eye.z + dir.z };
    cam.up         = Vector3{ 0.0f, 1.0f, 0.0f };
    cam.fovy       = 70.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    return cam;
}
