#include "stdafx.h"
#include "menu_fix.h"
#include "engine.h"
#include "intro_skip.h"
#include "settings.h"

#include <atomic>
#include <chrono>

#include <safetyhook.hpp>

// How it works:
//  - The equipment menu runs in a small world of its own; its character preview is a MultiWorldSceneCaptureComponent2D
//    that renders every frame into a 1024x1024 RGBA16F UTextureRenderTarget2D (TextureTarget, offset 0x360), which
//    the menu's UI draws stretched across ~2000 screen pixels. The component exists only while the menu is open.
//  - Resizing that render target with UKismetRenderingLibrary::ResizeRenderTarget2D makes the engine render the
//    whole capture at the new size; the UI keeps its layout. The game makes a new target when the menu reopens, so
//    the size is checked every few frames.
//  - The capture's shadows come from the game's shadow settings, which it keeps low (r.Shadow.MaxResolution 1024,
//    TexelsPerPixel 0.64): while the capture exists they are raised through UKismetSystemLibrary::
//    ExecuteConsoleCommand, and put back when it is gone (gameplay uses the same classic shadow maps).
// Everything runs in the UGameEngine::Tick hook, after the engine's own tick, on the game thread. A second hook on
// StaticConstructObject_Internal only notes a newly made capture component (any thread), so the tick picks it up
// the next frame instead of waiting for the object scan, whose full pass takes about a second (the preview would show
// at 1024 for that long). Both hooks also drive the start-up logo skip (intro_skip.cpp).
namespace MenuFix
{
    namespace
    {
        safetyhook::InlineHook g_tick;
        safetyhook::InlineHook g_construct;
        std::atomic<void*> g_watch_class{nullptr};  // the capture class, once Setup found it
        std::atomic<void*> g_new_capture{nullptr};  // the last capture component made, for the tick to take
        uint64_t g_frame = 0;
        bool g_started = false;  // the first tick ran the setup
        bool g_ready = false;    // the menu fix's lookups passed
        bool g_failed = false;   // CheckRuntime failed, or an error: the plugin stays out for the rest of the session

        // Objects the plugin calls, looked up once.
        void* g_capture_class = nullptr;
        void* g_krl = nullptr;            // Default__KismetRenderingLibrary
        void* g_ksl = nullptr;            // Default__KismetSystemLibrary
        void* g_fn_resize = nullptr;      // KismetRenderingLibrary::ResizeRenderTarget2D
        void* g_fn_exec = nullptr;        // KismetSystemLibrary::ExecuteConsoleCommand
        void* g_fn_get_float = nullptr;   // KismetSystemLibrary::GetConsoleVariableFloatValue

        // The capture component while the menu is open, and its slot in GUObjectArray (checked before the object is
        // touched: after the menu closes its memory is freed).
        void* g_capture = nullptr;
        int32_t g_capture_index = -1;
        void* g_last_target = nullptr;
        bool g_shadows_raised = false;

        // Offsets from the engine's reflection data for this build.
        constexpr size_t kTextureTarget = 0x360;  // USceneCaptureComponent2D::TextureTarget
        constexpr size_t kSizeX = 0x138, kSizeY = 0x13C, kFormat = 0x153;  // UTextureRenderTarget2D

        // An FString pointing at our own buffer: the native thunks copy it, nothing frees it.
        struct FString {
            const wchar_t* data;
            int32_t num;
            int32_t max;
        };
        FString Str(const std::wstring& s)
        {
            const auto n = static_cast<int32_t>(s.size() + 1);
            return {s.c_str(), n, n};
        }

        // Shadow settings the plugin changes, with the factor each gets.
        struct ShadowCvar {
            const wchar_t* name;
            bool is_max_resolution;
            float game_value = 0;
        };
        ShadowCvar g_cvars[] = {
            {L"r.Shadow.MaxResolution", true},
            {L"r.Shadow.TexelsPerPixel", false},
            {L"r.Shadow.TexelsPerPixelSpotlight", false},
            {L"r.Shadow.TexelsPerPixelPointlight", false},
            {L"r.Shadow.TexelsPerPixelRectlight", false},
        };
        bool g_cvars_read = false;

        float GetCvarFloat(const wchar_t* name)
        {
            const std::wstring text(name);
            struct {
                FString name;
                float result;
                uint32_t pad;
            } p{Str(text), -1.0f, 0};
            Engine::ProcessEvent(g_ksl, g_fn_get_float, &p);
            return p.result;
        }

        void ExecCommand(void* world_context, const std::wstring& command)
        {
            struct {
                void* context;
                FString command;
                void* player;
            } p{world_context, Str(command), nullptr};
            Engine::ProcessEvent(g_ksl, g_fn_exec, &p);
        }

        std::string Narrow(const wchar_t* w)
        {
            std::string s;
            for (; *w; ++w)
                s += static_cast<char>(*w);
            return s;
        }

        // The menu fix's lookups (Engine::CheckRuntime has passed).
        bool SetupMenu()
        {
            using Engine::Lookup;
            g_capture_class = Lookup("class MultiWorldSceneCaptureComponent2D", L"MultiWorldSceneCaptureComponent2D", nullptr, true);
            g_krl = Lookup("Default__KismetRenderingLibrary", L"Default__KismetRenderingLibrary", nullptr);
            g_ksl = Lookup("Default__KismetSystemLibrary", L"Default__KismetSystemLibrary", nullptr);
            g_fn_resize = Lookup("ResizeRenderTarget2D", L"ResizeRenderTarget2D", L"KismetRenderingLibrary");
            g_fn_exec = Lookup("ExecuteConsoleCommand", L"ExecuteConsoleCommand", L"KismetSystemLibrary");
            g_fn_get_float = Lookup("GetConsoleVariableFloatValue", L"GetConsoleVariableFloatValue", L"KismetSystemLibrary");
            const bool ok = g_capture_class && g_krl && g_ksl && g_fn_resize && g_fn_exec && g_fn_get_float;
            if (ok)
                g_watch_class.store(g_capture_class);
            return ok;
        }

        bool Live(void* o)
        {
            constexpr uint32_t kDead = Engine::RF_ClassDefaultObject | Engine::RF_ArchetypeObject | Engine::RF_BeginDestroyed |
                                       Engine::RF_FinishDestroyed;
            return o && (Engine::Flags(o) & kDead) == 0 && Engine::Class(o) == g_capture_class;
        }

        // The cached capture if its slot still holds it, else the next kScanPerFrame slots of GUObjectArray. A whole
        // pass at once takes several ms with ~470k objects, a hitch; spread over frames it is ~0.15 ms a frame, a full
        // pass every ~60 frames. The menu has exactly one capture, so the first match is the one.
        constexpr int32_t kScanPerFrame = 8192;
        int32_t g_scan_pos = 0;
        int g_passes_logged = 0;
        double g_pass_worst_ms = 0;
        int g_pass_frames = 0;

        void* FindCapture()
        {
            if (g_capture && g_capture_index >= 0 && g_capture_index < Engine::ObjectCount() &&
                Engine::ObjectAt(g_capture_index, nullptr) == g_capture && Live(g_capture))
                return g_capture;
            // A capture the construction hook just saw (made since the last tick, so not yet collected: the garbage
            // collector runs on this thread, inside the engine's tick).
            if (void* fresh = g_new_capture.exchange(nullptr)) {
                const int32_t i = Engine::Index(fresh);
                if (i >= 0 && i < Engine::ObjectCount() && Engine::ObjectAt(i, nullptr) == fresh && Live(fresh)) {
                    LOG_INFO("Scan: capture made at index {} (construction hook)", i);
                    g_capture_index = i;
                    g_scan_pos = 0;
                    return fresh;
                }
            }
            LARGE_INTEGER t0, t1, freq;
            QueryPerformanceCounter(&t0);
            const int32_t n = Engine::ObjectCount();
            const int32_t end = std::min(g_scan_pos + kScanPerFrame, n);
            void* found = nullptr;
            int32_t i = g_scan_pos;
            for (; i < end; ++i) {
                void* o = Engine::ObjectAt(i, nullptr);
                if (Live(o)) {
                    found = o;
                    break;
                }
            }
            g_scan_pos = (found || i >= n) ? 0 : i;
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&freq);
            g_pass_worst_ms = std::max(g_pass_worst_ms, double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart));
            ++g_pass_frames;
            if (g_scan_pos == 0) {  // a pass ended (or found the capture): its cost, for the first few passes
                if (g_passes_logged < 3 || found) {
                    LOG_INFO("Scan: pass over {} objects in {} frames, at most {:.2f} ms a frame{}", n, g_pass_frames,
                             g_pass_worst_ms, found ? std::format(", capture at index {}", i) : "");
                    ++g_passes_logged;
                }
                g_pass_worst_ms = 0;
                g_pass_frames = 0;
            }
            g_capture_index = found ? i : -1;
            return found;
        }

        void RaiseShadows(void* context)
        {
            if (!g_cvars_read) {
                for (ShadowCvar& c : g_cvars) {
                    c.game_value = GetCvarFloat(c.name);
                    LOG_INFO("Shadows: game's {} = {}", Narrow(c.name), c.game_value);
                }
                g_cvars_read = true;
            }
            if (Config::LogOnly || !Config::Sharper)
                return;
            for (const ShadowCvar& c : g_cvars) {
                // MaxResolution: 2 x Factor (the tested "2x" preset: 1024 -> 4096), at most 8192; the rest x Factor.
                const float v = c.is_max_resolution ? std::min(c.game_value * 2.0f * static_cast<float>(Config::Factor), 8192.0f)
                                                    : c.game_value * static_cast<float>(Config::Factor);
                ExecCommand(context, std::format(L"{} {}", c.name, v));
                LOG_INFO("Shadows: {} -> {} (now {})", Narrow(c.name), v, GetCvarFloat(c.name));
            }
            g_shadows_raised = true;
        }

        void RestoreShadows(void* context)
        {
            if (!g_shadows_raised)
                return;
            for (const ShadowCvar& c : g_cvars) {
                ExecCommand(context, std::format(L"{} {}", c.name, c.game_value));
                LOG_INFO("Shadows: {} back to {} (now {})", Narrow(c.name), c.game_value, GetCvarFloat(c.name));
            }
            g_shadows_raised = false;
        }

        // The client height of the game's window (the tallest visible UnrealWindow of this process), else the primary
        // screen's height.
        int GameWindowHeight()
        {
            struct Search {
                DWORD pid;
                int height;
            } s{GetCurrentProcessId(), 0};
            EnumWindows(
                [](HWND wnd, LPARAM param) -> BOOL {
                    auto& s = *reinterpret_cast<Search*>(param);
                    DWORD pid = 0;
                    GetWindowThreadProcessId(wnd, &pid);
                    wchar_t cls[32] = {};
                    RECT rc{};
                    if (pid == s.pid && IsWindowVisible(wnd) && GetClassNameW(wnd, cls, 32) && wcscmp(cls, L"UnrealWindow") == 0 &&
                        GetClientRect(wnd, &rc))
                        s.height = std::max(s.height, static_cast<int>(rc.bottom - rc.top));
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&s));
            return s.height > 0 ? s.height : GetSystemMetrics(SM_CYSCREEN);
        }

        // [Preview] Size, with Auto resolved. The preview is drawn about 0.8-0.9 of the screen's height and shrunk by
        // the UI: 4096 on a 2160-line screen is a 2x downscale, which also averages the hair's dither. On 1080-1600
        // lines 2048 is already 1.5-2x; 4096 still smooths the hair further there, but costs 4x the pixels and more
        // graphics memory, a lot for the GPUs usually paired with those screens (the ini tells players they can raise
        // it). So 4096 from 1800 lines up, else 2048.
        int PreviewSize()
        {
            if (Config::Size != 0)
                return Config::Size;
            const int height = GameWindowHeight();
            const int size = height >= 1800 ? 4096 : 2048;
            LOG_INFO("Preview: Size = Auto: the game's window is {} pixels tall, so {}", height, size);
            return size;
        }

        void CheckTarget(void* capture)
        {
            auto* target = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(capture) + kTextureTarget);
            if (!target)
                return;
            const int32_t sx = *reinterpret_cast<int32_t*>(target + kSizeX), sy = *reinterpret_cast<int32_t*>(target + kSizeY);
            if (target != g_last_target) {
                LOG_INFO("Preview: target {} {}x{} format {}", Engine::Describe(target), sx, sy, target[kFormat]);
                g_last_target = target;
            }
            // Only the game's 1024x1024 target, so a size set elsewhere (another mod, a future patch) is left alone.
            if (Config::LogOnly || !Config::Resize || sx != 1024 || sy != 1024)
                return;
            const int size = PreviewSize();
            if (size == 1024)
                return;
            struct {
                void* target;
                int32_t width;
                int32_t height;
            } p{target, size, size};
            Engine::ProcessEvent(g_krl, g_fn_resize, &p);
            LOG_INFO("Preview: resized to {}x{} (now {}x{})", size, size, *reinterpret_cast<int32_t*>(target + kSizeX),
                     *reinterpret_cast<int32_t*>(target + kSizeY));
        }

        void OnTick()
        {
            ++g_frame;
            if (g_failed)
                return;
            if (!g_started) {
                // The first tick: the engine is up and the default map (the boot map, which the logo skip needs) is
                // loaded. The checks need only the engine's own objects.
                g_started = true;
                const auto t0 = std::chrono::steady_clock::now();
                if (!Engine::CheckRuntime()) {
                    g_failed = true;
                    LOG_INFO("Setup FAILED at frame {}; the plugin stays out", g_frame);
                    return;
                }
                g_ready = SetupMenu();
                IntroSkip::Setup();
                LOG_INFO("Setup done at frame {} in {:.1f} ms{}{}", g_frame,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                         g_ready ? "" : "; the menu fix stays out", Config::LogOnly ? "; LogOnly: watching, changing nothing" : "");
                return;
            }
            IntroSkip::OnTick();
            if (!g_ready)
                return;
            void* capture = FindCapture();
            if (capture != g_capture) {
                if (capture) {
                    LOG_INFO("Menu: capture component appeared: {}", Engine::Describe(capture));
                    RaiseShadows(capture);
                } else {
                    LOG_INFO("Menu: capture component gone");
                    // Any live object serves as the world context for the console command; the old capture may be
                    // gone, so use the system library's default object, which always exists.
                    RestoreShadows(g_ksl);
                    g_last_target = nullptr;
                }
                g_capture = capture;
            }
            // Every frame: a few reads, and the game makes new targets while the menu is open (2 s after opening).
            if (g_capture)
                CheckTarget(g_capture);
        }

        // StaticConstructObject_Internal(const FStaticConstructObjectParameters&): any thread, very often - one compare.
        // unsafe_call, not call: call() takes the hook's mutex, and a thread holding it inside the engine can wait on
        // an async loading thread that is itself waiting for the mutex, which hangs the load screen. The hooks are
        // never removed while the game runs, which is all the mutex guards against.
        void* __fastcall ConstructDetour(const void* params)
        {
            void* o = g_construct.unsafe_call<void*>(params);
            if (!o)
                return o;
            void* watch = g_watch_class.load(std::memory_order_relaxed);
            if (watch && Engine::Class(o) == watch)
                g_new_capture.store(o, std::memory_order_release);
            IntroSkip::OnConstruct(o);
            return o;
        }

        void __fastcall TickDetour(void* self, float delta, bool idle)
        {
            g_tick.unsafe_call<void>(self, delta, idle);  // (see ConstructDetour)
            try {
                OnTick();
            }
            catch (const std::exception& e) {
                LOG_ERROR("Tick: stopped by an error ({}); the plugin stays out", e.what());
                g_failed = true;
            }
        }
    }

    bool Install()
    {
        auto hook = safetyhook::InlineHook::create(Engine::GameEngineTickAddress(), reinterpret_cast<void*>(&TickDetour));
        if (!hook) {
            LOG_ERROR("Hook: UGameEngine::Tick could not be hooked (safetyhook error {})", static_cast<int>(hook.error().type));
            return false;
        }
        g_tick = std::move(*hook);
        LOG_INFO("Hook: UGameEngine::Tick hooked");
        // Without this one the fix still works, just later (the object scan finds the capture).
        auto construct = safetyhook::InlineHook::create(Engine::StaticConstructObjectAddress(), reinterpret_cast<void*>(&ConstructDetour));
        if (construct) {
            g_construct = std::move(*construct);
            LOG_INFO("Hook: StaticConstructObject_Internal hooked");
        } else {
            LOG_WARN("Hook: StaticConstructObject_Internal could not be hooked (safetyhook error {}); the scan alone finds the capture",
                     static_cast<int>(construct.error().type));
        }
        return true;
    }
}
