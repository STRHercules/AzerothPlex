#pragma once

#include "PartyProtocol.hpp"
#include "SessionStore.hpp"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace wxl_party
{
    class PartyClient final
    {
    public:
        explicit PartyClient(SessionSettings settings);
        ~PartyClient();

        PartyClient(const PartyClient&) = delete;
        PartyClient& operator=(const PartyClient&) = delete;

        void Configure(SessionSettings settings);
        void Connect();
        void Disconnect();

        void CreateRoom(RoomVisibility visibility = RoomVisibility::Private);
        void JoinRoom(std::string code);
        void LeaveRoom();
        void RequestRoomState();
        void RequestPublicRooms();

        void SendPlaybackState(PartyMessageType type, const PartyPlaybackState& state);
        void SendPermissions(const PartyPermissions& permissions);
        void SendQueue(const std::vector<PartyQueueItem>& queue);
        void StartReadyCheck(int64_t startAtMs);
        void SetReady(bool ready);
        void SendCinema(const CinemaSettings& cinema);
        void SendControlRequest(std::string request, std::string requestId = {});
        void SendControlResponse(std::string requestId, bool allowed);

        bool PopEvent(PartyWireMessage& message);

        PartyConnectionState ConnectionState() const;
        PartyRoom Room() const;
        PartyRole Role() const;
        std::string LastError() const;
        int64_t RelayRttMs() const;
        int64_t ClockOffsetMs() const;
        uint32_t ReconnectCount() const { return reconnectCount_.load(); }
        std::string ClientId() const { return clientId_; }

    private:
        static int64_t NowMs();
        static std::string MakeClientId();
        static std::wstring Wide(std::string_view value);
        static std::string NormalizeRoomCode(std::string code);

        void Worker(std::stop_token stop);
        HINTERNET OpenWebSocket(HINTERNET& session, HINTERNET& connection);
        bool FlushOutgoing(HINTERNET socket);
        bool ReceiveOne(HINTERNET socket, std::string& text, bool& closed);
        void Queue(PartyWireMessage message);
        bool SendNow(HINTERNET socket, const PartyWireMessage& message);
        void HandleIncoming(const PartyWireMessage& message);
        void PushEvent(PartyWireMessage message);
        void SetState(PartyConnectionState state, std::string error = {});
        PartyWireMessage BaseMessage(PartyMessageType type) const;

        mutable std::mutex stateMutex_;
        SessionSettings settings_;
        PartyConnectionState state_ = PartyConnectionState::Disconnected;
        PartyRoom room_;
        std::string lastError_;
        int64_t relayRttMs_ = 0;
        int64_t clockOffsetMs_ = 0;

        std::mutex outgoingMutex_;
        std::deque<PartyWireMessage> outgoing_;
        std::condition_variable_any outgoingChanged_;

        std::mutex eventsMutex_;
        std::deque<PartyWireMessage> events_;

        std::mutex socketMutex_;
        HINTERNET socket_ = nullptr;
        std::atomic<bool> desiredConnected_{false};
        std::atomic<uint32_t> reconnectCount_{0};
        std::jthread worker_;
        std::string clientId_;
        int64_t lastPingSentMs_ = 0;
    };
}
