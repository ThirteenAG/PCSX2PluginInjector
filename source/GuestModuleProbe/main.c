#include "guest_module.h"
#include <stdint.h>

#ifndef PROBE_ID
#define PROBE_ID 1
#endif

/* Enable only on the two pilot titles. These modules deliberately have the
 * same source/layout and NO compiled base address. */
const uint32_t CompatibleCRCList[] = {0x4F32A11F, 0xBEBF8793};
char OSDText[1][255];
volatile uint32_t ProbeResult[8];
static volatile uint32_t initialized = 0x12348000;
static volatile uint32_t zero_initialized[16];
static volatile uint32_t* volatile relocated_pointer = &initialized;

__attribute__((noinline)) static uint32_t relocated_call(uint32_t value)
{
    return value ^ 0x55AA55AA;
}
static void hex(char* text, uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < 8; ++i) text[i] = digits[(value >> ((7 - i) * 4)) & 15];
}
static void entry(const void* raw_context)
{
    const struct PCSX2FModuleContext* context = raw_context;
    volatile uint32_t stack_value = PROBE_ID;
    uint32_t gp;
    __asm__ volatile("move %0, $gp" : "=r"(gp));
    ProbeResult[0] = PROBE_ID;
    ProbeResult[1] = context->module_base;
    ProbeResult[2] = (uint32_t)&stack_value;
    ProbeResult[3] = zero_initialized[0];
    ProbeResult[4] = relocated_call(*relocated_pointer);
    ProbeResult[5] = context->game_gp;
    ProbeResult[6] = gp;
    ProbeResult[7] = context->version;
    const char message[] = "Module ? OK: base 0x????????, stack 0x????????";
    for (unsigned i = 0; i < sizeof(message); ++i) OSDText[0][i] = message[i];
    OSDText[0][7] = '0' + PROBE_ID;
    hex(OSDText[0] + 20, context->module_base);
    hex(OSDText[0] + 38, (uint32_t)&stack_value);
    volatile uint32_t* heap_word = (volatile uint32_t*)context->heap_begin;
    const uint32_t heap_was_zero = *heap_word;
    *heap_word = PROBE_ID;
    if (ProbeResult[3] || heap_was_zero || ProbeResult[4] != (0x12348000 ^ 0x55AA55AA))
        OSDText[0][9] = '!';
}
PCSX2F_MODULE(entry, 64 * 1024, 4096);
