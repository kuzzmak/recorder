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
# Runtime-created per-user icons (TrayIcon::installToTheme); distinct from the
# copies that `cmake --install` puts under $(PREFIX)/share/icons.
ICONS       = $(HOME)/.local/share/icons/hicolor
LAUNCHER    = $(PREFIX)/share/applications/recorder.desktop
PREFIX_ICONS = $(PREFIX)/share/icons/hicolor

# Packaging (see `make deb`, `make ppa`)
DIST_DIR  ?= dist
SERIES    ?= noble
PPA       ?= ppa:tonkec/recorder
# Committed packaging version, e.g. 0.1.0+ppa1 (no inline comments on these
# lines: Make keeps the whitespace before '#' and would corrupt the version).
PKG_VER    = $(shell dpkg-parsechangelog -l debian/changelog -S Version)
# Numeric upstream version from project(Recorder VERSION x.y.z). Braces, not
# parentheses: the literal '(' in "project(" would otherwise end the $(shell).
CMAKE_VER  = ${shell sed -n 's/^project(Recorder VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt}
# Series-specific version used by both the local .deb and the PPA upload,
# e.g. 0.1.0+ppa1~noble1
FULL_VER   = $(PKG_VER)~$(SERIES)1
# Packaging revision N from <upstream>+ppaN (for `make bump`)
PPA_REV    = $(lastword $(subst +ppa, ,$(PKG_VER)))
# Changelog text for `make bump` / `make release`; dch opens an editor if empty
MSG       ?=
SRC_DIR    = $(DIST_DIR)/recorder-$(FULL_VER)
CHANGES    = $(DIST_DIR)/recorder_$(FULL_VER)

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
	sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev libpulse-dev libopusenc-dev \
	    debhelper devscripts lintian dput desktop-file-utils

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

## install: install to $(PREFIX), sync the autostart entry, restart the app
# --prefix overrides the prefix baked into the CMake cache at configure time,
# so `make install PREFIX=/other` works on an existing build directory.
.PHONY: install
install: build
	cmake --install $(BUILD_DIR) --prefix "$(PREFIX)"
	$(INSTALLED) --sync-autostart
	$(MAKE) stop
	($(INSTALLED) >/dev/null 2>&1 &)

## uninstall: stop the app; remove what `install` put under $(PREFIX) and the per-user autostart entry and icons
.PHONY: uninstall
uninstall: stop
	-rm -f $(INSTALLED) $(LAUNCHER)
	-rm -f $(PREFIX_ICONS)/*/apps/recorder.png $(PREFIX_ICONS)/*/apps/recorder-recording.png
	-rm -f $(AUTOSTART)
	-rm -f $(ICONS)/*/apps/recorder.png $(ICONS)/*/apps/recorder-recording.png
	@echo "Removed $(INSTALLED), $(LAUNCHER), $(AUTOSTART) and the hicolor icons"

## clean: remove built objects (keep CMake config)
.PHONY: clean
clean:
	-cmake --build $(BUILD_DIR) --target clean

## distclean: remove every build directory and dist/
.PHONY: distclean
distclean:
	rm -rf $(BUILD_DIR) $(ASAN_DIR) $(TSAN_DIR) $(DIST_DIR)

# ----------------------------------------------------------------------------
# Packaging: Debian package and Launchpad PPA
# ----------------------------------------------------------------------------

## bump: next packaging revision (+ppaN) in debian/changelog, e.g. make bump MSG="Fix X"
.PHONY: bump
bump:
	dch -v "$(CMAKE_VER)+ppa$$(( $(PPA_REV) + 1 ))" -D $(SERIES) --force-distribution "$${MSG:-Packaging update.}"
	@head -1 debian/changelog

## release: new upstream version in CMakeLists.txt and debian/changelog, e.g. make release VERSION=0.1.1 MSG="..."
.PHONY: release
release:
	@test -n "$(VERSION)" || { echo "usage: make release VERSION=x.y.z [MSG=\"...\"]"; exit 1; }
	sed -i 's/^project(Recorder VERSION [0-9.]*/project(Recorder VERSION $(VERSION)/' CMakeLists.txt
	dch -v "$(VERSION)+ppa1" -D $(SERIES) --force-distribution "$${MSG:-New upstream release $(VERSION).}"
	@grep -n '^project(' CMakeLists.txt; head -1 debian/changelog

## icons: regenerate data/icons from TrayIcon::render (commit the result)
.PHONY: icons
icons: build
	QT_QPA_PLATFORM=offscreen $(BIN) --export-icons data/icons

## export: clean copy of HEAD in dist/ with the $(SERIES) version applied (commit first)
.PHONY: export
export:
	@case "$(PKG_VER)" in $(CMAKE_VER)*) ;; *) echo "debian/changelog $(PKG_VER) != CMake $(CMAKE_VER)"; exit 1;; esac
	rm -rf $(SRC_DIR) && mkdir -p $(SRC_DIR)
	git archive HEAD | tar -x -C $(SRC_DIR)
	cd $(SRC_DIR) && dch -b -v "$(FULL_VER)" -D $(SERIES) --force-distribution "Build for $(SERIES)."

## deb: unsigned binary package (same version as the PPA build) into dist/, then lintian
.PHONY: deb
deb: export
	cd $(SRC_DIR) && dpkg-buildpackage -us -uc -b
	lintian -I $(CHANGES)_*.changes

## source: signed source package for $(SERIES) into dist/
.PHONY: source
source: export
	cd $(SRC_DIR) && debuild -S
	lintian -I $(CHANGES)_source.changes

## ppa: upload the $(SERIES) source package to $(PPA)
.PHONY: ppa
ppa: source
	dput $(PPA) $(CHANGES)_source.changes
