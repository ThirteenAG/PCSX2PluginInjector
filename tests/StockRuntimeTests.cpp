#include "../source/PCSX2PluginInjector/StockBuildConfig.h"
#include <iostream>
#include <thread>

static unsigned checks = 0;
static void check(bool result, const char* description)
{
    ++checks;
    if (!result) throw std::runtime_error(description);
}
template<class F> static void rejects(F function)
{
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Expected rejection");
}

static void runtime_tests()
{
    using GuestRuntime::Runtime;
    Runtime runtime;
    std::vector<uint8_t> memory(Runtime::ArenaEnd), registers(StockABI::RegisterPackSize);
    runtime.attach(memory.data(), static_cast<uint32_t>(memory.size()), registers.data(), StockABI::Registers);
    uint32_t value = 0x12345678;
    check(!runtime.write(Runtime::ArenaBegin, &value, 4), "Writes require load window");
    check(!runtime.queue(Runtime::ArenaBegin, Runtime::ArenaBegin + 16384, 0, Runtime::ArenaBegin + 16), "Queue requires load window");
    {
        Runtime::LoadWindow window(runtime);
        check(!runtime.write(UINT32_MAX - 1, &value, 4), "Write overflow");
        check(!runtime.write(Runtime::ReturnStub, &value, 4), "Reserved page protection");
        check(runtime.write(Runtime::ArenaBegin, &value, 4), "Module write");
        std::thread other([&] { check(!runtime.write(Runtime::ArenaBegin, &value, 4), "Load window CPU thread ownership"); });
        other.join();
        check(!runtime.queue(Runtime::ArenaBegin + 1, Runtime::ArenaBegin + 16384, 0, Runtime::ArenaBegin + 16), "Entry alignment");
        check(runtime.queue(Runtime::ArenaBegin, Runtime::ArenaBegin + 32768, 0, Runtime::ArenaBegin + 16), "First module");
        check(runtime.queue(Runtime::ArenaBegin + 4096, Runtime::ArenaBegin + 65536, 0, Runtime::ArenaBegin + 32), "Second module");
        check(runtime.commit(), "Commit");
        check(!runtime.write(Runtime::ArenaBegin, &value, 4), "Committed writes rejected");
        check(!runtime.commit(), "Double commit rejected");
    }
    const auto layout = StockABI::Registers;
    for (size_t i = 0; i < registers.size(); ++i) registers[i] = static_cast<uint8_t>(i * 17 + 3);
    runtime.set32(layout.branch, 0); runtime.set32(layout.delay, 0);
    runtime.set32(layout.pc, 0x100000); check(!runtime.start(0x100000), "Start after entry only");
    runtime.set32(layout.pc, 0x100010);
    runtime.set32(layout.branch, 1); check(!runtime.start(0x100000), "No delay-slot startup");
    runtime.set32(layout.branch, 0);
    auto saved = registers;
    check(runtime.start(0x100000), "Startup");
    check(runtime.get32(layout.pc) == Runtime::ArenaBegin, "First entry");
    check(runtime.reg32(29) == Runtime::ArenaBegin + 32768 && runtime.reg32(31) == Runtime::ReturnStub, "Private stack/return");
    uint32_t gp;
    std::memcpy(&gp, memory.data() + Runtime::ArenaBegin + 16 + 8, 4);
    check(gp == *reinterpret_cast<const uint32_t*>(saved.data() + 28 * 16), "Game GP in module context");
    check(std::memcmp(registers.data() + 28 * 16, saved.data() + 28 * 16, 16) == 0, "Game GP preserved at module entry");
    check(!runtime.start(0x100000), "No reentry");
    for (unsigned module = 0; module < 2; ++module)
    {
        std::memset(registers.data(), 0xDD, 544);
        std::memset(registers.data() + layout.fpu, 0xCC, 264);
        runtime.set32(layout.sa, 99);
        runtime.set_reg32(3, Runtime::ReturnSyscall);
        runtime.set32(layout.pc, Runtime::ReturnStub + 8);
        runtime.set32(layout.branch, 1); check(!runtime.return_from_module(), "Return in branch rejected");
        runtime.set32(layout.branch, 0); runtime.set32(layout.delay, 0);
        check(runtime.return_from_module(), "Return accepted");
        if (!module) check(runtime.get32(layout.pc) == Runtime::ArenaBegin + 4096, "Second entry");
    }
    check(!runtime.running(), "All modules finished");
    check(std::memcmp(registers.data(), saved.data(), 544) == 0, "Full 128-bit GPR/HI/LO restored");
    check(std::memcmp(registers.data() + layout.fpu, saved.data() + layout.fpu, 264) == 0, "FPU restored");
    check(runtime.get32(layout.sa) == *reinterpret_cast<const uint32_t*>(saved.data() + layout.sa), "SA restored");
    check(runtime.get32(layout.pc) == 0x100010, "Game PC restored");
    check(!runtime.start(0x100000), "Run once per ELF");
    runtime.set_reg32(5, UINT32_MAX); runtime.set_reg32(6, 0x10000);
    check(!runtime.reserve_memory_syscall(0x3c), "RFU060 still dispatches to BIOS");
    check(runtime.reg32(5) == Runtime::ReturnStub - 0x10000, "Game heap excludes arena");
    check(runtime.reserve_memory_syscall(0x7f) && runtime.reg32(2) == Runtime::ReturnStub, "Game memory size excludes arena");
    for (uint32_t status = 0; status < 256; ++status)
    {
        runtime.set32(layout.status, status);
        check(runtime.ei_permitted() == ((status & 6) != 0 || (status & 0x18) == 0), "EI privilege condition");
    }
    runtime.reset();
    check(!runtime.committed() && runtime.generation() == 1, "Reset generation");
    check(!runtime.reserve_memory_syscall(0x7f), "No reservation without plugins");
}

static void image_tests(const wchar_t* profile, const wchar_t* executable)
{
    using namespace StockPCSX2;
    const auto config = ReadConfiguration(profile);
    const auto file = ReadImageFile(executable);
    auto* mapped = static_cast<uint8_t*>(VirtualAlloc(nullptr, config.image_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mapped) throw std::runtime_error("Image test allocation failed");
    struct Release { uint8_t* p; ~Release() { VirtualFree(p, 0, MEM_RELEASE); } } release{mapped};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    std::memcpy(mapped, file.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        std::memcpy(mapped + sections[i].VirtualAddress, file.data() + sections[i].PointerToRawData, sections[i].SizeOfRawData);
    ValidateImage(config, file, mapped); ++checks;
    auto bad = config; bad.sha[0] = bad.sha[0] == '0' ? '1' : '0';
    rejects([&] { ValidateImage(bad, file, mapped); });
    bad = config; ++bad.age; rejects([&] { ValidateImage(bad, file, mapped); });
    bad = config; bad.image_size += 4096; rejects([&] { ValidateImage(bad, file, mapped); });
    for (size_t role = 0; role < FunctionRoleCount; ++role)
    {
        const auto& symbol = config.symbols.at(Roles[role]);
        for (size_t i = 0; i < symbol.bytes.size(); ++i)
        {
            mapped[symbol.rva + i] ^= 0x55;
            if (symbol.mask[i]) rejects([&] { ValidateImage(config, file, mapped); });
            else { ValidateImage(config, file, mapped); ++checks; }
            mapped[symbol.rva + i] ^= 0x55;
        }
    }
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        runtime_tests();
        if (argc == 3) image_tests(argv[1], argv[2]);
        std::cout << "Stock runtime/identity: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
