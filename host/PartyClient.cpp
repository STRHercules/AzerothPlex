#include "PartyClient.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <random>
#include <vector>

namespace wxl_party
{
    namespace
    {
        constexpr DWORD kReceiveBufferBytes = static_cast<DWORD>(kMaxPartyMessageBytes);

        bool IsSecureRelay(std::string_view url)
        {
            return url.rfind("wss://", 0) == 0 || url.rfind("https://", 0) == 0;
        }

        std::string HttpRelayUrl(std::string url)
        {
            if (url.rfind("ws://", 0) == 0) return "http://" + url.substr(5);
            if (url.rfind("wss://", 0) == 0) return "https://" + url.substr(6);
            return url;
        }
    }

    PartyClient::PartyClient(SessionSettings settings)
        : settings_(std::move(settings)), clientId_(MakeClientId()),
          worker_([this](std::stop_token stop) { Worker(stop); })
    {
    }

    PartyClient::~PartyClient()
    {
        Disconnect();
        worker_.request_stop();
        outgoingChanged_.notify_all();
    }

    int64_t PartyClient::NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string PartyClient::MakeClientId()
    {
        std::random_device random;
        const uint64_t a = (static_cast<uint64_t>(random()) << 32) ^ random();
        const uint64_t b = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
                           static_cast<uint64_t>(GetTickCount64());
        char buffer[48]{};
        std::snprintf(buffer, sizeof(buffer), "ap-%08lx-%016llx",
                      static_cast<unsigned long>(a & 0xffffffffu),
                      static_cast<unsigned long long>(a ^ b));
        return buffer;
    }

    std::wstring PartyClient::Wide(std::string_view value)
    {
        if (value.empty()) return {};
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               value.data(), static_cast<int>(value.size()),
                                               nullptr, 0);
        if (length <= 0) return {};
        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length);
        return result;
    }

    std::string PartyClient::NormalizeRoomCode(std::string code)
    {
        code.erase(std::remove_if(code.begin(), code.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }), code.end());
        std::transform(code.begin(), code.end(), code.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (code.size() > 16) code.resize(16);
        return code;
    }

    void PartyClient::Configure(SessionSettings settings)
    {
        std::lock_guard lock(stateMutex_);
        settings_ = std::move(settings);
    }

    void PartyClient::Connect()
    {
        desiredConnected_.store(true);
        outgoingChanged_.notify_all();
    }

    void PartyClient::Disconnect()
    {
        desiredConnected_.store(false);
        HINTERNET socket = nullptr;
        {
            std::lock_guard lock(socketMutex_);
            socket = socket_;
        }
        if (socket)
            WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                  nullptr, 0);
        SetState(PartyConnectionState::Disconnected);
        outgoingChanged_.notify_all();
    }

    PartyWireMessage PartyClient::BaseMessage(PartyMessageType type) const
    {
        PartyWireMessage message;
        message.type = type;
        message.senderId = clientId_;
        message.sentAtMs = NowMs();
        {
            std::lock_guard lock(stateMutex_);
            message.roomCode = room_.code;
            message.memberId = room_.memberId;
            message.roomEpoch = room_.roomEpoch;
            message.sequence = room_.sequence;
        }
        return message;
    }

    void PartyClient::CreateRoom(RoomVisibility visibility)
    {
        Connect();
        PartyWireMessage message = BaseMessage(PartyMessageType::RoomCreate);
        {
            std::lock_guard lock(stateMutex_);
            message.displayName = settings_.displayName;
            message.permissions.mode = settings_.permissionMode;
            message.permissions.syncAudio = settings_.syncAudio;
            message.permissions.syncSubtitles = settings_.syncSubtitles;
            message.permissions.queueMode = settings_.queueMode;
        }
        message.visibility = visibility;
        Queue(std::move(message));
    }

    void PartyClient::JoinRoom(std::string code)
    {
        code = NormalizeRoomCode(std::move(code));
        if (code.empty())
        {
            SetState(PartyConnectionState::Failed, "Enter a watch-party code.");
            return;
        }
        Connect();
        PartyWireMessage message = BaseMessage(PartyMessageType::RoomJoin);
        message.roomCode = std::move(code);
        {
            std::lock_guard lock(stateMutex_);
            message.displayName = settings_.displayName;
            message.memberId = room_.memberId;
        }
        Queue(std::move(message));
    }

    void PartyClient::LeaveRoom()
    {
        PartyWireMessage message = BaseMessage(PartyMessageType::RoomLeave);
        Queue(std::move(message));
        std::lock_guard lock(stateMutex_);
        room_ = {};
    }

    void PartyClient::RequestRoomState()
    {
        Queue(BaseMessage(PartyMessageType::RoomState));
    }

    void PartyClient::RequestPublicRooms()
    {
        Connect();
        Queue(BaseMessage(PartyMessageType::PublicRoomsRequest));
    }

    void PartyClient::SendPlaybackState(PartyMessageType type, const PartyPlaybackState& state)
    {
        PartyWireMessage message = BaseMessage(type);
        message.playback = state;
        message.roomEpoch = state.roomEpoch;
        message.sequence = state.sequence;
        Queue(std::move(message));
    }

    void PartyClient::SendPermissions(const PartyPermissions& permissions)
    {
        PartyWireMessage message = BaseMessage(PartyMessageType::Permissions);
        message.permissions = permissions;
        Queue(std::move(message));
    }

    void PartyClient::SendQueue(const std::vector<PartyQueueItem>& queue)
    {
        PartyWireMessage message = BaseMessage(PartyMessageType::QueueUpdate);
        message.queue = queue;
        if (message.queue.size() > kMaxPartyQueueItems)
            message.queue.resize(kMaxPartyQueueItems);
        Queue(std::move(message));
    }

    void PartyClient::StartReadyCheck(int64_t startAtMs)
    {
        PartyWireMessage message = BaseMessage(PartyMessageType::ReadyCheck);
        message.readyCheckActive = true;
        message.readyStartAtMs = startAtMs;
        Queue(std::move(message));
    }

    void PartyClient::SetReady(bool ready)
    {
        PartyWireMessage message = BaseMessage(PartyMessageType::ReadyState);
        message.ready = ready;
        Queue(std::move(message));
    }

    void PartyClient::SendCinema(const CinemaSettings& cinema)
    {
        PartyWireMessage message = BaseMessage(PartyMessageType::ScreenPreset);
        message.cinema = cinema;
        Queue(std::move(message));
    }

    void PartyClient::SendControlRequest(std::string request, std::string requestId)
    {
        PartyWireMessage message = BaseMessage(PartyMessageType::ControlRequest);
        message.request = std::move(request);
        message.requestId = requestId.empty()
            ? clientId_ + "-" + std::to_string(static_cast<long long>(NowMs()))
            : std::move(requestId);
        Queue(std::move(message));
    }

    void PartyClient::SendControlResponse(std::string requestId, bool allowed)
    {
        PartyWireMessage message = BaseMessage(PartyMessageType::ControlResponse);
        message.requestId = std::move(requestId);
        message.allowed = allowed;
        Queue(std::move(message));
    }

    void PartyClient::Queue(PartyWireMessage message)
    {
        if (message.senderId.empty()) message.senderId = clientId_;
        if (message.sentAtMs == 0) message.sentAtMs = NowMs();
        {
            std::lock_guard lock(outgoingMutex_);
            if (outgoing_.size() >= 128) outgoing_.pop_front();
            outgoing_.push_back(std::move(message));
        }
        outgoingChanged_.notify_all();
    }

    bool PartyClient::SendNow(HINTERNET socket, const PartyWireMessage& message)
    {
        if (!socket) return false;
        const std::string text = SerializePartyMessage(message);
        if (text.empty() || text.size() > kMaxPartyMessageBytes) return false;
        const DWORD result = WinHttpWebSocketSend(
            socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            const_cast<char*>(text.data()), static_cast<DWORD>(text.size()));
        return result == ERROR_SUCCESS;
    }

    bool PartyClient::FlushOutgoing(HINTERNET socket)
    {
        for (;;)
        {
            PartyWireMessage message;
            {
                std::lock_guard lock(outgoingMutex_);
                if (outgoing_.empty()) return true;
                message = std::move(outgoing_.front());
                outgoing_.pop_front();
            }
            if (!SendNow(socket, message))
            {
                std::lock_guard lock(outgoingMutex_);
                outgoing_.push_front(std::move(message));
                return false;
            }
        }
    }

    bool PartyClient::ReceiveOne(HINTERNET socket, std::string& text, bool& closed)
    {
        text.clear();
        closed = false;
        std::vector<char> buffer(kReceiveBufferBytes);
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
        do
        {
            DWORD read = 0;
            const DWORD result = WinHttpWebSocketReceive(socket, buffer.data(),
                                                          static_cast<DWORD>(buffer.size()),
                                                          &read, &type);
            if (result == ERROR_WINHTTP_TIMEOUT) return true;
            if (result != ERROR_SUCCESS) return false;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            {
                closed = true;
                return true;
            }
            if (read > 0)
            {
                if (text.size() + read > kMaxPartyMessageBytes) return false;
                text.append(buffer.data(), read);
            }
        } while (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE);
        return type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || text.empty();
    }

    HINTERNET PartyClient::OpenWebSocket(HINTERNET& session, HINTERNET& connection)
    {
        session = nullptr;
        connection = nullptr;
        SessionSettings settings;
        {
            std::lock_guard lock(stateMutex_);
            settings = settings_;
        }
        const bool secure = IsSecureRelay(settings.relayUrl);
        const std::wstring url = Wide(HttpRelayUrl(settings.relayUrl));
        if (url.empty()) return nullptr;

        URL_COMPONENTS components{sizeof(components)};
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) return nullptr;

        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path;
        if (components.lpszUrlPath && components.dwUrlPathLength)
            path.assign(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.lpszExtraInfo && components.dwExtraInfoLength)
            path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        if (path.empty()) path = L"/ws";

        session = WinHttpOpen(L"AzerothPlex/1.0 WatchTogether",
                              WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return nullptr;
        WinHttpSetTimeouts(session, 5000, 5000, 1000, 1000);
        connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
        if (!connection) return nullptr;

        const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               flags);
        if (!request) return nullptr;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
            !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
        {
            WinHttpCloseHandle(request);
            return nullptr;
        }

        DWORD status = 0;
        DWORD statusBytes = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 101)
        {
            WinHttpCloseHandle(request);
            return nullptr;
        }
        HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        WinHttpCloseHandle(request);
        return socket;
    }

    void PartyClient::Worker(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            if (!desiredConnected_.load())
            {
                std::unique_lock lock(outgoingMutex_);
                outgoingChanged_.wait_for(lock, stop, std::chrono::milliseconds(250), [this] {
                    return desiredConnected_.load();
                });
                continue;
            }

            SetState(reconnectCount_.load() == 0
                         ? PartyConnectionState::Connecting
                         : PartyConnectionState::Reconnecting);
            HINTERNET session = nullptr;
            HINTERNET connection = nullptr;
            HINTERNET socket = OpenWebSocket(session, connection);
            if (!socket)
            {
                SetState(PartyConnectionState::Reconnecting, "Could not connect to the AzerothPlex relay.");
                reconnectCount_.fetch_add(1);
                if (connection) WinHttpCloseHandle(connection);
                if (session) WinHttpCloseHandle(session);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            {
                std::lock_guard lock(socketMutex_);
                socket_ = socket;
            }
            SetState(PartyConnectionState::Connected);

            PartyWireMessage hello = BaseMessage(PartyMessageType::Hello);
            {
                std::lock_guard lock(stateMutex_);
                hello.displayName = settings_.displayName;
            }
            SendNow(socket, hello);

            PartyRoom previousRoom;
            {
                std::lock_guard lock(stateMutex_);
                previousRoom = room_;
            }
            if (!previousRoom.code.empty())
            {
                PartyWireMessage rejoin = BaseMessage(PartyMessageType::RoomJoin);
                rejoin.roomCode = previousRoom.code;
                rejoin.memberId = previousRoom.memberId;
                {
                    std::lock_guard lock(stateMutex_);
                    rejoin.displayName = settings_.displayName;
                }
                SendNow(socket, rejoin);
            }

            bool connected = true;
            while (connected && desiredConnected_.load() && !stop.stop_requested())
            {
                if (!FlushOutgoing(socket)) break;
                const int64_t now = NowMs();
                if (now - lastPingSentMs_ >= 5000)
                {
                    PartyWireMessage ping = BaseMessage(PartyMessageType::Ping);
                    ping.sentAtMs = now;
                    if (!SendNow(socket, ping)) break;
                    lastPingSentMs_ = now;
                }

                std::string text;
                bool closed = false;
                if (!ReceiveOne(socket, text, closed)) break;
                if (closed) break;
                if (text.empty()) continue;
                PartyWireMessage incoming;
                std::string error;
                if (!ParsePartyMessage(text, incoming, error))
                {
                    PartyWireMessage protocolError;
                    protocolError.type = PartyMessageType::Error;
                    protocolError.error = error;
                    PushEvent(std::move(protocolError));
                    continue;
                }
                HandleIncoming(incoming);
                PushEvent(std::move(incoming));
            }

            {
                std::lock_guard lock(socketMutex_);
                socket_ = nullptr;
            }
            WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(socket);
            if (connection) WinHttpCloseHandle(connection);
            if (session) WinHttpCloseHandle(session);

            if (desiredConnected_.load() && !stop.stop_requested())
            {
                reconnectCount_.fetch_add(1);
                SetState(PartyConnectionState::Reconnecting, "Party connection interrupted; reconnecting.");
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::min<uint32_t>(5000, 500 + reconnectCount_.load() * 500)));
            }
        }
    }

    void PartyClient::HandleIncoming(const PartyWireMessage& message)
    {
        const int64_t now = NowMs();
        std::lock_guard lock(stateMutex_);
        if (!message.roomCode.empty() && (room_.code.empty() || room_.code == message.roomCode))
        {
            if (message.roomEpoch > room_.roomEpoch)
            {
                room_.roomEpoch = message.roomEpoch;
                room_.sequence = message.sequence;
            }
            else if (message.roomEpoch == room_.roomEpoch)
            {
                room_.sequence = std::max(room_.sequence, message.sequence);
            }
        }
        switch (message.type)
        {
        case PartyMessageType::RoomCreated:
        case PartyMessageType::RoomJoined:
        case PartyMessageType::RoomState:
            room_.code = message.roomCode;
            if (!message.memberId.empty()) room_.memberId = message.memberId;
            if (!message.hostMemberId.empty()) room_.hostMemberId = message.hostMemberId;
            room_.roomEpoch = message.roomEpoch;
            room_.sequence = std::max(room_.sequence, message.sequence);
            room_.visibility = message.visibility;
            room_.permissions = message.permissions;
            room_.playback = message.playback;
            room_.members = message.members;
            room_.queue = message.queue;
            room_.cinema = message.cinema;
            room_.readyCheckActive = message.readyCheckActive;
            room_.readyStartAtMs = message.readyStartAtMs;
            settings_.lastRoomCode = room_.code;
            break;
        case PartyMessageType::MemberJoined:
        case PartyMessageType::MemberLeft:
            if (!message.members.empty()) room_.members = message.members;
            room_.roomEpoch = std::max(room_.roomEpoch, message.roomEpoch);
            room_.sequence = std::max(room_.sequence, message.sequence);
            break;
        case PartyMessageType::HostChanged:
            room_.hostMemberId = message.hostMemberId;
            room_.roomEpoch = message.roomEpoch;
            room_.sequence = message.sequence;
            for (auto& member : room_.members)
                member.role = member.id == room_.hostMemberId ? PartyRole::Host : PartyRole::Guest;
            break;
        case PartyMessageType::PlaybackState:
        case PartyMessageType::MediaChanged:
        case PartyMessageType::Play:
        case PartyMessageType::Pause:
        case PartyMessageType::Seek:
        case PartyMessageType::Stop:
            if (message.roomEpoch > room_.roomEpoch ||
                (message.roomEpoch == room_.roomEpoch && message.sequence >= room_.sequence))
            {
                room_.roomEpoch = message.roomEpoch;
                room_.sequence = message.sequence;
                room_.playback = message.playback;
                room_.readyCheckActive = message.readyCheckActive;
                room_.readyStartAtMs = message.readyStartAtMs;
            }
            break;
        case PartyMessageType::Permissions:
            room_.permissions = message.permissions;
            break;
        case PartyMessageType::QueueUpdate:
            room_.queue = message.queue;
            break;
        case PartyMessageType::ReadyCheck:
            room_.readyCheckActive = message.readyCheckActive;
            room_.readyStartAtMs = message.readyStartAtMs;
            if (room_.readyCheckActive)
                for (auto& member : room_.members) member.ready = false;
            break;
        case PartyMessageType::ReadyState:
            room_.readyCheckActive = message.readyCheckActive;
            room_.readyStartAtMs = message.readyStartAtMs;
            if (!message.members.empty()) room_.members = message.members;
            break;
        case PartyMessageType::ScreenPreset:
            room_.cinema = message.cinema;
            break;
        case PartyMessageType::Pong:
            if (message.sentAtMs > 0)
            {
                relayRttMs_ = std::max<int64_t>(0, now - message.sentAtMs);
                if (message.serverTimeMs != 0)
                    clockOffsetMs_ = message.serverTimeMs - (message.sentAtMs + relayRttMs_ / 2);
            }
            break;
        case PartyMessageType::Error:
            lastError_ = message.error;
            break;
        default:
            break;
        }
    }

    void PartyClient::PushEvent(PartyWireMessage message)
    {
        std::lock_guard lock(eventsMutex_);
        if (events_.size() >= 256) events_.pop_front();
        events_.push_back(std::move(message));
    }

    bool PartyClient::PopEvent(PartyWireMessage& message)
    {
        std::lock_guard lock(eventsMutex_);
        if (events_.empty()) return false;
        message = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    void PartyClient::SetState(PartyConnectionState state, std::string error)
    {
        std::lock_guard lock(stateMutex_);
        state_ = state;
        if (!error.empty()) lastError_ = std::move(error);
        if (state == PartyConnectionState::Connected) lastError_.clear();
    }

    PartyConnectionState PartyClient::ConnectionState() const
    {
        std::lock_guard lock(stateMutex_);
        return state_;
    }

    PartyRoom PartyClient::Room() const
    {
        std::lock_guard lock(stateMutex_);
        return room_;
    }

    PartyRole PartyClient::Role() const
    {
        std::lock_guard lock(stateMutex_);
        if (room_.memberId.empty()) return PartyRole::None;
        return room_.memberId == room_.hostMemberId ? PartyRole::Host : PartyRole::Guest;
    }

    std::string PartyClient::LastError() const
    {
        std::lock_guard lock(stateMutex_);
        return lastError_;
    }

    int64_t PartyClient::RelayRttMs() const
    {
        std::lock_guard lock(stateMutex_);
        return relayRttMs_;
    }

    int64_t PartyClient::ClockOffsetMs() const
    {
        std::lock_guard lock(stateMutex_);
        return clockOffsetMs_;
    }
}
