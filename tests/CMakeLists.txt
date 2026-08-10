# No test touches real hardware: every case points the code at a fake control
# surface in a temporary directory.

function(ombra_test name)
    cmake_parse_arguments(ARG "" "" "LIBS;ENV" ${ARGN})
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE ${OMBRA_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
    if(ARG_ENV)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${ARG_ENV}")
    endif()
endfunction()

ombra_test(sysfs_test LIBS ombra_core)
ombra_test(metrics_ring_test LIBS ombra_core)
ombra_test(setpoints_test LIBS ombra_core)
ombra_test(modes_test LIBS ombra_core)
ombra_test(sensors_test LIBS ombra_core)
ombra_test(actuator_test LIBS ombra_core)
ombra_test(power_source_test LIBS ombra_core)
ombra_test(exporter_test LIBS ombra_core)
ombra_test(backlight_test LIBS ombra_core)
ombra_test(brightness_map_test LIBS ombra_core)
ombra_test(daemon_state_test LIBS ombra_core)
ombra_test(profile_bridge_test LIBS ombra_core)
ombra_test(control_socket_test LIBS ombra_daemon)
ombra_test(signal_pipe_test LIBS ombra_daemon)
ombra_test(daemon_test LIBS ombra_daemon)

# Claiming the real names on the real bus would disturb the running desktop, so
# these run against a private bus started by dbus-run-session.
ombra_test(dbus_service_test LIBS ombra_daemon)

# Offscreen: the tray is tested without a display. Both drive the real daemon
# and the real tray binary as child processes, which is the topology they will
# run in, so the build order has to guarantee those exist.
ombra_test(autostart_test LIBS ombra_tray)
# No session bus either: following the desktop asks the portal, and a test that
# reached the real one would answer differently on every machine.
ombra_test(theme_test LIBS ombra_tray ENV "QT_QPA_PLATFORM=offscreen;DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/ombra-test-has-no-session-bus")
ombra_test(tray_icon_test LIBS ombra_tray ENV "QT_QPA_PLATFORM=offscreen")
# Pointed at the catalogues this build just compiled, not at the installed ones:
# a suite that tested what is in /usr would be testing the last release.
ombra_test(translations_test LIBS ombra_tray
    ENV "QT_QPA_PLATFORM=offscreen;OMBRA_TRANSLATIONS_DIR=${CMAKE_BINARY_DIR}")
ombra_test(tray_client_test LIBS ombra_tray ENV "QT_QPA_PLATFORM=offscreen")
ombra_test(tray_window_test LIBS ombra_tray ENV "QT_QPA_PLATFORM=offscreen")
add_dependencies(tray_client_test ombrad)
add_dependencies(tray_window_test ombrad ombra-tray)

# Builds the fake control surface as a directory, for the documentation
# pictures. Not installed, and not a test: it is the same tree the suite uses,
# handed to a throwaway daemon so the pictures carry no readings from a real
# machine and come out the same every time.
add_executable(ombra-fake-tree make_fake_tree.cpp)
target_link_libraries(ombra-fake-tree PRIVATE ombra_core Qt6::Test)
target_include_directories(ombra-fake-tree PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ombra-fake-tree PRIVATE ${OMBRA_WARNINGS})

# The binaries themselves, so that option parsing and the exit paths are
# covered by something other than reading them.
ombra_test(cli_test LIBS ombra_core)
target_compile_definitions(cli_test PRIVATE
    DAEMON_BINARY="$<TARGET_FILE:ombrad>"
    CTL_BINARY="$<TARGET_FILE:ombra>"
)
add_dependencies(cli_test ombrad ombra)
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
                 -DUNIT=${CMAKE_CURRENT_SOURCE_DIR}/../systemd/ombrad.service
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_unit.cmake)
