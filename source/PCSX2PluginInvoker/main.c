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

struct PluginInfo PluginData[50] = { 0xFFFFFFFF }; //needs to be initialized

void init()
{
    asm("ei\n");

    uint32_t MinBase = 0xFFFFFFFF;
    uint32_t MaxBase = 0;
    uint32_t MaxSize = 0;
    for (size_t i = 1; i < sizeof(PluginData); i++)
    {
        if (PluginData[i].Base == 0)
            break;

        if (PluginData[i].Base > MaxBase)
        {
            MaxBase = PluginData[i].Base;
            MaxSize = PluginData[i].Size;
        }

        if (PluginData[i].Base < MinBase)
        {
            MinBase = PluginData[i].Base;
        }
    }

    if (MinBase == 0xFFFFFFFF)
        return;

    if (PluginData[0].Malloc != 0)
    {
        void* (*malloc)(size_t size) = (void* (*)(size_t))PluginData[0].Malloc;
        size_t alloc_size = (MaxBase + MaxSize) - MinBase;
        uint32_t MallocReturnAddr = (uint32_t)malloc(alloc_size);
        //*(int*)0x100008 = MallocReturnAddr;

        if (MallocReturnAddr > MinBase)
            return;
    }

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
