#include <stdio.h>
#include <stdint.h>

struct PluginInfo
{
    uint32_t Base;
    uint32_t EntryPoint;
    uint32_t SegmentFileOffset;
    uint32_t Size;
    uint32_t DataAddr;
    uint32_t DataSize;
    uint32_t PCSX2DataAddr;
    uint32_t PCSX2DataSize;
    uint32_t CompatibleCRCListAddr;
    uint32_t CompatibleCRCListSize;
    uint32_t PatternDataAddr;
    uint32_t PatternDataSize;
    uint32_t KeyboardStateAddr;
    uint32_t KeyboardStateSize;
};

struct PluginInfo PluginData[100] = { 0xFFFFFFFF }; //needs to be initialized

void init()
{
    asm("ei\n");

    for (size_t i = 1; i < sizeof(PluginData); i++)
    {
        if (PluginData[i].Base == 0)
            break;

        if (PluginData[i].EntryPoint != 0)
        {
            void (*callee)() = (void(*)())PluginData[i].EntryPoint;
            callee();
        }
    }
}

int main()
{
    return 0;
}
