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
    uint32_t Malloc;
    uint32_t Free;
};

struct PluginInfo PluginData[50] = { 0 };
uint32_t MallocReturnAddr = 0;

void init()
{
    asm("ei\n");

    uint32_t MaxBase = PluginData[0].Base;
    uint32_t MaxSize = PluginData[0].Size;
    for (size_t i = 0; i < sizeof(PluginData); i++)
    {
        if (PluginData[i].Base > MaxBase)
        {
            MaxBase = PluginData[i].Base;
            MaxSize = PluginData[i].Size;
        }

        if (PluginData[i].Base == 0)
            break;
    }

    void* (*malloc)(size_t size) = (void* (*)(size_t))PluginData[0].Malloc;
    size_t alloc_size = (MaxBase + MaxSize) - PluginData[0].Base;
    MallocReturnAddr = (uint32_t)malloc(alloc_size * 10); // without *10 crashes, idk why

    if (MallocReturnAddr <= PluginData[0].Base)
    {
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
}

int main()
{
    return 0;
}
