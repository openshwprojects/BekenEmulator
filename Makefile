# C++ BekenSimulator build. Classic C++ (gnu++98), no external libraries.
#   make            -> bekensim(.exe), the CLI
#   make tests      -> unittests(.exe) and self_test(.exe)
#   make tools      -> make_obk_config(.exe) and sim_ui(.exe)
#   make all        -> everything
#   make check      -> build + run the module unit tests
# Override CXX=... for a different compiler; the default is g++.

CXX ?= g++
CXXSTD ?= gnu++98
OPT ?= -O3
# Link-time optimization lets the compiler inline the interpreter's hot memory
# accessors in src/arch/arm/arm_cpu.cpp into the instruction executors
# in arm_exec.cpp/arm_thumb.cpp, which are in other translation units. Measured
# ~10-15% faster on the compute-bound ARM core, byte-identical output. Override
# with `make LTO=` to disable (e.g. for a faster debug build).
LTO ?= -flto=auto -ffat-lto-objects
CXXFLAGS ?= -std=$(CXXSTD) $(OPT) $(LTO) -Isrc -Itests -Wall -Wextra -Wno-unused-parameter
# -MMD -MP: every object also depends on the headers it includes, so a header
# that changes a struct layout rebuilds all of its users (no stale-ABI links).
DEPFLAGS := -MMD -MP
LDFLAGS ?=

# Detect Windows robustly: cmd.exe make sets OS=Windows_NT, but the msys2 `make`
# leaves $(OS) empty while `uname -s` reports MINGW64_NT-/MSYS_NT-.
ifeq ($(OS),Windows_NT)
    IS_WINDOWS := 1
endif
ifneq (,$(findstring NT,$(shell uname -s 2>/dev/null)))
    IS_WINDOWS := 1
endif

ifeq ($(IS_WINDOWS),1)
    EXE := .exe
    PLATFORM_LIBS := -lws2_32 -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 -luser32
    # sim_ui is a console-subsystem exe: --selftest prints to the console and
    # main() FreeConsole()s when it owns the console (double-clicked).
    GUI_LDFLAGS :=
    STATIC := -static -static-libgcc -static-libstdc++
else
    EXE :=
    PLATFORM_LIBS := -lpthread
    GUI_LDFLAGS :=
    STATIC :=
endif

BUILD := build

# GCC needs a writable temporary directory. The msys2 `make` wrapper clears the
# inherited TMP/TEMP (leaving GCC to fall back to an unwritable C:\windows on
# some setups), so point them at a build-local folder we know we can write.
export TMP := $(CURDIR)/$(BUILD)/tmp
export TEMP := $(CURDIR)/$(BUILD)/tmp
export TMPDIR := $(CURDIR)/$(BUILD)/tmp

# Chip implementations live in separate directories. The same ESP core is
# linked into front ends and the firmware acceptance runner.
ARM_SRC := \
    src/arch/arm/arm_cpu.cpp \
    src/arch/arm/arm_exec.cpp \
    src/arch/arm/arm_thumb.cpp \
    src/arch/arm/arm_thumb2.cpp

BEKEN_SRC := \
    src/beken/chips.cpp \
    src/beken/beken_crypto.cpp \
    src/beken/gpio.cpp \
    src/beken/periph.cpp \
    src/beken/tuya_config.cpp \
    src/beken/blecore.cpp \
    src/beken/wifimac.cpp \
    src/beken/emulator.cpp \
    src/beken/emulator_run.cpp \
    src/beken/emulator_mmio.cpp \
    src/beken/emulator_mmio2.cpp \
    src/beken/emulator_mmio3.cpp \
    src/beken/emulator_build.cpp \
    src/beken/beken_backend.cpp

ESP_SHARED_SRC := \
    src/esp/esp_image.cpp \
    src/esp/esp_machine.cpp \
    src/esp/esp_gpio.cpp \
    src/esp/esp_multicore.cpp \
    src/esp/esp_clocked_devices.cpp \
    src/esp/esp_reset.cpp \
    src/esp/esp_wifi.cpp \
    src/esp/esp_rf_math.cpp \
    src/esp/esp_frc_timers.cpp \
    src/esp/esp_rom.cpp \
    src/esp/esp_spi.cpp \
    src/esp/esp_uart.cpp

ESP32_SRC := \
    src/esp32/esp_eri.cpp \
    src/esp32/esp32_mmio.cpp \
    src/esp32/esp32_analog.cpp \
    src/esp32/esp32_phy.cpp \
    src/esp32/esp32_rng.cpp \
    src/esp32/esp32_cache.cpp \
    src/esp32/esp32_rtc_io.cpp \
    src/esp32/esp_aes.cpp \
    src/esp32/esp_psram.cpp \
    src/esp32/esp32_psram_mmu.cpp \
    src/esp32/esp32_interrupts.cpp

ESP8266_SRC := \
    src/esp8266/esp8266_mmio.cpp \
    src/esp8266/esp8266_timers.cpp \
    src/esp8266/esp8266_watchdog.cpp \
    src/esp8266/esp8266_wdev.cpp \
    src/esp8266/esp8266_mac_timers.cpp \
    src/esp8266/esp8266_wifi_dma.cpp \
    src/esp8266/esp8266_radio.cpp

ESP_CORE_SRC := \
    src/arch/xtensa/xtensa_cpu.cpp \
    src/rom_image.cpp \
    $(ESP_SHARED_SRC) \
    $(ESP32_SRC) \
    $(ESP8266_SRC)

ESP_CORE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(ESP_CORE_SRC))

LIB_SRC := \
    src/common.cpp \
    src/platform.cpp \
    src/crypto_prims.cpp \
    src/json.cpp \
    src/tuyamcu.cpp \
    src/wpa2.cpp \
    src/wifipeer.cpp \
    src/netstack.cpp \
    src/emulator_backend.cpp \
    src/cli.cpp \
    $(ARM_SRC) \
    $(BEKEN_SRC) \
    src/esp/esp_backend.cpp \
    $(ESP_CORE_SRC)

LIB_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(LIB_SRC))

UNIT_SRC := \
    tests/unittests.cpp \
    tests/beken/test_chips.cpp \
    tests/beken/test_timers.cpp \
    tests/beken/test_gpio_button.cpp \
    tests/beken/test_gpio_emulator.cpp \
    tests/beken/test_image_loader.cpp \
    tests/beken/test_watchdog.cpp \
    tests/test_backends.cpp \
    tests/esp/test_esp_image.cpp \
    tests/arch/xtensa/test_xtensa.cpp \
    tests/esp/test_esp_machine.cpp \
    tests/test_rom_image.cpp \
    tests/esp32/test_esp32_mmio.cpp \
    tests/esp/test_esp_timing.cpp \
    tests/esp/test_esp_clocked_devices.cpp \
    tests/esp8266/test_esp8266_radio.cpp \
    tests/esp8266/test_esp_wifi_acceptance.cpp \
    tests/esp/test_esp_spi.cpp \
    tests/esp32/test_esp_interrupts.cpp \
    tests/esp/test_esp_exceptions.cpp \
    tests/esp/test_esp_uart.cpp \
    tests/esp32/test_esp_eri.cpp \
    tests/esp32/test_esp32_rng.cpp \
    tests/esp32/test_esp_multicore.cpp \
    tests/esp/test_esp_resets.cpp \
    tests/esp32/test_esp_psram.cpp \
    tests/esp/test_esp_gpio.cpp \
    tests/esp32/test_esp_memory.cpp \
    tests/esp/test_esp_acceptance.cpp \
    tests/esp8266/test_esp8266_timers.cpp \
    tests/esp32/test_esp32_frc.cpp \
    tests/esp8266/test_esp8266_watchdog.cpp \
    tests/esp32/test_esp_aes.cpp \
    tests/esp32/test_esp_cache_faults.cpp \
    tests/esp8266/test_esp8266_wdev.cpp \
    tests/esp8266/test_esp8266_mac_timers.cpp \
    tests/esp32/test_esp_rtc_io.cpp \
    tests/esp8266/test_esp8266_wifi_dma.cpp \
    tests/esp8266/test_esp_wifi.cpp \
    tests/esp32/test_esp32_analog.cpp \
    tests/esp32/test_esp32_phy.cpp \
    tests/esp/test_esp_rf_math.cpp

UNIT_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(UNIT_SRC))

CLI := bekensim$(EXE)
UNIT := unittests$(EXE)
SELFTEST := self_test$(EXE)
MKOBK := make_obk_config$(EXE)
SIMUI := sim_ui$(EXE)
ESPTEST := esp_firmware$(EXE)

.PHONY: all cli tests tools check check-esp-dumps clean dirs

cli: dirs $(CLI)

all: cli tests tools

dirs:
	@mkdir -p $(BUILD)/tmp

# Mirror source paths in build/ so equal basenames in different backends
# cannot collide. One rule handles library, test and tool sources.
$(BUILD)/%.o: %.cpp | dirs
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(CLI): $(LIB_OBJ) $(BUILD)/src/main.o
	$(CXX) $(CXXFLAGS) $(STATIC) $^ -o $@ $(LDFLAGS) $(PLATFORM_LIBS)

$(UNIT): $(LIB_OBJ) $(UNIT_OBJ)
	$(CXX) $(CXXFLAGS) $(STATIC) $^ -o $@ $(LDFLAGS) $(PLATFORM_LIBS)

$(BUILD)/tests/testcase_data.o: tests/testcase_data.cpp | dirs
	@mkdir -p "$(@D)"
	$(CXX) $(CXXFLAGS) -fno-lto $(DEPFLAGS) -c $< -o $@

$(SELFTEST): $(LIB_OBJ) $(BUILD)/tests/self_test.o $(BUILD)/tests/testcase_data.o $(BUILD)/tests/report.o
	$(CXX) $(CXXFLAGS) -fno-lto $(STATIC) $^ -o $@ $(LDFLAGS) $(PLATFORM_LIBS)

$(MKOBK): $(LIB_OBJ) $(BUILD)/tools/make_obk_config.o
	$(CXX) $(CXXFLAGS) $(STATIC) $^ -o $@ $(LDFLAGS) $(PLATFORM_LIBS)

SIMUI_OBJ := $(BUILD)/tools/sim_ui.o $(BUILD)/tools/sim_ui_util.o $(BUILD)/tools/sim_ui_logic.o $(BUILD)/tools/sim_ui_selftest.o

$(SIMUI): $(LIB_OBJ) $(SIMUI_OBJ)
	$(CXX) $(CXXFLAGS) $(STATIC) $^ -o $@ $(LDFLAGS) $(GUI_LDFLAGS) $(PLATFORM_LIBS)

$(ESPTEST): $(BUILD)/src/common.o $(BUILD)/src/platform.o $(BUILD)/src/crypto_prims.o \
            $(ESP_CORE_OBJ) $(BUILD)/tests/esp/esp_firmware.o
	$(CXX) $(CXXFLAGS) $(STATIC) $^ -o $@ $(LDFLAGS) $(PLATFORM_LIBS)

tests: dirs $(UNIT) $(SELFTEST) $(ESPTEST)

ifeq ($(IS_WINDOWS),1)
tools: dirs $(MKOBK) $(SIMUI)
else
tools: dirs $(MKOBK)
endif

check: $(UNIT)
	./$(UNIT)

# Uses ESP_FLASH_DUMPS or ../FlashDumps. Missing or mismatched samples fail.
check-esp-dumps: $(ESPTEST)
	./$(ESPTEST)

# Include dependencies for the current graph only; old flat build artifacts
# are ignored after a directory move.
ALL_OBJ := $(sort $(LIB_OBJ) $(UNIT_OBJ) $(SIMUI_OBJ) \
    $(BUILD)/src/main.o $(BUILD)/tests/self_test.o $(BUILD)/tests/testcase_data.o \
    $(BUILD)/tests/report.o $(BUILD)/tests/esp/esp_firmware.o $(BUILD)/tools/make_obk_config.o)
-include $(ALL_OBJ:.o=.d)

clean:
	rm -rf $(BUILD) $(CLI) $(UNIT) $(SELFTEST) $(ESPTEST) $(MKOBK) $(SIMUI)
