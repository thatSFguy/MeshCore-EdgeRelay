import { FrameLink, hasSerial, toast, toHex, fromHex, fromBase64, toBase64, serialUnsupportedNote } from "./serial.js";

// MeshCore companion protocol (examples/companion_radio/MyMesh.cpp)
const CMD_APP_START = 1, CMD_GET_DEVICE_TIME = 5, CMD_SET_DEVICE_TIME = 6, CMD_SET_ADVERT_NAME = 8, CMD_SET_RADIO_PARAMS = 11, CMD_SET_RADIO_TX_POWER = 12,
  CMD_REBOOT = 19, CMD_DEVICE_QUERY = 22, CMD_GET_CHANNEL = 31, CMD_SET_CHANNEL = 32;
const RESP_OK = 0, RESP_ERR = 1, RESP_SELF_INFO = 5, RESP_CURR_TIME = 9, RESP_DEVICE_INFO = 13, RESP_CHANNEL_INFO = 18;
const PUBLIC_PSK = "izOH6cXN6mrJ5e26oRXNcg==";

const $ = (s) => document.querySelector(s);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const enc = new TextEncoder();
const dec = new TextDecoder();
const link = new FrameLink();

let maxChannels = 8;
let channels = [];   // { idx, name, secret: Uint8Array }

link.onDisconnect = () => setConnected(false);

function setConnected(on) {
  $("#dot").classList.toggle("on", on);
  $("#status").textContent = on ? "Connected" : "Not connected";
  $("#connect").textContent = on ? "Disconnect" : "Connect";
  $("#panels").hidden = !on;
}

const cstr = (bytes) => { const z = bytes.indexOf(0); return dec.decode(z >= 0 ? bytes.subarray(0, z) : bytes); };
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
function putU32(arr, o, v) { arr[o] = v & 255; arr[o + 1] = (v >>> 8) & 255; arr[o + 2] = (v >>> 16) & 255; arr[o + 3] = (v >>> 24) & 255; }

async function expectOk(frame, what) {
  const r = await link.request(frame);
  if (r[0] !== RESP_OK) throw new Error(`${what} failed`);
}

// ---------------------------------------------------------------- loaders

async function loadDevice() {
  const di = await link.request(Uint8Array.of(CMD_DEVICE_QUERY, 3));
  let model = "", version = "";
  if (di[0] === RESP_DEVICE_INFO) {
    maxChannels = di[3] || 8;
    model = cstr(di.subarray(20, 60));
    version = cstr(di.subarray(60, 80));
  }
  const start = new Uint8Array(8 + 8);
  start[0] = CMD_APP_START;
  start[1] = 3;
  start.set(enc.encode("kiss-web"), 8);
  const si = await link.request(start);
  if (si[0] !== RESP_SELF_INFO) throw new Error("Not a MeshCore companion (is this a K.I.S.S. Messenger?)");
  const tx = (si[2] << 24) >> 24;
  const pub = si.subarray(4, 36);
  const freq = u32(si, 48) / 1000, bw = u32(si, 52) / 1000, sf = si[56], cr = si[57];
  const name = dec.decode(si.subarray(58));

  $("#name").value = name;
  $("#freq").value = freq; $("#bw").value = bw; $("#sf").value = sf; $("#cr").value = cr; $("#tx").value = tx;
  const preset = `${freq},${bw},${sf},${cr}`;
  $("#preset").value = [...$("#preset").options].some((o) => o.value === preset) ? preset : "";
  $("#device-kv").innerHTML = `
    <dt>Name</dt><dd>${esc(name)}</dd>
    <dt>Board</dt><dd>${esc(model)}</dd>
    <dt>Firmware</dt><dd class="mono">${esc(version)}</dd>
    <dt>Public key</dt><dd class="mono small">${toHex(pub)}</dd>`;
}

// Sets the device clock from this computer if it is behind. The firmware
// refuses to move a clock backwards (replay protection), so a clock that is
// ahead is only reported: a reboot resets it.
async function syncClock() {
  const r = await link.request(Uint8Array.of(CMD_GET_DEVICE_TIME));
  if (r[0] !== RESP_CURR_TIME) return "unknown";
  const dev = u32(r, 1);
  const now = Math.floor(Date.now() / 1000);
  const diff = dev - now;
  if (Math.abs(diff) <= 60) return "correct";
  if (diff < 0) {
    const f = new Uint8Array(5);
    f[0] = CMD_SET_DEVICE_TIME;
    putU32(f, 1, now);
    const ok = (await link.request(f))[0] === RESP_OK;
    return ok ? "set from this computer" : "couldn't set";
  }
  const h = diff / 3600;
  return `${h >= 1 ? h.toFixed(1) + " hours" : Math.round(diff / 60) + " minutes"} ahead. Reboot the device to reset it`;
}

async function loadChannels() {
  channels = [];
  for (let i = 0; i < maxChannels; i++) {
    const r = await link.request(Uint8Array.of(CMD_GET_CHANNEL, i));
    if (r[0] !== RESP_CHANNEL_INFO) continue;
    const name = cstr(r.subarray(2, 34));
    if (!name) continue;
    channels.push({ idx: r[1], name, secret: r.slice(34, 50) });
  }
  $("#chans").innerHTML = channels.length
    ? channels.map((c) => `<li>
        <span class="grow"><strong>${esc(c.name)}</strong><br><span class="mono small muted">${toHex(c.secret)}</span></span>
        <button data-copy="${c.idx}">Copy secret</button>
        <button class="danger" data-del="${c.idx}">Remove</button></li>`).join("")
    : `<li class="empty">No channels.</li>`;
  $("#chans").querySelectorAll("[data-copy]").forEach((b) => b.addEventListener("click", async () => {
    const c = channels.find((x) => x.idx === +b.dataset.copy);
    try { await navigator.clipboard.writeText(toHex(c.secret)); toast("Secret copied"); }
    catch { toast("Couldn't copy; select the secret instead"); }
  }));
  $("#chans").querySelectorAll("[data-del]").forEach((b) => b.addEventListener("click", async () => {
    const c = channels.find((x) => x.idx === +b.dataset.del);
    if (!confirm(`Remove "${c.name}"? You can add it back with its secret.`)) return;
    await setChannel(c.idx, "", new Uint8Array(16));
    toast("Channel removed");
    loadChannels();
  }));
}

async function setChannel(idx, name, secret) {
  const f = new Uint8Array(2 + 32 + 16);
  f[0] = CMD_SET_CHANNEL;
  f[1] = idx;
  f.set(enc.encode(name).subarray(0, 31), 2);
  f.set(secret.subarray(0, 16), 34);
  await expectOk(f, "Saving the channel");
}

async function addChannel(name, secret) {
  if (!name) { toast("Give the channel a name"); return; }
  if (!secret || secret.length !== 16) { toast("The secret must be 16 bytes: 32 hex characters or base64"); return; }
  if (channels.some((c) => toHex(c.secret) === toHex(secret))) { toast("That channel is already on the device"); return; }
  const used = new Set(channels.map((c) => c.idx));
  let idx = -1;
  for (let i = 0; i < maxChannels; i++) if (!used.has(i)) { idx = i; break; }
  if (idx < 0) { toast("All channel slots are in use"); return; }
  await setChannel(idx, name, secret);
  toast(`Added "${name}"`);
  $("#ch-name").value = "";
  $("#ch-secret").value = "";
  loadChannels();
}

function parseSecret(s) {
  const t = s.trim();
  if (/^[0-9a-fA-F]{32}$/.test(t)) return fromHex(t);
  const b = fromBase64(t);
  return b && b.length === 16 ? b : null;
}

// ---------------------------------------------------------------- actions

$("#connect").addEventListener("click", async () => {
  if (link.connected) { await link.disconnect(); return; }
  try {
    await link.connect(115200);
    setConnected(true);
    await loadDevice();
    const clock = await syncClock();
    $("#device-kv").insertAdjacentHTML("beforeend", `<dt>Clock</dt><dd>${esc(clock)}</dd>`);
    await loadChannels();
  } catch (e) {
    if (e.name !== "NotFoundError") toast(e.message);
    if (link.connected) await link.disconnect();
  }
});

$("#name-save").addEventListener("click", async () => {
  const n = $("#name").value.trim();
  if (!n) return;
  if (/[:@\[\]]/.test(n)) { toast("Names can't contain : @ [ ]"); return; }
  const f = new Uint8Array(1 + enc.encode(n).length);
  f[0] = CMD_SET_ADVERT_NAME;
  f.set(enc.encode(n), 1);
  try { await expectOk(f, "Saving the name"); toast("Name saved"); loadDevice(); } catch (e) { toast(e.message); }
});

$("#ch-generate").addEventListener("click", () => {
  const s = crypto.getRandomValues(new Uint8Array(16));
  $("#ch-secret").value = toHex(s);
});
$("#ch-add").addEventListener("click", async () => {
  try { await addChannel($("#ch-name").value.trim(), parseSecret($("#ch-secret").value)); } catch (e) { toast(e.message); }
});
$("#ch-public").addEventListener("click", async () => {
  try { await addChannel("Public", fromBase64(PUBLIC_PSK)); } catch (e) { toast(e.message); }
});

$("#preset").addEventListener("change", () => {
  const v = $("#preset").value;
  if (!v) return;
  [$("#freq").value, $("#bw").value, $("#sf").value, $("#cr").value] = v.split(",");
});

$("#radio-save").addEventListener("click", async () => {
  const freq = Math.round(parseFloat($("#freq").value) * 1000);
  const bw = Math.round(parseFloat($("#bw").value) * 1000);
  const sf = parseInt($("#sf").value, 10), cr = parseInt($("#cr").value, 10);
  const f = new Uint8Array(1 + 4 + 4 + 2);
  f[0] = CMD_SET_RADIO_PARAMS;
  putU32(f, 1, freq);
  putU32(f, 5, bw);
  f[9] = sf;
  f[10] = cr;
  try {
    await expectOk(f, "Saving radio settings (check the values)");
    const tx = parseInt($("#tx").value, 10);
    if (!Number.isNaN(tx)) await expectOk(Uint8Array.of(CMD_SET_RADIO_TX_POWER, tx & 255), "Saving TX power");
    toast("Radio saved");
  } catch (e) { toast(e.message); }
});

$("#reboot").addEventListener("click", async () => {
  if (!confirm("Reboot the K.I.S.S. Messenger?")) return;
  link.request(Uint8Array.of(CMD_REBOOT, ...enc.encode("reboot")), 1000).catch(() => {});
  toast("Rebooting. Reconnect in a few seconds.");
});

if (!hasSerial) {
  $("#unsupported").innerHTML = serialUnsupportedNote();
  $("#connect").disabled = true;
}
