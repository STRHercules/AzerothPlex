#include "../TokenStore.hpp"

#include <cassert>
#include <filesystem>

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / "AzerothPlex-token-test";
    std::filesystem::remove_all(directory);

    wxl_token::TokenStore store(directory);
    assert(store.Load().empty());
    assert(store.Save("plex-token"));
    assert(store.Load() == "plex-token");
    assert(store.Clear());
    assert(store.Load().empty());
    std::filesystem::remove_all(directory);
    return 0;
}
