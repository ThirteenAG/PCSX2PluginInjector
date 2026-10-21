#pragma once
#include "PluginSettingsService.h"
#include "GuestModuleLoader.h"

namespace PluginSettings {
inline bool BooleanPeerStorage(const GuestModule::Image& image, uint32_t address) {
    return !(address & 3) && image.contains(address, sizeof(PCSX2FBoolPeerRequest), 1);
}
inline bool BooleanToken(const char (&value)[64]) {
    return (value[0] == '0' || value[0] == '1') && value[1] == 0;
}
inline bool BooleanPeerHeader(const PCSX2FBoolPeerRequest& request) {
    if (request.size != sizeof(request) || request.version != 2 || request.count != 1 ||
        request.operation > PCSX2F_SETTINGS_WRITE || !Valid(request.section, true) || !Valid(request.key, true)) return false;
    const auto* end = static_cast<const char*>(std::memchr(request.module, 0, sizeof(request.module)));
    if (!end || end - request.module < 5 || std::string_view(end - 4, 4) != ".elf") return false;
    for (const char* p = request.module; p != end; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return false;
    return request.operation == PCSX2F_SETTINGS_READ || BooleanToken(request.value);
}
inline uint32_t ProcessBooleanPeer(const std::filesystem::path& path, PCSX2FBoolPeerRequest& peer) {
    if (!BooleanPeerHeader(peer)) return PCSX2F_SETTINGS_INVALID;
    PCSX2FIniRequest request{};
    request.size = sizeof(request); request.version = 1; request.operation = PCSX2F_SETTINGS_READ; request.count = 1;
    std::memcpy(request.entries[0].section, peer.section, sizeof(peer.section));
    std::memcpy(request.entries[0].key, peer.key, sizeof(peer.key));
    auto status = Process(path, request);
    if (status != PCSX2F_SETTINGS_OK) return status;
    if (!BooleanToken(request.entries[0].value)) return PCSX2F_SETTINGS_INVALID;
    if (peer.operation == PCSX2F_SETTINGS_WRITE) {
        request.operation = PCSX2F_SETTINGS_WRITE;
        std::memcpy(request.entries[0].value, peer.value, sizeof(peer.value));
        status = Process(path, request);
        if (status != PCSX2F_SETTINGS_OK) return status;
        request.operation = PCSX2F_SETTINGS_READ;
        status = Process(path, request);
        if (status != PCSX2F_SETTINGS_OK) return status;
        if (!BooleanToken(request.entries[0].value) || request.entries[0].value[0] != peer.value[0]) return PCSX2F_SETTINGS_IO_ERROR;
    }
    std::memcpy(peer.value, request.entries[0].value, sizeof(peer.value));
    return PCSX2F_SETTINGS_OK;
}
}
