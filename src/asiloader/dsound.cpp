// dsound.dll stand-in that loads the *.asi plugins next to Beast of Reincarnation's exe: the mod's own ASI loader.
// Another ASI loader (Ultimate ASI Loader, for example) can load the plugin in its place.
//
// Why dsound: the exe imports DSOUND.dll as a regular import (not delay-loaded), so this DLL is loaded with the exe,
// before any engine code runs; dsound is not a KnownDLL, so Windows takes it from the game folder; ReShade (dxgi.dll)
// and UE4SS (dwmapi.dll) use other names; and it exports only 12 functions, all by name, forwarded below.
//
// What it does in DllMain (process attach, only in the game's exe):
//   - LoadLibrary every *.asi in the exe's folder, in name order;
//   - write what it loaded, and the error of anything that failed, to BoRAsiLoader.log next to it (the previous run
//     is kept as BoRAsiLoader.prev.log).
// DirectSound calls are forwarded to System32\dsound.dll, loaded on first use (not in DllMain).
#include <windows.h>
#include <dsound.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

static wchar_t g_dir[MAX_PATH] = {};

static void Log(const wchar_t* fmt, ...)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sBoRAsiLoader.log", g_dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") != 0 || !f)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [pid %lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

static void LoadPlugins()
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    const HANDLE find = FindFirstFileW((std::wstring(g_dir) + L"*.asi").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                names.emplace_back(fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    Log(L"%zu plugin(s) in %s", names.size(), g_dir);
    for (const std::wstring& name : names) {
        const std::wstring path = std::wstring(g_dir) + name;
        if (const HMODULE m = LoadLibraryW(path.c_str()))
            Log(L"loaded %s at %p", name.c_str(), static_cast<void*>(m));
        else
            Log(L"FAILED to load %s: error %lu", name.c_str(), GetLastError());
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    wchar_t self[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameW(module, self, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wcscpy_s(g_dir, self);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\'))
        slash[1] = 0;
    // Only the game itself (BeastOfReincarnation-WinGDK-Shipping.exe from the Xbox app, -Win64- from Steam): stay out
    // of any other process that might pick this file up (no log either, so it can't rotate the game's log away).
    const wchar_t* exe_name = wcsrchr(exe, L'\\');
    exe_name = exe_name ? exe_name + 1 : exe;
    constexpr wchar_t kPrefix[] = L"BeastOfReincarnation-", kSuffix[] = L"-Shipping.exe";
    const size_t len = wcslen(exe_name), pre = std::size(kPrefix) - 1, suf = std::size(kSuffix) - 1;
    if (len <= pre + suf || _wcsnicmp(exe_name, kPrefix, pre) != 0 || _wcsicmp(exe_name + len - suf, kSuffix) != 0)
        return TRUE;

    wchar_t log[MAX_PATH], prev[MAX_PATH];
    swprintf_s(log, L"%sBoRAsiLoader.log", g_dir);
    swprintf_s(prev, L"%sBoRAsiLoader.prev.log", g_dir);
    MoveFileExW(log, prev, MOVEFILE_REPLACE_EXISTING);
    Log(L"BoRAsiLoader (dsound.dll) in %s", exe);
    LoadPlugins();
    return TRUE;
}

// ------------------------------------------------------------------ DirectSound forwarding
static FARPROC Real(const char* name)
{
    static HMODULE real = [] {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dsound.dll");
        return LoadLibraryW(path);
    }();
    return real ? GetProcAddress(real, name) : nullptr;
}

template <typename F>
static F RealAs(const char* name)
{
    return reinterpret_cast<F>(Real(name));
}

#define FORWARD(name, type, ...)                         \
    static const auto fn = RealAs<type>(#name);          \
    return fn ? fn(__VA_ARGS__) : E_NOTIMPL

extern "C" {

HRESULT WINAPI Proxy_DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND* ds, LPUNKNOWN outer)
{
    FORWARD(DirectSoundCreate, decltype(&Proxy_DirectSoundCreate), guid, ds, outer);
}

HRESULT WINAPI Proxy_DirectSoundEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx)
{
    FORWARD(DirectSoundEnumerateA, decltype(&Proxy_DirectSoundEnumerateA), cb, ctx);
}

HRESULT WINAPI Proxy_DirectSoundEnumerateW(LPDSENUMCALLBACKW cb, LPVOID ctx)
{
    FORWARD(DirectSoundEnumerateW, decltype(&Proxy_DirectSoundEnumerateW), cb, ctx);
}

HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    // While a plugin is loaded through this DLL it must stay.
    return S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* out)
{
    FORWARD(DllGetClassObject, decltype(&Proxy_DllGetClassObject), clsid, iid, out);
}

HRESULT WINAPI Proxy_DirectSoundCaptureCreate(LPCGUID guid, LPDIRECTSOUNDCAPTURE* dsc, LPUNKNOWN outer)
{
    FORWARD(DirectSoundCaptureCreate, decltype(&Proxy_DirectSoundCaptureCreate), guid, dsc, outer);
}

HRESULT WINAPI Proxy_DirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx)
{
    FORWARD(DirectSoundCaptureEnumerateA, decltype(&Proxy_DirectSoundCaptureEnumerateA), cb, ctx);
}

HRESULT WINAPI Proxy_DirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW cb, LPVOID ctx)
{
    FORWARD(DirectSoundCaptureEnumerateW, decltype(&Proxy_DirectSoundCaptureEnumerateW), cb, ctx);
}

HRESULT WINAPI Proxy_GetDeviceID(LPCGUID src, LPGUID dest)
{
    FORWARD(GetDeviceID, decltype(&Proxy_GetDeviceID), src, dest);
}

HRESULT WINAPI Proxy_DirectSoundFullDuplexCreate(LPCGUID capture_dev, LPCGUID render_dev, LPCDSCBUFFERDESC capture_desc,
                                                LPCDSBUFFERDESC render_desc, HWND wnd, DWORD level,
                                                LPDIRECTSOUNDFULLDUPLEX* duplex, LPDIRECTSOUNDCAPTUREBUFFER8* capture_buf,
                                                LPDIRECTSOUNDBUFFER8* render_buf, LPUNKNOWN outer)
{
    FORWARD(DirectSoundFullDuplexCreate, decltype(&Proxy_DirectSoundFullDuplexCreate), capture_dev, render_dev, capture_desc,
            render_desc, wnd, level, duplex, capture_buf, render_buf, outer);
}

HRESULT WINAPI Proxy_DirectSoundCreate8(LPCGUID guid, LPDIRECTSOUND8* ds, LPUNKNOWN outer)
{
    FORWARD(DirectSoundCreate8, decltype(&Proxy_DirectSoundCreate8), guid, ds, outer);
}

HRESULT WINAPI Proxy_DirectSoundCaptureCreate8(LPCGUID guid, LPDIRECTSOUNDCAPTURE8* dsc, LPUNKNOWN outer)
{
    FORWARD(DirectSoundCaptureCreate8, decltype(&Proxy_DirectSoundCaptureCreate8), guid, dsc, outer);
}

}  // extern "C"
