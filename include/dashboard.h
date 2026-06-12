#pragma once

#include <Arduino.h>

const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Plant watering</title>
  <style>
    :root { color-scheme: dark; font-family: system-ui, sans-serif; }
    body { max-width: 700px; margin: 0 auto; padding: 24px; background: #101712; color: #e8f3ea; }
    h1 { font-size: 1.6rem; margin-bottom: 8px; }
    .sub { color: #9db5a2; margin-bottom: 24px; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit,minmax(190px,1fr)); gap: 12px; }
    .card { background: #1a251d; border: 1px solid #304335; border-radius: 12px; padding: 16px; }
    .label { color: #9db5a2; font-size: .85rem; }
    .value { font-size: 1.7rem; font-weight: 650; margin-top: 4px; }
    .mode { color: #5ed47b; }
    .wide { margin-top: 12px; }
    button { width: 100%; border: 0; border-radius: 10px; padding: 14px; margin-top: 14px;
             background: #5ed47b; color: #08150b; font-size: 1rem; font-weight: 700; cursor: pointer; }
    button:disabled { opacity: .45; cursor: wait; }
    #message { min-height: 1.4em; color: #badcc2; }
  </style>
</head>
<body>
  <h1>Plant watering controller</h1>
  <div class="sub"><span id="clock">Waiting for time...</span> | <span id="network">Connecting...</span></div>
  <div class="grid">
    <div class="card"><div class="label">Active mode</div><div class="value mode" id="mode">--</div></div>
    <div class="card"><div class="label">Soil moisture</div><div class="value" id="soil">--</div></div>
    <div class="card"><div class="label">Battery</div><div class="value" id="battery">--</div></div>
    <div class="card"><div class="label">Pump</div><div class="value" id="pump">--</div></div>
  </div>
  <div class="card wide">
    <div class="label">Controller status</div>
    <p id="message">Loading...</p>
    <div class="label" id="details"></div>
    <button id="water" onclick="waterNow()">Water now</button>
  </div>
  <script>
    const e = id => document.getElementById(id);
    const duration = ms => {
      const total = Math.max(0, Math.ceil(ms / 1000));
      const m = Math.floor(total / 60);
      const s = total % 60;
      return `${m}:${String(s).padStart(2, '0')}`;
    };

    async function refresh() {
      try {
        const r = await fetch('/api/status', {cache: 'no-store'});
        const s = await r.json();
        e('network').textContent = `${s.wifi} - ${s.ip}`;
        e('clock').textContent = s.timeSynced ? s.currentDateTime : 'Waiting for NTP time';
        e('mode').textContent = s.controlMode;
        e('soil').textContent = s.soilValid
          ? `${s.soilPercent}%`
          : (s.scheduleFallbackActive ? 'Unavailable - schedule mode' : 'Sensor error');
        e('battery').textContent = `${s.batteryPercent}% (${s.batteryVoltage.toFixed(2)} V)`;
        e('pump').textContent = s.pumpRunning
          ? `#${s.wateringRunCount} ${duration(s.pumpElapsedMs)} / ${duration(s.pumpDurationMs)}`
          : `Off (${s.wateringRunCount} runs)`;
        e('message').textContent = s.message;
        const nextRun = s.scheduleFallbackActive
          ? ` | Next run: ${s.nextScheduledDateTime || duration(s.nextScheduledWateringMs)}`
          : '';
        const watering = s.pumpRunning
          ? ` | Remaining: ${duration(s.pumpRemainingMs)}`
          : '';
        e('details').textContent = `Trigger: ${s.pumpSource} | Soil raw: ${s.soilRaw} | Cooldown: ${duration(s.cooldownRemainingMs)}${watering}${nextRun} | Uptime: ${duration(s.uptimeMs)}`;
        e('water').disabled = s.pumpRunning;
      } catch (_) {
        e('message').textContent = 'Controller is not responding';
      }
    }

    async function waterNow() {
      e('water').disabled = true;
      try {
        const r = await fetch('/pump', {method: 'POST'});
        const result = await r.json();
        e('message').textContent = result.message;
      } finally {
        setTimeout(refresh, 200);
      }
    }

    refresh();
    setInterval(refresh, 1000);
  </script>
</body>
</html>
)HTML";
