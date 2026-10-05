#include "stdafx.h"
#include "EmulatorHost.h"
#include "StockBuildConfig.h"
#include <safetyhook/inline_hook.hpp>

extern "C" size_t GetOSDVectorSize();
extern "C" const char* GetOSDVectorData(size_t index);

namespace StockPCSX2
{
namespace
{
    Configuration s_config;
    uint8_t* s_image = nullptr;
    GuestRuntime::Runtime s_runtime;
    std::map<std::string, safetyhook::InlineHook> s_hooks;
    std::vector<InitCB> s_init;
    std::vector<ShutdownCB> s_shutdown;
    std::atomic<bool> s_unthrottle{false};
    std::atomic<bool> s_have_game{false};
    thread_local bool s_elf_init_window = false;
    thread_local bool s_loaded_this_entry = false;
    HWND s_window = nullptr;
    std::mutex s_warning_mutex;
    std::string s_warning;
    ULONGLONG s_warning_until = 0;

    uint8_t* Address(const char* role) { return s_image + s_config.symbols.at(role).rva; }
    template<class T> T Data(const char* role)
    {
        T value;
        std::memcpy(&value, Address(role), sizeof(value));
        return value;
    }
    template<class Fn> Fn Function(const char* role) { return reinterpret_cast<Fn>(Address(role)); }
    template<class Fn> Fn Original(const char* role) { return s_hooks.at(role).original<Fn>(); }

    void Warn(const char* message)
    {
        if (!message) return;
        spd::log()->warn("{}", message);
        std::lock_guard lock(s_warning_mutex);
        s_warning = message;
        s_warning_until = GetTickCount64() + 20000;
    }
    bool Write(uint32_t address, const void* bytes, uint32_t size) { return s_runtime.write(address, bytes, size); }
    bool Queue(uint32_t entry, uint32_t stack, uint32_t gp, uint32_t context) { return s_runtime.queue(entry, stack, gp, context); }
    bool Commit() { return s_runtime.commit(); }
    void Abort() { s_runtime.abort(); }
    const PCSX2FGuestHostV1* GuestAPI(uint32_t version, uint32_t size)
    {
        static PCSX2FGuestHostV1 api{sizeof(api), 1, GuestRuntime::Runtime::ArenaBegin, GuestRuntime::Runtime::ArenaEnd,
            0, Write, Queue, Commit, Abort, Warn};
        if (version != 1 || size != sizeof(api)) return nullptr;
        api.generation = s_runtime.generation();
        return &api;
    }
    void WriteBytes(uint32_t address, const void* bytes, uint32_t size)
    {
        // Input/display adapters write data after startup, potentially from the
        // UI thread. Code loading itself still goes through the load-window API.
        if (!bytes || !s_runtime.in_arena(address, size)) return;
        auto* memory = Data<uint8_t*>("ee_memory");
        if (memory) std::memcpy(memory + address, bytes, size);
    }
    bool GetUnthrottle() { return s_unthrottle.load(std::memory_order_relaxed); }
    void SetUnthrottle(bool value) { s_unthrottle.store(value, std::memory_order_relaxed); }
    VMState State()
    {
        const uint32_t state = Data<uint32_t>("vm_state");
        return state <= static_cast<uint32_t>(VMState::Stopping) ? static_cast<VMState>(state) : VMState::Shutdown;
    }
    void AddInit(InitCB callback) { s_init.push_back(callback); }
    void AddShutdown(ShutdownCB callback) { s_shutdown.push_back(callback); }
    void ClearGame()
    {
        s_have_game.store(false, std::memory_order_release);
        for (auto callback : s_shutdown) callback();
        s_runtime.reset();
        s_unthrottle.store(false);
        s_window = nullptr;
        std::lock_guard lock(s_warning_mutex);
        s_warning.clear();
    }
    std::string ReadString(const char* role)
    {
        // The reviewed Windows release uses MSVC STL's 32-byte string ABI.
        // Borrow its storage; do not move/destroy a host-owned string or pass an
        // allocated injector string to a host callee that would own it.
        const uint8_t* object = Address(role);
        uint64_t size = 0, capacity = 0;
        std::memcpy(&size, object + 16, 8); std::memcpy(&capacity, object + 24, 8);
        if (size > capacity || size > 4096) throw std::runtime_error("Invalid host metadata string");
        const uint8_t* text = object;
        if (capacity >= 16) std::memcpy(&text, object, 8);
        if (size && !Readable(text, static_cast<size_t>(size))) throw std::runtime_error("Unreadable host metadata string");
        return size ? std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(size)) : std::string();
    }
    BOOL CALLBACK FindWindow(HWND window, LPARAM)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
        wchar_t class_name[128]{};
        GetClassNameW(window, class_name, std::size(class_name));
        if (std::wstring_view(class_name).starts_with(L"Qt")) s_window = window;
        return TRUE;
    }
    void LoadOnCPUThread()
    {
        try
        {
            auto* memory = Data<uint8_t*>("ee_memory");
            const uint32_t size = Data<uint32_t>("exposed_ram");
            s_runtime.attach(memory, size, Address("registers"), StockABI::Registers);
            const auto serial = ReadString("disc_serial"), elf = ReadString("disc_elf"), version = ReadString("disc_version"),
                title = ReadString("title"), path = ReadString("elf_path");
            const auto* device = Data<const uint8_t*>("gs_device");
            const auto* thread = Data<const uint8_t*>("emu_thread");
            if (!Readable(device, StockABI::GSDeviceSize) || !Readable(thread, StockABI::EmuThreadSize))
                throw std::runtime_error("Stock render window/thread is unavailable at ELF startup");
            const auto* window_info = device + StockABI::GSWindowOffset;
            uint32_t width = 0, height = 0;
            std::memcpy(&s_window, window_info + StockABI::WindowHandleOffset, sizeof(s_window));
            std::memcpy(&width, window_info + StockABI::SurfaceWidthOffset, 4);
            std::memcpy(&height, window_info + StockABI::SurfaceHeightOffset, 4);
            if (!s_window) EnumWindows(FindWindow, 0);
            const bool fullscreen = thread[StockABI::FullscreenOffset] != 0;
            const uint8_t aspect = Address("config")[StockABI::AspectRatioOffset];
            s_have_game.store(true, std::memory_order_release);
            GuestRuntime::Runtime::LoadWindow window(s_runtime);
            for (auto callback : s_init)
                callback(serial.c_str(), elf.c_str(), version.c_str(), title.c_str(), path.c_str(), Data<uint32_t>("disc_crc"),
                    Data<uint32_t>("current_crc"), Data<uint32_t>("elf_entry"), memory, size, &s_window,
                    width, height, fullscreen, aspect);
        }
        catch (const std::exception& error)
        {
            GuestRuntime::Runtime::LoadWindow window(s_runtime);
            s_runtime.abort();
            Warn(error.what());
        }
    }
    void Entry()
    {
        if (Data<bool>("elf_executed")) { Original<void(*)()>("elf_init")(); return; }
        s_loaded_this_entry = false;
        s_elf_init_window = true;
        struct Close { ~Close() { s_elf_init_window = false; } } close;
        Original<void(*)()>("elf_init")();
        if (!s_loaded_this_entry) Warn("Stock PCSX2 did not reach the reviewed ELF load boundary; guest plugins were not started.");
    }
    void BootPatches()
    {
        if (s_elf_init_window && !s_loaded_this_entry)
        {
            s_loaded_this_entry = true;
            LoadOnCPUThread();
        }
        // The caller then resets block tracking/execution caches, just as in the
        // fork. Loading before this call needs no additional live-JIT reset.
        Original<void(*)()>("boot_patches")();
    }
    void ELFLoading(void* string_argument)
    {
        ClearGame();
        // Windows x64 passes this nontrivial std::string value indirectly. The
        // original callee owns its destruction; the hook forwards the pointer.
        Original<void(*)(void*)>("elf_loading")(string_argument);
    }
    void Shutdown(bool save_resume_state)
    {
        // Upstream may save a resume state during shutdown. Keep reservation
        // active until the original returns so that state is also rejected.
        for (auto callback : s_shutdown) callback();
        s_have_game.store(false, std::memory_order_release);
        Original<void(*)(bool)>("shutdown")(save_resume_state);
        s_runtime.reset();
        s_unthrottle.store(false);
        s_window = nullptr;
    }
    void Reset()
    {
        ClearGame();
        Original<void(*)()>("reset")();
    }
    void EI()
    {
        // The BIOS executes EI before any game is attached. Read the already
        // validated register pack directly without requiring a loaded module.
        s_runtime.attach(Data<uint8_t*>("ee_memory"), Data<uint32_t>("exposed_ram"), Address("registers"), StockABI::Registers);
        const bool permitted = s_runtime.ei_permitted();
        Original<void(*)()>("ei")();
        if (permitted && s_runtime.start(Data<uint32_t>("elf_entry")))
            spd::log()->info("Stock guest module startup at 0x{:08X}", s_runtime.get32(StockABI::Registers.pc));
    }
    void SYSCALL()
    {
        s_runtime.attach(Data<uint8_t*>("ee_memory"), Data<uint32_t>("exposed_ram"), Address("registers"), StockABI::Registers);
        if (s_runtime.return_from_module())
        {
            if (!s_runtime.running())
            {
                spd::log()->info("Stock guest modules returned; game context restored at 0x{:08X}",
                    s_runtime.get32(StockABI::Registers.pc));
                for (size_t i = 0; i < GetOSDVectorSize(); ++i)
                {
                    const char* row = GetOSDVectorData(i);
                    const auto size = strnlen_s(row, 255);
                    if (size) spd::log()->info("Guest startup message: {}", std::string_view(row, size));
                }
            }
            return;
        }
        const uint32_t code = s_runtime.reg32(3);
        const uint8_t call = static_cast<uint8_t>((code & 0x80000000) ? 0u - code : code);
        if (!s_runtime.reserve_memory_syscall(call)) Original<void(*)()>("syscall")();
    }
    void Throttle(bool vsync_start)
    {
        if (!GetUnthrottle()) Original<void(*)(bool)>("throttle")(vsync_start);
    }
    void SetError(void* error)
    {
        constexpr std::string_view message = "Guest plugins are active. Save/load states require the planned module persistence support.";
        if (error) Function<void(*)(void*, const std::string_view*)>("set_error")(error, &message);
        Warn(message.data());
    }
    void* SaveState(void* result, void* error)
    {
        if (!s_runtime.committed()) return Original<void*(*)(void*, void*)>("save_state")(result, error);
        // Verified hidden-return convention for unique_ptr<ArchiveEntryList>:
        // RCX is caller result storage, RDX is Error*. Return storage in RAX.
        *static_cast<void**>(result) = nullptr;
        SetError(error);
        return result;
    }
    bool LoadState(const void* filename, void* error)
    {
        if (!s_runtime.committed()) return Original<bool(*)(const void*, void*)>("load_state")(filename, error);
        SetError(error);
        return false;
    }
    void RenderOSD()
    {
        Original<void(*)()>("render_osd")();
        const bool show_rows = s_have_game.load(std::memory_order_acquire) && State() == VMState::Running;
        std::string warning;
        {
            std::lock_guard lock(s_warning_mutex);
            if (GetTickCount64() < s_warning_until) warning = s_warning;
        }
        const size_t count = show_rows ? GetOSDVectorSize() : 0;
        if (!count && warning.empty()) return;
        constexpr int flags = 1 | 2 | 4 | 8 | 64 | 128 | 256 | 512 | (1 << 18) | (1 << 19);
        if (Function<bool(*)(const char*, bool*, int)>("imgui_begin")("Guest plugins", nullptr, flags))
        {
            auto text = Function<void(*)(const char*, const char*, int)>("imgui_text");
            if (!warning.empty()) text(warning.data(), warning.data() + warning.size(), 0);
            for (size_t i = 0; i < count; ++i)
            {
                // Reset can remove rows between count/data. The shared getter
                // returns a safe empty row; bounds never depend on a guest NUL.
                const char* row = GetOSDVectorData(i);
                const size_t length = strnlen_s(row, 255);
                if (length) text(row, row + length, 0);
            }
        }
        Function<void(*)()>("imgui_end")();
    }
    void CreateHook(const char* role, void* destination)
    {
        auto hook = safetyhook::InlineHook::create(Address(role), destination, safetyhook::InlineHook::StartDisabled);
        if (!hook) throw std::runtime_error(std::string("Cannot prepare stock hook: ") + role);
        s_hooks.emplace(role, std::move(*hook));
    }
}

bool Initialize(Bindings& bindings)
{
    try
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Initialize), &self);
        std::array<wchar_t, 32768> self_path{}, exe_path{};
        if (!GetModuleFileNameW(self, self_path.data(), static_cast<DWORD>(self_path.size())) ||
            !GetModuleFileNameW(nullptr, exe_path.data(), static_cast<DWORD>(exe_path.size())))
            throw std::runtime_error("Cannot resolve stock injector/executable path");
        s_config = ReadConfiguration(std::filesystem::path(self_path.data()).parent_path() / L"PCSX2PluginInjector.stock.ini");
        s_image = reinterpret_cast<uint8_t*>(GetModuleHandle(nullptr));
        const auto file = ReadImageFile(exe_path.data());
        ValidateImage(s_config, file, s_image);
        if (s_config.activation == "disabled") throw std::runtime_error("Stock profile is disabled pending runtime validation");
        if (State() != VMState::Shutdown) throw std::runtime_error("Restart PCSX2 before installing the stock adapter");
        const std::pair<const char*, void*> hooks[] = {{"elf_init", reinterpret_cast<void*>(&Entry)},
            {"boot_patches", reinterpret_cast<void*>(&BootPatches)}, {"elf_loading", reinterpret_cast<void*>(&ELFLoading)},
            {"shutdown", reinterpret_cast<void*>(&Shutdown)}, {"reset", reinterpret_cast<void*>(&Reset)},
            {"ei", reinterpret_cast<void*>(&EI)}, {"syscall", reinterpret_cast<void*>(&SYSCALL)},
            {"throttle", reinterpret_cast<void*>(&Throttle)}, {"save_state", reinterpret_cast<void*>(&SaveState)},
            {"load_state", reinterpret_cast<void*>(&LoadState)}, {"render_osd", reinterpret_cast<void*>(&RenderOSD)}};
        // Prepare every trampoline disabled; only publish after complete validation.
        for (auto [role, destination] : hooks) CreateHook(role, destination);
        bindings = {GuestAPI, WriteBytes, GetUnthrottle, SetUnthrottle, State, AddInit, AddShutdown};
        return true;
    }
    catch (const std::exception& error)
    {
        s_hooks.clear(); // Roll back every installed hook on partial setup failure.
        spd::log()->error("Stock adapter rejected: {}", error.what());
        MessageBoxA(nullptr, error.what(), "PCSX2PluginInjector", MB_ICONWARNING);
        return false;
    }
}
bool Activate()
{
    try
    {
        for (auto& [role, hook] : s_hooks)
            if (!hook.enable()) throw std::runtime_error("Cannot activate stock hook: " + role);
        spd::log()->info("Stock adapter upstream-win64-v1 installed for PCSX2 {} (upstream {}), {}",
            s_config.version, s_config.source, s_config.activation);
        return true;
    }
    catch (const std::exception& error)
    {
        s_hooks.clear();
        spd::log()->error("Stock adapter activation failed: {}", error.what());
        MessageBoxA(nullptr, error.what(), "PCSX2PluginInjector", MB_ICONWARNING);
        return false;
    }
}
}
