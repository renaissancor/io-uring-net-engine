# Makefile
#
# Convenience wrapper around the CMake presets defined in
# CMakePresets.json. The pure CMake commands work too — see
# docs/05-cmake.md — this file exists so the common dev cycles fit on
# one line. Override the active preset with PRESET=<name>:
#
#   make hello                  # uses default
#   make test  PRESET=floor     # runs the floor (gcc-12) preset
#   make build PRESET=release

PRESET    ?= default
BUILD_DIR := build/$(PRESET)

.PHONY: help configure build test test-sds hello clean distclean

help:
	@echo "Targets (override preset with PRESET=floor|default|release|tsan):"
	@echo "  make configure  — cmake configure (preset=$(PRESET))"
	@echo "  make build      — cmake build      (preset=$(PRESET))"
	@echo "  make test       — ctest            (preset=$(PRESET))"
	@echo "  make test-sds   — run only sds:: data-structure tests"
	@echo "  make hello      — build and run examples/hello"
	@echo "  make clean      — remove the current preset's build dir"
	@echo "  make distclean  — remove every build dir"

configure:
	cmake --preset $(PRESET)

build: configure
	cmake --build --preset $(PRESET)

test: build
	ctest --preset $(PRESET)

test-sds: build
	@echo "--- running sds:: data-structure tests ---"
	$(BUILD_DIR)/tests/iouring_net-test "[sds]"

hello: build
	@echo "--- running iouring_net-hello ---"
	$(BUILD_DIR)/examples/hello/iouring_net-hello

clean:
	rm -rf $(BUILD_DIR)

distclean:
	rm -rf build/
