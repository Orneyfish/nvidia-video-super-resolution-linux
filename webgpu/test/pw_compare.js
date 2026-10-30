// Playwright check of the extension on a real page: plays a YouTube video at a chosen quality in a headed Chromium (run it inside a
// headless cage: tools/.../cage_env.sh), device scale factor 2 so a 1280x720 window gives 2560x1440 device pixels, lets the
// extension upscale (#nvvsr-force: no fullscreen needed), pauses, and saves the video element with the upscaled canvas shown and
// hidden at full resolution. usage: NODE_PATH=<dir with playwright> node pw_compare.js VIDEO_ID QUALITY OUT_DIR
//   QUALITY: YouTube's names (tiny=144p small=240p medium=360p large=480p hd720 hd1080)
const { chromium } = require('playwright');
const path = require('path');
const [id = 'aqz-KE-bpKQ', quality = 'hd720', out = '.'] = process.argv.slice(2);
const ext = path.resolve(__dirname, '../extension');

(async () => {
  const ctx = await chromium.launchPersistentContext(path.join(out, 'profile'), {
    headless: false, viewport: { width: 1280, height: 720 }, deviceScaleFactor: 2,
    args: [`--disable-extensions-except=${ext}`, `--load-extension=${ext}`, '--ozone-platform=wayland', '--ignore-gpu-blocklist', ...(process.env.PW_ARGS || '').split(' ').filter(Boolean),
           '--autoplay-policy=no-user-gesture-required'],
  });
  const page = ctx.pages()[0] || await ctx.newPage();
  const sw = ctx.serviceWorkers().length, bg = ctx.backgroundPages?.().length;
  const logs = [];
  page.on('console', m => { if (m.text().includes('[nvvsr]')) logs.push(m.text().slice(0, 300)); });
  await page.goto(`https://www.youtube.com/watch?v=${id}#nvvsr-debug&nvvsr-force`);
  await page.waitForSelector('video', { timeout: 30000 });
  await page.evaluate(q => { const p = document.getElementById('movie_player'); p.setPlaybackQualityRange(q, q); p.mute?.(); }, quality);
  await page.waitForTimeout(16000);  // quality switch + the extension starts 8 s after load
  const info = await page.evaluate(() => { const v = document.querySelector('video'), c = v.nextElementSibling;
    v.pause(); return { res: [v.videoWidth, v.videoHeight], canvas: c && c.tagName === 'CANVAS' ? [c.width, c.height] : null,
                        box: [Math.round(v.getBoundingClientRect().width), Math.round(v.getBoundingClientRect().height)], dpr: devicePixelRatio }; });
  await page.waitForTimeout(1500);
  const video = page.locator('video');
  await video.screenshot({ path: path.join(out, 'on.png') });
  await page.evaluate(() => { const c = document.querySelector('video').nextElementSibling; if (c) c.style.visibility = 'hidden'; });
  await page.waitForTimeout(500);
  await video.screenshot({ path: path.join(out, 'off.png') });
  console.log(JSON.stringify({ info, logs: logs.slice(-4) }));
  await ctx.close();
})().catch(e => { console.error('FAILED', e.message); process.exit(1); });
