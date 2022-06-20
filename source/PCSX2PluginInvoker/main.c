#define PLUGIN_INVOKER
#include <stdio.h>
#include "../API/pcsx2f_api.h"

struct PluginInfo PluginData[PluginsMaxNum] = { };

void init()
{
    asm("ei\n");
    asm("addiu $ra, -4\n");

    struct PluginInfoInvoker* PluginDataInvoker = (struct PluginInfoInvoker*)(&PluginData);

    for (size_t i = 1; i < sizeof(PluginData); i++)
    {
        if (PluginDataInvoker[i].Base == 0)
            break;

        if (PluginDataInvoker[i].EntryPoint != 0)
        {
            void (*callee)() = (void(*)())PluginDataInvoker[i].EntryPoint;
            callee();
        }
    }
}

int main()
{
    return 0;
}
