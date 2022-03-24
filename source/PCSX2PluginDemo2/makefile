EE_BIN = ../../data/scripts/PLUGINS/C0498D24-SCDA-[SLUS-21356]/PCSX2PluginDemo2.elf
EE_OBJS = main.o 

BASE_ADDRESS = 0x1D87CD8
EE_LINKFILE = linkfile
#EE_LIBS += -l:libc.a -l:libps2sdkc.a -l:libkernel.a
EE_LDFLAGS = -Wl,--entry=init -Wl,-Map,../../data/scripts/PLUGINS/C0498D24-SCDA-[SLUS-21356]/PCSX2PluginDemo2.map -nostdlib -nodefaultlibs -Wl,'--defsym=BASE_ADDRESS=$(BASE_ADDRESS)'

all: $(EE_BIN)

PS2SDK = ../../external/ps2sdk/ps2sdk
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
