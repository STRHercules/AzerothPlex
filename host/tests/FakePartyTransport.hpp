#pragma once

#include "../PartyTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace wxl_party::tests
{
    class FakePartyTransport final
    {
    public:
        void Send(PartyWireMessage message, int64_t deliverAtMs = 0, int copies = 1)
        {
            copies = std::max(1, copies);
            for (int index = 0; index < copies; ++index)
                pending_.push_back({deliverAtMs, nextOrder_++, message});
        }

        void ReorderLastTwo()
        {
            if (pending_.size() >= 2)
                std::swap(pending_[pending_.size() - 1], pending_[pending_.size() - 2]);
        }

        std::vector<PartyWireMessage> Poll(int64_t nowMs)
        {
            std::vector<PartyWireMessage> result;
            for (auto iterator = pending_.begin(); iterator != pending_.end();)
            {
                if (iterator->deliverAtMs <= nowMs)
                {
                    result.push_back(std::move(iterator->message));
                    iterator = pending_.erase(iterator);
                }
                else
                {
                    ++iterator;
                }
            }
            return result;
        }

        void Disconnect() { connected_ = false; }
        void Reconnect() { connected_ = true; ++reconnectCount_; }
        bool Connected() const { return connected_; }
        uint32_t ReconnectCount() const { return reconnectCount_; }

    private:
        struct Pending
        {
            int64_t deliverAtMs = 0;
            uint64_t order = 0;
            PartyWireMessage message;
        };

        std::vector<Pending> pending_;
        uint64_t nextOrder_ = 0;
        uint32_t reconnectCount_ = 0;
        bool connected_ = true;
    };
}
