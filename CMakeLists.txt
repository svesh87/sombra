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

find_package(Qt6 REQUIRED COMPONENTS Core Network DBus Widgets Test)

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
    src/daemon_state.cpp
    src/metrics_ring.cpp
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
    src/tray_client.cpp
    src/tray_window.cpp
)
target_link_libraries(ombra_tray PUBLIC ombra_core Qt6::Network Qt6::Widgets)
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
install(TARGETS ombrad ombra ombra-tray
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Absolute, not under the prefix: the daemon looks for its configuration at
# /etc/ombra/modes.conf and nowhere else, so a copy under a prefix
# would be a file nothing reads.
install(FILES config/modes.conf DESTINATION /etc/ombra)
install(FILES systemd/ombrad.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
install(FILES desktop/ombra-tray.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
# Documentation is installed by debhelper from debian/docs, together with the
# copyright file it requires; there is nothing here for a plain `make install`
# to do with it.
#
# Packaging lives in debian/ and is driven by dh with the cmake buildsystem,
# which configures this project with CMAKE_INSTALL_PREFIX=/usr. That is what
# puts the install rules above where a distribution package's files belong.
