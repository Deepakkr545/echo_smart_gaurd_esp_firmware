#include "webserver.h"
#include "config.h"
#include "sensor.h"
#include "alarm.h"
#include "wifi.h"
#include "calibration.h"
#include "notify.h"
#include "display.h"
#include "stats.h"
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h> // OTA firmware updates via /update — see the app's OTA_FIRMWARE_SETUP.md
#include <time.h>

static ESP8266WebServer httpServer(80);
static ESP8266HTTPUpdateServer httpUpdater; // Handles the entire /update OTA upload internally
static Settings *gSettings = nullptr;
static bool *gArmed = nullptr;
static bool *gArmedByNightMode = nullptr;
static float *gLastDistanceCm = nullptr;
static bool *gEStopActive = nullptr;
static unsigned long *gEStopStartMillis = nullptr;
static unsigned long *gMuteStartMillis = nullptr;

static const char SETUP_HTML[] PROGMEM = R"SETUPPAGE(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>First-Time Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,'Segoe UI',Roboto,Arial,sans-serif;background:radial-gradient(circle at top,#161a26,#08090d 75%);color:#e8eaf0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
.card{background:rgba(255,255,255,.05);border:1px solid rgba(148,163,255,.15);border-radius:20px;padding:28px;max-width:380px;width:100%;box-shadow:0 20px 60px -20px rgba(0,0,0,.7);}
h1{font-size:20px;font-weight:700;margin-bottom:6px;background:linear-gradient(90deg,#7dd3fc,#a78bfa);-webkit-background-clip:text;background-clip:text;color:transparent;}
p{font-size:13px;color:#8892b0;margin-bottom:20px;line-height:1.5;}
input{width:100%;padding:13px;margin:8px 0;border-radius:10px;border:1px solid rgba(255,255,255,.1);background:rgba(0,0,0,.35);color:#eee;font-size:14px;}
input:focus{outline:none;border-color:#22d3ee;}
.pwd-wrap{position:relative;}
.pwd-wrap span{position:absolute;right:12px;top:50%;transform:translateY(-50%);cursor:pointer;color:#22d3ee;font-size:11px;font-weight:700;}
button{width:100%;padding:14px;margin-top:10px;border:none;border-radius:12px;background:linear-gradient(135deg,#10b981,#059669);color:#031710;font-weight:700;font-size:14px;cursor:pointer;}
button:active{transform:scale(.97);}
.note{font-size:11.5px;color:#5a6478;margin-top:16px;text-align:center;}
</style>
</head>
<body>
<div class="card">
  <h1>🔧 First-Time Setup</h1>
  <p>Enter your home WiFi details so this device can connect and be reachable from your regular network.</p>
  <input id="ssid" placeholder="WiFi Network Name (SSID)">
  <div class="pwd-wrap">
    <input id="password" placeholder="WiFi Password" type="password">
    <span onclick="togglePwd()" id="pwdToggle">SHOW</span>
  </div>
  <button onclick="save()">Save & Connect</button>
  <div class="note">Device will restart and join your WiFi network. Reconnect your phone to your normal WiFi afterward, and open the device's new IP address (check its OLED screen).</div>
</div>
<script>
function togglePwd(){
  const p=document.getElementById('password'); const t=document.getElementById('pwdToggle');
  if(p.type==='password'){p.type='text';t.innerText='HIDE';}else{p.type='password';t.innerText='SHOW';}
}
async function save(){
  const ssid=document.getElementById('ssid').value;
  const password=document.getElementById('password').value;
  if(!ssid){alert('Enter your WiFi network name');return;}
  const body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(password);
  await fetch('/setwifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  alert('Saved! Device is restarting and will connect to your WiFi.');
}
</script>
</body>
</html>
)SETUPPAGE";

static const char DASHBOARD_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#f7f6f2">
<title id="pageTitle">Echo Smart Gaurd</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 24 24%22><path fill=%22%232563eb%22 d=%22M12 2l8 3v6c0 5-3.5 9.5-8 11-4.5-1.5-8-6-8-11V5l8-3z%22/></svg>">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&display=swap" rel="stylesheet">
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent;}
:root{
  --bg:#f7f6f2; --surface:#ffffff; --surface-2:#fbfaf6; --line:#e7e4dc; --line-2:#d9d5ca;
  --ink:#111418; --ink-2:#3e4550; --ink-3:#6b7280; --ink-mute:#98a0ac;
  --blue:#2563eb; --blue-2:#1d4ed8; --blue-soft:#eaf1ff;
  --green:#059669; --green-soft:#e6f6ee;
  --amber:#b45309; --amber-soft:#fdf2d7;
  --red:#dc2626; --red-soft:#fdecec;
  --violet:#7c3aed; --violet-soft:#f1eafe;
  --teal:#0d9488; --teal-soft:#e3f7f4;
  --orange:#ea580c; --orange-soft:#fef1e6;
  --radius:14px; --radius-lg:20px;
  --shadow-sm:0 1px 2px rgba(15,23,42,.04),0 1px 1px rgba(15,23,42,.03);
  --shadow-md:0 4px 14px -6px rgba(15,23,42,.10),0 2px 6px -2px rgba(15,23,42,.06);
  --shadow-hover:0 10px 26px -10px rgba(15,23,42,.16),0 3px 8px -3px rgba(15,23,42,.08);
}
@media (prefers-color-scheme:dark){
  :root{
    --bg:#0f1115; --surface:#171a21; --surface-2:#1c2028; --line:#252a34; --line-2:#323947;
    --ink:#eef1f6; --ink-2:#c6ccd6; --ink-3:#8a93a1; --ink-mute:#5a626e;
    --blue-soft:#152238; --green-soft:#0f2820; --amber-soft:#2a1f0b; --red-soft:#2a1414;
    --violet-soft:#241a3a; --teal-soft:#0d2624; --orange-soft:#2c1a0d;
    --shadow-md:0 6px 20px -6px rgba(0,0,0,.5);
    --shadow-hover:0 10px 30px -8px rgba(0,0,0,.6);
  }
}
/* Explicit theme override — always wins over system preference once the
   user has picked a theme via the header toggle (persisted in localStorage). */
:root[data-theme="light"]{
  --bg:#f7f6f2; --surface:#ffffff; --surface-2:#fbfaf6; --line:#e7e4dc; --line-2:#d9d5ca;
  --ink:#111418; --ink-2:#3e4550; --ink-3:#6b7280; --ink-mute:#98a0ac;
  --blue-soft:#eaf1ff; --green-soft:#e6f6ee; --amber-soft:#fdf2d7; --red-soft:#fdecec;
  --violet-soft:#f1eafe; --teal-soft:#e3f7f4; --orange-soft:#fef1e6;
  --shadow-md:0 4px 14px -6px rgba(15,23,42,.10),0 2px 6px -2px rgba(15,23,42,.06);
  --shadow-hover:0 10px 26px -10px rgba(15,23,42,.16),0 3px 8px -3px rgba(15,23,42,.08);
}
:root[data-theme="dark"]{
  --bg:#0f1115; --surface:#171a21; --surface-2:#1c2028; --line:#252a34; --line-2:#323947;
  --ink:#eef1f6; --ink-2:#c6ccd6; --ink-3:#8a93a1; --ink-mute:#5a626e;
  --blue-soft:#152238; --green-soft:#0f2820; --amber-soft:#2a1f0b; --red-soft:#2a1414;
  --violet-soft:#241a3a; --teal-soft:#0d2624; --orange-soft:#2c1a0d;
  --shadow-md:0 6px 20px -6px rgba(0,0,0,.5);
  --shadow-hover:0 10px 30px -8px rgba(0,0,0,.6);
}
html{background:var(--bg);}
*{transition:background-color .25s ease,border-color .25s ease,color .25s ease,box-shadow .25s ease;}
html,body{background:var(--bg);color:var(--ink);font-family:'Inter',system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;-webkit-font-smoothing:antialiased;font-feature-settings:"cv11","ss01";}
body{min-height:100vh;padding:0 0 96px;font-size:14px;line-height:1.45;}

/* Icon system */
.i{width:20px;height:20px;stroke:currentColor;fill:none;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;flex:none;}
.i-sm{width:16px;height:16px;}
.i-lg{width:28px;height:28px;}
.i-xl{width:44px;height:44px;stroke-width:1.6;}

/* Shell */
.shell{width:100%;max-width:none;margin:0;padding:16px;}
@media(min-width:720px){.shell{padding:24px 32px;}}
@media(min-width:1440px){.shell{padding:28px 56px;}}

/* Top bar */
.topbar{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:6px;flex-wrap:wrap;}
.brand{display:flex;align-items:center;gap:10px;min-width:0;}
.brand .logo{width:36px;height:36px;border-radius:10px;background:var(--blue);color:#fff;display:grid;place-items:center;flex:none;}
.brand h1{font-size:15px;font-weight:700;letter-spacing:-.01em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.brand .sub{font-size:11px;color:var(--ink-3);font-weight:500;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.header-right{display:flex;align-items:center;gap:8px;flex-wrap:wrap;justify-content:flex-end;}
.badges{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end;}
.hbadge{font-size:11px;font-weight:600;padding:5px 9px;border-radius:999px;display:inline-flex;align-items:center;gap:5px;background:var(--surface);border:1px solid var(--line);color:var(--ink-2);}
.hbadge .dot{width:6px;height:6px;border-radius:50%;background:currentColor;}
.g-safe{color:var(--green);background:var(--green-soft);border-color:transparent;}
.g-info{color:var(--blue);background:var(--blue-soft);border-color:transparent;}
.g-warn{color:var(--amber);background:var(--amber-soft);border-color:transparent;}
.g-crit{color:var(--red);background:var(--red-soft);border-color:transparent;}
.g-off{color:var(--ink-3);}
.g-crit .dot{animation:pulse 1s infinite;}
@keyframes pulse{50%{opacity:.35;}}

.theme-toggle{width:34px;height:34px;border-radius:10px;border:1px solid var(--line);background:var(--surface);color:var(--ink-2);display:grid;place-items:center;cursor:pointer;flex:none;}
.theme-toggle:hover{border-color:var(--line-2);background:var(--surface-2);color:var(--ink);}
.theme-toggle .i{width:17px;height:17px;}
.theme-toggle:active{transform:scale(.92);}

.updated-row{display:flex;align-items:center;gap:6px;color:var(--ink-mute);font-size:11.5px;font-weight:500;margin:2px 0 16px;}
.updated-row .i{width:13px;height:13px;}

/* Tabs / panels */
.tab-panel{display:none;animation:fade .18s ease;}
.tab-panel.active{display:block;}
@keyframes fade{from{opacity:0;transform:translateY(4px);}to{opacity:1;transform:none;}}

/* Card */
.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--radius-lg);padding:18px;box-shadow:var(--shadow-sm);margin-bottom:14px;}
.card-hd{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px;}
.card-hd h3{font-size:13px;font-weight:700;letter-spacing:.02em;color:var(--ink);display:flex;align-items:center;gap:8px;}
.card-hd h3 .i{color:var(--ink-3);}
.card-hd .meta{font-size:11px;color:var(--ink-3);font-weight:500;}

/* HERO */
.hero{position:relative;overflow:hidden;padding:26px 22px;background:
  radial-gradient(ellipse 640px 260px at 12% -10%, var(--blue-soft) 0%, transparent 60%),
  radial-gradient(ellipse 480px 240px at 105% 10%, var(--violet-soft) 0%, transparent 55%),
  var(--surface);}
.hero.st-safe{background:
  radial-gradient(ellipse 640px 260px at 12% -10%, var(--green-soft) 0%, transparent 60%),
  radial-gradient(ellipse 480px 240px at 105% 10%, var(--teal-soft) 0%, transparent 55%),
  var(--surface);}
.hero.st-alert{background:
  radial-gradient(ellipse 640px 280px at 12% -10%, var(--red-soft) 0%, transparent 62%),
  radial-gradient(ellipse 480px 240px at 105% 10%, var(--orange-soft) 0%, transparent 55%),
  var(--surface);}
.hero.st-off{background:
  radial-gradient(ellipse 640px 260px at 12% -10%, var(--surface-2) 0%, transparent 60%),
  var(--surface);}
.hero-grid{display:grid;grid-template-columns:1fr;gap:22px;align-items:stretch;position:relative;z-index:1;}
@media(min-width:860px){.hero-grid{grid-template-columns:1.1fr 1fr;gap:20px;}}
.hero-left{text-align:center;display:flex;flex-direction:column;align-items:center;}
@media(min-width:860px){.hero-left{text-align:left;align-items:flex-start;}}
.hero-chip{display:inline-flex;align-items:center;gap:6px;font-size:10.5px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--ink-3);background:var(--surface-2);border:1px solid var(--line);padding:4px 10px;border-radius:999px;margin-bottom:12px;}
.hero-chip .dot{width:6px;height:6px;border-radius:50%;background:var(--ink-mute);}
.hero.st-safe .hero-chip{color:var(--green);border-color:transparent;background:var(--green-soft);}
.hero.st-safe .hero-chip .dot{background:var(--green);}
.hero.st-alert .hero-chip{color:var(--red);border-color:transparent;background:var(--red-soft);}
.hero.st-alert .hero-chip .dot{background:var(--red);animation:pulse .8s infinite;}
.state-detail{font-size:12px;color:var(--ink-mute);margin-top:10px;font-weight:500;display:flex;align-items:center;gap:6px;flex-wrap:wrap;justify-content:center;}
@media(min-width:860px){.state-detail{justify-content:flex-start;}}
.state-detail b{color:var(--ink-2);font-weight:700;}

.hero .state-ic{width:78px;height:78px;border-radius:22px;display:grid;place-items:center;margin:0 auto 14px;background:var(--blue-soft);color:var(--blue);transition:background-color .25s ease,color .25s ease,transform .3s cubic-bezier(.34,1.56,.64,1);}
@media(min-width:860px){.hero .state-ic{margin:0 0 14px;}}
.hero .state-ic .i{width:42px;height:42px;stroke-width:1.5;}
.hero .state-ic.icon-pulse{animation:iconPulse .5s cubic-bezier(.34,1.56,.64,1);}
@keyframes iconPulse{0%{transform:scale(1);}45%{transform:scale(1.16) rotate(-4deg);}100%{transform:scale(1);}}
.hero .state-title{font-size:24px;font-weight:800;letter-spacing:-.02em;color:var(--ink);}
.hero .state-msg{font-size:13.5px;color:var(--ink-3);margin-top:4px;font-weight:500;max-width:360px;}
.hero.st-safe .state-ic{background:var(--green-soft);color:var(--green);}
.hero.st-alert .state-ic{background:var(--red-soft);color:var(--red);animation:shake .5s infinite;}
.hero.st-alert{border-color:var(--red);box-shadow:0 0 0 3px var(--red-soft);}
.hero.st-off .state-ic{background:#eef0f4;color:var(--ink-3);}
@media (prefers-color-scheme:dark){.hero.st-off .state-ic{background:#242832;}}
:root[data-theme="dark"] .hero.st-off .state-ic{background:#242832;}
@keyframes shake{0%,100%{transform:translateX(0);}25%{transform:translateX(-3px);}75%{transform:translateX(3px);}}

#zoneIcon{position:absolute;left:-9999px;top:-9999px;width:0;height:0;overflow:hidden;}
.zone-vis{contain:layout;}

/* Primary action */
.primary-action{margin-top:18px;display:flex;flex-direction:column;gap:10px;align-items:center;width:100%;}
@media(min-width:860px){.primary-action{align-items:flex-start;}}
.btn{font-family:inherit;font-size:14px;font-weight:600;padding:12px 18px;border:1px solid var(--line);border-radius:12px;background:var(--surface);color:var(--ink);cursor:pointer;transition:.12s;display:inline-flex;align-items:center;justify-content:center;gap:8px;}
.btn:hover{border-color:var(--line-2);background:var(--surface-2);}
.btn:active{transform:scale(.98);}
.btn:disabled{opacity:.5;cursor:not-allowed;}
.btn-big{width:100%;max-width:340px;padding:16px 20px;font-size:15px;border-radius:14px;font-weight:700;}
.btn-arm{background:var(--green);color:#fff;border-color:transparent;box-shadow:0 6px 18px -6px rgba(5,150,105,.5);}
.btn-arm:hover{background:#047857;border-color:transparent;}
.btn-disarm{background:var(--red);color:#fff;border-color:transparent;box-shadow:0 6px 18px -6px rgba(220,38,38,.5);}
.btn-disarm:hover{background:#b91c1c;border-color:transparent;}
.btn-ghost{background:transparent;border:none;color:var(--ink-3);font-weight:500;font-size:13px;padding:8px 10px;text-decoration:underline;text-underline-offset:3px;}
.btn-ghost:hover{color:var(--ink);background:transparent;}
.led-dot{display:none;}

/* Hero right: radar + live reading */
.hero-right{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:16px;background:var(--surface-2);border:1px solid var(--line);border-radius:18px;padding:22px 18px;}
.radar-wrap{position:relative;width:172px;height:172px;flex:none;}
@media(min-width:860px){.radar-wrap{width:196px;height:196px;}}
.radar-wrap::before{content:'';position:absolute;inset:8px;border-radius:50%;background:conic-gradient(from 0deg,rgba(37,99,235,.22),transparent 28%,transparent 100%);animation:radarSpin 9s linear infinite;filter:blur(1px);}
.radar-svg{width:100%;height:100%;display:block;position:relative;z-index:1;}
.radar-ring{fill:none;stroke:var(--blue);stroke-width:1;opacity:.35;}
.radar-ring.r2{opacity:.22;}
.radar-ring.r3{opacity:.14;}
.radar-ring.r4{opacity:.09;}
.radar-sweep{stroke:var(--blue);stroke-width:2;stroke-linecap:round;transform-origin:50px 50px;animation:radarSpin 9s linear infinite;opacity:.65;}
.radar-core{fill:var(--blue);opacity:.9;}
.radar-core.pulse-core{animation:coreBlip 2.4s ease-in-out infinite;}
@keyframes coreBlip{0%,100%{r:2.6;opacity:.9;}50%{r:3.6;opacity:1;}}
.radar-center-ic{position:absolute;inset:0;display:grid;place-items:center;color:var(--ink-3);pointer-events:none;z-index:2;}
.radar-center-ic .i{width:26px;height:26px;opacity:.55;}
@keyframes radarSpin{from{transform:rotate(0deg);}to{transform:rotate(360deg);}}
@keyframes ringPulseGreen{0%{stroke-opacity:.55;stroke-width:1;}70%{stroke-opacity:0;stroke-width:6;}100%{stroke-opacity:0;stroke-width:6;}}
@keyframes ringPulseRed{0%{stroke-opacity:.7;stroke-width:1.5;}60%{stroke-opacity:0;stroke-width:8;}100%{stroke-opacity:0;stroke-width:8;}}
.radar-wrap.st-armed::before{background:conic-gradient(from 0deg,rgba(5,150,105,.28),transparent 32%,transparent 100%);animation-duration:5s;}
.radar-wrap.st-armed .radar-ring{stroke:var(--green);}
.radar-wrap.st-armed .radar-ring.r2{stroke:var(--teal);}
.radar-wrap.st-armed .radar-sweep{stroke:var(--green);animation-duration:5s;opacity:.8;}
.radar-wrap.st-armed .radar-core{fill:var(--green);}
.radar-wrap.st-armed .r1{animation:ringPulseGreen 2.6s ease-out infinite;}
.radar-wrap.st-armed .r2{animation:ringPulseGreen 2.6s ease-out infinite .5s;}
.radar-wrap.st-armed .r3{animation:ringPulseGreen 2.6s ease-out infinite 1s;}
.radar-wrap.st-armed .r4{animation:ringPulseGreen 2.6s ease-out infinite 1.5s;}
.radar-wrap.st-alert::before{background:conic-gradient(from 0deg,rgba(220,38,38,.35),transparent 38%,transparent 100%);animation-duration:1.1s;}
.radar-wrap.st-alert .radar-ring{stroke:var(--red);}
.radar-wrap.st-alert .radar-ring.r2{stroke:var(--orange);}
.radar-wrap.st-alert .radar-sweep{stroke:var(--red);animation-duration:1.1s;opacity:.95;}
.radar-wrap.st-alert .radar-core{fill:var(--red);}
.radar-wrap.st-alert .r1{animation:ringPulseRed 1.1s ease-out infinite;}
.radar-wrap.st-alert .r2{animation:ringPulseRed 1.1s ease-out infinite .25s;}
.radar-wrap.st-alert .r3{animation:ringPulseRed 1.1s ease-out infinite .5s;}
.radar-wrap.st-alert .r4{animation:ringPulseRed 1.1s ease-out infinite .75s;}
.radar-wrap.st-alert{animation:alertGlow 1.1s ease-in-out infinite;border-radius:50%;}
@keyframes alertGlow{0%,100%{filter:drop-shadow(0 0 0 rgba(220,38,38,0));}50%{filter:drop-shadow(0 0 14px rgba(220,38,38,.5));}}
.radar-wrap.st-off::before{display:none;}
.radar-wrap.st-off .radar-ring{stroke:var(--ink-mute);opacity:.22;}
.radar-wrap.st-off .radar-ring.r2{stroke:var(--ink-mute);}
.radar-wrap.st-off .radar-sweep{display:none;}
.radar-wrap.st-off .radar-core{fill:var(--ink-mute);opacity:.4;}
.radar-wrap.st-off .radar-center-ic{color:var(--ink-mute);}

.hero-reading{text-align:center;}
.hero-reading-lbl{font-size:10.5px;text-transform:uppercase;letter-spacing:.12em;color:var(--ink-3);font-weight:700;}
.hero-reading-val{font-size:32px;font-weight:800;letter-spacing:-.02em;color:var(--ink);font-variant-numeric:tabular-nums;margin-top:2px;}
.hero-reading-val .unit{font-size:14px;color:var(--ink-3);font-weight:500;margin-left:4px;}
.radar-cap{font-size:11px;color:var(--ink-mute);font-weight:600;margin-top:4px;}
.hero-substats{display:flex;align-items:stretch;gap:0;width:100%;max-width:260px;border-top:1px solid var(--line);margin-top:4px;padding-top:14px;}
.hero-substat{flex:1;text-align:center;padding:0 10px;}
.hero-substat+.hero-substat{border-left:1px solid var(--line);}
.hero-substat-lbl{font-size:9.5px;text-transform:uppercase;letter-spacing:.08em;color:var(--ink-mute);font-weight:700;}
.hero-substat-val{font-size:14.5px;font-weight:700;color:var(--ink-2);margin-top:3px;font-variant-numeric:tabular-nums;}

/* KPI strip */
.kpi-strip{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px;}
@media(min-width:720px){.kpi-strip{grid-template-columns:repeat(4,1fr);}}
.kpi-tile{background:var(--surface);border:1px solid var(--line);border-radius:var(--radius);padding:14px 15px;box-shadow:var(--shadow-sm);display:flex;flex-direction:column;gap:8px;}
.kpi-tile:hover{box-shadow:var(--shadow-hover);border-color:var(--line-2);}
.kpi-ic{width:30px;height:30px;border-radius:9px;display:grid;place-items:center;background:var(--blue-soft);color:var(--blue);}
.kpi-lbl{font-size:10.5px;text-transform:uppercase;letter-spacing:.09em;color:var(--ink-3);font-weight:700;}
.kpi-val{font-size:21px;font-weight:800;color:var(--ink);font-variant-numeric:tabular-nums;letter-spacing:-.02em;}
.kpi-val .kpi-unit{font-size:11.5px;color:var(--ink-3);font-weight:600;margin-left:4px;letter-spacing:0;}
.kpi-cap{font-size:11px;color:var(--ink-mute);font-weight:500;}
.kpi-ic.ic-violet{background:var(--violet-soft);color:var(--violet);}
.kpi-ic.ic-teal{background:var(--teal-soft);color:var(--teal);}
.kpi-ic.ic-orange{background:var(--orange-soft);color:var(--orange);}
.kpi-tile.kpi-calm-blue .kpi-ic{background:var(--blue-soft);color:var(--blue);}
.kpi-tile.kpi-calm-green .kpi-ic{background:var(--green-soft);color:var(--green);}
.kpi-tile.kpi-warn{background:var(--amber-soft);border-color:transparent;}
.kpi-tile.kpi-warn .kpi-ic{background:rgba(180,83,9,.16);color:var(--amber);}
.kpi-tile.kpi-warn .kpi-val{color:var(--amber);}
.kpi-tile.kpi-alert{background:var(--red-soft);border-color:transparent;}
.kpi-tile.kpi-alert .kpi-ic{background:rgba(220,38,38,.16);color:var(--red);}
.kpi-tile.kpi-alert .kpi-val{color:var(--red);}
.kpi-tile.kpi-alert{animation:tileFlash 1s ease-in-out infinite;}
@keyframes tileFlash{0%,100%{box-shadow:var(--shadow-sm);}50%{box-shadow:0 0 0 3px var(--red-soft);}}
.num-bump{display:inline-block;animation:bump .28s ease;}
@keyframes bump{0%{transform:translateY(-4px);opacity:.4;}100%{transform:translateY(0);opacity:1;}}

/* Quick controls grid */
.qc-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;}
@media(max-width:420px){.qc-grid{grid-template-columns:1fr 1fr;}}
.qc{display:flex;flex-direction:column;align-items:center;gap:6px;padding:14px 8px;background:var(--surface);border:1px solid var(--line);border-radius:12px;font-size:12px;font-weight:600;color:var(--ink-2);cursor:pointer;transition:.12s;text-align:center;position:relative;}
.qc:hover{border-color:var(--line-2);box-shadow:var(--shadow-hover);}
.qc:active{transform:scale(.97);}
.qc .i{color:var(--ink-2);}
.qc .qc-state{position:absolute;top:8px;right:8px;width:7px;height:7px;border-radius:50%;background:var(--ink-mute);opacity:.5;}
.qc.on{background:var(--amber-soft);border-color:transparent;color:var(--amber);}
.qc.on .i{color:var(--amber);}
.qc.on .qc-state{background:var(--amber);opacity:1;}
.qc.warn{background:var(--red-soft);border-color:transparent;color:var(--red);}
.qc.warn .i{color:var(--red);}
.qc.warn .qc-state{background:var(--red);opacity:1;}
.qc-estop{border:1.5px solid var(--red);color:var(--red);background:transparent;}
.qc-estop .i{color:var(--red);}
.qc-estop:hover{background:var(--red-soft);}
.qc-estop.warn{background:var(--red);color:#fff;}
.qc-estop.warn .i{color:#fff;}
.qc-estop.warn .qc-state{background:#fff;}

/* Recent activity */
.activity-mini{display:flex;flex-direction:column;gap:2px;}
.act-group-lbl{font-size:10.5px;text-transform:uppercase;letter-spacing:.1em;color:var(--ink-mute);font-weight:700;padding:8px 4px 4px;}
.act-item{display:grid;grid-template-columns:auto auto 1fr;gap:10px;padding:9px 4px;font-size:13px;border-bottom:1px dashed var(--line);align-items:center;}
.act-item:last-child{border-bottom:none;}
.act-mk{width:9px;height:9px;border-radius:50%;flex:none;background:var(--ink-mute);}
.act-mk.mk-green{background:var(--green);}
.act-mk.mk-red{background:var(--red);}
.act-mk.mk-amber{background:var(--amber);}
.act-mk.mk-blue{background:var(--blue);}
.act-time{color:var(--ink-3);font-size:11.5px;font-variant-numeric:tabular-nums;}
.act-item.act-new{animation:slideIn .3s cubic-bezier(.2,.7,.3,1);}
@keyframes slideIn{from{opacity:0;transform:translateX(-10px);}to{opacity:1;transform:none;}}
.act-empty{padding:18px 8px;text-align:center;color:var(--ink-mute);font-size:12.5px;display:flex;flex-direction:column;align-items:center;gap:8px;}
.act-empty .i{width:26px;height:26px;opacity:.4;}

/* Data list */
.data-list{display:flex;flex-direction:column;gap:0;}
.data-list .row{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:11px 0;border-bottom:1px solid var(--line);font-size:13px;}
.data-list .row:last-child{border-bottom:none;}
.data-list .k{color:var(--ink-3);display:flex;align-items:center;gap:8px;font-weight:500;}
.data-list .k .ic{font-size:14px;opacity:.7;}
.data-list .v{font-weight:600;color:var(--ink);font-variant-numeric:tabular-nums;text-align:right;}
.data-list .v.good{color:var(--green);} .data-list .v.warn{color:var(--amber);} .data-list .v.bad{color:var(--red);}

/* Charts */
.chart-wrap{margin-top:12px;}
.chart-hd{display:flex;align-items:center;justify-content:space-between;margin-bottom:6px;}
.chart-title{font-size:11px;font-weight:700;color:var(--ink-3);text-transform:uppercase;letter-spacing:.08em;}
.chart-legend{display:flex;gap:10px;font-size:10.5px;color:var(--ink-mute);font-weight:600;}
.chart-legend span{display:inline-flex;align-items:center;gap:4px;}
.chart-legend .sw{width:10px;height:2px;border-radius:2px;display:inline-block;}
.chart-legend .sw.dashed{background:transparent;border-top:2px dashed var(--ink-mute);width:12px;height:0;}
canvas{width:100%;height:64px;display:block;border-radius:8px;background:var(--surface-2);border:1px solid var(--line);}

/* Settings tabs */
.settings-tabs{display:flex;gap:2px;background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:4px;margin-bottom:14px;overflow-x:auto;-webkit-overflow-scrolling:touch;scrollbar-width:none;}
.settings-tabs::-webkit-scrollbar{display:none;}
.st-btn{flex:1;min-width:80px;padding:9px 12px;font-size:12.5px;font-weight:600;color:var(--ink-3);background:transparent;border:none;border-radius:8px;cursor:pointer;white-space:nowrap;display:inline-flex;align-items:center;justify-content:center;gap:6px;transition:.12s;}
.st-btn:hover{color:var(--ink);}
.st-btn.active{background:var(--surface-2);color:var(--ink);box-shadow:var(--shadow-sm);}
.st-panel{display:none;}
.st-panel.active{display:block;animation:fade .18s ease;}

/* Forms */
.field{margin:10px 0;}
.field label{display:block;font-size:12px;font-weight:600;color:var(--ink-2);margin-bottom:6px;}
input,select,textarea{width:100%;padding:11px 13px;border-radius:10px;border:1px solid var(--line-2);background:var(--surface-2);color:var(--ink);font-size:13.5px;font-family:inherit;transition:.12s;}
textarea{resize:vertical;min-height:76px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12.5px;}
input:focus,select:focus,textarea:focus{outline:none;border-color:var(--blue);box-shadow:0 0 0 3px var(--blue-soft);}
input::placeholder{color:var(--ink-mute);}
.pwd-wrap{position:relative;}
.pwd-wrap>span{position:absolute;right:12px;top:50%;transform:translateY(-50%);cursor:pointer;color:var(--blue);font-size:11px;font-weight:700;letter-spacing:.06em;user-select:none;}
.form-actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px;}
.form-actions .btn{flex:1;min-width:140px;}
.btn-primary{background:var(--blue);color:#fff;border-color:transparent;}
.btn-primary:hover{background:var(--blue-2);border-color:transparent;}
.btn-danger{background:var(--red);color:#fff;border-color:transparent;}
.btn-danger:hover{background:#b91c1c;border-color:transparent;}
.btn-outline-danger{background:transparent;color:var(--red);border-color:var(--red);}
.btn-outline-danger:hover{background:var(--red-soft);border-color:var(--red);}
.section-title{font-size:11px;text-transform:uppercase;letter-spacing:.14em;color:var(--ink-3);font-weight:700;margin:20px 0 10px;display:flex;align-items:center;gap:8px;}
.section-title:first-child{margin-top:6px;}
.helper{font-size:12px;color:var(--ink-3);margin-top:-2px;margin-bottom:10px;line-height:1.5;}

/* Legacy hidden equivalents so JS still targets valid ids */
#armLed,#disarmLed{display:none;}
.collapsible-body{display:block;}
.collapsible-head{display:none;}

/* Danger card */
.card.danger-card{border-color:var(--red);background:var(--red-soft);}
.card.danger-card .card-hd h3{color:var(--red);}
.card.danger-card .card-hd h3 .i{color:var(--red);}

/* Sibling row */
#siblingListDisplay .row{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px dashed var(--line);font-size:13px;}
#siblingListDisplay .row:last-child{border-bottom:none;}
.sensor-block{background:var(--surface-2);border:1px solid var(--line);border-radius:10px;padding:6px 12px;margin-bottom:8px;}
.sensor-block .row{display:flex;justify-content:space-between;padding:6px 0;font-size:12.5px;border-bottom:1px dashed var(--line);}
.sensor-block .row:last-child{border-bottom:none;}
.sensor-block .row .v{font-weight:600;color:var(--ink);}
.sensor-block .row .v.good{color:var(--green);} .sensor-block .row .v.warn{color:var(--amber);} .sensor-block .row .v.bad{color:var(--red);}

/* Bottom nav */
.bottom-nav{position:fixed;bottom:0;left:0;right:0;background:var(--surface);border-top:1px solid var(--line);display:grid;grid-template-columns:1fr 1fr 1fr;padding:6px 4px calc(6px + env(safe-area-inset-bottom));z-index:80;box-shadow:0 -4px 20px -8px rgba(15,23,42,.06);}
.bn-btn{background:transparent;border:none;padding:8px 4px;display:flex;flex-direction:column;align-items:center;gap:2px;font-size:11px;font-weight:600;color:var(--ink-3);cursor:pointer;border-radius:10px;}
.bn-btn .i{width:22px;height:22px;}
.bn-btn.active{color:var(--blue);}
.bn-btn.alert{color:var(--red);}
@media(min-width:720px){
  .bottom-nav{position:sticky;top:12px;bottom:auto;width:calc(100% - 64px);max-width:none;margin:0 32px 16px;border:1px solid var(--line);border-radius:14px;grid-template-columns:repeat(3,1fr);justify-content:stretch;box-shadow:var(--shadow-sm);}
  .bn-btn{flex-direction:row;padding:9px 16px;gap:8px;font-size:13px;justify-content:center;}
  body{padding-bottom:32px;}
}
@media(min-width:1100px){.bottom-nav{margin-left:32px;margin-right:32px;}}

/* Toast */
#toastBox{position:fixed;bottom:96px;left:16px;right:16px;z-index:99;display:flex;flex-direction:column;gap:8px;align-items:center;pointer-events:none;}
@media(min-width:720px){#toastBox{bottom:24px;}}
.toast{background:var(--ink);color:var(--surface);padding:11px 14px 11px 12px;border-radius:12px;font-size:13px;font-weight:500;box-shadow:0 12px 30px -8px rgba(0,0,0,.35);display:flex;align-items:center;gap:9px;max-width:420px;pointer-events:auto;animation:slideUp .28s cubic-bezier(.2,.8,.3,1);border-left:3px solid var(--blue);}
.toast .t-ic{flex:none;width:18px;height:18px;display:grid;place-items:center;}
.toast.t-success{border-left-color:var(--green);}
.toast.t-warn{border-left-color:var(--amber);}
.toast.t-error{border-left-color:var(--red);}
.toast.leaving{animation:slideDown .22s ease forwards;}
@keyframes slideUp{from{opacity:0;transform:translateY(10px);}to{opacity:1;transform:none;}}
@keyframes slideDown{to{opacity:0;transform:translateY(8px);}}

@media (prefers-reduced-motion: reduce){
  *,*::before,*::after{animation:none!important;transition:none!important;}
}
</style>
</head>
<body>

<!-- Reusable inline SVG icon defs -->
<svg width="0" height="0" style="position:absolute" aria-hidden="true">
<defs>
<symbol id="ic-shield" viewBox="0 0 24 24"><path d="M12 3l8 3v6c0 4.6-3.2 8.7-8 10-4.8-1.3-8-5.4-8-10V6l8-3z"/></symbol>
<symbol id="ic-shield-check" viewBox="0 0 24 24"><path d="M12 3l8 3v6c0 4.6-3.2 8.7-8 10-4.8-1.3-8-5.4-8-10V6l8-3z"/><path d="M8.5 12l2.5 2.5L15.5 10"/></symbol>
<symbol id="ic-alert" viewBox="0 0 24 24"><path d="M12 3l10 18H2L12 3z"/><path d="M12 10v5"/><circle cx="12" cy="18" r=".6" fill="currentColor"/></symbol>
<symbol id="ic-shield-off" viewBox="0 0 24 24"><path d="M12 3l8 3v6c0 4.6-3.2 8.7-8 10-4.8-1.3-8-5.4-8-10V6l8-3z"/><path d="M5 5l14 14"/></symbol>
<symbol id="ic-bell" viewBox="0 0 24 24"><path d="M6 16V11a6 6 0 0 1 12 0v5l1.5 2h-15L6 16z"/><path d="M10 20a2 2 0 0 0 4 0"/></symbol>
<symbol id="ic-bell-off" viewBox="0 0 24 24"><path d="M6 16V11a6 6 0 0 1 9.5-4.9"/><path d="M18 11v5l1.5 2h-13"/><path d="M4 4l16 16"/><path d="M10 20a2 2 0 0 0 4 0"/></symbol>
<symbol id="ic-siren" viewBox="0 0 24 24"><path d="M6 18V13a6 6 0 0 1 12 0v5"/><rect x="4" y="18" width="16" height="3" rx="1"/><path d="M12 4V2M4.5 7l-1.4-1.4M19.5 7l1.4-1.4"/></symbol>
<symbol id="ic-siren-off" viewBox="0 0 24 24"><path d="M7 18V13a5 5 0 0 1 8-4"/><rect x="4" y="18" width="16" height="3" rx="1"/><path d="M4 4l16 16"/></symbol>
<symbol id="ic-monitor" viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/></symbol>
<symbol id="ic-moon" viewBox="0 0 24 24"><path d="M20 14A8 8 0 0 1 10 4a8 8 0 1 0 10 10z"/></symbol>
<symbol id="ic-sun" viewBox="0 0 24 24"><circle cx="12" cy="12" r="4.2"/><path d="M12 2.5v2.4M12 19.1v2.4M4.2 4.2l1.7 1.7M18.1 18.1l1.7 1.7M2.5 12h2.4M19.1 12h2.4M4.2 19.8l1.7-1.7M18.1 5.9l1.7-1.7"/></symbol>
<symbol id="ic-volume" viewBox="0 0 24 24"><path d="M4 10v4h4l5 4V6l-5 4H4z"/><path d="M17 8a5 5 0 0 1 0 8"/></symbol>
<symbol id="ic-volume-off" viewBox="0 0 24 24"><path d="M4 10v4h4l5 4V6l-5 4H4z"/><path d="M18 9l4 6M22 9l-4 6"/></symbol>
<symbol id="ic-wifi" viewBox="0 0 24 24"><path d="M2 8.5a15 15 0 0 1 20 0"/><path d="M5 12a11 11 0 0 1 14 0"/><path d="M8.5 15.5a6 6 0 0 1 7 0"/><circle cx="12" cy="19" r="1" fill="currentColor"/></symbol>
<symbol id="ic-stop" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M8 8l8 8M16 8l-8 8"/></symbol>
<symbol id="ic-play" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M10 8l6 4-6 4V8z" fill="currentColor"/></symbol>
<symbol id="ic-home" viewBox="0 0 24 24"><path d="M3 11l9-7 9 7v9a1 1 0 0 1-1 1h-5v-6h-6v6H4a1 1 0 0 1-1-1v-9z"/></symbol>
<symbol id="ic-activity" viewBox="0 0 24 24"><path d="M3 12h4l3-8 4 16 3-8h4"/></symbol>
<symbol id="ic-gear" viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.6 1.6 0 0 0 .3 1.7l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.6 1.6 0 0 0-1.7-.3 1.6 1.6 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.6 1.6 0 0 0-1-1.5 1.6 1.6 0 0 0-1.7.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.6 1.6 0 0 0 .3-1.7 1.6 1.6 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.6 1.6 0 0 0 1.5-1 1.6 1.6 0 0 0-.3-1.7l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.6 1.6 0 0 0 1.7.3H9a1.6 1.6 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.6 1.6 0 0 0 1 1.5 1.6 1.6 0 0 0 1.7-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.6 1.6 0 0 0-.3 1.7V9a1.6 1.6 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.6 1.6 0 0 0-1.5 1z"/></symbol>
<symbol id="ic-ruler" viewBox="0 0 24 24"><path d="M3 17L17 3l4 4L7 21l-4-4z"/><path d="M8 8l2 2M11 5l2 2M14 11l2 2M5 14l2 2"/></symbol>
<symbol id="ic-clock" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></symbol>
<symbol id="ic-refresh" viewBox="0 0 24 24"><path d="M20 12a8 8 0 1 1-2.3-5.6"/><path d="M20 4v5h-5"/></symbol>
<symbol id="ic-plus" viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></symbol>
<symbol id="ic-trash" viewBox="0 0 24 24"><path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/></symbol>
<symbol id="ic-save" viewBox="0 0 24 24"><path d="M5 3h11l4 4v14a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M8 3v5h8V3M8 15h8v6H8z"/></symbol>
<symbol id="ic-power" viewBox="0 0 24 24"><path d="M12 3v9"/><path d="M6.4 6.4a8 8 0 1 0 11.2 0"/></symbol>
<symbol id="ic-user" viewBox="0 0 24 24"><circle cx="12" cy="8" r="4"/><path d="M4 21c1-4 5-6 8-6s7 2 8 6"/></symbol>
<symbol id="ic-lock" viewBox="0 0 24 24"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 1 1 8 0v3"/></symbol>
<symbol id="ic-devices" viewBox="0 0 24 24"><rect x="3" y="5" width="12" height="10" rx="1"/><rect x="14" y="10" width="7" height="10" rx="1"/><path d="M6 19h6"/></symbol>
<symbol id="ic-send" viewBox="0 0 24 24"><path d="M22 2L11 13"/><path d="M22 2l-7 20-4-9-9-4 20-7z"/></symbol>
<symbol id="ic-radio" viewBox="0 0 24 24"><circle cx="12" cy="12" r="2"/><path d="M8 8a6 6 0 0 0 0 8M16 8a6 6 0 0 1 0 8M5 5a10 10 0 0 0 0 14M19 5a10 10 0 0 1 0 14"/></symbol>
<symbol id="ic-check" viewBox="0 0 24 24"><path d="M4 12l5 5L20 6"/></symbol>
<symbol id="ic-info" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 11v6"/><circle cx="12" cy="7.5" r=".6" fill="currentColor"/></symbol>
<symbol id="ic-inbox" viewBox="0 0 24 24"><path d="M3 12h5l2 3h4l2-3h5"/><path d="M5 5h14l2 7v7a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1v-7l2-7z"/></symbol>
</defs>
</svg>

<div class="shell">

<div class="topbar">
  <div class="brand">
    <div class="logo"><svg class="i i-lg" style="width:22px;height:22px;color:#fff"><use href="#ic-shield-check"/></svg></div>
    <div style="min-width:0">
      <h1 id="brandTitle">Echo Smart Gaurd</h1>
      <div class="sub" id="fwInfo">Loading telemetry...</div>
    </div>
  </div>
  <div class="header-right">
    <div class="badges" id="hBadges"></div>
    <button class="theme-toggle" id="themeToggle" onclick="toggleTheme()" aria-label="Toggle day/night theme">
      <svg class="i"><use href="#ic-moon" id="themeIconRef"/></svg>
    </button>
  </div>
</div>
<div class="updated-row"><svg class="i i-sm"><use href="#ic-clock"/></svg><span id="lastUpdatedTxt">Updated just now</span></div>

<!-- Bottom / top nav -->
<nav class="bottom-nav" role="tablist">
  <button class="bn-btn active" data-tab="overview" onclick="switchTab('overview')" aria-label="Overview">
    <svg class="i"><use href="#ic-home"/></svg><span>Overview</span>
  </button>
  <button class="bn-btn" data-tab="activity" onclick="switchTab('activity')" aria-label="Activity">
    <svg class="i"><use href="#ic-activity"/></svg><span>Activity</span>
  </button>
  <button class="bn-btn" data-tab="settings" onclick="switchTab('settings')" aria-label="Settings">
    <svg class="i"><use href="#ic-gear"/></svg><span>Settings</span>
  </button>
</nav>

<!-- =================== OVERVIEW =================== -->
<section class="tab-panel active" id="tab-overview">

<div class="card hero zone-vis st-off" id="heroCard">
  <div class="hero-grid">
    <div class="hero-left">
      <div class="hero-chip" id="heroChip"><span class="dot"></span><span id="heroChipTxt">System idle</span></div>
      <div class="state-ic" id="stateIc"><svg class="i"><use href="#ic-shield-off" id="stateIconRef"/></svg></div>
      <div class="state-title" id="stateTitle">Disarmed</div>
      <div class="state-msg" id="stateMsg">Home security is off. Arm to start monitoring.</div>
      <div class="state-detail" id="stateDetail">Awaiting first sensor reading…</div>
      <!-- Legacy element preserved for JS compatibility (hidden) -->
      <span id="zoneIcon">shield</span>

      <div class="primary-action">
        <button class="btn btn-big btn-arm" id="armBtn" onclick="post('/arm','System armed','armed','success')">
          <svg class="i" style="color:#fff"><use href="#ic-shield-check"/></svg> Arm System
          <span class="led-dot" id="armLed"></span>
        </button>
        <button class="btn btn-big btn-disarm" id="disarmBtn" onclick="post('/disarm','System disarmed','paused','warn')" style="display:none">
          <svg class="i" style="color:#fff"><use href="#ic-shield-off"/></svg> Disarm
          <span class="led-dot" id="disarmLed"></span>
        </button>
        <button class="btn-ghost" id="secondaryActionBtn" onclick="secondaryAction()">Disarm instead</button>
      </div>
    </div>

    <div class="hero-right">
      <div class="radar-wrap st-off" id="radarWrap">
        <svg class="radar-svg" viewBox="0 0 100 100" aria-hidden="true">
          <circle class="radar-ring r1" cx="50" cy="50" r="46"/>
          <circle class="radar-ring r2" cx="50" cy="50" r="34"/>
          <circle class="radar-ring r3" cx="50" cy="50" r="22"/>
          <circle class="radar-ring r4" cx="50" cy="50" r="11"/>
          <line class="radar-sweep" x1="50" y1="50" x2="50" y2="6"/>
          <circle class="radar-core pulse-core" cx="50" cy="50" r="2.6"/>
        </svg>
        <div class="radar-center-ic"><svg class="i i-lg"><use href="#ic-shield-off" id="radarIconRef"/></svg></div>
      </div>
      <div class="hero-reading">
        <div class="hero-reading-lbl">Live distance</div>
        <div class="hero-reading-val"><span id="heroReading">--</span><span class="unit">cm</span></div>
        <div class="radar-cap" id="radarCap">Perimeter scan idle</div>
      </div>
      <div class="hero-substats">
        <div class="hero-substat">
          <div class="hero-substat-lbl">Trigger at</div>
          <div class="hero-substat-val"><span id="heroTriggerVal">--</span> cm</div>
        </div>
        <div class="hero-substat">
          <div class="hero-substat-lbl">Margin</div>
          <div class="hero-substat-val" id="heroMarginVal">--</div>
        </div>
        <div class="hero-substat">
          <div class="hero-substat-lbl">Signal</div>
          <div class="hero-substat-val" id="heroSignalVal">--</div>
        </div>
      </div>
    </div>
  </div>

  <div class="chart-wrap">
    <div class="chart-hd">
      <span class="chart-title">Distance — last 40 readings</span>
      <span class="chart-legend"><span><span class="sw" style="background:#2563eb"></span>Live</span><span><span class="sw dashed"></span>Trigger line</span></span>
    </div>
    <canvas id="distChart" height="64"></canvas>
  </div>
</div>

<div class="kpi-strip" id="kpiStrip">
  <div class="kpi-tile kpi-calm-blue" id="kpiLiveTile">
    <div class="kpi-ic"><svg class="i i-sm"><use href="#ic-radio"/></svg></div>
    <div class="kpi-lbl">Live distance</div>
    <div class="kpi-val"><span id="kpiLiveVal">--</span><span class="kpi-unit">cm</span></div>
    <div class="kpi-cap" id="kpiLiveCap">Reading the sensor…</div>
  </div>
  <div class="kpi-tile kpi-calm-blue" id="kpiTriggerTile">
    <div class="kpi-ic ic-violet"><svg class="i i-sm"><use href="#ic-ruler"/></svg></div>
    <div class="kpi-lbl">Trigger distance</div>
    <div class="kpi-val"><span id="kpiTriggerVal">--</span><span class="kpi-unit">cm</span></div>
    <div class="kpi-cap" id="kpiTriggerCap">Alarm fires closer than this</div>
  </div>
  <div class="kpi-tile">
    <div class="kpi-ic ic-teal"><svg class="i i-sm"><use href="#ic-shield"/></svg></div>
    <div class="kpi-lbl">Baseline distance</div>
    <div class="kpi-val"><span id="kpiBaselineVal">--</span><span class="kpi-unit">cm</span></div>
    <div class="kpi-cap" id="kpiBaselineCap">From last calibration</div>
  </div>
  <div class="kpi-tile">
    <div class="kpi-ic ic-orange"><svg class="i i-sm"><use href="#ic-activity"/></svg></div>
    <div class="kpi-lbl">Triggers · total / today</div>
    <div class="kpi-val"><span id="kpiTriggersVal">--</span><span class="kpi-unit" id="kpiTriggersToday">· 0 today</span></div>
    <div class="kpi-cap" id="kpiTriggersCap">No detections yet</div>
  </div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-gear"/></svg> Quick controls</h3></div>
  <div class="qc-grid">
    <button class="qc" id="nightBtn" onclick="toggleNight()">
      <span class="qc-state"></span>
      <svg class="i"><use href="#ic-moon"/></svg><span>Night mode</span>
    </button>
    <button class="qc" id="muteBtn" onclick="toggleMute()">
      <span class="qc-state"></span>
      <svg class="i"><use href="#ic-bell"/></svg><span id="muteBtnLabel">Alerts</span>
    </button>
    <button class="qc" id="buzzerMasterBtn" onclick="toggleBuzzerMaster()">
      <span class="qc-state"></span>
      <svg class="i"><use href="#ic-siren"/></svg><span id="buzzerMasterLabel">Siren</span>
    </button>
    <button class="qc" id="oledBtn" onclick="toggleOled()">
      <span class="qc-state"></span>
      <svg class="i"><use href="#ic-monitor"/></svg><span>OLED</span>
    </button>
    <button class="qc" id="testBtn" onclick="testBuzzer()">
      <span class="qc-state"></span>
      <svg class="i"><use href="#ic-volume"/></svg><span>Test siren</span>
    </button>
    <button class="qc qc-estop" id="estopBtn" onclick="toggleEstop()">
      <span class="qc-state"></span>
      <svg class="i"><use href="#ic-stop"/></svg><span id="estopBtnLabel">Emergency stop</span>
    </button>
  </div>
</div>

<div class="card">
  <div class="card-hd">
    <h3><svg class="i"><use href="#ic-activity"/></svg> Recent activity</h3>
    <button class="btn-ghost" onclick="switchTab('activity')">View all</button>
  </div>
  <div class="activity-mini" id="timelineMini"><div class="act-empty"><svg class="i"><use href="#ic-inbox"/></svg>Waiting for events...</div></div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-send"/></svg> Push to Telegram</h3></div>
  <button class="btn" style="width:100%" onclick="showSettings()">
    <svg class="i"><use href="#ic-send"/></svg> Send current system settings
  </button>
</div>

</section>

<!-- =================== ACTIVITY =================== -->
<section class="tab-panel" id="tab-activity">

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-activity"/></svg> Timeline</h3></div>
  <div class="activity-mini" id="timeline"><div class="act-empty"><svg class="i"><use href="#ic-inbox"/></svg>Waiting for events...</div></div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-shield"/></svg> Security</h3></div>
  <div class="data-list" id="securityBox">Loading...</div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-radio"/></svg> Sensor</h3></div>
  <div class="data-list" id="sensorBox">Loading...</div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-wifi"/></svg> Network</h3></div>
  <div class="data-list" id="networkBox">Loading...</div>
  <div class="chart-wrap">
    <div class="chart-hd">
      <span class="chart-title">Wi‑Fi signal — last 40 readings</span>
      <span class="chart-legend"><span><span class="sw" style="background:#059669"></span>RSSI</span></span>
    </div>
    <canvas id="rssiChart" height="64"></canvas>
  </div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-send"/></svg> Telegram</h3></div>
  <div class="data-list" id="telegramBox">Loading...</div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-monitor"/></svg> Device</h3></div>
  <div class="data-list" id="deviceBox">Loading...</div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-siren"/></svg> Buzzer devices</h3></div>
  <div class="data-list" id="buzzerDevicesBox">No buzzers configured — add some in Settings › Devices.</div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-devices"/></svg> Other devices</h3></div>
  <div class="data-list" id="otherDevicesBox">No other devices configured — add some in Settings › Devices.</div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-clock"/></svg> Session statistics</h3></div>
  <div class="data-list" id="statsBox">Loading...</div>
</div>

<div class="card">
  <div class="card-hd"><h3><svg class="i"><use href="#ic-refresh"/></svg> Diagnostics</h3></div>
  <div class="data-list" id="diagnosticsBox">Loading...</div>
</div>

<!-- hidden legacy shells so onclick=toggle keeps parsing -->
<div id="statsBody" class="collapsible-body open" style="display:none"></div>
<div id="diagnosticsBody" class="collapsible-body open" style="display:none"></div>

</section>

<!-- =================== SETTINGS =================== -->
<section class="tab-panel" id="tab-settings">

<div class="settings-tabs" role="tablist">
  <button class="st-btn active" data-st="security" onclick="switchSettingsTab('security')"><svg class="i i-sm"><use href="#ic-shield"/></svg>Security</button>
  <button class="st-btn" data-st="alerts" onclick="switchSettingsTab('alerts')"><svg class="i i-sm"><use href="#ic-bell"/></svg>Alerts</button>
  <button class="st-btn" data-st="devices" onclick="switchSettingsTab('devices')"><svg class="i i-sm"><use href="#ic-devices"/></svg>Devices</button>
  <button class="st-btn" data-st="network" onclick="switchSettingsTab('network')"><svg class="i i-sm"><use href="#ic-wifi"/></svg>Network</button>
  <button class="st-btn" data-st="system" onclick="switchSettingsTab('system')"><svg class="i i-sm"><use href="#ic-gear"/></svg>System</button>
</div>

<div id="dangerBody" class="collapsible-body open">

<!-- SECURITY -->
<div class="st-panel active" id="st-security">
  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-ruler"/></svg> Trigger distance</h3></div>
    <div class="field">
      <input id="trigDist" type="number" step="0.1" placeholder="e.g. 76.4 cm">
    </div>
    <div class="helper">Alarm triggers when the sensor reads closer than this distance.</div>
    <div class="form-actions">
      <button class="btn btn-primary" onclick="saveTrigger()"><svg class="i i-sm"><use href="#ic-save"/></svg> Save trigger</button>
    </div>
  </div>

  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-refresh"/></svg> Calibration</h3></div>
    <div class="helper">Make sure the monitored area is clear, then calibrate the baseline.</div>
    <div class="form-actions">
      <button class="btn btn-primary" id="calBtn" onclick="calibrate()"><svg class="i i-sm" style="color:#fff"><use href="#ic-ruler"/></svg> Calibrate now</button>
    </div>
  </div>

  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-clock"/></svg> Dashboard refresh</h3></div>
    <div class="field">
      <label>Live status interval (seconds)</label>
      <input id="liveSec" type="number" placeholder="Live status interval (sec)">
    </div>
    <div class="form-actions">
      <button class="btn" onclick="setLiveInterval(document.getElementById('liveSec').value)"><svg class="i i-sm"><use href="#ic-save"/></svg> Save live interval</button>
    </div>
    <div class="field">
      <label>Statistics interval (seconds)</label>
      <input id="statsSec" type="number" placeholder="Statistics interval (sec)">
    </div>
    <div class="form-actions">
      <button class="btn" onclick="setStatsInterval(document.getElementById('statsSec').value)"><svg class="i i-sm"><use href="#ic-save"/></svg> Save stats interval</button>
    </div>
  </div>
</div>

<!-- ALERTS -->
<div class="st-panel" id="st-alerts">
  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-send"/></svg> Telegram bot</h3></div>
    <div class="field">
      <label>Bot token</label>
      <input id="botToken" placeholder="Bot Token">
    </div>
    <div class="field pwd-wrap">
      <label>Chat ID</label>
      <input id="chatId" placeholder="Chat ID" type="password">
      <span onclick="togglePwd('chatId','chatToggle')" id="chatToggle" style="top:auto;bottom:11px;transform:none">SHOW</span>
    </div>
    <div class="form-actions">
      <button class="btn btn-primary" onclick="saveTelegram()"><svg class="i i-sm"><use href="#ic-save"/></svg> Save Telegram</button>
      <button class="btn btn-outline-danger" onclick="removeTelegram()"><svg class="i i-sm"><use href="#ic-trash"/></svg> Disconnect</button>
    </div>
  </div>

  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-siren"/></svg> Siren timing</h3></div>
    <div class="field">
      <label>Short-term siren duration (s)</label>
      <input id="shortSec" type="number" placeholder="Short-term buzzer duration (s)">
    </div>
    <div class="field">
      <label>Long-term siren duration (s)</label>
      <input id="longSec" type="number" placeholder="Long-term sustained duration (s)">
    </div>
    <div class="field">
      <label>Sustained activity threshold (s)</label>
      <input id="thresholdSec" type="number" placeholder="Sustained activity threshold (s)">
    </div>
    <div class="field">
      <label>Short-term pattern</label>
      <select id="shortPattern">
        <option value="1">Continuous</option>
        <option value="2">Slow pulse</option>
        <option value="3">Fast pulse</option>
        <option value="4">Double-beep burst</option>
      </select>
    </div>
    <div class="field">
      <label>Long-term pattern</label>
      <select id="longPattern">
        <option value="1">Continuous</option>
        <option value="2">Slow pulse</option>
        <option value="3">Fast pulse</option>
        <option value="4">Double-beep burst</option>
      </select>
    </div>
    <div class="form-actions">
      <button class="btn btn-primary" onclick="saveBuzzerTimes()"><svg class="i i-sm"><use href="#ic-save"/></svg> Save siren timing</button>
    </div>
  </div>
</div>

<!-- DEVICES -->
<div class="st-panel" id="st-devices">
  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-user"/></svg> Device identity</h3></div>
    <div class="field">
      <label>Device name</label>
      <input id="devName" placeholder="Device Name (e.g. Stairs, Balcony)">
    </div>
    <div class="field">
      <label>Device ID</label>
      <input id="devId" placeholder="Device ID (e.g. Stairs System)">
    </div>
    <div class="form-actions">
      <button class="btn btn-primary" onclick="saveDeviceInfo()"><svg class="i i-sm"><use href="#ic-save"/></svg> Save identity</button>
    </div>
  </div>

  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-siren"/></svg> Buzzer units (up to 5)</h3></div>
    <div class="field">
      <textarea id="buzzerIpList" rows="4" placeholder="One IP per line, e.g.:&#10;192.168.1.45&#10;192.168.1.46"></textarea>
    </div>
    <div class="form-actions">
      <button class="btn btn-primary" onclick="saveBuzzerIps()"><svg class="i i-sm"><use href="#ic-save"/></svg> Save buzzer units</button>
    </div>
  </div>

  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-devices"/></svg> Other devices</h3></div>
    <div id="siblingListDisplay" style="margin-bottom:12px;">Loading...</div>
    <div class="field">
      <label>Device IP</label>
      <input id="sibIp" placeholder="192.168.1.92">
    </div>
    <div class="field">
      <label>Username</label>
      <input id="sibUser" placeholder="Username">
    </div>
    <div class="field pwd-wrap">
      <label>Password</label>
      <input id="sibPass" placeholder="Password" type="password">
      <span onclick="togglePwd('sibPass','sibPassToggle')" id="sibPassToggle" style="top:auto;bottom:11px;transform:none">SHOW</span>
    </div>
    <div class="form-actions">
      <button class="btn btn-primary" onclick="addSibling()"><svg class="i i-sm"><use href="#ic-plus"/></svg> Add device</button>
    </div>
  </div>

  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-monitor"/></svg> OLED display</h3></div>
    <div class="helper">Toggle the on-device screen from Overview › Quick controls.</div>
  </div>
</div>

<!-- NETWORK -->
<div class="st-panel" id="st-network">
  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-wifi"/></svg> Wi-Fi network</h3></div>
    <div class="field">
      <label>Network name (SSID)</label>
      <input id="ssid" placeholder="WiFi Network Name (SSID)">
    </div>
    <div class="field pwd-wrap">
      <label>Password</label>
      <input id="password" placeholder="WiFi Password" type="password">
      <span onclick="togglePwd('password','pwdToggle')" id="pwdToggle" style="top:auto;bottom:11px;transform:none">SHOW</span>
    </div>
    <div class="form-actions">
      <button class="btn btn-primary" id="wifiBtn" onclick="saveWifi()"><svg class="i i-sm"><use href="#ic-save"/></svg> Save Wi-Fi & reconnect</button>
    </div>
  </div>

  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-lock"/></svg> Dashboard login</h3></div>
    <div class="field">
      <label>New username</label>
      <input id="dashUser" placeholder="New Username">
    </div>
    <div class="field pwd-wrap">
      <label>New password</label>
      <input id="dashPass" placeholder="New Password" type="password">
      <span onclick="togglePwd('dashPass','dashPassToggle')" id="dashPassToggle" style="top:auto;bottom:11px;transform:none">SHOW</span>
    </div>
    <div class="form-actions">
      <button class="btn btn-primary" onclick="saveAuth()"><svg class="i i-sm"><use href="#ic-save"/></svg> Save login</button>
    </div>
  </div>
</div>

<!-- SYSTEM -->
<div class="st-panel" id="st-system">
  <div class="card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-power"/></svg> Restart</h3></div>
    <div class="helper">Reboot the device. Settings are preserved.</div>
    <div class="form-actions">
      <button class="btn btn-outline-danger" onclick="restartDevice()"><svg class="i i-sm"><use href="#ic-refresh"/></svg> Restart device</button>
    </div>
  </div>

  <div class="card danger-card">
    <div class="card-hd"><h3><svg class="i"><use href="#ic-alert"/></svg> Danger Zone — Factory reset</h3></div>
    <div class="helper" style="color:var(--red)">This erases ALL settings — Wi-Fi, calibration, Telegram, login — and restarts.</div>
    <div class="form-actions">
      <button class="btn btn-danger" onclick="factoryReset()"><svg class="i i-sm" style="color:#fff"><use href="#ic-trash"/></svg> Factory reset</button>
    </div>
  </div>
</div>

</div><!-- /#dangerBody -->
</section>

</div><!-- /.shell -->

<div id="toastBox"></div>

<script>
let lastStatus={};
let distHist=[], rssiHist=[];
let tlLog=[];
let tlSeq=0;
let lastTriggerCount=-1;
let connOnline=true;
let lastUpdateAt=null;
const prefersReducedMotion = window.matchMedia && matchMedia('(prefers-reduced-motion: reduce)').matches;

/* -------- Theme -------- */
function applyTheme(t){
  document.documentElement.setAttribute('data-theme', t);
  const use=document.querySelector('#themeToggle use');
  if(use) use.setAttribute('href', t==='dark' ? '#ic-sun' : '#ic-moon');
}
function toggleTheme(){
  const cur=document.documentElement.getAttribute('data-theme') || (matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light');
  const next = cur==='dark' ? 'light':'dark';
  try{ localStorage.setItem('theme', next); }catch(e){}
  applyTheme(next);
}
(function initTheme(){
  let saved=null;
  try{ saved=localStorage.getItem('theme'); }catch(e){}
  const sys = (window.matchMedia && matchMedia('(prefers-color-scheme:dark)').matches) ? 'dark':'light';
  applyTheme(saved || sys);
})();

/* -------- Tabs -------- */
function switchTab(name){
  document.querySelectorAll('.tab-panel').forEach(p=>p.classList.remove('active'));
  document.getElementById('tab-'+name).classList.add('active');
  document.querySelectorAll('.bn-btn').forEach(b=>b.classList.toggle('active', b.dataset.tab===name));
  window.scrollTo({top:0,behavior:prefersReducedMotion?'auto':'smooth'});
}
function switchSettingsTab(name){
  document.querySelectorAll('.st-btn').forEach(b=>b.classList.toggle('active', b.dataset.st===name));
  document.querySelectorAll('.st-panel').forEach(p=>p.classList.remove('active'));
  document.getElementById('st-'+name).classList.add('active');
}

/* -------- Toast + log -------- */
function toastIcon(type){
  if(type==='success') return '#ic-check';
  if(type==='warn') return '#ic-alert';
  if(type==='error') return '#ic-alert';
  return '#ic-info';
}
function toast(msg,type){
  type = type || 'info';
  const box=document.getElementById('toastBox');
  const t=document.createElement('div');
  t.className='toast t-'+type;
  t.innerHTML='<span class="t-ic"><svg class="i i-sm"><use href="'+toastIcon(type)+'"/></svg></span><span>'+msg+'</span>';
  box.appendChild(t);
  setTimeout(function(){
    t.classList.add('leaving');
    setTimeout(function(){t.remove();}, prefersReducedMotion?0:220);
  },3200);
}
function logEvent(msg,kind){
  const now=new Date();
  const time=now.toTimeString().slice(0,8);
  tlSeq++;
  tlLog.unshift({id:tlSeq,time,msg,kind:kind||'settings',date:now});
  if(tlLog.length>40)tlLog.pop();
  renderTimeline();
}
function markerClass(kind){
  if(kind==='armed') return 'mk-green';
  if(kind==='detection') return 'mk-red';
  if(kind==='paused') return 'mk-amber';
  return 'mk-blue';
}
function isSameDay(a,b){ return a.toDateString()===b.toDateString(); }
function renderTimelineInto(el, list, newestId){
  if(!el) return;
  if(list.length===0){
    el.innerHTML='<div class="act-empty"><svg class="i"><use href="#ic-inbox"/></svg>Waiting for events...</div>';
    return;
  }
  const today=new Date();
  let html='', lastGroup=null;
  list.forEach(function(e){
    const grp = isSameDay(e.date, today) ? 'Today' : 'Earlier';
    if(grp!==lastGroup){ html+='<div class="act-group-lbl">'+grp+'</div>'; lastGroup=grp; }
    const cls = (e.id===newestId) ? ' act-new' : '';
    html += '<div class="act-item'+cls+'"><span class="act-mk '+markerClass(e.kind)+'"></span><span class="act-time">'+e.time+'</span><span>'+e.msg+'</span></div>';
  });
  el.innerHTML=html;
}
function renderTimeline(){
  const newestId = tlLog.length ? tlLog[0].id : -1;
  renderTimelineInto(document.getElementById('timeline'), tlLog, newestId);
  renderTimelineInto(document.getElementById('timelineMini'), tlLog.slice(0,4), newestId);
}

/* -------- Sparkline (labelled, threshold line, glowing endpoint) -------- */
function drawSpark(id,arr,color,thresholdVal){
  const c=document.getElementById(id); if(!c)return;
  const ctx=c.getContext('2d'); const w=c.width=c.clientWidth; const h=c.height;
  ctx.clearRect(0,0,w,h);
  if(arr.length<2)return;
  const withThresh = arr.concat(thresholdVal!=null?[thresholdVal]:[]);
  const max=Math.max.apply(null,withThresh.concat([1])), min=Math.min.apply(null,withThresh.concat([0]));
  const pad=6;
  const yOf=function(v){ return h-((v-min)/(max-min+.001))*(h-pad*2)-pad; };

  const g=ctx.createLinearGradient(0,0,0,h);
  g.addColorStop(0,color); g.addColorStop(1,'rgba(0,0,0,0)');
  ctx.beginPath();
  arr.forEach(function(v,i){
    const x=i/(arr.length-1)*w; const y=yOf(v);
    i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);
  });
  ctx.lineTo(w,h); ctx.lineTo(0,h); ctx.closePath();
  ctx.fillStyle=g; ctx.globalAlpha=.16; ctx.fill(); ctx.globalAlpha=1;

  if(thresholdVal!=null){
    const ty=yOf(thresholdVal);
    ctx.save();
    ctx.setLineDash([4,4]);
    ctx.strokeStyle='rgba(220,38,38,.55)';
    ctx.lineWidth=1.4;
    ctx.beginPath(); ctx.moveTo(0,ty); ctx.lineTo(w,ty); ctx.stroke();
    ctx.restore();
  }

  ctx.beginPath(); ctx.strokeStyle=color; ctx.lineWidth=2;
  let lastX=0,lastY=0;
  arr.forEach(function(v,i){
    const x=i/(arr.length-1)*w; const y=yOf(v);
    i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);
    lastX=x; lastY=y;
  });
  ctx.stroke();

  /* glowing endpoint */
  ctx.save();
  ctx.shadowColor=color; ctx.shadowBlur=8;
  ctx.beginPath(); ctx.fillStyle=color; ctx.arc(lastX,lastY,3.4,0,Math.PI*2); ctx.fill();
  ctx.restore();
}
function fmtSec(s){
  s=Math.floor(s);
  const h=Math.floor(s/3600), m=Math.floor((s%3600)/60), sec=s%60;
  if(h>0)return h+'h '+m+'m';
  if(m>0)return m+'m '+sec+'s';
  return sec+'s';
}
function row(icon,label,value,cls){
  return '<div class="row"><span class="k"><span class="ic">'+icon+'</span>'+label+'</span><span class="v'+(cls?' '+cls:'')+'">'+value+'</span></div>';
}
function buzzerConnLabel(d){
  const ips=d.buzzerIps||[]; if(ips.length===0) return 'No buzzers configured';
  const reach=d.buzzerReachableList||[];
  return reach.filter(function(x){return x;}).length+'/'+ips.length+' online';
}
function buzzerConnClass(d){
  const ips=d.buzzerIps||[]; if(ips.length===0) return 'warn';
  const reach=d.buzzerReachableList||[];
  const ok=reach.filter(function(x){return x;}).length;
  if(ok===0) return 'bad'; if(ok<ips.length) return 'warn'; return 'good';
}

/* -------- Animated number set -------- */
function setNumber(el, text){
  if(!el) return;
  if(el.textContent===text) return;
  el.textContent=text;
  if(prefersReducedMotion) return;
  el.classList.remove('num-bump');
  void el.offsetWidth;
  el.classList.add('num-bump');
}

function setRadarCap(text){
  const el=document.getElementById('radarCap'); if(el) el.textContent=text;
}

/* -------- Primary state hero + radar -------- */
function updateHero(d){
  const hero=document.getElementById('heroCard');
  const stTitle=document.getElementById('stateTitle');
  const stMsg=document.getElementById('stateMsg');
  const stIconUse=document.querySelector('#stateIc use');
  const stateIcEl=document.getElementById('stateIc');
  const armBtn=document.getElementById('armBtn');
  const disarmBtn=document.getElementById('disarmBtn');
  const secBtn=document.getElementById('secondaryActionBtn');
  const navSet=document.querySelector('.bn-btn[data-tab="overview"]');
  const radar=document.getElementById('radarWrap');
  const radarIconUse=document.querySelector('#radarWrap use');
  const chipTxt=document.getElementById('heroChipTxt');
  const detail=document.getElementById('stateDetail');

  const prevIconHref = stIconUse ? stIconUse.getAttribute('href') : null;

  hero.classList.remove('st-safe','st-alert','st-off');
  radar.classList.remove('st-safe','st-armed','st-alert','st-off');
  if(navSet) navSet.classList.remove('alert');

  let newHref='';
  if(d.alarmActive){
    hero.classList.add('st-alert');
    radar.classList.add('st-alert');
    stTitle.textContent='Intrusion Detected';
    stMsg.textContent='Sensor triggered. Check the area or disarm the system.';
    newHref='#ic-alert';
    armBtn.style.display='none'; disarmBtn.style.display='inline-flex';
    secBtn.textContent='Silence & investigate';
    if(navSet) navSet.classList.add('alert');
    if(chipTxt) chipTxt.textContent='Live alert';
    if(detail) detail.innerHTML='Reading <b>'+(d.distanceValid?d.distanceCm.toFixed(1)+' cm':'--')+'</b> · well inside the <b>'+(d.calibrated?d.triggerDistanceCm.toFixed(1)+' cm':'--')+'</b> trigger line';
    setRadarCap('Motion in trigger zone — fast sweep');
  } else if(d.armed){
    hero.classList.add('st-safe');
    radar.classList.add('st-armed');
    stTitle.textContent='Protected';
    stMsg.textContent='System is armed. Monitoring perimeter.';
    newHref='#ic-shield-check';
    armBtn.style.display='none'; disarmBtn.style.display='inline-flex';
    secBtn.textContent='Disarm system';
    if(chipTxt) chipTxt.textContent='Monitoring live';
    if(detail) detail.innerHTML='Armed for <b>'+fmtSec(d.protectionSec||0)+'</b> · <b>'+(d.todayIntrusions||0)+'</b> events today · sensor <b>'+(d.sensorHealth||'—')+'</b>';
    setRadarCap('Armed & clear — soft pulse sweep');
  } else {
    hero.classList.add('st-off');
    radar.classList.add('st-off');
    stTitle.textContent='Disarmed';
    stMsg.textContent='Home security is off. Arm to start monitoring.';
    newHref='#ic-shield-off';
    armBtn.style.display='inline-flex'; disarmBtn.style.display='none';
    secBtn.textContent='Run buzzer test';
    if(chipTxt) chipTxt.textContent='Standing by';
    if(detail) detail.innerHTML=(d.lastTriggerSecAgo>=0?'Last activity <b>'+fmtSec(d.lastTriggerSecAgo)+'</b> ago':'No activity recorded yet')+' · <b>'+(d.uptimeSec?fmtSec(d.uptimeSec):'0s')+'</b> device uptime';
    setRadarCap('Disarmed — scan inactive');
  }
  stIconUse.setAttribute('href', newHref);
  if(radarIconUse) radarIconUse.setAttribute('href', newHref);
  if(prevIconHref!==null && prevIconHref!==newHref && !prefersReducedMotion){
    stateIcEl.classList.remove('icon-pulse');
    void stateIcEl.offsetWidth;
    stateIcEl.classList.add('icon-pulse');
  }
}
function secondaryAction(){
  if(!lastStatus) return;
  if(lastStatus.alarmActive || lastStatus.armed) post('/disarm','System disarmed','paused','warn');
  else testBuzzer();
}

/* -------- Refresh loops -------- */
function updateConnBadgeAndTime(){
  const hb=document.getElementById('hBadges');
  if(!hb) return;
  /* connection + last-updated are prepended by refreshLive's badge builder */
}
function tickLastUpdated(){
  const el=document.getElementById('lastUpdatedTxt'); if(!el) return;
  if(!lastUpdateAt){ el.textContent='Waiting for data...'; return; }
  const secs=Math.floor((Date.now()-lastUpdateAt.getTime())/1000);
  if(!connOnline){ el.textContent='Connection lost — last update '+fmtSec(secs)+' ago'; return; }
  if(secs<5) el.textContent='Updated just now';
  else if(secs<60) el.textContent='Updated '+secs+'s ago';
  else el.textContent='Updated '+fmtSec(secs)+' ago';
}
setInterval(tickLastUpdated, 1000);

async function refreshLive(){
  try{
    const r=await fetch('/status'); const d=await r.json(); lastStatus=d;
    connOnline=true; lastUpdateAt=new Date();

    const devLabel=(d.deviceName||'')+(d.deviceId?(' ('+d.deviceId+')'):'');
    document.getElementById('fwInfo').innerText=(devLabel?devLabel+' · ':'')+'v'+d.firmwareVersion;
    document.title=devLabel||'Echo Smart Gaurd';
    document.getElementById('devName').placeholder='Device Name (current: '+(d.deviceName||'—')+')';
    document.getElementById('devId').placeholder='Device ID (current: '+(d.deviceId||'—')+')';
    if(document.getElementById('buzzerIpList').dataset.set!=='1'){
      document.getElementById('buzzerIpList').value=(d.buzzerIps||[]).join('\n');
      document.getElementById('buzzerIpList').dataset.set='1';
    }

    let hb='';
    hb+='<span class="hbadge '+(connOnline?'g-safe':'g-crit')+'"><span class="dot"></span>'+(connOnline?'Online':'Offline')+'</span>';
    if(d.armed) hb+='<span class="hbadge g-safe"><span class="dot"></span>'+(d.armedByNightMode?'Armed (Night Mode)':'Armed')+'</span>';
    else hb+='<span class="hbadge g-off"><span class="dot"></span>Disarmed</span>';
    if(d.muted && !d.buzzerMasterEnabled) hb+='<span class="hbadge g-crit"><span class="dot"></span>Fully muted</span>';
    else if(d.muted) hb+='<span class="hbadge g-crit"><span class="dot"></span>Alerts paused</span>';
    if(d.estopActive) hb+='<span class="hbadge g-crit"><span class="dot"></span>E-Stop</span>';
    document.getElementById('hBadges').innerHTML=hb;

    const distText = d.distanceValid?d.distanceCm.toFixed(1):'--';
    const trigText = d.calibrated?d.triggerDistanceCm.toFixed(1):'--';
    const baseText = d.calibrated?d.wallDistanceCm.toFixed(1):'--';

    setNumber(document.getElementById('distance'), distText);
    setNumber(document.getElementById('heroReading'), distText);
    setNumber(document.getElementById('kpiLiveVal'), distText);
    setNumber(document.getElementById('triggerDist'), trigText);
    setNumber(document.getElementById('kpiTriggerVal'), trigText);
    setNumber(document.getElementById('kpiBaselineVal'), baseText);
    setNumber(document.getElementById('kpiTriggersVal'), String(d.triggerCount));
    const todayEl=document.getElementById('kpiTriggersToday'); if(todayEl) todayEl.textContent='· '+d.todayIntrusions+' today';
    document.getElementById('triggerCount') && (document.getElementById('triggerCount').innerText = d.triggerCount);

    const inZone=d.calibrated&&d.distanceValid&&d.distanceCm<d.triggerDistanceCm;
    document.getElementById('zoneIcon').innerText = inZone?'alert':'shield';

    /* Richer caption lines under each KPI tile — computed from data already in /status */
    const liveCap=document.getElementById('kpiLiveCap');
    if(liveCap) liveCap.textContent = !d.distanceValid ? 'No sensor reading yet'
      : (d.alarmActive ? 'Inside the trigger zone right now' : (inZone ? 'Approaching the trigger line' : 'Clear — outside the trigger zone'));
    const trigCap=document.getElementById('kpiTriggerCap');
    if(trigCap) trigCap.textContent = d.calibrated ? (d.distanceValid ? Math.abs(d.distanceCm-d.triggerDistanceCm).toFixed(1)+' cm margin right now' : 'Alarm fires closer than this') : 'Not calibrated yet';
    const baseCap=document.getElementById('kpiBaselineCap');
    if(baseCap) baseCap.textContent = d.lastCalibrationEpoch>100000 ? 'Calibrated '+fmtEpoch(d.lastCalibrationEpoch) : 'Run calibration in Settings';
    const trigsCap=document.getElementById('kpiTriggersCap');
    if(trigsCap) trigsCap.textContent = d.lastTriggerSecAgo>=0 ? 'Last detection '+fmtSec(d.lastTriggerSecAgo)+' ago' : 'No detections yet';

    /* Hero-right sub-stats row — fills the space next to the radar with data that's already on screen elsewhere, just handy here too */
    setNumber(document.getElementById('heroTriggerVal'), trigText);
    const marginEl=document.getElementById('heroMarginVal');
    if(marginEl) marginEl.textContent = (d.calibrated && d.distanceValid) ? Math.abs(d.distanceCm-d.triggerDistanceCm).toFixed(1)+' cm' : '--';
    const sigEl=document.getElementById('heroSignalVal');
    if(sigEl){
      if(d.wifiMode==='Station'){
        const pct=Math.max(0,Math.min(100,(d.rssi+100)*2));
        sigEl.textContent = (pct>60?'Strong':(pct>30?'Fair':'Weak'));
      } else {
        sigEl.textContent='AP mode';
      }
    }

    /* KPI tile calm/warn/alert coloring: trigger state = amber/red, normal = calm blue (armed) / green (safe/disarmed) */
    const liveTile=document.getElementById('kpiLiveTile'), trigTile=document.getElementById('kpiTriggerTile');
    [liveTile,trigTile].forEach(function(tile){
      if(!tile) return;
      tile.classList.remove('kpi-calm-blue','kpi-calm-green','kpi-warn','kpi-alert');
      if(d.alarmActive) tile.classList.add('kpi-alert');
      else if(inZone) tile.classList.add('kpi-warn');
      else if(d.armed) tile.classList.add('kpi-calm-blue');
      else tile.classList.add('kpi-calm-green');
    });

    updateHero(d);

    if(d.triggerCount!=lastTriggerCount){
      if(lastTriggerCount>=0) logEvent('Motion detected — #'+d.triggerCount,'detection');
      lastTriggerCount=d.triggerCount;
    }
    if(d.distanceValid){distHist.push(d.distanceCm); if(distHist.length>40)distHist.shift();}
    drawSpark('distChart',distHist,'#2563eb', d.calibrated?d.triggerDistanceCm:null);
  }catch(e){
    connOnline=false;
    const hbEl=document.getElementById('hBadges');
    if(hbEl && hbEl.innerHTML.indexOf('Offline')<0){
      hbEl.innerHTML='<span class="hbadge g-crit"><span class="dot"></span>Offline</span>'+hbEl.innerHTML;
    }
  }
}

async function refreshStats(){
  try{
    const r=await fetch('/status'); const d=await r.json(); lastStatus=d;

    /* Quick controls state */
    const nb=document.getElementById('nightBtn'); nb.classList.toggle('on', !!d.nightMode);
    const mb=document.getElementById('muteBtn'); mb.classList.toggle('warn', !!d.muted);
    document.querySelector('#muteBtn use').setAttribute('href', d.muted?'#ic-bell-off':'#ic-bell');
    document.getElementById('muteBtnLabel').textContent = d.muted?'Paused':'Alerts';

    const eb=document.getElementById('estopBtn'); eb.classList.toggle('warn', !!d.estopActive);
    document.querySelector('#estopBtn use').setAttribute('href', d.estopActive?'#ic-play':'#ic-stop');
    document.getElementById('estopBtnLabel').textContent = d.estopActive?'Resume':'Emergency stop';

    const ob=document.getElementById('oledBtn'); ob.classList.toggle('on', !!d.oledOn);

    const bmb=document.getElementById('buzzerMasterBtn'); bmb.classList.toggle('warn', !d.buzzerMasterEnabled);
    document.querySelector('#buzzerMasterBtn use').setAttribute('href', d.buzzerMasterEnabled?'#ic-siren':'#ic-siren-off');
    document.getElementById('buzzerMasterLabel').textContent = d.buzzerMasterEnabled?'Siren on':'Siren off';

    document.getElementById('shortSec').placeholder='Short-term (current: '+d.shortBuzzerSec+'s)';
    document.getElementById('longSec').placeholder='Long-term (current: '+d.longBuzzerSec+'s)';
    document.getElementById('thresholdSec').placeholder='Threshold (current: '+d.sustainedThresholdSec+'s)';
    if(document.getElementById('shortPattern').dataset.set!=='1'){
      document.getElementById('shortPattern').value=d.shortBuzzerPattern||1;
      document.getElementById('shortPattern').dataset.set='1';
    }
    if(document.getElementById('longPattern').dataset.set!=='1'){
      document.getElementById('longPattern').value=d.longBuzzerPattern||2;
      document.getElementById('longPattern').dataset.set='1';
    }

    document.getElementById('securityBox').innerHTML =
      row('','Calibration', d.calibrated?'Calibrated':'Not calibrated', d.calibrated?'good':'warn') +
      row('','Last calibrated', fmtEpoch(d.lastCalibrationEpoch)) +
      row('','Last detection', d.lastTriggerSecAgo>=0?fmtSec(d.lastTriggerSecAgo)+' ago':'Never') +
      row('','Total intrusions', d.triggerCount) +
      row('','Today', d.todayIntrusions) +
      row('','Since last', d.lastTriggerSecAgo>=0?fmtSec(d.lastTriggerSecAgo):'—');

    document.getElementById('sensorBox').innerHTML =
      row('','Sensor health', d.sensorHealth+(d.sensorFrozen?' (possibly frozen)':''), d.sensorFrozen?'bad':(d.sensorHealth==='Good'?'good':'warn')) +
      row('','Min distance today', d.minDistToday>=0?d.minDistToday.toFixed(1)+' cm':'—') +
      row('','Max distance today', d.maxDistToday>=0?d.maxDistToday.toFixed(1)+' cm':'—');

    const rssiPct = d.wifiMode==='Station'?Math.max(0,Math.min(100,(d.rssi+100)*2)):0;
    const rssiCls = rssiPct>60?'good':(rssiPct>30?'warn':'bad');
    document.getElementById('networkBox').innerHTML =
      row('','SSID', d.ssid) +
      row('','Signal', rssiPct+'% · '+d.rssi+' dBm', rssiCls) +
      row('','IP address', d.ipAddress);
    if(d.wifiMode==='Station'){rssiHist.push(d.rssi); if(rssiHist.length>40)rssiHist.shift();}
    drawSpark('rssiChart',rssiHist,'#059669', null);

    document.getElementById('telegramBox').innerHTML =
      row('','Bot status', d.botConfigured?'Connected':'Not configured', d.botConfigured?'good':'warn') +
      row('','Last alert', d.lastAlertSecAgo>=0?fmtSec(d.lastAlertSecAgo)+' ago':'Never') +
      row('','Last alert sent at', fmtEpoch(d.lastAlertEpoch)) +
      row('','Total sent', d.totalAlertsSent);

    document.getElementById('statsBox').innerHTML =
      row('','Today alerts', d.todayAlerts) +
      row('','Total protection time', fmtSec(d.protectionSec)) +
      row('','Longest armed duration', fmtSec(d.longestArmedSec)) +
      row('','Total pause time', fmtSec(d.pauseSec)) +
      row('','Total E-Stop time', fmtSec(d.estopTotalSec)) +
      row('','Uptime', fmtSec(d.uptimeSec));

    document.getElementById('deviceBox').innerHTML =
      row('','OLED display', d.oledOn?'On':'Off', d.oledOn?'good':'warn') +
      row('','Buzzer hardware', d.buzzerMasterEnabled?'Enabled':'Disabled', d.buzzerMasterEnabled?'good':'bad') +
      row('','Buzzer units', buzzerConnLabel(d), buzzerConnClass(d));

    document.getElementById('trigDist').placeholder = d.calibrated?d.triggerDistanceCm.toFixed(1):'Not calibrated';
  }catch(e){}
}

function refresh(){ refreshLive(); refreshStats(); }
async function post(path,msg,kind,type){ await fetch(path,{method:'POST'}); if(msg){toast(msg,type||'info');logEvent(msg,kind||'settings');} refresh(); }

async function testBuzzer(){
  toast('Buzzer sounding for 3 seconds','info'); logEvent('Buzzer test started','settings');
  const btn=document.getElementById('testBtn'); btn.disabled=true;
  await fetch('/test',{method:'POST'});
  btn.disabled=false; refresh();
}
async function toggleNight(){
  const on=!lastStatus.nightMode;
  await fetch(on?'/nightmode/on':'/nightmode/off',{method:'POST'});
  toast('Night mode '+(on?'enabled':'disabled'), on?'success':'info'); logEvent('Night mode '+(on?'ON':'OFF'),'settings');
  refresh();
}
async function toggleMute(){
  const willPause=!lastStatus.muted;
  await fetch(willPause?'/mute':'/unmute',{method:'POST'});
  toast(willPause?'Telegram alerts paused':'Telegram alerts resumed', willPause?'warn':'success'); logEvent('Telegram alerts '+(willPause?'paused':'resumed'), willPause?'paused':'settings');
  refresh();
}
async function toggleEstop(){
  const willActivate=!lastStatus.estopActive;
  if(willActivate && !confirm('Activate Emergency Stop? This silences the siren and pauses monitoring until you resume it.')) return;
  await fetch('/estop/toggle',{method:'POST'});
  toast(willActivate?'Emergency stop activated':'Monitoring resumed', willActivate?'warn':'success'); logEvent('Emergency stop '+(willActivate?'activated':'disabled'), willActivate?'paused':'settings');
  refresh();
}
async function toggleOled(){
  const willTurnOn=!lastStatus.oledOn;
  await fetch(willTurnOn?'/oled/on':'/oled/off',{method:'POST'});
  toast('OLED display '+(willTurnOn?'on':'off'),'info'); logEvent('OLED '+(willTurnOn?'ON':'OFF'),'settings');
  refresh();
}
async function toggleBuzzerMaster(){
  const willEnable=!lastStatus.buzzerMasterEnabled;
  await fetch(willEnable?'/buzzer/on':'/buzzer/off',{method:'POST'});
  toast('Siren hardware '+(willEnable?'on':'off'), willEnable?'success':'warn'); logEvent('Siren hardware '+(willEnable?'ON':'OFF'),'settings');
  refresh();
}
async function calibrate(){
  if(!confirm('Make sure the monitored area is empty. Continue?'))return;
  const btn=document.getElementById('calBtn'); btn.disabled=true; const orig=btn.innerHTML; btn.innerHTML='Calibrating...';
  await fetch('/calibrate',{method:'POST'});
  btn.disabled=false; btn.innerHTML=orig;
  toast('Calibration complete','success'); logEvent('Calibration completed','settings');
  refresh();
}
async function restartDevice(){
  if(!confirm('Restart the device now?'))return;
  toast('Restarting device...','info');
  await fetch('/restart',{method:'POST'});
}
async function saveAuth(){
  const u=document.getElementById('dashUser').value, p=document.getElementById('dashPass').value;
  if(!u||!p){toast('Enter both username and password','warn');return;}
  const body='username='+encodeURIComponent(u)+'&password='+encodeURIComponent(p);
  await fetch('/setauth',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  toast('Login updated — you may need to re-enter credentials','success'); logEvent('Dashboard login changed','settings');
}
async function factoryReset(){
  if(!confirm('This ERASES all settings (Wi-Fi, calibration, Telegram, login) and restarts. Are you sure?'))return;
  if(!confirm('Really sure? This cannot be undone.'))return;
  toast('Factory reset in progress...','warn');
  await fetch('/factoryreset',{method:'POST'});
}
async function showSettings(){
  const r=await fetch('/systemsettings'); await r.text();
  toast('Settings sent to dashboard & Telegram','success');
}
function togglePwd(inputId,toggleId){
  const p=document.getElementById(inputId); const t=document.getElementById(toggleId);
  if(p.type==='password'){p.type='text';t.innerText='HIDE';}else{p.type='password';t.innerText='SHOW';}
}
async function saveWifi(){
  const ssid=document.getElementById('ssid').value;
  const password=document.getElementById('password').value;
  if(!ssid){toast('Enter a network name','warn');return;}
  const btn=document.getElementById('wifiBtn'); btn.disabled=true; const o=btn.innerHTML; btn.innerHTML='Connecting...';
  const body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(password);
  await fetch('/setwifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  toast('Saved — device restarting','success');
}
async function saveTelegram(){
  const token=document.getElementById('botToken').value;
  const chatid=document.getElementById('chatId').value;
  if(!token||!chatid){toast('Enter both token and chat ID','warn');return;}
  const body='token='+encodeURIComponent(token)+'&chatid='+encodeURIComponent(chatid);
  await fetch('/settelegram',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  toast('Telegram config saved','success'); logEvent('Telegram config updated','settings');
}
async function removeTelegram(){
  if(!confirm('Disconnect Telegram? Saved bot token and chat ID will be permanently removed.'))return;
  await fetch('/removetelegram',{method:'POST'});
  toast('Telegram disconnected','warn'); logEvent('Telegram bot disconnected','settings');
  document.getElementById('botToken').value=''; document.getElementById('chatId').value='';
  refresh();
}
async function saveTrigger(){
  const val=document.getElementById('trigDist').value;
  if(!val||isNaN(val)){toast('Enter a valid number','warn');return;}
  await fetch('/settrigger?value='+encodeURIComponent(val),{method:'POST'});
  toast('Trigger distance updated to '+val+' cm','success'); logEvent('Trigger distance set to '+val+' cm','settings');
  refresh();
}
async function saveDeviceInfo(){
  const name=document.getElementById('devName').value;
  const id=document.getElementById('devId').value;
  if(!name||!id){toast('Enter both name and ID','warn');return;}
  const body='name='+encodeURIComponent(name)+'&id='+encodeURIComponent(id);
  await fetch('/setdeviceinfo',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  toast('Device identity updated','success'); logEvent('Device identity set to '+name+' / '+id,'settings');
  refresh();
}
async function saveBuzzerIps(){
  const lines=document.getElementById('buzzerIpList').value.split('\n').map(function(s){return s.trim();}).filter(Boolean).slice(0,5);
  const list=lines.join(';');
  await fetch('/setbuzzerips',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'list='+encodeURIComponent(list)});
  toast('Buzzer units updated','success'); logEvent('Buzzer unit list updated ('+lines.length+' device(s))','settings');
  refresh();
}
function getSiblingEntries(){
  const raw=lastStatus.siblingDevices||'';
  return raw.split(';').map(function(s){return s.trim();}).filter(Boolean).map(function(e){
    const p=e.split(','); return {ip:p[0]||'', user:p[1]||'', pass:p[2]||''};
  });
}
function saveSiblingEntries(entries){
  const list=entries.map(function(e){return e.ip+','+e.user+','+e.pass;}).join(';');
  return fetch('/setsiblings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'list='+encodeURIComponent(list)});
}
async function addSibling(){
  const ip=document.getElementById('sibIp').value.trim();
  const user=document.getElementById('sibUser').value;
  const pass=document.getElementById('sibPass').value;
  if(!ip||!user){toast('Enter IP and username','warn');return;}
  const entries=getSiblingEntries();
  if(entries.some(function(e){return e.ip===ip;})){toast('That IP is already added','warn');return;}
  entries.push({ip:ip,user:user,pass:pass});
  await saveSiblingEntries(entries);
  toast('Device added','success'); logEvent('Other device added: '+ip,'settings');
  document.getElementById('sibIp').value=''; document.getElementById('sibUser').value=''; document.getElementById('sibPass').value='';
  refresh();
}
async function removeSibling(ip){
  if(!confirm('Remove device '+ip+'?'))return;
  const entries=getSiblingEntries().filter(function(e){return e.ip!==ip;});
  await saveSiblingEntries(entries);
  toast('Device removed','warn'); logEvent('Other device removed: '+ip,'settings');
  refresh();
}
async function refreshSiblings(){
  const entries=getSiblingEntries();
  document.getElementById('siblingListDisplay').innerHTML = entries.length===0
    ? '<div class="row"><span>No other devices added yet</span></div>'
    : entries.map(function(e){return '<div class="row"><span>'+e.ip+'</span><span><button class="btn btn-outline-danger" style="padding:5px 10px;font-size:11px;" onclick="removeSibling(\''+e.ip+'\')">Remove</button></span></div>';}).join('');

  if(entries.length===0){
    document.getElementById('otherDevicesBox').innerHTML='<div class="row"><span class="k">No other devices configured</span></div>';
    return;
  }
  let html='';
  for(const e of entries){
    const ip=e.ip, user=e.user, pass=e.pass;
    try{
      const controller=new AbortController();
      const t=setTimeout(function(){controller.abort();},3000);
      const r=await fetch('http://'+ip+'/status',{headers:{'Authorization':'Basic '+btoa(user+':'+pass)},signal:controller.signal});
      clearTimeout(t);
      if(r.ok){
        const d=await r.json();
        const statusText=(d.alarmActive?'ALARM · ':'')+(d.armed?(d.armedByNightMode?'Armed (Night Mode)':'Armed'):'Disarmed')+' · '+(d.distanceValid?d.distanceCm.toFixed(1)+'cm':'no reading');
        const cls=d.alarmActive?'bad':(d.armed?'good':'warn');
        html += '<div class="sensor-block">'+
          '<div class="row"><span>Name</span><span class="v">'+(d.deviceName||'Unknown')+'</span></div>'+
          '<div class="row"><span>ID</span><span class="v">'+(d.deviceId||'')+'</span></div>'+
          '<div class="row"><span>IP</span><span class="v">'+ip+'</span></div>'+
          '<div class="row"><span>Status</span><span class="v '+cls+'">'+statusText+'</span></div>'+
          '</div>';
      } else {
        html += '<div class="sensor-block"><div class="row"><span>'+ip+'</span><span class="v bad">Auth failed / error</span></div></div>';
      }
    }catch(err){
      html += '<div class="sensor-block"><div class="row"><span>'+ip+'</span><span class="v bad">Offline / unreachable</span></div></div>';
    }
  }
  document.getElementById('otherDevicesBox').innerHTML=html;
}
function fmtEpoch(e){
  if(!e || e<100000) return '—';
  const d=new Date(e*1000);
  return d.toLocaleString();
}
function fmtDurSec(s){
  if(s<0) s=0;
  const h=Math.floor(s/3600), m=Math.floor((s%3600)/60);
  if(h>0) return h+'h '+m+'m';
  return m+'m';
}
async function refreshBuzzerDevices(){
  const ips=lastStatus.buzzerIps||[];
  if(ips.length===0){
    document.getElementById('buzzerDevicesBox').innerHTML='<div class="row"><span class="k">No buzzers configured</span></div>';
    return;
  }
  let html='';
  for(let idx=0; idx<ips.length; idx++){
    const ip=ips[idx];
    let label='Unknown buzzer', statusText='Checking...', cls='';
    try{
      const controller=new AbortController();
      const t=setTimeout(function(){controller.abort();},3000);
      const r=await fetch('http://'+ip+'/info',{signal:controller.signal});
      clearTimeout(t);
      if(r.ok){
        const d=await r.json();
        label=(d.name||'Buzzer')+' ('+(d.id||ip)+')';
        statusText=(d.buzzActive?'Sounding':'Idle')+' · Last: '+(d.lastBuzzSecAgo>=0?fmtDurSec(d.lastBuzzSecAgo)+' ago':'Never');
        cls=d.buzzActive?'bad':'good';
      } else { label=ip; statusText='Unreachable'; cls='bad'; }
    }catch(err){ label=ip; statusText='Offline / unreachable'; cls='bad'; }

    html += '<div class="row"><span class="k">'+label+'</span>'+
            '<span style="display:flex;align-items:center;gap:8px;">'+
            '<span class="v'+(cls?' '+cls:'')+'">'+statusText+'</span>'+
            '<button class="btn" style="padding:5px 10px;font-size:11px;" onclick="testOneBuzzer(\''+ip+'\')">Test</button>'+
            '<button class="btn btn-outline-danger" style="padding:5px 10px;font-size:11px;" onclick="removeBuzzer(\''+ip+'\')">Remove</button>'+
            '</span></div>';

    const since = (lastStatus.buzzerConnectedSince||[])[idx];
    const h1 = (lastStatus.buzzerHistory1||[])[idx];
    const h2 = (lastStatus.buzzerHistory2||[])[idx];
    let histHtml = '';
    if(since>0) histHtml += row('','Connected since', fmtEpoch(since), 'good');
    if(h1 && h1.c>0) histHtml += row('','Last session', fmtEpoch(h1.c)+' → '+fmtEpoch(h1.d)+' ('+fmtDurSec(h1.d-h1.c)+')');
    if(h2 && h2.c>0) histHtml += row('','Previous session', fmtEpoch(h2.c)+' → '+fmtEpoch(h2.d)+' ('+fmtDurSec(h2.d-h2.c)+')');
    if(histHtml) html += '<div style="padding-left:14px;">'+histHtml+'</div>';
  }
  document.getElementById('buzzerDevicesBox').innerHTML=html;
}
async function testOneBuzzer(ip){
  toast('Testing buzzer at '+ip,'info');
  try{ await fetch('http://'+ip+'/test?duration=3000'); }catch(e){ toast('Could not reach '+ip,'error'); }
}
async function removeBuzzer(ip){
  if(!confirm('Remove buzzer '+ip+'? This permanently deletes it from the saved list.'))return;
  await fetch('/removebuzzerip',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ip='+encodeURIComponent(ip)});
  toast('Buzzer removed','warn'); logEvent('Buzzer removed: '+ip,'settings');
  refresh();
}
function refreshDiagnostics(){
  const d=lastStatus;
  let html = row('','System on time', fmtSec(d.uptimeSec||0));
  if(!d.sessionStartEpoch || d.sessionStartEpoch<=0){
    html += '<div class="row"><span class="k">Time not synced yet — session times unavailable</span></div>';
    document.getElementById('diagnosticsBox').innerHTML = html;
    return;
  }
  html += row('','This session started', fmtEpoch(d.sessionStartEpoch)) +
          row('','Last shutdown', fmtEpoch(d.lastShutdownEpoch));
  const starts=d.historyStart||[], ends=d.historyEnd||[];
  if(starts.length===0){
    html += '<div class="row"><span class="k">No session history yet</span></div>';
  } else {
    for(let i=0;i<starts.length;i++){
      const dur = ends[i]>=starts[i] ? fmtDurSec(ends[i]-starts[i]) : '—';
      html += row('','Session '+(i+1), fmtEpoch(starts[i])+' → '+fmtEpoch(ends[i])+' ('+dur+')');
    }
  }
  document.getElementById('diagnosticsBox').innerHTML = html;
}
async function saveBuzzerTimes(){
  const s=document.getElementById('shortSec').value, l=document.getElementById('longSec').value, t=document.getElementById('thresholdSec').value;
  const sp=document.getElementById('shortPattern').value, lp=document.getElementById('longPattern').value;
  if(!s||!l||!t){toast('Fill all three fields','warn');return;}
  const body='short='+s+'&long='+l+'&threshold='+t+'&shortPattern='+sp+'&longPattern='+lp;
  await fetch('/setbuzzertimes',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
  toast('Siren timing updated','success'); logEvent('Siren timing updated (short:'+s+'s long:'+l+'s threshold:'+t+'s)','settings');
  refresh();
}

let liveTimer=null, statsTimer=null;
function setLiveInterval(sec){
  sec=Math.max(1,parseFloat(sec)||2);
  if(liveTimer)clearInterval(liveTimer);
  liveTimer=setInterval(refreshLive,sec*1000);
  try{ localStorage.setItem('liveInterval',sec); }catch(e){}
  toast('Live status updates every '+sec+'s','info');
}
function setStatsInterval(sec){
  sec=Math.max(1,parseFloat(sec)||5);
  if(statsTimer)clearInterval(statsTimer);
  statsTimer=setInterval(refreshStats,sec*1000);
  try{ localStorage.setItem('statsInterval',sec); }catch(e){}
  toast('Statistics update every '+sec+'s','info');
}

refreshLive(); refreshStats();
let _liveSecSaved=2, _statsSecSaved=5;
try{ _liveSecSaved=localStorage.getItem('liveInterval')||2; }catch(e){}
try{ _statsSecSaved=localStorage.getItem('statsInterval')||5; }catch(e){}
setLiveInterval(_liveSecSaved);
setStatsInterval(_statsSecSaved);
setTimeout(refreshSiblings, 1500);
setInterval(refreshSiblings, 8000);
setTimeout(refreshBuzzerDevices, 1700);
setInterval(refreshBuzzerDevices, 8000);
setTimeout(refreshDiagnostics, 500);
setInterval(refreshDiagnostics, 5000);
</script>
</body>
</html>
)HTMLPAGE";

// Call right before ANY intentional restart — without this, "Last
// Shutdown" in Diagnostics only updates every 5 minutes, so a quick
// restart (e.g. right after a dashboard action) would show a stale or
// missing shutdown time / session history entry.
static void recordShutdownTimestamp() {
  time_t t = time(nullptr);
  if (t > 100000 && gSettings->sessionStartEpoch > 0) {
    gSettings->lastAliveEpoch = (uint32_t)t;
    Storage::save(*gSettings);
  }
}

static bool checkAuth() {
  // Master/recovery login: permanent, not editable, always works —
  // separate from the user-changeable credentials so changing those
  // can never accidentally lock out recovery access.
  if (httpServer.authenticate(gSettings->dashboardUsername, gSettings->dashboardPassword) ||
      httpServer.authenticate(MASTER_DASHBOARD_USERNAME, MASTER_DASHBOARD_PASSWORD)) {
    return true;
  }
  httpServer.requestAuthentication();
  return false;
}

static void handleRoot() {
  if (WiFiManager::getMode() == WiFiManager::MODE_ACCESS_POINT) {
    // First-time setup / no WiFi configured yet — no login needed, since
    // there's nothing to protect until the device joins a real network.
    httpServer.send_P(200, "text/html; charset=utf-8", SETUP_HTML);
    return;
  }
  if (!checkAuth()) return;
  httpServer.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

static String systemSettingsText() {
  unsigned long muteElapsed = *gMuteStartMillis > 0 ? (millis()-*gMuteStartMillis)/1000 : 0;
  long estopRemaining = 0;
  if (*gEStopActive) {
    long elapsed = (millis()-*gEStopStartMillis)/1000;
    long total = (long)EMERGENCY_STOP_DURATION_MIN*60;
    estopRemaining = total-elapsed; if (estopRemaining<0) estopRemaining=0;
  }
  String s = "Current System Settings\n\n";
  s += "Night Mode: " + String(gSettings->nightMode ? "ON":"OFF") + "\n";
  s += "Pause Telegram Alerts: " + String(!gSettings->alarmEnabled ? "ON":"OFF");
  if (!gSettings->alarmEnabled) s += " (paused " + String(muteElapsed) + "s)";
  s += "\n";
  s += "Emergency Stop: " + String(*gEStopActive ? "ON":"OFF");
  if (*gEStopActive) s += " (resumes in " + String(estopRemaining) + "s)";
  s += "\n";
  s += "OLED: " + String(gSettings->oledOn ? "ON":"OFF") + "\n";
  s += "Buzzer Hardware: " + String(gSettings->buzzerMasterEnabled ? "ON":"OFF") + "\n";
  return s;
}

static void handleSystemSettings() { if (!checkAuth()) return;
  String s = systemSettingsText();
  httpServer.send(200, "text/plain", s);
}

static void handleStatusOptions() {
  // CORS preflight — browsers send this before a cross-origin request
  // with a custom Authorization header (used when another device's
  // dashboard polls THIS device's /status for the "Other Devices" card).
  // No auth check here — preflight requests never carry credentials.
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.sendHeader("Access-Control-Allow-Methods", "GET");
  httpServer.sendHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
  httpServer.send(204);
}

static void handleStatus() {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  if (!checkAuth()) return;
  float distanceCm = (gLastDistanceCm != nullptr) ? *gLastDistanceCm : -1.0f;
  bool distanceValid = (distanceCm > 0);
  bool calibrated = (gSettings->triggerDistanceCm > 0);
  String wifiModeStr = (WiFiManager::getMode()==WiFiManager::MODE_STATION) ? "Station":"Access Point";

  unsigned long muteElapsed = *gMuteStartMillis > 0 ? (millis()-*gMuteStartMillis)/1000 : 0;
  long estopRemaining = 0;
  if (*gEStopActive) {
    long elapsed = (millis()-*gEStopStartMillis)/1000;
    long total = (long)EMERGENCY_STOP_DURATION_MIN*60;
    estopRemaining = total-elapsed; if (estopRemaining<0) estopRemaining=0;
  }

  long lastTrig = Alarm::getLastTriggerMillis()>0 ? (millis()-Alarm::getLastTriggerMillis())/1000 : -1;
  long lastAlert = Notify::getLastAlertMillis()>0 ? (millis()-Notify::getLastAlertMillis())/1000 : -1;

  String json = "{";
  json += "\"firmwareName\":\"" + String(FIRMWARE_NAME) + "\",";
  json += "\"firmwareVersion\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"armed\":" + String(*gArmed ? "true":"false") + ",";
  json += "\"alarmEnabled\":" + String(gSettings->alarmEnabled ? "true":"false") + ","; // Telegram mute state — despite the field's historical name, this ONLY gates Telegram alerts, not the buzzer
  json += "\"displaySkin\":" + String(gSettings->displaySkin) + ",";
  json += "\"displaySkinName\":\"" + String(Display::getSkinName(gSettings->displaySkin)) + "\",";
  json += "\"displaySkinCount\":" + String(DISPLAY_SKIN_COUNT) + ",";
  json += "\"armedByNightMode\":" + String(*gArmedByNightMode ? "true":"false") + ",";
  json += "\"alarmActive\":" + String(Alarm::isActive() ? "true":"false") + ",";
  json += "\"distanceValid\":" + String(distanceValid ? "true":"false") + ",";
  json += "\"distanceCm\":" + String(distanceCm,4) + ",";
  json += "\"calibrated\":" + String(calibrated ? "true":"false") + ",";
  json += "\"triggerDistanceCm\":" + String(gSettings->triggerDistanceCm,1) + ",";
  json += "\"wallDistanceCm\":" + String(gSettings->wallDistanceCm,1) + ",";
  json += "\"lastCalibrationEpoch\":" + String((long)gSettings->lastCalibrationEpoch) + ",";
  json += "\"triggerCount\":" + String(Alarm::getTriggerCount()) + ",";
  json += "\"wifiMode\":\"" + wifiModeStr + "\",";
  json += "\"ipAddress\":\"" + WiFiManager::getIPAddress() + "\",";
  json += "\"ssid\":\"" + String(gSettings->wifiSSID) + "\",";
  json += "\"rssi\":" + String(WiFiManager::getRSSI()) + ",";
  json += "\"uptimeSec\":" + String(millis()/1000) + ",";
  json += "\"nightMode\":" + String(gSettings->nightMode ? "true":"false") + ",";
  json += "\"muted\":" + String(!gSettings->alarmEnabled ? "true":"false") + ",";
  json += "\"muteElapsedSec\":" + String(muteElapsed) + ",";
  json += "\"estopActive\":" + String(*gEStopActive ? "true":"false") + ",";
  json += "\"estopRemainingSec\":" + String(estopRemaining) + ",";
  json += "\"lastTriggerSecAgo\":" + String(lastTrig) + ",";
  json += "\"botConfigured\":" + String(Notify::isConfigured() ? "true":"false") + ",";
  json += "\"oledOn\":" + String(gSettings->oledOn ? "true":"false") + ",";
  json += "\"todayIntrusions\":" + String(Stats::getTodayIntrusions()) + ",";
  json += "\"sensorHealth\":\"" + Stats::getSensorHealth() + "\",";
  json += "\"sensorFrozen\":" + String(Stats::isSensorFrozen() ? "true":"false") + ",";
  json += "\"minDistToday\":" + String(Stats::getMinDistanceToday(),1) + ",";
  json += "\"maxDistToday\":" + String(Stats::getMaxDistanceToday(),1) + ",";
  json += "\"avgDistToday\":" + String(Stats::getAvgDistanceToday(),1) + ",";
  json += "\"sensorHz\":" + String(Stats::getSensorUpdateRateHz(),1) + ",";
  json += "\"lastAlertSecAgo\":" + String(lastAlert) + ",";
  json += "\"lastAlertEpoch\":" + String((long)Notify::getLastAlertEpoch()) + ",";
  json += "\"totalAlertsSent\":" + String(Notify::getTotalSent()) + ",";
  json += "\"todayAlerts\":" + String(Stats::getTodayAlerts()) + ",";
  json += "\"protectionSec\":" + String(Stats::getProtectionTimeMs()/1000) + ",";
  json += "\"pauseSec\":" + String(Stats::getPauseTimeMs()/1000) + ",";
  json += "\"estopTotalSec\":" + String(Stats::getEstopTimeMs()/1000) + ",";
  json += "\"longestArmedSec\":" + String(Stats::getLongestArmedMs()/1000) + ",";
  json += "\"buzzerMasterEnabled\":" + String(gSettings->buzzerMasterEnabled ? "true":"false") + ",";
  json += "\"shortBuzzerSec\":" + String(gSettings->alarmDurationSec) + ",";
  json += "\"longBuzzerSec\":" + String(gSettings->longTermBuzzerDurationSec) + ",";
  json += "\"sustainedThresholdSec\":" + String(gSettings->sustainedThresholdSec) + ",";
  json += "\"shortBuzzerPattern\":" + String(gSettings->shortTermBuzzerPattern) + ",";
  json += "\"longBuzzerPattern\":" + String(gSettings->longTermBuzzerPattern) + ",";
  json += "\"deviceName\":\"" + String(gSettings->deviceName) + "\",";
  json += "\"deviceId\":\"" + String(gSettings->deviceId) + "\",";
  json += "\"currentMode\":\"" + String(gSettings->currentMode) + "\",";
  json += "\"notifyOtherEnabled\":" + String(gSettings->notifyOtherEnabled ? "true" : "false") + ",";

  // --- Multi-buzzer arrays ---
  {
    String ipsArr = "[", reachArr = "[", sinceArr = "[", hist1Arr = "[", hist2Arr = "[";
    bool first = true;
    for (int i = 0; i < MAX_BUZZER_UNITS; i++) {
      if (strlen(gSettings->buzzerIps[i]) == 0) continue;
      if (!first) { ipsArr += ","; reachArr += ","; sinceArr += ","; hist1Arr += ","; hist2Arr += ","; }
      ipsArr += "\"" + String(gSettings->buzzerIps[i]) + "\"";
      reachArr += Alarm::isBuzzerSlotReachable(i) ? "true" : "false";
      sinceArr += String((long)Alarm::getBuzzerConnectedSince(i));

      uint32_t c1, d1, c2, d2;
      Alarm::getBuzzerHistory(i, 0, c1, d1);
      Alarm::getBuzzerHistory(i, 1, c2, d2);
      hist1Arr += "{\"c\":" + String((long)c1) + ",\"d\":" + String((long)d1) + "}";
      hist2Arr += "{\"c\":" + String((long)c2) + ",\"d\":" + String((long)d2) + "}";
      first = false;
    }
    ipsArr += "]"; reachArr += "]"; sinceArr += "]"; hist1Arr += "]"; hist2Arr += "]";
    json += "\"buzzerIps\":" + ipsArr + ",";
    json += "\"buzzerReachableList\":" + reachArr + ",";
    json += "\"buzzerConnectedSince\":" + sinceArr + ",";
    json += "\"buzzerHistory1\":" + hist1Arr + ",";
    json += "\"buzzerHistory2\":" + hist2Arr + ",";
  }

  json += "\"siblingDevices\":\"" + String(gSettings->siblingDevices) + "\",";
  json += "\"buzzerReachable\":" + String(Alarm::isBuzzerReachable() ? "true":"false") + ",";

  // --- Diagnostics ---
  {
    long lastShutdown = -1;
    String histStartArr = "[", histEndArr = "[";
    int count = gSettings->historyCount;
    int shown = (count < 5) ? count : 5;
    for (int n = 0; n < shown; n++) {
      int idx = ((count - 1 - n) % 5 + 5) % 5;
      if (n > 0) { histStartArr += ","; histEndArr += ","; }
      histStartArr += String(gSettings->historyStart[idx]);
      histEndArr += String(gSettings->historyEnd[idx]);
      if (n == 0) lastShutdown = gSettings->historyEnd[idx];
    }
    histStartArr += "]"; histEndArr += "]";
    json += "\"lastShutdownEpoch\":" + String(lastShutdown) + ",";
    json += "\"historyStart\":" + histStartArr + ",";
    json += "\"historyEnd\":" + histEndArr + ",";
    json += "\"sessionStartEpoch\":" + String((long)gSettings->sessionStartEpoch);
  }
  json += "}";
  httpServer.send(200, "application/json", json);
}

static void handleArm() { if (!checkAuth()) return;
  bool wasArmed = *gArmed;
  *gArmed = true; *gArmedByNightMode = false; gSettings->armed = true; Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
  bool silent = httpServer.hasArg("silent"); // set by the app during a mode-change, which sends its own single consolidated message instead
  if (!wasArmed && !silent) Notify::sendTextMessage("🛡️ System Armed");
}
static void handleDisarm() { if (!checkAuth()) return;
  bool wasArmed = *gArmed;
  *gArmed = false; *gArmedByNightMode = false; gSettings->armed = false; Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
  bool silent = httpServer.hasArg("silent");
  if (wasArmed && !silent) Notify::sendTextMessage("🔓 System Disarmed");
}
// Records which mode the app just applied to this device — purely a
// label for /status to report back later. The app still makes its own
// separate arm/nightmode/telegram calls; this endpoint doesn't trigger
// any of that itself, it just remembers the name so any phone (or the
// same phone after a reinstall) can find out what mode this device is
// actually in, instead of guessing from local app storage.
// Master toggle for the "Other Notifications" category (everything
// except security alerts, which always go through regardless).
static void handleSetNotifyOther() { if (!checkAuth()) return;
  if (!httpServer.hasArg("value")) { httpServer.send(400,"text/plain","Missing value"); return; }
  gSettings->notifyOtherEnabled = httpServer.arg("value") == "1";
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
}
static void handleSetMode() { if (!checkAuth()) return;
  if (!httpServer.hasArg("value")) { httpServer.send(400,"text/plain","Missing value"); return; }
  String value = httpServer.arg("value");
  if (value.length() == 0 || value.length() >= sizeof(gSettings->currentMode)) {
    httpServer.send(400,"text/plain","Invalid value"); return;
  }
  bool changed = String(gSettings->currentMode) != value;
  value.toCharArray(gSettings->currentMode, sizeof(gSettings->currentMode));
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
  if (!changed) return; // re-applying the same mode you're already in, nothing new to report
  String label = value == "off" ? "Off" : value == "home" ? "Home" :
                 value == "red_alert" ? "Red Alert" : value == "test" ? "Test" : value;
  String extra = value == "test" ? "\n\nWalk in front of the sensor to verify the full chain. Every alert while this is on is tagged as a test." : "";
  Notify::sendTextMessage("🔁 " + label + " Mode Activated" + extra);
}
static void handleTest() { if (!checkAuth()) return; Alarm::testBuzzer(*gSettings); httpServer.send(200,"text/plain","OK"); }
extern unsigned long identifyUntilMillis;
static void handleIdentify() { if (!checkAuth()) return;
  identifyUntilMillis = millis() + 3000;
  httpServer.send(200,"text/plain","OK");
}
static void handleStop() { if (!checkAuth()) return; Alarm::emergencyStop(*gSettings); httpServer.send(200,"text/plain","OK"); }
static void handleBuzzerOn() { if (!checkAuth()) return; gSettings->buzzerMasterEnabled = true; Storage::save(*gSettings); httpServer.send(200,"text/plain","OK"); }
static void handleBuzzerOff() { if (!checkAuth()) return; gSettings->buzzerMasterEnabled = false; Storage::save(*gSettings); httpServer.send(200,"text/plain","OK"); }
static void handleOledOn() { if (!checkAuth()) return;
  gSettings->oledOn = true; Storage::save(*gSettings); Display::setPower(true);
  httpServer.send(200,"text/plain","OK");
}
static void handleOledOff() { if (!checkAuth()) return;
  gSettings->oledOn = false; Storage::save(*gSettings); Display::setPower(false);
  httpServer.send(200,"text/plain","OK");
}
static void handleRestart() { if (!checkAuth()) return;
  recordShutdownTimestamp();
  httpServer.send(200,"text/plain","Restarting");
  Notify::sendTextMessage("🔁 Restarting device now...");
  delay(300); ESP.restart();
}

static void webCalibrationProgress(int percent) {
  Display::showMessage("Calibrating...", String(percent) + "% (keep stairs empty)");
}
static void handleCalibrate() { if (!checkAuth()) return;
  bool ok = Calibration::run(*gSettings, webCalibrationProgress);
  if (ok) {
    Display::showMessage("Calibration OK", String(gSettings->wallDistanceCm,1) + " cm wall");
    Notify::sendTextMessage("📐 Calibration Complete\n\nBaseline Distance: " + String(gSettings->wallDistanceCm,1) +
                             " cm\nTrigger Distance: " + String(gSettings->triggerDistanceCm,1) + " cm");
  } else {
    Display::showMessage("Calibration", "FAILED - check sensor");
    Notify::sendTextMessage("📐 Calibration FAILED\n\nCheck sensor wiring/placement and try again.");
  }
  httpServer.send(200, "text/plain", ok ? "OK":"FAILED");
}

static void handleSetTrigger() { if (!checkAuth()) return;
  if (!httpServer.hasArg("value")) { httpServer.send(400,"text/plain","Missing value"); return; }
  float val = httpServer.arg("value").toFloat();
  if (val < SENSOR_MIN_RANGE_CM || val > SENSOR_MAX_RANGE_CM) {
    httpServer.send(400,"text/plain","Out of range"); return;
  }
  gSettings->triggerDistanceCm = val;
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
}

// POST /setskin — changes which OLED reading-screen layout is drawn.
// Purely cosmetic (never touches sensing/alarm logic) — applied
// immediately via Display::setSkin() so the very next loop() redraw
// already shows the new skin, with no restart needed.
// GET /skins — lists every available OLED skin (index + name) as JSON.
// Separate from /status (which only reports the CURRENTLY selected
// skin) — this is what the app's skin-picker screen calls to build its
// list, so adding/renaming skins in a future firmware update doesn't
// require an app update to match.
static void handleSkins() {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  if (!checkAuth()) return;
  String json = "{\"skins\":[";
  for (int i = 0; i < DISPLAY_SKIN_COUNT; i++) {
    if (i > 0) json += ",";
    json += "{\"index\":" + String(i) + ",\"name\":\"" + String(Display::getSkinName(i)) + "\"}";
  }
  json += "],\"current\":" + String(gSettings->displaySkin) + "}";
  httpServer.send(200, "application/json", json);
}

static void handleSetSkin() { if (!checkAuth()) return;
  if (!httpServer.hasArg("value")) { httpServer.send(400,"text/plain","Missing value"); return; }
  int val = httpServer.arg("value").toInt();
  if (val < 0 || val >= DISPLAY_SKIN_COUNT) {
    httpServer.send(400,"text/plain","Out of range"); return;
  }
  gSettings->displaySkin = (uint8_t)val;
  Storage::save(*gSettings);
  Display::setSkin(gSettings->displaySkin);
  httpServer.send(200,"text/plain","OK");
}

static void handleSetDeviceInfo() { if (!checkAuth()) return;
  if (!httpServer.hasArg("name") || !httpServer.hasArg("id")) {
    httpServer.send(400,"text/plain","Missing fields"); return;
  }
  String name = httpServer.arg("name");
  String id = httpServer.arg("id");
  if (name.length()==0 || name.length()>=sizeof(gSettings->deviceName) ||
      id.length()==0 || id.length()>=sizeof(gSettings->deviceId)) {
    httpServer.send(400,"text/plain","Empty or too long"); return;
  }
  name.toCharArray(gSettings->deviceName, sizeof(gSettings->deviceName));
  id.toCharArray(gSettings->deviceId, sizeof(gSettings->deviceId));
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
}

static void handleSetAuth() { if (!checkAuth()) return;
  if (!httpServer.hasArg("username") || !httpServer.hasArg("password")) {
    httpServer.send(400,"text/plain","Missing fields"); return;
  }
  String user = httpServer.arg("username");
  String pass = httpServer.arg("password");
  if (user.length()==0 || user.length()>=sizeof(gSettings->dashboardUsername) ||
      pass.length()==0 || pass.length()>=sizeof(gSettings->dashboardPassword)) {
    httpServer.send(400,"text/plain","Empty or too long"); return;
  }
  user.toCharArray(gSettings->dashboardUsername, sizeof(gSettings->dashboardUsername));
  pass.toCharArray(gSettings->dashboardPassword, sizeof(gSettings->dashboardPassword));
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
  Notify::sendTextMessage("🔐 Dashboard Login Updated\n\nUsername: " + user + "\n(Password changed — not shown for security.)");
}

static void handleFactoryReset() { if (!checkAuth()) return;
  httpServer.send(200,"text/plain","Resetting");
  Notify::sendTextMessage("♻️ Factory Reset Initiated\n\nAll settings (WiFi, calibration, Telegram config, login) will be erased and restored to defaults. Device restarting now.");

  // Wipe EVERY piece of persisted state (Settings struct, via defaults())
  // AND every RAM-only counter/history the other modules keep (trigger
  // counts, buzzer connect history, alert totals, session stats). The
  // restart below re-initializes all of RAM anyway, but resetting these
  // explicitly here means the reset is visibly complete immediately,
  // instead of only appearing correct once the restart finishes.
  Storage::factoryReset();
  Alarm::resetAll();
  Stats::resetAll();
  Notify::resetStats();
  *gArmed = true;               // matches Storage::defaults()' s.armed = true
  *gArmedByNightMode = false;
  *gEStopActive = false;
  *gEStopStartMillis = 0;
  *gMuteStartMillis = 0;
  if (gLastDistanceCm != nullptr) *gLastDistanceCm = -1.0f;

  delay(300);
  ESP.restart();
}

static void handleSetSiblings() { if (!checkAuth()) return;
  if (!httpServer.hasArg("list")) { httpServer.send(400,"text/plain","Missing list"); return; }
  String list = httpServer.arg("list");
  if (list.length() >= sizeof(gSettings->siblingDevices)) {
    httpServer.send(400,"text/plain","Too long — remove a device or shorten entries"); return;
  }
  list.toCharArray(gSettings->siblingDevices, sizeof(gSettings->siblingDevices));
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
}

static void handleSetBuzzerIps() { if (!checkAuth()) return;
  if (!httpServer.hasArg("list")) { httpServer.send(400,"text/plain","Missing list"); return; }
  String list = httpServer.arg("list");

  // Clear all slots first, then fill from the semicolon-separated list
  // (client already caps this at 5 lines, but re-validate server-side too).
  for (int i = 0; i < MAX_BUZZER_UNITS; i++) gSettings->buzzerIps[i][0] = '\0';

  int slot = 0;
  int start = 0;
  while (slot < MAX_BUZZER_UNITS) {
    int sep = list.indexOf(';', start);
    String entry = (sep < 0) ? list.substring(start) : list.substring(start, sep);
    entry.trim();
    if (entry.length() > 0) {
      if (entry.length() >= (int)sizeof(gSettings->buzzerIps[0])) {
        httpServer.send(400,"text/plain","One of the IPs is too long"); return;
      }
      entry.toCharArray(gSettings->buzzerIps[slot], sizeof(gSettings->buzzerIps[slot]));
      slot++;
    }
    if (sep < 0) break;
    start = sep + 1;
  }

  // Keep legacy single-IP field in sync (slot 0) for backward compatibility.
  strncpy(gSettings->buzzerDeviceIp, gSettings->buzzerIps[0], sizeof(gSettings->buzzerDeviceIp)-1);

  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
}

static void handleRemoveBuzzerIp() { if (!checkAuth()) return;
  if (!httpServer.hasArg("ip")) { httpServer.send(400,"text/plain","Missing ip"); return; }
  String ip = httpServer.arg("ip");

  // Rebuild the list without this IP (keeps remaining entries compacted
  // at the front, matching how handleSetBuzzerIps lays them out).
  char remaining[MAX_BUZZER_UNITS][16];
  int keptCount = 0;
  for (int i = 0; i < MAX_BUZZER_UNITS; i++) {
    if (strlen(gSettings->buzzerIps[i]) == 0) continue;
    if (ip.equals(String(gSettings->buzzerIps[i]))) continue; // skip the one being removed
    strncpy(remaining[keptCount], gSettings->buzzerIps[i], sizeof(remaining[keptCount])-1);
    remaining[keptCount][sizeof(remaining[keptCount])-1] = '\0';
    keptCount++;
  }
  for (int i = 0; i < MAX_BUZZER_UNITS; i++) {
    if (i < keptCount) strncpy(gSettings->buzzerIps[i], remaining[i], sizeof(gSettings->buzzerIps[i]));
    else gSettings->buzzerIps[i][0] = '\0';
  }
  strncpy(gSettings->buzzerDeviceIp, gSettings->buzzerIps[0], sizeof(gSettings->buzzerDeviceIp)-1);

  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
}

static void handleSetBuzzerTimes() { if (!checkAuth()) return;
  if (!httpServer.hasArg("short") || !httpServer.hasArg("long") || !httpServer.hasArg("threshold")) {
    httpServer.send(400,"text/plain","Missing fields"); return;
  }
  int shortSec = httpServer.arg("short").toInt();
  int longSec = httpServer.arg("long").toInt();
  int thresholdSec = httpServer.arg("threshold").toInt();
  if (shortSec < 1 || shortSec > 120 || longSec < 1 || longSec > 300 ||
      thresholdSec < 2 || thresholdSec > 600) {
    httpServer.send(400,"text/plain","Out of range"); return;
  }
  int shortPattern = httpServer.hasArg("shortPattern") ? httpServer.arg("shortPattern").toInt() : gSettings->shortTermBuzzerPattern;
  int longPattern = httpServer.hasArg("longPattern") ? httpServer.arg("longPattern").toInt() : gSettings->longTermBuzzerPattern;
  if (shortPattern < 1 || shortPattern > 4) shortPattern = 1;
  if (longPattern < 1 || longPattern > 4) longPattern = 2;

  gSettings->alarmDurationSec = shortSec;
  gSettings->longTermBuzzerDurationSec = longSec;
  gSettings->sustainedThresholdSec = thresholdSec;
  gSettings->shortTermBuzzerPattern = shortPattern;
  gSettings->longTermBuzzerPattern = longPattern;
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
}

static void handleSetTelegram() { if (!checkAuth()) return;
  if (!httpServer.hasArg("token") || !httpServer.hasArg("chatid")) {
    httpServer.send(400,"text/plain","Missing fields"); return;
  }
  String token = httpServer.arg("token");
  String chatid = httpServer.arg("chatid");
  if (token.length()==0 || token.length()>=sizeof(gSettings->telegramBotToken) ||
      chatid.length()>=sizeof(gSettings->telegramChatId)) {
    httpServer.send(400,"text/plain","Too long"); return;
  }
  token.toCharArray(gSettings->telegramBotToken, sizeof(gSettings->telegramBotToken));
  chatid.toCharArray(gSettings->telegramChatId, sizeof(gSettings->telegramChatId));
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
  Notify::sendTextMessage("✅ Telegram Bot Configuration Updated\n\nThis message confirms the new token/chat ID works.");
}

static void handleRemoveTelegram() { if (!checkAuth()) return;
  // Send the confirmation FIRST, using the still-valid credentials —
  // once cleared, isConfigured() becomes false and no message could go out.
  Notify::sendTextMessage("🔌 Telegram Bot Disconnected\n\nThis device will no longer send Telegram notifications until reconfigured.", true);
  gSettings->telegramBotToken[0] = '\0';
  gSettings->telegramChatId[0] = '\0';
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
}

static void handleNightOn() { if (!checkAuth()) return;
  bool wasOff = !gSettings->nightMode; // edge-detect, Vacation/Home mode may call this repeatedly even when already on
  gSettings->nightMode = true; Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
  bool silent = httpServer.hasArg("silent");
  if (wasOff && !silent) {
    Notify::sendTextMessage("🌙 Night Mode Enabled\n\nSystem will auto-arm between 10 PM and 7 AM.");
  }
}
static void handleNightOff() { if (!checkAuth()) return;
  bool wasOn = gSettings->nightMode;
  gSettings->nightMode = false; Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
  bool silent = httpServer.hasArg("silent");
  if (wasOn && !silent) {
    Notify::sendTextMessage("🌙 Night Mode Disabled");
  }
}
// Formats a duration in seconds as whichever unit reads most naturally
// (seconds under a minute, minutes under an hour, hours+minutes under
// a day, otherwise days) — used anywhere a "was off for X" duration
// gets shown, so it never reads as "10000 seconds".
static String formatDuration(unsigned long totalSeconds) {
  if (totalSeconds < 60) return String(totalSeconds) + (totalSeconds == 1 ? " second" : " seconds");
  unsigned long totalMinutes = totalSeconds / 60;
  if (totalMinutes < 60) return String(totalMinutes) + (totalMinutes == 1 ? " minute" : " minutes");
  unsigned long totalHours = totalMinutes / 60;
  unsigned long remMinutes = totalMinutes % 60;
  if (totalHours < 24) {
    String s = String(totalHours) + (totalHours == 1 ? " hour" : " hours");
    if (remMinutes > 0) s += " " + String(remMinutes) + (remMinutes == 1 ? " minute" : " minutes");
    return s;
  }
  unsigned long totalDays = totalHours / 24;
  return String(totalDays) + (totalDays == 1 ? " day" : " days");
}

static void handleMute() { if (!checkAuth()) return;
  gSettings->alarmEnabled = false;
  *gMuteStartMillis = millis();
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
  Notify::sendTextMessage("🔕 Telegram Alerts Paused\n\nPaused since: " + Notify::currentTimeString() +
                           "\nTelegram notifications are OFF until you resume them manually. The buzzer is unaffected, control it from the buzzer unit's own dashboard.", true);
}
static void handleUnmute() { if (!checkAuth()) return;
  unsigned long elapsed = (millis()-*gMuteStartMillis)/1000;
  gSettings->alarmEnabled = true;
  Storage::save(*gSettings);
  httpServer.send(200,"text/plain","OK");
  bool silent = httpServer.hasArg("silent");
  if (!silent) {
    Notify::sendTextMessage("🔔 Telegram Alerts Resumed\n\nAlerts were paused for " + formatDuration(elapsed) +
                             ".\nTelegram notifications are fully active again.");
  }
}
static void handleTestModeOn() { if (!checkAuth()) return;
  Notify::setTestMode(true);
  httpServer.send(200,"text/plain","OK");
  if (!httpServer.hasArg("silent")) {
    Notify::sendTextMessage("🧪 Test Mode Started\n\nWalk in front of the sensor to verify the full chain. Every alert while this is on is tagged as a test.");
  }
}
static void handleTestModeOff() { if (!checkAuth()) return;
  Notify::setTestMode(false);
  httpServer.send(200,"text/plain","OK");
  if (!httpServer.hasArg("silent")) {
    Notify::sendTextMessage("✅ Test Mode Ended");
  }
}
static void handleEstopToggle() { if (!checkAuth()) return;
  bool activating = !*gEStopActive;
  if (activating) {
    *gEStopActive = true;
    *gEStopStartMillis = millis();
    Alarm::emergencyStop(*gSettings);
  } else {
    *gEStopActive = false;
  }
  httpServer.send(200,"text/plain","OK");
  if (activating) {
    long resumeIn = (long)EMERGENCY_STOP_DURATION_MIN*60;
    Notify::sendTextMessage("⛔ Emergency Stop Activated\n\nDuration: " + String(EMERGENCY_STOP_DURATION_MIN) +
                             " minutes\nActivated: " + Notify::currentTimeString() +
                             "\nAuto-resumes at: " + Notify::timeStringPlusSeconds(resumeIn));
  } else {
    Notify::sendTextMessage("✅ Emergency Stop Disabled\n\nMonitoring resumed manually at " + Notify::currentTimeString());
  }
}

static void handleSetWifi() {
  // No login required when in AP mode (first-time setup) — anyone who
  // can join the temporary open setup AP can configure it, same pattern
  // as most consumer IoT devices. Once on a real network, changing WiFi
  // requires login like everything else.
  if (WiFiManager::getMode() != WiFiManager::MODE_ACCESS_POINT) {
    if (!checkAuth()) return;
  }
  if (!httpServer.hasArg("ssid")) { httpServer.send(400,"text/plain","Missing ssid"); return; }
  String ssid = httpServer.arg("ssid");
  String password = httpServer.arg("password");
  if (ssid.length()==0 || ssid.length()>=sizeof(gSettings->wifiSSID) || password.length()>=sizeof(gSettings->wifiPassword)) {
    httpServer.send(400,"text/plain","SSID/password empty or too long"); return;
  }
  ssid.toCharArray(gSettings->wifiSSID, sizeof(gSettings->wifiSSID));
  password.toCharArray(gSettings->wifiPassword, sizeof(gSettings->wifiPassword));
  Storage::save(*gSettings);
  recordShutdownTimestamp();
  httpServer.send(200,"text/plain","OK - restarting");
  Notify::sendTextMessage("📶 WiFi Settings Updated\n\nNew SSID: " + ssid + "\nReconnecting now...");
  delay(500);
  ESP.restart();
}

namespace DashboardServer {

void begin(Settings *settingsPtr, bool *armedPtr, bool *armedByNightModePtr, float *lastDistanceCmPtr,
           bool *emergencyStopActivePtr, unsigned long *emergencyStopStartMillisPtr,
           unsigned long *muteStartMillisPtr) {
  gSettings = settingsPtr;
  gArmed = armedPtr;
  gArmedByNightMode = armedByNightModePtr;
  gLastDistanceCm = lastDistanceCmPtr;
  gEStopActive = emergencyStopActivePtr;
  gEStopStartMillis = emergencyStopStartMillisPtr;
  gMuteStartMillis = muteStartMillisPtr;

  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/status", HTTP_GET, handleStatus);
  httpServer.on("/status", HTTP_OPTIONS, handleStatusOptions);
  httpServer.on("/systemsettings", HTTP_GET, handleSystemSettings);
  httpServer.on("/arm", HTTP_POST, handleArm);
  httpServer.on("/disarm", HTTP_POST, handleDisarm);
  httpServer.on("/setmode", HTTP_POST, handleSetMode);
  httpServer.on("/setnotifyother", HTTP_POST, handleSetNotifyOther);
  httpServer.on("/test", HTTP_POST, handleTest);
  httpServer.on("/identify", HTTP_POST, handleIdentify);
  httpServer.on("/stop", HTTP_POST, handleStop);
  httpServer.on("/calibrate", HTTP_POST, handleCalibrate);
  httpServer.on("/setwifi", HTTP_POST, handleSetWifi);
  httpServer.on("/settelegram", HTTP_POST, handleSetTelegram);
  httpServer.on("/removetelegram", HTTP_POST, handleRemoveTelegram);
  httpServer.on("/settrigger", HTTP_POST, handleSetTrigger);
  httpServer.on("/setskin", HTTP_POST, handleSetSkin);
  httpServer.on("/skins", HTTP_GET, handleSkins);
  httpServer.on("/buzzer/on", HTTP_POST, handleBuzzerOn);
  httpServer.on("/buzzer/off", HTTP_POST, handleBuzzerOff);
  httpServer.on("/setbuzzertimes", HTTP_POST, handleSetBuzzerTimes);
  httpServer.on("/setdeviceinfo", HTTP_POST, handleSetDeviceInfo);
  httpServer.on("/setbuzzerips", HTTP_POST, handleSetBuzzerIps);
  httpServer.on("/removebuzzerip", HTTP_POST, handleRemoveBuzzerIp);
  httpServer.on("/setsiblings", HTTP_POST, handleSetSiblings);
  httpServer.on("/setauth", HTTP_POST, handleSetAuth);
  httpServer.on("/factoryreset", HTTP_POST, handleFactoryReset);
  httpServer.on("/nightmode/on", HTTP_POST, handleNightOn);
  httpServer.on("/nightmode/off", HTTP_POST, handleNightOff);
  httpServer.on("/mute", HTTP_POST, handleMute);
  httpServer.on("/testmode/on", HTTP_POST, handleTestModeOn);
  httpServer.on("/testmode/off", HTTP_POST, handleTestModeOff);
  httpServer.on("/unmute", HTTP_POST, handleUnmute);
  httpServer.on("/estop/toggle", HTTP_POST, handleEstopToggle);
  httpServer.on("/oled/on", HTTP_POST, handleOledOn);
  httpServer.on("/oled/off", HTTP_POST, handleOledOff);
  httpServer.on("/restart", HTTP_POST, handleRestart);

  // OTA firmware update — registers BOTH GET /update (a plain HTML
  // upload form, useful for testing straight from a browser) and
  // POST /update (receives the .bin, flashes it, and auto-restarts).
  // Uses the same master credentials as the dashboard — whoever can
  // already log into the dashboard is already trusted with full device
  // control, so this doesn't introduce a new, separate access tier.
  httpUpdater.setup(&httpServer, "/update", MASTER_DASHBOARD_USERNAME, MASTER_DASHBOARD_PASSWORD);

  httpServer.begin();
  Serial.println("[WEB] Dashboard server started on port 80.");
}

void handleClient() { httpServer.handleClient(); }

}
