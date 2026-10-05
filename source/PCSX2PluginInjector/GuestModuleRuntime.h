#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

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
    static constexpr uint32_t ReturnStub = 0x02000000;
    static constexpr uint32_t ArenaBegin = ReturnStub + 4096;
    static constexpr uint32_t ArenaEnd = 0x08000000;
    static constexpr uint32_t ReturnSyscall = 0xf1;

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
