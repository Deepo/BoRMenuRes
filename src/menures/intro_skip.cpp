#include "stdafx.h"
#include "intro_skip.h"
#include "engine.h"
#include "settings.h"

#include <atomic>
#include <chrono>

// How the game starts: the default map /Game/Maps/Boot has a level Blueprint, Boot_C, that plays
// Content/Movies/BootLogo/BootLogo.mp4 in the game instance's movie widget (WBP_MoviePlayer_C, a native
// AibouMoviePlayerWidget), then shows the Game Freak logo widget WBP_Logo_GF_C, and loads /Game/Maps/Title once that
// logo is done (Boot_C::OnFinishedLogoAnimationEvent). After reading the save data Boot_C sets the game's skip switch
// (AibouBlueprintFunctionLibrary::CanSkipLogoMovie) - off on the very first start. The video plays muted; its sound
// is the Wwise event Play_BootLogo, which Boot_C posts when the video starts (AkGameplayStatics::PostEvent, no
// actor), so stopping the video alone leaves the sound playing. While the switch is on, a left click on the video
// calls AibouMoviePlayerWidget::StopMovie and then posts Stop_BootLogo the same way, and a click on the logo calls
// Boot_C::OnFinishedLogoAnimationEvent and WBP_Logo_GF_C::ForceClose.
// The plugin makes those same calls - not the click handler (an editor-generated event that takes the key), but the
// named functions it calls - and only while the game's switch is on. The tick hook runs after the engine's tick and
// before the frame is drawn, so it stops the video (and its sound) in the frame the video starts, and closes the
// logo in the frame StopMovie makes it: neither is ever drawn or heard. If the media player still plays kStopCheck
// after StopMovie it closes it itself. ForceClose may or may not fire OnFinishedLogoAnimationEvent itself, so
// ForceClose goes first and OnFinishedLogoAnimationEvent follows only if the boot map is still there kFinishAfter
// later - never twice. Everything is looked up by name at the first tick and checked before each call; if anything is
// missing or the boot map does not go as described, the logos play as normal.
namespace IntroSkip
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto kStopCheck = std::chrono::milliseconds(300);
        constexpr auto kFinishAfter = std::chrono::milliseconds(500);
        constexpr auto kLogoLate = std::chrono::seconds(2);  // no logo this long after StopMovie: logged once
        constexpr auto kGiveUp = std::chrono::seconds(30);   // without skipping, the boot map took 14 s
        constexpr auto kSlowWindow = std::chrono::seconds(120);
        constexpr double kSlowFrameMs = 100.0;
        constexpr int kMaxSlowFrames = 40;
        constexpr int kTitleSeconds = 12;  // per-second frame summaries once the title map loads

        enum class State { Off, WaitSkippable, WaitVideo, WaitLogo, WaitClosed, WaitTitle, Done };
        State g_state = State::Off;

        void* g_boot_class = nullptr;
        void* g_boot = nullptr;  // the boot map's Boot_C, while it lives
        int32_t g_boot_index = -1;
        void* g_movie_class = nullptr;
        void* g_movie = nullptr;  // the game instance's WBP_MoviePlayer_C
        int32_t g_movie_index = -1;
        std::atomic<void*> g_logo_class{nullptr};  // set while a logo widget is awaited
        std::atomic<void*> g_new_logo{nullptr};
        void* g_bfl = nullptr;  // Default__AibouBlueprintFunctionLibrary
        void* g_fn_can_skip = nullptr;
        void* g_fn_is_playing = nullptr;
        void* g_fn_stop = nullptr;
        void* g_fn_finished = nullptr;
        void* g_fn_close = nullptr;
        void* g_akgs = nullptr;           // Default__AkGameplayStatics
        void* g_fn_post_event = nullptr;  // AkGameplayStatics::PostEvent
        void* g_stop_sound = nullptr;     // the Wwise event Stop_BootLogo
        // The movie widget's UMediaPlayer (optional: without it, wall time and no stop check).
        void* g_media_class = nullptr;
        void* g_media = nullptr;
        int32_t g_media_index = -1;
        void* g_fn_media_time = nullptr;     // UMediaPlayer::GetTime() -> FTimespan@0
        void* g_fn_media_playing = nullptr;  // UMediaPlayer::IsPlaying() -> bool@0
        void* g_fn_media_close = nullptr;    // UMediaPlayer::Close()
        bool g_stop_checked = false;

        Clock::time_point g_start;  // the first tick
        Clock::time_point g_since;  // when the logo was closed
        Clock::time_point g_stopped;  // when the video was stopped
        bool g_late_logged = false;

        // Timing log state.
        Clock::time_point g_last_tick;
        uint64_t g_frames = 0, g_frames_at_slow = 0;
        int g_slow_frames = 0;
        Clock::time_point g_title_start;  // the boot map gone: per-second summaries from here
        int g_title_second = 0;
        int g_second_frames = 0;
        double g_second_worst_ms = 0;

        double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

        // The first live instance of `cls` (not its class default or a template) that `accept` takes.
        template <typename F>
        void* FindInstance(void* cls, int32_t* index, F&& accept)
        {
            void* found = nullptr;
            Engine::ForEachObject([&](void* o, int32_t) {
                if (Engine::Class(o) != cls || !Engine::InSlot(o, Engine::Index(o), cls) || !accept(o))
                    return true;
                found = o;
                *index = Engine::Index(o);
                return false;
            });
            return found;
        }

        // True if `ancestor` is among the first `depth` outers of `o`.
        bool Within(const void* o, const void* ancestor, int depth)
        {
            for (const void* p = Engine::Outer(o); p && depth > 0; p = Engine::Outer(p), --depth)
                if (p == ancestor)
                    return true;
            return false;
        }

        bool MediaReady() { return g_media && Engine::InSlot(g_media, g_media_index, g_media_class); }

        // AkGameplayStatics::PostEvent(AkEvent@0, Actor@8, CallbackMask@0x10, PostEventCallback@0x14 (FScriptDelegate,
        // unbound), bStopWhenAttachedToDestroyed@0x24) -> int32 playing ID@0x28 (0: failed). No actor, as the game.
        int32_t PostEvent(void* event)
        {
            struct {
                void* event;
                void* actor;
                int32_t callback_mask;
                uint8_t callback[16];
                bool stop_when_destroyed;
                uint8_t pad[3];
                int32_t result;
                uint8_t pad2[4];
            } p{event, nullptr, 0, {}, true, {}, 0, {}};
            static_assert(offsetof(decltype(p), stop_when_destroyed) == 0x24 && offsetof(decltype(p), result) == 0x28);
            Engine::ProcessEvent(g_akgs, g_fn_post_event, &p);
            return p.result;
        }

        // Seconds of video the media player has played.
        double MediaTime()
        {
            struct {
                int64_t ticks;  // FTimespan: 100 ns
                uint8_t pad[8];
            } p{};
            Engine::ProcessEvent(g_media, g_fn_media_time, &p);
            return static_cast<double>(p.ticks) / 1e7;
        }

        bool MediaPlaying()
        {
            struct {
                bool result;
                uint8_t pad[15];
            } p{};
            Engine::ProcessEvent(g_media, g_fn_media_playing, &p);
            return p.result;
        }

        // A function without parameters (a zeroed buffer anyway, in case one has an output).
        void Call(void* object, void* function)
        {
            alignas(16) uint8_t params[256]{};
            Engine::ProcessEvent(object, function, params);
        }

        // AibouBlueprintFunctionLibrary::CanSkipLogoMovie(WorldContextObject@0) -> bool@8, a static function.
        bool CanSkip()
        {
            struct {
                void* context;
                bool result;
                uint8_t pad[7];
            } p{g_boot, false, {}};
            Engine::ProcessEvent(g_bfl, g_fn_can_skip, &p);
            return p.result;
        }

        // AibouMoviePlayerWidget::IsPlayingMovie() -> bool@0.
        bool IsPlaying()
        {
            struct {
                bool result;
                uint8_t pad[15];
            } p{};
            Engine::ProcessEvent(g_movie, g_fn_is_playing, &p);
            return p.result;
        }

        // Closes the Game Freak logo widget if StopMovie has made it; true if it did.
        bool TryCloseLogo(Clock::time_point now)
        {
            void* logo = g_new_logo.load(std::memory_order_acquire);
            void* logo_class = g_logo_class.load(std::memory_order_relaxed);
            if (!logo || !Engine::InSlot(logo, Engine::Index(logo), logo_class))
                return false;
            Call(logo, g_fn_close);
            LOG_INFO("Intro: Game Freak logo {} closed as it appeared (WBP_Logo_GF_C::ForceClose, {:.2f} s)", Engine::Describe(logo),
                     Seconds(now - g_start));
            g_logo_class.store(nullptr, std::memory_order_relaxed);
            g_since = now;
            return true;
        }

        void Finish()
        {
            g_state = State::Done;
            g_logo_class.store(nullptr, std::memory_order_relaxed);
        }

        // Frames of 100 ms or more in the first two minutes, and once the title map loads a summary per second for
        // kTitleSeconds: smooth frames while the title menu is still missing mean the game waits on purpose (or on
        // another thread), slow ones that it is loading.
        void LogTiming(Clock::time_point now)
        {
            const double ms = Seconds(now - g_last_tick) * 1000.0;
            g_last_tick = now;
            ++g_frames;
            if (ms >= kSlowFrameMs && now - g_start < kSlowWindow && g_slow_frames < kMaxSlowFrames) {
                ++g_slow_frames;
                LOG_INFO("Timing: a {:.0f} ms frame, {} frames after the last slow one ({:.2f} s after the first tick)", ms,
                         g_frames - g_frames_at_slow, Seconds(now - g_start));
                g_frames_at_slow = g_frames;
            }
            if (g_title_start == Clock::time_point{} || g_title_second >= kTitleSeconds)
                return;
            ++g_second_frames;
            g_second_worst_ms = std::max(g_second_worst_ms, ms);
            if (now - g_title_start >= std::chrono::seconds(g_title_second + 1)) {
                LOG_INFO("Timing: second {} after the boot map: {} frames, slowest {:.0f} ms", g_title_second + 1, g_second_frames,
                         g_second_worst_ms);
                ++g_title_second;
                g_second_frames = 0;
                g_second_worst_ms = 0;
            }
        }
    }

    void Setup()
    {
        g_start = g_last_tick = Clock::now();
        if (!Config::SkipLogos) {
            LOG_INFO("Intro: SkipLogos = false - the logos play as normal");
            return;
        }
        g_boot_class = Engine::Lookup("class Boot_C (boot map)", L"Boot_C", L"/Game/Maps/Boot");
        g_movie_class = Engine::Lookup("class WBP_MoviePlayer_C", L"WBP_MoviePlayer_C", L"/Game/UI/Widget/VideoPlayer/WBP_MoviePlayer");
        void* logo_class = Engine::Lookup("class WBP_Logo_GF_C", L"WBP_Logo_GF_C", L"/Game/UI/Widget/Logo/WBP_Logo_GF");
        g_bfl = Engine::Lookup("Default__AibouBlueprintFunctionLibrary", L"Default__AibouBlueprintFunctionLibrary", nullptr);
        g_fn_can_skip = Engine::Lookup("CanSkipLogoMovie", L"CanSkipLogoMovie", L"AibouBlueprintFunctionLibrary");
        g_fn_is_playing = Engine::Lookup("IsPlayingMovie", L"IsPlayingMovie", L"AibouMoviePlayerWidget");
        g_fn_stop = Engine::Lookup("StopMovie", L"StopMovie", L"AibouMoviePlayerWidget");
        g_fn_finished = Engine::Lookup("OnFinishedLogoAnimationEvent", L"OnFinishedLogoAnimationEvent", L"Boot_C");
        g_fn_close = Engine::Lookup("ForceClose", L"ForceClose", L"WBP_Logo_GF_C");
        g_akgs = Engine::Lookup("Default__AkGameplayStatics", L"Default__AkGameplayStatics", nullptr);
        g_fn_post_event = Engine::Lookup("AkGameplayStatics::PostEvent", L"PostEvent", L"AkGameplayStatics");
        g_stop_sound = Engine::Lookup("Wwise event Stop_BootLogo", L"Stop_BootLogo", L"/Game/WwiseAudio/Events/Common/Stop_BootLogo");
        if (!g_boot_class || !g_movie_class || !logo_class || !g_bfl || !g_fn_can_skip || !g_fn_is_playing || !g_fn_stop ||
            !g_fn_finished || !g_fn_close || !g_akgs || !g_fn_post_event || !g_stop_sound) {
            LOG_WARN("Intro: not all of the boot map's parts found - the logos play as normal");
            return;
        }
        g_boot = FindInstance(g_boot_class, &g_boot_index, [](void*) { return true; });
        const Engine::Name gi_class = Engine::FindName(L"BP_GameInstance_C");
        g_movie = FindInstance(g_movie_class, &g_movie_index, [&](void* o) {
            const void* outer = Engine::Outer(o);
            return gi_class.valid() && outer && Engine::NameOf(Engine::Class(outer)) == gi_class;
        });
        if (!g_boot || !g_movie) {
            LOG_WARN("Intro: no {} - the logos play as normal", !g_boot ? "boot map (Boot_C)" : "movie widget of the game instance");
            return;
        }
        LOG_INFO("Intro: boot map {}, movie widget {}", Engine::Describe(g_boot), Engine::Describe(g_movie));
        // The widget's media player: WBP_MoviePlayer_C.WidgetTree.VideoPlayer.MediaPlayer.
        g_media_class = Engine::Lookup("class MediaPlayer", L"MediaPlayer", nullptr, true);
        g_fn_media_time = Engine::Lookup("MediaPlayer::GetTime", L"GetTime", L"MediaPlayer");
        g_fn_media_playing = Engine::Lookup("MediaPlayer::IsPlaying", L"IsPlaying", L"MediaPlayer");
        g_fn_media_close = Engine::Lookup("MediaPlayer::Close", L"Close", L"MediaPlayer");
        if (g_media_class && g_fn_media_time && g_fn_media_playing && g_fn_media_close)
            g_media = FindInstance(g_media_class, &g_media_index, [](void* o) { return Within(o, g_movie, 4); });
        if (g_media)
            LOG_INFO("Intro: media player {}", Engine::Describe(g_media));
        else
            LOG_WARN("Intro: no media player in the movie widget - it is not checked after the stop");
        g_logo_class.store(logo_class, std::memory_order_release);
        g_state = State::WaitSkippable;
    }

    void OnConstruct(void* object)
    {
        void* watch = g_logo_class.load(std::memory_order_relaxed);
        if (watch && Engine::Class(object) == watch)
            g_new_logo.store(object, std::memory_order_release);
    }

    void OnTick()
    {
        const auto now = Clock::now();
        LogTiming(now);
        if (g_state == State::Off || g_state == State::Done)
            return;
        if (!Engine::InSlot(g_boot, g_boot_index, g_boot_class)) {
            LOG_INFO("Intro: boot map gone {:.2f} s after the first tick{}", Seconds(now - g_start),
                     g_state == State::WaitTitle   ? ""
                     : g_state == State::WaitClosed ? " (ForceClose was enough)"
                                                    : " - the logos were not skipped");
            g_title_start = now;
            Finish();
            return;
        }
        // Once, kStopCheck after StopMovie: the media player must have stopped too (its sound would play on).
        if (!g_stop_checked && g_stopped != Clock::time_point{} && now - g_stopped >= kStopCheck) {
            g_stop_checked = true;
            if (!MediaReady()) {
                LOG_INFO("Intro: media player not checked after the stop (none found)");
            }
            else if (MediaPlaying()) {
                Call(g_media, g_fn_media_close);
                LOG_WARN("Intro: the media player still played {:.1f} s after StopMovie - closed it (now playing: {})",
                         Seconds(now - g_stopped), MediaPlaying());
            }
            else {
                LOG_INFO("Intro: media player stopped with the video");
            }
        }
        if (now - g_start > kGiveUp) {
            LOG_WARN("Intro: the boot map did not go as expected within {} s - the logos play as normal",
                     std::chrono::duration_cast<std::chrono::seconds>(kGiveUp).count());
            Finish();
            return;
        }
        switch (g_state) {
        case State::WaitSkippable:
            if (CanSkip()) {
                LOG_INFO("Intro: the game allows skipping the logos ({:.2f} s)", Seconds(now - g_start));
                g_state = State::WaitVideo;
            }
            break;
        case State::WaitVideo:
            if (!Engine::InSlot(g_movie, g_movie_index, g_movie_class)) {
                LOG_WARN("Intro: the movie widget is gone - the logos play as normal");
                Finish();
                break;
            }
            if (!IsPlaying())
                break;
            if (Config::LogOnly) {
                LOG_INFO("Intro: video playing ({:.2f} s); LogOnly - not stopping it", Seconds(now - g_start));
                Finish();
                break;
            }
            // The frame the video starts, before it is drawn; Play_BootLogo was posted earlier in this same frame.
            Call(g_movie, g_fn_stop);
            LOG_INFO("Intro: video stopped in the frame it started (AibouMoviePlayerWidget::StopMovie, {:.2f} s{})",
                     Seconds(now - g_start), MediaReady() ? std::format(", media player at {:.2f} s", MediaTime()) : "");
            {
                const int32_t id = PostEvent(g_stop_sound);
                LOG_INFO("Intro: posted Stop_BootLogo (playing ID {}{})", id, id ? "" : " - Wwise refused it");
            }
            g_stopped = now;
            // StopMovie makes the Game Freak logo at once: close it in this frame too, else next frame.
            g_state = TryCloseLogo(now) ? State::WaitClosed : State::WaitLogo;
            break;
        case State::WaitLogo:
            if (TryCloseLogo(now))
                g_state = State::WaitClosed;
            else if (!g_late_logged && now - g_stopped > kLogoLate) {
                g_late_logged = true;
                LOG_WARN("Intro: no Game Freak logo {:.1f} s after the video was stopped", Seconds(now - g_stopped));
            }
            break;
        case State::WaitClosed:
            // Still on the boot map (it is gone once the title map loads; see the top of OnTick).
            if (now - g_since < kFinishAfter)
                break;
            Call(g_boot, g_fn_finished);
            LOG_INFO("Intro: boot map still there {:.1f} s after the close - called Boot_C::OnFinishedLogoAnimationEvent",
                     Seconds(now - g_since));
            g_state = State::WaitTitle;
            break;
        default:
            break;
        }
    }
}
