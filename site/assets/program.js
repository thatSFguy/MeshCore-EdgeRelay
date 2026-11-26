import { hasSerial } from "./serial.js";

const KIND_TEXT = {
  relay: "For a relay you leave in your car. Configure it afterwards on the Configure relay page.",
  kiss: "For the LilyGo T-LoRa V2.1-1.6 with a CardKB keyboard, buzzer (GPIO 15) and vibration motor (GPIO 13).",
};

const PLATFORM_LABEL = { esp32: "ESP32", nrf52: "nRF52", rp2040: "RP2040", stm32: "STM32" };

let firmwares = [];
let kind = "relay";
let selected = null;

const $ = (s) => document.querySelector(s);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

async function load() {
  try {
    const r = await fetch("firmware/index.json", { cache: "no-cache" });
    if (!r.ok) throw new Error(r.status);
    const data = await r.json();
    firmwares = data.firmwares || [];
  } catch {
    $("#boards").innerHTML = `<p class="muted" style="padding:10px 12px;margin:0">Firmware list unavailable. Try again later.</p>`;
    return;
  }
  render();
}

function render() {
  $("#kind-desc").textContent = KIND_TEXT[kind];
  document.querySelectorAll(".seg button").forEach((b) => b.setAttribute("aria-pressed", String(b.dataset.kind === kind)));
  const q = $("#search").value.trim().toLowerCase();
  const list = firmwares
    .filter((f) => f.kind === kind)
    .filter((f) => !q || f.board.toLowerCase().includes(q) || f.id.toLowerCase().includes(q))
    .sort((a, b) => a.board.localeCompare(b.board));
  $("#boards").innerHTML = list.length
    ? list.map((f) => `<button type="button" role="option" data-id="${esc(f.id)}" aria-pressed="${selected?.id === f.id}">
        <span>${esc(f.board)}</span><span class="plat">${esc(f.chip || PLATFORM_LABEL[f.platform] || f.platform)}</span></button>`).join("")
    : `<p class="muted" style="padding:10px 12px;margin:0">No matching boards.</p>`;
  $("#board-count").textContent = `${list.length} board${list.length === 1 ? "" : "s"}`;
  $("#boards").querySelectorAll("button").forEach((b) => b.addEventListener("click", () => {
    selected = firmwares.find((f) => f.id === b.dataset.id);
    render();
    renderInstall();
  }));
}

function download(f, key, label) {
  const path = f.files[key];
  return path ? `<a class="button" href="firmware/${esc(path)}" download>${label}</a>` : "";
}

function renderInstall() {
  const el = $("#install");
  const f = selected;
  if (!f) { el.innerHTML = `<p class="muted">Choose a board above.</p>`; return; }
  const head = `<p style="margin-top:0"><strong>${esc(f.board)}</strong> &middot; <span class="mono">${esc(f.version)}</span></p>`;

  if (f.platform === "esp32") {
    el.innerHTML = head + (hasSerial
      ? `<esp-web-install-button manifest="firmware/${esc(f.manifest)}">
           <button slot="activate" class="primary">Connect and install</button>
           <span slot="unsupported" class="note">Use desktop Chrome or Edge to install from the browser.</span>
           <span slot="not-allowed" class="note">The page must be served over HTTPS.</span>
         </esp-web-install-button>`
      : `<div class="note">Installing from the browser needs desktop Chrome or Edge. You can still download the file below.</div>`)
      + `<ol class="howto">
          <li>Plug the board in with a USB data cable.</li>
          <li>Click <strong>Connect and install</strong> and pick its port.</li>
          <li><strong>Updating?</strong> Leave <em>Erase device</em> unticked to keep your settings, contacts and keys.
            Tick it for a clean first install.</li>
        </ol>
        <div class="actions">${download(f, "merged", "Download .bin (full image)")}</div>`;
  } else if (f.platform === "nrf52") {
    el.innerHTML = head + `<ol class="howto">
        <li>Download the <code>.uf2</code> file.</li>
        <li>Plug the board in, then <strong>double-press its reset button</strong> quickly. A USB drive appears
          (for example <code>RAK4631</code> or <code>T1000-E</code>).</li>
        <li>Drag the <code>.uf2</code> file onto that drive. The board installs it and restarts. Your settings are kept.</li>
      </ol>
      <div class="actions">${download(f, "uf2", "Download .uf2")}</div>`;
  } else if (f.platform === "rp2040") {
    el.innerHTML = head + `<ol class="howto">
        <li>Download the <code>.uf2</code> file.</li>
        <li>Hold the <strong>BOOTSEL</strong> button while plugging the board in. A drive called <code>RPI-RP2</code> appears.</li>
        <li>Drag the <code>.uf2</code> file onto it. The board installs it and restarts.</li>
      </ol>
      <div class="actions">${download(f, "uf2", "Download .uf2")}</div>`;
  } else {
    el.innerHTML = head + `<p>STM32 boards are flashed with
        <a href="https://www.st.com/en/development-tools/stm32cubeprog.html">STM32CubeProgrammer</a>
        (ST-Link or the board's serial bootloader). Download the file and open it there.</p>
      <div class="actions">${download(f, "hex", "Download .hex")}${download(f, "bin", "Download .bin")}</div>`;
  }
}

document.querySelectorAll(".seg button").forEach((b) => b.addEventListener("click", () => {
  kind = b.dataset.kind;
  selected = null;
  render();
  renderInstall();
}));
$("#search").addEventListener("input", render);
load();
