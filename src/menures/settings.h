#pragma once

#include "config.h"

// BoRMenuRes's switches, read from BoRMenuRes.ini (settings.cpp).
namespace Config
{
    // [General] Enabled: false = the plugin does nothing at all (no hook either), for example to rule it out when
    // something goes wrong, or next to another mod that changes the same things.
    inline bool Enabled = true;

    // [General] LogOnly: observe and log, change nothing. Not in the shipped ini.
    inline bool LogOnly = false;

    // [Preview] Resize: render the equipment menu's character preview (a 1024x1024 scene capture) at Size x Size.
    inline bool Resize = true;
    // [Preview] Size: Auto (0 here) = from the game window's height when the menu opens (menu_fix.cpp: PreviewSize),
    // or a number, 1024-8192. 8192 halves the menu's frame rate even on a fast GPU.
    inline int Size = 0;

    // [Shadows] Sharper: while the menu's capture exists, multiply the shadow resolution settings
    // (r.Shadow.TexelsPerPixel*, r.Shadow.MaxResolution) by Factor; the game's values come back when it is gone.
    inline bool Sharper = true;
    // [Shadows] Factor: 2 (the 4x settings cost 6-12 fps in the menu). 1-4.
    inline int Factor = 2;

    // [Intro] SkipLogos: skip the boot video and the Game Freak logo as the game's own click-to-skip does, once the
    // game allows skipping (not on the very first start) - intro_skip.cpp.
    inline bool SkipLogos = true;
}
