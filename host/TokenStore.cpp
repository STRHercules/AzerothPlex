#include "TokenStore.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <fstream>
#include <vector>

namespace wxl_token
{
    TokenStore::TokenStore(std::filesystem::path directory)
        : path_(std::move(directory) / L"token.dat")
    {
    }

    std::string TokenStore::Load() const
    {
        std::ifstream input(path_, std::ios::binary);
        if (!input) return {};
        const std::vector<char> protectedBytes((std::istreambuf_iterator<char>(input)), {});
        if (protectedBytes.empty()) return {};

        DATA_BLOB encrypted{
            static_cast<DWORD>(protectedBytes.size()),
            reinterpret_cast<BYTE*>(const_cast<char*>(protectedBytes.data()))};
        DATA_BLOB clear{};
        if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, &clear))
            return {};

        std::string result(reinterpret_cast<char*>(clear.pbData), clear.cbData);
        LocalFree(clear.pbData);
        return result;
    }

    bool TokenStore::Save(std::string_view token) const
    {
        if (token.empty()) return Clear();
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) return false;

        DATA_BLOB clear{
            static_cast<DWORD>(token.size()),
            reinterpret_cast<BYTE*>(const_cast<char*>(token.data()))};
        DATA_BLOB encrypted{};
        if (!CryptProtectData(&clear, L"AzerothPlex", nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &encrypted))
            return false;

        const auto temporary = path_.wstring() + L".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            LocalFree(encrypted.pbData);
            return false;
        }
        output.write(reinterpret_cast<const char*>(encrypted.pbData), encrypted.cbData);
        output.close();
        LocalFree(encrypted.pbData);
        if (!output) return false;
        return MoveFileExW(temporary.c_str(), path_.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    bool TokenStore::Clear() const
    {
        if (DeleteFileW(path_.c_str())) return true;
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
}
