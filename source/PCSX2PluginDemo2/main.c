#include <stdio.h>
#include "../API/pcsx2f_api.h"

int CompatibleCRCList[] = { 0xC0498D24 };
char ElfPattern[] = "10 00 BF FF 00 00 B0 7F 30 00 A4 AF 40 00 A5 AF";
int PCSX2Data[PCSX2Data_Size] = { 1 };

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
    int DesktopSizeX = PCSX2Data[PCSX2Data_DesktopSizeX];
    int DesktopSizeY = PCSX2Data[PCSX2Data_DesktopSizeY];
    int WindowSizeX = PCSX2Data[PCSX2Data_WindowSizeX];
    int WindowSizeY = PCSX2Data[PCSX2Data_WindowSizeY];
    int IsFullscreen = PCSX2Data[PCSX2Data_IsFullscreen];
    int AspectRatioSetting = PCSX2Data[PCSX2Data_AspectRatioSetting];

    if (IsFullscreen || !WindowSizeX || !WindowSizeY)
    {
        WindowSizeX = DesktopSizeX;
        WindowSizeY = DesktopSizeY;
    }

    switch (AspectRatioSetting)
    {
    case RAuto4_3_3_2: //not implemented
        //if (GSgetDisplayMode() == GSVideoMode::SDTV_480P)
        //    AspectRatio = 3.0f / 2.0f;
        //else
        AspectRatio = 4.0f / 3.0f;
        break;
    case R4_3:
        AspectRatio = 4.0f / 3.0f;
        break;
    case R16_9:
        AspectRatio = 16.0f / 9.0f;
        break;
    case Stretch:
    default:
        AspectRatio = (float)WindowSizeX / (float)WindowSizeY;
        break;
    }

    float intResX = 640.0f;
    float intResY = 480.0f;

    Scale = (((intResX / intResY)) / (AspectRatio));

    *(int*)0x25F5A4 = ((0x0C000000 | (((unsigned int)fun & 0x0fffffff) >> 2))); //jal
}

int main()
{
    return 0;
}
