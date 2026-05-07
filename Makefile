# ykern build system — thin wrapper around cmake + ninja, mirroring yetty's
# target naming so muscle memory carries over.

# Pin to system tools so the build uses system gcc + system libc + system
# library headers consistently. Mixing nix gcc with system libyaml (or any
# system .so) drags two glibcs into one process and triggers spurious
# __stack_chk_fail aborts during startup.
SYSTEM_PATH := /usr/bin:/bin:/usr/local/bin:$(PATH)

BUILD_DIR_DESKTOP_YTRACE_RELEASE := build-desktop-ytrace-release
BUILD_DIR_DESKTOP_YTRACE_DEBUG   := build-desktop-ytrace-debug
BUILD_DIR_DESKTOP_YTRACE_ASAN    := build-desktop-ytrace-asan

PARALLEL_JOBS ?=
CMAKE_PARALLEL := $(if $(PARALLEL_JOBS),--parallel $(PARALLEL_JOBS),--parallel)

# Build acceleration — same convention as yetty
USE_DISTCC ?= 0
USE_CCACHE ?= 0
DISTCC_HOSTS ?= localhost 192.168.1.10
export DISTCC_HOSTS

ifeq ($(USE_DISTCC),1)
ifneq ($(shell which distcc 2>/dev/null),)
export CCACHE_PREFIX := distcc
endif
LAUNCHER := -DCMAKE_C_COMPILER_LAUNCHER=ccache
else ifeq ($(USE_CCACHE),1)
LAUNCHER := -DCMAKE_C_COMPILER_LAUNCHER=ccache
else
LAUNCHER :=
endif

CMAKE        := cmake
GENERATOR    := -G Ninja
CMAKE_RELEASE := -DCMAKE_BUILD_TYPE=Release
CMAKE_DEBUG   := -DCMAKE_BUILD_TYPE=Debug
CMAKE_ASAN    := -DCMAKE_BUILD_TYPE=Debug \
                 -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
                 -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"

.PHONY: all
all: help

#=============================================================================
# Desktop — ytrace release (daily driver)
#=============================================================================

.PHONY: config-desktop-ytrace-release
config-desktop-ytrace-release: ## Configure desktop ytrace release build
	PATH="$(SYSTEM_PATH)" $(CMAKE) -B $(BUILD_DIR_DESKTOP_YTRACE_RELEASE) $(GENERATOR) $(CMAKE_RELEASE) $(LAUNCHER)
	@ln -sfn $(BUILD_DIR_DESKTOP_YTRACE_RELEASE)/compile_commands.json compile_commands.json

.PHONY: build-desktop-ytrace-release
build-desktop-ytrace-release: ## Build desktop ytrace release (daily driver)
	@if [ ! -f "$(BUILD_DIR_DESKTOP_YTRACE_RELEASE)/build.ninja" ]; then $(MAKE) config-desktop-ytrace-release; fi
	PATH="$(SYSTEM_PATH)" $(CMAKE) --build $(BUILD_DIR_DESKTOP_YTRACE_RELEASE) $(CMAKE_PARALLEL)

.PHONY: run-desktop-ytrace-release
run-desktop-ytrace-release: build-desktop-ytrace-release ## Build and run the smoke walker
	./$(BUILD_DIR_DESKTOP_YTRACE_RELEASE)/tests/walk

.PHONY: ykern
ykern: build-desktop-ytrace-release ## Build and run the ykern CLI (use ARGS="..." to pass args)
	./$(BUILD_DIR_DESKTOP_YTRACE_RELEASE)/ykern $(ARGS)

#=============================================================================
# Desktop — ytrace debug
#=============================================================================

.PHONY: config-desktop-ytrace-debug
config-desktop-ytrace-debug: ## Configure desktop ytrace debug build
	PATH="$(SYSTEM_PATH)" $(CMAKE) -B $(BUILD_DIR_DESKTOP_YTRACE_DEBUG) $(GENERATOR) $(CMAKE_DEBUG) $(LAUNCHER)
	@ln -sfn $(BUILD_DIR_DESKTOP_YTRACE_DEBUG)/compile_commands.json compile_commands.json

.PHONY: build-desktop-ytrace-debug
build-desktop-ytrace-debug: ## Build desktop ytrace debug
	@if [ ! -f "$(BUILD_DIR_DESKTOP_YTRACE_DEBUG)/build.ninja" ]; then $(MAKE) config-desktop-ytrace-debug; fi
	PATH="$(SYSTEM_PATH)" $(CMAKE) --build $(BUILD_DIR_DESKTOP_YTRACE_DEBUG) $(CMAKE_PARALLEL)

#=============================================================================
# Desktop — ytrace asan
#=============================================================================

.PHONY: config-desktop-ytrace-asan
config-desktop-ytrace-asan: ## Configure desktop ytrace ASAN build
	PATH="$(SYSTEM_PATH)" $(CMAKE) -B $(BUILD_DIR_DESKTOP_YTRACE_ASAN) $(GENERATOR) $(CMAKE_ASAN) $(LAUNCHER)

.PHONY: build-desktop-ytrace-asan
build-desktop-ytrace-asan: ## Build desktop ytrace ASAN
	@if [ ! -f "$(BUILD_DIR_DESKTOP_YTRACE_ASAN)/build.ninja" ]; then $(MAKE) config-desktop-ytrace-asan; fi
	PATH="$(SYSTEM_PATH)" $(CMAKE) --build $(BUILD_DIR_DESKTOP_YTRACE_ASAN) $(CMAKE_PARALLEL)

#=============================================================================
# Cleanup + help
#=============================================================================

#=============================================================================
# Metadata regeneration — extracts cmd-id ↔ name mappings from kernel
# headers via libclang (Python wheel) and writes them to
# metadata/netlink/genl/_generated/<family>.yaml. Hand-curated descriptions
# live alongside, in metadata/netlink/genl/<family>.yaml; the loader merges
# both at runtime.
#=============================================================================

EXTRACT_NETLINK := uv run tools/libclang/extract-netlink/extract-netlink.py

.PHONY: regen-metadata-ethtool
regen-metadata-ethtool: ## Regenerate the ethtool overlay's mechanical half from kernel headers
	@mkdir -p metadata/netlink/genl/_generated
	$(EXTRACT_NETLINK) \
	    --header /usr/include/linux/ethtool_netlink.h \
	    --start-marker ETHTOOL_MSG_USER_NONE \
	    --strip-prefix ETHTOOL_MSG_ \
	    --attr-prefix ETHTOOL_A_ \
	    --family ethtool \
	    --output metadata/netlink/genl/_generated/ethtool.yaml
	@echo "wrote metadata/netlink/genl/_generated/ethtool.yaml"

.PHONY: regen-metadata-all
regen-metadata-all: ## Regenerate every family listed in families.yaml
	@mkdir -p metadata/netlink/genl/_generated
	uv run tools/libclang/extract-netlink/regen-all.py \
	    --config tools/libclang/extract-netlink/families.yaml \
	    --extractor tools/libclang/extract-netlink/extract-netlink.py \
	    --out-dir metadata/netlink/genl/_generated

.PHONY: regen-metadata
regen-metadata: regen-metadata-all ## Alias for regen-metadata-all

.PHONY: clean
clean: ## Remove all build directories
	rm -rf $(BUILD_DIR_DESKTOP_YTRACE_RELEASE) \
	       $(BUILD_DIR_DESKTOP_YTRACE_DEBUG) \
	       $(BUILD_DIR_DESKTOP_YTRACE_ASAN) \
	       compile_commands.json

.PHONY: help
help: ## Show this help
	@echo "ykern build targets:"
	@grep -E '^[a-zA-Z0-9_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
	    sort | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-32s\033[0m %s\n", $$1, $$2}'
