# The daemon cannot restore the fan from inside a crash, so the unit has to do
# it. That makes these lines part of the hardware-safety contract rather than
# packaging detail, and worth asserting.
file(READ "${UNIT}" unit)

foreach(required
        "ExecStopPost="
        "pwm1_enable"
        "Restart=always"
        # The vendor unit's real name, not its alias: see the unit's own note.
        "Conflicts=com.system76.PowerDaemon.service"
        "RuntimeDirectory=cpu_throttler"
        "StateDirectory=cpu_throttler")
    string(FIND "${unit}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "systemd unit is missing '${required}'")
    endif()
endforeach()
