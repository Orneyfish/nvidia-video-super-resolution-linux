// Runs in an extension-origin iframe that content.js injects into the page. The WebSocket to the local daemon is opened from here
// because a public page origin (youtube.com) is not allowed to reach 127.0.0.1, even from its content script. Frames cross a
// MessageChannel as ArrayBuffers (copied in, transferred out). The URL is fixed: this page must not become a general proxy.
addEventListener('message', e => {
  const port = e.ports[0];
  if (e.source !== parent || !port) return;
  const ws = new WebSocket('ws://127.0.0.1:41777');
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => port.postMessage({ t: 'open' });
  ws.onclose = ws.onerror = () => port.postMessage({ t: 'close' });
  ws.onmessage = m => port.postMessage({ t: 'msg', d: m.data }, typeof m.data === 'string' ? [] : [m.data]);
  port.onmessage = m => { if (m.data === 'close') ws.close(); else if (ws.readyState === 1) ws.send(m.data); };
}, { once: true });
