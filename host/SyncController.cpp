#include "SyncController.hpp"

#include <algorithm>
#include <cmath>

namespace wxl_party
{
    void SyncController::Reset()
    {
        effectiveSpeed_ = baseSpeed_;
        lastDriftMs_ = 0;
        lastCorrection_ = SyncCorrection::None;
        lastRoomEpoch_ = 0;
        lastSequence_ = 0;
    }

    void SyncController::SetBaseSpeed(double speed)
    {
        baseSpeed_ = std::clamp(speed, 0.25, 4.0);
        if (lastCorrection_ == SyncCorrection::None || lastCorrection_ == SyncCorrection::HardSeek)
            effectiveSpeed_ = baseSpeed_;
    }

    void SyncController::SetThresholds(int64_t noCorrectionMs, int64_t hardSeekMs)
    {
        noCorrectionMs_ = std::clamp<int64_t>(noCorrectionMs, 50, 2000);
        hardSeekMs_ = std::clamp<int64_t>(hardSeekMs, noCorrectionMs_ + 50, 10000);
    }

    bool SyncController::CanControlLocally() const
    {
        if (role_ == PartyRole::Host) return true;
        if (role_ != PartyRole::Guest) return true;
        return permissions_.mode == PartyPermissionMode::SharedControl;
    }

    bool SyncController::ShouldRequestControl() const
    {
        return role_ == PartyRole::Guest &&
               permissions_.mode == PartyPermissionMode::RequestControl;
    }

    bool SyncController::IsNewer(uint64_t roomEpoch, uint64_t sequence) const
    {
        if (roomEpoch > lastRoomEpoch_) return true;
        if (roomEpoch < lastRoomEpoch_) return false;
        return sequence > lastSequence_;
    }

    int64_t SyncController::ExpectedPositionMs(const PartyPlaybackState& remote, int64_t nowMs) const
    {
        int64_t expected = std::max<int64_t>(0, remote.positionMs);
        if (remote.playing && !remote.paused && !remote.stopped && remote.authoritativeTimestampMs > 0)
            expected += std::max<int64_t>(0, nowMs - remote.authoritativeTimestampMs);
        if (remote.durationMs > 0) expected = std::min(expected, remote.durationMs);
        return expected;
    }

    SyncDecision SyncController::EvaluateRemoteState(const PartyPlaybackState& remote,
                                                       int64_t nowMs,
                                                       int64_t localPositionMs,
                                                       bool localPaused,
                                                       const MediaIdentity& localMedia,
                                                       bool explicitSeek,
                                                       bool reconnect)
    {
        SyncDecision decision;
        if (!IsNewer(remote.roomEpoch, remote.sequence)) return decision;

        decision.accepted = true;
        lastRoomEpoch_ = remote.roomEpoch;
        lastSequence_ = remote.sequence;

        decision.mediaChanged = !remote.media.ratingKey.empty() && !SameMedia(localMedia, remote.media);
        decision.targetPositionMs = ExpectedPositionMs(remote, nowMs);
        decision.driftMs = decision.targetPositionMs - std::max<int64_t>(0, localPositionMs);
        lastDriftMs_ = decision.driftMs;

        if (remote.stopped)
        {
            decision.shouldStop = true;
            decision.correction = SyncCorrection::None;
            decision.effectiveSpeed = baseSpeed_;
        }
        else
        {
            decision.shouldPause = remote.paused && !localPaused;
            decision.shouldPlay = remote.playing && !remote.paused && localPaused;

            const int64_t absoluteDrift = std::llabs(decision.driftMs);
            if (decision.mediaChanged || explicitSeek || reconnect || absoluteDrift > hardSeekMs_)
            {
                decision.shouldSeek = true;
                decision.correction = SyncCorrection::HardSeek;
                decision.effectiveSpeed = baseSpeed_;
            }
            else if (!remote.paused && absoluteDrift > noCorrectionMs_)
            {
                const double normalized = std::clamp(
                    static_cast<double>(absoluteDrift - noCorrectionMs_) /
                        static_cast<double>(std::max<int64_t>(1, hardSeekMs_ - noCorrectionMs_)),
                    0.0, 1.0);
                const double adjustment = 0.01 + normalized * 0.01;
                if (decision.driftMs > 0)
                {
                    decision.correction = SyncCorrection::SpeedUp;
                    decision.effectiveSpeed = baseSpeed_ * (1.0 + adjustment);
                }
                else
                {
                    decision.correction = SyncCorrection::SlowDown;
                    decision.effectiveSpeed = baseSpeed_ * (1.0 - adjustment);
                }
            }
            else
            {
                decision.correction = SyncCorrection::None;
                decision.effectiveSpeed = baseSpeed_;
            }
        }

        lastCorrection_ = decision.correction;
        effectiveSpeed_ = decision.effectiveSpeed;
        return decision;
    }

    PartyPlaybackState SyncController::BuildLocalState(const MediaIdentity& media,
                                                        int64_t positionMs,
                                                        int64_t durationMs,
                                                        bool paused,
                                                        bool stopped,
                                                        int64_t nowMs,
                                                        uint64_t roomEpoch,
                                                        uint64_t sequence) const
    {
        PartyPlaybackState state;
        state.media = media;
        state.playing = !stopped && !paused;
        state.paused = !stopped && paused;
        state.stopped = stopped;
        state.positionMs = std::max<int64_t>(0, positionMs);
        state.durationMs = std::max<int64_t>(0, durationMs);
        state.authoritativeTimestampMs = nowMs;
        state.roomEpoch = roomEpoch;
        state.sequence = sequence;
        return state;
    }
}
