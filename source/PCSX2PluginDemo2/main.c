#include <stdio.h>

void __attribute__((naked)) fun()
{
    //added code
    asm("li.s    $f31, 3.5\n");
    asm("mul.s   $f12, $f12, $f31\n");

    //original code
    asm("swc1    $f12, 0($a0)\n");
    asm("move    $v0, $a0\n");
    asm("swc1    $f13, 4($a0)\n");
    asm("swc1    $f14, 8($a0)\n");
    asm("swc1    $f15, 0xC($a0)\n");
    asm("jr      $ra\n");
    asm("nop\n");
}

void init()
{
    *(int*)0x25F5A4 = ((0x0C000000 | (((unsigned int)fun & 0x0fffffff) >> 2))); //jal
}

int main()
{
    return 0;
}
