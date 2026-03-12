#pragma once

// Single-page Vue 3 application served from flash.
// Embedded as a raw-string literal to avoid escaping issues.
// ~8 KB gzipped; served with Content-Encoding: none (plain text is fine for
// the typical short-range WiFi AP use-case).

static const char WEB_UI[] = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1.0"/>
<title>FR34 Cooler</title>
<style>
/* ── Reset ───────────────────────────────────── */
*,::before,::after{box-sizing:border-box}
body{margin:0}
/* ── Layout ──────────────────────────────────── */
.flex{display:flex}.flex-col{flex-direction:column}.flex-1{flex:1 1 0%}
.items-center{align-items:center}.justify-between{justify-content:space-between}
.grid{display:grid}
.grid-cols-2{grid-template-columns:repeat(2,minmax(0,1fr))}
.grid-cols-3{grid-template-columns:repeat(3,minmax(0,1fr))}
.gap-1{gap:.25rem}.gap-2{gap:.5rem}.gap-3{gap:.75rem}.gap-4{gap:1rem}
/* ── Sizing ───────────────────────────────────── */
.w-8{width:2rem}.h-8{height:2rem}.w-full{width:100%}.min-h-screen{min-height:100vh}
.max-w-2xl{max-width:42rem}
/* ── Spacing ──────────────────────────────────── */
.mx-auto{margin-left:auto;margin-right:auto}
.mb-3{margin-bottom:.75rem}.mb-6{margin-bottom:1.5rem}
.mt-1{margin-top:.25rem}.mt-2{margin-top:.5rem}
.p-4{padding:1rem}.p-5{padding:1.25rem}
.pb-4{padding-bottom:1rem}.py-1{padding-top:.25rem;padding-bottom:.25rem}
/* ── Typography ───────────────────────────────── */
.font-sans{font-family:ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,sans-serif}
.text-xs{font-size:.75rem;line-height:1rem}
.text-sm{font-size:.875rem;line-height:1.25rem}
.text-lg{font-size:1.125rem;line-height:1.75rem}
.text-xl{font-size:1.25rem;line-height:1.75rem}
.text-2xl{font-size:1.5rem;line-height:2rem}
.text-4xl{font-size:2.25rem;line-height:2.5rem}
.font-normal{font-weight:400}.font-semibold{font-weight:600}.font-bold{font-weight:700}
.tracking-tight{letter-spacing:-.025em}.tracking-widest{letter-spacing:.1em}
.uppercase{text-transform:uppercase}.text-center{text-align:center}
/* ── Colors ───────────────────────────────────── */
.text-slate-100{color:#f1f5f9}.text-slate-400{color:#94a3b8}
.text-slate-500{color:#64748b}.text-slate-600{color:#475569}
.text-emerald-400{color:#34d399}.text-sky-400,.text-brand-400{color:#38bdf8}
.text-violet-400{color:#a78bfa}.text-yellow-400{color:#facc15}
.text-orange-400{color:#fb923c}
.bg-slate-700{background-color:#334155}
/* ── Borders / Radius ─────────────────────────── */
.rounded-lg{border-radius:.5rem}.rounded-xl{border-radius:.75rem}
/* ── Interaction ──────────────────────────────── */
.transition-colors{transition-property:color,background-color,border-color;transition-duration:150ms}
.hover\:bg-slate-600:hover{background-color:#475569}
.active\:bg-slate-500:active{background-color:#64748b}
.accent-violet-500{accent-color:#8b5cf6}.accent-brand-400{accent-color:#38bdf8}
/* ── SVG helpers ──────────────────────────────── */
.fill-none{fill:none}.stroke-current{stroke:currentColor}
</style>
<style>
  body { background:#0f172a; }
  .card { background:#1e293b; border:1px solid #334155; }
  .value-big { font-variant-numeric:tabular-nums; }
  input[type=range]::-webkit-slider-thumb { accent-color:#38bdf8; }
  .status-dot { width:10px; height:10px; border-radius:50%; display:inline-block; margin-right:6px; }
  .dot-ok   { background:#22c55e; box-shadow:0 0 6px #22c55e; }
  .dot-warn { background:#f59e0b; box-shadow:0 0 6px #f59e0b; }
  .dot-err  { background:#ef4444; box-shadow:0 0 6px #ef4444; }
  .spin { animation: spin 1s linear infinite; }
  @keyframes spin { from{transform:rotate(0deg)} to{transform:rotate(360deg)} }
</style>
</head>
<body class="dark text-slate-100 min-h-screen p-4 font-sans">
<div id="app">
  <!-- Header -->
  <header class="flex items-center justify-between mb-6 max-w-2xl mx-auto">
    <div class="flex items-center gap-3">
      <svg class="w-8 h-8 text-brand-400" fill="none" stroke="currentColor" stroke-width="1.8"
           viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round"
           d="M12 3v18M3 12h18M5.6 5.6l12.8 12.8M18.4 5.6L5.6 18.4"/></svg>
      <h1 class="text-xl font-bold tracking-tight">FR34 Cooler</h1>
    </div>
    <div class="flex items-center text-sm text-slate-400">
      <span :class="statusDotClass" class="status-dot"></span>
      <span>{{ statusLabel }}</span>
    </div>
  </header>

  <main class="max-w-2xl mx-auto grid gap-4">

    <!-- Temperature row -->
    <div class="grid grid-cols-2 gap-4">
      <!-- Current temp -->
      <div class="card rounded-xl p-5 flex flex-col gap-1">
        <span class="text-xs uppercase tracking-widest text-slate-400">Cabinet Temp</span>
        <span class="value-big text-4xl font-bold" :class="tempColor">
          {{ fmt1(state.temp) }}<span class="text-xl font-normal text-slate-400"> °C</span>
        </span>
        <span class="text-xs text-slate-500 mt-1">current reading</span>
      </div>

      <!-- Setpoint -->
      <div class="card rounded-xl p-5 flex flex-col gap-1">
        <span class="text-xs uppercase tracking-widest text-slate-400">Setpoint</span>
        <span class="value-big text-4xl font-bold text-brand-400">
          {{ fmt1(state.setpoint) }}<span class="text-xl font-normal text-slate-400"> °C</span>
        </span>
        <div class="flex gap-2 mt-2">
          <button @click="adjustSetpoint(-5)"
            class="flex-1 rounded-lg bg-slate-700 hover:bg-slate-600 active:bg-slate-500
                   text-lg font-bold py-1 transition-colors">−0.5</button>
          <button @click="adjustSetpoint(5)"
            class="flex-1 rounded-lg bg-slate-700 hover:bg-slate-600 active:bg-slate-500
                   text-lg font-bold py-1 transition-colors">+0.5</button>
        </div>
      </div>
    </div>

    <!-- Power metrics row -->
    <div class="grid grid-cols-3 gap-4">
      <div class="card rounded-xl p-4 flex flex-col gap-1">
        <span class="text-xs uppercase tracking-widest text-slate-400">Voltage</span>
        <span class="value-big text-2xl font-semibold text-emerald-400">
          {{ fmt2(state.voltage) }}<span class="text-sm font-normal text-slate-400"> V</span>
        </span>
      </div>
      <div class="card rounded-xl p-4 flex flex-col gap-1">
        <span class="text-xs uppercase tracking-widest text-slate-400">Fan</span>
        <span class="value-big text-2xl font-semibold text-sky-400">
          {{ fmt2(state.fanCurrent) }}<span class="text-sm font-normal text-slate-400"> A</span>
        </span>
      </div>
      <div class="card rounded-xl p-4 flex flex-col gap-1">
        <span class="text-xs uppercase tracking-widest text-slate-400">Compressor</span>
        <span class="value-big text-2xl font-semibold text-violet-400">
          {{ state.compPower === 0 ? 'AUTO' : state.compPower + '%' }}
        </span>
      </div>
    </div>

    <!-- Compressor override -->
    <div class="card rounded-xl p-5">
      <div class="flex items-center justify-between mb-3">
        <span class="font-semibold">Compressor Override</span>
        <span class="text-sm text-slate-400">
          {{ powerOverrideLabel }}
        </span>
      </div>
      <input type="range" min="0" max="100" step="5"
             v-model.number="pendingPower"
             @change="setPower"
             class="w-full accent-violet-500"/>
      <div class="flex justify-between text-xs text-slate-500 mt-1">
        <span>AUTO (0)</span>
        <span>Full (100%)</span>
      </div>
      <p class="text-xs text-slate-500 mt-2">
        Set to 0 for automatic control. Any other value forces the
        compressor to that duty cycle regardless of temperature.
      </p>
    </div>

    <!-- Power cap -->
    <div class="card rounded-xl p-5">
      <div class="flex items-center justify-between mb-3">
        <span class="font-semibold">Power Limit (hard cap)</span>
        <span class="text-sm text-slate-400">{{ pendingPowerMax }}%</span>
      </div>
      <input type="range" min="0" max="100" step="5"
             v-model.number="pendingPowerMax"
             @change="setPowerMax"
             class="w-full accent-brand-400"/>
      <div class="flex justify-between text-xs text-slate-500 mt-1">
        <span>0%</span>
        <span>100%</span>
      </div>
      <p class="text-xs text-slate-500 mt-2">
        Maximum allowed compressor duty cycle. Useful for battery management.
      </p>
    </div>

    <!-- Last update -->
    <div class="text-center text-xs text-slate-600 pb-4">
      Last update: {{ lastUpdate || '—' }} &nbsp;·&nbsp;
      Updates every second via WebSocket
    </div>
  </main>
</div>

<script src="/vue.js"></script>
<script>
const { createApp, ref, computed } = Vue;

createApp({
  setup() {
    const state = ref({
      temp: null, setpoint: null,
      voltage: null, fanCurrent: null,
      compPower: null, compPowerMax: null
    });

    const connected   = ref(false);
    const lastUpdate  = ref('');

    // Pending slider values — synced from server, then user drags
    const pendingPower    = ref(0);
    const pendingPowerMax = ref(100);

    // ── WebSocket ─────────────────────────────────────────────────────────
    let ws = null;
    let reconnectTimer = null;

    function connect() {
      const host = location.hostname || '192.168.4.1';
      ws = new WebSocket('ws://' + host + '/ws');

      ws.onopen  = () => { connected.value = true; };
      ws.onclose = () => {
        connected.value = false;
        ws = null;
        reconnectTimer = setTimeout(connect, 3000);
      };
      ws.onerror = () => ws.close();

      ws.onmessage = (ev) => {
        try {
          const d = JSON.parse(ev.data);
          // temperatures come as tenths of °C from ESP32
          state.value.temp       = d.temp      ?? null;
          state.value.setpoint   = d.setpoint  ?? null;
          state.value.voltage    = d.voltage   ?? null;
          state.value.fanCurrent = d.fanCurrent?? null;
          state.value.compPower  = d.compPower ?? null;
          state.value.compPowerMax = d.compPowerMax ?? null;

          // Sync sliders only when the server sends fresh values
          // (avoid overwriting while the user is dragging)
          if (document.querySelector('input[type=range]:active') === null) {
            pendingPower.value    = d.compPower    ?? 0;
            pendingPowerMax.value = d.compPowerMax ?? 100;
          }

          const now = new Date();
          lastUpdate.value = now.toLocaleTimeString();
        } catch(e) {}
      };
    }

    connect();

    // ── Helpers ───────────────────────────────────────────────────────────
    function send(obj) {
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(obj));
      }
    }

    function fmt1(v) { return v !== null ? v.toFixed(1) : '—'; }
    function fmt2(v) { return v !== null ? v.toFixed(2) : '—'; }

    // ── Controls ──────────────────────────────────────────────────────────
    function adjustSetpoint(delta10) {
      // delta10 is in tenths of °C
      if (state.value.setpoint === null) return;
      const newVal = Math.round(state.value.setpoint * 10 + delta10);
      // Clamp to cooler operating range: -18°C to +10°C
      const clamped = Math.max(-180, Math.min(100, newVal));
      send({ cmd: 'setTemp', value: clamped });
    }

    function setPower() {
      send({ cmd: 'setPower', value: pendingPower.value });
    }

    function setPowerMax() {
      send({ cmd: 'setPowerMax', value: pendingPowerMax.value });
    }

    // ── Computed visuals ──────────────────────────────────────────────────
    const tempColor = computed(() => {
      const t = state.value.temp;
      if (t === null) return 'text-slate-500';
      if (t < -5)  return 'text-sky-400';
      if (t < 5)   return 'text-emerald-400';
      if (t < 15)  return 'text-yellow-400';
      return 'text-orange-400';
    });

    const statusDotClass = computed(() => {
      if (!connected.value) return 'dot-err';
      if (state.value.temp === null) return 'dot-warn';
      return 'dot-ok';
    });

    const statusLabel = computed(() => {
      if (!connected.value) return 'Disconnected';
      if (state.value.temp === null) return 'Waiting…';
      return 'Live';
    });

    const powerOverrideLabel = computed(() => {
      return pendingPower.value === 0 ? 'AUTO' : pendingPower.value + '%';
    });

    return {
      state, connected, lastUpdate,
      pendingPower, pendingPowerMax,
      tempColor, statusDotClass, statusLabel, powerOverrideLabel,
      fmt1, fmt2,
      adjustSetpoint, setPower, setPowerMax
    };
  }
}).mount('#app');
</script>
</body>
</html>
)rawhtml";
