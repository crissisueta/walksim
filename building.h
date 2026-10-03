#pragma once
#include "raylib.h"
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

enum RoomType {
    ROOM_ENTRANCE,
    ROOM_HALLWAY,
    ROOM_ROOM,
    ROOM_BATHROOM
};

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

struct Socket {
    // Local doorway/boundary midpoint and its outward-facing rotation.
    Vector3 position;
    float rotation;
    SocketType type;
};

struct Room {
    RoomType type;
    Vector3 position;
    float rotation;
    int width;
    int depth;
    float height;
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

struct Building {
    BuildingType type;
    Vector3 position;
    unsigned int seed;
    int entranceRoom;
    int exteriorSocket;
    std::vector<Room> rooms;
    std::vector<BuildingConnection> connections;
    std::vector<BuildingSupport> supports;
};

Building Building_GenerateTestBuilding(unsigned int seed);
// Loads definitions in the simple [room_type] key=value format. A failed load
// leaves the built-in definitions active, so generation always remains usable.
bool Building_LoadRoomDefinitions(const char *path);
const char *Building_RoomTypeName(RoomType type);
// Call after position and terrain pads are final. This samples terrain once and
// stores the resulting visual supports in the building instance.
void Building_GenerateSupports(Building *building);
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
const char *Building_ModuleAssetPath(RoomType type);
