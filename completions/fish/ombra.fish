# fish completion for ombra. Installed as
# /usr/share/fish/vendor_completions.d/ombra.fish.
#
# Verbs are listed here rather than asked of the daemon: completion runs on every
# Tab, and a Tab that opens a socket is a Tab that hangs when the daemon is not
# there. Mode names are the exception — nothing else knows them.

function __ombra_ids --argument-names field
    set -l socket /run/ombra/control.sock
    test -S $socket; or return 0
    ombra --socket $socket config show 2>/dev/null \
        | string replace -r ".*\"$field\":\[(.*)" '$1' \
        | string match -ar '"id":"([^"]*)"' \
        | string match -v -r '^"id"'
end

complete -c ombra -f
complete -c ombra -l socket -r -d 'Control socket'
complete -c ombra -l help -d 'Usage'
complete -c ombra -l version -d 'Version'

complete -c ombra -n __fish_use_subcommand -a status -d 'Readings and setpoints as JSON'
complete -c ombra -n __fish_use_subcommand -a mode -d 'Select a mode'
complete -c ombra -n __fish_use_subcommand -a set -d 'Live tuning, not saved by itself'
complete -c ombra -n __fish_use_subcommand -a reset -d 'Drop live tuning'
complete -c ombra -n __fish_use_subcommand -a mark -d 'Stamp a verdict onto the sample'
complete -c ombra -n __fish_use_subcommand -a fan -d "Full speed or the mode's own curve"
complete -c ombra -n __fish_use_subcommand -a brightness -d 'The backlight and its automation'
complete -c ombra -n __fish_use_subcommand -a charge -d 'Battery charge threshold'
complete -c ombra -n __fish_use_subcommand -a config -d 'The saved definitions'
complete -c ombra -n __fish_use_subcommand -a reload -d 'Re-read the settings file'
complete -c ombra -n __fish_use_subcommand -a export -d 'Read the telemetry ring'

complete -c ombra -n '__fish_seen_subcommand_from mode' -a '(__ombra_ids modes)'
complete -c ombra -n '__fish_seen_subcommand_from mark' -a 'good too-loud too-slow'
complete -c ombra -n '__fish_seen_subcommand_from fan' -a 'max auto'
complete -c ombra -n '__fish_seen_subcommand_from set' \
    -a 'pl1= pl2= max_perf_pct= turbo= epp= hwp_boost= gt_max_mhz= profile= fan='

complete -c ombra -n '__fish_seen_subcommand_from brightness; and not __fish_seen_subcommand_from on off level set' \
    -a 'on off level set'
complete -c ombra -n '__fish_seen_subcommand_from brightness; and __fish_seen_subcommand_from set' \
    -a 'response_pct= floor_pct= curve='

complete -c ombra -n '__fish_seen_subcommand_from config; and not __fish_seen_subcommand_from show set save default_mode reset' \
    -a 'show set save default_mode reset'
complete -c ombra -n '__fish_seen_subcommand_from config; and __fish_seen_subcommand_from set; and not __fish_seen_subcommand_from mode brightness' \
    -a 'mode brightness'
complete -c ombra -n '__fish_seen_subcommand_from config; and __fish_seen_subcommand_from save; and not __fish_seen_subcommand_from live' \
    -a live
complete -c ombra -n '__fish_seen_subcommand_from config; and __fish_seen_subcommand_from live; and not __fish_seen_subcommand_from to' \
    -a to
complete -c ombra -n '__fish_seen_subcommand_from config; and __fish_seen_subcommand_from to' \
    -a '(__ombra_ids modes)'
complete -c ombra -n '__fish_seen_subcommand_from config; and __fish_seen_subcommand_from reset' \
    -a 'modes brightness all'
complete -c ombra -n '__fish_seen_subcommand_from config; and __fish_seen_subcommand_from default_mode' \
    -a '(__ombra_ids modes)'

complete -c ombra -n '__fish_seen_subcommand_from export' -l format -a 'csv json' -d 'Output format'
complete -c ombra -n '__fish_seen_subcommand_from export' -l from -d 'Earliest unix timestamp'
complete -c ombra -n '__fish_seen_subcommand_from export' -l to -d 'Latest unix timestamp'
complete -c ombra -n '__fish_seen_subcommand_from export' -l ring -r -d 'Telemetry ring'
