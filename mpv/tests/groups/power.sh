#!/bin/bash
# Group "power": AC / battery switching, driven by a fake sysfs power_supply directory.

_setac() {  # _setac VALUE [SUPPLY]: the online flag of a fake supply (ADP0 mains, ucsi USB-C); default ADP0
    printf '%d\n' "$1" >"$T_SCR/power/${2:-ADP0}/online"
}

_off_no_filter() { [ "$(r_state)" = off ] && [ -z "$(r_labels)" ]; }
_out_is() { [ "$(r_out)" = "$1" ]; }

group_power() {
    wanted power || return 0
    section "6. AC / battery"
    local pdir=$T_SCR/power

    rm -rf "$pdir"
    mkdir -p "$pdir/ADP0" "$pdir/BAT0" "$pdir/ucsi"
    printf 'Mains\n' >"$pdir/ADP0/type"
    printf 'Battery\n' >"$pdir/BAT0/type"
    printf 'USB\n' >"$pdir/ucsi/type"
    _setac 0 ucsi
    _setac 0

    if r_open --opt vsr-power_dir="$pdir" --opt vsr-check_interval=1 v720p30.mp4; then
        r_wait_decided
        r_quiet 1.0
        [ "$(r_state)" = off ] && [ "$(r_reason)" = "on battery" ] && [ -z "$(r_labels)" ]
        check "on battery at start: SR off with reason 'on battery', no filter" $? "$(r_state) $(r_reason) $(r_labels)"
        r_quiet 2.5
        [ "$(r_count 'SR off: on battery (ac_only=yes)')" = 1 ]
        check "'SR off: on battery' is logged exactly once" $? "$(r_count 'SR off: on battery')"
        _setac 1
        r_wait_state on 15
        check "AC plugged in: SR turns on within the poll interval" $? "$(r_state) $(r_reason)"
        _setac 0
        r_wait 8 _off_no_filter
        check "AC unplugged: SR turns off, filter removed" $? "$(r_state) $(r_labels)"
        [ "$(r_reason)" = "on battery" ] && r_wait 4 _out_is 1280x720
        check "... reason 'on battery', output back to the source size" $? "$(r_reason) $(r_out)"
        _setac 0; _setac 1 ucsi
        r_wait_state on 15
        check "a USB-C PD charger counts as AC" $? "$(r_state) $(r_reason)"
        r_no_lua_errors power
        r_close
    fi

    _setac 0
    if r_open --opt vsr-power_dir="$pdir" --opt vsr-check_interval=1 --opt vsr-ac_only=no v720p30.mp4; then
        r_wait_state on 20
        check "ac_only=no: SR runs on battery" $? "$(r_state) $(r_reason)"
        r_close
    fi
}
