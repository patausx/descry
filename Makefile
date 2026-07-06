#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

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

.PHONY: $(BUILD) clean all run send cia cci

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

$(BUILD)/$(TARGET).bnr: branding/final/banner_256x128.png branding/final/banner_audio.wav
	$(BANNERTOOL) makebanner -i branding/final/banner_256x128.png \
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
