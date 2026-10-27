# BoRMenuRes

Source code of Beast of Reincarnation - Sharp Menu Model and Logo Skip, for the Xbox app / PC Game Pass version (also
reported to work on Steam): renders the equipment menu's character at up to 4096 x 4096 instead of a stretched 1024 x
1024, and skips the start-up video and Game Freak logo. Download, installation and support:
https://www.nexusmods.com/beastofreincarnation/mods/160

Building needs Visual Studio 2026 (MSVC) and xmake:

    git clone --recursive https://github.com/Deepo/BoRMenuRes.git
    cd BoRMenuRes
    xmake f -p windows -a x64 -m release -y
    xmake build -y BoRMenuRes

This gives build\windows\x64\release\BoRMenuRes.asi (src/menures): `menu_fix.cpp` enlarges the menu's scene capture,
`intro_skip.cpp` skips the logos, `engine.cpp` finds the engine functions by byte pattern. Settings:
config/BoRMenuRes.ini. The release ships it with [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
(the official x64 dinput8.dll, renamed winmm.dll) as its loader; 1.0.0 had a loader of its own, in the git history.

MIT License. Uses SafetyHook (BSL-1.0), Zydis and Zycore (MIT) and mINI (MIT), as git submodules in external/. The
version information and log layout follow Lyall's game fixes (MIT); the loader is Ultimate ASI Loader (MIT): see
THIRD-PARTY-NOTICES.txt.
