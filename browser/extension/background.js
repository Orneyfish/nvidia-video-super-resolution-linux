// NVVSR service worker: the only place that talks to the native host (com.nvvsr.mpv_handoff). It answers
// content scripts of this extension only, enforces the allowlist and a per-tab rate limit again, keeps the
// context menu and the toolbar toggle in step with the settings, and registers content scripts for extra
// allowlisted sites (those whose host permission the user granted on the options page).
importScripts("lib/logic.js");

const L = self.NvvsrLogic;
const MENU_ID = "nvvsr-open-mpv";
const DYNAMIC_PREFIX = "nvvsr-extra-";
const CONTENT_JS = ["lib/logic.js", "content.js"];
const limiter = new L.RateLimiter(L.HANDOFF_INTERVAL_MS);

async function getSettings() {
  const s = await chrome.storage.sync.get({ enabled: true, allowlist: L.DEFAULT_ALLOWLIST.slice() });
  return {
    enabled: s.enabled === true,
    allowlist: Array.isArray(s.allowlist) ? s.allowlist.filter(L.isValidAllowEntry) : [],
  };
}

function patternFor(entry) {
  return entry.startsWith("*.") ? "*://*." + entry.slice(2) + "/*" : "*://" + entry + "/*";
}

function hostnameOf(url) {
  try {
    return new URL(String(url)).hostname;
  } catch (e) {
    return "";
  }
}

async function ensureDefaults() {
  const cur = await chrome.storage.sync.get(["enabled", "allowlist"]);
  const set = {};
  if (typeof cur.enabled !== "boolean") set.enabled = true;
  if (!Array.isArray(cur.allowlist)) set.allowlist = L.DEFAULT_ALLOWLIST.slice();
  if (Object.keys(set).length) await chrome.storage.sync.set(set);
}

// "Open in mpv (super resolution)": only on pages of the allowlist, and nothing when handoff is off.
async function rebuildMenu() {
  const s = await getSettings();
  await chrome.contextMenus.removeAll();
  if (!s.enabled || s.allowlist.length === 0) return;
  chrome.contextMenus.create({
    id: MENU_ID,
    title: "Open in mpv (super resolution)",
    contexts: ["video"],
    documentUrlPatterns: s.allowlist.map(patternFor),
  });
}

// Content scripts for extra allowlisted sites. The defaults are static content scripts in the manifest.
async function syncDynamicScripts() {
  const s = await getSettings();
  const registered = await chrome.scripting.getRegisteredContentScripts();
  const stale = registered.filter((c) => c.id.startsWith(DYNAMIC_PREFIX)).map((c) => c.id);
  if (stale.length) await chrome.scripting.unregisterContentScripts({ ids: stale });
  const scripts = [];
  for (const entry of s.allowlist) {
    if (L.DEFAULT_ALLOWLIST.includes(entry)) continue;
    const origins = [patternFor(entry)];
    if (!(await chrome.permissions.contains({ origins: origins }))) continue;
    scripts.push({ id: DYNAMIC_PREFIX + entry, matches: origins, js: CONTENT_JS, runAt: "document_idle", allFrames: false });
  }
  if (scripts.length) await chrome.scripting.registerContentScripts(scripts);
}

async function refreshBadge() {
  const s = await getSettings();
  await chrome.action.setBadgeText({ text: s.enabled ? "" : "OFF" });
  await chrome.action.setBadgeBackgroundColor({ color: "#777777" });
  await chrome.action.setTitle({ title: s.enabled ? "NVVSR mpv handoff: on (click to turn off)" : "NVVSR mpv handoff: off (click to turn on)" });
}

async function refreshAll() {
  await refreshBadge();
  await rebuildMenu();
  await syncDynamicScripts();
}

chrome.runtime.onInstalled.addListener(() => { ensureDefaults().then(refreshAll).catch(() => {}); });
chrome.runtime.onStartup.addListener(() => { refreshAll().catch(() => {}); });

chrome.storage.onChanged.addListener((changes, area) => {
  if (area === "sync" && (changes.enabled || changes.allowlist)) refreshAll().catch(() => {});
});

chrome.action.onClicked.addListener(async () => {
  const s = await getSettings();
  await chrome.storage.sync.set({ enabled: !s.enabled });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId !== MENU_ID || !tab || tab.id < 0) return;
  chrome.tabs.sendMessage(tab.id, { type: "contextHandoff" }, { frameId: info.frameId }).catch(() => {});
});

chrome.tabs.onRemoved.addListener((tabId) => limiter.forget("tab:" + tabId));

function nativeCall(message) {
  return new Promise((resolve) => {
    chrome.runtime.sendNativeMessage(L.HOST_NAME, message, (reply) => {
      const err = chrome.runtime.lastError;
      if (err) {
        resolve({ ok: false, time: null, ended: false, error: L.hostErrorFrom(err.message) });
        return;
      }
      resolve(reply || { ok: false, time: null, ended: false, error: L.HOST_ERRORS.failed });
    });
  });
}

async function handleHandoff(raw, sender) {
  const s = await getSettings();
  const page = L.checkPage(hostnameOf(sender.url), s);
  if (!page.ok) return { ok: false, time: null, ended: false, error: "handoff not allowed on this site" };
  const req = L.validateRequest(raw);
  if (!req) return { ok: false, time: null, ended: false, error: "invalid request" };
  const gate = limiter.check("tab:" + sender.tab.id, Date.now());
  if (!gate.ok) return { ok: false, time: null, ended: false, error: "too many handoffs, wait a moment" };
  return nativeCall(req);
}

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  // Only this extension's own parts may talk to us (externally_connectable is not set, so pages cannot).
  if (!sender || sender.id !== chrome.runtime.id || !msg || typeof msg !== "object") return false;
  if (msg.type === "ping" && !sender.tab) {
    nativeCall({ op: "ping" }).then(sendResponse);
    return true;
  }
  if (msg.type === "handoff") {
    // Content scripts of a tab only: a tab, the top frame, and a page URL (not an extension page).
    if (!sender.tab || sender.tab.id < 0 || sender.frameId !== 0) return false;
    if (typeof sender.url !== "string" || sender.url.startsWith("chrome-extension://")) return false;
    handleHandoff(msg.request, sender).then(sendResponse);
    return true;
  }
  return false;
});
