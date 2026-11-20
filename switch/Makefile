# The Familia for Nintendo Switch (libnx). Run from the repository root inside
# the devkitPro container:
#   make -f switch/Makefile -j$(nproc)
# Inputs: build/gen (tools/translate.py output). Outputs: build/switch/familia.{elf,nro}
DEVKITPRO ?= /opt/devkitpro
include $(DEVKITPRO)/libnx/switch_rules

GEN     ?= build/gen
OUT     := build/switch
PC      := third_party/pcrecomp
PORTLIBS := $(DEVKITPRO)/portlibs/switch
TARGET  := $(OUT)/familia

# Tegra X1: 4x Cortex-A57 (3 for applications), ARMv8.0-A with CRC + crypto.
ARCH    := -mcpu=cortex-a57+crc+crypto -mtp=soft -fPIE
OPT     ?= -O2
DEFS    := -D__SWITCH__ -DFAMILIA_SWITCH -D_GNU_SOURCE
INCS    := -Iswitch/compat -I$(PC)/recomp32 -I$(PC)/win32hle -Iruntime \
           -I$(DEVKITPRO)/libnx/include -I$(PORTLIBS)/include -I$(PORTLIBS)/include/SDL2
# No RECOMP_MEM_BASE: Horizon decides where the guest's 4 GB lives, so the
# lift adds the runtime g_mem_base (recomp_types.h).
CFLAGS  := $(ARCH) $(OPT) -g1 -ffunction-sections -fno-strict-aliasing -fwrapv $(DEFS) $(INCS)
GENFLAGS := $(CFLAGS) -w -DRECOMP_LOCAL_REGS -DRECOMP_NONVOLATILE_MEM -include switch/compat/gen_compat.h
# threadCreate is wrapped (nx_platform.c) to spread threads over the cores.
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,-Map,$(TARGET).map -Wl,--wrap=threadCreate
LIBS    := -L$(PORTLIBS)/lib -L$(DEVKITPRO)/libnx/lib \
           -lSDL2_ttf -lfreetype -lharfbuzz -lbz2 -lpng16 \
           -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau \
           -lavformat -lavcodec -lswscale -lswresample -lavutil -ldav1d \
           -lvorbisfile -lvorbis -logg -lz -lnx -lm

HLE_SRC := $(filter-out %_selftest.c,$(wildcard $(PC)/win32hle/*.c))
RT_SRC  := $(filter-out runtime/crash.c runtime/prof.c,$(wildcard runtime/*.c))
NX_SRC  := switch/nx_platform.c switch/nx_prof.c
GEN_SRC := $(wildcard $(GEN)/*/f_*.c) $(GEN)/dispatch.c $(GEN)/probes.c

obj = $(patsubst %.c,$(OUT)/%.o,$(1))
OBJS := $(call obj,$(HLE_SRC)) $(call obj,$(RT_SRC)) $(call obj,$(NX_SRC)) $(call obj,$(GEN_SRC))

.PHONY: all clean platform
all: $(TARGET).nro
platform: $(call obj,$(HLE_SRC)) $(call obj,$(RT_SRC)) $(call obj,$(NX_SRC))

# Mesa is C++: link with g++ for libstdc++
$(TARGET).elf: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS) -lstdc++

$(TARGET).nacp:
	nacptool --create "Mafia" "The Familia" "0.1.0" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	elf2nro $< $@ --nacp=$(TARGET).nacp --icon=switch/icon.jpg --romfsdir=switch/romfs

$(OUT)/$(GEN)/%.o: $(GEN)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -MMD -MP -I$(dir $<) -c $< -o $@

# runtime/main.c's main() becomes familia_main(); switch/nx_platform.c owns main()
$(OUT)/runtime/main.o: CFLAGS += -Dmain=familia_main
$(OUT)/runtime/movie.o: CFLAGS += -I$(PORTLIBS)/include

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -include switch/compat/nx_compat.h -Wall -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
