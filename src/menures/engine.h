#pragma once

#include <cstdint>
#include <string>

// The few pieces of Unreal Engine 5.4 the plugin uses, found by byte pattern in the exe's code (engine.cpp), so game
// updates that leave them alone keep working. Everything is checked before use: each pattern matches once, functions
// at a function start, GUObjectArray in .data (CheckBuild), then the object array's layout and the FName constructor
// (CheckRuntime); any mismatch and the plugin stays out.
namespace Engine
{
    // An FName as stored in UObjectBase::NamePrivate (8 bytes in this build).
    struct Name {
        uint32_t index = 0;
        uint32_t number = 0;
        bool operator==(const Name&) const = default;
        bool valid() const { return index != 0; }
    };

    // UObjectBase (this build's layout: ObjectFlags 0x8, InternalIndex 0xC, ClassPrivate 0x10,
    // NamePrivate 0x18, OuterPrivate 0x20).
    inline uint32_t Flags(const void* o) { return *reinterpret_cast<const uint32_t*>(static_cast<const char*>(o) + 0x8); }
    inline int32_t Index(const void* o) { return *reinterpret_cast<const int32_t*>(static_cast<const char*>(o) + 0xC); }
    inline void* Class(const void* o) { return *reinterpret_cast<void* const*>(static_cast<const char*>(o) + 0x10); }
    inline Name NameOf(const void* o) { return *reinterpret_cast<const Name*>(static_cast<const char*>(o) + 0x18); }
    inline void* Outer(const void* o) { return *reinterpret_cast<void* const*>(static_cast<const char*>(o) + 0x20); }

    // EObjectFlags this plugin looks at.
    constexpr uint32_t RF_ClassDefaultObject = 0x10;
    constexpr uint32_t RF_ArchetypeObject = 0x20;
    constexpr uint32_t RF_BeginDestroyed = 0x8000;
    constexpr uint32_t RF_FinishDestroyed = 0x10000;

    // Finds the engine pieces and installs nothing; true if every pattern was found and checked. Call once from DllMain.
    bool CheckBuild(void* exe_base);
    // The checks that need the engine running (the object array, the FName constructor). Call on the game thread
    // once the engine ticks; true if they passed.
    bool CheckRuntime();

    // Address of UGameEngine::Tick, to hook.
    void* GameEngineTickAddress();
    // Address of StaticConstructObject_Internal(const FStaticConstructObjectParameters&), which every new UObject
    // passes through (on any thread, async loading included), to hook.
    void* StaticConstructObjectAddress();

    // FName for `text` if the name exists (FNAME_Find), else an invalid Name.
    Name FindName(const wchar_t* text);

    // Calls `fn(object, item_flags)` for every live object in GUObjectArray until it returns false.
    template <typename F>
    void ForEachObject(F&& fn);

    // The UClass named `name` (an object whose class is UClass's own class).
    void* FindClass(Name name);
    // The first object named `name` whose outer is named `outer_name` (outer_name invalid: any outer).
    void* FindObject(Name name, Name outer_name);
    // FindObject (or FindClass) by text, logging what it found as `what`. A Blueprint class is an object: its outer
    // is its package ("/Game/Maps/Boot"); a function's outer is its class.
    void* Lookup(const char* what, const wchar_t* name, const wchar_t* outer, bool is_class = false);

    // True if `o` still sits in GUObjectArray slot `index` (so it is not freed: compare before touching an object
    // kept across frames), is of class `cls` and is a live instance (no class default, template or destroyed one).
    bool InSlot(const void* o, int32_t index, const void* cls);

    // UObject::ProcessEvent(function, params) on the game thread.
    void ProcessEvent(void* object, void* function, void* params);

    // A short description for the log: "Name (0xADDR)".
    std::string Describe(const void* object);

    // ---- object array access, used by ForEachObject
    int32_t ObjectCount();
    void* ObjectAt(int32_t i, int32_t* item_flags);
}

template <typename F>
void Engine::ForEachObject(F&& fn)
{
    const int32_t n = ObjectCount();
    for (int32_t i = 0; i < n; ++i) {
        int32_t item_flags = 0;
        void* o = ObjectAt(i, &item_flags);
        if (o && !fn(o, item_flags))
            return;
    }
}
