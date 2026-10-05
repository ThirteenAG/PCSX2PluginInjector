#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
extern const uint32_t CompatibleCRCList[] = {0x4F32A11F, 0xBEBF8793};
char OSDText[1][255];
volatile uint32_t ProbeResult[8];
}
static uint32_t constructor_count;
struct Constructed
{
    std::vector<uint32_t> values;
    std::string message;
    Constructed() : values{17, 29, 41}, message("C++ constructors, vector/string and private heap OK") { ++constructor_count; }
};
static Constructed constructed;
extern "C" void init()
{
    auto local = constructed.values;
    local.push_back(53);
    ProbeResult[0] = constructor_count;
    ProbeResult[1] = static_cast<uint32_t>(local.size());
    ProbeResult[2] = local[0] + local[1] + local[2] + local[3];
    auto* allocation = static_cast<unsigned char*>(std::calloc(32, 1));
    bool heap_ok = allocation && (reinterpret_cast<uintptr_t>(allocation) % 16 == 0);
    if (allocation) {
        for (size_t i = 0; i < 32; ++i) heap_ok = heap_ok && allocation[i] == 0;
        std::memset(allocation, 0x5a, 32);
        auto* larger = static_cast<unsigned char*>(std::realloc(allocation, 8192));
        if (larger) {
            for (size_t i = 0; i < 32; ++i) heap_ok = heap_ok && larger[i] == 0x5a;
            std::free(larger);
        } else { heap_ok = false; std::free(allocation); }
    }
    volatile size_t impossible = SIZE_MAX;
    heap_ok = heap_ok && !std::malloc(impossible) && !std::calloc(impossible, 2);
    auto* coalesced = std::malloc(128 * 1024);
    heap_ok = heap_ok && coalesced;
    std::free(coalesced);
    ProbeResult[3] = heap_ok;
    const bool valid = constructor_count == 1 && local.size() == 4 && ProbeResult[2] == 140 && heap_ok;
    const std::string message = valid ? constructed.message : "C++ module runtime FAILED";
    for (size_t i = 0; i < message.size() && i < 254; ++i) OSDText[0][i] = message[i];
}
