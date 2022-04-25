EE_BIN = ../../data/PLUGINS/PCSX2PluginDummy.elf
EE_OBJS = main.o 

BASE_ADDRESS = 0x02100000
EE_LINKFILE = linkfile
EE_LIBS += -l:libc.a -l:libgcc.a
EE_LDFLAGS = -Wl,--entry=init -Wl,-Map,../../data/PLUGINS/PCSX2PluginDummy.map -nostdlib -nodefaultlibs -Wl,'--defsym=BASE_ADDRESS=$(BASE_ADDRESS)'

all: clean main-build

main-build: $(EE_BIN)

clean:
	rm -f $(EE_OBJS) $(EE_BIN)

PS2SDK = ../../external/ps2sdk/ps2sdk
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
