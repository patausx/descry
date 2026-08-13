#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

# host-only targets (`make tests`) build with the system g++ and must work on a
# machine without the 3DS toolchain - don't demand DEVKITARM for them.
HOST_ONLY_GOALS := tests demo
ifeq ($(filter-out $(HOST_ONLY_GOALS),$(or $(MAKECMDGOALS),all)),)
  HOST_ONLY := 1
endif

ifndef HOST_ONLY
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules
endif

#---------------------------------------------------------------------------------
TARGET      := descry
BUILD       := build
SOURCES     := platform/3ds \
               core/audio core/dsp core/synth core/sequencer core/ui core/ui/screens
INCLUDES    := core platform/3ds
APP_TITLE   := descry
APP_DESCRIPTION := m8-style tracker + synth for new3ds
APP_AUTHOR  := patausx
# NB: 3ds_rules' smdh recipe reads APP_ICON (not ICON!) - without the export
# smdhtool silently falls back to the devkitPro default icon
export APP_ICON := $(TOPDIR)/assets/icon.png

#---------------------------------------------------------------------------------
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS  += $(INCLUDE) -D__3DS__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS := $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES := $(addsuffix .o,$(BINFILES)) \
                 $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh

.PHONY: $(BUILD) clean all run send cia cci tests

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).cia $(TARGET).3ds

# wifi upload to the 3ds via netloader (press Y in the homebrew launcher)
# uses $THREEDS_IP when set, otherwise 3dslink searches via broadcast
run: $(BUILD)
	3dslink $(if $(THREEDS_IP),-a $(THREEDS_IP)) $(TARGET).3dsx

send: run

# --- cia / cci packaging (needs makerom + bannertool, see ~/tools/3ds) ---
MAKEROM    ?= makerom
BANNERTOOL ?= bannertool

$(BUILD)/$(TARGET).bnr: branding/final/banner_256x128_alpha.png branding/final/banner_audio.wav
	$(BANNERTOOL) makebanner -i branding/final/banner_256x128_alpha.png \
	  -a branding/final/banner_audio.wav -o $@

$(BUILD)/$(TARGET)_cia.smdh: assets/icon.png
	$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" \
	  -p "$(APP_AUTHOR)" -i assets/icon.png -o $@

cia: $(BUILD) $(BUILD)/$(TARGET).bnr $(BUILD)/$(TARGET)_cia.smdh
	$(MAKEROM) -f cia -o $(TARGET).cia -elf $(TARGET).elf -rsf $(TARGET).rsf \
	  -icon $(BUILD)/$(TARGET)_cia.smdh -banner $(BUILD)/$(TARGET).bnr -target t -exefslogo

cci: $(BUILD) $(BUILD)/$(TARGET).bnr $(BUILD)/$(TARGET)_cia.smdh
	$(MAKEROM) -f cci -o $(TARGET).3ds -elf $(TARGET).elf -rsf $(TARGET).rsf \
	  -icon $(BUILD)/$(TARGET)_cia.smdh -banner $(BUILD)/$(TARGET).bnr -target t -exefslogo

# --- host tests -------------------------------------------------------------
# Built with the SYSTEM g++, not devkitARM: these exercise pure fixed-point core
# logic (no libctru), so they run on the dev machine in a couple of seconds.
#   make tests
HOST_CXX  ?= g++
TEST_SRCS := $(wildcard tools/test_*.cpp)
TEST_CORE := core/synth/wavetable.cpp core/synth/sampler.cpp core/synth/wav_loader.cpp \
             core/synth/wavsynth.cpp core/synth/fm.cpp core/synth/dsn_synth.cpp \
             core/synth/mic_recorder.cpp core/synth/sample_utils.cpp \
             core/synth/drumkit.cpp core/synth/drum_gen.cpp core/audio/fixed.cpp \
             core/synth/fm_presets.cpp core/synth/wave_presets.cpp \
             core/synth/dsn_presets.cpp \
             core/audio/mixer.cpp core/sequencer/player.cpp \
             core/sequencer/undo.cpp core/sequencer/project.cpp \
             core/sequencer/phrase_gen.cpp
TEST_OUT  := build/hosttests

tests:
	@mkdir -p $(TEST_OUT)
	@fail=0; \
	for t in $(TEST_SRCS); do \
	  name=$$(basename $$t .cpp); \
	  if ! $(HOST_CXX) -std=c++17 -O1 -I. -o $(TEST_OUT)/$$name $$t $(TEST_CORE) \
	       > $(TEST_OUT)/$$name.log 2>&1; then \
	    echo "BUILD FAIL  $$name"; tail -5 $(TEST_OUT)/$$name.log; fail=1; continue; \
	  fi; \
	  if $(TEST_OUT)/$$name > $(TEST_OUT)/$$name.out 2>&1; then \
	    echo "pass  $$name"; \
	  else \
	    echo "FAIL  $$name"; tail -15 $(TEST_OUT)/$$name.out; fail=1; \
	  fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "host tests FAILED"; exit 1; fi; \
	echo "all host tests passed"

# --- demo content -----------------------------------------------------------
# Builds the shipped demo project(s) with the host compiler: a .tr3d project
# PLUS the .s16 samples it references (a .tr3d carries no audio - see
# docs/BUGS.md B3). Output lands in dist/demo/, ready to zip into a release or
# copy to sdmc:/3ds/descry/.
#   make demo
DEMO_OUT  := dist/demo
DEMO_CORE := core/sequencer/project.cpp core/sequencer/serialize.cpp \
             core/synth/wavsynth.cpp core/synth/sampler.cpp \
             core/synth/drumkit.cpp core/synth/fm.cpp \
             core/synth/dsn_synth.cpp core/synth/wavetable.cpp \
             core/synth/wav_loader.cpp core/synth/drum_gen.cpp \
             core/synth/dsn_presets.cpp core/synth/fm_presets.cpp \
             core/audio/fixed.cpp

demo:
	@mkdir -p $(DEMO_OUT) build
	@$(HOST_CXX) -std=c++17 -O2 -I. -o build/gen_jungle tools/gen_jungle.cpp $(DEMO_CORE)
	@./build/gen_jungle $(DEMO_OUT)
	@echo "demo content in $(DEMO_OUT)/"

.PHONY: tests demo

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3dsx : $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf  : $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
