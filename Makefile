# Makefile
#
# Convenience wrapper around the CMake presets defined in
# CMakePresets.json, mirroring iouring-net-lib/Makefile. The pure CMake
# commands work too. Override the active preset with PRESET=<name>:
#
#   make run                    # uses default
#   make test  PRESET=floor     # runs the floor (gcc-12) preset
#   make build PRESET=release
#
# Prerequisite: the library must be installed to a prefix on
# CMAKE_PREFIX_PATH (presets default to $HOME/.local):
#
#   cmake -S ../iouring-net-lib -B ../iouring-net-lib/build/seam \
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$$HOME/.local \
#         -DIOURING_NET_BUILD_TESTS=OFF -DIOURING_NET_BUILD_EXAMPLES=OFF
#   cmake --build ../iouring-net-lib/build/seam
#   cmake --install ../iouring-net-lib/build/seam

PRESET    ?= default
BUILD_DIR := build/$(PRESET)

.PHONY: help configure build test run clean distclean

help:
	@echo "Targets (override preset with PRESET=floor|default|release|tsan):"
	@echo "  make configure  — cmake configure (preset=$(PRESET))"
	@echo "  make build      — cmake build      (preset=$(PRESET))"
	@echo "  make test       — ctest            (preset=$(PRESET))"
	@echo "  make run        — build and run the server (--dry-run)"
	@echo "  make clean      — remove the current preset's build dir"
	@echo "  make distclean  — remove every build dir"

configure:
	cmake --preset $(PRESET)

build: configure
	cmake --build --preset $(PRESET)

test: build
	ctest --preset $(PRESET)

run: build
	@echo "--- running iouring_net-server (dry run) ---"
	$(BUILD_DIR)/server/iouring_net-server --dry-run

clean:
	rm -rf $(BUILD_DIR)

distclean:
	rm -rf build/
