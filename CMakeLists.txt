cmake_minimum_required(VERSION 3.16)
project(ombra VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)
if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network DBus Widgets Test LinguistTools)

set(OMBRA_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(OMBRA_WARNINGS -Wall -Wextra -Wpedantic -Werror)

# Everything that touches the machine or the stored samples, with no event
# loop and no user interface. Tests point it at a fake tree in a temporary
# directory, which is why the sysfs root is a constructor argument rather than
# a compiled-in constant.
add_library(ombra_core STATIC
    src/sysfs.cpp
    src/setpoints.cpp
    src/modes.cpp
    src/sensors.cpp
    src/actuator.cpp
    src/power_source.cpp
    src/light_sensor.cpp
    src/backlight.cpp
    src/brightness_map.cpp
    src/settings_file.cpp
    src/daemon_state.cpp
    src/metrics_ring.cpp
    src/ring_migrate.cpp
    src/exporter.cpp
    src/profile_bridge.cpp
)
target_include_directories(ombra_core PUBLIC ${OMBRA_SOURCE_DIR})
target_link_libraries(ombra_core PUBLIC Qt6::Core)
target_compile_options(ombra_core PRIVATE ${OMBRA_WARNINGS})

# The loop, its control channel and its signal handling.
add_library(ombra_daemon STATIC
    src/daemon.cpp
    src/control_socket.cpp
    src/signal_pipe.cpp
    src/dbus_service.cpp
)
target_link_libraries(ombra_daemon PUBLIC ombra_core Qt6::Network Qt6::DBus)
target_compile_options(ombra_daemon PRIVATE ${OMBRA_WARNINGS})

# The tray talks to the daemon over the control socket and shares nothing else
# with it, so it links the core for the setpoint types and nothing more.
add_library(ombra_tray STATIC
    src/autostart.cpp
    src/theme.cpp
    src/tray_client.cpp
    src/tray_icon.cpp
    src/translations.cpp
    src/tray_window.cpp
)
# DBus is here for one thing: asking the desktop portal whether it wants light
# or dark. The tray talks to the daemon over the socket and to nothing else.
target_link_libraries(ombra_tray PUBLIC ombra_core Qt6::DBus Qt6::Network Qt6::Widgets)
target_compile_options(ombra_tray PRIVATE ${OMBRA_WARNINGS})

add_executable(ombrad src/daemon_main.cpp)
target_link_libraries(ombrad PRIVATE ombra_daemon)
target_compile_options(ombrad PRIVATE ${OMBRA_WARNINGS})

# The one command that does not go through the socket is `export`, which reads
# the ring off disk, so this links the core rather than only the socket.
add_executable(ombra src/ctl_main.cpp)
target_link_libraries(ombra PRIVATE ombra_core Qt6::Network)
target_compile_options(ombra PRIVATE ${OMBRA_WARNINGS})

add_executable(ombra-tray src/tray_main.cpp)
target_link_libraries(ombra-tray PRIVATE ombra_tray)
target_compile_options(ombra-tray PRIVATE ${OMBRA_WARNINGS})

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)

# Where the console looks for its catalogues, and what it is told at compile time.
# One place for both, because a mismatch between them is a translation that is
# installed and never loaded.
set(OMBRA_TRANSLATIONS_DIR ${CMAKE_INSTALL_DATAROOTDIR}/ombra/translations)
target_compile_definitions(ombra_tray PRIVATE
    OMBRA_TRANSLATIONS_PATH="${CMAKE_INSTALL_FULL_DATAROOTDIR}/ombra/translations")
install(TARGETS ombrad ombra ombra-tray
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# No configuration is installed. The daemon carries its defaults compiled in
# and writes its own settings file under /var/lib on first change, so a file
# shipped in /etc would be a second source of truth with nothing to gain by it.
install(FILES systemd/ombrad.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
install(FILES desktop/ombra-tray.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# Shell completion, as static files rather than generated ones: three binaries
# and a fixed list of verbs make a generator cost more than its output.
install(FILES completions/bash/ombra
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/bash-completion/completions)
install(FILES completions/zsh/_ombra
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/zsh/vendor-completions)
install(FILES completions/fish/ombra.fish
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/fish/vendor_completions.d)

# The console's catalogues. Compiled here rather than committed: a .qm is a build
# product, and the .ts files beside them are the source. `lupdate` is a target of
# its own — running it is how new strings get into the .ts, and it is not part of
# an ordinary build, because it rewrites files in the source tree.
set(OMBRA_TS_FILES
    translations/ombra_ru.ts
    translations/ombra_es.ts)
# The target form, which is what this Qt takes: it hooks the compilation of the
# catalogues to the library's own build and gives `ombra_tray_lupdate` for
# refreshing the .ts files by hand.
# `src/tray_main.cpp` is named on top of the target's own sources: it belongs to
# the executable rather than to this library, and the menu of the panel icon lives
# in it.
# The two files with translatable strings, named rather than taken from a target:
# the window belongs to this library and the panel menu to the executable, and
# lupdate needs both in one pass or the strings of the other become obsolete.
qt_add_lupdate(ombra_tray TS_FILES ${OMBRA_TS_FILES}
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/tray_window.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tray_main.cpp)
qt_add_lrelease(ombra_tray TS_FILES ${OMBRA_TS_FILES}
    QM_FILES_OUTPUT_VARIABLE OMBRA_QM_FILES)
install(FILES ${OMBRA_QM_FILES} DESTINATION ${OMBRA_TRANSLATIONS_DIR})

install(FILES man/ombra.1 man/ombra-tray.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES man/ombrad.8 DESTINATION ${CMAKE_INSTALL_MANDIR}/man8)
# Documentation is installed by debhelper from debian/docs, together with the
# copyright file it requires; there is nothing here for a plain `make install`
# to do with it.
#
# Packaging lives in debian/ and is driven by dh with the cmake buildsystem,
# which configures this project with CMAKE_INSTALL_PREFIX=/usr. That is what
# puts the install rules above where a distribution package's files belong.
