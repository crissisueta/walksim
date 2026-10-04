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
struct RoomDefinition {
    int width;
    int depth;
    float height;
    SocketType socketType;
    std::vector<RoomWindow> windows;
};

static const int ROOM_TYPE_COUNT = 4;
static RoomDefinition g_roomDefinitions[ROOM_TYPE_COUNT];
static bool g_roomDefinitionsInitialized = false;

const char *Building_RoomTypeName(RoomType type)
{
    switch (type) {
        case ROOM_ENTRANCE: return "entrance";
        case ROOM_HALLWAY: return "hallway";
        case ROOM_ROOM: return "room";
        case ROOM_BATHROOM: return "bathroom";
        default: return "unknown";
    }
}

static int RoomTypeFromName(const char *name)
{
    for (int type = 0; type < ROOM_TYPE_COUNT; type++)
        if (strcmp(name, Building_RoomTypeName((RoomType)type)) == 0) return type;
    return -1;
}

static void SetDefaultRoomDefinitions(RoomDefinition *definitions)
{
    definitions[ROOM_ENTRANCE] = RoomDefinition{ 10, 10, 3.0f, SOCKET_DOOR, std::vector<RoomWindow>() };
    definitions[ROOM_HALLWAY] = RoomDefinition{ 5, 10, 3.0f, SOCKET_CORRIDOR, std::vector<RoomWindow>() };
    definitions[ROOM_ROOM] = RoomDefinition{ 10, 10, 3.0f, SOCKET_DOOR, std::vector<RoomWindow>() };
    definitions[ROOM_BATHROOM] = RoomDefinition{ 5, 5, 3.0f, SOCKET_DOOR, std::vector<RoomWindow>() };
}

static char *Trim(char *text)
{
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static bool ParsePositiveInteger(const char *text, int *value)
{
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (!end || *Trim(end) || parsed <= 0 || parsed > 1000) return false;
    *value = (int)parsed;
    return true;
}

static bool ParsePositiveFloat(const char *text, float *value)
{
    char *end = NULL;
    float parsed = strtof(text, &end);
    if (!end || *Trim(end) || parsed <= 0.0f || parsed > 1000.0f) return false;
    *value = parsed;
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

bool Building_LoadRoomDefinitions(const char *path)
{
    RoomDefinition definitions[ROOM_TYPE_COUNT];
    SetDefaultRoomDefinitions(definitions);
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Building definitions: could not open %s; using built-in defaults.\n", path);
        g_roomDefinitionsInitialized = true;
        SetDefaultRoomDefinitions(g_roomDefinitions);
        return false;
    }

    int fields[ROOM_TYPE_COUNT] = {};
    int currentType = -1;
    int lineNumber = 0;
    char line[256];
    bool valid = true;
    while (fgets(line, sizeof(line), file)) {
        lineNumber++;
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *text = Trim(line);
        if (!*text) continue;

        size_t length = strlen(text);
        if (text[0] == '[' && length > 2 && text[length - 1] == ']') {
            text[length - 1] = '\0';
            currentType = RoomTypeFromName(Trim(text + 1));
            if (currentType < 0) {
                // Extra templates are useful to keep beside the active four
                // types, even though the current enum cannot select them yet.
                fprintf(stderr, "Building definitions:%d: [%s] is not used by the current generator; ignoring it.\n",
                        lineNumber, text + 1);
                currentType = -2;
            }
            continue;
        }

        char *equals = strchr(text, '=');
        if (currentType == -2) continue;
        if (currentType < 0 || !equals) {
            fprintf(stderr, "Building definitions:%d: expected [room_type] or key=value.\n", lineNumber);
            valid = false;
            continue;
        }
        *equals = '\0';
        char *key = Trim(text);
        char *value = Trim(equals + 1);
        bool parsed = false;
        if (strcmp(key, "width") == 0) {
            parsed = ParsePositiveInteger(value, &definitions[currentType].width);
            fields[currentType] |= 1;
        } else if (strcmp(key, "depth") == 0) {
            parsed = ParsePositiveInteger(value, &definitions[currentType].depth);
            fields[currentType] |= 2;
        } else if (strcmp(key, "height") == 0) {
            parsed = ParsePositiveFloat(value, &definitions[currentType].height);
            fields[currentType] |= 4;
        } else if (strcmp(key, "socket") == 0) {
            if (strcmp(value, "door") == 0) definitions[currentType].socketType = SOCKET_DOOR;
            else if (strcmp(value, "corridor") == 0) definitions[currentType].socketType = SOCKET_CORRIDOR;
            else {
                fprintf(stderr, "Building definitions:%d: socket must be door or corridor.\n", lineNumber);
                valid = false;
                continue;
            }
            parsed = true;
            fields[currentType] |= 8;
        } else if (strcmp(key, "window") == 0) {
            RoomWindow window = {};
            parsed = ParseWindow(value, &window);
            if (parsed) definitions[currentType].windows.push_back(window);
        } else {
            fprintf(stderr, "Building definitions:%d: unknown key '%s'.\n", lineNumber, key);
            valid = false;
            continue;
        }
        if (!parsed) {
            fprintf(stderr, "Building definitions:%d: invalid %s value '%s'.\n", lineNumber, key, value);
            valid = false;
        }
    }
    fclose(file);

    for (int type = 0; type < ROOM_TYPE_COUNT; type++) {
        if (fields[type] != 15) {
            fprintf(stderr, "Building definitions: [%s] must define width, depth, height, and socket.\n",
                    Building_RoomTypeName((RoomType)type));
            valid = false;
        }
        const RoomDefinition &definition = definitions[type];
        for (size_t windowIndex = 0; windowIndex < definition.windows.size(); windowIndex++) {
            const RoomWindow &window = definition.windows[windowIndex];
            float span = window.side < ROOM_SIDE_EAST ? (float)definition.width : (float)definition.depth;
            if (fabsf(window.offset) + window.width * 0.5f > span * 0.5f - WALL_THICKNESS ||
                window.bottom + window.height > definition.height - CEILING_THICKNESS) {
                fprintf(stderr, "Building definitions: [%s] has a window outside its wall bounds.\n",
                        Building_RoomTypeName((RoomType)type));
                valid = false;
            }
        }
    }
    g_roomDefinitionsInitialized = true;
    if (!valid) {
        fprintf(stderr, "Building definitions: using built-in defaults because %s is invalid.\n", path);
        SetDefaultRoomDefinitions(g_roomDefinitions);
        return false;
    }
    for (int type = 0; type < ROOM_TYPE_COUNT; type++)
        g_roomDefinitions[type] = definitions[type];
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

static int RoomWidth(RoomType type)
{
    EnsureRoomDefinitionsLoaded();
    return g_roomDefinitions[type].width;
}

static int RoomDepth(RoomType type)
{
    EnsureRoomDefinitionsLoaded();
    return g_roomDefinitions[type].depth;
}

static float RoomHeight(RoomType type)
{
    EnsureRoomDefinitionsLoaded();
    return g_roomDefinitions[type].height;
}

static SocketType RoomSocketType(RoomType type)
{
    EnsureRoomDefinitionsLoaded();
    return g_roomDefinitions[type].socketType;
}

static Room MakeRoom(RoomType type, Vector3 position, float rotation)
{
    Room room = {};
    room.type = type;
    room.position = position;
    room.rotation = rotation;
    room.width = RoomWidth(type);
    room.depth = RoomDepth(type);
    room.height = RoomHeight(type);
    SocketType socketType = RoomSocketType(type);
    room.windows = g_roomDefinitions[type].windows;
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
                      RoomType type, int candidateSocket)
{
    const Room &parent = building->rooms[parentRoom];
    const Socket &parentConnection = parent.sockets[parentSocket];
    Room candidate = MakeRoom(type, Vector3{ 0.0f, 0.0f, 0.0f }, 0.0f);
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
        float gap = room.type == ROOM_BATHROOM ? 1.2f : DOOR_WIDTH;
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
        float width = room.type == ROOM_BATHROOM ? 1.2f : DOOR_WIDTH;
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
    float width = entrance.type == ROOM_BATHROOM ? 1.2f : DOOR_WIDTH;
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

        float gap = room.type == ROOM_BATHROOM ? 1.2f : DOOR_WIDTH;
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

static void DrawPlaceholderModule(const Building &building, const Room &room, int roomIndex,
                                  std::vector<DebugGeometry> *debugGeometry)
{
    Color floorColor = room.type == ROOM_ENTRANCE ? Color{ 133, 121, 96, 255 } :
                       room.type == ROOM_HALLWAY ? Color{ 122, 133, 131, 255 } :
                       room.type == ROOM_BATHROOM ? Color{ 105, 147, 154, 255 } :
                       Color{ 145, 133, 112, 255 };
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

Building Building_Generate(unsigned int seed)
{
    Building building = {};
    building.type = BUILDING_GENERIC;
    building.seed = seed;
    building.entranceRoom = 0;
    building.exteriorSocket = 1;
    unsigned int state = seed ? seed : 1;

    building.rooms.push_back(MakeRoom(ROOM_ENTRANCE, Vector3{ 0.0f, 0.0f, 0.0f }, 0.0f));
    TryAttach(&building, 0, 0, ROOM_HALLWAY, 1);
    if (building.rooms.size() > 1) {
        TryAttach(&building, 1, 2, ROOM_ROOM, 3);
        TryAttach(&building, 1, 3, ROOM_ROOM, 2);
    }

    int extraModules = 2 + (int)(NextRandom(&state) % 5u);
    for (int added = 0, attempts = 0; added < extraModules && attempts < 80; attempts++) {
        int parentRoom = (int)(NextRandom(&state) % building.rooms.size());
        int parentSocket = (int)(NextRandom(&state) % 4u);
        if (parentRoom == building.entranceRoom && parentSocket == building.exteriorSocket) continue;
        if (IsConnected(building, parentRoom, parentSocket)) continue;

        unsigned int choice = NextRandom(&state) % 100u;
        RoomType type = choice < 22u ? ROOM_HALLWAY :
                        choice < 38u ? ROOM_BATHROOM : ROOM_ROOM;
        int socket = (parentSocket + 2) % 4;
        if (TryAttach(&building, parentRoom, parentSocket, type, socket)) added++;
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
        if (room.width != RoomWidth(room.type) || room.depth != RoomDepth(room.type) ||
            fabsf(room.height - RoomHeight(room.type)) > 0.001f ||
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

const char *Building_ModuleAssetPath(RoomType type)
{
    switch (type) {
        case ROOM_ENTRANCE: return "assets/entrance.glb";
        case ROOM_HALLWAY: return "assets/hallway.glb";
        case ROOM_BATHROOM: return "assets/bathroom.glb";
        default: return "assets/room.glb";
    }
}
