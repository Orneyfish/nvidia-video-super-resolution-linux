// Manifest and lint checks for browser/extension and browser/host. Run: node --test browser/tests/manifest.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const vm = require("node:vm");
const L = require("../extension/lib/logic.js");

const BROWSER = path.join(__dirname, "..");
const EXT = path.join(BROWSER, "extension");
const HOST = path.join(BROWSER, "host");
const read = (p) => fs.readFileSync(p, "utf8");
const manifest = JSON.parse(read(path.join(EXT, "manifest.json")));

// Chrome's extension ID: first 128 bits of SHA-256 of the DER public key, hex digits mapped to a..p.
function extensionIdFromKey(b64) {
  const digest = crypto.createHash("sha256").update(Buffer.from(b64, "base64")).digest().subarray(0, 16);
  return [...digest.toString("hex")].map((c) => String.fromCharCode("a".charCodeAt(0) + parseInt(c, 16))).join("");
}

function jsFiles(dir) {
  const out = [];
  for (const name of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, name.name);
    if (name.isDirectory()) out.push(...jsFiles(p));
    else if (/\.(js|html)$/.test(name.name)) out.push(p);
  }
  return out;
}

test("manifest: valid MV3 JSON with a service worker and classic scripts", () => {
  assert.equal(manifest.manifest_version, 3);
  assert.equal(manifest.background.service_worker, "background.js");
  assert.equal(manifest.background.type, undefined, "classic worker (importScripts), not a module");
  assert.ok(fs.existsSync(path.join(EXT, "background.js")));
});

test("manifest: permissions are exactly the four the design needs", () => {
  assert.deepEqual([...manifest.permissions].sort(), ["contextMenus", "nativeMessaging", "scripting", "storage"]);
  for (const forbidden of ["tabs", "activeTab", "webRequest", "cookies", "<all_urls>", "debugger", "history"]) {
    assert.equal(manifest.permissions.includes(forbidden), false, forbidden);
  }
});

test("manifest: host permissions are only the default allowlisted sites", () => {
  const expected = ["*://youtube.com/*", "*://www.youtube.com/*", "*://m.youtube.com/*", "*://youtu.be/*",
    "*://vimeo.com/*", "*://twitch.tv/*"];
  assert.deepEqual([...manifest.host_permissions].sort(), [...expected].sort());
  for (const entry of L.DEFAULT_ALLOWLIST) {
    assert.ok(manifest.host_permissions.includes("*://" + entry + "/*"), entry);
  }
});

test("manifest: optional host permissions are the only broad grant, and only on request", () => {
  assert.deepEqual(manifest.optional_host_permissions, ["*://*/*"]);
  assert.equal(manifest.externally_connectable, undefined, "no page may message the extension");
  assert.equal(manifest.web_accessible_resources, undefined, "nothing is exposed to pages");
});

test("manifest: content scripts run on the default sites only, with the shared logic first", () => {
  assert.equal(manifest.content_scripts.length, 1);
  const cs = manifest.content_scripts[0];
  assert.deepEqual(cs.js, ["lib/logic.js", "content.js"]);
  assert.equal(cs.run_at, "document_idle");
  assert.equal(cs.all_frames, false);
  const matched = cs.matches.map((m) => m).sort();
  assert.deepEqual(matched, [...manifest.host_permissions].sort());
  for (const f of cs.js) assert.ok(fs.existsSync(path.join(EXT, f)), f);
});

test("manifest: no CSP relaxation", () => {
  assert.equal(manifest.content_security_policy, undefined);
});

test("key: a public key only, and its ID is the one the host manifest pins", () => {
  const der = Buffer.from(manifest.key, "base64");
  assert.equal(der[0], 0x30, "DER SubjectPublicKeyInfo");
  assert.ok(der.includes(Buffer.from("2a864886f70d010101", "hex")), "rsaEncryption OID");
  assert.equal(der.length < 400, true, "2048-bit public key is 294 bytes");
  const id = extensionIdFromKey(manifest.key);
  assert.match(id, /^[a-p]{32}$/);
  const tmpl = JSON.parse(read(path.join(HOST, "com.nvvsr.mpv_handoff.json.in")).replace("@HOST_PATH@", "/x/nvvsr_host.py"));
  assert.deepEqual(tmpl.allowed_origins, ["chrome-extension://" + id + "/"]);
  assert.equal(tmpl.name, L.HOST_NAME);
  assert.equal(tmpl.type, "stdio");
  assert.equal(tmpl.path, "/x/nvvsr_host.py");
  assert.ok(read(path.join(HOST, "com.nvvsr.mpv_handoff.json.in")).includes('"@HOST_PATH@"'), "path placeholder");
});

test("no private key material anywhere in browser/", () => {
  const all = [];
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p);
      else all.push(p);
    }
  };
  walk(BROWSER);
  for (const f of all) {
    const text = fs.readFileSync(f, "latin1");
    assert.equal(/BEGIN (RSA |EC |ENCRYPTED )?PRIVATE KEY/.test(text), false, f);
  }
});

test("lint: every extension script parses, with no remote code and no dynamic evaluation", () => {
  const banned = [
    [/\beval\s*\(/, "eval("],
    [/new\s+Function\s*\(/, "new Function("],
    [/set(Timeout|Interval)\s*\(\s*["'`]/, "string timer"],
    [/importScripts\s*\(\s*["'](https?:|\/\/)/, "remote importScripts"],
    [/\bimport\s*\(/, "dynamic import("],
    [/<script(?![^>]*\bsrc=["'](?!https?:)[^"':]+["'])[^>]*>/i, "inline or remote <script>"],
    [/\son[a-z]+\s*=\s*["']/i, "inline event handler"],
    [/\.innerHTML\s*=/, "innerHTML"],
    [/document\.write\s*\(/, "document.write"],
    [/\bXMLHttpRequest\b|\bfetch\s*\(/, "network request"],
  ];
  // control characters (except tab, newline, carriage return), line/paragraph separators, bidi controls, BOM
  const badCodePoint = (cp) => (cp < 32 && cp !== 9 && cp !== 10 && cp !== 13) || cp === 127 ||
    (cp >= 8232 && cp <= 8239) || (cp >= 8294 && cp <= 8303) || cp === 65279;
  for (const f of jsFiles(EXT)) {
    const src = read(f);
    if (f.endsWith(".js")) new vm.Script(src, { filename: f }); // throws on a syntax error
    for (const [re, what] of banned) {
      assert.equal(re.test(src), false, path.relative(BROWSER, f) + ": " + what);
    }
    const odd = [...src].filter((ch) => badCodePoint(ch.codePointAt(0)));
    assert.equal(odd.length, 0, path.relative(BROWSER, f) + ": control or bidi character in source");
  }
});

test("lint: the host never uses a shell and never writes protocol-unsafe output", () => {
  const src = read(path.join(HOST, "nvvsr_host.py"));
  assert.equal(/shell\s*=\s*True/.test(src), false, "shell=True");
  assert.equal(/os\.system\s*\(|os\.popen\s*\(/.test(src), false, "os.system / os.popen");
  assert.equal(/\bprint\s*\(/.test(src), false, "print() would corrupt stdout");
  assert.equal(/\beval\s*\(|\bexec\s*\(/.test(src), false, "eval/exec");
  assert.equal(/write_frame\(stdout,/.test(src), true, "replies go through write_frame(stdout, ...)");
});

test("the extension and host name agree", () => {
  assert.equal(read(path.join(EXT, "background.js")).includes("L.HOST_NAME"), true);
  assert.equal(L.HOST_NAME, "com.nvvsr.mpv_handoff");
});
