#include "PlexClient.hpp"

#include "json.hpp"
#include "TokenStore.hpp"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <condition_variable>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace wxl_plex
{
    namespace
    {
        std::string Attribute(std::string_view tag, std::string_view name)
        {
            size_t cursor = 0;
            while (cursor < tag.size())
            {
                cursor = tag.find(name, cursor);
                if (cursor == std::string_view::npos) return {};

                const bool leftBoundary = cursor == 0 ||
                    (!std::isalnum(static_cast<unsigned char>(tag[cursor - 1])) &&
                     tag[cursor - 1] != '_');
                const size_t afterName = cursor + name.size();
                const bool rightBoundary = afterName >= tag.size() ||
                    (!std::isalnum(static_cast<unsigned char>(tag[afterName])) &&
                     tag[afterName] != '_');
                if (!leftBoundary || !rightBoundary)
                {
                    cursor = afterName;
                    continue;
                }

                size_t equals = afterName;
                while (equals < tag.size() && std::isspace(static_cast<unsigned char>(tag[equals])))
                    ++equals;
                if (equals >= tag.size() || tag[equals] != '=')
                {
                    cursor = afterName;
                    continue;
                }
                ++equals;
                while (equals < tag.size() && std::isspace(static_cast<unsigned char>(tag[equals])))
                    ++equals;
                if (equals >= tag.size() || (tag[equals] != '\'' && tag[equals] != '"'))
                    return {};
                const char quote = tag[equals++];
                const size_t end = tag.find(quote, equals);
                return end == std::string_view::npos
                    ? std::string{}
                    : std::string(tag.substr(equals, end - equals));
            }
            return {};
        }

        int IntegerAttribute(std::string_view tag, std::string_view name)
        {
            const std::string value = Attribute(tag, name);
            int result = 0;
            std::from_chars(value.data(), value.data() + value.size(), result);
            return result;
        }

        std::string StateName(TimelineState state)
        {
            switch (state)
            {
            case TimelineState::Playing: return "playing";
            case TimelineState::Paused: return "paused";
            case TimelineState::Stopped: return "stopped";
            case TimelineState::Watched: return "watched";
            }
            return "stopped";
        }

        std::string MakeServerUri(std::string_view protocol, std::string_view address,
                                  std::string_view port)
        {
            std::string result(protocol);
            result += "://";
            result += address;
            if (!port.empty())
            {
                result += ':';
                result += port;
            }
            return result;
        }

        PlexItem ParseItemTag(std::string_view tag)
        {
            PlexItem result;
            result.ratingKey = Attribute(tag, "ratingKey");
            result.guid = Attribute(tag, "guid");
            result.title = Attribute(tag, "title");
            result.type = Attribute(tag, "type");
            result.grandparentTitle = Attribute(tag, "grandparentTitle");
            result.parentRatingKey = Attribute(tag, "parentRatingKey");
            result.grandparentRatingKey = Attribute(tag, "grandparentRatingKey");
            result.thumb = Attribute(tag, "thumb");
            result.year = IntegerAttribute(tag, "year");
            result.parentIndex = IntegerAttribute(tag, "parentIndex");
            result.index = IntegerAttribute(tag, "index");
            result.viewOffsetMs = IntegerAttribute(tag, "viewOffset");
            result.durationMs = IntegerAttribute(tag, "duration");
            result.viewed = IntegerAttribute(tag, "viewCount") > 0;
            return result;
        }

        void ApplyExternalGuid(PlexItem& item, std::string_view id)
        {
            if (id.rfind("imdb://", 0) == 0) item.imdbId = std::string(id.substr(7));
            else if (id.rfind("tmdb://", 0) == 0) item.tmdbId = std::string(id.substr(7));
            else if (id.rfind("tvdb://", 0) == 0) item.tvdbId = std::string(id.substr(7));
        }

        bool IsTagBoundary(char value)
        {
            return value == '>' || value == '/' ||
                   std::isspace(static_cast<unsigned char>(value));
        }

        size_t FindItemStart(std::string_view xml, size_t cursor)
        {
            constexpr std::string_view tags[] = {
                "<Video", "<Directory", "<Track", "<Photo", "<Playlist"};
            size_t result = std::string_view::npos;
            for (const auto tag : tags)
            {
                size_t candidate = xml.find(tag, cursor);
                while (candidate != std::string_view::npos &&
                       candidate + tag.size() < xml.size() &&
                       !IsTagBoundary(xml[candidate + tag.size()]))
                    candidate = xml.find(tag, candidate + tag.size());
                if (candidate != std::string_view::npos &&
                    (result == std::string_view::npos || candidate < result))
                    result = candidate;
            }
            return result;
        }

        std::wstring Wide(std::string_view value)
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

        std::string UrlEncode(std::string_view value)
        {
            constexpr char hex[] = "0123456789ABCDEF";
            std::string result;
            for (const unsigned char character : value)
            {
                if (std::isalnum(character) || character == '-' || character == '_' ||
                    character == '.' || character == '~')
                    result.push_back(static_cast<char>(character));
                else
                {
                    result.push_back('%');
                    result.push_back(hex[character >> 4]);
                    result.push_back(hex[character & 0x0F]);
                }
            }
            return result;
        }

        struct HttpResponse
        {
            DWORD status = 0;
            DWORD error = ERROR_SUCCESS;
            std::string body;
        };

        HttpResponse Request(std::string_view url, std::wstring_view method,
                             std::string_view token, std::string_view clientId,
                             bool jsonResponse = false, std::string_view body = {},
                             int containerStart = -1, int containerSize = -1)
        {
            const std::wstring wideUrl = Wide(url);
            if (wideUrl.empty()) return {0, ERROR_INVALID_PARAMETER, {}};

            URL_COMPONENTS components{sizeof(components)};
            wchar_t host[256]{};
            wchar_t path[8192]{};
            components.lpszHostName = host;
            components.dwHostNameLength = static_cast<DWORD>(std::size(host));
            components.lpszUrlPath = path;
            components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
            if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0,
                                 &components))
            {
                return {0, GetLastError(), {}};
            }

            HINTERNET session = WinHttpOpen(L"AzerothPlex/0.5.0",
                                             WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session) return {0, GetLastError(), {}};
            WinHttpSetTimeouts(session, 10000, 10000, 10000, 30000);
            HINTERNET connection = WinHttpConnect(session, host, components.nPort, 0);
            if (!connection)
            {
                WinHttpCloseHandle(session);
                return {0, GetLastError(), {}};
            }
            const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS
                ? WINHTTP_FLAG_SECURE : 0;
            HINTERNET request = WinHttpOpenRequest(connection, method.data(), path, nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   flags);
            if (!request)
            {
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return {0, GetLastError(), {}};
            }

            std::wstring headers = jsonResponse
                ? L"Accept: application/json\r\n"
                : L"Accept: application/xml\r\n";
            headers += L"X-Plex-Client-Identifier: " + Wide(clientId) + L"\r\n";
            headers += L"X-Plex-Product: AzerothPlex\r\nX-Plex-Version: 0.5.0\r\n";
            headers += L"X-Plex-Platform: Windows\r\nX-Plex-Device: WarcraftXL\r\n";
            if (containerStart >= 0)
                headers += L"X-Plex-Container-Start: " +
                           std::to_wstring(containerStart) + L"\r\n";
            if (containerSize > 0)
                headers += L"X-Plex-Container-Size: " +
                           std::to_wstring(containerSize) + L"\r\n";
            if (!token.empty()) headers += L"X-Plex-Token: " + Wide(token) + L"\r\n";
            WinHttpAddRequestHeaders(request, headers.c_str(), -1L,
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

            const BOOL sent = WinHttpSendRequest(
                request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
            HttpResponse response;
            if (sent && WinHttpReceiveResponse(request, nullptr))
            {
                DWORD statusSize = sizeof(response.status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize,
                                    WINHTTP_NO_HEADER_INDEX);
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0)
                {
                    const size_t offset = response.body.size();
                    response.body.resize(offset + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, response.body.data() + offset, available, &read))
                    {
                        response.body.clear();
                        break;
                    }
                    response.body.resize(offset + read);
                    if (read == 0) break;
                }
            }
            else response.error = GetLastError();
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return response;
        }

        std::string JoinUrl(const PlexServer& server, std::string_view path)
        {
            return server.uri + (path.empty() || path.front() == '/' ? std::string(path)
                                                                       : "/" + std::string(path));
        }

    }

    bool ParsePinJson(std::string_view text, PinAuth& result)
    {
        try
        {
            const auto object = nlohmann::json::parse(text.begin(), text.end());
            result.id = object.value("id", 0);
            result.code = object.contains("code") && object["code"].is_string()
                ? object["code"].get<std::string>() : std::string{};
            result.authToken = object.contains("authToken") && object["authToken"].is_string()
                ? object["authToken"].get<std::string>() : std::string{};
            return result.id > 0 && (!result.code.empty() || !result.authToken.empty());
        }
        catch (const nlohmann::json::exception&)
        {
            return false;
        }
    }

    std::vector<PlexServer> ParseResourcesXml(std::string_view xml)
    {
        std::vector<PlexServer> result;
        size_t deviceStart = 0;
        while ((deviceStart = xml.find("<Device", deviceStart)) != std::string_view::npos)
        {
            const size_t deviceEnd = xml.find('>', deviceStart);
            if (deviceEnd == std::string_view::npos) break;
            const size_t close = xml.find("</Device>", deviceEnd);
            const size_t blockEnd = close == std::string_view::npos ? xml.size() : close;
            const std::string_view device = xml.substr(deviceStart, blockEnd - deviceStart);
            const std::string_view deviceTag = xml.substr(deviceStart, deviceEnd - deviceStart);
            const std::string name = Attribute(deviceTag, "name");
            const std::string token = Attribute(deviceTag, "accessToken");
            const std::string machineIdentifier = Attribute(deviceTag, "clientIdentifier");

            size_t connectionStart = device.find("<Connection");
            PlexServer best;
            std::string mediaUri;
            int bestRank = -1;
            while (connectionStart != std::string_view::npos)
            {
                const size_t connectionEnd = device.find('>', connectionStart);
                if (connectionEnd == std::string_view::npos) break;
                const std::string_view connection = device.substr(connectionStart,
                                                                    connectionEnd - connectionStart);
                if (Attribute(connection, "available") != "0")
                {
                    const std::string protocol = Attribute(connection, "protocol");
                    const int rank = protocol == "https" ? 2 : 1;
                    if (rank > bestRank)
                    {
                        bestRank = rank;
                        best.name = name;
                        best.accessToken = token;
                        best.machineIdentifier = machineIdentifier;
                        best.uri = Attribute(connection, "uri");
                        if (best.uri.empty())
                            best.uri = MakeServerUri(protocol, Attribute(connection, "address"),
                                                     Attribute(connection, "port"));
                    }
                    if (protocol == "http" && mediaUri.empty())
                        mediaUri = Attribute(connection, "uri");
                    if (protocol == "http" && mediaUri.empty())
                        mediaUri = MakeServerUri(protocol, Attribute(connection, "address"),
                                                 Attribute(connection, "port"));
                }
                connectionStart = device.find("<Connection", connectionEnd);
            }
            if (bestRank >= 0)
            {
                best.mediaUri = mediaUri.empty() ? best.uri : mediaUri;
                if (mediaUri.empty() && best.mediaUri.rfind("https://", 0) == 0 &&
                    best.mediaUri.find(".plex.direct") != std::string::npos)
                    best.mediaUri = "http://" + best.mediaUri.substr(8);
                result.push_back(std::move(best));
            }
            deviceStart = blockEnd;
        }
        return result;
    }

    std::vector<PlexSection> ParseSectionsXml(std::string_view xml)
    {
        std::vector<PlexSection> result;
        size_t cursor = 0;
        while ((cursor = xml.find("<Directory", cursor)) != std::string_view::npos)
        {
            const size_t end = xml.find('>', cursor);
            if (end == std::string_view::npos) break;
            const std::string_view tag = xml.substr(cursor, end - cursor);
            result.push_back({Attribute(tag, "key"), Attribute(tag, "title"),
                              Attribute(tag, "type")});
            cursor = end + 1;
        }
        return result;
    }

    std::vector<PlexItem> ParseItemsXml(std::string_view xml)
    {
        std::vector<PlexItem> result;
        size_t cursor = 0;
        while ((cursor = FindItemStart(xml, cursor)) != std::string_view::npos)
        {
            const size_t end = xml.find('>', cursor);
            if (end == std::string_view::npos) break;
            result.push_back(ParseItemTag(xml.substr(cursor, end - cursor)));
            cursor = end + 1;
        }
        return result;
    }

    PlexPage ParseItemsPageXml(std::string_view xml)
    {
        PlexPage result;
        const size_t containerStart = xml.find("<MediaContainer");
        if (containerStart != std::string_view::npos)
        {
            const size_t containerEnd = xml.find('>', containerStart);
            if (containerEnd != std::string_view::npos)
            {
                const std::string_view tag = xml.substr(containerStart,
                                                         containerEnd - containerStart);
                result.offset = std::max(0, IntegerAttribute(tag, "offset"));
                result.size = std::max(0, IntegerAttribute(tag, "size"));
                result.totalSize = std::max(0, IntegerAttribute(tag, "totalSize"));
            }
        }
        result.items = ParseItemsXml(xml);
        if (result.size == 0) result.size = static_cast<int>(result.items.size());
        if (result.totalSize == 0) result.totalSize = result.offset + result.size;
        return result;
    }

    PlexItem ParseMetadataXml(std::string_view xml)
    {
        PlexItem result;
        const size_t start = FindItemStart(xml, 0);
        if (start == std::string_view::npos) return result;
        const size_t tagEnd = xml.find('>', start);
        if (tagEnd == std::string_view::npos) return result;
        const std::string_view tag = xml.substr(start, tagEnd - start);
        result = ParseItemTag(tag);

        size_t guidStart = tagEnd;
        while ((guidStart = xml.find("<Guid", guidStart)) != std::string_view::npos)
        {
            const size_t guidEnd = xml.find('>', guidStart);
            if (guidEnd == std::string_view::npos) break;
            ApplyExternalGuid(result, Attribute(xml.substr(guidStart, guidEnd - guidStart), "id"));
            guidStart = guidEnd + 1;
        }

        const size_t partStart = xml.find("<Part", tagEnd);
        if (partStart != std::string_view::npos)
        {
            const size_t partEnd = xml.find('>', partStart);
            if (partEnd != std::string_view::npos)
            {
                result.partKey = Attribute(xml.substr(partStart, partEnd - partStart), "key");
                const size_t partClose = xml.find("</Part", partEnd);
                size_t streamStart = partEnd;
                while ((streamStart = xml.find("<Stream", streamStart)) != std::string_view::npos)
                {
                    const size_t streamEnd = xml.find('>', streamStart);
                    if (streamEnd == std::string_view::npos ||
                        (partClose != std::string_view::npos && streamStart > partClose))
                        break;
                    const std::string_view stream = xml.substr(streamStart, streamEnd - streamStart);
                    PlexStream parsed;
                    parsed.id = IntegerAttribute(stream, "id");
                    parsed.streamType = IntegerAttribute(stream, "streamType");
                    parsed.language = Attribute(stream, "language");
                    parsed.title = Attribute(stream, "title");
                    parsed.codec = Attribute(stream, "codec");
                    parsed.forced = IntegerAttribute(stream, "forced") != 0;
                    parsed.hearingImpaired = IntegerAttribute(stream, "hearingImpaired") != 0;
                    parsed.selected = IntegerAttribute(stream, "selected") != 0;
                    result.streams.push_back(std::move(parsed));
                    streamStart = streamEnd + 1;
                }
            }
        }

        size_t markerStart = tagEnd;
        while ((markerStart = xml.find("<Marker", markerStart)) != std::string_view::npos)
        {
            const size_t markerEnd = xml.find('>', markerStart);
            if (markerEnd == std::string_view::npos) break;
            const std::string_view marker = xml.substr(markerStart, markerEnd - markerStart);
            PlexMarker parsed;
            parsed.type = Attribute(marker, "type");
            parsed.startMs = IntegerAttribute(marker, "startTimeOffset");
            parsed.endMs = IntegerAttribute(marker, "endTimeOffset");
            if (!parsed.type.empty() && parsed.endMs >= parsed.startMs)
                result.markers.push_back(std::move(parsed));
            markerStart = markerEnd + 1;
        }
        return result;
    }

    std::string BuildChildrenPath(std::string_view ratingKey)
    {
        return "/library/metadata/" + std::string(ratingKey) + "/children";
    }

    std::string BuildWatchlistPath()
    {
        return "/library/sections/watchlist/all?includeCollections=1&includeExternalMedia=1";
    }

    std::string BuildHomePath(PlexHomeKind kind)
    {
        switch (kind)
        {
        case PlexHomeKind::ContinueWatching:
            return "/hubs/home/continueWatching?includeGuids=1";
        case PlexHomeKind::RecentlyAddedMovies:
            return "/library/recentlyAdded?type=1&includeGuids=1";
        case PlexHomeKind::RecentlyAddedTV:
            return "/library/recentlyAdded?type=4&includeGuids=1";
        case PlexHomeKind::OnDeck:
            return "/library/onDeck?includeGuids=1";
        case PlexHomeKind::RecentlyWatched:
            return "/status/sessions/history/all?includeGuids=1";
        }
        return "/hubs/home/continueWatching?includeGuids=1";
    }

    PlexPlayback BuildDirectPlayback(const PlexItem& item, const PlexServer& server)
    {
        PlexPlayback result;
        const std::string& mediaUri = server.mediaUri.empty() ? server.uri : server.mediaUri;
        if (mediaUri.empty() || item.partKey.empty()) return result;
        result.uri = mediaUri + item.partKey + "?X-Plex-Token=" + server.accessToken;
        return result;
    }

    PlexPlayback BuildTranscodePlayback(const PlexItem& item, const PlexServer& server,
                                        std::string_view sessionId)
    {
        PlexPlayback result;
        const std::string& mediaUri = server.mediaUri.empty() ? server.uri : server.mediaUri;
        if (mediaUri.empty() || item.partKey.empty()) return result;
        const std::string source = mediaUri + item.partKey;
        result.uri = mediaUri + "/video/:/transcode/universal/start.m3u8?path=" +
                     UrlEncode(source) + "&protocol=hls&session=" + UrlEncode(sessionId) +
                     "&copyts=1&mediaIndex=0&videoResolution=1080&videoQuality=100&audioBoost=100";
        if (!server.accessToken.empty()) result.uri += "&X-Plex-Token=" + UrlEncode(server.accessToken);
        result.sessionId = std::string(sessionId);
        result.transcoded = true;
        return result;
    }

    std::string BuildTimelineRequest(const PlexItem& item, TimelineState state, int positionMs)
    {
        return "/:/timeline?ratingKey=" + item.ratingKey + "&state=" + StateName(state) +
               "&time=" + std::to_string(std::max(0, positionMs));
    }

    PlexClient::PlexClient(std::filesystem::path dataDirectory)
        : dataDirectory_(std::move(dataDirectory)),
          worker_([this](std::stop_token stop) {
              while (!stop.stop_requested())
              {
                  Job job;
                  {
                      std::unique_lock lock(jobsMutex_);
                      jobsChanged_.wait(lock, stop, [this] { return !jobs_.empty(); });
                      if (stop.stop_requested()) return;
                      job = std::move(jobs_.front());
                      jobs_.pop_front();
                  }
                  job(stop);
              }
          })
    {
        token_ = wxl_token::TokenStore(dataDirectory_).Load();
        Enqueue([this](std::stop_token) {
            Emit({token_.empty() ? PlexEvent::Kind::LoginRequired
                                 : PlexEvent::Kind::LoginSucceeded, {}, {}, {}, {}, {}});
            if (!token_.empty())
            {
                LoadResources();
                LoadWatchlist();
            }
        });
    }

    PlexClient::~PlexClient()
    {
        worker_.request_stop();
        jobsChanged_.notify_all();
    }

    void PlexClient::Enqueue(Job job)
    {
        {
            std::lock_guard lock(jobsMutex_);
            jobs_.push_back(std::move(job));
        }
        jobsChanged_.notify_one();
    }

    void PlexClient::Emit(PlexEvent event)
    {
        std::lock_guard lock(eventsMutex_);
        events_.push_back(std::move(event));
    }

    bool PlexClient::PopEvent(PlexEvent& event)
    {
        std::lock_guard lock(eventsMutex_);
        if (events_.empty()) return false;
        event = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    void PlexClient::StartLogin()
    {
        Enqueue([this](std::stop_token stop) {
            if (!token_.empty())
            {
                LoadResources();
                return;
            }

            const HttpResponse response = Request("https://plex.tv/api/v2/pins?strong=true",
                                                  L"POST", {}, clientId_, true);
            PinAuth pin{};
            if (response.status < 200 || response.status >= 300 ||
                !ParsePinJson(response.body, pin))
            {
                Emit({PlexEvent::Kind::Error,
                      "Plex login PIN creation failed (HTTP " + std::to_string(response.status) +
                      ", WinHTTP " + std::to_string(response.error) + ")"});
                return;
            }

            const std::string authUrl =
                "https://app.plex.tv/auth#?clientID=" + UrlEncode(clientId_) +
                "&code=" + UrlEncode(pin.code) +
                "&context%5Bdevice%5D%5Bproduct%5D=AzerothPlex";
            const std::wstring wideAuthUrl = Wide(authUrl);
            ShellExecuteW(nullptr, L"open", wideAuthUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Emit({PlexEvent::Kind::LoginPending, "Complete Plex login in your browser"});
            PollLogin(stop, pin.id);
        });
    }

    void PlexClient::PollLogin(std::stop_token stop, int pinId)
    {
        for (int attempt = 0; attempt < 120 && !stop.stop_requested(); ++attempt)
        {
            Sleep(1000);
            const HttpResponse response = Request(
                "https://plex.tv/api/v2/pins/" + std::to_string(pinId), L"GET", {}, clientId_, true);
            PinAuth pin{};
            if (response.status >= 200 && response.status < 300 && ParsePinJson(response.body, pin) &&
                !pin.authToken.empty())
            {
                token_ = pin.authToken;
                if (!wxl_token::TokenStore(dataDirectory_).Save(token_))
                {
                    Emit({PlexEvent::Kind::Error, "Plex token could not be stored"});
                    return;
                }
                Emit({PlexEvent::Kind::LoginSucceeded, "Plex login complete"});
                LoadResources();
                LoadWatchlist();
                return;
            }
        }
        if (!stop.stop_requested()) Emit({PlexEvent::Kind::Error, "Plex login timed out"});
    }

    void PlexClient::LoadResources()
    {
        Enqueue([this](std::stop_token) {
            if (token_.empty())
            {
                Emit({PlexEvent::Kind::LoginRequired});
                return;
            }
            const HttpResponse response = Request("https://plex.tv/api/resources?includeHttps=1",
                                                  L"GET", token_, clientId_);
            auto servers = ParseResourcesXml(response.body);
            for (auto& server : servers)
                if (server.accessToken.empty()) server.accessToken = token_;
            if (response.status < 200 || response.status >= 300 || servers.empty())
            {
                Emit({PlexEvent::Kind::Error, "No reachable Plex servers found"});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Servers};
            event.servers = std::move(servers);
            Emit(std::move(event));
        });
    }

    void PlexClient::LoadWatchlist(int offset, int size)
    {
        Enqueue([this, offset, size](std::stop_token stop) {
            if (stop.stop_requested()) return;
            PlexEvent event{PlexEvent::Kind::Watchlist};
            if (token_.empty())
            {
                event.message = "Sign in to load the Plex watchlist.";
                Emit(std::move(event));
                return;
            }

            const HttpResponse response = Request(
                "https://discover.provider.plex.tv" + BuildWatchlistPath(), L"GET",
                token_, clientId_, false, {}, offset, size);
            if (response.status < 200 || response.status >= 300)
            {
                event.message = "Plex watchlist could not be loaded.";
                Emit(std::move(event));
                return;
            }
            const PlexPage page = ParseItemsPageXml(response.body);
            event.items = page.items;
            event.pageOffset = page.offset;
            event.pageSize = page.size;
            event.pageTotalSize = page.totalSize;
            Emit(std::move(event));
        });
    }

    void PlexClient::LoadHome(const PlexServer& server, PlexHomeKind kind, int offset, int size)
    {
        Enqueue([this, server, kind, offset, size](std::stop_token stop) {
            if (stop.stop_requested()) return;
            const std::string token = server.accessToken.empty() ? token_ : server.accessToken;
            const HttpResponse response = Request(
                JoinUrl(server, BuildHomePath(kind)), L"GET", token, clientId_,
                false, {}, offset, size);
            if (response.status < 200 || response.status >= 300)
            {
                PlexEvent error{PlexEvent::Kind::Error};
                error.message = "The selected Plex home section could not be loaded";
                error.preserveView = true;
                error.homeKind = kind;
                Emit(std::move(error));
                return;
            }
            const PlexPage page = ParseItemsPageXml(response.body);
            PlexEvent event{PlexEvent::Kind::HomeItems};
            event.items = page.items;
            event.homeKind = kind;
            event.pageOffset = page.offset;
            event.pageSize = page.size;
            event.pageTotalSize = page.totalSize;
            Emit(std::move(event));
        });
    }

    void PlexClient::LoadSections(const PlexServer& server)
    {
        Enqueue([this, server](std::stop_token) {
            const HttpResponse response = Request(JoinUrl(server, "/library/sections"), L"GET",
                                                  server.accessToken.empty() ? token_ : server.accessToken,
                                                  clientId_);
            const auto sections = ParseSectionsXml(response.body);
            if (response.status < 200 || response.status >= 300 || sections.empty())
            {
                Emit({PlexEvent::Kind::Error, "Plex libraries could not be loaded",
                      {}, {}, {}, {}, true});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Sections};
            event.sections = sections;
            Emit(std::move(event));
        });
    }

    void PlexClient::LoadItems(const PlexServer& server, const PlexSection& section,
                               int offset, int size)
    {
        Enqueue([this, server, section, offset, size](std::stop_token) {
            const HttpResponse response = Request(
                JoinUrl(server, "/library/sections/" + section.key + "/all"), L"GET",
                server.accessToken.empty() ? token_ : server.accessToken, clientId_,
                false, {}, offset, size);
            const PlexPage page = ParseItemsPageXml(response.body);
            if (response.status < 200 || response.status >= 300)
            {
                Emit({PlexEvent::Kind::Error, "Plex library items could not be loaded",
                      {}, {}, {}, {}, true});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Items};
            event.items = page.items;
            event.pageOffset = page.offset;
            event.pageSize = page.size;
            event.pageTotalSize = page.totalSize;
            Emit(std::move(event));
        });
    }

    void PlexClient::LoadChildren(const PlexServer& server, const PlexItem& item,
                                  int offset, int size)
    {
        Enqueue([this, server, item, offset, size](std::stop_token) {
            const std::string token = server.accessToken.empty() ? token_ : server.accessToken;
            const HttpResponse response = Request(
                JoinUrl(server, BuildChildrenPath(item.ratingKey)), L"GET", token, clientId_,
                false, {}, offset, size);
            const PlexPage page = ParseItemsPageXml(response.body);
            if (response.status < 200 || response.status >= 300)
            {
                Emit({PlexEvent::Kind::Error, "Plex child items could not be loaded", {}, {}, {}, {}, true});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Children};
            event.items = page.items;
            event.pageOffset = page.offset;
            event.pageSize = page.size;
            event.pageTotalSize = page.totalSize;
            Emit(std::move(event));
        });
    }

    void PlexClient::ResolveWatchlistItem(const PlexServer& server, const PlexItem& item)
    {
        Enqueue([this, server, item](std::stop_token) {
            if (item.guid.empty())
            {
                Emit({PlexEvent::Kind::Error, "This watchlist item has no server match", {}, {}, {}, {}, true});
                return;
            }

            const std::string token = server.accessToken.empty() ? token_ : server.accessToken;
            const HttpResponse response = Request(
                JoinUrl(server, "/library/all?guid=" + UrlEncode(item.guid)),
                L"GET", token, clientId_);
            const auto matches = ParseItemsXml(response.body);
            if (response.status < 200 || response.status >= 300 || matches.empty())
            {
                Emit({PlexEvent::Kind::Error,
                      "The watchlist item is not available on the selected Plex server",
                      {}, {}, {}, {}, true});
                return;
            }
            PlexEvent event{PlexEvent::Kind::WatchlistResolved};
            event.item = matches.front();
            Emit(std::move(event));
        });
    }

    void PlexClient::Search(const PlexServer& server, std::string query,
                            int offset, int size)
    {
        Enqueue([this, server, query = std::move(query), offset, size](std::stop_token) {
            const HttpResponse response = Request(
                JoinUrl(server, "/hubs/search?query=" + UrlEncode(query)), L"GET",
                server.accessToken.empty() ? token_ : server.accessToken, clientId_,
                false, {}, offset, size);
            const PlexPage page = ParseItemsPageXml(response.body);
            if (response.status < 200 || response.status >= 300)
            {
                Emit({PlexEvent::Kind::Error, "Plex search failed",
                      {}, {}, {}, {}, true});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Items};
            event.items = page.items;
            event.pageOffset = page.offset;
            event.pageSize = page.size;
            event.pageTotalSize = page.totalSize;
            Emit(std::move(event));
        });
    }

    void PlexClient::LoadMetadata(const PlexServer& server, const PlexItem& item)
    {
        Enqueue([this, server, item](std::stop_token) {
            const HttpResponse response = Request(
                JoinUrl(server, "/library/metadata/" + item.ratingKey), L"GET",
                server.accessToken.empty() ? token_ : server.accessToken, clientId_);
            if (response.status < 200 || response.status >= 300)
            {
                Emit({PlexEvent::Kind::Error, "Plex metadata could not be loaded",
                      {}, {}, {}, {}, true});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Metadata};
            event.item = ParseMetadataXml(response.body);
            Emit(std::move(event));
        });
    }

    void PlexClient::LoadNextEpisode(const PlexServer& server, const PlexItem& item)
    {
        Enqueue([this, server, item](std::stop_token stop) {
            PlexEvent event{PlexEvent::Kind::NextEpisode};
            if (stop.stop_requested() || item.type != "episode" || item.grandparentRatingKey.empty())
            {
                event.message = "No next episode is available.";
                Emit(std::move(event));
                return;
            }
            const std::string token = server.accessToken.empty() ? token_ : server.accessToken;
            const HttpResponse response = Request(
                JoinUrl(server, "/library/metadata/" + item.grandparentRatingKey + "/allLeaves"),
                L"GET", token, clientId_);
            if (response.status < 200 || response.status >= 300)
            {
                event.message = "The next episode could not be loaded.";
                Emit(std::move(event));
                return;
            }
            const auto episodes = ParseItemsXml(response.body);
            size_t current = episodes.size();
            for (size_t index = 0; index < episodes.size(); ++index)
            {
                if ((!item.ratingKey.empty() && episodes[index].ratingKey == item.ratingKey) ||
                    (item.parentIndex > 0 && item.index > 0 &&
                     episodes[index].parentIndex == item.parentIndex && episodes[index].index == item.index))
                {
                    current = index;
                    break;
                }
            }
            if (current == episodes.size() || current + 1 >= episodes.size())
            {
                event.message = "No next episode is available.";
                Emit(std::move(event));
                return;
            }
            const PlexItem& next = episodes[current + 1];
            const HttpResponse metadata = Request(
                JoinUrl(server, "/library/metadata/" + next.ratingKey), L"GET", token, clientId_);
            if (metadata.status >= 200 && metadata.status < 300)
                event.item = ParseMetadataXml(metadata.body);
            if (event.item.ratingKey.empty()) event.message = "The next episode could not be resolved.";
            Emit(std::move(event));
        });
    }

    void PlexClient::ResolvePartyMedia(const PlexServer& server, const PlexItem& hint,
                                       std::string remoteServerIdentifier)
    {
        Enqueue([this, server, hint, remoteServerIdentifier = std::move(remoteServerIdentifier)](std::stop_token stop) {
            PlexEvent event{PlexEvent::Kind::PartyMediaResolved};
            if (stop.stop_requested()) return;
            const std::string token = server.accessToken.empty() ? token_ : server.accessToken;

            auto loadMetadata = [&](const PlexItem& candidate) -> PlexItem {
                if (candidate.ratingKey.empty()) return {};
                const HttpResponse metadata = Request(
                    JoinUrl(server, "/library/metadata/" + candidate.ratingKey), L"GET", token, clientId_);
                return metadata.status >= 200 && metadata.status < 300
                    ? ParseMetadataXml(metadata.body) : PlexItem{};
            };

            if (!remoteServerIdentifier.empty() &&
                remoteServerIdentifier == server.machineIdentifier && !hint.ratingKey.empty())
            {
                event.item = loadMetadata(hint);
                if (!event.item.ratingKey.empty())
                {
                    Emit(std::move(event));
                    return;
                }
            }

            std::vector<std::string> guids;
            if (!hint.guid.empty()) guids.push_back(hint.guid);
            if (!hint.tmdbId.empty()) guids.push_back("tmdb://" + hint.tmdbId);
            if (!hint.tvdbId.empty()) guids.push_back("tvdb://" + hint.tvdbId);
            if (!hint.imdbId.empty()) guids.push_back("imdb://" + hint.imdbId);
            for (const auto& guid : guids)
            {
                const HttpResponse response = Request(
                    JoinUrl(server, "/library/all?guid=" + UrlEncode(guid)), L"GET", token, clientId_);
                const auto matches = ParseItemsXml(response.body);
                if (response.status >= 200 && response.status < 300 && matches.size() == 1)
                {
                    event.item = loadMetadata(matches.front());
                    if (!event.item.ratingKey.empty())
                    {
                        Emit(std::move(event));
                        return;
                    }
                }
            }

            if (!hint.title.empty())
            {
                const HttpResponse response = Request(
                    JoinUrl(server, "/hubs/search?query=" + UrlEncode(hint.title)), L"GET", token, clientId_);
                const auto matches = ParseItemsXml(response.body);
                std::vector<PlexItem> candidates;
                for (const auto& candidate : matches)
                {
                    if (!hint.type.empty() && candidate.type != hint.type) continue;
                    if (hint.year > 0 && candidate.year > 0 && hint.year != candidate.year) continue;
                    if (hint.type == "episode")
                    {
                        if (hint.parentIndex > 0 && candidate.parentIndex > 0 &&
                            hint.parentIndex != candidate.parentIndex) continue;
                        if (hint.index > 0 && candidate.index > 0 && hint.index != candidate.index) continue;
                        if (!hint.grandparentTitle.empty() && !candidate.grandparentTitle.empty() &&
                            hint.grandparentTitle != candidate.grandparentTitle) continue;
                    }
                    if (hint.durationMs > 0 && candidate.durationMs > 0)
                    {
                        const int tolerance = std::max(5000, std::min(hint.durationMs, candidate.durationMs) / 100);
                        if (std::abs(hint.durationMs - candidate.durationMs) > tolerance) continue;
                    }
                    candidates.push_back(candidate);
                }
                if (candidates.size() == 1)
                {
                    event.item = loadMetadata(candidates.front());
                    if (!event.item.ratingKey.empty())
                    {
                        Emit(std::move(event));
                        return;
                    }
                }
                if (candidates.size() > 1)
                    event.message = "Party media matched multiple Plex items; choose it manually.";
            }
            if (event.message.empty())
                event.message = "This party media could not be matched in your Plex library.";
            Emit(std::move(event));
        });
    }

    void PlexClient::ReportTimeline(const PlexServer& server, const PlexItem& item,
                                    TimelineState state, int positionMs)
    {
        Enqueue([this, server, item, state, positionMs](std::stop_token) {
            std::string path = BuildTimelineRequest(item, state, positionMs);
            const std::string token = server.accessToken.empty() ? token_ : server.accessToken;
            if (!token.empty()) path += "&X-Plex-Token=" + UrlEncode(token);
            Request(JoinUrl(server, path), L"GET", token, clientId_);
        });
    }

    void PlexClient::SignOut()
    {
        Enqueue([this](std::stop_token) {
            token_.clear();
            wxl_token::TokenStore(dataDirectory_).Clear();
            Emit({PlexEvent::Kind::LoginRequired, "Signed out of Plex"});
        });
    }
}
