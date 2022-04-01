#include <stdio.h>
#include <stdint.h>

int CompatibleCRCList[] = { 0x43341C03 };
char PluginData[100] = { 0 };

int(*load_hero_model)(int ptr);

int hook_load_hero_model(int ptr)
{
    int* gp = (int*)0x5EA000;
    int p_konquest_info = (*(gp + 209));

    *(char*)(p_konquest_info + 92) = 1;

    return load_hero_model(ptr);
}

void init()
{
    load_hero_model = (void*)0x303A90;
    // skip intro
    *(int*)0x1980F8 = 0;
    *(int*)0x197DF8 = 0;
    // make shujinko young
    *(int*)0x3039E0 = ((0x0C000000 | (((uint32_t)hook_load_hero_model & 0x0fffffff) >> 2))); //jal
    *(int*)0x3B16C4 = ((0x0C000000 | (((uint32_t)hook_load_hero_model & 0x0fffffff) >> 2))); //jal
}

int main()
{
    return 0;
}