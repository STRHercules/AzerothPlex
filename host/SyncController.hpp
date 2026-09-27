#pragma once

#include "PartyTypes.hpp"

#include <cstdint>

namespace wxl_party
{
    struct SyncDecision
    {
        bool accepted = false;
        bool mediaChanged = false;
        bool shouldPlay = false;
        bool shouldPause = false;
        bool shouldStop = false;
        bool shouldSeek = false;
        int64_t targetPositionMs = 0;
        int64_t driftMs = 0;
        double effectiveSpeed = 1.0;
        SyncCorrection correction = SyncCorrection::None;
    };

    class SyncController final
    {
    public:
        void Reset();
        void SetRole(PartyRole role) { role_ = role; }
        void SetPermissions(PartyPermissions permissions) { permissions_ = permissions; }
        void SetBaseSpeed(double speed);
        void SetThresholds(int64_t noCorrectionMs, int64_t hardSeekMs);

        PartyRole Role() const { return role_; }
        const PartyPermissions& Permissions() const { return permissions_; }
        double BaseSpeed() const { return baseSpeed_; }
        double EffectiveSpeed() const { return effectiveSpeed_; }
        int64_t LastDriftMs() const { return lastDriftMs_; }
        SyncCorrection LastCorrection() const { return lastCorrection_; }
        uint64_t LastRoomEpoch() const { return lastRoomEpoch_; }
        uint64_t LastSequence() const { return lastSequence_; }

        bool CanControlLocally() const;
        bool ShouldRequestControl() const;
        bool IsNewer(uint64_t roomEpoch, uint64_t sequence) const;
        int64_t ExpectedPositionMs(const PartyPlaybackState& remote, int64_t nowMs) const;

        SyncDecision EvaluateRemoteState(const PartyPlaybackState& remote,
                                         int64_t nowMs,
                                         int64_t localPositionMs,
                                         bool localPaused,
                                         const MediaIdentity& localMedia,
                                         bool explicitSeek = false,
                                         bool reconnect = false);

        PartyPlaybackState BuildLocalState(const MediaIdentity& media,
                                           int64_t positionMs,
                                           int64_t durationMs,
                                           bool paused,
                                           bool stopped,
                                           int64_t nowMs,
                                           uint64_t roomEpoch,
                                           uint64_t sequence) const;

    private:
        PartyRole role_ = PartyRole::None;
        PartyPermissions permissions_{};
        double baseSpeed_ = 1.0;
        double effectiveSpeed_ = 1.0;
        int64_t noCorrectionMs_ = kNoCorrectionDriftMs;
        int64_t hardSeekMs_ = kHardSeekDriftMs;
        int64_t lastDriftMs_ = 0;
        SyncCorrection lastCorrection_ = SyncCorrection::None;
        uint64_t lastRoomEpoch_ = 0;
        uint64_t lastSequence_ = 0;
    };
}
