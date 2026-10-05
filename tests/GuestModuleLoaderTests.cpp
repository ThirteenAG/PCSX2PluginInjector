#include "../source/PCSX2PluginInjector/GuestModuleLoader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace GuestModule;
using namespace GuestModule::Detail;
static unsigned checks = 0;
static void Check(bool value, const std::string& message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
static void Put16(std::vector<char>& b, size_t at, uint16_t value)
{
    b[at] = char(value); b[at + 1] = char(value >> 8);
}
struct Fixture
{
    std::vector<char> file = std::vector<char>(4096, 0);
    uint32_t headers = 2048, text = 64, data = 128, symbols = 800, strings = 1024, names = 1200, textRel = 1400, dataRel = 1500;
    Fixture()
    {
        std::memcpy(file.data(), "\x7f" "ELF\1\1\1", 7);
        Put16(file, 16, 1); Put16(file, 18, 8); Put(file, 20, 1); Put(file, 32, headers);
        Put16(file, 40, 52); Put16(file, 46, 40); Put16(file, 48, 9); Put16(file, 50, 6);
        const char str[] = "\0PCSX2FModule\0CompatibleCRCList\0OSDText\0TestEntry\0Target\0ZeroBss\0weak\0";
        std::memcpy(file.data() + strings, str, sizeof(str));
        const char sectionNames[] = "\0.text\0.data\0.bss\0.symtab\0.strtab\0.shstrtab\0.rel.text\0.rel.data\0";
        std::memcpy(file.data() + names, sectionNames, sizeof(sectionNames));
        auto sh = [&](unsigned index, uint32_t name, uint32_t type, uint32_t flags, uint32_t offset, uint32_t size,
            uint32_t link = 0, uint32_t info = 0, uint32_t align = 4, uint32_t entrySize = 0)
        {
            size_t at = headers + index * 40;
            const uint32_t fields[] = {name, type, flags, 0, offset, size, link, info, align, entrySize};
            for (unsigned i = 0; i < 10; ++i) Put(file, at + i * 4, fields[i]);
        };
        sh(1, 1, 1, 6, text, 64, 0, 0, 16);
        sh(2, 7, 1, 3, data, 512, 0, 0, 128);
        sh(3, 13, 8, 3, 0, 32, 0, 0, 16);
        sh(4, 18, 2, 0, symbols, 8 * 16, 5, 1, 4, 16);
        sh(5, 26, 3, 0, strings, sizeof(str), 0, 0, 1);
        sh(6, 34, 3, 0, names, sizeof(sectionNames), 0, 0, 1);
        sh(7, 44, 9, 0, textRel, 4 * 8, 4, 1, 4, 8);
        sh(8, 54, 9, 0, dataRel, 2 * 8, 4, 2, 4, 8);
        auto sym = [&](unsigned index, unsigned name, unsigned value, unsigned size, unsigned owner, unsigned binding = 1)
        {
            const size_t at = symbols + index * 16;
            Put(file, at, name); Put(file, at + 4, value); Put(file, at + 8, size);
            file[at + 12] = char(binding << 4); Put16(file, at + 14, uint16_t(owner));
        };
        sym(1, 1, 0, 32, 2); sym(2, 14, 32, 4, 2); sym(3, 32, 48, 255, 2);
        sym(4, 40, 0, 16, 1); sym(5, 50, 36, 4, 2); sym(6, 57, 0, 32, 3); sym(7, 65, 0, 0, 0, 2);
        Put(file, data, PCSX2F_MODULE_MAGIC); Put(file, data + 4, 1); Put(file, data + 8, 32);
        Put(file, data + 20, 16384); Put(file, data + 24, 4096);
        Put(file, data + 32, 0x4f32a11f); Put(file, data + 36, 0x12348000);
        Put(file, text, 0x3c080000); Put(file, text + 4, 0x25088000); Put(file, text + 8, 0x3c090001); Put(file, text + 12, 0x0c000000);
        // Multiple HI16s before their shared signed LO16.
        const uint32_t tr[] = {0, (5 << 8) | 5, 8, (5 << 8) | 5, 4, (5 << 8) | 6, 12, (4 << 8) | 4};
        const uint32_t dr[] = {16, (4 << 8) | 2, 40, (5 << 8) | 2};
        for (unsigned i = 0; i < 8; ++i) Put(file, textRel + i * 4, tr[i]);
        for (unsigned i = 0; i < 4; ++i) Put(file, dataRel + i * 4, dr[i]);
        file.resize(headers + 9 * 40);
    }
};
static void Reject(const std::vector<char>& file, const std::string& expected = "", uint32_t base = ArenaBegin, uint32_t end = ArenaEnd)
{
    Image image; image.info.Base = 0xdeadbeef; image.bytes = {'x'};
    std::string error;
    Check(!Load(file, base, end, image, error), "accepted malformed module: " + expected);
    Check(!error.empty() && (expected.empty() || error.find(expected) != std::string::npos), "wrong error: " + error + " expected " + expected);
    Check(image.info.Base == 0xdeadbeef && image.bytes == std::vector<char>{'x'}, "failure changed output");
}
int main(int argc, char** argv)
{
    try
    {
        Fixture fixture;
        Image image, rebased;
        std::string error;
        Check(Load(fixture.file, ArenaBegin, ArenaEnd, image, error), error);
        Check(Load(fixture.file, ArenaBegin + 0x18000, ArenaEnd, rebased, error), error);
        const auto target = image.find("Target")->address;
        const auto text = image.info.EntryPoint;
        Check(image.word(text) == (0x3c080000 | (target >> 16)), "signed HI16 carry");
        Check(image.word(text + 8) == (0x3c090000 | ((target + 65536) >> 16)), "shared LO16 pairing");
        Check((image.word(text + 4) & 65535) == ((target - 32768) & 65535), "signed LO16");
        Check(image.word(text + 12) == (0x0c000000 | (text >> 2)), "jump relocation");
        Check(image.word(image.find("PCSX2FModule")->address + 16) == text, "entry pointer relocation");
        Check(image.word(image.find("PCSX2FModule")->address + 40) == target, "data pointer relocation");
        auto rela = fixture.file;
        const uint32_t explicitText[] = {0, (5 << 8) | 5, uint32_t(-32768), 8, (5 << 8) | 5, 32768,
            4, (5 << 8) | 6, uint32_t(-32768), 12, (4 << 8) | 4, 0};
        const uint32_t explicitData[] = {16, (4 << 8) | 2, 0, 40, (5 << 8) | 2, 0};
        for (unsigned i = 0; i < 12; ++i) Put(rela, fixture.textRel + i * 4, explicitText[i]);
        for (unsigned i = 0; i < 6; ++i) Put(rela, fixture.dataRel + i * 4, explicitData[i]);
        for (unsigned index : {7u, 8u})
        {
            Put(rela, fixture.headers + index * 40 + 4, 4);
            Put(rela, fixture.headers + index * 40 + 20, index == 7 ? 48 : 24);
            Put(rela, fixture.headers + index * 40 + 36, 12);
        }
        Image explicitImage;
        const bool explicitLoaded = Load(rela, ArenaBegin, ArenaEnd, explicitImage, error);
        Check(explicitLoaded, error);
        Check(explicitImage.bytes == image.bytes, "REL/RELA addend equivalence");
        auto weak = fixture.file;
        Put(weak, fixture.dataRel + 12, (7 << 8) | 2);
        Check(Load(weak, ArenaBegin, ArenaEnd, explicitImage, error), "undefined weak resolution");
        Check(explicitImage.word(explicitImage.find("PCSX2FModule")->address + 40) == 0, "weak pointer not zero");
        auto negativeJump = fixture.file;
        Put(negativeJump, fixture.text + 12, 0x0fffffff); // jal TestEntry - 4
        Check(Load(negativeJump, ArenaBegin, ArenaEnd, explicitImage, error), "negative REL jump addend");
        Check(explicitImage.word(explicitImage.info.EntryPoint + 12) == (0x0c000000 | ((explicitImage.info.EntryPoint - 4) >> 2)), "signed R_MIPS_26");
        Check(rebased.find("Target")->address - target == 0x18000, "automatic rebase");
        Check(rebased.info.Size == image.info.Size, "base-dependent size");
        Check(image.context % 16 == 0 && image.stackTop % 16 == 0 && image.heapEnd - image.heapBegin == 4096, "runtime layout");
        auto cpic = fixture.file;
        Put(cpic, 36, 4);
        Check(Load(cpic, ArenaBegin, ArenaEnd, explicitImage, error), "absolute SDK object CPIC flag");
        auto packedPointer = fixture.file;
        Put(packedPointer, fixture.dataRel + 8, 41);
        Check(Load(packedPointer, ArenaBegin, ArenaEnd, explicitImage, error), "packed R_MIPS_32 data pointer");
        Check(explicitImage.word(explicitImage.find("PCSX2FModule")->address + 41) == target, "packed data pointer relocation");
        auto unalignedInstruction = fixture.file;
        Put(unalignedInstruction, fixture.textRel + 24, 13);
        Reject(unalignedInstruction, "location");
        auto constructors = fixture.file;
        std::memcpy(constructors.data() + fixture.strings + 80, "__init_array_start", 18);
        std::memcpy(constructors.data() + fixture.strings + 110, "__init_array_end", 16);
        Put(constructors, fixture.headers + 5 * 40 + 20, 140);
        Put(constructors, fixture.headers + 4 * 40 + 20, 10 * 16);
        for (unsigned index : {8u, 9u}) {
            const size_t at = fixture.symbols + index * 16;
            Put(constructors, at, index == 8 ? 80 : 110);
            Put(constructors, at + 4, index == 8 ? 64 : 68);
            constructors[at + 12] = 0x10; Put16(constructors, at + 14, 2);
        }
        Put(constructors, fixture.headers + 8 * 40 + 20, 3 * 8);
        Put(constructors, fixture.dataRel + 16, 64);
        Put(constructors, fixture.dataRel + 20, (4 << 8) | 2);
        Check(Load(constructors, ArenaBegin, ArenaEnd, explicitImage, error), "relocated constructor array");
        auto invalidConstructor = constructors;
        Put(invalidConstructor, fixture.dataRel + 20, (5 << 8) | 2);
        Reject(invalidConstructor, "constructor/destructor target");
        invalidConstructor = constructors;
        Put(invalidConstructor, fixture.symbols + 9 * 16 + 4, 63);
        Reject(invalidConstructor, "constructor/destructor range");
        invalidConstructor = constructors;
        invalidConstructor[fixture.symbols + 9 * 16 + 12] = 0;
        Reject(invalidConstructor, "constructor/destructor range");
        Check(image.matchesCRC(image.info.CompatibleCRCListAddr, 4, 0x4f32a11f), "CRC lookup");
        Check(!image.matchesCRC(image.info.CompatibleCRCListAddr, 4, 1), "CRC mismatch");
        const auto* bss = image.find("ZeroBss");
        for (uint32_t i = 0; i < bss->size; ++i) Check(image.bytes[bss->address - image.info.Base + i] == 0, "BSS not zero");
        for (size_t n = 0; n < fixture.file.size(); ++n) Reject({fixture.file.begin(), fixture.file.begin() + n});
        auto mutate = [&](size_t at, uint32_t value, std::string expected)
        { auto bad = fixture.file; Put(bad, at, value); Reject(bad, expected); };
        mutate(fixture.headers + 40 + 16, UINT32_MAX, "section range");
        mutate(fixture.headers + 40 + 32, 3, "alignment");
        mutate(fixture.textRel + 4, (0xffffffu << 8) | 2, "symbol index");
        mutate(fixture.textRel, 62, "location");
        mutate(fixture.textRel + 4, (5 << 8) | 7, "unsupported MIPS relocation");
        mutate(fixture.textRel + 20, (5 << 8), "no matching LO16");
        mutate(fixture.data + 4, 2, "ABI");
        mutate(fixture.data + 20, 15, "stack/heap");
        mutate(fixture.symbols + 5 * 16 + 4, 510, "outside section");
        mutate(fixture.symbols + 7 * 16 + 12, 0x10, "unresolved runtime symbol");
        mutate(fixture.headers + 7 * 40 + 24, 3, "relocation table");
        mutate(36, 2, "non-PIC");
        mutate(16, 0x00080002, "update/rebuild");
        Reject(fixture.file, "arena", ArenaBegin - 128);
        Reject(fixture.file, "arena", ArenaBegin + 1);
        Reject(fixture.file, "stack/heap exceed", ArenaBegin, ArenaBegin + 4096);
        bool accept_modules = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string_view(argv[i]) == "--modules") { accept_modules = true; continue; }
            const std::filesystem::path path(argv[i]);
            std::ifstream file(path, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
            Check(!bytes.empty(), "missing test file: " + path.string());
            if (accept_modules || path.filename().string().starts_with("ModuleProbe"))
            {
                const bool loaded = Load(bytes, ArenaBegin, ArenaEnd, image, error);
                Check(loaded, path.string() + ": " + error);
                const bool moved = Load(bytes, ArenaBegin + 0x30000, ArenaEnd, rebased, error);
                Check(moved, path.string() + ": " + error);
                if (!accept_modules) Check(image.find("ProbeResult") && image.info.OSDTextSize == 255, "real module exports");
                Check(rebased.info.EntryPoint - image.info.EntryPoint == 0x30000, "real module rebase");
                Check(image.info.CompatibleCRCListSize >= 4, "module game compatibility list");
                std::cout << "Relocated twice: " << path.filename().string() << '\n';
            }
            else { Reject(bytes); std::cout << "Rejected obsolete/incomplete: " << path.filename().string() << '\n'; }
        }
        std::cout << "PASS: " << checks << " loader checks\n";
        return 0;
    }
    catch (const std::exception& exception) { std::cerr << "FAIL: " << exception.what() << '\n'; return 1; }
}
