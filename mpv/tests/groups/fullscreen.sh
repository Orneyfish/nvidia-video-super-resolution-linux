#!/bin/bash
# Group "fullscreen": SR follows fullscreen, keeps the user's own filters, and
# switches hwdec to its -copy variant while it runs and restores it afterwards. 33 checks.

_fs_user_entries() {  # the vf entries labelled "user", canonical JSON
    r_ctl get vf 2>/dev/null | jq -cS '[.[]? | select(.label == "user")]'
}

_fs_out_is() { [ "$(r_out)" = "$1" ]; }

_fs_cycle() {  # fullscreen on/off with a user filter in front, then the user's own `vf clr`
    r_open --extra "--vf=@user:lavfi=[eq=brightness=0.02]" v720p30.mp4 || return 0
    r_wait_state on 20
    check_eq "user's own vf entry kept, @nvvsr appended after it" "$(r_labels)" "user nvvsr"
    local user_before
    user_before=$(_fs_user_entries)

    r_ctl set fullscreen false
    r_wait_state off 8
    check "leaving fullscreen: state off" $? "state=$(r_state)"
    [ "$(r_labels)" = user ] && [ "$(_fs_user_entries)" = "$user_before" ]
    check "leaving fullscreen: @nvvsr removed, user's entry untouched" $? "$(r_get vf)"
    r_wait 4 _fs_out_is 1280x720
    check "leaving fullscreen: output is the source size again" $? "$(r_out)"
    [[ $(r_reason) == *"not fullscreen"* ]]
    check "reason says why" $? "$(r_reason)"

    r_ctl set fullscreen true
    r_wait_state on 15
    check "entering fullscreen again: SR back on" $? "$(r_state) $(r_reason)"
    [ "$(r_labels)" = "user nvvsr" ] && [ "$(r_out)" = 2560x1440 ]
    check "... with the same filter list and size" $? "$(r_labels) $(r_out)"

    r_cmd vf clr "" >/dev/null 2>&1
    r_wait_state off 6 && [[ $(r_reason) == *"removed by user"* ]]
    check "vf clr by the user: SR state off and it stays off" $? "$(r_state) $(r_reason)"
    r_ctl set fullscreen false >/dev/null; r_quiet 0.6
    r_ctl set fullscreen true >/dev/null; r_quiet 1.5
    [ "$(r_state)" = off ] && [ -z "$(r_labels)" ]
    check "... even after a fullscreen toggle" $? "$(r_state) $(r_labels)"
    r_no_lua_errors "fullscreen cycle"
    r_close
}

_fs_hwdec_case() {  # _fs_hwdec_case TITLE HW TARGET CURRENT: TARGET is the hwdec name while SR runs, CURRENT the
                    # decoder in use ("" = not checked)
    local title=$1 hw=$2 target=$3 current=$4 was i back adv st
    local -a samples=()
    r_open --extra "--hwdec=$hw" v720p30.mp4 || return 0
    for ((i = 0; i < 200; i++)); do
        [ "$(r_state)" = on ] && break
        samples+=("$(r_get time-pos 0)"); sleep 0.1
    done
    [ "$(r_state)" = on ]
    check "$title: SR on" $? "$(r_state) $(r_reason)"
    r_quiet 1.0
    [ "$(r_hwdec)" = "[\"$target\"]" ]
    check "$title: hwdec while SR runs = $(py_list "$target")" $? "$(r_hwdec)"
    if [ -n "$current" ]; then
        [ "$(r_get hwdec-current)" = "$current" ]
        check "$title: decoder in use = $current" $? "$(r_get hwdec-current)"
    fi
    was=$(r_get user-data/vsr/hwdec)
    [ "$was" = "$hw" ]
    check "$title: original value remembered ($hw)" $? "$was"
    for ((i = 0; i < 10; i++)); do samples+=("$(r_get time-pos 0)"); sleep 0.1; done
    r_ctl set fullscreen false >/dev/null
    r_wait_state off 8
    for ((i = 0; i < 15; i++)); do samples+=("$(r_get time-pos 0)"); sleep 0.1; done
    [ "$(r_hwdec)" = "[\"$hw\"]" ]
    check "$title: hwdec restored to [$hw] when SR stops" $? "$(r_hwdec)"
    read -r back adv < <(awk -v s="${samples[*]}" 'BEGIN {
        n = split(s, a, " "); back = 0
        for (i = 1; i < n; i++) if (a[i] - a[i + 1] > back) back = a[i] - a[i + 1]
        printf "%.2f %.1f\n", back, a[n] - a[1] }')
    awk -v b="$back" -v a="$adv" 'BEGIN { exit !(b < 0.5 && a > 1.5) }'; st=$?
    check "$title: playback position kept (largest step back ${back} s, advanced ${adv} s)" $st \
        "${samples[*]:0:3} ... ${samples[*]: -3}"
    r_no_lua_errors "$title"
    r_close
}

_fs_hwdec() {
    # --vo=null has no hardware interop, so a non-copy hwdec decodes in software here ("hwdec-current" is "no"):
    # the option handling is what is tested, not the decoder.
    _fs_hwdec_case "hwdec=nvdec" nvdec nvdec-copy nvdec-copy
    _fs_hwdec_case "hwdec=vaapi" vaapi vaapi-copy ""

    r_open --extra "--hwdec=auto" v720p30.mp4 || return 0
    r_wait_state on 20; r_quiet 0.8
    [ "$(r_hwdec)" = '["auto-copy"]' ] && [ "$(r_get hwdec-current)" = nvdec-copy ]
    check "hwdec=auto -> auto-copy while SR runs (decoding on nvdec-copy)" $? \
        "$(r_hwdec) $(r_get hwdec-current)"
    r_ctl set fullscreen false >/dev/null; r_wait_state off 8; r_quiet 0.5
    [ "$(r_hwdec)" = '["auto"]' ]
    check "hwdec=auto restored" $? "$(r_hwdec)"
    r_close

    local hw
    for hw in nvdec-copy no; do
        r_open --extra "--hwdec=$hw" v720p30.mp4 || continue
        r_wait_state on 20; r_quiet 0.5
        [ "$(r_hwdec)" = "[\"$hw\"]" ] && [ -z "$(r_get user-data/vsr/hwdec)" ]
        check "hwdec=$hw: left alone (already sw-memory)" $? "$(r_hwdec) $(r_get user-data/vsr/hwdec)"
        r_close
    done

    r_open --extra "--hwdec=nvdec" --opt vsr-hwdec=keep v720p30.mp4 || return 0
    r_wait_state on 20; r_quiet 0.5
    [ "$(r_hwdec)" = '["nvdec"]' ] && [ "$(r_state)" = on ]
    check "hwdec option 'keep': mpv's setting untouched, SR still on" $? "$(r_hwdec) $(r_state)"
    r_close

    r_open --extra "--hwdec=nvdec" --opt vsr-hwdec=vaapi-copy v720p30.mp4 || return 0
    r_wait_state on 20; r_quiet 0.5
    [ "$(r_hwdec)" = '["vaapi-copy"]' ] && [ "$(r_get hwdec-current)" = vaapi-copy ]
    check "hwdec option with an explicit value is used while SR runs" $? \
        "$(r_hwdec) $(r_get hwdec-current)"
    r_close

    r_open --extra "--hwdec=nvdec" v720p30.mp4 || return 0
    r_wait_state on 20; r_quiet 0.5
    r_ctl set hwdec '"no"' >/dev/null     # the user changes hwdec (Ctrl+h) while SR runs
    r_quiet 0.5
    r_ctl set fullscreen false >/dev/null; r_wait_state off 8; r_quiet 0.5
    [ "$(r_hwdec)" = '["no"]' ]
    check "a hwdec change made by the user meanwhile is not overwritten on restore" $? "$(r_hwdec)"
    r_close
}

group_fullscreen() {
    wanted fullscreen || wanted hwdec || return 0
    section "2. fullscreen on/off, user filters, hwdec"
    _fs_cycle
    _fs_hwdec
}
