#pragma once
#include "raylib.h"
#include <vector>

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

struct Socket {
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
    std::vector<Socket> sockets;
};

struct BuildingConnection {
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
};

Building Building_GenerateTestBuilding(unsigned int seed);
bool Building_ValidatePlan(const Building &building);
void Building_Draw(const Building &building, bool debug);
bool Building_Collides(const Building &building, float x, float z,
                       float radius, float feetY, float height);
const char *Building_ModuleAssetPath(RoomType type);