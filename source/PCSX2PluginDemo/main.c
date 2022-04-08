#include <stdio.h>
#include <stdint.h>
#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS 0
#define NANOPRINTF_IMPLEMENTATION
#include "nanoprintf.h"

int CompatibleCRCList[] = { 0x4F32A11F };
char PluginData[100] = { 0 };
char KeyboardState[256] = { 1 };
char OSDText[10][255] = { 1 };

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

float __ln(float x);
float __log10(float x);
int __pow(int x, unsigned int y);
char* ftoa(float num, char* buf, size_t buf_size, int precision);

float* GetCamPos()
{
    return (float*)0x6F4F20;
}

float coords[3] = { -1618.71f, -129.48f, 13.82f };
void RenderCoronas()
{
    static char pos_x[10] = { 1 };
    static char pos_y[10] = { 1 };
    static char pos_z[10] = { 1 };
    ftoa(GetCamPos()[0], pos_x, sizeof(pos_x), 3);
    ftoa(GetCamPos()[1], pos_y, sizeof(pos_y), 3);
    ftoa(GetCamPos()[2], pos_z, sizeof(pos_z), 3);
    npf_snprintf(OSDText[0], 255, "Cam Pos: %s %s %s", pos_x, pos_y, pos_z);

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
    npf_snprintf(OSDText[1], 255, "This is test message");
    npf_snprintf(OSDText[2], 255, "This is formatted message: %d + %d = %d", 2, 2, 2 + 2);

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

float __ln(float x)
{
    float old_sum = 0.0;
    float xmlxpl = (x - 1) / (x + 1);
    float xmlxpl_2 = xmlxpl * xmlxpl;
    float denom = 1.0f;
    float frac = xmlxpl;
    float term = frac;
    float sum = term;

    while (sum != old_sum)
    {
        old_sum = sum;
        denom += 2.0f;
        frac *= xmlxpl_2;
        sum += frac / denom;
    }
    return 2.0f * sum;
}

float __log10(float x) {
    return __ln(x) / 2.3025850929940456840179914546844f;
}

int __pow(int x, unsigned int y)
{
    int temp;
    if (y == 0)
        return 1;

    temp = __pow(x, y / 2);
    if ((y % 2) == 0)
        return temp * temp;
    else
        return x * temp * temp;
}

char* ftoa(float num, char* buf, size_t buf_size, int precision)
{
    int sign = 1;
    if (num < 0.0f)
    {
        sign = -1;
        num *= -1.0f;
    }
    int whole_part = num;
    int digit = 0, reminder = 0;
    int log_value = __log10(num), index = log_value;
    long wt = 0;

    if (sign < 0)
        index++;
    for (int i = 1; i < log_value + 2; i++)
    {
        wt = __pow(10, i);
        reminder = whole_part % wt;
        digit = (reminder - digit) / (wt / 10);

        //Store digit in string
        buf[index--] = digit + 48;
        if (index == ((sign < 0) ? 0 : -1))
            break;
    }
    if (sign < 0)
        buf[index] = '-';

    index = log_value + ((sign < 0) ? 2 : 1);
    buf[index] = '.';

    float fraction_part = num - whole_part;
    float tmp1 = fraction_part, tmp = 0;

    for (int i = 1; i < precision; i++)
    {
        wt = 10;
        tmp = tmp1 * wt;
        digit = tmp;

        buf[++index] = digit + 48;
        tmp1 = tmp - digit;
    }

    return buf;
}
