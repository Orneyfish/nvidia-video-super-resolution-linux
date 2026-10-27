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
| bt709 | bilinear | 2 | 0.56 | 255,0,255 -> 253,0,254 |
| bt709 | fsr | 2 | 0.56 | 255,0,255 -> 253,0,254 |
| bt601 | bilinear | 70 | 18.13 | 0,255,255 -> 70,253,255 |
| bt601 | fsr | 70 | 18.13 | 0,255,255 -> 70,253,255 |
| bt709pc | bilinear | 2 | 1.31 | 235,235,235 -> 233,236,233 |
| bt709pc | fsr | 2 | 1.31 | 235,235,235 -> 233,236,233 |
| bt2020 | bilinear | 90 | 12.38 | 90,140,200 -> 0,144,205 |
| bt2020 | fsr | 90 | 12.38 | 90,140,200 -> 0,144,205 |

### Rotated video (display matrix 90)

| corner | WebGPU canvas | drawImage(video) |
|---|---|---|
| tl | 64,64,64 | 64,64,64 |
| tr | 30,160,89 | 30,160,89 |
| bl | 235,235,235 | 235,235,235 |
| br | 120,80,60 | 120,80,60 |

### Geometry: screenshot with only the <video> visible vs only the canvas visible (same CSS box)

| scene | video | canvas backing store | PSNR video vs canvas (dB) |
|---|---|---|---|
| contain, same aspect box | 852x480 | 1278x720 | NaN |
| contain, 4:3 box (letterbox) | 852x480 | 960x541 | NaN |
| cover, square box | 852x480 | 1278x720 | NaN |
| fill, 4:3 box (stretch) | 852x480 | 960x720 | NaN |
| CSS rotate(10deg) scale(.8) on the container | 852x480 | 1278x720 | NaN |
| CSS scaleX(-1) on the container | 852x480 | 1278x720 | NaN |

### Iframes / secure context

| case | isSecureContext | navigator.gpu | upscaler | detail |
|---|---|---|---|---|
| same-origin iframe | true | true | ok (nvidia) |  |
| cross-origin iframe (other port), no allow= | true | true | ok (nvidia) |  |
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
| <video> element | VIDEO#v | canvas (upscaled) |
| container <div> with video + canvas | DIV#box | canvas (upscaled) |
