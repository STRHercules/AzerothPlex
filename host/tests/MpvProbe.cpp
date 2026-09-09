#include <mpv/client.h>

#include <windows.h>

namespace
{
    template <typename Function>
    Function Resolve(HMODULE module, const char* name)
    {
        return reinterpret_cast<Function>(GetProcAddress(module, name));
    }
}

int main()
{
    HMODULE module = LoadLibraryW(L"libmpv-2.dll");
    if (!module) return 1;

    const auto create = Resolve<mpv_handle* (*)()>(module, "mpv_create");
    const auto setOption = Resolve<int (*)(mpv_handle*, const char*, const char*)>(
        module, "mpv_set_option_string");
    const auto initialize = Resolve<int (*)(mpv_handle*)>(module, "mpv_initialize");
    const auto destroy = Resolve<void (*)(mpv_handle*)>(module, "mpv_destroy");
    if (!create || !setOption || !initialize || !destroy)
    {
        FreeLibrary(module);
        return 2;
    }

    mpv_handle* handle = create();
    if (!handle)
    {
        FreeLibrary(module);
        return 3;
    }

    setOption(handle, "vo", "libmpv");
    const int result = initialize(handle);
    destroy(handle);
    FreeLibrary(module);
    return result >= 0 ? 0 : 4;
}
