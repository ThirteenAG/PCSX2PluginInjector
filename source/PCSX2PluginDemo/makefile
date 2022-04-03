EE_BIN = ../../data/PLUGINS/GTAVCS/PCSX2PluginDemo.elf
EE_OBJS = main.o 

BASE_ADDRESS = 0x02020000
EE_LINKFILE = linkfile
#EE_LIBS += -l:libc.a -l:libps2sdkc.a -l:libkernel.a
EE_LDFLAGS = -Wl,--entry=init -Wl,-Map,../../data/PLUGINS/GTAVCS/PCSX2PluginDemo.map -nostdlib -nodefaultlibs -Wl,'--defsym=BASE_ADDRESS=$(BASE_ADDRESS)'

all: $(EE_BIN)

PS2SDK = ../../external/ps2sdk/ps2sdk
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
