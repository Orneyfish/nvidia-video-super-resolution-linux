#!/bin/bash
# Bash port of the helpers of mpv/tests/integration.py (sourced by run_groups.sh, not run directly).
# A check prints PASS/FAIL and counts; a run is one headless mpv (--vo=null --ao=null) driven through mpvctl.
# Needs from the driver: T_REPO, T_SCR (scratch dir), T_MPV (mpv binary), T_MEDIA (test clips), MPVCTL (built
# mpvctl), NVVSR_LIBDIR, NVVSR_PLUGIN. Optional NVVSR_ONLY_TAGS: space-separated; wanted() skips other tags.

PASSED=0
FAILED=0
read -r -a ONLY_TAGS <<<"${NVVSR_ONLY_TAGS:-}"
R_PID=
R_DIR=
R_SOCK=m.sock
R_LOG=

check() {  # check NAME STATUS DETAIL: STATUS is the exit status of the test (0 = pass)
    if [ "$2" = 0 ]; then
        PASSED=$((PASSED + 1)); echo "PASS $1"
    else
        FAILED=$((FAILED + 1)); echo "FAIL $1: $3"
    fi
}

check_eq() {  # check_eq NAME GOT WANT
    if [ "$2" = "$3" ]; then check "$1" 0 ""; else check "$1" 1 "got '$2', want '$3'"; fi
}

section() { echo "== $1"; }

wanted() {  # wanted TAG: true when no tags are selected or one of them occurs in TAG
    [ ${#ONLY_TAGS[@]} -eq 0 ] && return 0
    local o
    for o in "${ONLY_TAGS[@]}"; do [[ $1 == *"$o"* ]] && return 0; done
    return 1
}

py_list() {  # py_list WORD...: Python's repr of a list of strings, so check names match integration.py
    local w s=
    for w in "$@"; do s+="${s:+, }'$w'"; done
    printf '[%s]' "$s"
}

# ---- one headless mpv -------------------------------------------------------------------------------------
r_ctl() { (cd "$R_DIR" && "$MPVCTL" "$R_SOCK" "$@"); }   # the socket name is relative: sun_path is short

r_get() {  # r_get PROP [DEFAULT]: the value as text (strings unquoted), DEFAULT when mpv has none
    local v
    if v=$(r_ctl get "$1" 2>/dev/null); then
        jq -r --arg d "${2-}" 'if . == null then $d elif type == "string" then . else tojson end' <<<"$v"
    else
        printf '%s\n' "${2-}"
    fi
}

r_state() { r_get user-data/vsr/state; }
r_reason() { r_get user-data/vsr/reason; }
r_out() { printf '%sx%s\n' "$(r_get video-out-params/w None)" "$(r_get video-out-params/h None)"; }
r_hwdec() { r_get hwdec; }

r_labels() {  # r_labels: the vf labels separated by spaces ("" when there is no filter)
    local v
    v=$(r_ctl get vf 2>/dev/null) || v='[]'
    jq -r '[.[]? | (.label // "?")] | join(" ")' <<<"$v"
}

r_cmd() {  # r_cmd ARG...: one IPC command; arguments that look like numbers are sent as numbers
    local a j='[' sep=
    for a in "$@"; do
        if [[ $a =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; then j+="$sep$a"; else j+="$sep$(jq -Rn --arg s "$a" '$s')"; fi
        sep=,
    done
    r_ctl cmd "$j]"
}

r_key() { r_cmd keypress "$1" >/dev/null 2>&1; }

r_alive() {  # mpv runs (an exited but not yet reaped child, a zombie, does not count)
    local st=
    [ -n "$R_PID" ] && read -r _ _ st _ 2>/dev/null </proc/$R_PID/stat
    [ -n "$st" ] && [ "$st" != Z ]
}

r_log() { cat "$R_LOG" 2>/dev/null; }

r_quiet() { sleep "${1:-1.0}"; }

r_wait() {  # r_wait TIMEOUT CMD...: polls CMD every 0.1 s; status 0 when it succeeds, 1 on timeout or exit
    local i
    for ((i = 0; i < $1 * 10; i++)); do
        "${@:2}" && return 0
        r_alive || return 1
        sleep 0.1
    done
    return 1
}

_state_is() { [ "$(r_state)" = "$1" ]; }
r_wait_state() { r_wait "${2:-15}" _state_is "$1"; }

_decided() {  # the script has evaluated the file once
    local why
    why=$(r_reason)
    [ -n "$why" ] && [ "$why" != "file loaded" ] && [ "$why" != "not started" ]
}
r_wait_decided() { r_wait "${1:-10}" _decided; }

r_close() {  # quit, wait up to 5 s, then kill and reap
    [ -n "$R_PID" ] || return 0
    r_alive && r_cmd quit >/dev/null 2>&1
    local i
    for ((i = 0; i < 50; i++)); do r_alive || break; sleep 0.1; done
    r_alive && kill -9 "$R_PID" 2>/dev/null
    wait "$R_PID" 2>/dev/null
    R_PID=
}

_r_cleanup() { r_close; return 0; }
trap _r_cleanup EXIT
trap 'exit 130' INT TERM

r_open() {  # r_open [--windowed] [--audio] [--nodisplay] [--script P] [--config-dir D] [--opt K=V]...
            #        [--extra ARG]... [--env K=V]... FILE...
    # Same options and environment as integration.py Run.__init__ (fullscreen, no audio, display 2560x1440).
    local fs=1 audio=0 display=2560x1440 script=$T_REPO/mpv/vsr.lua config_dir= f k i
    local -A opts=([vsr-confirm_timeout]=3)
    local -a extra=() envs=() files=() args=() env_cmd
    while [ $# -gt 0 ]; do
        case $1 in
            --windowed) fs=0 ;;
            --audio) audio=1 ;;
            --nodisplay) display= ;;
            --script) script=$2; shift ;;
            --config-dir) config_dir=$2; shift ;;
            --opt) opts[${2%%=*}]=${2#*=}; shift ;;
            --extra) extra+=("$2"); shift ;;
            --env) envs+=("$2"); shift ;;
            *) files+=("$1") ;;
        esac
        shift
    done
    args=(--vo=null --ao=null --idle=yes --keep-open=yes --msg-level=all=info,vsr=v --osd-level=1)
    if [ -n "$config_dir" ]; then args+=(--config-dir="$config_dir"); else args+=(--no-config); fi
    [ $audio = 1 ] || args+=(--no-audio)
    [ $fs = 1 ] && args+=(--fs)
    [ -n "$script" ] && args+=(--script="$script")
    local sopts=
    for k in "${!opts[@]}"; do sopts+="${sopts:+,}$k=${opts[$k]}"; done
    args+=(--script-opts="$sopts")
    args+=("${extra[@]}")
    for f in "${files[@]}"; do
        case $f in /*) args+=("$f") ;; *) args+=("$T_MEDIA/$f") ;; esac
    done
    env_cmd=(env -u NVVSR_DISPLAY)
    [ -n "$display" ] && env_cmd+=("NVVSR_DISPLAY=$display")
    env_cmd+=("${envs[@]}")

    R_DIR=$T_SCR
    R_LOG=$R_DIR/mpv.log
    rm -f "$R_DIR/$R_SOCK"
    ( cd "$R_DIR" && exec "${env_cmd[@]}" "$T_MPV" --input-ipc-server="$R_SOCK" "${args[@]}" ) </dev/null >"$R_LOG" 2>&1 &
    R_PID=$!

    for ((i = 0; i < 150; i++)); do
        r_ctl get mpv-version >/dev/null 2>&1 && return 0
        r_alive || break
        sleep 0.1
    done
    check "mpv starts (${files[*]})" 1 "no IPC socket, see $R_LOG"
    r_close
    return 1
}

r_no_lua_errors() {  # r_no_lua_errors NAME: the log has no Lua error and mpv still runs
    local bad
    bad=$(r_log | grep -E 'Lua error|stack traceback' | head -2)
    [ -z "$bad" ]; check "$1: no Lua errors in the mpv log" $? "$bad"
    r_alive; check "$1: mpv still running" $? "mpv exited"
}

r_count() { r_log | grep -o -F -- "$1" | wc -l; }  # r_count TEXT: occurrences of TEXT in the mpv log (str.count)

r_hook_plugin() {  # r_hook_plugin: builds the plugin with the test hook into $T_SCR; prints its path, nothing on failure
    local b=$T_SCR/hook-build
    cmake -S "$T_REPO/vapoursynth" -B "$b" -DNVVSR_VS_TESTHOOK=ON -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 &&
        cmake --build "$b" -j >/dev/null 2>&1 && echo "$b/libvsnvvsr.so"
}
