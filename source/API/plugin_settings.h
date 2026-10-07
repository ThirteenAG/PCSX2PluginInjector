#ifndef PCSX2F_PLUGIN_SETTINGS_H
#define PCSX2F_PLUGIN_SETTINGS_H
#include <stdint.h>

/* Optional, synchronous host service. Export PCSX2FSettingsVersion initialized
 * to zero; the injector sets it to 1 when this service is available. The host
 * selects the INI beside the calling module; no host path is supplied by the
 * guest. Use only on settings changes/menu close, never in a rendering loop.
 * Requests live in the module's own data, stack or bounded heap. */
#define PCSX2F_SETTINGS_API() uint32_t PCSX2FSettingsVersion = 0
extern uint32_t PCSX2FSettingsVersion;
enum PCSX2FSettingsStatus {
    PCSX2F_SETTINGS_OK = 0, PCSX2F_SETTINGS_UNSUPPORTED = 1,
    PCSX2F_SETTINGS_INVALID = 2, PCSX2F_SETTINGS_NOT_FOUND = 3,
    PCSX2F_SETTINGS_IO_ERROR = 4
};
enum { PCSX2F_SETTINGS_READ = 0, PCSX2F_SETTINGS_WRITE = 1,
       PCSX2F_SETTINGS_MAX_ENTRIES = 8, PCSX2F_SETTINGS_SYSCALL = 0xF5,
       PCSX2F_SETTINGS_MAGIC = 0x50434653 };
struct PCSX2FIniEntry { char section[64], key[64], value[64]; };
struct PCSX2FIniRequest {
    uint32_t size, version, operation, count;
    struct PCSX2FIniEntry entries[PCSX2F_SETTINGS_MAX_ENTRIES];
};
#if defined(__mips__)
static inline uint32_t PCSX2F_IniRequest(struct PCSX2FIniRequest* request) {
    if (PCSX2FSettingsVersion != 1) return PCSX2F_SETTINGS_UNSUPPORTED;
    register uint32_t number __asm__("$3") = PCSX2F_SETTINGS_SYSCALL;
    register uint32_t magic __asm__("$4") = PCSX2F_SETTINGS_MAGIC;
    register struct PCSX2FIniRequest* data __asm__("$5") = request;
    register uint32_t result __asm__("$2");
    __asm__ __volatile__("syscall" : "=r"(result), "+r"(number), "+r"(magic), "+r"(data) : : "memory");
    return result;
}
#endif
#endif
