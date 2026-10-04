#include "../PartyProtocol.hpp"
#include "../PartyTypes.hpp"
#include "../SyncController.hpp"
#include "FakePartyTransport.hpp"

#include <cmath>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    using namespace wxl_party;

    MediaIdentity local;
    local.serverIdentifier = "server-a";
    local.ratingKey = "42";
    local.plexGuid = "plex://movie/abc";
    local.title = "Alien";
    local.year = 1979;
    local.durationMs = 117000;

    MediaIdentity same = local;
    CHECK(MatchMediaIdentity(local, same) == MediaMatchQuality::ExactServerItem);

    MediaIdentity crossServer = local;
    crossServer.serverIdentifier = "server-b";
    crossServer.ratingKey = "99";
    CHECK(MatchMediaIdentity(local, crossServer) == MediaMatchQuality::PlexGuid);

    MediaIdentity externalLeft;
    externalLeft.tmdbId = "348";
    MediaIdentity externalRight;
    externalRight.tmdbId = "348";
    CHECK(MatchMediaIdentity(externalLeft, externalRight) == MediaMatchQuality::ExternalGuid);

    MediaIdentity episodeLeft;
    episodeLeft.type = "episode";
    episodeLeft.grandparentTitle = "The Expanse";
    episodeLeft.season = 2;
    episodeLeft.episode = 5;
    episodeLeft.title = "Home";
    MediaIdentity episodeRight = episodeLeft;
    episodeRight.grandparentTitle = "the expanse";
    CHECK(MatchMediaIdentity(episodeLeft, episodeRight) == MediaMatchQuality::EpisodeIdentity);

    MediaIdentity fallback;
    fallback.title = "Alien";
    fallback.year = 1979;
    fallback.durationMs = 118000;
    CHECK(MatchMediaIdentity(local, fallback) == MediaMatchQuality::Fallback);
    fallback.year = 1980;
    CHECK(MatchMediaIdentity(local, fallback) == MediaMatchQuality::None);

    StreamIdentity englishAac;
    englishAac.language = "eng";
    englishAac.title = "English";
    englishAac.codec = "aac";
    englishAac.valid = true;
    StreamIdentity englishAacCopy = englishAac;
    englishAacCopy.language = "ENG";
    CHECK(SameStream(englishAac, englishAacCopy));
    englishAacCopy.forced = true;
    CHECK(!SameStream(englishAac, englishAacCopy));

    PartyWireMessage outgoing;
    outgoing.type = PartyMessageType::PlaybackState;
    outgoing.roomCode = "AZ-7K3F";
    outgoing.senderId = "client-a";
    outgoing.roomEpoch = 3;
    outgoing.sequence = 19;
    outgoing.sentAtMs = 1000;
    outgoing.playback.media = local;
    outgoing.playback.playing = true;
    outgoing.playback.stopped = false;
    outgoing.playback.positionMs = 42000;
    outgoing.playback.authoritativeTimestampMs = 1000;
    outgoing.playback.roomEpoch = 3;
    outgoing.playback.sequence = 19;

    PartyWireMessage parsed;
    std::string error;
    CHECK(ParsePartyMessage(SerializePartyMessage(outgoing), parsed, error));
    CHECK(parsed.type == PartyMessageType::PlaybackState);
    CHECK(parsed.roomCode == "AZ-7K3F");
    CHECK(parsed.playback.positionMs == 42000);
    CHECK(parsed.playback.media.plexGuid == local.plexGuid);

    PartyWireMessage permissionsMessage;
    permissionsMessage.type = PartyMessageType::Permissions;
    permissionsMessage.roomCode = "AZ-7K3F";
    permissionsMessage.senderId = "client-a";
    permissionsMessage.permissions.mode = PartyPermissionMode::HostOnly;
    permissionsMessage.permissions.queueMode = PartyQueueMode::EveryoneMayAdd;
    CHECK(ParsePartyMessage(SerializePartyMessage(permissionsMessage), parsed, error));
    CHECK(parsed.permissions.mode == PartyPermissionMode::HostOnly);
    CHECK(parsed.permissions.queueMode == PartyQueueMode::EveryoneMayAdd);

    PartyWireMessage liveMessage;
    liveMessage.type = PartyMessageType::LiveStarted;
    liveMessage.roomCode = "AZ-7K3F";
    liveMessage.senderId = "client-a";
    liveMessage.live.active = true;
    liveMessage.live.broadcasterId = "client-a";
    liveMessage.live.watcherCount = 2;
    CHECK(ParsePartyMessage(SerializePartyMessage(liveMessage), parsed, error));
    CHECK(parsed.type == PartyMessageType::LiveStarted);
    CHECK(parsed.live.active);
    CHECK(parsed.live.broadcasterId == "client-a");
    CHECK(parsed.live.watcherCount == 2);
    CHECK(PartyMessageTypeFromString("live_watch") == PartyMessageType::LiveWatch);
    CHECK(PartyMessageTypeFromString("live_stopped") == PartyMessageType::LiveStopped);

    PartyWireMessage readyMessage;
    readyMessage.type = PartyMessageType::ReadyCheck;
    readyMessage.roomCode = "AZ-7K3F";
    readyMessage.senderId = "client-a";
    readyMessage.readyCheckActive = true;
    readyMessage.readyStartAtMs = 0;
    CHECK(ParsePartyMessage(SerializePartyMessage(readyMessage), parsed, error));
    CHECK(parsed.type == PartyMessageType::ReadyCheck);
    CHECK(parsed.readyCheckActive);
    CHECK(parsed.readyStartAtMs == 0);

    SyncController permissions;
    permissions.SetRole(PartyRole::Guest);
    permissions.SetPermissions({PartyPermissionMode::HostOnly, false, false});
    CHECK(!permissions.CanControlLocally());
    CHECK(!permissions.ShouldRequestControl());
    permissions.SetPermissions({PartyPermissionMode::SharedControl, false, false});
    CHECK(permissions.CanControlLocally());
    permissions.SetPermissions({PartyPermissionMode::RequestControl, false, false});
    CHECK(!permissions.CanControlLocally());
    CHECK(permissions.ShouldRequestControl());
    permissions.SetRole(PartyRole::Host);
    CHECK(permissions.CanControlLocally());

    SyncController sync;
    sync.SetRole(PartyRole::Guest);
    sync.SetPermissions({PartyPermissionMode::HostOnly, false, false});

    PartyPlaybackState remote;
    remote.media = local;
    remote.playing = true;
    remote.stopped = false;
    remote.positionMs = 10000;
    remote.authoritativeTimestampMs = 1000;
    remote.roomEpoch = 1;
    remote.sequence = 1;

    auto noCorrection = sync.EvaluateRemoteState(remote, 1100, 10050, false, local);
    CHECK(noCorrection.accepted);
    CHECK(noCorrection.correction == SyncCorrection::None);

    remote.sequence = 2;
    auto soft = sync.EvaluateRemoteState(remote, 1500, 10100, false, local);
    CHECK(soft.accepted);
    CHECK(soft.correction == SyncCorrection::SpeedUp);
    CHECK(soft.effectiveSpeed > 1.0 && soft.effectiveSpeed <= 1.02);

    remote.sequence = 3;
    auto hard = sync.EvaluateRemoteState(remote, 3000, 10000, false, local);
    CHECK(hard.accepted);
    CHECK(hard.correction == SyncCorrection::HardSeek);
    CHECK(hard.shouldSeek);

    auto duplicate = sync.EvaluateRemoteState(remote, 3000, 10000, false, local);
    CHECK(!duplicate.accepted);

    remote.sequence = 4;
    remote.positionMs = 12000;
    remote.authoritativeTimestampMs = 3000;
    auto explicitSeek = sync.EvaluateRemoteState(remote, 3050, 12020, false, local, true, false);
    CHECK(explicitSeek.accepted);
    CHECK(explicitSeek.correction == SyncCorrection::HardSeek);
    CHECK(explicitSeek.shouldSeek);

    remote.sequence = 5;
    auto reconnect = sync.EvaluateRemoteState(remote, 3100, 12050, false, local, false, true);
    CHECK(reconnect.accepted);
    CHECK(reconnect.correction == SyncCorrection::HardSeek);
    CHECK(reconnect.shouldSeek);

    remote.roomEpoch = 2;
    remote.sequence = 1;
    auto newEpoch = sync.EvaluateRemoteState(remote, 3200, 12000, false, local);
    CHECK(newEpoch.accepted);
    CHECK(sync.LastRoomEpoch() == 2);

    tests::FakePartyTransport transport;
    PartyWireMessage older;
    older.type = PartyMessageType::PlaybackState;
    older.playback = remote;
    older.playback.roomEpoch = 3;
    older.playback.sequence = 9;
    PartyWireMessage newer = older;
    newer.playback.sequence = 10;
    transport.Send(older, 100);
    transport.Send(newer, 100);
    transport.ReorderLastTwo();
    transport.Send(newer, 100);
    CHECK(transport.Poll(99).empty());
    const auto delivered = transport.Poll(100);
    CHECK(delivered.size() == 3);

    SyncController reordered;
    bool acceptedSequence10 = false;
    int rejectedStaleOrDuplicate = 0;
    for (const auto& message : delivered)
    {
        const auto decision = reordered.EvaluateRemoteState(
            message.playback, 1000, 0, true, {}, false, false);
        if (decision.accepted && message.playback.sequence == 10) acceptedSequence10 = true;
        if (!decision.accepted) ++rejectedStaleOrDuplicate;
    }
    CHECK(acceptedSequence10);
    CHECK(rejectedStaleOrDuplicate >= 1);

    transport.Disconnect();
    CHECK(!transport.Connected());
    transport.Reconnect();
    CHECK(transport.Connected());
    CHECK(transport.ReconnectCount() == 1);

    return 0;
}
