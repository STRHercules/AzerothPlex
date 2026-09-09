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
            result.title = Attribute(tag, "title");
            result.type = Attribute(tag, "type");
            result.grandparentTitle = Attribute(tag, "grandparentTitle");
            result.thumb = Attribute(tag, "thumb");
            result.viewOffsetMs = IntegerAttribute(tag, "viewOffset");
            result.durationMs = IntegerAttribute(tag, "duration");
            result.viewed = IntegerAttribute(tag, "viewCount") > 0;
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
                             std::string_view body = {})
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

            std::wstring headers = L"Accept: application/json, application/xml\r\n";
            headers += L"X-Plex-Client-Identifier: " + Wide(clientId) + L"\r\n";
            headers += L"X-Plex-Product: AzerothPlex\r\nX-Plex-Version: 0.5.0\r\n";
            headers += L"X-Plex-Platform: Windows\r\nX-Plex-Device: WarcraftXL\r\n";
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
            const std::string name = Attribute(xml.substr(deviceStart, deviceEnd - deviceStart), "name");
            const std::string token = Attribute(xml.substr(deviceStart, deviceEnd - deviceStart), "accessToken");

            size_t connectionStart = device.find("<Connection");
            PlexServer best;
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
                        best.uri = Attribute(connection, "uri");
                        if (best.uri.empty())
                            best.uri = MakeServerUri(protocol, Attribute(connection, "address"),
                                                     Attribute(connection, "port"));
                    }
                }
                connectionStart = device.find("<Connection", connectionEnd);
            }
            if (bestRank >= 0) result.push_back(std::move(best));
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
        while ((cursor = xml.find("<Video", cursor)) != std::string_view::npos)
        {
            const size_t end = xml.find('>', cursor);
            if (end == std::string_view::npos) break;
            result.push_back(ParseItemTag(xml.substr(cursor, end - cursor)));
            cursor = end + 1;
        }
        return result;
    }

    PlexItem ParseMetadataXml(std::string_view xml)
    {
        PlexItem result;
        const size_t start = xml.find("<Video");
        if (start == std::string_view::npos) return result;
        const size_t tagEnd = xml.find('>', start);
        if (tagEnd == std::string_view::npos) return result;
        const std::string_view tag = xml.substr(start, tagEnd - start);
        result = ParseItemTag(tag);

        const size_t partStart = xml.find("<Part", tagEnd);
        if (partStart != std::string_view::npos)
        {
            const size_t partEnd = xml.find('>', partStart);
            if (partEnd != std::string_view::npos)
            {
                result.partKey = Attribute(xml.substr(partStart, partEnd - partStart), "key");
                size_t streamStart = partEnd;
                while ((streamStart = xml.find("<Stream", streamStart)) != std::string_view::npos)
                {
                    const size_t streamEnd = xml.find('>', streamStart);
                    if (streamEnd == std::string_view::npos || streamStart > xml.find("</Part", partEnd))
                        break;
                    const std::string_view stream = xml.substr(streamStart, streamEnd - streamStart);
                    result.streams.push_back({IntegerAttribute(stream, "id"),
                                              IntegerAttribute(stream, "streamType"),
                                              Attribute(stream, "language"),
                                              Attribute(stream, "title")});
                    streamStart = streamEnd + 1;
                }
            }
        }
        return result;
    }

    PlexPlayback BuildDirectPlayback(const PlexItem& item, const PlexServer& server)
    {
        PlexPlayback result;
        if (server.uri.empty() || item.partKey.empty()) return result;
        result.uri = server.uri + item.partKey + "?X-Plex-Token=" + server.accessToken;
        return result;
    }

    PlexPlayback BuildTranscodePlayback(const PlexItem& item, const PlexServer& server,
                                        std::string_view sessionId)
    {
        PlexPlayback result;
        if (server.uri.empty() || item.partKey.empty()) return result;
        const std::string source = server.uri + item.partKey;
        result.uri = server.uri + "/video/:/transcode/universal/start.m3u8?path=" +
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
            if (!token_.empty()) LoadResources();
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
                                                  L"POST", {}, clientId_);
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
                "https://plex.tv/api/v2/pins/" + std::to_string(pinId), L"GET", {}, clientId_);
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

    void PlexClient::LoadSections(const PlexServer& server)
    {
        Enqueue([this, server](std::stop_token) {
            const HttpResponse response = Request(JoinUrl(server, "/library/sections"), L"GET",
                                                  server.accessToken.empty() ? token_ : server.accessToken,
                                                  clientId_);
            const auto sections = ParseSectionsXml(response.body);
            if (response.status < 200 || response.status >= 300 || sections.empty())
            {
                Emit({PlexEvent::Kind::Error, "Plex libraries could not be loaded"});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Sections};
            event.sections = sections;
            Emit(std::move(event));
        });
    }

    void PlexClient::LoadItems(const PlexServer& server, const PlexSection& section)
    {
        Enqueue([this, server, section](std::stop_token) {
            const HttpResponse response = Request(JoinUrl(server, "/library/sections/" + section.key + "/all"),
                                                  L"GET", server.accessToken.empty() ? token_ : server.accessToken,
                                                  clientId_);
            const auto items = ParseItemsXml(response.body);
            if (response.status < 200 || response.status >= 300)
            {
                Emit({PlexEvent::Kind::Error, "Plex library items could not be loaded"});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Items};
            event.items = items;
            Emit(std::move(event));
        });
    }

    void PlexClient::Search(const PlexServer& server, std::string query)
    {
        Enqueue([this, server, query = std::move(query)](std::stop_token) {
            const HttpResponse response = Request(
                JoinUrl(server, "/hubs/search?query=" + UrlEncode(query)), L"GET",
                server.accessToken.empty() ? token_ : server.accessToken, clientId_);
            const auto items = ParseItemsXml(response.body);
            if (response.status < 200 || response.status >= 300)
            {
                Emit({PlexEvent::Kind::Error, "Plex search failed"});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Items};
            event.items = items;
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
                Emit({PlexEvent::Kind::Error, "Plex metadata could not be loaded"});
                return;
            }
            PlexEvent event{PlexEvent::Kind::Metadata};
            event.item = ParseMetadataXml(response.body);
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
