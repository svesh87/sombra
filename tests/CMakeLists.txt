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
ombra_test(tray_client_test LIBS ombra_tray ENV "QT_QPA_PLATFORM=offscreen")
ombra_test(tray_window_test LIBS ombra_tray ENV "QT_QPA_PLATFORM=offscreen")
add_dependencies(tray_client_test ombrad)
add_dependencies(tray_window_test ombrad ombra-tray)

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
