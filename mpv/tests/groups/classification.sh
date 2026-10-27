#!/bin/bash
# Group "classification": which video gets SR, and which does not.
# Table columns: name|file|expected state|reason fragment|labels (space-separated)|output size.

group_classification() {
    section "1. which video gets SR (display 2560x1440, fullscreen)"
    local cases name f state frag labels size extra
    cases='720p30 H.264|v720p30.mp4|on||nvvsr|2560x1440
1080p60 H.264|v1080p60.mp4|on||nvvsr|2560x1440
854x480 (aspect fit, even size)|v480p.mp4|on||nvvsr|2560x1438
10-bit HEVC SDR (mpv converts to 8-bit)|v720p10b_hevc.mp4|on||nvvsr-fmt nvvsr|2560x1440
10-bit H.264 SDR (mpv converts to 8-bit)|v720p10b_h264.mp4|on||nvvsr-fmt nvvsr|2560x1440
4:4:4 H.264 (mpv converts to 4:2:0)|v720p444.mp4|on||nvvsr-fmt nvvsr|2560x1440
240p (below min_height)|v240p.mp4|off|too small||320x240
1440p (needs no SR)|v1440p.mp4|off|not smaller||2560x1440
HDR10 (PQ, BT.2020)|hdr_pq.mp4|off|HDR||1280x720
HLG|hdr_hlg.mp4|off|HDR||1280x720
SDR tagged BT.2020|sdr_bt2020.mp4|off|BT.2020||1280x720
interlaced 1080i|v1080i.mp4|off|interlaced||1920x1080
interlaced 576i|v576i.mp4|off|interlaced||720x576'
    while IFS='|' read -r name f state frag labels size; do
        wanted class || wanted "$f" || continue
        r_open "$f" || continue
        if [ "$state" = on ]; then
            r_wait_state on 20
            check "$name: SR on" $? "state=$(r_state) reason=$(r_reason)"
            check_eq "$name: filters $(py_list $labels)" "$(r_labels)" "$labels"
            check_eq "$name: output $size" "$(r_out)" "$size"
            if [[ " $labels " == *" nvvsr-fmt "* ]]; then
                check_eq "$name: filter input is 8-bit 4:2:0" "$(r_get video-out-params/pixelformat)" yuv420p
            fi
        else
            r_wait_decided
            r_quiet 1.0
            [ "$(r_state)" = off ] && [[ $(r_reason) == *"$frag"* ]]
            check "$name: SR stays off ($frag)" $? "state=$(r_state) reason=$(r_reason)"
            [ -z "$(r_labels)" ] && [ "$(r_out)" = "$size" ]
            check "$name: no filter, output $size" $? "$(r_labels) $(r_out)"
        fi
        r_no_lua_errors "$name"
        r_close
    done <<<"$cases"

    while IFS='|' read -r name f extra frag; do
        wanted class || wanted "$f" || continue
        if [ -n "$extra" ]; then r_open --audio --extra "$extra" "$f"; else r_open --audio "$f"; fi || continue
        r_wait_decided
        r_quiet 0.8
        [ "$(r_state)" = off ] && [[ $(r_reason) == *"$frag"* ]] && [ -z "$(r_labels)" ]
        check "$name: SR stays off ($frag)" $? "state=$(r_state) reason=$(r_reason) vf=$(r_labels)"
        r_no_lua_errors "$name"
        r_close
    done <<'EOF2'
audio-only file|audio.m4a||no video
audio file with cover art|art.m4a||image or cover art
still image|still.png|--image-display-duration=inf|image or cover art
EOF2

    if wanted class || wanted window; then
        r_open --windowed v720p30.mp4 || return 0
        r_wait_decided
        r_quiet 1.0
        [ "$(r_state)" = off ] && [[ $(r_reason) == *"not fullscreen"* ]] && [ -z "$(r_labels)" ]
        check "windowed playback: SR off (not fullscreen), no filter" $? "$(r_state) $(r_reason) $(r_labels)"
        r_close

        r_open --nodisplay v720p30.mp4 || return 0
        r_wait_decided
        r_quiet 1.0
        [ "$(r_state)" = off ] && [[ $(r_reason) == *"display size unknown"* ]]
        check "display size unknown (no NVVSR_DISPLAY, vo=null): SR off with that reason" $? "$(r_state) $(r_reason)"
        r_close

        r_open --opt vsr-min_height=1000 v720p30.mp4 || return 0
        r_wait_decided
        [ "$(r_state)" = off ] && [[ $(r_reason) == *"too small"* ]]
        check "--script-opts vsr-min_height=1000 excludes 720p" $? "$(r_reason)"
        r_close

        r_open --opt vsr-enabled=no v720p30.mp4 || return 0
        r_wait_decided
        r_quiet 0.8
        [ "$(r_state)" = off ] && [ -z "$(r_labels)" ] && [[ $(r_reason) == *"vsr.conf"* ]]
        check "enabled=no: nothing happens by itself" $? "$(r_state) $(r_reason)"
        r_key ctrl+s
        r_wait_state on 15
        check "enabled=no: the toggle key still turns SR on for the file" $? "$(r_state) $(r_reason)"
        r_close
    fi
}
