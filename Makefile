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
# accessors (mem_load/mem_store in arm_cpu.cpp) into the instruction executors
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

# Library objects shared by every executable (every src/*.cpp except main.cpp).
LIB_SRC := \
    src/common.cpp \
    src/platform.cpp \
    src/chips.cpp \
    src/crypto_prims.cpp \
    src/json.cpp \
    src/beken_crypto.cpp \
    src/arm_cpu.cpp \
    src/arm_exec.cpp \
    src/arm_thumb.cpp \
    src/arm_thumb2.cpp \
    src/gpio.cpp \
    src/periph.cpp \
    src/tuya_config.cpp \
    src/tuyamcu.cpp \
    src/blecore.cpp \
    src/wifimac.cpp \
    src/wpa2.cpp \
    src/wifipeer.cpp \
    src/netstack.cpp \
    src/emulator.cpp \
    src/emulator_run.cpp \
    src/emulator_mmio.cpp \
    src/emulator_mmio2.cpp \
    src/emulator_mmio3.cpp \
    src/emulator_build.cpp \
    src/cli.cpp

LIB_OBJ := $(patsubst src/%.cpp,$(BUILD)/%.o,$(LIB_SRC))

CLI := bekensim$(EXE)
UNIT := unittests$(EXE)
SELFTEST := self_test$(EXE)
MKOBK := make_obk_config$(EXE)
SIMUI := sim_ui$(EXE)

.PHONY: all cli tests tools check clean dirs

cli: dirs $(CLI)

all: cli tests tools

dirs:
	@mkdir -p $(BUILD)/tmp

$(BUILD)/%.o: src/%.cpp | dirs
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/%.o: tests/%.cpp | dirs
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/%.o: tools/%.cpp | dirs
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(CLI): $(LIB_OBJ) $(BUILD)/main.o
	$(CXX) $(CXXFLAGS) $(STATIC) $^ -o $@ $(LDFLAGS) $(PLATFORM_LIBS)

$(UNIT): $(LIB_OBJ) $(BUILD)/unittests.o $(BUILD)/test_chips.o $(BUILD)/test_timers.o \
        $(BUILD)/test_gpio_button.o $(BUILD)/test_gpio_emulator.o $(BUILD)/test_image_loader.o \
        $(BUILD)/test_watchdog.o
	$(CXX) $(CXXFLAGS) $(STATIC) $^ -o $@ $(LDFLAGS) $(PLATFORM_LIBS)

$(BUILD)/testcase_data.o: tests/testcase_data.cpp | dirs
	$(CXX) $(CXXFLAGS) -fno-lto $(DEPFLAGS) -c $< -o $@

$(SELFTEST): $(LIB_OBJ) $(BUILD)/self_test.o $(BUILD)/testcase_data.o $(BUILD)/report.o
	$(CXX) $(CXXFLAGS) -fno-lto $(STATIC) $^ -o $@ $(LDFLAGS) $(PLATFORM_LIBS)

$(MKOBK): $(LIB_OBJ) $(BUILD)/make_obk_config.o
	$(CXX) $(CXXFLAGS) $(STATIC) $^ -o $@ $(LDFLAGS) $(PLATFORM_LIBS)

SIMUI_OBJ := $(BUILD)/sim_ui.o $(BUILD)/sim_ui_util.o $(BUILD)/sim_ui_logic.o $(BUILD)/sim_ui_selftest.o

$(SIMUI): $(LIB_OBJ) $(SIMUI_OBJ)
	$(CXX) $(CXXFLAGS) $(STATIC) $^ -o $@ $(LDFLAGS) $(GUI_LDFLAGS) $(PLATFORM_LIBS)

tests: dirs $(UNIT) $(SELFTEST)

ifeq ($(IS_WINDOWS),1)
tools: dirs $(MKOBK) $(SIMUI)
else
tools: dirs $(MKOBK)
endif

check: $(UNIT)
	./$(UNIT)

-include $(wildcard $(BUILD)/*.d)

clean:
	rm -rf $(BUILD) $(CLI) $(UNIT) $(SELFTEST) $(MKOBK) $(SIMUI)
