#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include "GuestHookState.h"

// Register storage belongs to the emulator. Only offsets established by the
// selected source/PDB profile may be supplied here; no host struct casts.
namespace GuestRuntime
{
struct Layout
{
    uint32_t gpr, hi, lo, fpu, sa, pc, branch, delay, status;
};

class Runtime
{
    struct Module { uint32_t entry, stack, gp, context; };
    std::vector<Module> m_modules;
    std::array<uint8_t, 512> m_gpr{};
    std::array<uint8_t, 16> m_hi{}, m_lo{};
    std::array<uint8_t, 264> m_fpu{};
    uint8_t* m_memory = nullptr;
    uint8_t* m_registers = nullptr;
    uint32_t m_memory_size = 0;
    Layout m_layout{};
    uint32_t m_resume_pc = 0, m_sa = 0;
    size_t m_index = 0;
    bool m_running = false;
    std::atomic<bool> m_committed{false};
    uint64_t m_generation = 0;
    void (*m_clear_code)(uint32_t, uint32_t) = nullptr;
    uint8_t* m_accumulator = nullptr;
    uint8_t* m_vu = nullptr;
    PluginHookState::VULayout m_vu_layout{};
    void (*m_finish_vu)() = nullptr;
    inline static thread_local Runtime* s_loader = nullptr;

    void enter()
    {
        const auto& module = m_modules[m_index];
        restore();
        set_reg64(29, module.stack);
        // -G0 modules need no private GP. Preserve the game's value so existing
        // hook helpers also see it while the plugin's init() is executing.
        if (module.gp) set_reg64(28, module.gp);
        set_reg64(31, ReturnStub);
        set_reg64(4, module.context);
        set_reg64(25, module.entry);
        uint32_t game_gp = 0;
        std::memcpy(&game_gp, m_gpr.data() + 28 * 16, 4);
        std::memcpy(m_memory + module.context + 8, &game_gp, 4);
        const uint32_t services[] = {m_clear_code ? CacheStub : 0u, m_clear_code ? CacheCapability | (m_accumulator && m_vu && m_finish_vu ? 4u : 0u) : 0u};
        std::memcpy(m_memory + module.context + 24, services, sizeof(services));
        set32(m_layout.pc, module.entry);
        set32(m_layout.branch, 0);
        set32(m_layout.delay, 0);
    }
    void restore()
    {
        std::memcpy(m_registers + m_layout.gpr, m_gpr.data(), m_gpr.size());
        std::memcpy(m_registers + m_layout.hi, m_hi.data(), m_hi.size());
        std::memcpy(m_registers + m_layout.lo, m_lo.data(), m_lo.size());
        std::memcpy(m_registers + m_layout.fpu, m_fpu.data(), m_fpu.size());
        set32(m_layout.sa, m_sa);
    }
    void set_reg64(uint32_t index, uint64_t value)
    {
        std::memcpy(m_registers + m_layout.gpr + index * 16, &value, 8);
    }

public:
    void set_result(uint32_t value) { set_reg64(2, value); }
    static constexpr uint32_t ReturnStub = 0x02000000;
    static constexpr uint32_t ArenaBegin = ReturnStub + 4096;
    static constexpr uint32_t ArenaEnd = 0x08000000;
    static constexpr uint32_t ReturnSyscall = 0xf1;
    static constexpr uint32_t CacheStub = ReturnStub + 32;
    static constexpr uint32_t WriteStub = ReturnStub + 64;
    static constexpr uint32_t StateStub = ReturnStub + 96;
    static constexpr uint32_t CacheCapability = 0x00010003;

    void set_cache_clear(void (*clear)(uint32_t, uint32_t)) { m_clear_code = clear; }

    void set_hook_state(uint8_t* accumulator, uint8_t* vu, PluginHookState::VULayout layout, void (*finish)())
    { m_accumulator = accumulator; m_vu = vu; m_vu_layout = layout; m_finish_vu = finish; }

    // Stack lifetime gives the load window exception-safe CPU-thread ownership.
    class LoadWindow
    {
        Runtime* m_previous;
    public:
        explicit LoadWindow(Runtime& runtime) : m_previous(s_loader) { s_loader = &runtime; }
        ~LoadWindow() { s_loader = m_previous; }
        LoadWindow(const LoadWindow&) = delete;
        LoadWindow& operator=(const LoadWindow&) = delete;
    };
    void attach(uint8_t* memory, uint32_t memory_size, uint8_t* registers, Layout layout)
    {
        m_memory = memory;
        m_memory_size = std::min(memory_size, ArenaEnd);
        m_registers = registers;
        m_layout = layout;
    }
    void reset()
    {
        m_committed.store(false, std::memory_order_release);
        m_running = false;
        m_modules.clear();
        m_index = 0;
        ++m_generation;
    }
    uint64_t generation() const { return m_generation; }
    bool committed() const { return m_committed.load(std::memory_order_acquire); }
    bool running() const { return m_running; }
    bool loading() const { return s_loader == this; }
    bool in_arena(uint32_t address, uint32_t size) const
    {
        return m_memory && address >= ArenaBegin && size &&
            static_cast<uint64_t>(address) + size <= m_memory_size;
    }
    bool write(uint32_t address, const void* bytes, uint32_t size)
    {
        if (!loading() || committed() || !bytes || !in_arena(address, size)) return false;
        std::memcpy(m_memory + address, bytes, size);
        return true;
    }
    bool queue(uint32_t entry, uint32_t stack, uint32_t gp, uint32_t context)
    {
        if (!loading() || committed() || m_modules.size() >= 256 || !in_arena(entry, 4) || entry % 4 ||
            stack < ArenaBegin + 16384 || stack > m_memory_size || stack % 16 ||
            !in_arena(context, 32) || context % 16) return false;
        m_modules.push_back({entry, stack, gp, context});
        return true;
    }
    bool commit()
    {
        if (!loading() || committed() || m_modules.empty() || !m_registers) return false;
        const uint32_t stub[] = {0x240300f1, 0x0000000c, 0x00000000};
        std::memcpy(m_memory + ReturnStub, stub, sizeof(stub));
        if (m_clear_code) {
            const uint32_t cache_stub[] = {0x240300f2, 0x0000000c, 0x03e00008, 0x00000000};
            std::memcpy(m_memory + CacheStub, cache_stub, sizeof(cache_stub));
            const uint32_t write_stub[] = {0x240300f3, 0x0000000c, 0x03e00008, 0x00000000};
            std::memcpy(m_memory + WriteStub, write_stub, sizeof(write_stub));
            if (m_accumulator && m_vu && m_finish_vu) {
                const uint32_t state_stub[] = {0x240300f4, 0x0000000c, 0x03e00008, 0x00000000};
                std::memcpy(m_memory + StateStub, state_stub, sizeof(state_stub));
            }
        }
        m_committed.store(true, std::memory_order_release);
        return true;
    }
    void abort()
    {
        if (!loading()) return;
        m_modules.clear();
        m_committed.store(false, std::memory_order_release);
    }
    uint32_t get32(uint32_t offset) const
    {
        uint32_t value;
        std::memcpy(&value, m_registers + offset, 4);
        return value;
    }
    void set32(uint32_t offset, uint32_t value) { std::memcpy(m_registers + offset, &value, 4); }
    uint32_t reg32(uint32_t index) const { return get32(m_layout.gpr + index * 16); }
    void set_reg32(uint32_t index, uint32_t value) { set32(m_layout.gpr + index * 16, value); }
    bool ei_permitted() const
    {
        const uint32_t status = get32(m_layout.status);
        return (status & 0x20006) || !(status & 0x18);
    }
    bool start(uint32_t entry)
    {
        if (!committed() || m_running || m_index || m_modules.empty() ||
            get32(m_layout.branch) || get32(m_layout.delay)) return false;
        const uint32_t pc = get32(m_layout.pc) & 0x1fffffff;
        if (pc <= entry || static_cast<uint64_t>(pc) > static_cast<uint64_t>(entry) + 2000) return false;
        m_resume_pc = get32(m_layout.pc);
        std::memcpy(m_gpr.data(), m_registers + m_layout.gpr, m_gpr.size());
        std::memcpy(m_hi.data(), m_registers + m_layout.hi, m_hi.size());
        std::memcpy(m_lo.data(), m_registers + m_layout.lo, m_lo.size());
        std::memcpy(m_fpu.data(), m_registers + m_layout.fpu, m_fpu.size());
        m_sa = get32(m_layout.sa);
        m_running = true;
        enter();
        return true;
    }
    bool return_from_module()
    {
        if (!m_running || reg32(3) != ReturnSyscall || get32(m_layout.pc) != ReturnStub + 8 ||
            get32(m_layout.branch) || get32(m_layout.delay)) return false;
        if (++m_index < m_modules.size()) enter();
        else
        {
            restore();
            set32(m_layout.pc, m_resume_pc);
            m_running = false;
        }
        return true;
    }
    bool clear_code_syscall()
    {
        const uint32_t code = reg32(3);
        if (committed() && m_accumulator && m_vu && m_finish_vu && code == 0xf4 &&
            get32(m_layout.pc) == StateStub + 8 && !get32(m_layout.branch) && !get32(m_layout.delay))
        {
            const uint32_t address = reg32(4), flags = reg32(5), operation = reg32(6);
            const uint32_t size = 8 + ((flags & 2) ? PluginHookState::VUBytes : 0);
            const bool valid = in_arena(address, size) && !(address & 7) && operation <= 1 && flags && !(flags & ~3u);
            set_reg64(2, valid && PluginHookState::Transfer(m_memory + address, flags, operation != 0,
                m_accumulator, m_vu, m_vu_layout, m_finish_vu));
            return true;
        }
        if (!committed() || !m_clear_code || (code != 0xf2 && code != 0xf3) ||
            get32(m_layout.pc) != (code == 0xf2 ? CacheStub : WriteStub) + 8 ||
            get32(m_layout.branch) || get32(m_layout.delay))
            return false;
        const uint32_t address = reg32(4), source = reg32(5), size = reg32(code == 0xf2 ? 5 : 6);
        bool valid = size && !((address | size) & 3) &&
            static_cast<uint64_t>(address) + size <= m_memory_size;
        if (code == 0xf3)
            valid = valid && source && static_cast<uint64_t>(source) + size <= m_memory_size &&
                (static_cast<uint64_t>(address) + size <= ReturnStub || address >= ArenaBegin);
        set_reg64(2, valid);
        if (valid) {
            if (code == 0xf3) std::memmove(m_memory + address, m_memory + source, size);
            m_clear_code(address, size / 4);
        }
        return true;
    }
    // The adapter calls the original RFU060 handler afterwards so BIOS exception
    // dispatch remains intact. GetMemorySize is the existing extended-RAM HLE.
    bool reserve_memory_syscall(uint8_t call)
    {
        if (!committed()) return false;
        if (call == 0x3c && reg32(5) == UINT32_MAX)
            set_reg32(5, ReturnStub - reg32(6));
        if (call == 0x7f)
        {
            set_reg32(2, ReturnStub);
            return true;
        }
        return false;
    }
};
}
