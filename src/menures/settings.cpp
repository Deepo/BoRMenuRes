#include "stdafx.h"
#include "settings.h"

#include <algorithm>
#include <charconv>

namespace Config
{
    namespace
    {
        // "Auto" or a whole number, clamped to 1024-8192; anything else is Auto, with a warning.
        int ParseSize(const std::string& raw)
        {
            if (ToLower(raw) == "auto")
                return 0;
            int value = 0;
            const auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
            if (ec != std::errc{} || end != raw.data() + raw.size()) {
                LOG_WARN("Config: [Preview] Size: \"{}\" is neither Auto nor a number, using Auto", raw);
                return 0;
            }
            return std::clamp(value, 1024, 8192);
        }
    }

    void Init(const std::filesystem::path& folder)
    {
        Read(folder);
        Enabled = ParseConfig("General", "Enabled", Enabled);
        LogOnly = ParseConfig("General", "LogOnly", LogOnly);
        Resize  = ParseConfig("Preview", "Resize", Resize);
        Size    = ParseSize(ParseConfig("Preview", "Size", std::string("Auto")));
        Sharper = ParseConfig("Shadows", "Sharper", Sharper);
        Factor  = std::clamp(ParseConfig("Shadows", "Factor", Factor), 1, 4);
        SkipLogos = ParseConfig("Intro", "SkipLogos", SkipLogos);
    }
}
