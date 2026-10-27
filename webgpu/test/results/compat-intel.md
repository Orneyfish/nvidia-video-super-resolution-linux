### Cross-origin video

| case | result | canvas not black | detail |
|---|---|---|---|
| same origin | ok | true |  |
| cross origin, no CORS headers, no crossorigin attr | cross-origin |  | video is cross-origin without CORS (tainted); serve it with Access-Control-Allow-Origin and crossorigin="anony |
| cross origin, ACAO:*, no crossorigin attr | cross-origin |  | video is cross-origin without CORS (tainted); serve it with Access-Control-Allow-Origin and crossorigin="anony |
| cross origin, ACAO:*, crossorigin=anonymous | ok | true |  |
| cross origin, no CORS headers, crossorigin=anonymous | video-load-error |  | video error MEDIA_ELEMENT_ERROR: Format error |

### Colour (16 flat patches read back from the canvas, error in 8-bit levels vs the RGB the clip was made from)

| clip tag | mode | max error | mean of per-patch max | worst patch (expected -> got) |
|---|---|---|---|---|
| bt709 | bilinear | 22 | 9.44 | 255,0,0 -> 233,0,2 |
| bt709 | fsr | 22 | 9.44 | 255,0,0 -> 233,0,2 |
| bt601 | bilinear | 70 | 18.19 | 0,255,255 -> 70,253,255 |
| bt601 | fsr | 70 | 18.19 | 0,255,255 -> 70,253,255 |
| bt709pc | bilinear | 23 | 10.13 | 255,0,0 -> 232,0,1 |
| bt709pc | fsr | 23 | 10.13 | 255,0,0 -> 232,0,1 |
| bt2020 | bilinear | 90 | 12.25 | 90,140,200 -> 0,144,205 |
| bt2020 | fsr | 90 | 12.25 | 90,140,200 -> 0,144,205 |

### Rotated video (display matrix 90)

| corner | WebGPU canvas | drawImage(video) |
|---|---|---|
| tl | 64,64,64 | 64,64,64 |
| tr | 41,178,91 | 41,178,91 |
| bl | 235,235,235 | 235,235,235 |
| br | 116,78,61 | 116,78,61 |

### Geometry: screenshot with only the <video> visible vs only the canvas visible (same CSS box)

| scene | video | canvas backing store | PSNR video vs canvas (dB) |
|---|---|---|---|
| contain, same aspect box | 852x480 | 1278x720 | 57.2 |
| contain, 4:3 box (letterbox) | 852x480 | 960x541 | 59.6 |
| cover, square box | 852x480 | 1278x720 | 58.9 |
| fill, 4:3 box (stretch) | 852x480 | 960x720 | 58.5 |
| CSS rotate(10deg) scale(.8) on the container | 852x480 | 1278x720 | 55.3 |
| CSS scaleX(-1) on the container | 852x480 | 1278x720 | 57.2 |

### Iframes / secure context

| case | isSecureContext | navigator.gpu | upscaler | detail |
|---|---|---|---|---|
| same-origin iframe | true | true | ok (intel) |  |
| cross-origin iframe (other port), no allow= | true | true | ok (intel) |  |
| iframe on an insecure origin (http://nvvsr.test) | false | false | unsupported | navigator.gpu is not available (isSecureContext=false) |

### EME (ClearKey, CENC-encrypted fMP4)

| loaded | video.mediaKeys set | upscaler | canvas not black | detail |
|---|---|---|---|---|
| false |  | Error |  | timeout waiting for loadeddata events: loadedmetadata,waitingforkey; {"videoReady":1,"videoError":null,"mediaSource":"ended"} |

### Picture-in-picture

| requestPictureInPicture | detail |
|---|---|
| ok | 284x160 |

### Fullscreen (canvas is hue-rotated so it can be told from the video)

| fullscreen element | document.fullscreenElement | what is on screen |
|---|---|---|
| <video> element | VIDEO#v | original video (canvas hidden) |
| container <div> with video + canvas | DIV#box | canvas (upscaled) |
