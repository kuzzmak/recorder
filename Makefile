# Convenience wrapper around the CMake/Ninja build.
# Run `make` or `make help` for the list of targets.

BUILD_DIR  ?= build
ASAN_DIR   ?= build-asan
TSAN_DIR   ?= build-tsan
GENERATOR  ?= Ninja
PREFIX     ?= $(HOME)/.local
BIN         = $(BUILD_DIR)/recorder
INSTALLED   = $(PREFIX)/bin/recorder
CONFIG      = $(BUILD_DIR)/CMakeCache.txt
AUTOSTART   = $(HOME)/.config/autostart/recorder.desktop
ICONS       = $(HOME)/.local/share/icons/hicolor

# Extra flags, e.g.  make configure CMAKE_FLAGS=-DBUILD_TESTING=OFF
CMAKE_FLAGS ?=

.DEFAULT_GOAL := help

## help: list available targets
.PHONY: help
help:
	@echo "Recorder — make targets:"
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'

## deps: install build dependencies with apt (needs sudo)
.PHONY: deps
deps:
	sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev libpulse-dev libopusenc-dev

## configure: run CMake (first time, or after editing CMakeLists)
.PHONY: configure
configure: $(CONFIG)
$(CONFIG):
	cmake -B $(BUILD_DIR) -G $(GENERATOR) -DCMAKE_INSTALL_PREFIX=$(PREFIX) $(CMAKE_FLAGS)

## build: compile the app and tests
.PHONY: build
build: configure
	cmake --build $(BUILD_DIR)

## run: stop any running instance, build, then launch the app (X11)
.PHONY: run
run: build stop
	QT_QPA_PLATFORM=xcb $(BIN) --no-autostart-sync

## test: build, then run the unit-test suite
.PHONY: test
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

## test-asan: run the tests under AddressSanitizer (separate build dir)
.PHONY: test-asan
test-asan:
	cmake -B $(ASAN_DIR) -G $(GENERATOR) -DSANITIZE=address $(CMAKE_FLAGS)
	cmake --build $(ASAN_DIR)
	ctest --test-dir $(ASAN_DIR) --output-on-failure

## test-tsan: run the tests under ThreadSanitizer (separate build dir)
# Recent kernels map memory where TSan does not expect it ("unexpected memory
# mapping"); running with ASLR disabled (setarch -R) avoids that. The
# suppression file hides one race inside libpulse's own mainloop mutex.
.PHONY: test-tsan
test-tsan:
	cmake -B $(TSAN_DIR) -G $(GENERATOR) -DSANITIZE=thread $(CMAKE_FLAGS)
	cmake --build $(TSAN_DIR)
	TSAN_OPTIONS="suppressions=$(CURDIR)/tests/tsan.supp" \
	setarch $$(uname -m) -R ctest --test-dir $(TSAN_DIR) --output-on-failure

## stop: ask any running instance to quit (SIGTERM finalizes the file), wait up to 10 s
.PHONY: stop
stop:
	@pkill -x recorder || true
	@i=0; while pgrep -x recorder >/dev/null && [ $$i -lt 50 ]; do sleep 0.2; i=$$((i+1)); done; \
	if pgrep -x recorder >/dev/null; then echo "recorder did not exit; killing"; pkill -KILL -x recorder || true; fi

## install: install to $(PREFIX)/bin, sync the autostart entry, restart the app
.PHONY: install
install: build
	cmake --install $(BUILD_DIR)
	$(INSTALLED) --sync-autostart
	$(MAKE) stop
	($(INSTALLED) >/dev/null 2>&1 &)

## uninstall: stop the app and remove the binary, autostart entry and icons
.PHONY: uninstall
uninstall: stop
	-rm -f $(INSTALLED)
	-rm -f $(AUTOSTART)
	-rm -f $(ICONS)/*/apps/recorder.png $(ICONS)/*/apps/recorder-recording.png
	@echo "Removed $(INSTALLED), $(AUTOSTART) and the hicolor icons"

## clean: remove built objects (keep CMake config)
.PHONY: clean
clean:
	-cmake --build $(BUILD_DIR) --target clean

## distclean: remove every build directory
.PHONY: distclean
distclean:
	rm -rf $(BUILD_DIR) $(ASAN_DIR) $(TSAN_DIR)
