#include "building.h"
#include <math.h>

static const float GRID_SIZE = 2.5f;
static const float MODULE_HEIGHT = 3.0f;
static const float WALL_THICKNESS = 0.2f;
static const float DOOR_WIDTH = 1.6f;
static const float DOOR_HEIGHT = 2.2f;
static const float BUILDING_LIMIT = 35.0f;

struct WallPiece {
    Vector3 first;
    Vector3 second;
    float minY;
    float maxY;
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
    if (type == ROOM_HALLWAY || type == ROOM_BATHROOM) return 5;
    return 10;
}

static int RoomDepth(RoomType type)
{
    if (type == ROOM_BATHROOM) return 5;
    return 10;
}

static Room MakeRoom(RoomType type, Vector3 position, float rotation)
{
    Room room = {};
    room.type = type;
    room.position = position;
    room.rotation = rotation;
    room.width = RoomWidth(type);
    room.depth = RoomDepth(type);
    SocketType socketType = type == ROOM_HALLWAY ? SOCKET_CORRIDOR : SOCKET_DOOR;
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

static int RoomWallPieces(const Building &building, int roomIndex, WallPiece *pieces)
{
    const Room &room = building.rooms[roomIndex];
    int pieceCount = 0;
    for (int side = 0; side < 4; side++) {
        float span = side < 2 ? (float)room.width : (float)room.depth;
        float edge = side == 0 ? room.depth * 0.5f :
                     side == 1 ? -room.depth * 0.5f :
                     side == 2 ? room.width * 0.5f : -room.width * 0.5f;
        bool open = SocketIsOpen(building, roomIndex, side);
        float gap = room.type == ROOM_BATHROOM ? 1.2f : DOOR_WIDTH;
        if (!open) {
            AddWallPiece(pieces, &pieceCount, side, edge, -span * 0.5f, span * 0.5f,
                         0.0f, MODULE_HEIGHT);
            continue;
        }
        AddWallPiece(pieces, &pieceCount, side, edge, -span * 0.5f, -gap * 0.5f,
                     0.0f, MODULE_HEIGHT);
        AddWallPiece(pieces, &pieceCount, side, edge, gap * 0.5f, span * 0.5f,
                     0.0f, MODULE_HEIGHT);
        AddWallPiece(pieces, &pieceCount, side, edge, -gap * 0.5f, gap * 0.5f,
                     DOOR_HEIGHT, MODULE_HEIGHT);
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
static void DrawRoomCube(const Building &building, const Room &room,
                         Vector3 localCenter, Vector3 localSize, Color color)
{
    float angle = room.rotation * DEG2RAD;
    Vector3 size = Vector3{
        fabsf(cosf(angle)) * localSize.x + fabsf(sinf(angle)) * localSize.z,
        localSize.y,
        fabsf(cosf(angle)) * localSize.z + fabsf(sinf(angle)) * localSize.x
    };
    DrawCubeV(WorldPosition(building, room, localCenter), size, color);
}

static void DrawDoorFrames(const Building &building, const Room &room, int roomIndex)
{
    const Color trim = Color{ 101, 83, 64, 255 };
    const float gap = room.type == ROOM_BATHROOM ? 1.2f : DOOR_WIDTH;
    for (int side = 0; side < 4; side++) {
        if (!SocketIsOpen(building, roomIndex, side)) continue;
        float edge = side == 0 ? room.depth * 0.5f :
                     side == 1 ? -room.depth * 0.5f :
                     side == 2 ? room.width * 0.5f : -room.width * 0.5f;
        if (side < 2) {
            DrawRoomCube(building, room, Vector3{ -gap * 0.5f, DOOR_HEIGHT * 0.5f, edge },
                         Vector3{ 0.12f, DOOR_HEIGHT, WALL_THICKNESS * 1.35f }, trim);
            DrawRoomCube(building, room, Vector3{  gap * 0.5f, DOOR_HEIGHT * 0.5f, edge },
                         Vector3{ 0.12f, DOOR_HEIGHT, WALL_THICKNESS * 1.35f }, trim);
            DrawRoomCube(building, room, Vector3{ 0.0f, DOOR_HEIGHT, edge },
                         Vector3{ gap + 0.18f, 0.12f, WALL_THICKNESS * 1.35f }, trim);
            DrawRoomCube(building, room, Vector3{ 0.0f, 0.12f, edge },
                         Vector3{ gap, 0.08f, 0.36f }, ShadeColor(trim, 0.72f));
        } else {
            DrawRoomCube(building, room, Vector3{ edge, DOOR_HEIGHT * 0.5f, -gap * 0.5f },
                         Vector3{ WALL_THICKNESS * 1.35f, DOOR_HEIGHT, 0.12f }, trim);
            DrawRoomCube(building, room, Vector3{ edge, DOOR_HEIGHT * 0.5f,  gap * 0.5f },
                         Vector3{ WALL_THICKNESS * 1.35f, DOOR_HEIGHT, 0.12f }, trim);
            DrawRoomCube(building, room, Vector3{ edge, DOOR_HEIGHT, 0.0f },
                         Vector3{ WALL_THICKNESS * 1.35f, 0.12f, gap + 0.18f }, trim);
            DrawRoomCube(building, room, Vector3{ edge, 0.12f, 0.0f },
                         Vector3{ 0.36f, 0.08f, gap }, ShadeColor(trim, 0.72f));
        }
    }
}

static void DrawPlaceholderModule(const Building &building, const Room &room, int roomIndex)
{
    Color floorColor = room.type == ROOM_ENTRANCE ? Color{ 133, 121, 96, 255 } :
                       room.type == ROOM_HALLWAY ? Color{ 122, 133, 131, 255 } :
                       room.type == ROOM_BATHROOM ? Color{ 105, 147, 154, 255 } :
                       Color{ 145, 133, 112, 255 };
    DrawRoomCube(building, room, Vector3{ 0.0f, 0.08f, 0.0f },
                 Vector3{ (float)room.width, 0.16f, (float)room.depth }, floorColor);
    DrawRoomCube(building, room, Vector3{ 0.0f, MODULE_HEIGHT - 0.08f, 0.0f },
                 Vector3{ (float)room.width, 0.16f, (float)room.depth },
                 Color{ 184, 184, 174, 255 });

    WallPiece pieces[12];
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
    }
    DrawDoorFrames(building, room, roomIndex);
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

Building Building_GenerateTestBuilding(unsigned int seed)
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
    return building;
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
    for (size_t roomIndex = 0; roomIndex < building.rooms.size(); roomIndex++) {
        const Room &room = building.rooms[roomIndex];
        DrawPlaceholderModule(building, room, (int)roomIndex);
        if (!debug) continue;

        float angle = room.rotation * DEG2RAD;
        float halfWidth = (room.width * fabsf(cosf(angle)) + room.depth * fabsf(sinf(angle))) * 0.5f;
        float halfDepth = (room.depth * fabsf(cosf(angle)) + room.width * fabsf(sinf(angle))) * 0.5f;
        Vector3 minimum = Vector3{ building.position.x + room.position.x - halfWidth,
                                   building.position.y, building.position.z + room.position.z - halfDepth };
        Vector3 maximum = Vector3{ building.position.x + room.position.x + halfWidth,
                                   building.position.y + MODULE_HEIGHT,
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
}

bool Building_Collides(const Building &building, float x, float z,
                       float radius, float feetY, float height)
{
    for (size_t roomIndex = 0; roomIndex < building.rooms.size(); roomIndex++) {
        const Room &room = building.rooms[roomIndex];
        WallPiece pieces[12];
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
    }
    return false;
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
