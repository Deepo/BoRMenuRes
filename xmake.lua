set_project("BoRMenuRes")
add_rules("mode.debug", "mode.release")
set_languages("cxxlatest", "clatest")

-- ===================== Zycore / Zydis (for safetyhook) =====================
-- git submodules in external/ (clone with --recursive).

target("zycore")
    set_kind("static")
    add_defines("ZYCORE_STATIC_BUILD", {public = true})
    add_includedirs("external/zydis/dependencies/zycore/include", {public = true})
    add_files("external/zydis/dependencies/zycore/src/*.c")
    set_runtimes(is_mode("release") and "MT" or "MTd")

target("zydis")
    add_deps("zycore")
    set_kind("static")
    add_defines("ZYDIS_STATIC_BUILD", {public = true})
    add_includedirs("external/zydis/include", "external/zydis/src", {public = true})
    add_files("external/zydis/src/*.c")
    set_runtimes(is_mode("release") and "MT" or "MTd")

-- ===================== BoRMenuRes.asi: the fix =====================
-- src/menures: a hook on UGameEngine::Tick that renders the equipment menu's character preview at [Preview] Size
-- (4096) and raises the shadow resolution settings while the menu is open (menu_fix.cpp), and skips the start-up logos
-- (intro_skip.cpp). Engine functions found by byte pattern at start-up (engine.cpp). Settings: config/BoRMenuRes.ini;
-- log BoRMenuRes.log next to it.

target("BoRMenuRes")
    set_kind("shared")
    set_prefixname("")
    set_extension(".asi")
    add_includedirs("src/menures", "src", "external/safetyhook/include", "external/mINI/src/mini")
    add_headerfiles("src/menures/*.h", "src/*.h")
    add_files("src/menures/*.cpp", "src/menures/version.rc", "src/config.cpp", "src/logger.cpp",
              "external/safetyhook/src/**.cpp")
    add_deps("zydis")
    add_syslinks("user32")  -- the game window's height for [Preview] Size = Auto
    -- xmake does not see version.rc's #include of version.h, so a version bump alone left the file's version stale:
    -- drop the compiled resource whenever version.h is newer.
    before_build(function (target)
        local obj = target:objectfile("src/menures/version.rc")
        if os.isfile(obj) and os.mtime("src/menures/version.h") > os.mtime(obj) then
            print("version.h changed: recompiling version.rc")
            os.rm(obj)
        end
    end)
    if is_plat("windows") then
        set_toolchains("msvc")
        add_cxflags("/utf-8", "/W4")
        if is_mode("release") then
            set_optimize("fastest")
            set_strip("all")
            set_runtimes("MT")
            add_ldflags("/OPT:REF", "/OPT:ICF")
        else
            set_strip("none")
            set_runtimes("MTd")
            add_cxflags("/Zi")
            add_defines("_DEBUG")
        end
    end

-- ===================== the loader: dsound.dll =====================
-- Our own ASI loader (src/asiloader): a dsound.dll stand-in next to the exe that loads every *.asi there at start-up
-- and forwards DirectSound's 12 exports to System32\dsound.dll. Log: BoRAsiLoader.log next to it.

target("BoRAsiLoader")
    set_kind("shared")
    set_basename("dsound")
    add_files("src/asiloader/dsound.cpp", "src/asiloader/dsound.def", "src/asiloader/version.rc")
    add_syslinks("kernel32")
    -- the same version.h as the plugin, and the same stale-resource problem as there
    before_build(function (target)
        local obj = target:objectfile("src/asiloader/version.rc")
        if os.isfile(obj) and os.mtime("src/menures/version.h") > os.mtime(obj) then
            print("version.h changed: recompiling the loader's version.rc")
            os.rm(obj)
        end
    end)
    if is_plat("windows") then
        set_toolchains("msvc")
        add_cxflags("/utf-8", "/W4")
        set_runtimes(is_mode("release") and "MT" or "MTd")
        if is_mode("release") then
            set_optimize("fastest")
            set_strip("all")
        end
    end
