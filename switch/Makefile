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
           -I$(DEVKITPRO)/libnx/include -I$(PORTLIBS)/include -I$(PORTLIBS)/include/SDL2 -I$(PORTLIBS)/include/opus
# No RECOMP_MEM_BASE: Horizon decides where the guest's 4 GB lives, so the
# lift adds the runtime g_mem_base (recomp_types.h). RECOMP_MEM_BASE_CONST
# declares it const (set once at start): GCC stops reloading it after every
# guest store. hidden.h: one static PIE, so globals are reached PC-relative
# instead of through the GOT. Together: -34% load instructions in the lift.
DEFS    += -DRECOMP_MEM_BASE_CONST
CFLAGS  := $(ARCH) $(OPT) -g1 -ffunction-sections -fno-strict-aliasing -fwrapv $(DEFS) $(INCS) \
           -include switch/compat/hidden.h
GENFLAGS := $(CFLAGS) -w -DRECOMP_LOCAL_REGS -DRECOMP_NONVOLATILE_MEM -include switch/compat/gen_compat.h

# Profile-guided optimisation (as upstream's Makefile):
#   make -f switch/Makefile -B PGO=gen   instrumented; the console/emulator
#        writes counters to sd:$(PGO_RUN_DIR) (config.ini GCOV_DUMP=<seconds>)
#   copy them to $(PGO_DIR), then make -f switch/Makefile -B PGO=use
# Same object paths in both builds: the counter files are named after them.
PGO ?=
PGO_RUN_DIR ?= /switch/mafia/pgo
PGO_DIR ?= build/pgo-switch
ifeq ($(PGO),gen)
CFLAGS   += -fprofile-generate=$(PGO_RUN_DIR) -fprofile-update=single -DFAMILIA_PGO_GEN
GENFLAGS += -fprofile-generate=$(PGO_RUN_DIR) -fprofile-update=single
LDPGO    := -fprofile-generate=$(PGO_RUN_DIR)
endif
ifeq ($(PGO),use)
PGOUSE   := -fprofile-use=$(abspath $(PGO_DIR)) -fprofile-partial-training -fprofile-correction \
            -Wno-missing-profile -Wno-coverage-mismatch
CFLAGS   += $(PGOUSE)
GENFLAGS += $(PGOUSE)
endif
# threadCreate is wrapped (nx_platform.c) to spread threads over the cores.
LDFLAGS := -specs=switch/familia.specs $(ARCH) -Wl,-Map,$(TARGET).map -Wl,--wrap=threadCreate
LIBS    := -L$(PORTLIBS)/lib -L$(DEVKITPRO)/libnx/lib \
           -lSDL2_ttf -lfreetype -lharfbuzz -lbz2 -lpng16 \
           -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau \
           -lavformat -lavcodec -lswscale -lswresample -lavutil -ldav1d \
           -lvorbisfile -lvorbis -lopusfile -lopus -logg -lz -lnx -lm

HLE_SRC := $(filter-out %_selftest.c,$(wildcard $(PC)/win32hle/*.c))
RT_SRC  := $(filter-out runtime/crash.c runtime/prof.c,$(wildcard runtime/*.c))
NX_SRC  := switch/nx_platform.c switch/nx_prof.c switch/nx_codeload.c switch/nx_textinput.c switch/nx_pad.c switch/nx_opusfs.c
GEN_SRC := $(wildcard $(GEN)/*/f_*.c) $(GEN)/dispatch.c $(GEN)/probes.c

obj = $(patsubst %.c,$(OUT)/%.o,$(1))
OBJS := $(call obj,$(HLE_SRC)) $(call obj,$(RT_SRC)) $(call obj,$(NX_SRC)) $(call obj,$(GEN_SRC))

.PHONY: all clean platform
# The NSO is built from familia_nso.elf (the module without .gentext); the
# translated game code goes into the NSP's RomFS as gentext.bin and is mapped
# at run time (switch/familia.ld, switch/nx_codeload.c).
all: $(OUT)/familia_nso.elf $(OUT)/gentext.bin
platform: $(call obj,$(HLE_SRC)) $(call obj,$(RT_SRC)) $(call obj,$(NX_SRC))

# Mesa is C++: link with g++ for libstdc++
$(TARGET).elf: $(OBJS) switch/familia.ld switch/familia.specs
	$(CXX) $(LDFLAGS) $(LDPGO) -o $@ $(OBJS) $(LIBS) -lstdc++

$(OUT)/gentext.bin: $(TARGET).elf
	$(PREFIX)objcopy -O binary --only-section=.gentext $< $@

$(OUT)/familia_nso.elf: $(TARGET).elf
	$(PREFIX)objcopy --remove-section=.gentext $< $@

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
