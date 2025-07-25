/* We simply call the root header file "App.h", giving you uWS::App and uWS::SSLApp */
#include "App.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include "json.hpp"
#include <atomic>

/* ws->getUserData returns one of these */
struct PerSocketData
{
    std::string ip;            /** Client IP */
    std::string roomType;
    std::string roomId;
    std::string roomName;
    std::string id;            /** Websocket Id */
};

/** Room Data */
struct RoomData
{
    std::string roomName;
    long long createTime;
    std::string roomType;
    std::string roomId;
    int connections;

    /** Function to convert structure to json */
    nlohmann::json toJson() const {
        return {
            {"roomName", roomName},
            {"connections", connections},
            {"roomId", roomId},
            {"roomType", roomType},
            {"createTime", createTime}
        };
    }
};

/** Error & Broadcast Messages */
constexpr const char* ACCESS_DENIED = "Access Denied";
constexpr const char* RATE_LIMIT_EXCEEDED = "Rate Limit Exceeded";
constexpr const char* RESOURCE_NOT_FOUND = "Resource Not Found";
constexpr const char* YOU_ARE_CONNECTED_TO_THE_ROOM = "YOU_ARE_CONNECTED_TO_THE_ROOM";
constexpr const char* STRANGER_CONNECTED_TO_THE_ROOM = "STRANGER_CONNECTED_TO_THE_ROOM";
constexpr const char* PAIRED = "PAIRED";
constexpr const char* INITIATOR = "INITIATOR";
constexpr const char* ROOM_NOT_FOUND = "ROOM_NOT_FOUND";
constexpr const char* STRANGER_DISCONNECTED_FROM_THE_ROOM = "STRANGER_DISCONNECTED_FROM_THE_ROOM";
constexpr const char* PEER_DISCONNECTED = "PEER_DISCONNECTED";

/** Other Constants */
constexpr int MAX_CONNECTIONS_ALLOWED_FROM_SINGLE_IP = 3;

/** Room Codes */
constexpr const char* PRIVATE_TEXT_CHAT_DUO = "0";
constexpr const char* PRIVATE_VIDEO_CHAT_DUO = "1";
constexpr const char* PUBLIC_TEXT_CHAT_MULTI = "2";
constexpr const char* PRIVATE_TEXT_CHAT_MULTI = "3";

/** Data Structures */
constexpr std::array<std::string_view, 4> allowedRoomTypes = {
    PRIVATE_TEXT_CHAT_DUO, 
    PRIVATE_VIDEO_CHAT_DUO, 
    PUBLIC_TEXT_CHAT_MULTI, 
    PRIVATE_TEXT_CHAT_MULTI
};

const std::unordered_set<std::string_view> allowedOrigins = {"https://picolon.com"};

// Thread-safe data structures
std::unordered_map<std::string, int> connectionsPerIp;
std::unordered_map<std::string, std::string> socketIdToRoomType;
std::unordered_map<std::string, std::string> socketIdToRoomId;
std::unordered_map<std::string, std::unordered_set<uWS::WebSocket<true, true, PerSocketData>*>> textChatMultiRoomIdToSockets;
std::unordered_map<std::string, std::unordered_set<uWS::WebSocket<true, true, PerSocketData>*>> textChatDuoRoomIdToSockets;
std::unordered_map<std::string, std::unordered_set<uWS::WebSocket<true, true, PerSocketData>*>> videoChatDuoRoomIdToSockets;
std::unordered_map<std::string, RoomData> publicRoomIdToRoomData;
std::unordered_map<std::string, RoomData> privateRoomIdToRoomData;
std::vector<uWS::WebSocket<true, true, PerSocketData>*> doubleChatRoomWaitingPeople;
std::vector<uWS::WebSocket<true, true, PerSocketData>*> doubleVideoRoomWaitingPeople;

/** Mutex for thread-safety */
std::mutex sharedMutex;

/** Global Thread-Safe Variables */
std::atomic<int> connections(0);
std::atomic<int> idCounter(0);

int generateUniqueID() {
    return idCounter.fetch_add(1);
}

/** Set Response Headers */
void setResponseHeaders(auto *res, const std::string& origin) {
    res->writeHeader("Access-Control-Allow-Origin", origin);
    res->writeHeader("Access-Control-Allow-Methods", "GET, OPTIONS, POST");
    res->writeHeader("Access-Control-Allow-Headers", "Content-Type");

    // Security headers
    res->writeHeader("Content-Security-Policy", "default-src 'self'; img-src 'self' https://picolon.com; script-src 'self'; style-src 'self';");
    res->writeHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
    res->writeHeader("X-Content-Type-Options", "nosniff");
    res->writeHeader("X-Frame-Options", "DENY");
    res->writeHeader("X-XSS-Protection", "1; mode=block");
    res->writeHeader("Referrer-Policy", "no-referrer");
    res->writeHeader("Permissions-Policy", "geolocation=(self)");
}

void reconnect(uWS::WebSocket<true, true, PerSocketData> *ws, bool isNewConnection = false)
{
    try
    {
        if(isNewConnection){
            connections++; 
            connectionsPerIp[(ws->getUserData())->ip]++;
        }

        auto userData = ws->getUserData();

        socketIdToRoomType.emplace(userData->id, userData->roomType);

        if (userData->roomType == PUBLIC_TEXT_CHAT_MULTI || userData->roomType == PRIVATE_TEXT_CHAT_MULTI)
        {
            bool isPublicRoom = (userData->roomType == PUBLIC_TEXT_CHAT_MULTI);
            auto &roomMap = isPublicRoom ? publicRoomIdToRoomData : privateRoomIdToRoomData;

            if (!userData->roomName.empty())
            {
                std::string roomId = std::to_string(generateUniqueID());
                socketIdToRoomId.emplace(userData->id, roomId);
                textChatMultiRoomIdToSockets.emplace(roomId, std::unordered_set<uWS::WebSocket<true, true, PerSocketData> *>{ws});

                RoomData roomData;

                roomData.roomName = userData->roomName;
                roomData.connections = 1;
                roomData.roomId = roomId;
                roomData.roomType = userData->roomType;
                roomData.createTime = generateUniqueID();

                roomMap.emplace(roomId, roomData);

                nlohmann::json response = {
                    {"type", YOU_ARE_CONNECTED_TO_THE_ROOM},
                    {"roomData", roomData.toJson()}
                };

                ws->subscribe(roomId);
                ws->send(response.dump(), uWS::OpCode::TEXT, false);
            }
            else if (!userData->roomId.empty())
            {
                auto it = roomMap.find(userData->roomId);

                if (it != roomMap.end())
                {
                    RoomData roomData = it->second;
                    auto &socketsInRoom = textChatMultiRoomIdToSockets[userData->roomId];
                    socketsInRoom.insert(ws);

                    socketIdToRoomId.emplace(userData->id, userData->roomId);
                    textChatMultiRoomIdToSockets[userData->roomId] = socketsInRoom;

                    /** Updating the connection count in room */
                    roomData.connections++;

                    /** Assigning the updated value to the map */
                    roomMap[userData->roomId] = roomData;

                    nlohmann::json response = {
                        {"type", YOU_ARE_CONNECTED_TO_THE_ROOM},
                        {"roomData", roomData.toJson()}
                    };

                    /** Publish message to the room */
                    nlohmann::json publishMessage = {
                        {"type", STRANGER_CONNECTED_TO_THE_ROOM}
                    };

                    ws->subscribe(userData->roomId);
                    ws->send(response.dump(), uWS::OpCode::TEXT, false);
                    ws->publish(userData->roomId, publishMessage.dump(), uWS::OpCode::TEXT, false);
                }
                else
                {
                    nlohmann::json response = {
                        {"type", ROOM_NOT_FOUND}
                    };

                    ws->send(response.dump(), uWS::OpCode::TEXT, false);
                }
            }
        }
        else
        {
            auto &waitingPeople = (userData->roomType == PRIVATE_TEXT_CHAT_DUO ? doubleChatRoomWaitingPeople : doubleVideoRoomWaitingPeople);
            auto &rooms = (userData->roomType == PRIVATE_TEXT_CHAT_DUO ? textChatDuoRoomIdToSockets : videoChatDuoRoomIdToSockets);

            if (!waitingPeople.empty())
            {
                uWS::WebSocket<true, true, PerSocketData> *peerSocket = waitingPeople.back();
                waitingPeople.pop_back();

                std::string roomId = std::to_string(generateUniqueID());

                rooms.emplace(roomId, std::unordered_set<uWS::WebSocket<true, true, PerSocketData> *>{ws, peerSocket});
                socketIdToRoomId.emplace(userData->id, roomId);
                socketIdToRoomId.emplace(peerSocket->getUserData()->id, roomId);

                peerSocket->subscribe(roomId);
                ws->subscribe(roomId);

                nlohmann::json duoRoomConnectedMessage = {
                    {"type", PAIRED},
                    {"message", "You are connected to Stranger"}
                };

                std::string duoMessageStr = duoRoomConnectedMessage.dump();
                ws->send(duoMessageStr, uWS::OpCode::TEXT, false);
                peerSocket->send(duoMessageStr, uWS::OpCode::TEXT, false);

                if (userData->roomType == PRIVATE_VIDEO_CHAT_DUO)
                {
                    nlohmann::json initiatorMessage = {
                        {"type", INITIATOR},
                        {"message", "You are the initiator!"}
                    };

                    ws->send(initiatorMessage.dump(), uWS::OpCode::TEXT, false);
                }
            }
            else
            {
                waitingPeople.push_back(ws);
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error in reconnect: " << e.what() << std::endl;
    }
}

void handleDisconnect(uWS::WebSocket<true, true, PerSocketData> *ws)
{
    std::unique_lock<std::mutex> lock(sharedMutex);

    try
    {
        connections--; 
        connectionsPerIp[(ws->getUserData())->ip]--;

        std::string roomId = socketIdToRoomId[ws->getUserData()->id];
        socketIdToRoomId.erase(ws->getUserData()->id);
        std::string roomType = socketIdToRoomType[ws->getUserData()->id];
        socketIdToRoomType.erase(ws->getUserData()->id);

        if (!roomId.empty() && (roomType == PUBLIC_TEXT_CHAT_MULTI || roomType == PRIVATE_TEXT_CHAT_MULTI))
        {
            std::unordered_set<uWS::WebSocket<true, true, PerSocketData> *> socketsInRoom;

            auto it = textChatMultiRoomIdToSockets.find(roomId);

            if (it != textChatMultiRoomIdToSockets.end())
            {
                socketsInRoom = it->second;
            }

            if (!socketsInRoom.empty())
            {
                socketsInRoom.erase(ws);

                if (socketsInRoom.empty())
                {
                    textChatMultiRoomIdToSockets.erase(roomId);

                    if (roomType == PRIVATE_TEXT_CHAT_MULTI)
                    {
                        privateRoomIdToRoomData.erase(roomId);
                    }
                    else
                    {
                        publicRoomIdToRoomData.erase(roomId);
                    }
                }
                else
                {
                    RoomData roomData;

                    if (roomType == PRIVATE_TEXT_CHAT_MULTI)
                    {
                        roomData = privateRoomIdToRoomData[roomId];
                    }
                    else
                    {
                        roomData = publicRoomIdToRoomData[roomId];
                    }

                    roomData.connections--;

                    if (roomType == PRIVATE_TEXT_CHAT_MULTI)
                    {
                        privateRoomIdToRoomData[roomId] = roomData;
                    }
                    else
                    {
                        publicRoomIdToRoomData[roomId] = roomData;
                    }

                    textChatMultiRoomIdToSockets[roomId] = socketsInRoom;

                    nlohmann::json jsonMessage = {
                        {"type", STRANGER_DISCONNECTED_FROM_THE_ROOM}
                    };

                    std::string message = jsonMessage.dump();

                    for (auto *socket : socketsInRoom)
                    {
                        socket->send(message, uWS::OpCode::TEXT, false);
                    }
                }
            }
        }
        else
        {
            if (!roomId.empty())
            {
                auto &rooms = (roomType == PRIVATE_TEXT_CHAT_DUO) ? textChatDuoRoomIdToSockets : videoChatDuoRoomIdToSockets;
                auto &pair = rooms[roomId];

                uWS::WebSocket<true, true, PerSocketData> *remainingSocket = nullptr;

                for (auto *socket : pair)
                {
                    if (socket != ws)
                    {
                        remainingSocket = socket;
                        break;
                    }
                }

                nlohmann::json jsonMessage = {
                    {"type", PEER_DISCONNECTED},
                    {"message", "Your peer is disconnected"}
                };

                remainingSocket->send(jsonMessage.dump(), uWS::OpCode::TEXT, false);

                rooms.erase(roomId);
                socketIdToRoomId.erase(remainingSocket->getUserData()->id);

                reconnect(remainingSocket);
            }
            else
            {
                auto &waitingPeople = (roomType == PRIVATE_TEXT_CHAT_DUO) ? doubleChatRoomWaitingPeople : doubleVideoRoomWaitingPeople;
                waitingPeople.erase(std::remove(waitingPeople.begin(), waitingPeople.end(), ws), waitingPeople.end());
            }
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error in handleDisconnect: " << e.what() << std::endl;
    }
}

/* This is a simple WebSocket "sync" upgrade example.
 * You may compile it with "WITH_OPENSSL=1 make" or with "make" */

int main() {
    uWS::SSLApp({
        /* There are example certificates in uWebSockets.js repo */
	    .key_file_name = "ssl/private.key",
	    .cert_file_name = "ssl/certificate.crt",
	}).ws<PerSocketData>("/*", {
        /* Settings */
        .compression = uWS::SHARED_COMPRESSOR,
        .maxPayloadLength = 1048576,
        .idleTimeout = 10,
        .maxBackpressure = 1 * 1024 * 1024,
        /* Handlers */
        .upgrade = [](auto *res, auto *req, auto *context) {
            std::string ip = std::string(res->getRemoteAddressAsText());
            std::string roomType = std::string(req->getQuery("RT"));
            std::string roomName = std::string(req->getQuery("RN"));
            std::string roomId = std::string(req->getQuery("RID"));
            std::string socketId = std::string(req->getHeader("sec-websocket-key"));

            int ipCount = connectionsPerIp[ip];

            if (ipCount >= MAX_CONNECTIONS_ALLOWED_FROM_SINGLE_IP) {
                res->writeStatus("403 Forbidden")->end(ACCESS_DENIED);
                return;
            }

            if (std::find(allowedRoomTypes.begin(), allowedRoomTypes.end(), roomType) == allowedRoomTypes.end()) {
                res->writeStatus("403 Forbidden")->end(ACCESS_DENIED);
                return;
            }

            if (roomType == PUBLIC_TEXT_CHAT_MULTI || roomType == PRIVATE_TEXT_CHAT_MULTI) {
                if(!roomName.empty()){
                    if(roomName.length() > 160){
                        res->writeStatus("403 Forbidden")->end(ACCESS_DENIED);
                        return;
                    }
                } else if(roomId.empty()){
                    res->writeStatus("403 Forbidden")->end(ACCESS_DENIED);
                    return;
                } 
            }

            res->template upgrade<PerSocketData>({
                /* We initialize PerSocketData struct here */
                .ip = ip,
                .roomType = roomType,
                .roomId = roomId,
                .roomName = roomName,
                .id = socketId
            }, req->getHeader("sec-websocket-key"),
                req->getHeader("sec-websocket-protocol"),
                req->getHeader("sec-websocket-extensions"),
                context
            );
        },
        .open = [](auto *ws) {
            std::thread reconnectThread([ws]() {
                std::unique_lock<std::mutex> lock(sharedMutex);

                reconnect(ws, true);  // Call reconnect in a new thread
            });

            reconnectThread.join();
        },
        .message = [](auto *ws, std::string_view message, uWS::OpCode opCode) {
            /* We simply echo whatever data we get */
            std::string roomId = socketIdToRoomId[ws->getUserData()->id];
            if (!roomId.empty()) ws->publish(roomId, message, opCode, false);
        },
        .drain = [](auto */*ws*/) {
            /* Check ws->getBufferedAmount() here */
        },
        .close = [](auto *ws, int /*code*/, std::string_view /*message*/) {
            std::thread disconnectThread([ws]() {
                handleDisconnect(ws);  
            });

            disconnectThread.join();
        }
    }).get("/api/v1/connections", [](auto *res, auto *req) {
	    std::string clientIp = std::string(req->getHeader("x-forwarded-for"));
        if (clientIp.empty()) {
            clientIp = std::string(req->getHeader("remote-address"));
        }

        std::string origin = std::string(req->getHeader("origin"));

        if (allowedOrigins.find(origin) != allowedOrigins.end()) {
            setResponseHeaders(res, origin);
            res->end(std::to_string(connections));
        } else {
            nlohmann::json response;
            response["error"] = ACCESS_DENIED;
            response["message"] = "You do not have permission to access this resource.";
            response["code"] = 403;
            res->writeStatus("403 Forbidden")->writeHeader("Content-Type", "application/json")->end(response.dump());
        }
	}).get("/api/v1/public-text-chat-rooms", [](auto *res, auto *req) {
	    std::string clientIp = std::string(req->getHeader("x-forwarded-for"));
        if (clientIp.empty()) {
            clientIp = std::string(req->getHeader("remote-address"));
        }

        std::string origin = std::string(req->getHeader("origin"));

        if (allowedOrigins.find(origin) != allowedOrigins.end()) {
            setResponseHeaders(res, origin);
            std::vector<nlohmann::json> rooms;
            for (const auto& pair : publicRoomIdToRoomData) {
                rooms.push_back(pair.second.toJson());
            }

            nlohmann::json response = rooms;
            res->end(response.dump());
        } else {
            nlohmann::json response;
            response["error"] = ACCESS_DENIED;
            response["message"] = "You do not have permission to access this resource.";
            response["code"] = 403;
            res->writeStatus("403 Forbidden")->writeHeader("Content-Type", "application/json")->end(response.dump());
        }
	}).any("/*", [](auto *res, auto /** *req */) {
        nlohmann::json response = {
            {"error", RESOURCE_NOT_FOUND},
            {"message", "The requested resource could not be found."},
            {"code", 404}
        };

        std::string responseBody = response.dump();
        res->writeStatus("404 Not Found")->writeHeader("Content-Type", "application/json")->end(responseBody);
    }).listen(443, [](auto *listen_socket) {
        if (listen_socket) {
            std::cout << "Listening on port " << 443 << std::endl;
        }
    }).run();
}
