#include <stdio.h>

int CompatibleCRCList[] = { 0xC0498D24 };
char ElfPattern[] = "10 00 BF FF 00 00 B0 7F 30 00 A4 AF 40 00 A5 AF";
char PCSX2Data[20] = { 1 };

enum AspectRatioType
{
    Stretch,
    R4_3,
    R16_9,
    MaxCount
};
float AspectRatio = 0.0f;
float Scale = 0.0f;

void __attribute__((naked)) fun()
{
    asm volatile ("" :: "r"(Scale));
    asm volatile ("mtc1    $v0, $f31\n");
    asm volatile ("mul.s   $f12, $f12, $f31\n");

    //original code
    asm volatile ("swc1    $f12, 0($a0)\n");
    asm volatile ("move    $v0, $a0\n");
    asm volatile ("swc1    $f13, 4($a0)\n");
    asm volatile ("swc1    $f14, 8($a0)\n");
    asm volatile ("swc1    $f15, 0xC($a0)\n");
    asm volatile ("jr      $ra\n");
    asm volatile ("nop\n");
}

void init()
{
    int DesktopSizeX = *(uint32_t*)((uintptr_t)&PCSX2Data + (sizeof(uint32_t) * 0));
    int DesktopSizeY = *(uint32_t*)((uintptr_t)&PCSX2Data + (sizeof(uint32_t) * 1));
    int WindowSizeX = *(uint32_t*)((uintptr_t)&PCSX2Data + (sizeof(uint32_t) * 2));
    int WindowSizeY = *(uint32_t*)((uintptr_t)&PCSX2Data + (sizeof(uint32_t) * 3));
    int IsFullscreen = *(uint32_t*)((uintptr_t)&PCSX2Data + (sizeof(uint32_t) * 4));
    int AspectRatioSetting = *(uint32_t*)((uintptr_t)&PCSX2Data + (sizeof(uint32_t) * 5));

    if (IsFullscreen || !WindowSizeX || !WindowSizeY)
    {
        WindowSizeX = DesktopSizeX;
        WindowSizeY = DesktopSizeY;
    }

    if (AspectRatioSetting == Stretch)
        AspectRatio = (float)WindowSizeX / (float)WindowSizeY;
    else if (AspectRatioSetting == R4_3)
        AspectRatio = 4.0f / 3.0f;
    else if (AspectRatioSetting == R16_9)
        AspectRatio = 16.0f / 9.0f;

    float intResX = 640.0f;
    float intResY = 480.0f;

    Scale = (((intResX / intResY)) / (AspectRatio));

    *(int*)0x25F5A4 = ((0x0C000000 | (((unsigned int)fun & 0x0fffffff) >> 2))); //jal
}

int main()
{
    return 0;
}
