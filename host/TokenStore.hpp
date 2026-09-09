#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace wxl_token
{
    class TokenStore final
    {
    public:
        explicit TokenStore(std::filesystem::path directory);

        std::string Load() const;
        bool Save(std::string_view token) const;
        bool Clear() const;

    private:
        std::filesystem::path path_;
    };
}
