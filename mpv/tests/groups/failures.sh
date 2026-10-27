#!/bin/bash
# Group "failures": every failure path ends in the original video, without a retry loop.

_nfailed() { r_count 'SR failed:'; }
_nfailed_is() { [ "$(_nfailed)" = "$1" ]; }
_nfailed_ge() { [ "$(_nfailed)" -ge "$1" ]; }
_filename_is() { [ "$(r_get filename)" = "$1" ]; }

_advancing() { awk -v a="${1:-0}" -v b="${2:-0}" 'BEGIN { exit !(b > a) }'; }

_failed_once() {  # _failed_once TITLE: state failed, original video still playing, logged once (on the current mpv)
    local title=$1 t1 t2
    r_wait_state failed 14
    check "$title: state failed" $? "$(r_state) $(r_reason)"
    r_quiet 3.0
    [ -z "$(r_labels)" ] && [ "$(r_out)" = 1280x720 ] && [ "$(r_state)" = failed ]
    check "$title: filter removed, original size, still playing" $? "$(r_labels) $(r_out) $(r_state)"
    t1=$(r_get time-pos); r_quiet 1.0; t2=$(r_get time-pos)
    [ -n "$t1" ] && [ -n "$t2" ] && _advancing "$t1" "$t2"
    check "$title: playback keeps advancing" $? "$t1 $t2"
    [ "$(_nfailed)" = 1 ] && [ "$(r_count '[vsr] SR on:')" -le 1 ]
    check "$title: logged once, no retry loop (1x 'SR failed', 1x 'SR on')" $? "failed=$(_nfailed) on=$(r_count '[vsr] SR on:')"
    r_no_lua_errors "$title"
}

group_failures() {
    wanted fail || return 0
    section "4. failure paths fall back to the original video, no retry loop"
    local hook

    # bogus libdir; retry on the next file (default): same failure again
    if r_open --env NVVSR_LIBDIR=/nonexistent/dir v720p30.mp4; then
        _failed_once "bogus NVVSR_LIBDIR (script passes the video through)"
        [[ $(r_reason) == *"no scaled frames"* ]]
        check "bogus NVVSR_LIBDIR: reason mentions the missing output scaling" $? "$(r_reason)"
        r_cmd loadfile "$T_MEDIA/v480p.mp4" replace >/dev/null
        r_wait 14 _nfailed_ge 2
        _nfailed_is 2
        check "retry_on_next_file=yes: the next file tries again (and fails again)" $? "$(_nfailed)"
        r_close
    fi

    # after a failure the toggle key retries once
    if r_open --env NVVSR_LIBDIR=/nonexistent/dir v720p30.mp4; then
        r_wait_state failed 14
        r_key ctrl+s
        r_wait 14 _nfailed_is 2 && r_wait_state failed 14 && r_quiet 3.0 && _nfailed_is 2
        check "after a failure the toggle key retries once (and fails again, no loop)" $? "$(_nfailed)"
        r_close
    fi

    # retry_on_next_file=no: the next file does not try SR at all
    if r_open --env NVVSR_LIBDIR=/nonexistent/dir --opt vsr-retry_on_next_file=no v720p30.mp4; then
        r_wait_state failed 14
        r_cmd loadfile "$T_MEDIA/v480p.mp4" replace >/dev/null
        r_wait 5 _filename_is v480p.mp4
        r_quiet 2.5
        [ "$(r_count '[vsr] SR on:')" = 1 ] && [ -z "$(r_labels)" ] && { [ "$(r_state)" = off ] || [ "$(r_state)" = failed ]; }
        check "retry_on_next_file=no: next file does not try SR at all" $? \
            "$(r_count '[vsr] SR on:') $(r_labels) $(r_state)"
        r_close
    fi

    if r_open --env NVVSR_PLUGIN=/nonexistent/libvsnvvsr.so v720p30.mp4; then
        _failed_once "missing plugin file"
        r_close
    fi

    # plugin whose frames fail after frame 60 (mpv disables the filter itself)
    hook=$(r_hook_plugin)
    if [ -z "$hook" ]; then
        echo "SKIP test-hook plugin cases (no hook build)"
    elif r_open --env "NVVSR_PLUGIN=$hook" --env NVVSR_VS_FAIL_AFTER=60 v720p30.mp4; then
        r_wait_state on 20
        check "test-hook plugin: SR on at first" $? "$(r_state) $(r_reason)"
        r_wait_state failed 10
        check "test-hook plugin: frame errors after frame 60 -> state failed within 6 s" $? "$(r_state) $(r_reason)"
        r_quiet 2.0
        [[ $(r_log) == *"Disabling filter nvvsr because it has failed"* ]] && [ -z "$(r_labels)" ] &&
            [ "$(r_out)" = 1280x720 ] && [ "$(_nfailed)" = 1 ]
        check "test-hook plugin: mpv disabled the filter, we removed it, original size, one 'SR failed'" $? \
            "$(r_labels) $(r_out) $(_nfailed)"
        t1=$(r_get time-pos); r_quiet 1.0
        _advancing "${t1:-0}" "$(r_get time-pos)"
        check "test-hook plugin: playback goes on" $? "$t1 -> $(r_get time-pos)"
        r_no_lua_errors "test-hook plugin"
        r_close
    fi

    # vsr.vpy missing: a copy of vsr.lua alone in a directory
    local lone=$T_SCR/lone
    rm -rf "$lone"; mkdir -p "$lone"; cp "$T_REPO/mpv/vsr.lua" "$lone/"
    if r_open --script "$lone/vsr.lua" v720p30.mp4; then
        r_wait_state failed 10
        [ "$(r_state)" = failed ] && [[ $(r_reason) == *"vsr.vpy not found"* ]] && [ -z "$(r_labels)" ] &&
            [ "$(r_out)" = 1280x720 ]
        check "vsr.vpy not found: failed with that reason, video unchanged" $? "$(r_state) $(r_reason) $(r_labels)"
        r_close
    fi
}
