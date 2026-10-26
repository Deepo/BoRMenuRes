#include "stdafx.h"
#include "engine.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <vector>

namespace Engine
{
    namespace
    {
        // The engine pieces are found by byte pattern in the exe's code, so a game update that leaves them alone keeps
        // working. Patterns made from the 1.1.1.0 exe (Xbox app / GDK, UE 5.4.4 "++aibou+mainline") and checked against
        // 1.1.2.0. ?? marks bytes that move between builds (RIP-relative displacements, branch and call targets). Each
        // pattern must match exactly once in .text, and each function pattern at the start of a function.
        struct Pattern {
            const char* name;
            const char* bytes;
            int disp = 0;   // global: where the instruction's 32-bit displacement is, from the match
            int end = 0;    // global: where that instruction ends (the displacement is relative to it)
            int field = 0;  // global: the offset into the global that the instruction addresses
        };
        constexpr Pattern kProcessEvent{
            "UObject::ProcessEvent",
            "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 10 01 00 00 48 8D 6C 24 30 48 89 9D 38 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 D0 00 00 00"};
        constexpr Pattern kFNameCtor{
            "FName::FName(const wchar_t*, EFindName)",
            "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 41 8B F8 33 C9 4C 8B DA 44 8B D1 4C 8B CA 48 85 D2 74 ?? 0F B7 02 66 85 C0 74 ?? 0F 1F 84 00 00 00 00 00"};
        constexpr Pattern kGameEngineTick{
            "UGameEngine::Tick",
            "40 55 53 57 41 54 41 57 48 8D 6C 24 D0 48 81 EC 30 01 00 00 0F 29 B4 24 10 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 00 0F 28 F1 44 88 44 24 30"};
        constexpr Pattern kStaticConstructObject{
            "StaticConstructObject_Internal",
            "4C 8B DC 49 89 5B 18 49 89 73 20 55 57 41 54 41 56 41 57 49 8D AB 58 FE FF FF 48 81 EC 80 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 70 01 00 00"};
        // GUObjectArray from two places that read it, in different functions: a compare with ObjObjects.NumElements
        // (+0x24) and a load of ObjObjects.Objects (+0x10). Either is enough; if both match they must agree.
        constexpr Pattern kGUObjectArray[] = {
            {"GUObjectArray (NumElements compare)",
             "E8 ?? ?? ?? ?? 48 8B D8 4C 8B 40 40 8B 40 0C 3B 05 ?? ?? ?? ?? 7D ?? 8B C8 0F B7 C0 48 C1 E9 10", 17, 21, 0x24},
            {"GUObjectArray (Objects load)",
             "48 8B 05 ?? ?? ?? ?? 48 8B 14 C8 4B 8D 0C 40 4C 8D 04 CA EB ?? 4D 8B C7 41 8B 40 08 0F BA E0 15", 3, 7, 0x10},
        };
        constexpr uint32_t kKnownTimeDateStamp = 0x5cdc39da;  // 1.1.1.0, only for the log

        // FUObjectArray: ObjFirstGCIndex, ObjLastNonGCIndex, MaxObjectsNotConsideredByGC, OpenForDisregardForGC, then
        // FChunkedFixedUObjectArray ObjObjects at 0x10: Objects (FUObjectItem**), PreAllocatedObjects, MaxElements,
        // NumElements, MaxChunks, NumChunks. 64K items per chunk.
        struct ChunkedArray {
            uint8_t** objects;
            void* preallocated;
            int32_t max_elements;
            int32_t num_elements;
            int32_t max_chunks;
            int32_t num_chunks;
        };
        constexpr int32_t kChunk = 64 * 1024;

        uintptr_t g_base = 0;
        ChunkedArray* g_array = nullptr;
        size_t g_stride = 0;  // sizeof(FUObjectItem), found by CheckRuntime
        void* g_class_class = nullptr;

        using ProcessEventFn = void(__fastcall*)(void* self, void* function, void* params);
        using FNameCtorFn = Name*(__fastcall*)(Name* self, const wchar_t* text, int32_t find_type);
        ProcessEventFn g_process_event = nullptr;
        FNameCtorFn g_fname_ctor = nullptr;
        void* g_tick = nullptr;
        void* g_construct = nullptr;

        struct Section {
            uint8_t* lo = nullptr;
            uint8_t* hi = nullptr;
        };
        Section g_text, g_data;

        Section FindSection(const IMAGE_NT_HEADERS64* nt, const char* name)
        {
            const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
                if (std::strncmp(reinterpret_cast<const char*>(s->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
                    auto* lo = reinterpret_cast<uint8_t*>(g_base + s->VirtualAddress);
                    return {lo, lo + s->Misc.VirtualSize};
                }
            }
            return {};
        }

        // True if every page of the section can be read (the scan touches all of it).
        bool Readable(const Section& s)
        {
            constexpr DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            for (uint8_t* p = s.lo; p < s.hi;) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & kRead) ||
                    (mbi.Protect & PAGE_GUARD))
                    return false;
                p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            }
            return true;
        }

        // A pattern as bytes (-1 for ??), with its runs of known bytes, longest first.
        struct Parsed {
            std::vector<int> bytes;
            std::vector<std::pair<size_t, size_t>> runs;  // (offset, length)
        };

        Parsed Parse(const Pattern& p)
        {
            Parsed out;
            for (const char* c = p.bytes; *c;) {
                if (*c == ' ') {
                    ++c;
                    continue;
                }
                out.bytes.push_back(c[0] == '?' ? -1 : static_cast<int>(std::strtoul(std::string(c, 2).c_str(), nullptr, 16)));
                c += 2;
            }
            for (size_t i = 0; i < out.bytes.size();) {
                size_t j = i;
                while (j < out.bytes.size() && out.bytes[j] >= 0)
                    ++j;
                if (j > i)
                    out.runs.emplace_back(i, j - i);
                i = j + 1;
            }
            std::ranges::stable_sort(out.runs, std::greater{}, &std::pair<size_t, size_t>::second);
            return out;
        }

        // Every hit of the pattern's run `run` in .text (where the whole pattern would start), up to `limit`, passed
        // to `fn`, which returns false to stop.
        template <typename F>
        void ForEachRunHit(const Parsed& pp, std::pair<size_t, size_t> run, F&& fn)
        {
            const auto [run_at, run_len] = run;
            std::vector<uint8_t> needle;
            for (size_t i = run_at; i < run_at + run_len; ++i)
                needle.push_back(static_cast<uint8_t>(pp.bytes[i]));
            const std::boyer_moore_horspool_searcher searcher(needle.begin(), needle.end());
            uint8_t* from = g_text.lo + run_at;
            uint8_t* const to = g_text.hi - pp.bytes.size() + run_at + run_len;
            while (from < to) {
                uint8_t* at = std::search(from, to, searcher);
                if (at == to || !fn(at - run_at))
                    return;
                from = at + 1;
            }
        }

        // How many of the pattern's known bytes match at `m`.
        int Score(const Parsed& pp, const uint8_t* m)
        {
            int score = 0;
            for (size_t k = 0; k < pp.bytes.size(); ++k)
                score += pp.bytes[k] >= 0 && m[k] == pp.bytes[k];
            return score;
        }

        int Known(const Parsed& pp)
        {
            return static_cast<int>(std::ranges::count_if(pp.bytes, [](int b) { return b >= 0; }));
        }

        // Up to `limit` matches of `p` in .text: the hits of its longest run of known bytes where the rest matches too.
        std::vector<uint8_t*> Scan(const Pattern& p, size_t limit = 2)
        {
            const Parsed pp = Parse(p);
            const int known = Known(pp);
            std::vector<uint8_t*> hits;
            ForEachRunHit(pp, pp.runs.front(), [&](uint8_t* m) {
                if (Score(pp, m) == known)
                    hits.push_back(m);
                return hits.size() < limit;
            });
            return hits;
        }

        uint32_t Rva(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) - g_base); }

        bool IsFunctionStart(const uint8_t* p)
        {
            DWORD64 base = 0;
            const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(p), &base, nullptr);
            return f && base == g_base && f->BeginAddress == Rva(p);
        }

        std::string Hex(const uint8_t* p, size_t n)
        {
            std::string s;
            for (size_t i = 0; i < n; ++i)
                s += std::format("{:02X} ", p[i]);
            if (!s.empty())
                s.pop_back();
            return s;
        }

        // For a build the patterns don't fit (the Steam version, a future update): the closest places in the code, so
        // a pattern for that build can be made from the log alone. A function pattern is scored at every function
        // start in the exe's unwind table; every pattern also at each hit of its two longest runs of known bytes.
        // Logs the best three: how many known bytes match, and the first 64 bytes there.
        void Diagnose(const Pattern& p, bool function)
        {
            const Parsed pp = Parse(p);
            const int known = Known(pp);
            const size_t len = pp.bytes.size();
            std::vector<std::pair<int, uint8_t*>> best;  // (score, place), best first, at most 3
            const auto consider = [&](uint8_t* m) {
                if (m < g_text.lo || m + std::max<size_t>(len, 64) > g_text.hi ||
                    std::ranges::any_of(best, [m](const auto& b) { return b.second == m; }))
                    return;
                const int s = Score(pp, m);
                if (best.size() == 3 && s <= best.back().first)
                    return;
                best.insert(std::ranges::find_if(best, [s](const auto& b) { return b.first < s; }), {s, m});
                if (best.size() > 3)
                    best.pop_back();
            };
            if (function) {
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                    g_base + reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
                const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                const auto* table = reinterpret_cast<const RUNTIME_FUNCTION*>(g_base + dir.VirtualAddress);
                for (size_t i = 0; i < dir.Size / sizeof(RUNTIME_FUNCTION); ++i)
                    consider(reinterpret_cast<uint8_t*>(g_base + table[i].BeginAddress));
            }
            for (size_t r = 0; r < std::min<size_t>(2, pp.runs.size()); ++r) {
                if (pp.runs[r].second < 4)
                    continue;
                int seen = 0;
                ForEachRunHit(pp, pp.runs[r], [&](uint8_t* m) {
                    consider(m);
                    return ++seen < 20000;
                });
            }
            LOG_WARN("Engine: {}: the closest places in this exe ({} known bytes in the pattern):", p.name, known);
            for (const auto& [score, m] : best) {
                std::string extra;
                if (!function) {
                    const uint8_t* g = m + p.end + *reinterpret_cast<const int32_t*>(m + p.disp) - p.field;
                    extra = std::format(", would give rva 0x{:08x}{}", Rva(g), g >= g_data.lo && g < g_data.hi ? " (in .data)" : "");
                }
                LOG_WARN("Engine:   rva 0x{:08x}: {} of {} match{}{}: {}", Rva(m), score, known,
                         IsFunctionStart(m) ? ", a function start" : "", extra, Hex(m, 64));
            }
        }

        void LogMatches(const Pattern& p)
        {
            for (uint8_t* m : Scan(p, 4))
                LOG_WARN("Engine:   match at rva 0x{:08x}{}", Rva(m), IsFunctionStart(m) ? " (a function start)" : "");
        }

        // The function `p` matches: exactly one match, at the start of a function in the exe's unwind table.
        uint8_t* FindFunction(const Pattern& p)
        {
            const std::vector<uint8_t*> hits = Scan(p);
            if (hits.size() != 1) {
                LOG_ERROR("Engine: {}: {} - the pattern must match once", p.name, hits.empty() ? "not found" : "several matches");
                if (hits.empty())
                    Diagnose(p, true);
                else
                    LogMatches(p);
                return nullptr;
            }
            if (!IsFunctionStart(hits[0])) {
                LOG_ERROR("Engine: {}: the match at rva 0x{:08x} is not the start of a function", p.name, Rva(hits[0]));
                Diagnose(p, true);
                return nullptr;
            }
            LOG_INFO("Engine: {} at {} (rva 0x{:08x})", p.name, static_cast<void*>(hits[0]), Rva(hits[0]));
            return hits[0];
        }

        // The global the patterns address: in .data, from at least one of them, and the same from each that matches.
        uint8_t* FindGlobal(const Pattern* patterns, size_t count)
        {
            uint8_t* found = nullptr;
            for (size_t i = 0; i < count; ++i) {
                const Pattern& p = patterns[i];
                const std::vector<uint8_t*> hits = Scan(p);
                if (hits.size() != 1) {
                    LOG_WARN("Engine: {}: {}", p.name, hits.empty() ? "not found" : "several matches");
                    if (hits.empty())
                        Diagnose(p, false);
                    else
                        LogMatches(p);
                    continue;
                }
                uint8_t* g = hits[0] + p.end + *reinterpret_cast<const int32_t*>(hits[0] + p.disp) - p.field;
                if (g < g_data.lo || g >= g_data.hi) {
                    LOG_WARN("Engine: {}: the match at rva 0x{:08x} points outside .data", p.name, Rva(hits[0]));
                    continue;
                }
                LOG_INFO("Engine: {} at rva 0x{:08x}: GUObjectArray at {} (rva 0x{:08x})", p.name, Rva(hits[0]),
                         static_cast<void*>(g), Rva(g));
                if (found && found != g) {
                    LOG_ERROR("Engine: the GUObjectArray patterns disagree");
                    return nullptr;
                }
                found = g;
            }
            if (!found)
                LOG_ERROR("Engine: GUObjectArray not found");
            return found;
        }

        // The raw item at index i for a given stride (no validation).
        uint8_t* ItemAt(int32_t i, size_t stride)
        {
            uint8_t* chunk = g_array->objects[i / kChunk];
            return chunk ? chunk + static_cast<size_t>(i % kChunk) * stride : nullptr;
        }
    }

    bool CheckBuild(void* exe_base)
    {
        g_base = reinterpret_cast<uintptr_t>(exe_base);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        const uint32_t stamp = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage;
        LOG_INFO("Engine: exe at 0x{:x}, PE timestamp 0x{:08x}{}, SizeOfImage 0x{:x}", g_base, stamp,
                 stamp == kKnownTimeDateStamp ? " (1.1.1.0)" : "", size);
        g_text = FindSection(nt, ".text");
        g_data = FindSection(nt, ".data");
        if (!g_text.lo || !g_data.lo || !Readable(g_text)) {
            LOG_ERROR("Engine: the exe's code can't be searched (.text {}, .data {}); the plugin stays out",
                      static_cast<void*>(g_text.lo), static_cast<void*>(g_data.lo));
            return false;
        }
        const auto t0 = std::chrono::steady_clock::now();
        uint8_t* const process_event = FindFunction(kProcessEvent);
        uint8_t* const fname_ctor = FindFunction(kFNameCtor);
        uint8_t* const tick = FindFunction(kGameEngineTick);
        uint8_t* const construct = FindFunction(kStaticConstructObject);
        uint8_t* const array = FindGlobal(kGUObjectArray, std::size(kGUObjectArray));
        LOG_INFO("Engine: searched {} MB of code in {} ms", (g_text.hi - g_text.lo) >> 20,
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (!process_event || !fname_ctor || !tick || !construct || !array) {
            LOG_ERROR("Engine: not everything was found - this build of the game is not supported; the plugin stays out");
            return false;
        }
        g_process_event = reinterpret_cast<ProcessEventFn>(process_event);
        g_fname_ctor = reinterpret_cast<FNameCtorFn>(fname_ctor);
        g_tick = tick;
        g_construct = construct;
        g_array = reinterpret_cast<ChunkedArray*>(array + 0x10);
        return true;
    }

    bool CheckRuntime()
    {
        const ChunkedArray& a = *g_array;
        LOG_INFO("Engine: GUObjectArray: {} of {} objects, {} of {} chunks", a.num_elements, a.max_elements, a.num_chunks,
                 a.max_chunks);
        if (!a.objects || a.num_elements <= 0 || a.num_elements > a.max_elements || a.num_chunks <= 0 ||
            a.num_chunks > a.max_chunks || static_cast<int64_t>(a.num_chunks) * kChunk < a.num_elements) {
            LOG_ERROR("Engine: GUObjectArray does not look like one; the plugin stays out");
            return false;
        }
        // FUObjectItem's size: nominally 0x18 in 5.4; take the stride under which every item in the first 4096 that has
        // an object points to an object whose InternalIndex is its own index.
        for (size_t stride : {size_t{0x18}, size_t{0x20}, size_t{0x10}}) {
            int checked = 0, bad = 0;
            for (int32_t i = 0; i < std::min(a.num_elements, 4096); ++i) {
                const uint8_t* item = ItemAt(i, stride);
                const void* o = item ? *reinterpret_cast<void* const*>(item) : nullptr;
                if (!o)
                    continue;
                ++checked;
                if (IsBadReadPtr(o, 0x28) || Index(o) != i)
                    ++bad;
            }
            LOG_INFO("Engine: FUObjectItem stride 0x{:x}: {} objects checked, {} mismatches", stride, checked, bad);
            if (checked > 100 && bad == 0) {
                g_stride = stride;
                break;
            }
        }
        if (!g_stride) {
            LOG_ERROR("Engine: no FUObjectItem layout fits; the plugin stays out");
            return false;
        }
        // UClass's own class is the one object whose class is itself; its name must be the FName of "Class".
        ForEachObject([](void* o, int32_t) {
            if (Class(o) == o) {
                g_class_class = o;
                return false;
            }
            return true;
        });
        const Name class_name = FindName(L"Class");
        if (!g_class_class || !class_name.valid() || NameOf(g_class_class) != class_name) {
            LOG_ERROR("Engine: FName check failed (class object {}, FName(\"Class\") {}:{}, its name {}:{}); the plugin stays out",
                      g_class_class, class_name.index, class_name.number,
                      g_class_class ? NameOf(g_class_class).index : 0, g_class_class ? NameOf(g_class_class).number : 0);
            return false;
        }
        LOG_INFO("Engine: FName constructor checked (\"Class\" = {}, UClass's class at {})", class_name.index, g_class_class);
        return true;
    }

    void* GameEngineTickAddress() { return g_tick; }
    void* StaticConstructObjectAddress() { return g_construct; }

    Name FindName(const wchar_t* text)
    {
        Name n;
        g_fname_ctor(&n, text, 0 /* FNAME_Find */);
        return n;
    }

    int32_t ObjectCount() { return g_array->num_elements; }

    void* ObjectAt(int32_t i, int32_t* item_flags)
    {
        uint8_t* item = ItemAt(i, g_stride);
        if (!item)
            return nullptr;
        if (item_flags)
            *item_flags = *reinterpret_cast<int32_t*>(item + 8);
        return *reinterpret_cast<void**>(item);
    }

    void* FindClass(Name name)
    {
        void* found = nullptr;
        ForEachObject([&](void* o, int32_t) {
            if (Class(o) == g_class_class && NameOf(o) == name) {
                found = o;
                return false;
            }
            return true;
        });
        return found;
    }

    void* FindObject(Name name, Name outer_name)
    {
        void* found = nullptr;
        ForEachObject([&](void* o, int32_t) {
            if (NameOf(o) != name)
                return true;
            if (outer_name.valid()) {
                const void* outer = Outer(o);
                if (!outer || NameOf(outer) != outer_name)
                    return true;
            }
            found = o;
            return false;
        });
        return found;
    }

    void* Lookup(const char* what, const wchar_t* name, const wchar_t* outer, bool is_class)
    {
        const Name n = FindName(name);
        const Name o = outer ? FindName(outer) : Name{};
        void* found = !n.valid() ? nullptr : is_class ? FindClass(n) : FindObject(n, o);
        if (found)
            LOG_INFO("Lookup: {} at {}", what, Describe(found));
        else
            LOG_ERROR("Lookup: {} not found (name {})", what, n.valid() ? "exists" : "unknown");
        return found;
    }

    bool InSlot(const void* o, int32_t index, const void* cls)
    {
        constexpr uint32_t kNotLive = RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed;
        return o && index >= 0 && index < ObjectCount() && ObjectAt(index, nullptr) == o && Class(o) == cls &&
               (Flags(o) & kNotLive) == 0;
    }

    void ProcessEvent(void* object, void* function, void* params) { g_process_event(object, function, params); }

    std::string Describe(const void* object)
    {
        return std::format("{} (index {}, flags 0x{:x})", object, object ? Index(object) : -1, object ? Flags(object) : 0);
    }
}
