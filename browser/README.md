# browser/: Chrome handoff to mpv (super resolution)

A Manifest V3 extension and a native-messaging host. On an allowlisted site (YouTube, Vimeo, Twitch by default),
when a video goes fullscreen after a click or key press and its picture is smaller than the screen, the extension
pauses the page video, leaves fullscreen and opens the page in the private mpv (`mpv-vsr`). mpv runs fullscreen with
super resolution (`mpv/vsr.lua`). When mpv quits, the page video seeks to the same time and stays paused.

Files: `extension/` (manifest, service worker, content script, options page, `lib/logic.js` shared decisions),
`host/nvvsr_host.py` (native host, Python 3 stdlib only), `host/com.nvvsr.mpv_handoff.json.in` (host manifest template),
`tests/` (headless tests).

## Limitations

- **DRM sites cannot be handed off.** Netflix, Disney+, Prime Video, Hulu, Max and similar sites, and any video with
  `mediaKeys` set, stay in the browser. mpv cannot decrypt their video.
- The host must be installed for Chrome (`com.nvvsr.mpv_handoff`). Without it the page goes back to ordinary
  fullscreen and a toast says why.
- Only the top frame of a page is handled. Videos inside cross-origin iframes are not.
- The extension pins its ID with a `key` in `manifest.json`: `dfdalgcnmhhegbimecinmffikmkdmjee`. The host manifest
  allows only that origin. The private key is not in this repository.

## Run the tests (headless, no Chrome, no GPU)

```sh
browser/tests/run.sh
```

Prints `PASS`/`FAIL`/`SKIP` per test and a summary; exit status 1 on any failure. The mpv tests use the private
mpv with `--vo=null --ao=null --no-audio --hwdec=no` on a local range-capable HTTP server (127.0.0.1 only). They
generate a 20 s clip with ffmpeg, or use `NVVSR_TEST_CLIP`. `NVVSR_GPU_LOCK` makes the run wait while another agent
holds a GPU-measurement lock file.

## Install for Chrome (the one visible test is yours)

Phase 6 (`install/`) installs the host manifest. Until then, a manual copy that you run yourself:

1. Write the host manifest with the real path, e.g.
   `sed "s#@HOST_PATH@#$PWD/browser/host/nvvsr_host.py#" browser/host/com.nvvsr.mpv_handoff.json.in > /tmp/com.nvvsr.mpv_handoff.json`
2. `cp /tmp/com.nvvsr.mpv_handoff.json ~/.config/google-chrome/NativeMessagingHosts/`
   (other Chrome profiles use their own `NativeMessagingHosts` directory).
3. Open `chrome://extensions`, enable Developer mode, "Load unpacked", choose `browser/extension/`.
   Its ID must be `dfdalgcnmhhegbimecinmffikmkdmjee`.
4. Options page: "Check the native host" should say OK.

## The visible test

1. Play a 720p YouTube video in Chrome (a 1080p or lower source on a 1440p panel).
2. Enter fullscreen (the button or `f`).
3. Expected: the page leaves fullscreen and mpv opens fullscreen with super resolution (`Alt+s` shows the stats).
4. Quit mpv (`q`) at a point you can recognise.
5. Expected: the Chrome tab shows the video paused at the same time, no toast.

If something fails, the page goes back to fullscreen and a toast says why. Logs: `$XDG_STATE_HOME/nvvsr/handoff.log`
(default `~/.local/state/nvvsr/handoff.log`). URLs there keep only the host, path and the `v`, `list`, `t`, `index`,
`start` and `time_continue` parameters.

## Configuration

Environment (or `~/.config/nvvsr/handoff.json`, environment wins): `NVVSR_MPV` (mpv binary, default
`~/.local/opt/mpv-vsr/bin/mpv`), `NVVSR_VSR_LUA` (default `mpv/vsr.lua` of this repository), `NVVSR_LIBDIR` (the
NVIDIA wheel libraries, default from `.venv`), `NVVSR_PANEL_H` / `panel_h` (panel height for the yt-dlp format cap,
default 1440), `use_chrome_cookies` (JSON `true` adds `--ytdl-raw-options=cookies-from-browser=chrome`, default off).
The toolbar button turns handoff on or off; "Open in mpv (super resolution)" appears on the video context menu of
allowlisted sites.
