#!/bin/bash
# Group "lifecycle": no stale filter or hwdec change between files, display changes,
# and fullscreen flapping. 11 checks.

_lc_pos_is() { [ "$(r_get playlist-pos)" = "$1" ]; }
_lc_logged() { r_log | grep -qF -- "$1"; }
_lc_off_reason() { [ "$(r_state)" = off ] && [[ $(r_reason) == *"$1"* ]]; }

_lc_files() {  # three files in one mpv: no filter, hwdec or state left over from the previous file
    r_open --extra --hwdec=nvdec --extra --keep-open=no --extra --end=3 v720p30.mp4 hdr_pq.mp4 v1440p.mp4 || return 0
    r_wait_state on 20
    [ "$(r_hwdec)" = '["nvdec-copy"]' ]
    check "file 1: SR on, hwdec switched" $? "$(r_hwdec)"
    r_wait 12 _lc_pos_is 1
    r_quiet 0.2
    [ -z "$(r_labels)" ] && [ "$(r_hwdec)" = '["nvdec"]' ]
    check "file 2 (HDR) starts with no @nvvsr filter and the user's hwdec back" $? "$(r_labels) $(r_hwdec)"
    r_wait_decided; r_quiet 1.0
    [ "$(r_state)" = off ] && [[ $(r_reason) == *HDR* ]]
    check "file 2 (HDR): SR off, reason HDR" $? "$(r_state) $(r_reason)"
    r_wait 12 _lc_pos_is 2
    r_quiet 1.0
    [ -z "$(r_labels)" ] && [ "$(r_hwdec)" = '["nvdec"]' ] && [ "$(r_state)" = off ]
    check "file 3 (1440p): still nothing left behind" $? "$(r_labels) $(r_hwdec) $(r_state)"
    r_no_lua_errors lifecycle
    r_close
}

_lc_display() {  # the display size changes under a running SR (the script's user-data/vsr/display stands in for a move)
    r_open v720p30.mp4 || return 0
    r_wait_state on 20
    r_ctl set user-data/vsr/display '"1920x1080"' >/dev/null
    r_wait 6 _lc_logged "display changed to 1920x1080" && r_wait_state on 10 && [ "$(r_labels)" = nvvsr ]
    check "display change while on: the filter is restarted for the new size (state back to on)" $? \
        "$(r_state) $(r_labels)"
    r_ctl set user-data/vsr/display '"1280x720"' >/dev/null
    r_wait 6 _lc_off_reason "not smaller" && [ -z "$(r_labels)" ]
    check "display change to 720p (nothing to gain): SR off" $? "$(r_state) $(r_reason)"
    r_ctl set user-data/vsr/display '"2560x1440"' >/dev/null
    r_wait_state on 10
    check "display back to 1440p: SR on again" $? "$(r_state)"
    local i
    for ((i = 0; i < 3; i++)); do
        r_ctl set fullscreen false >/dev/null; r_quiet 1.0
        r_ctl set fullscreen true >/dev/null; r_quiet 1.2
    done
    r_wait_state on 15 && [ "$(r_labels)" = nvvsr ] && [ "$(r_out)" = 2560x1440 ]
    check "3 quick fullscreen toggles: ends consistently on" $? "$(r_state) $(r_labels) $(r_out)"
    r_close
}

_lc_flap() {  # fullscreen toggled 8 times: the flapping guard refuses the 7th enable
    r_open v720p30.mp4 || return 0
    r_wait_state on 20
    local i
    for ((i = 0; i < 8; i++)); do
        r_ctl set fullscreen false >/dev/null; r_quiet 0.9
        r_ctl set fullscreen true >/dev/null; r_quiet 1.1
        [ "$(r_state)" = failed ] && break
    done
    r_quiet 1.0
    [ "$(r_state)" = failed ] && [[ $(r_reason) == *"too many restarts"* ]] && [ -z "$(r_labels)" ] && r_alive
    check "flapping guard: 7th enable within 30 s is refused (state failed, filter gone, video plays)" $? \
        "$(r_state) $(r_reason) $(r_labels)"
    r_close
}

group_lifecycle() {
    wanted lifecycle || return 0
    section "8. no stale filter or hwdec change between files"
    _lc_files
    _lc_display
    _lc_flap
}
