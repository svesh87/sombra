# No test touches real hardware: every case points the code at a fake control
# surface in a temporary directory.

function(sombra_test name)
    cmake_parse_arguments(ARG "" "" "LIBS;ENV" ${ARGN})
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE ${SOMBRA_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
    if(ARG_ENV)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${ARG_ENV}")
    endif()
endfunction()

sombra_test(sysfs_test LIBS sombra_core)
sombra_test(metrics_ring_test LIBS sombra_core)
sombra_test(setpoints_test LIBS sombra_core)
sombra_test(modes_test LIBS sombra_core)
sombra_test(sensors_test LIBS sombra_core)
sombra_test(actuator_test LIBS sombra_core)
sombra_test(power_source_test LIBS sombra_core)
sombra_test(exporter_test LIBS sombra_core)
sombra_test(monitor_bus_test LIBS sombra_core)
# The worker drives a bus in a thread of its own, so it links the daemon's half.
sombra_test(monitor_worker_test LIBS sombra_daemon)
sombra_test(backlight_test LIBS sombra_core)
sombra_test(brightness_map_test LIBS sombra_core)
sombra_test(daemon_state_test LIBS sombra_core)
sombra_test(profile_bridge_test LIBS sombra_core)
sombra_test(control_socket_test LIBS sombra_daemon)
sombra_test(signal_pipe_test LIBS sombra_daemon)
sombra_test(daemon_test LIBS sombra_daemon)

# Claiming the real names on the real bus would disturb the running desktop, so
# these run against a private bus started by dbus-run-session.
sombra_test(dbus_service_test LIBS sombra_daemon)

# Offscreen: the tray is tested without a display. Both drive the real daemon
# and the real tray binary as child processes, which is the topology they will
# run in, so the build order has to guarantee those exist.
sombra_test(autostart_test LIBS sombra_tray)
# No session bus either: following the desktop asks the portal, and a test that
# reached the real one would answer differently on every machine.
sombra_test(theme_test LIBS sombra_tray ENV "QT_QPA_PLATFORM=offscreen;DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/sombra-test-has-no-session-bus")
# The window's icon is a file, and it comes from the source tree here: the theme's
# copy is whatever the last install put there.
sombra_test(tray_icon_test LIBS sombra_tray
    ENV "QT_QPA_PLATFORM=offscreen;SOMBRA_ICONS_DIR=${CMAKE_SOURCE_DIR}/desktop")
# Pointed at the catalogues this build just compiled, not at the installed ones:
# a suite that tested what is in /usr would be testing the last release.
sombra_test(translations_test LIBS sombra_tray
    ENV "QT_QPA_PLATFORM=offscreen;SOMBRA_TRANSLATIONS_DIR=${CMAKE_BINARY_DIR}")
sombra_test(tray_client_test LIBS sombra_tray ENV "QT_QPA_PLATFORM=offscreen")
# The help pages come from the source tree for the same reason the catalogues come
# from the build tree: testing what is in /usr would be testing the last release.
sombra_test(tray_window_test LIBS sombra_tray
    ENV "QT_QPA_PLATFORM=offscreen;SOMBRA_HELP_DIR=${CMAKE_SOURCE_DIR}/help")
add_dependencies(tray_client_test sombrad)
add_dependencies(tray_window_test sombrad sombra-tray)

# Builds the fake control surface as a directory, for the documentation
# pictures. Not installed, and not a test: it is the same tree the suite uses,
# handed to a throwaway daemon so the pictures carry no readings from a real
# machine and come out the same every time.
add_executable(sombra-fake-tree make_fake_tree.cpp)
target_link_libraries(sombra-fake-tree PRIVATE sombra_core Qt6::Test)
target_include_directories(sombra-fake-tree PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(sombra-fake-tree PRIVATE ${SOMBRA_WARNINGS})

# The binaries themselves, so that option parsing and the exit paths are
# covered by something other than reading them.
sombra_test(cli_test LIBS sombra_core)
target_compile_definitions(cli_test PRIVATE
    DAEMON_BINARY="$<TARGET_FILE:sombrad>"
    CTL_BINARY="$<TARGET_FILE:sombra>"
)
add_dependencies(cli_test sombrad sombra)
set_tests_properties(cli_test PROPERTIES TIMEOUT 120)

# The package is a work item like any other, and a packaging mistake is only
# ever found by looking at what came out. The script builds its own tree with
# the packaging prefix, so it disturbs neither this build nor the system: the
# package is inspected with dpkg-deb, never installed.
find_program(DPKG_DEB_EXECUTABLE dpkg-deb)
if(DPKG_DEB_EXECUTABLE)
    add_test(NAME package_contents
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check_package.sh
                     ${CMAKE_BINARY_DIR}/package)
    set_tests_properties(package_contents PROPERTIES TIMEOUT 600)
endif()

# The unit file is a work item like any other: the crash path depends on it.
add_test(NAME systemd_unit_restores_the_fan
         COMMAND ${CMAKE_COMMAND}
                 -DUNIT=${CMAKE_CURRENT_SOURCE_DIR}/../systemd/sombrad.service
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_unit.cmake)
