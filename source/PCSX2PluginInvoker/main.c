#define PLUGIN_INVOKER
#include <stdio.h>
#include "../API/pcsx2f_api.h"

struct PluginInfo PluginData[PluginsMaxNum] = { };

void init()
{
    asm("ei\n");
    asm("addiu $ra, -4\n");

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
