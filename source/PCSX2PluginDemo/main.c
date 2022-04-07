#include <stdio.h>
#include <stdint.h>

int CompatibleCRCList[] = { 0x4F32A11F };
char PluginData[100] = { 0 };
char KeyboardState[256] = { 1 };

char GetAsyncKeyState(int vKey)
{
    return KeyboardState[vKey];
}

enum KeyCodes
{
    VK_KEY_A = 0x41,
    VK_KEY_D = 0x44,
    VK_KEY_S = 0x53,
    VK_KEY_W = 0x57,
};

int16_t sub_287470(struct CPad* pad)
{
    if (GetAsyncKeyState(VK_KEY_D))
        return 0xFF;
}

int16_t sub_287450(struct CPad* pad)
{
    if (GetAsyncKeyState(VK_KEY_A))
        return 0xFF;
}

int16_t sub_287430(struct CPad* pad)
{
    if (GetAsyncKeyState(VK_KEY_S))
        return 0xFF;
}

int16_t sub_287410(struct CPad* pad)
{
    if (GetAsyncKeyState(VK_KEY_W))
        return 0xFF;
}

void(*CCoronas__RegisterCoronaINT)(unsigned int id, unsigned char r, unsigned char g, unsigned char b, unsigned char a, void* pos, unsigned char coronaType, unsigned char flareType, unsigned char reflection, unsigned char LOScheck, unsigned char drawStreak, unsigned char flag4);
void __attribute__((optimize("O0"))) CCoronas__RegisterCoronaFLT(float radius, float farClip, float unk3, float unk4)
{
    (float)radius;
    (float)farClip;
    (float)unk3;
    (float)unk4;
}

void CCoronas__RegisterCorona(int id, char r, char g, char b, char a, void* pos, char coronaType, char flareType, float radius, float farClip, float unk3, float unk4, char reflection, char LOScheck, char drawStreak, char flag4)
{
    CCoronas__RegisterCoronaINT = (void*)0x27dd10;
    CCoronas__RegisterCoronaFLT(radius, farClip, unk3, unk4);
    CCoronas__RegisterCoronaINT(id, r, g, b, a, pos, coronaType, flareType, reflection, LOScheck, drawStreak, flag4);
}

float coords[3] = { -1618.71f, -129.48f, 13.82f };
void RenderCoronas()
{
    int id = 100;
    for (size_t i = 0; i < 5; i++)
    {
        for (size_t j = 0; j < 5; j++)
        {
            float vec[3] = { coords[0], coords[1] + (float)i, coords[2] + (float)j };
            CCoronas__RegisterCorona(id, 255, 0, 0, 255, &vec, 0, 0, -1.0f, 450.0f, 0.0f, 1.5f, 1, 0, 0, 0);
            id++;
        }
    }
}

void init()
{
    //jal, renders coronas
    *(int*)0x21ED38 = ((0x0C000000 | (((uint32_t)RenderCoronas & 0x0fffffff) >> 2)));
    //nop, skips intro
    *(int*)0x21C8F0 = 0;
    //jal, player movement with WASD
    *(int*)0x285764 = ((0x0C000000 | (((intptr_t)sub_287470 & 0x0fffffff) >> 2)));
    *(int*)0x285770 = ((0x0C000000 | (((intptr_t)sub_287450 & 0x0fffffff) >> 2)));
    *(int*)0x2858BC = ((0x0C000000 | (((intptr_t)sub_287430 & 0x0fffffff) >> 2)));
    *(int*)0x2858C8 = ((0x0C000000 | (((intptr_t)sub_287410 & 0x0fffffff) >> 2)));
}

int main()
{
    return 0;
}
