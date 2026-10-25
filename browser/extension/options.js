// Options page: allowlist, enable switch, host check. Runs as an extension page (no inline script, no remote code).
(function () {
  "use strict";
  const L = globalThis.NvvsrLogic;
  const $ = (id) => document.getElementById(id);

  function status(text, cls) {
    const el = $("status");
    el.textContent = text;
    el.className = cls || "";
  }

  function patternFor(entry) {
    return entry.startsWith("*.") ? "*://*." + entry.slice(2) + "/*" : "*://" + entry + "/*";
  }

  async function load() {
    const s = await chrome.storage.sync.get({ enabled: true, allowlist: L.DEFAULT_ALLOWLIST.slice() });
    $("enabled").checked = s.enabled === true;
    $("allowlist").value = (Array.isArray(s.allowlist) ? s.allowlist : []).join("\n");
  }

  // Extra sites need a host permission; the defaults come with the manifest.
  async function save(entries, enabled) {
    const extras = entries.filter((e) => !L.DEFAULT_ALLOWLIST.includes(e));
    const origins = extras.map(patternFor);
    if (origins.length) {
      const granted = await chrome.permissions.request({ origins: origins });
      if (!granted) {
        status("Not saved: Chrome did not grant access to " + extras.join(", ") + ".", "err");
        return false;
      }
    }
    const previous = await chrome.storage.sync.get({ allowlist: [] });
    const dropped = (previous.allowlist || []).filter((e) => !entries.includes(e) && !L.DEFAULT_ALLOWLIST.includes(e));
    if (dropped.length) {
      await chrome.permissions.remove({ origins: dropped.map(patternFor) }).catch(() => {});
    }
    await chrome.storage.sync.set({ enabled: enabled, allowlist: entries });
    return true;
  }

  $("save").addEventListener("click", async () => {
    const parsed = L.parseAllowlist($("allowlist").value);
    if (parsed.invalid.length) {
      status("Not saved. Invalid entries: " + parsed.invalid.join(", ") + ". Use hostnames such as example.com.", "err");
      return;
    }
    if (parsed.entries.length === 0) {
      status("Not saved: the allowlist is empty.", "err");
      return;
    }
    try {
      if (await save(parsed.entries, $("enabled").checked)) {
        status("Saved.", "ok");
        $("allowlist").value = parsed.entries.join("\n");
      }
    } catch (e) {
      status("Not saved: " + String(e && e.message || e), "err");
    }
  });

  $("reset").addEventListener("click", async () => {
    try {
      if (await save(L.DEFAULT_ALLOWLIST.slice(), true)) {
        await load();
        status("Defaults restored.", "ok");
      }
    } catch (e) {
      status("Not reset: " + String(e && e.message || e), "err");
    }
  });

  $("check").addEventListener("click", async () => {
    status("Checking the native host...");
    const reply = await chrome.runtime.sendMessage({ type: "ping" }).catch(() => null);
    if (reply && reply.ok === true) {
      status("Native host OK (protocol " + reply.version + ").", "ok");
    } else {
      const why = reply && reply.error === L.HOST_ERRORS.missing
        ? "not installed: run install/ (Phase 6) to register com.nvvsr.mpv_handoff"
        : "failed: " + (reply && reply.error || "no answer");
      status("Native host " + why, "err");
    }
  });

  load();
})();
