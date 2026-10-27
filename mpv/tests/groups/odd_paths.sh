#!/bin/bash
# Group "odd_paths": installation layouts (odd script directory, config directory) and where vsr.vpy looks for the
# plugin (next to the script, ../lib, ../vapoursynth/build, NVVSR_PLUGIN).

_vf_param() {  # _vf_param LABEL KEY: a parameter of the vf entry with that label, empty when there is none
    r_ctl get vf 2>/dev/null | jq -r --arg l "$1" --arg k "$2" '.[]? | select(.label == $l) | .params[$k] // empty' | head -n 1
}

_vpy_playing() { local t; t=$(r_get time-pos 0); [ "${t%.*}" -ge 1 ] 2>/dev/null; }

_vpy_lookup() {  # _vpy_lookup NAME SCRIPT WANT [PLUGIN]: mpv plays with SCRIPT as vsr.vpy, NVVSR_PLUGIN unset (or PLUGIN)
    local name=$1 script=$2 want=$3 plugin=${4-} saved=$NVVSR_PLUGIN log p=1
    unset NVVSR_PLUGIN
    if r_open --script "" --extra "--vf=vapoursynth=file=$script" ${plugin:+--env "NVVSR_PLUGIN=$plugin"} v720p30.mp4; then
        r_wait 10 _vpy_playing; p=$?
        log=$(r_log | grep -F 'vsr.vpy: pass-through' | head -n 1)
        [ $p = 0 ] && [[ $log == *"Failed to load $want"* ]]
        check "$name" $? "playing=$p log: $log"
        r_close
    fi
    export NVVSR_PLUGIN=$saved
}

group_odd_paths() {
    wanted path || return 0
    section "5. installation layouts"
    local weird="$T_SCR/dir with space:colon" cfg=$T_SCR/cfg on ud

    rm -rf "$weird"; mkdir -p "$weird"
    cp "$T_REPO/mpv/vsr.lua" "$T_REPO/mpv/vsr.vpy" "$weird/"
    if r_open --script "$weird/vsr.lua" v720p30.mp4; then
        r_wait_state on 20 && [ "$(r_out)" = 2560x1440 ]
        check "script directory with a space and a colon: SR on (file= is length-prefixed)" $? "$(r_state) $(r_reason)"
        [ "$(_vf_param nvvsr file)" = "$weird/vsr.vpy" ]
        check "... vsr.vpy found next to vsr.lua" $? "$(_vf_param nvvsr file)"
        r_close
    fi

    rm -rf "$cfg"; mkdir -p "$cfg/scripts" "$cfg/script-opts"
    cp "$T_REPO/mpv/vsr.lua" "$T_REPO/mpv/vsr.vpy" "$cfg/scripts/"
    cp "$T_REPO/mpv/script-opts/vsr.conf" "$cfg/script-opts/"
    : >"$cfg/mpv.conf"
    if r_open --script "" --config-dir "$cfg" v720p30.mp4; then
        r_wait_state on 20 && [ "$(r_out)" = 2560x1440 ]
        check "auto-loaded from <config>/scripts with the shipped vsr.conf (all defaults commented): SR on" $? "$(r_state) $(r_reason)"
        r_close
    fi

    printf '# test\nmin_height=1000\nac_only=no\nquality=high\n' >"$cfg/script-opts/vsr.conf"
    if r_open --script "" --config-dir "$cfg" v720p30.mp4; then
        r_wait_decided
        [ "$(r_state)" = off ] && [[ $(r_reason) == *"too small (720p < 1000p)"* ]]
        check "script-opts/vsr.conf is read (min_height=1000 excludes 720p)" $? "$(r_reason)"
        r_close
    fi

    printf 'quality=high\n' >"$cfg/script-opts/vsr.conf"
    if r_open --script "" --config-dir "$cfg" v480p.mp4; then
        r_wait_state on 20
        ud=$(_vf_param nvvsr user-data)
        [ "$ud" = "quality=high;pool=1" ]
        check "vsr.conf quality=high reaches the filter's user-data" $? "$ud"
        r_close
    fi

    if r_open --script "" --config-dir "$cfg" --opt vsr-quality=low --opt vsr-pool=2 --opt vsr-strength=0.5 v480p.mp4; then
        r_wait_state on 20; on=$?
        ud=$(_vf_param nvvsr user-data)
        [ $on = 0 ] && [ "$ud" = "quality=low;pool=2;strength=0.5" ] && [ "$(r_out)" = 2560x1438 ]
        check "--script-opts overrides vsr.conf (quality=low, pool=2, strength=0.5) and the plugin accepts it" $? \
            "$on $ud $(r_out)"
        r_close
    fi

    if r_open --opt vsr-quality=streaming_ultra v480p.mp4; then
        r_wait_state on 20; on=$?
        ud=$(_vf_param nvvsr user-data)
        [ $on = 0 ] && [ "$ud" = "quality=streaming_ultra;pool=1" ]
        check "quality=streaming_ultra is passed through and accepted" $? "$on $ud"
        r_close
    fi

    # Empty .so files make LoadPlugin fail; the pass-through line names the path vsr.vpy chose.
    local lk=$T_SCR/lookup l
    rm -rf "$lk"
    for l in lay1 lay2 lay3; do mkdir -p "$lk/$l/mpv"; cp "$T_REPO/mpv/vsr.vpy" "$lk/$l/mpv/"; done
    mkdir -p "$lk/lay1/lib" "$lk/lay2/lib"
    : >"$lk/lay1/lib/libvsnvvsr.so"; : >"$lk/lay2/lib/libvsnvvsr.so"; : >"$lk/lay2/mpv/libvsnvvsr.so"
    _vpy_lookup "vsr.vpy plugin lookup: <script>/../lib/libvsnvvsr.so (installed layout)" \
        "$lk/lay1/mpv/vsr.vpy" "$lk/lay1/lib/libvsnvvsr.so"
    _vpy_lookup "vsr.vpy plugin lookup: next to the script wins" \
        "$lk/lay2/mpv/vsr.vpy" "$lk/lay2/mpv/libvsnvvsr.so"
    _vpy_lookup "vsr.vpy plugin lookup: none found -> the development path" \
        "$lk/lay3/mpv/vsr.vpy" "$lk/lay3/vapoursynth/build/libvsnvvsr.so"
    _vpy_lookup "vsr.vpy plugin lookup: NVVSR_PLUGIN wins" \
        "$lk/lay1/mpv/vsr.vpy" /x/y.so /x/y.so
}
