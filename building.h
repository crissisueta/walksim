#pragma once
#include "raylib.h"
#include <string>
#include <vector>

// A procedural building is a graph of axis-aligned local room modules. Room
// transforms are evaluated relative to Building::position before rendering.
// A visual column from the terrain to the underside of a room floor. Supports
// deliberately do not participate in collision; the player can pass through them.
struct BuildingSupport {
    Vector3 position;
    float height;
    float width;
};

enum BuildingType {
    BUILDING_GENERIC
};

// Room templates are data loaded from room_types.txt, not C++ enum values.
// A Room references its template by templateId (index into the registry).

enum SocketType {
    SOCKET_DOOR,
    SOCKET_CORRIDOR,
    SOCKET_WINDOW,
    SOCKET_STAIRS
};

// Sides use the room's unrotated local axes: north (+Z), south (-Z), east
// (+X), west (-X). Room::rotation is applied when window geometry is drawn.
enum RoomSide {
    ROOM_SIDE_NORTH,
    ROOM_SIDE_SOUTH,
    ROOM_SIDE_EAST,
    ROOM_SIDE_WEST
};

// A window opening along one wall. offset is measured from the wall centre;
// bottom and height are measured upward from the room floor underside.
struct RoomWindow {
    RoomSide side;
    float offset;
    float width;
    float bottom;
    float height;
};

// A generic room template loaded from room_types.txt. weight <= 0 means the
// template exists (e.g. the entrance) but is never randomly selected.
// role identifies special templates ("entrance", "normal", "stairs", "utility").
struct RoomDefinition {
    std::string name;
    float width;
    float depth;
    float height;
    SocketType socketType;
    std::vector<RoomWindow> windows;
    float weight;
    std::string assetPath;
    float doorWidth;
    std::string role;
};

struct Socket {
    // Local doorway/boundary midpoint and its outward-facing rotation.
    Vector3 position;
    float rotation;
    SocketType type;
};

struct Room {
    int templateId;
    Vector3 position;
    float rotation;
    float width;
    float depth;
    float height;
    // Zero-based story index. position.y is the slab underside elevation of
    // this floor; rooms on the same floor always share position.y.
    int floor;
    std::vector<Socket> sockets;
    std::vector<RoomWindow> windows;
};

struct BuildingConnection {
    // The paired sockets occupy the same boundary and form one passage.
    int roomA;
    int socketA;
    int roomB;
    int socketB;
};

// A movable panel derived from an existing socket opening. position and hinge
// are in building-local coordinates; rotation faces out through the owning
// room wall. The topology remains owned by rooms/connections above.
struct BuildingDoor {
    Vector3 position;
    Vector3 hingePosition;
    float width;
    float height;
    float rotation;
    float openAngle;
    float targetAngle;
    bool open;
    int roomIndex;
    int socketIndex;
    int openingDirection;
};

struct Building {
    BuildingType type;
    Vector3 position;
    unsigned int seed;
    int entranceRoom;
    int exteriorSocket;
    std::vector<Room> rooms;
    std::vector<BuildingConnection> connections;
    std::vector<BuildingDoor> doors;
    std::vector<BuildingSupport> supports;
};

Building Building_GenerateTestBuilding(unsigned int seed);
// Loads definitions in the simple [room_type] key=value format. A failed load
// leaves the built-in definitions active, so generation always remains usable.
bool Building_LoadRoomDefinitions(const char *path);
int Building_RoomTemplateCount(void);
const RoomDefinition *Building_RoomTemplate(int templateId);
const char *Building_RoomTemplateName(int templateId);
const char *Building_RoomName(const Room &room);
float Building_RoomDoorWidth(const Room &room);
// Call after position and terrain pads are final. This samples terrain once and
// stores the resulting visual supports in the building instance.
void Building_GenerateSupports(Building *building);
// Rebuilds movable panels from the existing connection/exterior doorway data.
// Editors which change room topology can call this after their edit.
void Building_GenerateDoors(Building *building);
void Building_UpdateDoors(Building *building, float dt);
int Building_FindDoor(const Building &building, Camera3D camera, float maxDistance,
                      float *score);
void Building_ToggleDoor(Building *building, int doorIndex);
// Checks grid alignment, overlap, socket pairing, and graph connectivity.
bool Building_ValidatePlan(const Building &building);
// Rendering and collision consume the generated plan; neither changes it.
void Building_Draw(const Building &building, bool debug);
bool Building_Collides(const Building &building, float x, float z,
                       float radius, float feetY, float height);
// Returns room slab tops that are reachable from maximumHeight, or currentGround.
float Building_FloorHeight(const Building &building, float x, float z,
                           float currentGround, float maximumHeight);
// Returns the nearest ceiling above minimumHeight, or FLT_MAX if none exists.
float Building_CeilingHeight(const Building &building, float x, float z,
                             float minimumHeight);
const char *Building_ModuleAssetPath(int templateId);

// Guide-compatible alias and headless validation entry point.
Building Building_Generate(unsigned int seed);
int Building_RunSeedCheck(unsigned int count);
// Headless gameplay test: walks a scripted player up and down every stair
// link with the real movement physics. Returns the number of failed links.
int Building_RunStairWalkCheck(unsigned int count);
