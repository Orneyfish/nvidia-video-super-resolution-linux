#!/bin/bash
# Group "keys" (integration.py keys()): toggle, quality and stats key bindings, and the script-binding. 13 checks.

_keys_ud() {  # user-data string of the @nvvsr filter ("" when there is none)
    r_ctl get vf 2>/dev/null | jq -r '[.[]? | select(.label == "nvvsr") | .params["user-data"]][0] // ""'
}

_keys_quality_is() { [ "$(_keys_ud)" = "$1" ] && [ "$(r_state)" = on ]; }

_keys_has_stats() { [ -n "$(r_get user-data/vsr/stats)" ]; }

_keys_stats_ok() {  # the stats overlay shows each of these lines
    local k
    [ -n "$1" ] || return 1
    for k in "state:   on" "854x480 -> 2560x1438" "quality: high" "fps:" "drops:" "hwdec:" "power:"; do
        [[ $1 == *"$k"* ]] || return 1
    done
}

group_keys() {
    wanted keys || return 0
    section "3. key bindings: toggle, quality, stats"
    r_open v720p30.mp4 || return 0
    r_wait_state on 20

    r_cmd keypress ctrl+s >/dev/null 2>&1
    check "Ctrl+s accepted" $?
    r_wait_state off 6 && [ -z "$(r_labels)" ] && [[ $(r_reason) == *user* ]]
    check "toggle off: filter removed, state off, 'turned off by user'" $? \
        "$(r_state) $(r_labels) $(r_reason)"
    r_wait 4 _fs_out_is 1280x720
    check "toggle off: original size" $? "$(r_out)"
    r_ctl set fullscreen false >/dev/null; r_quiet 0.6
    r_ctl set fullscreen true >/dev/null; r_quiet 2.0
    [ "$(r_state)" = off ] && [ -z "$(r_labels)" ]
    check "manual off sticks across a fullscreen toggle" $? "$(r_state) $(r_labels)"
    r_cmd script-binding vsr/toggle >/dev/null
    r_wait_state on 15 && [ "$(r_out)" = 2560x1440 ]
    check "script-binding vsr/toggle turns it on again" $? "$(r_state) $(r_out)"
    r_key ctrl+s
    r_wait_state off 6
    r_cmd loadfile "$T_MEDIA/v480p.mp4" replace >/dev/null
    r_wait_state on 20 && [ "$(r_out)" = 2560x1438 ]
    check "next file: manual off no longer applies, SR on again" $? "$(r_state) $(r_out)"

    [ "$(_keys_ud)" = "quality=auto;pool=1" ] && [ "$(r_get user-data/vsr/quality)" = "auto/ultra" ]
    check "auto quality for 480p is ultra" $? "$(_keys_ud) $(r_get user-data/vsr/quality)"
    r_key ctrl+shift+s
    r_wait 15 _keys_quality_is "quality=ultra;pool=1"
    check "Ctrl+Shift+s: quality ultra, filter re-added" $? "$(_keys_ud) $(r_state)"
    r_key ctrl+shift+s
    r_wait 15 _keys_quality_is "quality=high;pool=1"
    check "Ctrl+Shift+s again: high" $? "$(_keys_ud) $(r_state)"
    [ "$(r_labels)" = nvvsr ] && [ "$(r_out)" = 2560x1438 ]
    check "... still one @nvvsr filter and the right size" $? "$(r_labels) $(r_out)"

    r_key alt+s
    r_wait 4 _keys_has_stats
    local stats
    stats=$(r_get user-data/vsr/stats)
    _keys_stats_ok "$stats"
    check "Alt+s: stats overlay renders (state, sizes, quality, fps, drops, hwdec, power)" $? "$stats"
    r_key alt+s
    r_no_lua_errors keys
    r_close
}
