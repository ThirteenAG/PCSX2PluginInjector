#include <stdio.h>
#include <stdint.h>

char PluginData[100] = { 0 };

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
    *(int*)0x21ED38 = ((0x0C000000 | (((uint32_t)RenderCoronas & 0x0fffffff) >> 2))); //jal
    *(int*)0x21C8F0 = 0; //nop, skips intro
}

int main()
{
    return 0;
}
