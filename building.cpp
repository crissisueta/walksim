#include "building.h"
#include "terrain.h"
#include "rlgl.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// This module owns the deterministic room-plan generator and all geometry
// derived from it. The support pass is deliberately separate from topology.
static const float GRID_SIZE = 2.5f;
static const float WALL_THICKNESS = 0.2f;
static const float FLOOR_THICKNESS = 0.16f;
static const float CEILING_THICKNESS = 0.16f;
static const float DOOR_WIDTH = 1.6f;
static const float DOOR_HEIGHT = 2.2f;
static const float BUILDING_LIMIT = 35.0f;
static const int WALL_PIECE_LIMIT = 64;
static const float BUILDING_SUPPORT_MIN_HEIGHT = 0.25f;
static const float BUILDING_SUPPORT_DUPLICATE_DISTANCE = 0.75f;
static const float BUILDING_SUPPORT_WIDTH = 0.35f;
static const float BUILDING_STEP_HEIGHT = 0.30f;
static const float DOOR_PANEL_THICKNESS = 0.075f;
static const float DOOR_FRAME_CLEARANCE = 0.10f;
static const float DOOR_OPEN_ANGLE = 92.0f;
static const float DOOR_ANGULAR_SPEED = 220.0f;

// Room templates intentionally use a tiny INI-like format instead of a
// general serialization system. The generator only needs dimensions and the
// socket category; socket positions are still derived from the room rectangle.
static std::vector<RoomDefinition> g_roomDefinitions;
static bool g_roomDefinitionsInitialized = false;

static void SetDefaultRoomDefinitions(std::vector<RoomDefinition> *definitions)
{
    definitions->clear();
    RoomDefinition entrance;
    entrance.name = "entrance";
    entrance.width = 10.0f; entrance.depth = 10.0f; entrance.height = 3.0f;
    entrance.socketType = SOCKET_DOOR;
    entrance.weight = 0.0f;
    entrance.doorWidth = DOOR_WIDTH;
    entrance.role = "entrance";
    entrance.windows.push_back(RoomWindow{ ROOM_SIDE_EAST, 0.0f, 2.4f, 1.0f, 1.2f });
    definitions->push_back(entrance);
    RoomDefinition hallway;
    hallway.name = "hallway";
    hallway.width = 5.0f; hallway.depth = 10.0f; hallway.height = 3.0f;
    hallway.socketType = SOCKET_CORRIDOR;
    hallway.weight = 22.0f;
    hallway.doorWidth = DOOR_WIDTH;
    hallway.role = "normal";
    definitions->push_back(hallway);
    RoomDefinition room;
    room.name = "room";
    room.width = 10.0f; room.depth = 10.0f; room.height = 4.0f;
    room.socketType = SOCKET_DOOR;
    room.weight = 62.0f;
    room.doorWidth = DOOR_WIDTH;
    room.role = "normal";
    room.windows.push_back(RoomWindow{ ROOM_SIDE_NORTH, 0.0f, 2.6f, 1.0f, 1.2f });
    room.windows.push_back(RoomWindow{ ROOM_SIDE_EAST, 0.0f, 2.6f, 1.0f, 1.2f });
    definitions->push_back(room);
    RoomDefinition bathroom;
    bathroom.name = "bathroom";
    bathroom.width = 5.0f; bathroom.depth = 5.0f; bathroom.height = 3.0f;
    bathroom.socketType = SOCKET_DOOR;
    bathroom.weight = 16.0f;
    bathroom.doorWidth = 1.2f;
    bathroom.role = "utility";
    bathroom.windows.push_back(RoomWindow{ ROOM_SIDE_WEST, 0.0f, 1.4f, 1.3f, 0.9f });
    definitions->push_back(bathroom);
}

static char *Trim(char *text)
{
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static bool ParsePositiveFloat(const char *text, float *value)
{
    char *end = NULL;
    float parsed = strtof(text, &end);
    if (!end || *Trim(end) || !(parsed > 0.0f) || parsed > 1000.0f) return false;
    *value = parsed;
    return true;
}

static bool ParseSocket(const char *text, SocketType *socket)
{
    if (strcmp(text, "door") == 0) *socket = SOCKET_DOOR;
    else if (strcmp(text, "corridor") == 0) *socket = SOCKET_CORRIDOR;
    else if (strcmp(text, "window") == 0) *socket = SOCKET_WINDOW;
    else if (strcmp(text, "stairs") == 0) *socket = SOCKET_STAIRS;
    else return false;
    return true;
}

static int RoomSideFromName(const char *name)
{
    static const char *names[] = { "north", "south", "east", "west" };
    for (int side = 0; side < 4; side++)
        if (strcmp(name, names[side]) == 0) return side;
    return -1;
}

// window=side,offset,width,bottom,height; parsing it here keeps the external
// format deliberately small while allowing several window lines per template.
static bool ParseWindow(const char *text, RoomWindow *window)
{
    char side[16] = {};
    if (sscanf(text, " %15[^,],%f,%f,%f,%f", side, &window->offset, &window->width,
               &window->bottom, &window->height) != 5) return false;
    int parsedSide = RoomSideFromName(Trim(side));
    if (parsedSide < 0 || window->width <= 0.0f || window->height <= 0.0f ||
        window->bottom < FLOOR_THICKNESS) return false;
    window->side = (RoomSide)parsedSide;
    return true;
}

static int FindTemplateByName(const std::vector<RoomDefinition> &defs, const std::string &name)
{
    for (size_t i = 0; i < defs.size(); i++) if (defs[i].name == name) return (int)i;
    return -1;
}

static int FindTemplateByRole(const std::vector<RoomDefinition> &defs, const char *role)
{
    for (size_t i = 0; i < defs.size(); i++) if (defs[i].role == role) return (int)i;
    return -1;
}

bool Building_LoadRoomDefinitions(const char *path)
{
    std::vector<RoomDefinition> parsed;
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Building definitions: could not open %s; using built-in defaults.\n", path);
        g_roomDefinitionsInitialized = true;
        SetDefaultRoomDefinitions(&g_roomDefinitions);
        return false;
    }

    RoomDefinition current;
    bool inSection = false;
    bool sectionValid = true;
    bool seenScalar[8] = {};
    int lineNumber = 0;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        lineNumber++;
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *text = Trim(line);
        if (!*text) continue;

        size_t length = strlen(text);
        if (text[0] == '[' && length > 2 && text[length - 1] == ']') {
            if (inSection && sectionValid) parsed.push_back(current);
            text[length - 1] = '\0';
            std::string name = Trim(text + 1);
            if (name.empty() || FindTemplateByName(parsed, name) >= 0) {
                fprintf(stderr, "Building definitions:%d: duplicate or empty section [%s]; ignoring it.\n",
                        lineNumber, name.c_str());
                inSection = false;
                sectionValid = false;
            } else {
                current = RoomDefinition();
                current.name = name;
                current.socketType = SOCKET_DOOR;
                current.weight = 1.0f;
                current.doorWidth = DOOR_WIDTH;
                current.role = "normal";
                memset(seenScalar, 0, sizeof(seenScalar));
                inSection = true;
                sectionValid = true;
            }
            continue;
        }

        char *equals = strchr(text, '=');
        if (!inSection || !equals) {
            fprintf(stderr, "Building definitions:%d: expected [section] or key=value.\n", lineNumber);
            sectionValid = false;
            continue;
        }
        *equals = '\0';
        char *key = Trim(text);
        char *value = Trim(equals + 1);
        if (!strcmp(key, "width") || !strcmp(key, "depth") ||
            !strcmp(key, "height") || !strcmp(key, "socket") ||
            !strcmp(key, "weight") || !strcmp(key, "asset") ||
            !strcmp(key, "door_width") || !strcmp(key, "role")) {
            int slot = !strcmp(key, "width") ? 0 : !strcmp(key, "depth") ? 1 :
                       !strcmp(key, "height") ? 2 : !strcmp(key, "socket") ? 3 :
                       !strcmp(key, "weight") ? 4 : !strcmp(key, "asset") ? 5 :
                       !strcmp(key, "door_width") ? 6 : 7;
            if (seenScalar[slot])
                fprintf(stderr, "Building definitions:%d: duplicate '%s', using the last value.\n",
                        lineNumber, key);
            seenScalar[slot] = true;
        }
        if (strcmp(key, "width") == 0) {
            if (!ParsePositiveFloat(value, &current.width)) {
                fprintf(stderr, "Building definitions:%d: invalid width value '%s'.\n", lineNumber, value);
                sectionValid = false;
            }
        } else if (strcmp(key, "depth") == 0) {
            if (!ParsePositiveFloat(value, &current.depth)) {
                fprintf(stderr, "Building definitions:%d: invalid depth value '%s'.\n", lineNumber, value);
                sectionValid = false;
            }
        } else if (strcmp(key, "height") == 0) {
            if (!ParsePositiveFloat(value, &current.height)) {
                fprintf(stderr, "Building definitions:%d: invalid height value '%s'.\n", lineNumber, value);
                sectionValid = false;
            }
        } else if (strcmp(key, "socket") == 0) {
            if (!ParseSocket(value, &current.socketType)) {
                fprintf(stderr, "Building definitions:%d: socket must be door, corridor, window, or stairs.\n",
                        lineNumber);
                sectionValid = false;
            }
        } else if (strcmp(key, "weight") == 0) {
            char *end = NULL;
            float parsed = strtof(value, &end);
            if (!end || *Trim(end)) {
                fprintf(stderr, "Building definitions:%d: invalid weight value '%s'.\n", lineNumber, value);
                sectionValid = false;
            } else current.weight = parsed;
        } else if (strcmp(key, "asset") == 0) {
            current.assetPath = value;
        } else if (strcmp(key, "door_width") == 0) {
            if (!ParsePositiveFloat(value, &current.doorWidth)) {
                fprintf(stderr, "Building definitions:%d: invalid door_width value '%s'.\n", lineNumber, value);
                sectionValid = false;
            }
        } else if (strcmp(key, "role") == 0) {
            if (strcmp(value, "entrance") && strcmp(value, "normal") &&
                strcmp(value, "stairs") && strcmp(value, "utility"))
                fprintf(stderr, "Building definitions:%d: unknown role '%s'.\n", lineNumber, value);
            current.role = value;
        } else if (strcmp(key, "window") == 0) {
            RoomWindow window = {};
            if (ParseWindow(value, &window)) current.windows.push_back(window);
            else fprintf(stderr, "Building definitions:%d: ignoring invalid window '%s'.\n", lineNumber, value);
        } else {
            fprintf(stderr, "Building definitions:%d: unknown key '%s'.\n", lineNumber, key);
        }
    }
    fclose(file);
    if (inSection && sectionValid) parsed.push_back(current);

    // Malformed templates are dropped; out-of-bounds windows are dropped
    // individually. Require at least one entrance so generation stays usable.
    std::vector<RoomDefinition> validDefs;
    for (size_t i = 0; i < parsed.size(); i++) {
        const RoomDefinition &definition = parsed[i];
        if (!(definition.width > 0.0f) || !(definition.depth > 0.0f) ||
            !(definition.height > 0.0f)) {
            fprintf(stderr, "Building definitions: [%s] must define width, depth, and height.\n",
                    definition.name.c_str());
            continue;
        }
        RoomDefinition kept = definition;
        kept.windows.clear();
        for (size_t w = 0; w < definition.windows.size(); w++) {
            const RoomWindow &window = definition.windows[w];
            float span = window.side < ROOM_SIDE_EAST ? definition.width : definition.depth;
            if (fabsf(window.offset) + window.width * 0.5f > span * 0.5f - WALL_THICKNESS ||
                window.bottom + window.height > definition.height - CEILING_THICKNESS) {
                fprintf(stderr, "Building definitions: [%s] has a window outside its wall bounds; ignoring it.\n",
                        definition.name.c_str());
                continue;
            }
            kept.windows.push_back(window);
        }
        validDefs.push_back(kept);
    }
    g_roomDefinitionsInitialized = true;
    if (validDefs.empty() || FindTemplateByRole(validDefs, "entrance") < 0) {
        fprintf(stderr, "Building definitions: using built-in defaults because %s is invalid.\n", path);
        SetDefaultRoomDefinitions(&g_roomDefinitions);
        return false;
    }
    g_roomDefinitions = validDefs;
    return true;
}

static void EnsureRoomDefinitionsLoaded(void)
{
    if (!g_roomDefinitionsInitialized)
        Building_LoadRoomDefinitions("assets/buildings/room_types.txt");
}

struct WallPiece {
    Vector3 first;
    Vector3 second;
    float minY;
    float maxY;
};

enum DebugGeometryType {
    DEBUG_WALL,
    DEBUG_FRAME_JAMB,
    DEBUG_FRAME_HEADER,
    DEBUG_FLOOR,
    DEBUG_CEILING
};

struct DebugGeometry {
    BoundingBox bounds;
    DebugGeometryType type;
    int roomIndex;
    int pieceIndex;
};

struct BoundaryInterval {
    float first;
    float second;
    int peerRoom;
    int peerSocket;
};

static unsigned int NextRandom(unsigned int *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static Vector3 RotateLocal(Vector3 point, float degrees)
{
    float angle = degrees * DEG2RAD;
    float cosine = cosf(angle);
    float sine = sinf(angle);
    return Vector3{ point.x * cosine + point.z * sine, point.y,
                    -point.x * sine + point.z * cosine };
}

static Vector3 RoomToBuilding(const Room &room, Vector3 point)
{
    Vector3 rotated = RotateLocal(point, room.rotation);
    return Vector3{ room.position.x + rotated.x,
                    room.position.y + rotated.y,
                    room.position.z + rotated.z };
}

static const RoomDefinition *TemplateById(int templateId)
{
    EnsureRoomDefinitionsLoaded();
    if (templateId < 0 || templateId >= (int)g_roomDefinitions.size()) return NULL;
    return &g_roomDefinitions[templateId];
}

int Building_RoomTemplateCount(void)
{
    EnsureRoomDefinitionsLoaded();
    return (int)g_roomDefinitions.size();
}

const RoomDefinition *Building_RoomTemplate(int templateId) { return TemplateById(templateId); }

const char *Building_RoomTemplateName(int templateId)
{
    const RoomDefinition *definition = TemplateById(templateId);
    return definition ? definition->name.c_str() : "unknown";
}

const char *Building_RoomName(const Room &room) { return Building_RoomTemplateName(room.templateId); }

float Building_RoomDoorWidth(const Room &room)
{
    const RoomDefinition *definition = TemplateById(room.templateId);
    return definition ? definition->doorWidth : DOOR_WIDTH;
}

static Room MakeRoom(int templateId, Vector3 position, float rotation)
{
    EnsureRoomDefinitionsLoaded();
    if (templateId < 0 || templateId >= (int)g_roomDefinitions.size()) templateId = 0;
    const RoomDefinition &definition = g_roomDefinitions[templateId];
    Room room = {};
    room.templateId = templateId;
    room.position = position;
    room.rotation = rotation;
    room.width = definition.width;
    room.depth = definition.depth;
    room.height = definition.height;
    room.windows = definition.windows;
    SocketType socketType = definition.socketType;
    room.sockets.push_back(Socket{ Vector3{ 0.0f, 1.2f, room.depth * 0.5f }, 0.0f, socketType });
    room.sockets.push_back(Socket{ Vector3{ 0.0f, 1.2f, -room.depth * 0.5f }, 180.0f, socketType });
    room.sockets.push_back(Socket{ Vector3{ room.width * 0.5f, 1.2f, 0.0f }, 90.0f, socketType });
    room.sockets.push_back(Socket{ Vector3{ -room.width * 0.5f, 1.2f, 0.0f }, 270.0f, socketType });
    return room;
}

static bool Compatible(SocketType a, SocketType b)
{
    return (a == SOCKET_DOOR || a == SOCKET_CORRIDOR) &&
           (b == SOCKET_DOOR || b == SOCKET_CORRIDOR);
}

static bool IsConnected(const Building &building, int roomIndex, int socketIndex)
{
    for (size_t i = 0; i < building.connections.size(); i++) {
        const BuildingConnection &connection = building.connections[i];
        if ((connection.roomA == roomIndex && connection.socketA == socketIndex) ||
            (connection.roomB == roomIndex && connection.socketB == socketIndex)) return true;
    }
    return false;
}

static Vector3 SocketPosition(const Room &room, int socketIndex)
{
    return RoomToBuilding(room, room.sockets[socketIndex].position);
}

static float SocketDirection(const Room &room, int socketIndex)
{
    return room.rotation + room.sockets[socketIndex].rotation;
}

static bool RoomInsideBounds(const Room &room)
{
    float angle = room.rotation * DEG2RAD;
    float cosine = fabsf(cosf(angle));
    float sine = fabsf(sinf(angle));
    float halfWidth = (room.width * cosine + room.depth * sine) * 0.5f;
    float halfDepth = (room.depth * cosine + room.width * sine) * 0.5f;
    return room.position.x - halfWidth >= -BUILDING_LIMIT &&
           room.position.x + halfWidth <= BUILDING_LIMIT &&
           room.position.z - halfDepth >= -BUILDING_LIMIT &&
           room.position.z + halfDepth <= BUILDING_LIMIT;
}

static bool RoomsOverlap(const Room &a, const Room &b)
{
    float angleA = a.rotation * DEG2RAD;
    float angleB = b.rotation * DEG2RAD;
    float halfWidthA = (a.width * fabsf(cosf(angleA)) + a.depth * fabsf(sinf(angleA))) * 0.5f;
    float halfDepthA = (a.depth * fabsf(cosf(angleA)) + a.width * fabsf(sinf(angleA))) * 0.5f;
    float halfWidthB = (b.width * fabsf(cosf(angleB)) + b.depth * fabsf(sinf(angleB))) * 0.5f;
    float halfDepthB = (b.depth * fabsf(cosf(angleB)) + b.width * fabsf(sinf(angleB))) * 0.5f;
    return fabsf(a.position.x - b.position.x) < halfWidthA + halfWidthB - 0.01f &&
           fabsf(a.position.z - b.position.z) < halfDepthA + halfDepthB - 0.01f;
}

static bool TryAttach(Building *building, int parentRoom, int parentSocket,
                      int templateId, int candidateSocket)
{
    const Room &parent = building->rooms[parentRoom];
    const Socket &parentConnection = parent.sockets[parentSocket];
    Room candidate = MakeRoom(templateId, Vector3{ 0.0f, 0.0f, 0.0f }, 0.0f);
    if (!Compatible(parentConnection.type, candidate.sockets[candidateSocket].type)) return false;

    float targetDirection = SocketDirection(parent, parentSocket) + 180.0f;
    candidate.rotation = targetDirection - candidate.sockets[candidateSocket].rotation;
    while (candidate.rotation < 0.0f) candidate.rotation += 360.0f;
    while (candidate.rotation >= 360.0f) candidate.rotation -= 360.0f;

    Vector3 target = SocketPosition(parent, parentSocket);
    Vector3 socketOffset = RotateLocal(candidate.sockets[candidateSocket].position,
                                       candidate.rotation);
    candidate.position.x = target.x - socketOffset.x;
    candidate.position.z = target.z - socketOffset.z;
    candidate.position.x = roundf(candidate.position.x / GRID_SIZE) * GRID_SIZE;
    candidate.position.z = roundf(candidate.position.z / GRID_SIZE) * GRID_SIZE;

    if (!RoomInsideBounds(candidate)) return false;
    for (size_t i = 0; i < building->rooms.size(); i++)
        if (RoomsOverlap(candidate, building->rooms[i])) return false;

    int newRoom = (int)building->rooms.size();
    building->rooms.push_back(candidate);
    building->connections.push_back(BuildingConnection{
        parentRoom, parentSocket, newRoom, candidateSocket
    });
    if (Building_ValidatePlan(*building)) return true;

    building->connections.pop_back();
    building->rooms.pop_back();
    return false;
}

static bool SocketIsOpen(const Building &building, int roomIndex, int socketIndex)
{
    if (IsConnected(building, roomIndex, socketIndex)) return true;
    return roomIndex == building.entranceRoom && socketIndex == building.exteriorSocket;
}

// Finds another module side on exactly the same boundary plane.  Connections
// are the normal case, but this also handles two grid-aligned modules that
// happen to touch without an explicit doorway connection.
static int BoundaryPeer(const Building &building, int roomIndex, int socketIndex,
                        int *peerSocket = NULL)
{
    Vector3 position = SocketPosition(building.rooms[roomIndex], socketIndex);
    float direction = SocketDirection(building.rooms[roomIndex], socketIndex) * DEG2RAD;
    for (size_t i = 0; i < building.connections.size(); i++) {
        const BuildingConnection &connection = building.connections[i];
        if (connection.roomA == roomIndex && connection.socketA == socketIndex) {
            if (peerSocket) *peerSocket = connection.socketB;
            return connection.roomB;
        }
        if (connection.roomB == roomIndex && connection.socketB == socketIndex) {
            if (peerSocket) *peerSocket = connection.socketA;
            return connection.roomA;
        }
    }
    for (size_t otherIndex = 0; otherIndex < building.rooms.size(); otherIndex++) {
        if ((int)otherIndex == roomIndex) continue;
        const Room &other = building.rooms[otherIndex];
        for (size_t otherSocket = 0; otherSocket < other.sockets.size(); otherSocket++) {
            Vector3 otherPosition = SocketPosition(other, (int)otherSocket);
            float dx = position.x - otherPosition.x;
            float dz = position.z - otherPosition.z;
            float otherDirection = SocketDirection(other, (int)otherSocket) * DEG2RAD;
            if (dx * dx + dz * dz < 0.0001f &&
                cosf(direction) * cosf(otherDirection) + sinf(direction) * sinf(otherDirection) < -0.999f)
            {
                if (peerSocket) *peerSocket = (int)otherSocket;
                return (int)otherIndex;
            }
        }
    }
    return -1;
}

// Every shared plane has exactly one owner. Explicit connections retain their
// generation direction; incidental touching boundaries use room index order.
static bool SocketOwnsWall(const Building &building, int roomIndex, int socketIndex)
{
    if (roomIndex == building.entranceRoom && socketIndex == building.exteriorSocket) return true;
    int peer = BoundaryPeer(building, roomIndex, socketIndex);
    if (peer < 0) return true; // exterior wall
    for (size_t i = 0; i < building.connections.size(); i++) {
        const BuildingConnection &connection = building.connections[i];
        if (connection.roomA == roomIndex && connection.socketA == socketIndex) return true;
        if (connection.roomB == roomIndex && connection.socketB == socketIndex) return false;
    }
    return roomIndex < peer;
}

static void AddWallPiece(WallPiece *pieces, int *pieceCount, int side,
                         float edge, float first, float second,
                         float minY, float maxY)
{
    if (second - first <= 0.01f) return;
    WallPiece piece = {};
    if (side < 2) {
        piece.first = Vector3{ first, 0.0f, edge };
        piece.second = Vector3{ second, 0.0f, edge };
    } else {
        piece.first = Vector3{ edge, 0.0f, first };
        piece.second = Vector3{ edge, 0.0f, second };
    }
    piece.minY = minY;
    piece.maxY = maxY;
    pieces[(*pieceCount)++] = piece;
}

static Vector3 SidePoint(const Room &room, int side, float along)
{
    float edge = side == 0 ? room.depth * 0.5f :
                 side == 1 ? -room.depth * 0.5f :
                 side == 2 ? room.width * 0.5f : -room.width * 0.5f;
    return side < 2 ? RoomToBuilding(room, Vector3{ along, 0.0f, edge })
                    : RoomToBuilding(room, Vector3{ edge, 0.0f, along });
}

static bool ConnectedBoundaryOwner(const Building &building, int roomIndex, int socketIndex,
                                   int peerRoom, int peerSocket)
{
    for (size_t i = 0; i < building.connections.size(); i++) {
        const BuildingConnection &connection = building.connections[i];
        if (connection.roomA == roomIndex && connection.socketA == socketIndex &&
            connection.roomB == peerRoom && connection.socketB == peerSocket) return true;
        if (connection.roomB == roomIndex && connection.socketB == socketIndex &&
            connection.roomA == peerRoom && connection.socketA == peerSocket) return false;
    }
    return roomIndex < peerRoom;
}

static bool IsOpenConnection(const Building &building, int roomIndex, int socketIndex,
                             int peerRoom, int peerSocket)
{
    for (size_t i = 0; i < building.connections.size(); i++) {
        const BuildingConnection &connection = building.connections[i];
        if ((connection.roomA == roomIndex && connection.socketA == socketIndex &&
             connection.roomB == peerRoom && connection.socketB == peerSocket) ||
            (connection.roomB == roomIndex && connection.socketB == socketIndex &&
             connection.roomA == peerRoom && connection.socketA == peerSocket)) return true;
    }
    return false;
}

static void CollectBoundaryIntervals(const Building &building, int roomIndex, int socketIndex,
                                     std::vector<BoundaryInterval> *intervals)
{
    const Room &room = building.rooms[roomIndex];
    float span = socketIndex < 2 ? (float)room.width : (float)room.depth;
    Vector3 midpoint = SocketPosition(room, socketIndex);
    float direction = SocketDirection(room, socketIndex) * DEG2RAD;
    Vector3 normal = Vector3{ sinf(direction), 0.0f, cosf(direction) };
    Vector3 origin = SidePoint(room, socketIndex, 0.0f);
    Vector3 tangentEnd = SidePoint(room, socketIndex, 1.0f);
    Vector3 tangent = Vector3{ tangentEnd.x - origin.x, 0.0f, tangentEnd.z - origin.z };

    for (size_t peerRoom = 0; peerRoom < building.rooms.size(); peerRoom++) {
        if ((int)peerRoom == roomIndex) continue;
        const Room &other = building.rooms[peerRoom];
        for (int peerSocket = 0; peerSocket < 4; peerSocket++) {
            float peerDirection = SocketDirection(other, peerSocket) * DEG2RAD;
            if (sinf(direction) * sinf(peerDirection) + cosf(direction) * cosf(peerDirection) > -0.999f)
                continue;
            Vector3 peerMidpoint = SocketPosition(other, peerSocket);
            float planeDistance = (peerMidpoint.x - midpoint.x) * normal.x +
                                  (peerMidpoint.z - midpoint.z) * normal.z;
            if (fabsf(planeDistance) > 0.001f) continue;

            float peerSpan = peerSocket < 2 ? (float)other.width : (float)other.depth;
            Vector3 firstPoint = SidePoint(other, peerSocket, -peerSpan * 0.5f);
            Vector3 secondPoint = SidePoint(other, peerSocket, peerSpan * 0.5f);
            float first = (firstPoint.x - origin.x) * tangent.x +
                          (firstPoint.z - origin.z) * tangent.z;
            float second = (secondPoint.x - origin.x) * tangent.x +
                           (secondPoint.z - origin.z) * tangent.z;
            if (first > second) { float swap = first; first = second; second = swap; }
            if (first < -span * 0.5f) first = -span * 0.5f;
            if (second > span * 0.5f) second = span * 0.5f;
            if (second - first > 0.01f)
                intervals->push_back(BoundaryInterval{ first, second, (int)peerRoom, peerSocket });
        }
    }
}

static void AddBoundaryWallPieces(WallPiece *pieces, int *pieceCount, int side, float edge,
                                  float first, float second, bool opening, float gap,
                                  float roomHeight, const RoomWindow *window)
{
    if (window) {
        // The interval has already been split at both window edges. Preserve
        // wall below and above the glazing so this is a real wall opening.
        AddWallPiece(pieces, pieceCount, side, edge, first, second,
                     FLOOR_THICKNESS, window->bottom);
        AddWallPiece(pieces, pieceCount, side, edge, first, second,
                     window->bottom + window->height, roomHeight - CEILING_THICKNESS);
        return;
    }
    if (!opening) {
        AddWallPiece(pieces, pieceCount, side, edge, first, second,
                     FLOOR_THICKNESS, roomHeight - CEILING_THICKNESS);
        return;
    }
    AddWallPiece(pieces, pieceCount, side, edge, first, fminf(second, -gap * 0.5f),
                 FLOOR_THICKNESS, roomHeight - CEILING_THICKNESS);
    AddWallPiece(pieces, pieceCount, side, edge, fmaxf(first, gap * 0.5f), second,
                 FLOOR_THICKNESS, roomHeight - CEILING_THICKNESS);
    AddWallPiece(pieces, pieceCount, side, edge, fmaxf(first, -gap * 0.5f),
                 fminf(second, gap * 0.5f), DOOR_HEIGHT,
                 roomHeight - CEILING_THICKNESS);
}

static int RoomWallPieces(const Building &building, int roomIndex, WallPiece *pieces)
{
    const Room &room = building.rooms[roomIndex];
    int pieceCount = 0;
    for (int side = 0; side < 4; side++) {
        float span = side < 2 ? (float)room.width : (float)room.depth;
        float edge = side == 0 ? room.depth * 0.5f :
                     side == 1 ? -room.depth * 0.5f :
                     side == 2 ? room.width * 0.5f : -room.width * 0.5f;
        float gap = Building_RoomDoorWidth(room);
        std::vector<BoundaryInterval> intervals;
        std::vector<float> cuts;
        cuts.push_back(-span * 0.5f);
        cuts.push_back(span * 0.5f);
        CollectBoundaryIntervals(building, roomIndex, side, &intervals);
        for (size_t i = 0; i < intervals.size(); i++) {
            cuts.push_back(intervals[i].first);
            cuts.push_back(intervals[i].second);
        }
        for (size_t i = 0; i < room.windows.size(); i++) {
            const RoomWindow &window = room.windows[i];
            if ((int)window.side != side) continue;
            cuts.push_back(window.offset - window.width * 0.5f);
            cuts.push_back(window.offset + window.width * 0.5f);
        }
        std::sort(cuts.begin(), cuts.end());
        for (size_t intervalIndex = 0; intervalIndex + 1 < cuts.size(); intervalIndex++) {
            float first = cuts[intervalIndex];
            float second = cuts[intervalIndex + 1];
            if (second - first <= 0.01f) continue;
            float midpoint = (first + second) * 0.5f;
            const BoundaryInterval *peer = NULL;
            for (size_t i = 0; i < intervals.size(); i++)
                if (midpoint > intervals[i].first + 0.001f && midpoint < intervals[i].second - 0.001f) {
                    peer = &intervals[i];
                    break;
                }

            bool owner = peer == NULL || ConnectedBoundaryOwner(building, roomIndex, side,
                                                                  peer->peerRoom, peer->peerSocket);
            if (!owner) continue;
            // A shared segment has one draw owner, but its vertical extent is
            // structural for both rooms. Without this, a shorter owner leaves
            // a gap beside a taller neighbouring room.
            float wallHeight = room.height;
            if (peer && building.rooms[peer->peerRoom].height > wallHeight)
                wallHeight = building.rooms[peer->peerRoom].height;
            bool opening = peer ? IsOpenConnection(building, roomIndex, side,
                                                    peer->peerRoom, peer->peerSocket)
                                : (roomIndex == building.entranceRoom && side == building.exteriorSocket);
            const RoomWindow *window = NULL;
            if (!peer && !opening) {
                for (size_t i = 0; i < room.windows.size(); i++) {
                    const RoomWindow &candidate = room.windows[i];
                    if ((int)candidate.side == side &&
                        midpoint > candidate.offset - candidate.width * 0.5f + 0.001f &&
                        midpoint < candidate.offset + candidate.width * 0.5f - 0.001f) {
                        window = &candidate;
                        break;
                    }
                }
            }
            AddBoundaryWallPieces(pieces, &pieceCount, side, edge, first, second, opening, gap,
                                  wallHeight, window);
        }
    }
    return pieceCount;
}

static Vector3 WorldPosition(const Building &building, const Room &room, Vector3 local)
{
    Vector3 point = RoomToBuilding(room, local);
    return Vector3{ building.position.x + point.x,
                    building.position.y + point.y,
                    building.position.z + point.z };
}

static Vector3 RotateHorizontal(Vector3 point, float degrees)
{
    return RotateLocal(point, degrees);
}

static Vector3 DoorWorldPoint(const Building &building, const BuildingDoor &door, Vector3 local)
{
    return Vector3{ building.position.x + door.hingePosition.x + local.x,
                    building.position.y + door.hingePosition.y + local.y,
                    building.position.z + door.hingePosition.z + local.z };
}

static Vector3 DoorLeafDirection(const BuildingDoor &door)
{
    return RotateHorizontal(Vector3{ 1.0f, 0.0f, 0.0f },
                            door.rotation + door.openAngle * door.openingDirection);
}

static Vector3 DoorCenter(const Building &building, const BuildingDoor &door)
{
    Vector3 leaf = DoorLeafDirection(door);
    return DoorWorldPoint(building, door,
                          Vector3{ leaf.x * door.width * 0.5f, door.height * 0.5f, leaf.z * door.width * 0.5f });
}

static float SegmentDistanceSquared(float x, float z, Vector3 a, Vector3 b);

static void DrawDoorPanel(const Building &building, const BuildingDoor &door, bool debug)
{
    Vector3 center = DoorCenter(building, door);
    float angle = door.rotation + door.openAngle * door.openingDirection;
    const Color panel = Color{ 112, 76, 47, 255 };
    // This project uses a raylib version without DrawCubePro.  Apply the
    // equivalent local transform around the panel centre through rlgl.
    rlPushMatrix();
        rlTranslatef(center.x, center.y, center.z);
        rlRotatef(angle, 0.0f, 1.0f, 0.0f);
        DrawCubeV(Vector3{ 0.0f, 0.0f, 0.0f },
                  Vector3{ door.width, door.height, DOOR_PANEL_THICKNESS }, panel);
    rlPopMatrix();

    // A small handle on the free edge makes the moving panel legible without
    // adding another asset or changing the existing doorway frame geometry.
    Vector3 leaf = DoorLeafDirection(door);
    Vector3 handle = DoorWorldPoint(building, door, Vector3{
        leaf.x * (door.width - 0.16f), door.height * 0.52f,
        leaf.z * (door.width - 0.16f)
    });
    DrawSphere(handle, 0.045f, Color{ 206, 180, 104, 255 });
    if (debug) {
        Vector3 hinge = DoorWorldPoint(building, door, Vector3{ 0.0f, 0.08f, 0.0f });
        DrawSphere(hinge, 0.07f, ORANGE);
        DrawLine3D(hinge, center, door.open ? LIME : RED);
    }
}

static bool DoorCollides(const Building &building, const BuildingDoor &door,
                         float x, float z, float radius, float feetY, float height)
{
    float minY = building.position.y + door.hingePosition.y;
    float maxY = minY + door.height;
    if (feetY >= maxY || feetY + height <= minY) return false;

    Vector3 hinge = DoorWorldPoint(building, door, Vector3{ 0.0f, 0.0f, 0.0f });
    Vector3 leaf = DoorLeafDirection(door);
    Vector3 end = Vector3{ hinge.x + leaf.x * door.width, hinge.y, hinge.z + leaf.z * door.width };
    float collisionRadius = radius + DOOR_PANEL_THICKNESS * 0.5f;
    return SegmentDistanceSquared(x, z, hinge, end) < collisionRadius * collisionRadius;
}

void Building_GenerateDoors(Building *building)
{
    if (!building) return;
    building->doors.clear();

    // Connections store a unique owner (roomA); this exactly matches the wall
    // opening/frame owner and prevents a second panel in shared doorways.
    for (size_t connectionIndex = 0; connectionIndex < building->connections.size(); connectionIndex++) {
        const BuildingConnection &connection = building->connections[connectionIndex];
        const int roomIndex = connection.roomA;
        const int socketIndex = connection.socketA;
        const Room &room = building->rooms[roomIndex];
        Vector3 midpoint = SocketPosition(room, socketIndex);
        float direction = SocketDirection(room, socketIndex);
        float width = Building_RoomDoorWidth(room);
        Vector3 tangent = RotateHorizontal(Vector3{ 1.0f, 0.0f, 0.0f }, direction);
        midpoint.y = room.position.y + FLOOR_THICKNESS;
        building->doors.push_back(BuildingDoor{
            midpoint,
            Vector3{ midpoint.x - tangent.x * width * 0.5f,
                     midpoint.y,
                     midpoint.z - tangent.z * width * 0.5f },
            width, DOOR_HEIGHT - FLOOR_THICKNESS - DOOR_FRAME_CLEARANCE,
            direction, 0.0f, 0.0f, false, roomIndex, socketIndex, 1
        });
    }

    const Room &entrance = building->rooms[building->entranceRoom];
    const int socketIndex = building->exteriorSocket;
    Vector3 midpoint = SocketPosition(entrance, socketIndex);
    float direction = SocketDirection(entrance, socketIndex);
    float width = Building_RoomDoorWidth(entrance);
    Vector3 tangent = RotateHorizontal(Vector3{ 1.0f, 0.0f, 0.0f }, direction);
    midpoint.y = entrance.position.y + FLOOR_THICKNESS;
    building->doors.push_back(BuildingDoor{
        midpoint,
        Vector3{ midpoint.x - tangent.x * width * 0.5f,
                 midpoint.y,
                 midpoint.z - tangent.z * width * 0.5f },
        width, DOOR_HEIGHT - FLOOR_THICKNESS - DOOR_FRAME_CLEARANCE,
        direction, 0.0f, 0.0f, false, building->entranceRoom, socketIndex, 1
    });
}

void Building_UpdateDoors(Building *building, float dt)
{
    if (!building) return;
    float step = DOOR_ANGULAR_SPEED * dt;
    for (size_t i = 0; i < building->doors.size(); i++) {
        BuildingDoor &door = building->doors[i];
        if (door.openAngle < door.targetAngle) door.openAngle = fminf(door.openAngle + step, door.targetAngle);
        else if (door.openAngle > door.targetAngle) door.openAngle = fmaxf(door.openAngle - step, door.targetAngle);
    }
}

int Building_FindDoor(const Building &building, Camera3D camera, float maxDistance, float *score)
{
    Vector3 view = Vector3{ camera.target.x - camera.position.x, camera.target.y - camera.position.y,
                            camera.target.z - camera.position.z };
    float viewLength = sqrtf(view.x * view.x + view.y * view.y + view.z * view.z);
    if (viewLength <= 0.001f) return -1;
    view.x /= viewLength; view.y /= viewLength; view.z /= viewLength;
    int best = -1;
    float bestScore = -FLT_MAX;
    for (size_t i = 0; i < building.doors.size(); i++) {
        Vector3 center = DoorCenter(building, building.doors[i]);
        float dx = center.x - camera.position.x, dy = center.y - camera.position.y, dz = center.z - camera.position.z;
        float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (distance > maxDistance || distance < 0.001f) continue;
        float alignment = (dx * view.x + dy * view.y + dz * view.z) / distance;
        if (alignment < 0.72f) continue;
        float candidate = alignment * 2.0f - distance / maxDistance;
        if (candidate > bestScore) { best = (int)i; bestScore = candidate; }
    }
    if (score) *score = bestScore;
    return best;
}

void Building_ToggleDoor(Building *building, int doorIndex)
{
    if (!building || doorIndex < 0 || doorIndex >= (int)building->doors.size()) return;
    BuildingDoor &door = building->doors[doorIndex];
    if (fabsf(door.openAngle - door.targetAngle) > 0.01f) return;
    door.open = !door.open;
    door.targetAngle = door.open ? DOOR_OPEN_ANGLE : 0.0f;
}

void Building_GenerateSupports(Building *building)
{
    if (!building) return;

    // Supports are derived data. Rebuilding them here is intentional when a
    // world is regenerated after terrain settings change, but never per frame.
    building->supports.clear();
    for (size_t roomIndex = 0; roomIndex < building->rooms.size(); roomIndex++) {
        const Room &room = building->rooms[roomIndex];
        const float halfWidth = room.width * 0.5f;
        const float halfDepth = room.depth * 0.5f;
        const Vector3 corners[4] = {
            Vector3{ -halfWidth, 0.0f, -halfDepth },
            Vector3{  halfWidth, 0.0f, -halfDepth },
            Vector3{  halfWidth, 0.0f,  halfDepth },
            Vector3{ -halfWidth, 0.0f,  halfDepth }
        };

        for (int cornerIndex = 0; cornerIndex < 4; cornerIndex++) {
            // RoomToBuilding is the authoritative room rotation transform.
            // Using it prevents this code from drifting from room rendering.
            Vector3 world = WorldPosition(*building, room, corners[cornerIndex]);
            float terrainY = Terrain_Height(world.x, world.z);
            // The room-local y coordinate is the floor underside; honoring it
            // also keeps supports correct if modules gain vertical offsets.
            float height = world.y - terrainY;
            if (height <= BUILDING_SUPPORT_MIN_HEIGHT) continue;

            bool duplicate = false;
            for (size_t supportIndex = 0; supportIndex < building->supports.size(); supportIndex++) {
                const BuildingSupport &support = building->supports[supportIndex];
                float dx = support.position.x - world.x;
                float dz = support.position.z - world.z;
                if (dx * dx + dz * dz < BUILDING_SUPPORT_DUPLICATE_DISTANCE *
                                            BUILDING_SUPPORT_DUPLICATE_DISTANCE) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            // The cube centre places its top at the floor underside and its
            // bottom at the terrain sample, so there is no floating column.
            building->supports.push_back(BuildingSupport{
                Vector3{ world.x, terrainY + height * 0.5f, world.z },
                height,
                BUILDING_SUPPORT_WIDTH
            });
        }
    }
}

static Color ShadeColor(Color color, float factor)
{
    return Color{
        (unsigned char)(color.r * factor),
        (unsigned char)(color.g * factor),
        (unsigned char)(color.b * factor),
        color.a
    };
}

// Room rotations are constrained to right angles by Building_ValidatePlan().
// Transforming dimensions this way keeps the primitive renderer simple while
// allowing every room to use the same local-space drawing code.
static Vector3 RoomCubeSize(const Room &room, Vector3 localSize)
{
    float angle = room.rotation * DEG2RAD;
    return Vector3{
        fabsf(cosf(angle)) * localSize.x + fabsf(sinf(angle)) * localSize.z,
        localSize.y,
        fabsf(cosf(angle)) * localSize.z + fabsf(sinf(angle)) * localSize.x
    };
}

static BoundingBox RoomCubeBounds(const Building &building, const Room &room,
                                  Vector3 localCenter, Vector3 localSize)
{
    Vector3 center = WorldPosition(building, room, localCenter);
    Vector3 size = RoomCubeSize(room, localSize);
    Vector3 half = Vector3{ size.x * 0.5f, size.y * 0.5f, size.z * 0.5f };
    return BoundingBox{ Vector3{ center.x - half.x, center.y - half.y, center.z - half.z },
                        Vector3{ center.x + half.x, center.y + half.y, center.z + half.z } };
}

static void DrawRoomCube(const Building &building, const Room &room,
                         Vector3 localCenter, Vector3 localSize, Color color)
{
    Vector3 size = RoomCubeSize(room, localSize);
    DrawCubeV(WorldPosition(building, room, localCenter), size, color);
}

static void AddDebugGeometry(std::vector<DebugGeometry> *geometry,
                             const Building &building, const Room &room,
                             Vector3 localCenter, Vector3 localSize,
                             DebugGeometryType type, int roomIndex, int pieceIndex)
{
    if (!geometry) return;
    geometry->push_back(DebugGeometry{
        RoomCubeBounds(building, room, localCenter, localSize), type, roomIndex, pieceIndex
    });
}

static Color DebugGeometryColor(DebugGeometryType type)
{
    switch (type) {
        case DEBUG_WALL: return RED;
        case DEBUG_FRAME_JAMB: return YELLOW;
        case DEBUG_FRAME_HEADER: return ORANGE;
        case DEBUG_FLOOR: return LIME;
        default: return SKYBLUE;
    }
}

static const char *DebugGeometryName(DebugGeometryType type)
{
    switch (type) {
        case DEBUG_WALL: return "wall";
        case DEBUG_FRAME_JAMB: return "frame-jamb";
        case DEBUG_FRAME_HEADER: return "frame-header";
        case DEBUG_FLOOR: return "floor";
        default: return "ceiling";
    }
}

static void ReportDebugIntersections(const Building &building,
                                     const std::vector<DebugGeometry> &geometry)
{
    // Print each plan once while F3 debug is enabled. Face/edge contact is
    // intentionally ignored; only a material three-dimensional overlap is reported.
    static unsigned int reportedSeeds[32] = {};
    static int reportedCount = 0;
    for (int i = 0; i < reportedCount; i++) if (reportedSeeds[i] == building.seed) return;
    if (reportedCount < 32) reportedSeeds[reportedCount++] = building.seed;

    int overlaps = 0;
    for (size_t a = 0; a < geometry.size(); a++) {
        for (size_t b = a + 1; b < geometry.size(); b++) {
            const BoundingBox &first = geometry[a].bounds;
            const BoundingBox &second = geometry[b].bounds;
            float overlapX = fminf(first.max.x, second.max.x) - fmaxf(first.min.x, second.min.x);
            float overlapY = fminf(first.max.y, second.max.y) - fmaxf(first.min.y, second.min.y);
            float overlapZ = fminf(first.max.z, second.max.z) - fmaxf(first.min.z, second.min.z);
            if (overlapX <= 0.01f || overlapY <= 0.01f || overlapZ <= 0.01f ||
                overlapX * overlapY * overlapZ <= 0.001f) continue;
            if (geometry[a].type == DEBUG_WALL && geometry[b].type == DEBUG_WALL &&
                overlapX <= WALL_THICKNESS + 0.001f && overlapZ <= WALL_THICKNESS + 0.001f)
                continue; // Perpendicular wall-end contact, not duplicate wall volume.
            printf("building %u overlap: %s room %d piece %d [%.2f %.2f %.2f]-[%.2f %.2f %.2f] "
                   "with %s room %d piece %d [%.2f %.2f %.2f]-[%.2f %.2f %.2f] "
                   "(%.3f x %.3f x %.3f)\n", building.seed,
                   DebugGeometryName(geometry[a].type), geometry[a].roomIndex, geometry[a].pieceIndex,
                   first.min.x, first.min.y, first.min.z, first.max.x, first.max.y, first.max.z,
                   DebugGeometryName(geometry[b].type), geometry[b].roomIndex, geometry[b].pieceIndex,
                   second.min.x, second.min.y, second.min.z, second.max.x, second.max.y, second.max.z,
                   overlapX, overlapY, overlapZ);
            overlaps++;
        }
    }
    printf("building %u debug geometry: %d boxes, %d substantial overlaps\n",
           building.seed, (int)geometry.size(), overlaps);
    fflush(stdout);
}

static void DrawDoorFrames(const Building &building, const Room &room, int roomIndex,
                           std::vector<DebugGeometry> *debugGeometry)
{
    const float frameWidth = 0.10f;
    const float frameDepth = 0.08f;
    const float frameClearance = 0.03f;
    const Color trim = Color{ 101, 83, 64, 255 };

    for (int side = 0; side < 4; side++) {
        if (!SocketIsOpen(building, roomIndex, side) ||
            !SocketOwnsWall(building, roomIndex, side)) continue;

        float gap = Building_RoomDoorWidth(building.rooms[roomIndex]);
        float jambHeight = DOOR_HEIGHT - FLOOR_THICKNESS;
        float jambY = FLOOR_THICKNESS + jambHeight * 0.5f;
        float edge = side == 0 ? room.depth * 0.5f :
                     side == 1 ? -room.depth * 0.5f :
                     side == 2 ? room.width * 0.5f : -room.width * 0.5f;
        float inset = WALL_THICKNESS * 0.5f + frameClearance + frameDepth * 0.5f;

        if (side < 2) {
            float z = side == 0 ? edge - inset : edge + inset;
            DrawRoomCube(building, room, Vector3{ -gap * 0.5f + frameWidth * 0.5f, jambY, z },
                         Vector3{ frameWidth, jambHeight, frameDepth }, trim);
            AddDebugGeometry(debugGeometry, building, room,
                             Vector3{ -gap * 0.5f + frameWidth * 0.5f, jambY, z },
                             Vector3{ frameWidth, jambHeight, frameDepth }, DEBUG_FRAME_JAMB, roomIndex, side * 3);
            DrawRoomCube(building, room, Vector3{  gap * 0.5f - frameWidth * 0.5f, jambY, z },
                         Vector3{ frameWidth, jambHeight, frameDepth }, trim);
            AddDebugGeometry(debugGeometry, building, room,
                             Vector3{ gap * 0.5f - frameWidth * 0.5f, jambY, z },
                             Vector3{ frameWidth, jambHeight, frameDepth }, DEBUG_FRAME_JAMB, roomIndex, side * 3 + 1);
            DrawRoomCube(building, room, Vector3{ 0.0f, DOOR_HEIGHT - frameWidth * 0.5f, z },
                         Vector3{ gap - 2.0f * frameWidth, frameWidth, frameDepth }, trim);
            AddDebugGeometry(debugGeometry, building, room,
                             Vector3{ 0.0f, DOOR_HEIGHT - frameWidth * 0.5f, z },
                             Vector3{ gap - 2.0f * frameWidth, frameWidth, frameDepth }, DEBUG_FRAME_HEADER, roomIndex, side * 3 + 2);
        } else {
            float x = side == 2 ? edge - inset : edge + inset;
            DrawRoomCube(building, room, Vector3{ x, jambY, -gap * 0.5f + frameWidth * 0.5f },
                         Vector3{ frameDepth, jambHeight, frameWidth }, trim);
            AddDebugGeometry(debugGeometry, building, room,
                             Vector3{ x, jambY, -gap * 0.5f + frameWidth * 0.5f },
                             Vector3{ frameDepth, jambHeight, frameWidth }, DEBUG_FRAME_JAMB, roomIndex, side * 3);
            DrawRoomCube(building, room, Vector3{ x, jambY,  gap * 0.5f - frameWidth * 0.5f },
                         Vector3{ frameDepth, jambHeight, frameWidth }, trim);
            AddDebugGeometry(debugGeometry, building, room,
                             Vector3{ x, jambY, gap * 0.5f - frameWidth * 0.5f },
                             Vector3{ frameDepth, jambHeight, frameWidth }, DEBUG_FRAME_JAMB, roomIndex, side * 3 + 1);
            DrawRoomCube(building, room, Vector3{ x, DOOR_HEIGHT - frameWidth * 0.5f, 0.0f },
                         Vector3{ frameDepth, frameWidth, gap - 2.0f * frameWidth }, trim);
            AddDebugGeometry(debugGeometry, building, room,
                             Vector3{ x, DOOR_HEIGHT - frameWidth * 0.5f, 0.0f },
                             Vector3{ frameDepth, frameWidth, gap - 2.0f * frameWidth }, DEBUG_FRAME_HEADER, roomIndex, side * 3 + 2);
        }
    }
}

static bool WindowIsExterior(const Building &building, int roomIndex, const RoomWindow &window)
{
    if (roomIndex == building.entranceRoom && (int)window.side == building.exteriorSocket) return false;
    std::vector<BoundaryInterval> intervals;
    CollectBoundaryIntervals(building, roomIndex, (int)window.side, &intervals);
    for (size_t i = 0; i < intervals.size(); i++)
        if (window.offset > intervals[i].first + 0.001f &&
            window.offset < intervals[i].second - 0.001f) return false;
    return true;
}

// The glass is deliberately a thin tinted cube: it remains asset-free and the
// surrounding four trim pieces make the opening visually distinct from a door.
static void DrawRoomWindows(const Building &building, const Room &room, int roomIndex)
{
    const float frame = 0.10f;
    const float glassDepth = 0.035f;
    const Color trim = Color{ 76, 76, 70, 255 };
    const Color glass = Color{ 116, 180, 205, 145 };
    for (size_t index = 0; index < room.windows.size(); index++) {
        const RoomWindow &window = room.windows[index];
        if (!WindowIsExterior(building, roomIndex, window)) continue;
        float edge = window.side == ROOM_SIDE_NORTH ? room.depth * 0.5f :
                     window.side == ROOM_SIDE_SOUTH ? -room.depth * 0.5f :
                     window.side == ROOM_SIDE_EAST ? room.width * 0.5f : -room.width * 0.5f;
        float centerY = window.bottom + window.height * 0.5f;
        if (window.side == ROOM_SIDE_NORTH || window.side == ROOM_SIDE_SOUTH) {
            DrawRoomCube(building, room, Vector3{ window.offset, centerY, edge },
                         // Local X runs along north/south walls; Y is always
                         // vertical, so glass thickness belongs on local Z.
                         Vector3{ window.width, window.height, glassDepth }, glass);
            for (int sign = -1; sign <= 1; sign += 2)
                DrawRoomCube(building, room, Vector3{ window.offset + sign * (window.width - frame) * 0.5f, centerY, edge },
                             Vector3{ frame, window.height + frame * 2.0f, frame }, trim);
            DrawRoomCube(building, room, Vector3{ window.offset, window.bottom - frame * 0.5f, edge },
                         Vector3{ window.width, frame, frame }, trim);
            DrawRoomCube(building, room, Vector3{ window.offset, window.bottom + window.height + frame * 0.5f, edge },
                         Vector3{ window.width, frame, frame }, trim);
        } else {
            DrawRoomCube(building, room, Vector3{ edge, centerY, window.offset },
                         Vector3{ glassDepth, window.height, window.width }, glass);
            for (int sign = -1; sign <= 1; sign += 2)
                DrawRoomCube(building, room, Vector3{ edge, centerY, window.offset + sign * (window.width - frame) * 0.5f },
                             Vector3{ frame, window.height + frame * 2.0f, frame }, trim);
            DrawRoomCube(building, room, Vector3{ edge, window.bottom - frame * 0.5f, window.offset },
                         Vector3{ frame, frame, window.width }, trim);
            DrawRoomCube(building, room, Vector3{ edge, window.bottom + window.height + frame * 0.5f, window.offset },
                         Vector3{ frame, frame, window.width }, trim);
        }
    }
}

static Color RoomFloorColor(const Room &room)
{
    // Roles drive the tint so new templates style themselves without a switch.
    const RoomDefinition *definition = TemplateById(room.templateId);
    const char *role = definition ? definition->role.c_str() : "normal";
    if (!strcmp(role, "entrance")) return Color{ 133, 121, 96, 255 };
    if (!strcmp(role, "utility")) return Color{ 105, 147, 154, 255 };
    if (definition && definition->socketType == SOCKET_CORRIDOR) return Color{ 122, 133, 131, 255 };
    return Color{ 145, 133, 112, 255 };
}

static void DrawPlaceholderModule(const Building &building, const Room &room, int roomIndex,
                                  std::vector<DebugGeometry> *debugGeometry)
{
    Color floorColor = RoomFloorColor(room);
    DrawRoomCube(building, room, Vector3{ 0.0f, FLOOR_THICKNESS * 0.5f, 0.0f },
                 Vector3{ (float)room.width, FLOOR_THICKNESS, (float)room.depth }, floorColor);
    AddDebugGeometry(debugGeometry, building, room, Vector3{ 0.0f, FLOOR_THICKNESS * 0.5f, 0.0f },
                     Vector3{ (float)room.width, FLOOR_THICKNESS, (float)room.depth }, DEBUG_FLOOR, roomIndex, 0);
    DrawRoomCube(building, room, Vector3{ 0.0f, room.height - CEILING_THICKNESS * 0.5f, 0.0f },
                 Vector3{ (float)room.width, CEILING_THICKNESS, (float)room.depth },
                 Color{ 184, 184, 174, 255 });
    AddDebugGeometry(debugGeometry, building, room,
                     Vector3{ 0.0f, room.height - CEILING_THICKNESS * 0.5f, 0.0f },
                     Vector3{ (float)room.width, CEILING_THICKNESS, (float)room.depth }, DEBUG_CEILING, roomIndex, 0);

    WallPiece pieces[WALL_PIECE_LIMIT];
    int pieceCount = RoomWallPieces(building, roomIndex, pieces);
    for (int i = 0; i < pieceCount; i++) {
        const WallPiece &piece = pieces[i];
        Vector3 localCenter = Vector3{
            (piece.first.x + piece.second.x) * 0.5f,
            (piece.minY + piece.maxY) * 0.5f,
            (piece.first.z + piece.second.z) * 0.5f
        };
        float length = sqrtf((piece.second.x - piece.first.x) * (piece.second.x - piece.first.x) +
                             (piece.second.z - piece.first.z) * (piece.second.z - piece.first.z));
        bool alongX = fabsf(piece.second.x - piece.first.x) > fabsf(piece.second.z - piece.first.z);
        float sizeX = alongX ? length : WALL_THICKNESS;
        float sizeZ = alongX ? WALL_THICKNESS : length;
        float lightFacing = alongX ? 0.92f : 0.82f;
        DrawRoomCube(building, room, localCenter,
                     Vector3{ sizeX, piece.maxY - piece.minY, sizeZ },
                     ShadeColor(Color{ 214, 210, 197, 255 }, lightFacing));
        AddDebugGeometry(debugGeometry, building, room, localCenter,
                         Vector3{ sizeX, piece.maxY - piece.minY, sizeZ },
                         DEBUG_WALL, roomIndex, i);
    }
    DrawDoorFrames(building, room, roomIndex, debugGeometry);
    DrawRoomWindows(building, room, roomIndex);
}

static float SegmentDistanceSquared(float x, float z, Vector3 a, Vector3 b)
{
    float sx = b.x - a.x;
    float sz = b.z - a.z;
    float lengthSquared = sx * sx + sz * sz;
    float t = lengthSquared > 0.0f ? ((x - a.x) * sx + (z - a.z) * sz) / lengthSquared : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float dx = x - (a.x + sx * t);
    float dz = z - (a.z + sz * t);
    return dx * dx + dz * dz;
}

// Tests the player's horizontal circle against a room footprint. Together with
// a vertical overlap check this treats floors and ceilings as solid slabs.
static bool RoomFootprintCollides(const Building &building, const Room &room,
                                  float x, float z, float radius)
{
    float worldX = x - building.position.x - room.position.x;
    float worldZ = z - building.position.z - room.position.z;
    float angle = room.rotation * DEG2RAD;
    float localX = worldX * cosf(angle) - worldZ * sinf(angle);
    float localZ = worldX * sinf(angle) + worldZ * cosf(angle);
    return localX >= -room.width * 0.5f - radius &&
           localX <=  room.width * 0.5f + radius &&
           localZ >= -room.depth * 0.5f - radius &&
           localZ <=  room.depth * 0.5f + radius;
}

static bool RoomContainsPoint(const Building &building, const Room &room, float x, float z)
{
    return RoomFootprintCollides(building, room, x, z, 0.0f);
}

// Picks a template by weight among entries that opt into random selection
// (weight > 0). Returns -1 when no template carries a positive weight.
static int PickWeightedTemplate(unsigned int *state)
{
    EnsureRoomDefinitionsLoaded();
    float total = 0.0f;
    for (size_t i = 0; i < g_roomDefinitions.size(); i++)
        if (g_roomDefinitions[i].weight > 0.0f) total += g_roomDefinitions[i].weight;
    if (!(total > 0.0f)) return -1;
    float roll = (float)(NextRandom(state) % 100000u) * (total / 100000.0f);
    for (size_t i = 0; i < g_roomDefinitions.size(); i++) {
        if (g_roomDefinitions[i].weight <= 0.0f) continue;
        roll -= g_roomDefinitions[i].weight;
        if (roll < 0.0f) return (int)i;
    }
    return -1;
}

Building Building_Generate(unsigned int seed)
{
    Building building = {};
    building.type = BUILDING_GENERIC;
    building.seed = seed;
    building.entranceRoom = 0;
    building.exteriorSocket = 1;
    unsigned int state = seed ? seed : 1;

    EnsureRoomDefinitionsLoaded();
    int entranceTemplate = FindTemplateByRole(g_roomDefinitions, "entrance");
    building.rooms.push_back(MakeRoom(entranceTemplate >= 0 ? entranceTemplate : 0,
                                      Vector3{ 0.0f, 0.0f, 0.0f }, 0.0f));

    int first = PickWeightedTemplate(&state);
    if (first >= 0) TryAttach(&building, 0, 0, first, 1);
    if (building.rooms.size() > 1) {
        int second = PickWeightedTemplate(&state);
        if (second >= 0) TryAttach(&building, 1, 2, second, 3);
        int third = PickWeightedTemplate(&state);
        if (third >= 0) TryAttach(&building, 1, 3, third, 2);
    }

    int extraModules = 2 + (int)(NextRandom(&state) % 5u);
    for (int added = 0, attempts = 0; added < extraModules && attempts < 80; attempts++) {
        int parentRoom = (int)(NextRandom(&state) % building.rooms.size());
        int parentSocket = (int)(NextRandom(&state) % 4u);
        if (parentRoom == building.entranceRoom && parentSocket == building.exteriorSocket) continue;
        if (IsConnected(building, parentRoom, parentSocket)) continue;

        int templateId = PickWeightedTemplate(&state);
        if (templateId < 0) break;
        int socket = (parentSocket + 2) % 4;
        if (TryAttach(&building, parentRoom, parentSocket, templateId, socket)) added++;
    }
    Building_GenerateDoors(&building);
    return building;
}

Building Building_GenerateTestBuilding(unsigned int seed)
{
    return Building_Generate(seed);
}

bool Building_ValidatePlan(const Building &building)
{
    if (building.rooms.empty() || building.entranceRoom < 0 ||
        building.entranceRoom >= (int)building.rooms.size()) return false;
    if (building.exteriorSocket < 0 ||
        building.exteriorSocket >= (int)building.rooms[building.entranceRoom].sockets.size()) return false;

    std::vector<std::vector<int> > used(building.rooms.size());
    for (size_t i = 0; i < building.rooms.size(); i++) {
        const Room &room = building.rooms[i];
        const RoomDefinition *definition = TemplateById(room.templateId);
        if (!definition || room.width != definition->width || room.depth != definition->depth ||
            fabsf(room.height - definition->height) > 0.001f ||
            !RoomInsideBounds(room)) return false;
        if (fabsf(room.position.x / GRID_SIZE - roundf(room.position.x / GRID_SIZE)) > 0.001f ||
            fabsf(room.position.z / GRID_SIZE - roundf(room.position.z / GRID_SIZE)) > 0.001f ||
            fabsf(room.rotation / 90.0f - roundf(room.rotation / 90.0f)) > 0.001f) return false;
        used[i].resize(room.sockets.size(), 0);
        for (size_t j = 0; j < i; j++)
            if (RoomsOverlap(room, building.rooms[j])) return false;
    }

    std::vector<std::vector<int> > neighbors(building.rooms.size());
    for (size_t i = 0; i < building.connections.size(); i++) {
        const BuildingConnection &connection = building.connections[i];
        if (connection.roomA < 0 || connection.roomA >= (int)building.rooms.size() ||
            connection.roomB < 0 || connection.roomB >= (int)building.rooms.size() ||
            connection.roomA == connection.roomB) return false;
        const Room &roomA = building.rooms[connection.roomA];
        const Room &roomB = building.rooms[connection.roomB];
        if (connection.socketA < 0 || connection.socketA >= (int)roomA.sockets.size() ||
            connection.socketB < 0 || connection.socketB >= (int)roomB.sockets.size() ||
            used[connection.roomA][connection.socketA] || used[connection.roomB][connection.socketB]) return false;
        const Socket &socketA = roomA.sockets[connection.socketA];
        const Socket &socketB = roomB.sockets[connection.socketB];
        if (!Compatible(socketA.type, socketB.type)) return false;

        Vector3 positionA = SocketPosition(roomA, connection.socketA);
        Vector3 positionB = SocketPosition(roomB, connection.socketB);
        float dx = positionA.x - positionB.x;
        float dz = positionA.z - positionB.z;
        if (dx * dx + dz * dz > 0.0001f || fabsf(positionA.y - positionB.y) > 0.01f) return false;
        float directionA = SocketDirection(roomA, connection.socketA) * DEG2RAD;
        float directionB = SocketDirection(roomB, connection.socketB) * DEG2RAD;
        if (cosf(directionA) * cosf(directionB) + sinf(directionA) * sinf(directionB) > -0.999f) return false;

        used[connection.roomA][connection.socketA] = 1;
        used[connection.roomB][connection.socketB] = 1;
        neighbors[connection.roomA].push_back(connection.roomB);
        neighbors[connection.roomB].push_back(connection.roomA);
    }
    if (used[building.entranceRoom][building.exteriorSocket]) return false;

    std::vector<int> visited(building.rooms.size(), 0);
    std::vector<int> queue(1, building.entranceRoom);
    visited[building.entranceRoom] = 1;
    for (size_t cursor = 0; cursor < queue.size(); cursor++) {
        int room = queue[cursor];
        for (size_t i = 0; i < neighbors[room].size(); i++) {
            int next = neighbors[room][i];
            if (!visited[next]) {
                visited[next] = 1;
                queue.push_back(next);
            }
        }
    }
    for (size_t i = 0; i < visited.size(); i++)
        if (!visited[i]) return false;
    return true;
}

void Building_Draw(const Building &building, bool debug)
{
    // Supports are stored when the plan is placed, making drawing allocation-
    // free and avoiding terrain-height samples in the render loop.
    const Color supportColor = Color{ 104, 104, 98, 255 };
    for (size_t supportIndex = 0; supportIndex < building.supports.size(); supportIndex++) {
        const BuildingSupport &support = building.supports[supportIndex];
        DrawCube(support.position, support.width, support.height, support.width, supportColor);
    }

    std::vector<DebugGeometry> debugGeometry;
    for (size_t roomIndex = 0; roomIndex < building.rooms.size(); roomIndex++) {
        const Room &room = building.rooms[roomIndex];
        DrawPlaceholderModule(building, room, (int)roomIndex,
                              debug ? &debugGeometry : NULL);
        if (!debug) continue;

        float angle = room.rotation * DEG2RAD;
        float halfWidth = (room.width * fabsf(cosf(angle)) + room.depth * fabsf(sinf(angle))) * 0.5f;
        float halfDepth = (room.depth * fabsf(cosf(angle)) + room.width * fabsf(sinf(angle))) * 0.5f;
        Vector3 minimum = Vector3{ building.position.x + room.position.x - halfWidth,
                                   building.position.y, building.position.z + room.position.z - halfDepth };
        Vector3 maximum = Vector3{ building.position.x + room.position.x + halfWidth,
                                   building.position.y + room.position.y + room.height,
                                   building.position.z + room.position.z + halfDepth };
        DrawBoundingBox(BoundingBox{ minimum, maximum }, YELLOW);

        for (size_t socketIndex = 0; socketIndex < room.sockets.size(); socketIndex++) {
            Vector3 marker = WorldPosition(building, room, room.sockets[socketIndex].position);
            marker.y = building.position.y + 1.2f;
            float direction = SocketDirection(room, (int)socketIndex) * DEG2RAD;
            Vector3 end = Vector3{ marker.x + sinf(direction) * 1.5f, marker.y,
                                   marker.z + cosf(direction) * 1.5f };
            bool connected = IsConnected(building, (int)roomIndex, (int)socketIndex);
            bool exterior = (int)roomIndex == building.entranceRoom &&
                            (int)socketIndex == building.exteriorSocket;
            Color color = connected ? GREEN : exterior ? ORANGE : RED;
            DrawSphere(marker, 0.22f, color);
            DrawLine3D(marker, end, color);
        }
    }
    for (size_t doorIndex = 0; doorIndex < building.doors.size(); doorIndex++)
        DrawDoorPanel(building, building.doors[doorIndex], debug);
    if (debug) {
        for (size_t i = 0; i < debugGeometry.size(); i++)
            DrawBoundingBox(debugGeometry[i].bounds, DebugGeometryColor(debugGeometry[i].type));
        ReportDebugIntersections(building, debugGeometry);
    }
}

bool Building_Collides(const Building &building, float x, float z,
                       float radius, float feetY, float height)
{
    // The doorway remains an opening in the wall topology.  Only the panel
    // itself blocks movement, and its collision segment follows its hinge.
    for (size_t doorIndex = 0; doorIndex < building.doors.size(); doorIndex++)
        if (DoorCollides(building, building.doors[doorIndex], x, z, radius, feetY, height))
            return true;

    for (size_t roomIndex = 0; roomIndex < building.rooms.size(); roomIndex++) {
        const Room &room = building.rooms[roomIndex];
        WallPiece pieces[WALL_PIECE_LIMIT];
        int pieceCount = RoomWallPieces(building, (int)roomIndex, pieces);
        for (int pieceIndex = 0; pieceIndex < pieceCount; pieceIndex++) {
            const WallPiece &piece = pieces[pieceIndex];
            float wallMinY = building.position.y + piece.minY;
            float wallMaxY = building.position.y + piece.maxY;
            if (feetY >= wallMaxY || feetY + height <= wallMinY) continue;
            Vector3 localA = RoomToBuilding(room, piece.first);
            Vector3 localB = RoomToBuilding(room, piece.second);
            Vector3 worldA = Vector3{ building.position.x + localA.x, 0.0f,
                                      building.position.z + localA.z };
            Vector3 worldB = Vector3{ building.position.x + localB.x, 0.0f,
                                      building.position.z + localB.z };
            float collisionRadius = radius + WALL_THICKNESS * 0.5f;
            if (SegmentDistanceSquared(x, z, worldA, worldB) < collisionRadius * collisionRadius)
                return true;
        }

        // Floors and ceilings are thin cuboids, not infinitely thin planes.
        // Their vertical faces therefore block a player trying to enter a
        // slab from the side while below or above the room.
        float floorMinY = building.position.y + room.position.y;
        float floorMaxY = floorMinY + FLOOR_THICKNESS;
        float ceilingMaxY = building.position.y + room.position.y + room.height;
        float ceilingMinY = ceilingMaxY - CEILING_THICKNESS;
        bool overlapsFloor = feetY < floorMaxY && feetY + height > floorMinY;
        bool overlapsCeiling = feetY < ceilingMaxY && feetY + height > ceilingMinY;
        // A slab no higher than the player's ordinary ground snap is a step,
        // so test its exact edge. Higher slabs retain the radius-expanded edge
        // and cannot be entered from below as though they were ramps.
        float floorRadius = floorMaxY <= feetY + BUILDING_STEP_HEIGHT ? 0.0f : radius;
        if (overlapsFloor && RoomFootprintCollides(building, room, x, z, floorRadius)) return true;
        if (overlapsCeiling && RoomFootprintCollides(building, room, x, z, radius)) return true;
    }
    return false;
}

float Building_FloorHeight(const Building &building, float x, float z,
                           float currentGround, float maximumHeight)
{
    float ground = currentGround;
    for (size_t roomIndex = 0; roomIndex < building.rooms.size(); roomIndex++) {
        const Room &room = building.rooms[roomIndex];
        if (!RoomContainsPoint(building, room, x, z)) continue;

        float floorTop = building.position.y + room.position.y + FLOOR_THICKNESS;
        float ceilingTop = building.position.y + room.position.y + room.height;
        if (floorTop <= maximumHeight && floorTop > ground) ground = floorTop;
        if (ceilingTop <= maximumHeight && ceilingTop > ground) ground = ceilingTop;
    }
    return ground;
}

float Building_CeilingHeight(const Building &building, float x, float z,
                             float minimumHeight)
{
    float ceiling = FLT_MAX;
    for (size_t roomIndex = 0; roomIndex < building.rooms.size(); roomIndex++) {
        const Room &room = building.rooms[roomIndex];
        if (!RoomContainsPoint(building, room, x, z)) continue;
        float floorUnderside = building.position.y + room.position.y;
        float underside = building.position.y + room.position.y + room.height - CEILING_THICKNESS;
        if (floorUnderside >= minimumHeight && floorUnderside < ceiling)
            ceiling = floorUnderside;
        if (underside >= minimumHeight && underside < ceiling)
            ceiling = underside;
    }
    return ceiling;
}

// Headless self test for --check mode. It intentionally avoids raylib and the window.
int Building_RunSeedCheck(unsigned int count)
{
    int failures = 0;
    size_t minRooms = (size_t)-1;
    size_t maxRooms = 0;
    size_t totalRooms = 0;
    size_t totalLoops = 0;

    for (unsigned int seed = 1; seed <= count; seed++) {
        Building building = Building_Generate(seed);
        if (!Building_ValidatePlan(building)) {
            failures++;
            continue;
        }

        if (building.rooms.size() < minRooms) minRooms = building.rooms.size();
        if (building.rooms.size() > maxRooms) maxRooms = building.rooms.size();
        totalRooms += building.rooms.size();

        size_t roomLoops = 0;
        for (size_t i = 0; i < building.connections.size(); i++) {
            const BuildingConnection &connection = building.connections[i];
            if (connection.roomA != connection.roomB) roomLoops++;
        }
        totalLoops += roomLoops;
    }

    unsigned int ok = count - (unsigned int)failures;
    printf("checked %u seeds, %d failed; rooms min %u avg %.1f max %u; loop doors total %zu\n",
           count, failures,
           (unsigned)(minRooms == (size_t)-1 ? 0 : minRooms),
           ok ? (double)totalRooms / ok : 0.0,
           (unsigned)maxRooms,
           totalLoops);
    return failures;
}

const char *Building_ModuleAssetPath(int templateId)
{
    const RoomDefinition *definition = TemplateById(templateId);
    if (!definition) return "assets/room.glb";
    if (!definition->assetPath.empty()) return definition->assetPath.c_str();
    // Templates without an explicit asset= key fall back to assets/<name>.glb,
    // which matches the historical layout for entrance/hallway/room/bathroom.
    static std::string derivedPath;
    derivedPath = "assets/" + definition->name + ".glb";
    return derivedPath.c_str();
}
