#include "stdafx.h"
#include "version.h"
#include "settings.h"
#include "engine.h"
#include "menu_fix.h"

// ---------------------------------------------------------------------------
// BoRMenuRes ("Beast of Reincarnation - Menu Resolution") - the equipment menu's character preview at full
// sharpness on large screens.
//
// The preview is rendered into a 1024x1024 image and stretched to ~2000 pixels high on a 5120x2160 screen, with
// unsmoothed hair dither and blocky shadows. While the menu is open the plugin renders it at 4096x4096 on screens
// about 2160 pixels tall, 2048x2048 on smaller ones ([Preview] Size = Auto), and doubles the shadow resolution
// settings, putting them back when the menu closes (menu_fix.cpp). It also skips the start-up video and Game Freak
// logo the way the game's click-to-skip does, once the game allows it (intro_skip.cpp).
//
// Loaded by an ASI loader from the game folder at start-up: the release ships Ultimate ASI Loader as dsound.dll (or
// renamed to winmm.dll when another mod needs dsound.dll). Made for the Xbox app / PC Game Pass build (1.1.1.0 and
// 1.1.2.0; reported to work on Steam); the engine functions it needs are found by byte pattern (engine.cpp), and if
// any is missing the game is left untouched and the log says so, listing the closest places it found. Files:
// BoRMenuRes.ini (read) and BoRMenuRes.log (previous run: .prev.log) next to the plugin.
// ---------------------------------------------------------------------------

// BeastOfReincarnation-WinGDK-Shipping.exe (Xbox app / PC Game Pass) or -Win64- (Steam).
static bool IsGameExe(const std::wstring& name)
{
    constexpr std::wstring_view kPrefix = L"BeastOfReincarnation-", kSuffix = L"-Shipping.exe";
    return name.size() > kPrefix.size() + kSuffix.size() && _wcsnicmp(name.c_str(), kPrefix.data(), kPrefix.size()) == 0 &&
           _wcsicmp(name.c_str() + name.size() - kSuffix.size(), kSuffix.data()) == 0;
}

// For a build or setup the plugin hasn't seen yet: whether the exe imports dsound.dll (the shipped loader is that
// file, and must load with the exe, before engine code runs), which dsound.dll and winmm.dll (the loader's other
// name here) are loaded, and how long after the process started the plugin got loaded.
static void LogStartup(HMODULE exe)
{
    const auto base = reinterpret_cast<const uint8_t*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dirs = nt->OptionalHeader.DataDirectory;
    std::string how = "not imported";
    if (const DWORD rva = dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) {
        for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + rva); d->Name; ++d)
            if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "dsound.dll") == 0)
                how = "imported (loads with the exe)";
    }
    if (const DWORD rva = dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress) {
        for (auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + rva); d->DllNameRVA; ++d)
            if (_stricmp(reinterpret_cast<const char*>(base + d->DllNameRVA), "dsound.dll") == 0)
                how = "delay-load imported (loads on first use)";
    }
    const auto loaded = [](const wchar_t* name) {
        wchar_t path[MAX_PATH] = L"(none)";
        if (const HMODULE m = GetModuleHandleW(name))
            GetModuleFileNameW(m, path, MAX_PATH);
        return Logger::Utf8(path);
    };
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimeAsFileTime(&now);
    const auto ticks = [](const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    LOG_INFO("Start-up: dsound.dll is {} by the exe; loaded: {}; winmm.dll: {}; plugin loaded {} ms after the process "
             "started", how, loaded(L"dsound.dll"), loaded(L"winmm.dll"), (ticks(now) - ticks(created)) / 10000);
}

template <typename F>
static void Step(const char* part, F&& run)
{
    try {
        run();
    }
    catch (const std::exception& e) {
        LOG_ERROR("{}: stopped by an error ({}); the lines above show how far it got.", part, e.what());
    }
    catch (...) {
        LOG_ERROR("{}: stopped by an error; the lines above show how far it got.", part);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    // No DisableThreadLibraryCalls: the static C runtime needs the thread notifications.
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    try {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        const std::filesystem::path exePath(buf);
        if (!IsGameExe(exePath.filename().wstring())) return TRUE;

        GetModuleFileNameW(module, buf, MAX_PATH);
        const std::filesystem::path asiPath(buf);
        Logger::Init(asiPath.parent_path());

        SYSTEMTIME st;
        GetLocalTime(&st);
        LOG_INFO("----------");
        LOG_INFO("{} v{} (built {} {})", FixName, FixVersion, __DATE__, __TIME__);
        LOG_INFO("Session start: {:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
        LOG_INFO("Plugin: {} at {}", Logger::Utf8(asiPath), static_cast<void*>(module));
        LOG_INFO("Host: {} (pid {})", Logger::Utf8(exePath), GetCurrentProcessId());
        Step("Start-up", [&] { LogStartup(GetModuleHandleW(nullptr)); });
        Step("Config", [&] { Config::Init(asiPath.parent_path()); });
        LOG_INFO("----------");

        if (!Config::Enabled) {
            LOG_INFO("Enabled = false in the ini: nothing installed");
            return TRUE;
        }
        Step("Install", [&] {
            if (Engine::CheckBuild(GetModuleHandleW(nullptr)))
                MenuFix::Install();
        });
        LOG_INFO("----------");
    }
    catch (...) {
        LOG_ERROR("Start-up stopped by an error: nothing was changed.");
    }
    return TRUE;
}
