# No test touches real hardware: every case points the code at a fake control
# surface in a temporary directory.

function(throttler_test name)
    cmake_parse_arguments(ARG "" "" "LIBS;ENV" ${ARGN})
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE ${THROTTLER_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
    if(ARG_ENV)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${ARG_ENV}")
    endif()
endfunction()

throttler_test(sysfs_test LIBS throttler_core)
throttler_test(metrics_ring_test LIBS throttler_core)
throttler_test(setpoints_test LIBS throttler_core)
throttler_test(modes_test LIBS throttler_core)
throttler_test(sensors_test LIBS throttler_core)
throttler_test(actuator_test LIBS throttler_core)
throttler_test(power_source_test LIBS throttler_core)
throttler_test(exporter_test LIBS throttler_core)
throttler_test(backlight_test LIBS throttler_core)
throttler_test(brightness_map_test LIBS throttler_core)
throttler_test(daemon_state_test LIBS throttler_core)
throttler_test(profile_bridge_test LIBS throttler_core)
throttler_test(control_socket_test LIBS throttler_daemon)
throttler_test(signal_pipe_test LIBS throttler_daemon)
throttler_test(daemon_test LIBS throttler_daemon)

# Claiming the real names on the real bus would disturb the running desktop, so
# these run against a private bus started by dbus-run-session.
throttler_test(dbus_service_test LIBS throttler_daemon)

# Offscreen: the tray is tested without a display. Both drive the real daemon
# and the real tray binary as child processes, which is the topology they will
# run in, so the build order has to guarantee those exist.
throttler_test(tray_client_test LIBS throttler_tray ENV "QT_QPA_PLATFORM=offscreen")
throttler_test(tray_window_test LIBS throttler_tray ENV "QT_QPA_PLATFORM=offscreen")
add_dependencies(tray_client_test cpu-throttlerd)
add_dependencies(tray_window_test cpu-throttlerd cpu-throttler-tray)

# The binaries themselves, so that option parsing and the exit paths are
# covered by something other than reading them.
throttler_test(cli_test LIBS throttler_core)
target_compile_definitions(cli_test PRIVATE
    DAEMON_BINARY="$<TARGET_FILE:cpu-throttlerd>"
    CTL_BINARY="$<TARGET_FILE:cpu-throttlerctl>"
    EXPORT_BINARY="$<TARGET_FILE:cpu-throttler-export>"
)
add_dependencies(cli_test cpu-throttlerd cpu-throttlerctl cpu-throttler-export)
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
                 -DUNIT=${CMAKE_CURRENT_SOURCE_DIR}/../systemd/cpu-throttlerd.service
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_unit.cmake)
