# Nubby GBA - build with devkitARM (devkitPro)

TARGET  := nubby-gba
BUILD   := build
SOURCES := $(wildcard source/*.c)
OBJECTS := $(SOURCES:source/%.c=$(BUILD)/%.o)

PREFIX  ?= $(DEVKITARM)/bin/arm-none-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
GBAFIX  ?= $(DEVKITPRO)/tools/bin/gbafix

ARCH    := -mthumb -mthumb-interwork -mcpu=arm7tdmi
CFLAGS  := $(ARCH) -O2 -Wall -Wextra -std=c99 -Iinclude
LDFLAGS := $(ARCH) -specs=gba.specs

.PHONY: all clean assets test

all: $(TARGET).gba

$(TARGET).gba: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -tNUBBYGBA

$(BUILD)/$(TARGET).elf: $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/%.o: source/%.c include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

# Regenerate source/assets.c and include/assets.h (needs python3)
assets:
	python3 tools/gen_assets.py

# Host-side tests for the game logic
test: | $(BUILD)
	cc -std=c99 -Wall -Wextra -Iinclude tests/test_game.c source/game.c -o $(BUILD)/test_game
	./$(BUILD)/test_game

clean:
	rm -rf $(BUILD) $(TARGET).gba
