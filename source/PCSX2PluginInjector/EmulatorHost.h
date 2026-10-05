#pragma once
#include <cstddef>
#include <cstdint>
#include "guest_module.h"

enum class VMState { Shutdown, Initializing, Running, Paused, Resetting, Stopping };
using InitCB = void (*)(const char*, const char*, const char*, const char*, const char*,
    uint32_t, uint32_t, uint32_t, uint8_t*, size_t, const void*, uint32_t, uint32_t, bool, uint8_t);
using ShutdownCB = void (*)();
using tWriteBytes = void (*)(uint32_t, const void*, uint32_t);
using tGetIsThrottlerTempDisabled = bool (*)();
using tSetIsThrottlerTempDisabled = void (*)(bool);
using tGetVMState = VMState (*)();
using tAddOnGameElfInitCallback = void (*)(InitCB);
using tAddOnGameShutdownCallback = void (*)(ShutdownCB);
using GetGuestHostApi = const PCSX2FGuestHostV1* (*)(uint32_t, uint32_t);

namespace StockPCSX2
{
struct Bindings
{
    GetGuestHostApi guest_api;
    tWriteBytes write;
    tGetIsThrottlerTempDisabled get_unthrottle;
    tSetIsThrottlerTempDisabled set_unthrottle;
    tGetVMState state;
    tAddOnGameElfInitCallback add_init;
    tAddOnGameShutdownCallback add_shutdown;
};
bool Initialize(Bindings& bindings);
bool Activate();
}
