#pragma once
#include "GuestModuleLoader.h"
#include <filesystem>

// Opt-in modules use CFG exclusively; unmarked modules retain their existing path.
inline bool UsesCfg(const GuestModule::Image& image)
{
    const auto* format = image.find("PCSX2FSettingsFormat");
    return format && format->size == sizeof(uint32_t) && !(format->address & 3) &&
        image.contains(format->address, sizeof(uint32_t)) && image.word(format->address) == 1;
}
inline std::filesystem::path GuestSettingsPath(std::filesystem::path path, const GuestModule::Image& image)
{
    return path.replace_extension(UsesCfg(image) ? L".cfg" : L".ini");
}

