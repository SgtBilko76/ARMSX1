.ONESHELL:
.SHELLFLAGS := -ec

WASM_TARGET := $(filter wasm,$(MAKECMDGOALS))

ifeq ($(WASM_TARGET),wasm)
	CC := emcc
	CXX := em++
	SDL_STATIC := 0
	SDL_CFLAGS := -sUSE_SDL=2
	SDL_LIBS_DYNAMIC :=
	SDL_LIBS_STATIC :=
	WASM_LDFLAGS := -sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1 -sFORCE_FILESYSTEM=1 -sFULL_ES3=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sNO_EXIT_RUNTIME=0 -sEXPORTED_RUNTIME_METHODS='["FS","ccall","cwrap"]' -sEXPORTED_FUNCTIONS='["_main","_psxe_run","_external_main","_psxe_wasm_on_file","_psxe_wasm_on_error","_psxe_enqueue_launch_argument"]' --preload-file icons@/icons
endif

SDL_CONFIG ?= sdl2-config

CC ?= gcc
CXX ?= g++
AR := /usr/bin/ar
RANLIB := /usr/bin/ranlib

SDL_CFLAGS ?= $(shell $(SDL_CONFIG) --cflags 2>/dev/null)
SDL_LIBS_DYNAMIC ?= $(shell $(SDL_CONFIG) --libs 2>/dev/null)
SDL_LIBS_STATIC ?= $(shell $(SDL_CONFIG) --static-libs 2>/dev/null || $(SDL_CONFIG) --libs --static 2>/dev/null)
SDL_STATIC ?= 1
WASM_LDFLAGS ?=
USE_CHD ?= 1
HW_DEBUG ?= 0

PLATFORM := $(shell uname -s)
ifeq ($(WASM_TARGET),wasm)
PLATFORM := Emscripten
endif
IOS_TARGET ?= 0
IOS_SDK ?= iphoneos
IOS_DEPLOYMENT_TARGET ?= 14.0
MACOS_DEPLOYMENT_TARGET ?= 10.15
WINDOWS_TARGET ?= 0
UWP_TARGET ?= 0
PSVITA_TARGET ?= 0
VITASDK ?=
IOS_SDL_FRAMEWORK ?= $(CURDIR)/ios/Frameworks/SDL2.xcframework/ios-arm64/SDL2.framework
IOS_SDL_FRAMEWORK_PARENT := $(dir $(IOS_SDL_FRAMEWORK))
SDKROOT ?=
CONTROLLER_GENERIC ?= 0
FSUI_DIR := third_party/fuse-lib
FSUI_BUILD_DIR ?= build/fsui/native
WASM_HTML_POSTPROCESS := web/postprocess_web_html.cmake
FSUI_LINK_SYSTEM_GL ?= 1

ifeq ($(PSVITA_TARGET),1)
	ifeq ($(strip $(VITASDK)),)
$(error VITASDK must be set when PSVITA_TARGET=1)
	endif
	SDL_CONFIG := $(VITASDK)/bin/arm-vita-eabi-pkg-config
	CC := $(VITASDK)/bin/arm-vita-eabi-gcc
	CXX := $(VITASDK)/bin/arm-vita-eabi-g++
	AR := $(VITASDK)/bin/arm-vita-eabi-ar
	RANLIB := $(VITASDK)/bin/arm-vita-eabi-ranlib
	PLATFORM := Vita
	SDL_STATIC := 1
endif

ifeq ($(IOS_TARGET),1)
	SDKROOT ?= $(shell xcrun --sdk $(IOS_SDK) --show-sdk-path)
	CC ?= $(shell xcrun --sdk $(IOS_SDK) --find clang)
	CXX ?= $(shell xcrun --sdk $(IOS_SDK) --find clang++)

	# Force dynamic SDL when targeting iOS
	SDL_STATIC := 0
	SDL_CFLAGS := -isysroot $(SDKROOT) -arch arm64 -miphoneos-version-min=$(IOS_DEPLOYMENT_TARGET) \
		-F$(IOS_SDL_FRAMEWORK_PARENT) -I$(IOS_SDL_FRAMEWORK)/Headers -DIOS_TARGET -D_THREAD_SAFE
	SDL_LIBS_DYNAMIC := -isysroot $(SDKROOT) -arch arm64 -miphoneos-version-min=$(IOS_DEPLOYMENT_TARGET) \
		-F$(IOS_SDL_FRAMEWORK_PARENT) -framework SDL2
	SDL_LIBS_STATIC :=
endif

BASE_CFLAGS = -g -DLOG_USE_COLOR -I"." -I"psx" $(SDL_CFLAGS)
BASE_CFLAGS += -O3 -ffast-math -Wno-overflow -Wall -pedantic -Wno-address-of-packed-member -flto

FSUI_INCLUDE_FLAGS = \
	-I$(FSUI_DIR)/include \
	-I$(FSUI_DIR)/third_party/imgui \
	-I$(FSUI_DIR)/third_party/imgui/backends \
	-I$(FSUI_DIR)/third_party/stb

ifneq ($(WASM_TARGET),wasm)
FSUI_INCLUDE_FLAGS += -I$(FSUI_BUILD_DIR)/gladsources/fsui_glad/include
endif

FSUI_COMPILE_DEFS = -DIMGUI_FRONTEND -DFSUI_HAS_SDL2_PLATFORM -DFSUI_HAS_SDL2SURFACE_RENDERER -DFSUI_HAS_SDL2RENDERER_RENDERER -DSDL_MAIN_HANDLED

LIBCHDR_DIR := third_party/libchdr
LIBCHDR_BUILD_DIR :=
LIBCHDR_INCLUDE_FLAGS :=
LIBCHDR_INPUTS :=
LIBCHDR_ARCHIVE :=
LIBCHDR_LIBS :=
LIBCHDR_CONFIGURE :=
LIBCHDR_CMAKE_ARGS :=
LIBCHDR_CMAKE_TOOLCHAIN_FILE ?=
LIBCHDR_ANDROID_ABI ?=
LIBCHDR_ANDROID_PLATFORM ?=
LIBCHDR_ANDROID_FLEXIBLE_PAGE_SIZES ?=
CHD_COMPILE_DEFS :=
CHD_BUILD_DEPS :=
CHD_LINK_LIBS :=
ifeq ($(USE_CHD),1)
	ifeq ($(WASM_TARGET),wasm)
		LIBCHDR_BUILD_DIR := build/libchdr/wasm
	else ifeq ($(PSVITA_TARGET),1)
		LIBCHDR_BUILD_DIR := build/libchdr/psvita
	else ifeq ($(IOS_TARGET),1)
		LIBCHDR_BUILD_DIR := build/libchdr/ios
	else
		LIBCHDR_BUILD_DIR := build/libchdr/native
	endif

	LIBCHDR_INCLUDE_FLAGS := -I$(LIBCHDR_DIR)/include -I$(LIBCHDR_DIR)/deps/miniz-3.1.1
	LIBCHDR_INPUTS := $(shell find $(LIBCHDR_DIR) -type f 2>/dev/null)
	LIBCHDR_ARCHIVE := $(LIBCHDR_BUILD_DIR)/libchdr-static.a
	LIBCHDR_LIBS := \
		$(LIBCHDR_ARCHIVE) \
		$(LIBCHDR_BUILD_DIR)/deps/lzma-25.01/libchdr-lzma.a \
		$(LIBCHDR_BUILD_DIR)/deps/miniz-3.1.1/libminiz.a \
		$(LIBCHDR_BUILD_DIR)/deps/zstd-1.5.7/libzstd.a
	LIBCHDR_CONFIGURE := cmake
	ifeq ($(WASM_TARGET),wasm)
		LIBCHDR_CONFIGURE := emcmake cmake
	endif

	LIBCHDR_CMAKE_ARGS := -S $(LIBCHDR_DIR) -B $(LIBCHDR_BUILD_DIR) -DBUILD_SHARED_LIBS=OFF -DCHDR_WANT_RAW_DATA_SECTOR=ON -DCHDR_WANT_SUBCODE=ON -DMINIZ_ARCHIVE_APIS=ON -DMINIZ_STDIO=ON -DCMAKE_BUILD_TYPE=Release
	ifneq ($(strip $(LIBCHDR_CMAKE_TOOLCHAIN_FILE)),)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_TOOLCHAIN_FILE=$(LIBCHDR_CMAKE_TOOLCHAIN_FILE)
	endif
	ifneq ($(strip $(LIBCHDR_ANDROID_ABI)),)
		LIBCHDR_CMAKE_ARGS += -DANDROID_ABI=$(LIBCHDR_ANDROID_ABI)
	endif
	ifneq ($(strip $(LIBCHDR_ANDROID_PLATFORM)),)
		LIBCHDR_CMAKE_ARGS += -DANDROID_PLATFORM=$(LIBCHDR_ANDROID_PLATFORM)
	endif
	ifneq ($(strip $(LIBCHDR_ANDROID_FLEXIBLE_PAGE_SIZES)),)
		LIBCHDR_CMAKE_ARGS += -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=$(LIBCHDR_ANDROID_FLEXIBLE_PAGE_SIZES)
	endif
	ifneq ($(strip $(CC)),)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_C_COMPILER=$(CC)
	endif
	ifneq ($(strip $(CXX)),)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_CXX_COMPILER=$(CXX)
	endif
	ifneq ($(strip $(AR)),)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_AR=$(AR)
	endif
	ifneq ($(strip $(RANLIB)),)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_RANLIB=$(RANLIB)
	endif
	ifeq ($(PLATFORM),Darwin)
	ifneq ($(IOS_TARGET),1)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_OSX_DEPLOYMENT_TARGET=$(MACOS_DEPLOYMENT_TARGET)
	endif
	endif
	ifeq ($(IOS_TARGET),1)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$(IOS_DEPLOYMENT_TARGET) -DCMAKE_OSX_SYSROOT=$(SDKROOT)
	endif

	CHD_COMPILE_DEFS := -DUSE_CHD
	CHD_BUILD_DEPS := $(LIBCHDR_ARCHIVE)
	CHD_LINK_LIBS := $(LIBCHDR_LIBS)
endif

BASE_CXXFLAGS = -std=c++20 $(BASE_CFLAGS) $(FSUI_INCLUDE_FLAGS) $(FSUI_COMPILE_DEFS)
# Quest VR mode (frontend/vr). Headers only: the loader is dlopen'ed on Android, and every other
# platform compiles inert stubs.
BASE_CXXFLAGS += -Ithird_party/openxr/include
BASE_CFLAGS += $(LIBCHDR_INCLUDE_FLAGS) $(CHD_COMPILE_DEFS)
BASE_CXXFLAGS += $(LIBCHDR_INCLUDE_FLAGS) $(CHD_COMPILE_DEFS)

ifeq ($(CONTROLLER_GENERIC),1)
	BASE_CFLAGS += -DCONTROLLER_GENERIC
	BASE_CXXFLAGS += -DCONTROLLER_GENERIC
endif

BASE_CFLAGS += -DUSE_HARDWARE
BASE_CXXFLAGS += -DUSE_HARDWARE

ifeq ($(HW_DEBUG),1)
	BASE_CFLAGS += -DHW_DEBUG
	BASE_CXXFLAGS += -DHW_DEBUG
endif

ifeq ($(UWP_TARGET),1)
		BASE_CFLAGS += -DUWP_TARGET
		BASE_CXXFLAGS += -DUWP_TARGET
endif

ifeq ($(PSVITA_TARGET),1)
	BASE_CFLAGS += -DPSVITA_TARGET -D__DLL_BUILD -I$(VITASDK)/arm-vita-eabi/include -I$(VITASDK)/arm-vita-eabi/include/SDL2 -marm
	BASE_CXXFLAGS += -DPSVITA_TARGET -D__DLL_BUILD -I$(VITASDK)/arm-vita-eabi/include -I$(VITASDK)/arm-vita-eabi/include/SDL2 -marm
endif

ifeq ($(WASM_TARGET),wasm)
		BASE_CFLAGS := $(filter-out -flto,$(BASE_CFLAGS))
		BASE_CXXFLAGS := $(filter-out -flto,$(BASE_CXXFLAGS))
endif

ifeq ($(PSVITA_TARGET),1)
	BASE_CFLAGS := $(filter-out -flto,$(BASE_CFLAGS))
	BASE_CXXFLAGS := $(filter-out -flto,$(BASE_CXXFLAGS))
endif

ifeq ($(IOS_TARGET),1)
	BASE_CFLAGS += -fembed-bitcode
	BASE_CXXFLAGS += -fembed-bitcode
endif

ifeq ($(WASM_TARGET),wasm)
OS_INFO := Emscripten
else ifeq ($(PSVITA_TARGET),1)
OS_INFO := PS Vita
else ifneq ($(IOS_TARGET),1)
OS_INFO := $(shell uname -rmo)
else
OS_INFO := iOS
endif

ifeq ($(PLATFORM),Darwin)
ifneq ($(IOS_TARGET),1)
	BASE_CFLAGS += -mmacosx-version-min=$(MACOS_DEPLOYMENT_TARGET) -Wno-newline-eof
	BASE_CXXFLAGS += -mmacosx-version-min=$(MACOS_DEPLOYMENT_TARGET)
endif
endif

SHARED_EXT := .so
SHARED_LDFLAGS := -shared
SHARED_CFLAGS := $(BASE_CFLAGS) -D__DLL_BUILD -fPIC
SHARED_CXXFLAGS := $(BASE_CXXFLAGS) -D__DLL_BUILD -fPIC

ifeq ($(PLATFORM),Darwin)
	SHARED_EXT := .dylib
	SHARED_LDFLAGS := -dynamiclib
else ifeq ($(WINDOWS_TARGET),1)
	SHARED_EXT := .dll
endif

ifeq ($(IOS_TARGET),1)
	SHARED_LDFLAGS += -Wl,-install_name,@rpath/libarmsx$(SHARED_EXT)
	SHARED_CFLAGS += -DIOS_TARGET
	SHARED_CXXFLAGS += -DIOS_TARGET
endif

VERSION_TAG := $(shell git describe --always --tags --abbrev=0)
COMMIT_HASH := $(shell git rev-parse --short HEAD)

BIN_DIR := bin
ifeq ($(WASM_TARGET),wasm)
BIN_DIR := bin/wasm
else ifeq ($(PSVITA_TARGET),1)
BIN_DIR := bin/psvita
endif
OBJ_DIR := $(BIN_DIR)/obj

BIN      := $(BIN_DIR)/armsx
ifeq ($(WASM_TARGET),wasm)
BIN      := $(BIN_DIR)/armsx.html
else ifeq ($(WINDOWS_TARGET),1)
BIN      := $(BIN_DIR)/armsx.exe
endif
SHARED_BIN := $(BIN_DIR)/libarmsx$(SHARED_EXT)
VITA_NATIVE_LIB := $(BIN_DIR)/libarmsx_vita.a
RUNTIME_ICON_SRC := icons/FsuiAppIcon.png
RUNTIME_ICON_DEST := $(BIN_DIR)/icons/FsuiAppIcon.png
WINDRES ?= windres

C_SOURCES := $(wildcard psx/*.c) \
             $(wildcard psx/dev/*.c) \
             $(wildcard psx/dev/cdrom/*.c) \
             $(wildcard psx/input/*.c) \
             $(wildcard psx/disc/*.c) \
             frontend/argparse.c \
             frontend/config.c \
             frontend/diagnostics.c \
             frontend/platform_file.c \
             frontend/toml.c
C_SOURCES := $(filter-out psx/dev/cdrom/chd.c,$(C_SOURCES))
C_SOURCES += frontend/gpu_hw.c
ifeq ($(USE_CHD),1)
C_SOURCES += psx/dev/cdrom/chd.c
endif
ifeq ($(PSVITA_TARGET),1)
C_SOURCES += frontend/vita_sdl_stubs.c
endif
C_SOURCES_SHARED := $(C_SOURCES)

CPP_SOURCES := frontend/archive.cpp frontend/main.cpp frontend/vr/vr_openxr.cpp

FSUI_LIBS := \
	$(FSUI_BUILD_DIR)/libfsui-donor.a \
	$(FSUI_BUILD_DIR)/libfsui-backend-sdl.a \
	$(FSUI_BUILD_DIR)/libfsui-platform-sdl2.a \
	$(FSUI_BUILD_DIR)/libfsui-renderer-sdl2.a \
	$(FSUI_BUILD_DIR)/libfsui-renderer-sdl2surface.a \
	$(FSUI_BUILD_DIR)/libfsui-core.a \
	$(FSUI_BUILD_DIR)/libfsui_imgui.a \
	$(FSUI_BUILD_DIR)/libfsui_resources.a
ifeq ($(WASM_TARGET),wasm)
FSUI_LIBS += \
	$(FSUI_BUILD_DIR)/libfsui-renderer-opengl.a
else
FSUI_LIBS += \
	$(FSUI_BUILD_DIR)/libfsui-renderer-opengl.a \
	$(FSUI_BUILD_DIR)/libfsui_glad.a
endif

PLATFORM_EXTRA_LDFLAGS :=
PLATFORM_EXTRA_LIBS :=

ifeq ($(PLATFORM),Darwin)
	FSUI_LIBS += $(FSUI_BUILD_DIR)/libfsui-renderer-metal.a
	ifeq ($(IOS_TARGET),1)
		PLATFORM_EXTRA_LIBS += -framework Foundation -framework Metal -framework OpenGLES -framework QuartzCore -framework UIKit
	else
		PLATFORM_EXTRA_LDFLAGS += -mmacosx-version-min=$(MACOS_DEPLOYMENT_TARGET)
		PLATFORM_EXTRA_LIBS += -framework Cocoa -framework Foundation -framework IOKit -framework Metal -framework OpenGL -framework QuartzCore
	endif
else ifeq ($(WINDOWS_TARGET),1)
	PLATFORM_EXTRA_LIBS += -lsetupapi -limm32 -lversion -lwinmm -lgdi32 -lole32 -loleaut32 -lshell32 -luuid -lopengl32
ifneq ($(UWP_TARGET),1)
	PLATFORM_EXTRA_LIBS += -ldbghelp
endif
else ifneq ($(WASM_TARGET),wasm)
	PLATFORM_EXTRA_LIBS += -ldl
ifeq ($(FSUI_LINK_SYSTEM_GL),1)
	PLATFORM_EXTRA_LIBS += -lGL
endif
endif

C_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(C_SOURCES))
C_OBJS_SHARED := $(patsubst %.c,$(OBJ_DIR)/%.o,$(C_SOURCES_SHARED))
CPP_OBJS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(CPP_SOURCES))
RC_SOURCES :=
ifeq ($(WINDOWS_TARGET),1)
RC_SOURCES += resources/windows/armsx.rc
endif
RC_OBJS := $(patsubst %.rc,$(OBJ_DIR)/%.o,$(RC_SOURCES))
ALL_OBJS := $(C_OBJS) $(CPP_OBJS) $(RC_OBJS)
ALL_OBJS_SHARED := $(C_OBJS_SHARED) $(CPP_OBJS)

# Force dynamic SDL when building the shared library
ifneq (,$(filter shared,$(MAKECMDGOALS)))
override SDL_STATIC := 0
BASE_CFLAGS += -D__DLL_BUILD -fPIC
BASE_CXXFLAGS += -D__DLL_BUILD -fPIC
endif

ifeq ($(SDL_STATIC),1)
	ifeq ($(SDL_LIBS_STATIC),)
$(warning Static SDL2 libraries not found; falling back to dynamic SDL2)
		SDL_STATIC := 0
	endif
endif

SDL_LIBS := $(if $(filter 1,$(SDL_STATIC)),$(SDL_LIBS_STATIC),$(SDL_LIBS_DYNAMIC))
SDL_LIBS_SHARED := $(SDL_LIBS_DYNAMIC)

.PHONY: all clean shared wasm psvita-lib test test-cpu test-gpu test-audio test-sdl-audio test-chd test-zip test-vr test-sdl-runtime disc-probe frame-hash

all: $(BIN)

shared: $(SHARED_BIN)

wasm: $(BIN)

psvita-lib: $(VITA_NATIVE_LIB)

TEST_CORE_SOURCES := $(wildcard psx/*.c) \
                     $(wildcard psx/dev/*.c) \
                     $(wildcard psx/dev/cdrom/*.c) \
                     $(wildcard psx/input/*.c)
TEST_CORE_SOURCES := $(filter-out psx/dev/cdrom/chd.c,$(TEST_CORE_SOURCES))
TEST_CPU_BIN := build/tests/cpu_differential
TEST_GPU_BIN := build/tests/gpu_renderer_parity
TEST_AUDIO_BIN := build/tests/audio_timing
TEST_SDL_AUDIO_BIN := build/tests/sdl_audio_queue_smoke
TEST_CHD_BIN := build/tests/chd_logic
TEST_ZIP_BIN := build/tests/zip_integration
TEST_SDL_BIN := build/tests/sdl_renderer_smoke
TEST_VR_BIN := build/tests/quest_vr_logic
DISC_PROBE_BIN := build/tests/disc_probe
TEST_PLATFORM_FILE_OBJ := build/tests/platform_file.o

$(TEST_PLATFORM_FILE_OBJ): frontend/platform_file.c frontend/platform_file.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -I. -c frontend/platform_file.c -o $@

$(TEST_CPU_BIN): tests/cpu_differential.c $(TEST_CORE_SOURCES) $(TEST_PLATFORM_FILE_OBJ)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $^ -lm -o $@

test-cpu: $(TEST_CPU_BIN)
	./$(TEST_CPU_BIN)

$(TEST_GPU_BIN): tests/gpu_renderer_parity.c psx/dev/gpu.c psx/dev/depth.c frontend/gpu_hw.c
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_HARDWARE -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(SDL_CFLAGS) $^ -lm -o $@

test-gpu: $(TEST_GPU_BIN)
	./$(TEST_GPU_BIN)

$(TEST_VR_BIN): tests/quest_vr_logic_test.cpp frontend/vr/quest_pad_mapper.h frontend/vr/screen_geometry.h frontend/vr/stereo_reproject.h
	mkdir -p $(dir $@)
	$(CXX) -std=c++20 -O2 -g -Wall -Wextra -I. $< -o $@

test-vr: $(TEST_VR_BIN)
	./$(TEST_VR_BIN)

$(TEST_AUDIO_BIN): tests/audio_timing.c
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -I. -Ipsx $< -lm -o $@

test-audio: $(TEST_AUDIO_BIN)
	./$(TEST_AUDIO_BIN)

$(TEST_SDL_AUDIO_BIN): tests/sdl_audio_queue_smoke.c
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g $(SDL_CFLAGS) $< $(SDL_LIBS) -o $@

test-sdl-audio: $(TEST_SDL_AUDIO_BIN)
	./$(TEST_SDL_AUDIO_BIN)

$(TEST_CHD_BIN): tests/chd_logic.c $(TEST_PLATFORM_FILE_OBJ) $(CHD_BUILD_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_CHD -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(LIBCHDR_INCLUDE_FLAGS) $< $(TEST_PLATFORM_FILE_OBJ) $(CHD_LINK_LIBS) -lm -o $@

test-chd: $(TEST_CHD_BIN)
	./$(TEST_CHD_BIN)

$(TEST_ZIP_BIN): tests/zip_integration.cpp frontend/archive.cpp $(TEST_PLATFORM_FILE_OBJ) $(CHD_BUILD_DEPS)
	mkdir -p $(dir $@)
	$(CXX) -std=c++20 -O0 -DUSE_CHD -I. $(LIBCHDR_INCLUDE_FLAGS) \
		tests/zip_integration.cpp frontend/archive.cpp $(TEST_PLATFORM_FILE_OBJ) $(CHD_LINK_LIBS) -o $@

test-zip: $(TEST_ZIP_BIN)
	python3 tests/validate_zip.py ./$(TEST_ZIP_BIN)

$(TEST_SDL_BIN): tests/sdl_renderer_smoke.c
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g $(SDL_CFLAGS) $< $(SDL_LIBS) -o $@

test-sdl-runtime: $(TEST_SDL_BIN)
	./$(TEST_SDL_BIN)

$(DISC_PROBE_BIN): tests/disc_probe.c psx/dev/cdrom/disc.c psx/dev/cdrom/cue.c psx/dev/cdrom/list.c psx/dev/cdrom/chd.c $(CHD_BUILD_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_CHD -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(LIBCHDR_INCLUDE_FLAGS) \
		tests/disc_probe.c psx/dev/cdrom/disc.c psx/dev/cdrom/cue.c psx/dev/cdrom/list.c psx/dev/cdrom/chd.c \
		$(CHD_LINK_LIBS) -lm -o $@

disc-probe: $(DISC_PROBE_BIN)

# Determinism and speed probe for core changes (tests/frame_hash.c). Needs a BIOS and a disc, so
# it is not part of the validation matrix. Same optimisation flags as the app build.
FRAME_HASH_BIN := build/tests/frame_hash
FRAMES ?= 1800
$(FRAME_HASH_BIN): tests/frame_hash.c $(TEST_CORE_SOURCES) $(TEST_PLATFORM_FILE_OBJ)
	mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O3 -ffast-math -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $^ -lm -o $@

frame-hash: $(FRAME_HASH_BIN)
	./$(FRAME_HASH_BIN) "$(BIOS)" "$(IMAGE)" $(FRAMES)

test:
	python3 tests/run_validation.py

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(BASE_CFLAGS) \
		-DOS_INFO="$(OS_INFO)" \
		-DREP_VERSION="$(VERSION_TAG)" \
		-DREP_COMMIT_HASH="$(COMMIT_HASH)"

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(BASE_CXXFLAGS) \
		-DOS_INFO="$(OS_INFO)" \
		-DREP_VERSION="$(VERSION_TAG)" \
		-DREP_COMMIT_HASH="$(COMMIT_HASH)"

$(OBJ_DIR)/%.o: %.rc | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(WINDRES) -I. $< $@

ifeq ($(USE_CHD),1)
$(LIBCHDR_ARCHIVE): $(LIBCHDR_INPUTS)
	mkdir -p $(LIBCHDR_BUILD_DIR)
	$(LIBCHDR_CONFIGURE) $(LIBCHDR_CMAKE_ARGS)
	cmake --build $(LIBCHDR_BUILD_DIR) -j$(BUILD_JOBS)
endif

$(RUNTIME_ICON_DEST): $(RUNTIME_ICON_SRC) | $(BIN_DIR)
	mkdir -p $(dir $@)
	cp $< $@

$(BIN): $(ALL_OBJS) $(RUNTIME_ICON_DEST) $(CHD_BUILD_DEPS) | $(BIN_DIR)
	$(CXX) $(ALL_OBJS) $(CHD_LINK_LIBS) $(FSUI_LIBS) -o $(BIN) $(SDL_LIBS) $(PLATFORM_EXTRA_LDFLAGS) $(PLATFORM_EXTRA_LIBS) $(WASM_LDFLAGS)
ifeq ($(WASM_TARGET),wasm)
	@if [ ! -f "$(BIN)" ]; then \
		echo "WASM build did not produce $(BIN)"; \
		exit 1; \
	fi
	cmake -DINPUT_FILE="$(BIN)" -P "$(WASM_HTML_POSTPROCESS)"
endif

$(VITA_NATIVE_LIB): $(ALL_OBJS) $(RUNTIME_ICON_DEST) | $(BIN_DIR)
	$(AR) rcs $@ $(ALL_OBJS)
	$(RANLIB) $@

$(SHARED_BIN): $(ALL_OBJS_SHARED) $(CHD_BUILD_DEPS) | $(BIN_DIR)
	$(CXX) $(SHARED_LDFLAGS) $(ALL_OBJS_SHARED) $(CHD_LINK_LIBS) $(FSUI_LIBS) -o $(SHARED_BIN) $(SDL_LIBS_SHARED) $(PLATFORM_EXTRA_LDFLAGS) $(PLATFORM_EXTRA_LIBS)

clean:
	rm -rf "$(BIN_DIR)"
