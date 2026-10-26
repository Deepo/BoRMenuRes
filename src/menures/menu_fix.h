#pragma once

// The fix itself (menu_fix.cpp): a hook on UGameEngine::Tick that, on the game thread, finds the equipment menu's
// character capture and, while it exists, renders it at [Preview] Size and raises the shadow resolution settings.
// The same hooks drive the start-up logo skip (intro_skip.h).
namespace MenuFix
{
    // Hooks UGameEngine::Tick. Call from DllMain after Engine::CheckBuild passed. True if the hook is in.
    bool Install();
}
