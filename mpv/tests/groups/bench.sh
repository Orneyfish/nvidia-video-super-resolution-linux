#!/bin/bash
# Group "bench" (integration.py bench() and bench_untimed()): prints BENCH lines only, no checks. Runs only with
# run_groups.sh --only bench (not in the default set), so the numbers come from an idle machine with Ollama idle.

# true when the awk expression ($1, numbers only, from mpv or the clock) holds
_bn_true() { awk "BEGIN { exit !($1) }"; }

_bn_timed() {  # _bn_timed NAME FILE ADAPTIVE: 15 s of real-time playback, then one BENCH line
    local name=$1 f=$2 adaptive=$3 t0=$EPOCHREALTIME t_on t_start pos v wall mean min
    local -a fps=()
    r_open --extra --end=15 --opt vsr-adaptive="$adaptive" "$f" || { echo "BENCH $name: mpv did not start"; return 0; }
    if ! r_wait_state on 20; then
        echo "BENCH $name: SR did not start ($(r_state) $(r_reason))"; r_close; return 0
    fi
    t_on=$EPOCHREALTIME
    t_start=$(r_get time-pos 0)
    while r_alive; do
        pos=$(r_get time-pos 0)
        _bn_true "$pos < 14.5 && $EPOCHREALTIME - $t_on < 40" || break
        v=$(r_get estimated-vf-fps "")
        if [ -n "$v" ] && _bn_true "$v != 0 && $pos - $t_start > 2"; then fps+=("$v"); fi
        sleep 0.5
    done
    wall=$(awk -v a="$t0" -v b="$EPOCHREALTIME" 'BEGIN { printf "%.1f", b - a }')
    read -r mean min <<<"$(printf '%s\n' "${fps[@]}" | awk '{ s += $1; if (NR == 1 || $1 < m) m = $1 }
        END { printf "%.2f %.2f", NR ? s / NR : 0, NR ? m : 0 }')"
    printf 'BENCH %s: quality %s, container %.2f fps, estimated-vf-fps mean %s (min %s), dropped: vo=%s decoder=%s delayed=%s, steps=%s, wall %s s for %.1f s of video\n' \
        "$name" "$(r_get user-data/vsr/quality None)" "$(r_get container-fps 0)" "$mean" "$min" \
        "$(r_get frame-drop-count None)" "$(r_get decoder-frame-drop-count None)" "$(r_get vo-delayed-frame-count None)" \
        "$(r_get user-data/vsr/steps None)" "$wall" "$(r_get time-pos 0)"
    r_close
}

_bn_untimed() {  # _bn_untimed NAME FILE END LIMIT: --untimed throughput from time-pos over wall time, one BENCH line
    local name=$1 f=$2 end=$3 lim=$4 pos t first_t= first_p= last_t= last_p= fps_c speed fps
    r_open --extra --end="$end" --extra --untimed --opt vsr-adaptive=no "$f" || { echo "BENCH $name untimed: mpv did not start"; return 0; }
    if ! r_wait_state on 20; then
        echo "BENCH $name untimed: SR did not start ($(r_state) $(r_reason))"; r_close; return 0
    fi
    fps_c=$(r_get container-fps 0)
    while r_alive; do
        pos=$(r_get time-pos "")
        [ -n "$pos" ] || break
        t=$EPOCHREALTIME
        if [ -z "$first_t" ] && _bn_true "$pos > 1.0"; then first_t=$t; first_p=$pos; fi
        last_t=$t; last_p=$pos
        _bn_true "$pos >= $lim" && break
        sleep 0.1
    done
    if [ -n "$first_t" ] && _bn_true "$last_t > $first_t"; then
        read -r speed fps <<<"$(awk -v p1="$first_p" -v p2="$last_p" -v t1="$first_t" -v t2="$last_t" -v c="$fps_c" \
            'BEGIN { s = (p2 - p1) / (t2 - t1); printf "%.1f %.0f", s, s * c }')"
        echo "BENCH $name untimed: ${speed}x real time = ${fps} fps through mpv + SR (quality $(r_get user-data/vsr/quality None)), dropped: vo=$(r_get frame-drop-count None) decoder=$(r_get decoder-frame-drop-count None)"
    else
        echo "BENCH $name untimed: no measurement"
    fi
    r_close
}

group_bench() {
    section "9. real-time playback (--vo=null, 15 s each, default framedrop)"
    _bn_timed 720p30 v720p30.mp4 no
    _bn_timed "1080p60" v1080p60.mp4 no
    _bn_timed "1080p60 adaptive=yes" v1080p60.mp4 yes
    section "9b. throughput headroom (--untimed: as fast as the chain goes)"
    _bn_untimed 720p30 long720p30.mp4 60 59.4
    _bn_untimed 1080p60 v1080p60.mp4 15 14.4
}
