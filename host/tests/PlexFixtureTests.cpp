#include "../PlexClient.hpp"

#include <cassert>
#include <string>
#include <vector>

int main()
{
    wxl_plex::PinAuth pin{};
    assert(wxl_plex::ParsePinJson(
        R"({"id":17,"code":"ABCD","authToken":"token-value"})", pin));
    assert(pin.id == 17);
    assert(pin.code == "ABCD");
    assert(pin.authToken == "token-value");

    const std::string resources =
        R"(<MediaContainer><Device name="Test Plex" accessToken="server-token">
        <Connection protocol="https" address="plex.test" port="32400"
         uri="https://plex.test:32400" local="1" available="1"/>
        </Device></MediaContainer>)";
    const auto servers = wxl_plex::ParseResourcesXml(resources);
    assert(servers.size() == 1);
    assert(servers[0].name == "Test Plex");
    assert(servers[0].uri == "https://plex.test:32400");
    assert(servers[0].accessToken == "server-token");

    const std::string sectionsXml =
        R"(<MediaContainer><Directory key="1" title="Movies" type="movie"/>
        <Directory key="2" title="Shows" type="show"/></MediaContainer>)";
    const auto sections = wxl_plex::ParseSectionsXml(sectionsXml);
    assert(sections.size() == 2);
    assert(sections[0].key == "1");
    assert(sections[1].type == "show");

    const std::string itemsXml =
        R"(<MediaContainer><Video ratingKey="8" title="Episode One" type="episode"
        grandparentTitle="Test Show" viewOffset="0" duration="60000"/>
        <Video ratingKey="9" title="Episode Two" type="episode"/></MediaContainer>)";
    const auto items = wxl_plex::ParseItemsXml(itemsXml);
    assert(items.size() == 2);
    assert(items[0].title == "Episode One");
    assert(items[0].grandparentTitle == "Test Show");

    const std::string metadata =
        R"(<MediaContainer><Video ratingKey="7" title="Test Movie" type="movie"
        viewOffset="42000" duration="120000"><Media videoResolution="1080">
        <Part key="/library/parts/7/file"><Stream streamType="2" id="11"
        language="English" title="Stereo"/><Stream streamType="3" id="12"
        language="English" title="Subtitles"/></Part></Media></Video></MediaContainer>)";
    const auto item = wxl_plex::ParseMetadataXml(metadata);
    assert(item.ratingKey == "7");
    assert(item.title == "Test Movie");
    assert(item.viewOffsetMs == 42000);
    assert(item.partKey == "/library/parts/7/file");
    assert(item.streams.size() == 2);
    assert(item.streams[0].id == 11);
    assert(item.streams[1].streamType == 3);

    const auto playback = wxl_plex::BuildDirectPlayback(item, servers[0]);
    assert(playback.uri == "https://plex.test:32400/library/parts/7/file?X-Plex-Token=server-token");

    const auto transcoded = wxl_plex::BuildTranscodePlayback(item, servers[0], "session-1");
    assert(transcoded.transcoded);
    assert(transcoded.uri.find("/video/:/transcode/universal/start.m3u8") != std::string::npos);
    assert(transcoded.uri.find("session=session-1") != std::string::npos);

    const auto timeline = wxl_plex::BuildTimelineRequest(
        item, wxl_plex::TimelineState::Paused, 42000);
    assert(timeline.find("state=paused") != std::string::npos);
    assert(timeline.find("time=42000") != std::string::npos);
    return 0;
}
