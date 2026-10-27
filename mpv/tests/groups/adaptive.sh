#!/bin/bash
# Group "adaptive" (integration.py adaptive()): the quality step-down ladder under forced frame drops, adaptive=no,
# and pause handling. 13 checks, as in the Python.

# shortcut: a fixed short window so the ladder runs in seconds, the defaults are 30 s and 5 drops
_AD_QUICK=(--opt vsr-adaptive_window=3 --opt vsr-adaptive_drops=5 --opt vsr-settle=1)
# shortcut: --vo=null with no audio never counts a late frame (playback just slows), so the heavy chain uses a wall
# clock (display-vdrop at 60 Hz) and its late frames show up as vo-delayed-frame-count, not frame-drop-count
# shortcut: confirm_timeout 15 s, not 3 s: under load the heavy chain's first frames take longer than 3 s
_AD_HEAVY=(--extra "--vf=@heavy:lavfi=[bm3d=sigma=10]" --extra --video-sync=display-vdrop --extra --display-fps-override=60 --opt vsr-confirm_timeout=15)

_ad_ud() {  # user-data string of the @nvvsr filter ("" when there is none)
    r_ctl get vf 2>/dev/null | jq -r '[.[]? | select(.label == "nvvsr") | .params["user-data"]][0] // ""'
}

_ad_drops() {  # the drop count vsr.lua watches (its drop_counter): vo, decoder and delayed frames
    echo $(( $(r_get frame-drop-count 0) + $(r_get decoder-frame-drop-count 0) + $(r_get vo-delayed-frame-count 0) ))
}

_ad_logged() { r_log | grep -qF -- "$1"; }
_ad_advancing() { awk -v a="${1:-0}" -v b="${2:-0}" 'BEGIN { exit !(b > a) }'; }
_ad_off_reason() { [ "$(r_state)" = off ] && [[ $(r_reason) == *"$1"* ]]; }
_ad_failed() { [ "$(r_state)" = failed ]; }

_ad_ladder() {
    # A deliberately heavy CPU filter (bm3d, ~60 ms per 720p frame) in front of SR makes the chain miss its 33 ms
    # budget on any machine, so frames are dropped at the VO; stepping SR down cannot help, which exercises the
    # complete ladder down to the floor.
    r_open "${_AD_HEAVY[@]}" "${_AD_QUICK[@]}" v720p30.mp4 || return 0
    local -a seen=() uds=() f=()
    # shortcut: 90 s, not the Python's 45 s: under CPU load from other jobs the floor came after 45 s (drops were fine)
    local deadline=$((SECONDS + 90)) drops_seen=0 q ud v s t1 t2
    while [ "$SECONDS" -lt "$deadline" ]; do
        q=$(r_get user-data/vsr/quality)
        if [ -n "$q" ] && { [ ${#seen[@]} -eq 0 ] || [ "${seen[-1]}" != "$q" ]; }; then seen+=("$q"); fi
        v=$(_ad_drops)
        if (( v > drops_seen )); then drops_seen=$v; fi
        ud=$(_ad_ud)
        if [ -n "$ud" ] && { [ ${#uds[@]} -eq 0 ] || [ "${uds[-1]}" != "$ud" ]; }; then uds+=("$ud"); fi
        [ "$(r_state)" = failed ] && break
        sleep 0.25
    done
    r_wait 30 _ad_failed

    for s in "${seen[@]}"; do
        case $s in auto/high | medium | low | bicubic) f+=("$s") ;; esac
    done
    [ "${f[*]}" = "auto/high medium low bicubic" ]
    check "step-down ladder auto/high > medium > low > bicubic, never up (seen $(py_list "${seen[@]}"))" $? \
        "$(py_list "${seen[@]}")"
    [ "$(r_get user-data/vsr/steps)" = 3 ]
    check "steps counted (3 step-downs)" $? "$(r_get user-data/vsr/steps)"
    (( drops_seen > 5 ))
    check "frames really were dropped (frame-drop-count max $drops_seen)" $? "$drops_seen"
    [ "$(r_state)" = failed ] && [[ $(r_reason) == *"cannot keep up"* ]] && [ "$(r_labels)" = heavy ]
    check "at the floor SR is switched off for the file; the heavy user filter stays; video keeps playing" $? \
        "$(r_state) $(r_reason) $(r_labels)"
    [ "$(r_count 'SR restarted, quality ')" = 3 ] && _ad_logged "cannot keep up"
    check "each step is logged" $? "$(r_count 'SR restarted, quality ')"
    [ "${uds[*]}" = "quality=auto;pool=1 quality=medium;pool=1 quality=low;pool=1 quality=bicubic;pool=1" ]
    check "... each as a remove+re-add of the filter with new user-data" $? "${uds[*]}"
    t1=$(r_get time-pos 0); r_quiet 1.0; t2=$(r_get time-pos 0)
    _ad_advancing "$t1" "$t2"
    check "playback continues after giving up" $? "$t1 $t2"
    r_no_lua_errors adaptive
    r_close
}

_ad_no_adaptive() {
    r_open "${_AD_HEAVY[@]}" "${_AD_QUICK[@]}" --opt vsr-adaptive=no v720p30.mp4 || return 0
    r_wait_state on 20; r_quiet 8
    local drops
    drops=$(_ad_drops)
    [ "$(r_state)" = on ] && [ "$(r_get user-data/vsr/steps)" = 0 ] && (( drops > 5 ))
    check "adaptive=no: no step-down despite $(_ad_drops) dropped frames" $? \
        "$(r_state) $(r_get user-data/vsr/steps) $drops"
    r_close
}

_ad_pause() {
    r_open --extra --pause "${_AD_QUICK[@]}" v720p30.mp4 || return 0
    r_wait_state starting 20; r_quiet 5
    local s
    s=$(r_state)
    [ "$s" = starting ] || [ "$s" = on ]
    check "paused while starting: not declared failed after 5 s (confirm_timeout 3 s)" $? "$s $(r_reason)"
    r_ctl set pause false >/dev/null
    r_wait_state on 15
    check "... resumes and reaches 'on'" $? "$(r_state) $(r_reason)"
    r_ctl set pause true >/dev/null; r_quiet 4; r_ctl set pause false >/dev/null; r_quiet 2
    [ "$(r_state)" = on ] && [ "$(r_get user-data/vsr/steps)" = 0 ]
    check "pausing while on does not step down or fail" $? "$(r_state) $(r_get user-data/vsr/steps)"
    r_close
}

# idle and total jiffies of all cpus, from the first line of /proc/stat
_ad_cpu_times() { awk '/^cpu /{ t = 0; for (i = 2; i <= 9; i++) t += $i; print $5 + $6, t }' /proc/stat; }

group_adaptive() {
    wanted adaptive || return 0
    # shortcut: the heavy chain's timing depends on the CPU load, so a busy machine skips the group instead of
    # failing. One second of /proc/stat, not the 1-minute load average: that lags and counts this suite's own encoding.
    local i1 t1 i2 t2 busy
    read -r i1 t1 < <(_ad_cpu_times)
    sleep 1
    read -r i2 t2 < <(_ad_cpu_times)
    busy=$((100 * ((t2 - t1) - (i2 - i1)) / (t2 - t1)))
    if [ "$busy" -gt 40 ]; then
        echo "SKIP adaptive: machine busy (cpu $busy% in the last second)"
        return 0
    fi
    section "7. adaptive quality step-down and pause handling"
    _ad_ladder
    _ad_no_adaptive
    _ad_pause
}
