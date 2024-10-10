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
            void (*ps2sdk_libcpp_init)() = (void(*)())PluginData[i].ps2sdk_libcpp_init;
            void (*__cxa_atexit)() = (void(*)())PluginData[i].__cxa_atexit;

            if (__cxa_atexit)
            {
                // used after invoking ps2sdk_libcpp_init
                // returning here because the code uses gp register, which won't have valid value
                *(int*)(__cxa_atexit + 0) = 0x3E00008; // jr ra
                *(int*)(__cxa_atexit + 4) = 0x0000000; // nop
            }

            if (ps2sdk_libcpp_init)
                ps2sdk_libcpp_init();

            if (callee)
                callee();
        }
    }
}

int main()
{
    return 0;
}
