#include "../TokenStore.hpp"

#include <filesystem>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / "AzerothPlex-token-test";
    std::filesystem::remove_all(directory);

    wxl_token::TokenStore store(directory);
    CHECK(store.Load().empty());
    CHECK(store.Save("plex-token"));
    CHECK(store.Load() == "plex-token");
    CHECK(store.Clear());
    CHECK(store.Load().empty());
    std::filesystem::remove_all(directory);
    return 0;
}
