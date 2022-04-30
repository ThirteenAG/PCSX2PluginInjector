function writemakefile(prj_name, scripts_addr, base, ...)
   local args = {...}
   local files = "main.o";
   for i, v in ipairs( args ) do
       files = files .. " " .. v:match("(.+)%..+") .. ".o"
   end
   files = string.gsub(files, "^%s*(.-)%s*$", "%1")
   file = io.open("source/" .. prj_name .. "/makefile", "w")
   if (file) then
str = [[
EE_BIN = ../../data/%s.elf
EE_OBJS = %s 

BASE_ADDRESS = %s
EE_LINKFILE = linkfile
EE_LIBS += -l:libc.a -l:libgcc.a
EE_LDFLAGS = -Wl,--entry=init -Wl,-Map,../../data/%s.map -nostdlib -nodefaultlibs -Wl,'--defsym=BASE_ADDRESS=$(BASE_ADDRESS)'

all: clean main-build

main-build: $(EE_BIN)

clean:
	rm -f $(EE_OBJS) $(EE_BIN)

PS2SDK = ../../external/ps2sdk/ps2sdk
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
]]
      file:write(string.format(str, scripts_addr .. prj_name, files, base, scripts_addr .. prj_name))
      file:close()
   end
end

function writelinkfile(prj_name)
   file = io.open("source/" .. prj_name .. "/linkfile", "w")
   if (file) then
str = [[
ENTRY(__start);

PHDRS {
  text PT_LOAD;
}

SECTIONS {
    .text BASE_ADDRESS : {
        _ftext = . ;
        *(.text)
        *(.text.*)
        *(.gnu.linkonce.t*)
        KEEP(*(.init))
        KEEP(*(.fini))
        QUAD(0)
    } :text

    PROVIDE(_etext = .);
    PROVIDE(etext = .);

    .reginfo : { *(.reginfo) }

    /* Global/static constructors and deconstructors. */
    .ctors ALIGN(16): {
        KEEP(*crtbegin*.o(.ctors))
        KEEP(*(EXCLUDE_FILE(*crtend*.o) .ctors))
        KEEP(*(SORT(.ctors.*)))
        KEEP(*(.ctors))
    }
    .dtors ALIGN(16): {
        KEEP(*crtbegin*.o(.dtors))
        KEEP(*(EXCLUDE_FILE(*crtend*.o) .dtors))
        KEEP(*(SORT(.dtors.*)))
        KEEP(*(.dtors))
    }

    /* Static data.  */
    .rodata ALIGN(128): {
        *(.rodata)
        *(.rodata.*)
        *(.gnu.linkonce.r*)
    }

    .data ALIGN(128): {
        _fdata = . ;
        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d*)
        SORT(CONSTRUCTORS)
    }

    .rdata ALIGN(128): { *(.rdata) }
    .gcc_except_table ALIGN(128): { *(.gcc_except_table) }

    _gp = ALIGN(128) + 0x7ff0;
    .lit4 ALIGN(128): { *(.lit4) }
    .lit8 ALIGN(128): { *(.lit8) }

    .sdata ALIGN(128): {
        *(.sdata)
        *(.sdata.*)
        *(.gnu.linkonce.s*)
    }

    _edata = .;
    PROVIDE(edata = .);

    /* Uninitialized data.  */
    .sbss ALIGN(128) : {
        _fbss = . ;
        *(.sbss)
        *(.sbss.*)
        *(.gnu.linkonce.sb*)
        *(.scommon)
    }

    .bss ALIGN(128) : {
        *(.bss)
        *(.bss.*)
        *(.gnu.linkonce.b*)
        *(COMMON)
    }
    _end_bss = .;

    _end = . ;
    PROVIDE(end = .);

    /* Symbols needed by crt0.s.  */
    PROVIDE(_heap_size = -1);
    PROVIDE(_stack = -1);
    PROVIDE(_stack_size = 128 * 1024);
}
]]
      file:write(str)
      file:close()
   end
end