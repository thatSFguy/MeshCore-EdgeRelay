import { LineConsole, hasSerial, toast, serialUnsupportedNote } from "./serial.js";

const $ = (s) => document.querySelector(s);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const dev = new LineConsole();

// "OK - a:1 b:two" -> { a: "1", b: "two" }
function kv(reply) {
  const out = {};
  for (const m of reply.matchAll(/([a-z_]+(?:\([^)]*\))?):(\S+)/gi)) out[m[1]] = m[2];
  return out;
}
const ok = (r) => /^OK\b/.test(r);

function log(text, cls = "") {
  const el = $("#log");
  const line = document.createElement("div");
  if (cls) line.className = cls;
  line.textContent = text;
  el.appendChild(line);
  while (el.childNodes.length > 400) el.firstChild.remove();
  el.scrollTop = el.scrollHeight;
}

async function cmd(c, { quiet = false } = {}) {
  log("> " + c, "tx");
  try {
    const r = await dev.command(c);
    if (!quiet && /^(Err|ERROR|Error)/.test(r)) toast(r);
    return r;
  } catch (e) {
    log(e.message, "err");
    toast(e.message);
    throw e;
  }
}

dev.onLine = (line) => log(line);
dev.onDisconnect = () => setConnected(false);

function setConnected(on) {
  $("#dot").classList.toggle("on", on);
  $("#status").textContent = on ? "Connected" : "Not connected";
  $("#connect").textContent = on ? "Disconnect" : "Connect";
  $("#panels").hidden = !on;
}

// ---------------------------------------------------------------- loaders

async function loadStatus() {
  const [ver, name, radio, tx, st] = [
    await cmd("ver", { quiet: true }),
    await cmd("get name", { quiet: true }),
    await cmd("get radio", { quiet: true }),
    await cmd("get tx", { quiet: true }),
    await cmd("edge status", { quiet: true }),
  ];
  const nm = name.replace(/^>\s*/, "");
  $("#name").value = nm;
  const rp = radio.replace(/^>\s*/, "").split(",");
  if (rp.length === 4) [$("#freq").value, $("#bw").value, $("#sf").value, $("#cr").value] = rp;
  $("#tx").value = tx.replace(/^>\s*/, "");

  const s = kv(st);
  $("#device-kv").innerHTML = `
    <dt>Name</dt><dd>${esc(nm)}</dd>
    <dt>Firmware</dt><dd class="mono">${esc(ver)}</dd>
    <dt>Policy</dt><dd>${s.valid === "1" ? "Active" : "<strong>Receive-only</strong> (add an owner)"}</dd>`;
  const stats = [
    ["up", "Uplinks sent"], ["copy", "Copies to owners"], ["odir", "Owner DMs carried"], ["dfwd", "Direct forwards"],
    ["echo", "Echo-cancelled"], ["held", "Held at home"], ["drop", "Dropped"], ["rblk", "Region-blocked"], ["fblk", "Forward-blocked"],
  ];
  $("#stats").innerHTML = stats.map(([k, label]) =>
    `<div class="stat"><div class="v">${esc(s[k] ?? "–")}</div><div class="k">${label}</div></div>`).join("");
}

async function loadOwners() {
  const r = await cmd("edge owner list", { quiet: true });
  const n = parseInt((r.match(/(\d+) owner/) || [])[1] || "0", 10);
  const keys = [];
  for (let i = 0; i < n; i++) {
    const k = await cmd(`edge owner show ${i}`, { quiet: true });
    const m = k.match(/[0-9a-f]{64}/i);
    if (m) keys.push(m[0].toLowerCase());
  }
  $("#owners").innerHTML = keys.length
    ? keys.map((k) => `<li><span class="grow mono small">${k}</span><button class="danger" data-del-owner="${k}">Remove</button></li>`).join("")
    : `<li class="empty">No owners yet.</li>`;
  $("#owners").querySelectorAll("[data-del-owner]").forEach((b) => b.addEventListener("click", async () => {
    if (!confirm("Remove this owner? The relay stops forwarding for that companion.")) return;
    toast(await cmd(`edge owner del ${b.dataset.delOwner}`));
    loadOwners();
  }));
}

async function loadChannels() {
  const r = await cmd("edge chan list", { quiet: true });
  const hashes = (r.split(":").slice(1).join(":").match(/\b[0-9a-f]{2}\b/gi) || []);
  $("#chans").innerHTML = hashes.length
    ? hashes.map((h) => `<li><span class="grow">Channel hash <span class="mono">${h}</span></span>
        <button class="danger" data-del-chan="${h}">Remove</button></li>`).join("")
    : `<li class="empty">No channels.</li>`;
  $("#chans").querySelectorAll("[data-del-chan]").forEach((b) => b.addEventListener("click", async () => {
    toast(await cmd(`edge chan del ${b.dataset.delChan}`));
    loadChannels();
  }));
}

async function loadEcho() {
  const e = kv(await cmd("edge echo", { quiet: true }));
  $("#echo-on").checked = e.echo === "on";
  $("#echo-wait").value = e.wait ?? "";
}

async function loadHome() {
  const r = await cmd("edge home", { quiet: true });
  if (/home:off/.test(r)) {
    $("#home-state").textContent = "Off";
    return;
  }
  const h = kv(r);
  const state = (r.match(/^OK - (\S+)/) || [])[1] || "";
  $("#home-state").textContent = `${state} · node ${h.node} · avg ${h.avg} dBm · last ${h.last} dBm · ${h.n} samples`;
  if (h.enter) $("#home-enter").value = h.enter;
  if (h.exit) $("#home-exit").value = h.exit;
  if (h.timeout) $("#home-timeout").value = parseInt(h.timeout, 10);
}

async function loadOptions() {
  const o = kv(await cmd("edge opt", { quiet: true }));
  for (const k of ["owner_direct", "mirror_adverts", "fwd_acks"]) $(`#opt-${k}`).checked = o[k] === "1";
}

async function loadAll() {
  try {
    await loadStatus();
    await loadOwners();
    await loadChannels();
    await loadEcho();
    await loadHome();
    await loadOptions();
  } catch {
    // errors are already logged and toasted
  }
}

// ---------------------------------------------------------------- actions

$("#connect").addEventListener("click", async () => {
  if (dev.connected) { await dev.disconnect(); return; }
  try {
    await dev.connect(115200);
    setConnected(true);
    await loadAll();
  } catch (e) {
    if (e.name !== "NotFoundError") toast(e.message);   // NotFoundError: user closed the picker
  }
});

$("#refresh").addEventListener("click", loadAll);
$("#advert").addEventListener("click", async () => toast(await cmd("edge advert")));

$("#owner-add").addEventListener("click", async () => {
  const k = $("#owner-key").value.trim().toLowerCase();
  if (!/^[0-9a-f]{64}$/.test(k)) { toast("A public key is 64 hex characters"); return; }
  const r = await cmd(`edge owner add ${k}`);
  toast(r);
  if (ok(r)) { $("#owner-key").value = ""; loadOwners(); loadStatus(); }
});

$("#chan-add").addEventListener("click", async () => {
  const k = $("#chan-key").value.trim() || "public";
  const r = await cmd(`edge chan add ${k}`);
  toast(r);
  if (ok(r)) { $("#chan-key").value = ""; loadChannels(); }
});

$("#echo-save").addEventListener("click", async () => {
  await cmd(`edge echo ${$("#echo-on").checked ? "on" : "off"}`);
  const w = parseInt($("#echo-wait").value, 10);
  if (!Number.isNaN(w)) await cmd(`edge echo wait ${w}`);
  toast("Echo suppression saved");
  loadEcho();
});

$("#home-set").addEventListener("click", async () => {
  const k = $("#home-key").value.trim().toLowerCase();
  if (!/^([0-9a-f]{2}){1,32}$/.test(k)) { toast("Enter the home node's key in hex"); return; }
  const r = await cmd(`edge home set ${k} ${$("#home-enter").value} ${$("#home-exit").value} ${$("#home-timeout").value}`);
  toast(r);
  loadHome();
});
$("#home-off").addEventListener("click", async () => { toast(await cmd("edge home off")); loadHome(); });
$("#home-override").addEventListener("click", async () => { toast(await cmd("edge home override 30")); loadHome(); });

for (const k of ["owner_direct", "mirror_adverts", "fwd_acks"]) {
  $(`#opt-${k}`).addEventListener("change", async (e) => {
    toast(await cmd(`edge opt ${k} ${e.target.checked ? 1 : 0}`));
    loadOptions();
  });
}

$("#name-save").addEventListener("click", async () => {
  const n = $("#name").value.trim();
  if (!n) return;
  toast(await cmd(`set name ${n}`));
  loadStatus();
});
$("#password-save").addEventListener("click", async () => {
  const p = $("#password").value;
  if (!p) return;
  const r = await cmd(`password ${p}`);
  toast(/password now/.test(r) ? "Password set" : r);
  $("#password").value = "";
});
$("#radio-save").addEventListener("click", async () => {
  const r = await cmd(`set radio ${$("#freq").value},${$("#bw").value},${$("#sf").value},${$("#cr").value}`);
  if ($("#tx").value !== "") await cmd(`set tx ${$("#tx").value}`);
  toast(r);
});
$("#reboot").addEventListener("click", async () => {
  if (!confirm("Reboot the relay?")) return;
  log("> reboot", "tx");
  dev.command("reboot", 1500).catch(() => {});   // no reply: it restarts
  toast("Rebooting. Reconnect in a few seconds.");
});

async function sendConsole() {
  const c = $("#cmd").value.trim();
  if (!c) return;
  $("#cmd").value = "";
  try { await cmd(c, { quiet: true }); } catch {}
}
$("#cmd-send").addEventListener("click", sendConsole);
$("#cmd").addEventListener("keydown", (e) => { if (e.key === "Enter") sendConsole(); });

if (!hasSerial) {
  $("#unsupported").innerHTML = serialUnsupportedNote();
  $("#connect").disabled = true;
}
