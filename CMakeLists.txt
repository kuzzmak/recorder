cmake_minimum_required(VERSION 3.21)

project(Recorder VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTING "Build unit tests" ON)
# Non-empty => instrument every target with -fsanitize=<value>. GCC refuses
# address+thread together, so use one build directory per sanitizer
# (see `make test-asan` / `make test-tsan`).
set(SANITIZE "" CACHE STRING "address|thread|undefined (empty = none)")

set(RECORDER_QT_COMPONENTS Core Gui Widgets DBus)
if(BUILD_TESTING)
    list(APPEND RECORDER_QT_COMPONENTS Test)
endif()
find_package(Qt6 REQUIRED COMPONENTS ${RECORDER_QT_COMPONENTS})
qt_standard_project_setup(REQUIRES 6.4)

# System audio libraries: libpulse must match the running sound server, and
# libopusenc is one small package, so both are expected from apt (`make deps`).
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
pkg_check_modules(OPUSENC REQUIRED IMPORTED_TARGET libopusenc)

# ----------------------------------------------------------------------------
# recorder_core : capture, alignment, encoding and the session state machine.
# No UI. Linked by the app and by every test. Public headers only forward-
# declare pulse/opus types, so the audio packages stay a private dependency.
# ----------------------------------------------------------------------------
add_library(recorder_core STATIC
    src/core/AudioFormat.h
    src/core/TimingSnapshot.h
    src/core/SampleQueue.cpp
    src/core/SampleQueue.h
    src/core/AudioSource.h
    src/core/PulseCapture.cpp
    src/core/PulseCapture.h
    src/core/StreamClock.cpp
    src/core/StreamClock.h
    src/core/StreamAligner.cpp
    src/core/StreamAligner.h
    src/core/AudioMixer.cpp
    src/core/AudioMixer.h
    src/core/AudioWriter.h
    src/core/OpusWriter.cpp
    src/core/OpusWriter.h
    src/core/WavWriter.cpp
    src/core/WavWriter.h
    src/core/OutputPath.cpp
    src/core/OutputPath.h
    src/core/EncoderThread.cpp
    src/core/EncoderThread.h
    src/core/Recorder.cpp
    src/core/Recorder.h
    src/core/Settings.cpp
    src/core/Settings.h
    src/core/AutoStart.cpp
    src/core/AutoStart.h
    src/core/TrayHost.cpp
    src/core/TrayHost.h
    src/core/TrayIcon.cpp
    src/core/TrayIcon.h
)
set_target_properties(recorder_core PROPERTIES AUTOMOC ON)
target_include_directories(recorder_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(recorder_core
    PUBLIC Qt6::Core Qt6::Gui Qt6::DBus
    PRIVATE PkgConfig::PULSE PkgConfig::OPUSENC
)

if(SANITIZE)
    # PUBLIC so every executable that links the instrumented static library
    # (tests and the app) also gets the matching link flags.
    target_compile_options(recorder_core PUBLIC -fsanitize=${SANITIZE} -fno-omit-frame-pointer)
    target_link_options(recorder_core PUBLIC -fsanitize=${SANITIZE})
endif()

# ----------------------------------------------------------------------------
# Application
# ----------------------------------------------------------------------------
qt_add_executable(recorder
    src/main.cpp
    src/ui/TrayController.cpp
    src/ui/TrayController.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
)
set_target_properties(recorder PROPERTIES AUTOMOC ON)
target_link_libraries(recorder PRIVATE recorder_core Qt6::Widgets)

# Version string for --version / QCoreApplication::applicationVersion().
# Only the numeric upstream version lives here; the packaging revision
# (+ppaN) is debian/changelog's business.
target_compile_definitions(recorder PRIVATE RECORDER_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
install(TARGETS recorder RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/recorder.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# Pre-rendered copies of TrayIcon::render(); regenerate with `make icons`.
install(DIRECTORY data/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
