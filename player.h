#pragma once
#include "raylib.h"

struct Player {
    Vector3 pos;    // position of the feet
    float   velY;   // vertical velocity (for gravity)
    float   yaw;    // left/right look angle, radians
    float   pitch;  // up/down look angle, radians
    bool    crouching;
};

// Settings the in-game panel can change. Defaults live in Player_ResetParams().
struct PlayerParams {
    float walkSpeed;    // m/s
    float sprintSpeed;  // m/s (hold Left Shift)
};
extern PlayerParams g_playerParams;

void     Player_ResetParams(void);
void     Player_Init(Player *p, float x, float z);
// controls=false ignores mouse and keyboard (gravity and ground following still run).
void     Player_Update(Player *p, float dt, bool controls);
Camera3D Player_GetCamera(const Player *p);
