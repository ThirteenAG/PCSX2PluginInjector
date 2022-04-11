EE_BIN = ../../data/PLUGINS/PCSX2PluginInvoker.elf
EE_OBJS = main.o 

BASE_ADDRESS = 0x02000000
EE_LINKFILE = linkfile
EE_LIBS += -l:libc.a -l:libgcc.a
EE_LDFLAGS = -Wl,--entry=init -Wl,-Map,../../data/PLUGINS/PCSX2PluginInvoker.map -nostdlib -nodefaultlibs -Wl,'--defsym=BASE_ADDRESS=$(BASE_ADDRESS)'

all: $(EE_BIN)

PS2SDK = ../../external/ps2sdk/ps2sdk
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
