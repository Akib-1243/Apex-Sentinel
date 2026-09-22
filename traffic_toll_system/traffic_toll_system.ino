/*
  🦅 Hawkeye — Traffic Toll & Over-speed System (NodeMCU ESP8266)
  ------------------------------------------------------------------
  - Immediate Toll Deduction for both Registered & Unregistered cards
  - Unregistered card balance starts at 0 and goes negative with Toll & Fines
  - Speed trap active for both Registered and Unregistered vehicles
  - Accrued negative balance settles automatically upon account registration
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>

// ---------- CONFIG — edit WiFi here ----------
const char* WIFI_SSID     = "Solo Lan";
const char* WIFI_PASSWORD = "112233445566";

// ---------- DYNAMIC SYSTEM VARIABLES ----------
float SENSOR_GAP_M  = 0.5;   // Distance between Sonar 1 and Sonar 2 in meters (50cm)
float MIN_TIME_SEC  = 2.0;   // Travel time threshold in seconds (under 2.0s = overspeed)
float TOLL_TK       = 50.0;  // Dynamic Toll amount per scan
float FINE_TK       = 200.0; // Dynamic Fine amount for overspeeding
float DETECT_CM     = 10.0;  // Max distance of car from sonar sensor (10 cm)

// ---------- SAFE PINS ----------
#define RFID_RST D4
#define RFID_SS  D8
#define TRIG_PIN D1   // Shared Trigger for Sonar 1 & 2
#define ECHO1    D2   // Sonar 1 Echo (Entry)
#define ECHO2    D0   // Sonar 2 Echo (Exit)

MFRC522 rfid(RFID_SS, RFID_RST);
ESP8266WebServer server(80);

bool pendingFine = false;
float lastSpeedKmh = 0.0;
float lastTimeSec = 0.0;
bool sensor1Armed = false;
unsigned long t1 = 0;

// ---------- RFID ACTIVE STATE ----------
bool rfidActive = false;
String activeUid = "";
unsigned long rfidScanTime = 0;

// ---------- STORAGE HELPERS ----------
void ensureFile(const char* path) {
  if (!LittleFS.exists(path)) {
    File f = LittleFS.open(path, "w");
    if (f) {
      f.print("[]");
      f.close();
    }
  }
}

bool loadArray(const char* path, JsonDocument& doc) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  return !err;
}

void saveArray(const char* path, JsonDocument& doc) {
  File f = LittleFS.open(path, "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
}

void logTransaction(const String& uid, const String& type, float amount, float balanceAfter, float speed = 0.0, float travelTime = 0.0) {
  JsonDocument doc;
  loadArray("/logs.json", doc);
  JsonArray logs = doc.as<JsonArray>();
  
  JsonObject entry = logs.add<JsonObject>();
  entry["id"] = millis();
  entry["ts"] = millis();
  entry["uid"] = uid;
  entry["type"] = type;
  entry["amount"] = amount;
  entry["balance"] = balanceAfter;
  entry["speed"] = speed;
  entry["time"] = travelTime;
  
  if (logs.size() > 100) {
    logs.remove(0);
  }

  saveArray("/logs.json", doc);
}

// ---------- PROCESS OVERSPEED FINE ----------
void processFine(const String& uid, float travelTimeSec, float speedKmh) {
  // 1. Check Registered Cars
  JsonDocument doc;
  loadArray("/cars.json", doc);
  JsonArray cars = doc.as<JsonArray>();

  for (JsonObject car : cars) {
    if (car["uid"].as<String>() == uid) {
      float balance = car["balance"].as<float>() - FINE_TK;
      car["balance"] = balance;
      car["overspeedCount"] = (car["overspeedCount"] | 0) + 1;
      car["totalFines"] = (car["totalFines"] | 0.0) + FINE_TK;
      car["lastSpeed"] = speedKmh;
      car["lastTime"] = travelTimeSec;

      saveArray("/cars.json", doc);
      logTransaction(uid, "fine", FINE_TK, balance, speedKmh, travelTimeSec);

      Serial.print("🚨 Overspeed Fine Charged (Registered): "); Serial.print(uid);
      Serial.print(" | Fine: ৳"); Serial.print(FINE_TK);
      Serial.print(" | Balance: ৳"); Serial.println(balance);
      return;
    }
  }

  // 2. If not registered, update Unregistered record
  JsonDocument unregDoc;
  loadArray("/unregistered.json", unregDoc);
  JsonArray unreg = unregDoc.as<JsonArray>();

  for (JsonObject item : unreg) {
    if (item["uid"].as<String>() == uid) {
      float balance = (item["balance"] | 0.0) - FINE_TK;
      item["balance"] = balance;
      item["overspeedCount"] = (item["overspeedCount"] | 0) + 1;
      item["lastSpeed"] = speedKmh;
      item["lastTime"] = travelTimeSec;

      saveArray("/unregistered.json", unregDoc);
      logTransaction(uid, "unregistered_fine", FINE_TK, balance, speedKmh, travelTimeSec);

      Serial.print("🚨 Overspeed Fine Charged (Unregistered): "); Serial.print(uid);
      Serial.print(" | Fine: ৳"); Serial.print(FINE_TK);
      Serial.print(" | Balance: ৳"); Serial.println(balance);
      return;
    }
  }
}

void updateVehicleStats(const String& uid, float travelTimeSec, float speedKmh) {
  JsonDocument doc;
  loadArray("/cars.json", doc);
  JsonArray cars = doc.as<JsonArray>();

  float currBalance = 0.0;
  bool isReg = false;

  for (JsonObject car : cars) {
    if (car["uid"].as<String>() == uid) {
      car["lastSpeed"] = speedKmh;
      car["lastTime"] = travelTimeSec;
      currBalance = car["balance"].as<float>();
      saveArray("/cars.json", doc);
      isReg = true;
      break;
    }
  }

  if (!isReg) {
    JsonDocument unregDoc;
    loadArray("/unregistered.json", unregDoc);
    JsonArray unreg = unregDoc.as<JsonArray>();
    for (JsonObject item : unreg) {
      if (item["uid"].as<String>() == uid) {
        item["lastSpeed"] = speedKmh;
        item["lastTime"] = travelTimeSec;
        currBalance = item["balance"] | 0.0;
        saveArray("/unregistered.json", unregDoc);
        break;
      }
    }
  }

  logTransaction(uid, isReg ? "speed_ok" : "unreg_speed_ok", 0, currBalance, speedKmh, travelTimeSec);
}

// ---------- SONAR / SPEED ----------
float readDistanceCm(int echoPin) {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(echoPin, HIGH, 25000); // 25ms timeout (~4m)
  if (duration == 0) return -1;
  return duration * 0.0343 / 2.0;
}

void checkSpeed() {
  if (!rfidActive) return;

  if (millis() - rfidScanTime > 10000) {
    Serial.println("⏱️ RFID Session Timed Out (Car took longer than 10s to start). Resetting.");
    rfidActive = false;
    sensor1Armed = false;
    return;
  }

  float d1 = readDistanceCm(ECHO1);
  
  if (d1 > 0 && d1 <= DETECT_CM && !sensor1Armed) {
    t1 = millis();
    sensor1Armed = true;
    Serial.print("🟢 Sonar 1 Triggered! Distance: ");
    Serial.print(d1);
    Serial.println(" cm");
  }

  if (sensor1Armed) {
    float d2 = readDistanceCm(ECHO2);
    if (d2 > 0 && d2 <= DETECT_CM) {
      unsigned long dt = millis() - t1;
      if (dt > 10) { // minimum 10ms debounce
        lastTimeSec = dt / 1000.0;
        float speedMps = SENSOR_GAP_M / lastTimeSec;
        lastSpeedKmh = speedMps * 3.6;
        
        Serial.print("🔴 Sonar 2 Triggered! Time taken: ");
        Serial.print(lastTimeSec, 3);
        Serial.print(" sec | Speed: ");
        Serial.print(lastSpeedKmh, 1);
        Serial.println(" km/h");

        if (lastTimeSec < MIN_TIME_SEC) {
          pendingFine = true;
          Serial.print("⚠️ OVERSPEED DETECTED! Time: ");
          Serial.print(lastTimeSec, 2);
          Serial.print("s (Threshold: < ");
          Serial.print(MIN_TIME_SEC, 1);
          Serial.println("s)");

          processFine(activeUid, lastTimeSec, lastSpeedKmh);
        } else {
          pendingFine = false;
          updateVehicleStats(activeUid, lastTimeSec, lastSpeedKmh);
          Serial.println("✅ NORMAL SPEED. No fine applied.");
        }
      }
      sensor1Armed = false;
      rfidActive = false;
    } 
    else if (millis() - t1 > 4000) {
      Serial.println("❌ Speed Trap Timeout (Car took longer than 4s between Sonar 1 & 2). Resetting.");
      sensor1Armed = false;
      rfidActive = false;
    }
  }
}

// ---------- RFID / TOLL ----------
void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  JsonDocument doc;
  loadArray("/cars.json", doc);
  JsonArray cars = doc.as<JsonArray>();

  bool found = false;
  for (JsonObject car : cars) {
    if (car["uid"].as<String>() == uid) {
      found = true;

      // Deduct Toll immediately from registered car
      float newBalance = car["balance"].as<float>() - TOLL_TK;
      car["balance"] = newBalance;
      saveArray("/cars.json", doc);
      logTransaction(uid, "toll", TOLL_TK, newBalance, 0, 0);

      rfidActive = true;
      activeUid = uid;
      rfidScanTime = millis();
      sensor1Armed = false;
      pendingFine = false;

      Serial.print("💳 RFID Scanned (Registered): "); Serial.print(uid);
      Serial.print(" | Toll Charged: ৳"); Serial.print(TOLL_TK);
      Serial.print(" | Balance: ৳"); Serial.println(newBalance);
      Serial.println(" | Speed Trap ACTIVATED!");
      break;
    }
  }

  // UNREGISTERED CARD HANDLING (Deduct toll starting from 0)
  if (!found) {
    JsonDocument unregDoc;
    loadArray("/unregistered.json", unregDoc);
    JsonArray unreg = unregDoc.as<JsonArray>();

    float unregBal = 0.0;
    bool unregFound = false;

    for (JsonObject item : unreg) {
      if (item["uid"].as<String>() == uid) {
        unregFound = true;
        unregBal = (item["balance"] | 0.0) - TOLL_TK;
        item["balance"] = unregBal;
        item["scans"] = (item["scans"] | 0) + 1;
        item["lastTs"] = millis();
        break;
      }
    }

    if (!unregFound) {
      unregBal = -TOLL_TK;
      JsonObject newItem = unreg.add<JsonObject>();
      newItem["uid"] = uid;
      newItem["balance"] = unregBal;
      newItem["scans"] = 1;
      newItem["overspeedCount"] = 0;
      newItem["lastTs"] = millis();
    }

    saveArray("/unregistered.json", unregDoc);
    logTransaction(uid, "unregistered_toll", TOLL_TK, unregBal, 0, 0);

    rfidActive = true;
    activeUid = uid;
    rfidScanTime = millis();
    sensor1Armed = false;
    pendingFine = false;

    Serial.print("⚠️ Unregistered Card Scanned: "); Serial.print(uid);
    Serial.print(" | Toll Deducted: ৳"); Serial.print(TOLL_TK);
    Serial.print(" | Balance: ৳"); Serial.println(unregBal);
    Serial.println(" | Speed Trap ACTIVATED!");
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ---------- DASHBOARD WEBSITE (HAWKEYE) ----------
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Hawkeye - Traffic Toll & Speed Radar</title>
<link href="https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@400;600;700;800&display=swap" rel="stylesheet">
<style>
:root { --bg: #090d16; --surface: #111827; --border: rgba(255,255,255,0.08); --primary: #3b82f6; --accent: #10b981; --danger: #ef4444; --warning: #f59e0b; --text: #f3f4f6; --muted: #9ca3af; }
* { box-sizing: border-box; margin: 0; padding: 0; font-family: 'Plus Jakarta Sans', sans-serif; }
body { background: var(--bg); color: var(--text); padding: 24px; min-height: 100vh; }
.container { max-width: 1200px; margin: 0 auto; }
header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; padding-bottom: 16px; border-bottom: 1px solid var(--border); }
.brand h1 { font-size: 24px; font-weight: 800; background: linear-gradient(to right, #60a5fa, #a7f3d0); -webkit-background-clip: text; -webkit-text-fill-color: transparent; display:flex; align-items:center; gap:10px; }
.live { display: flex; align-items: center; gap: 8px; background: rgba(16,185,129,0.1); border: 1px solid rgba(16,185,129,0.2); padding: 6px 14px; border-radius: 30px; font-size: 12px; color: var(--accent); font-weight: 700; }
.dot { width: 8px; height: 8px; background: var(--accent); border-radius: 50%; box-shadow: 0 0 10px var(--accent); }

.radar-hero { display: grid; grid-template-columns: 1.5fr 1fr; gap: 20px; margin-bottom: 24px; }
@media(max-width: 850px) { .radar-hero { grid-template-columns: 1fr; } }
.speed-display { background: var(--surface); border: 1px solid var(--border); border-radius: 20px; padding: 24px; text-align: center; position: relative; }
.speed-val { font-size: 64px; font-weight: 800; line-height: 1; margin: 8px 0; font-feature-settings: "tnum"; }
.badge { display: inline-block; padding: 6px 16px; border-radius: 20px; font-size: 12px; font-weight: 800; letter-spacing: 0.5px; }
.bg-danger { background: rgba(239,68,68,0.15); color: var(--danger); border: 1px solid rgba(239,68,68,0.3); }
.bg-success { background: rgba(16,185,129,0.15); color: var(--accent); border: 1px solid rgba(16,185,129,0.3); }

.grid { display: grid; grid-template-columns: 1fr 1.5fr; gap: 20px; margin-bottom: 24px; }
@media (max-width: 850px) { .grid { grid-template-columns: 1fr; } }
.card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; padding: 20px; }
.field { margin-bottom: 12px; }
.field label { display: block; font-size: 12px; color: var(--muted); margin-bottom: 4px; font-weight: 600; }
input { width: 100%; padding: 10px 14px; background: #0b1120; border: 1px solid var(--border); border-radius: 8px; color: #fff; font-size: 14px; outline: none; }
button.btn { width: 100%; padding: 10px 14px; background: var(--primary); border: none; border-radius: 8px; color: #fff; font-weight: 700; cursor: pointer; }
button.btn-danger { background: rgba(239,68,68,0.2); color: var(--danger); border: 1px solid rgba(239,68,68,0.4); padding: 5px 10px; border-radius: 6px; cursor: pointer; font-size: 12px; font-weight: 700; }
button.btn-sm { padding: 4px 8px; background: var(--primary); border: none; color: #fff; border-radius: 4px; cursor: pointer; font-size: 12px; font-weight: 700; }

table { width: 100%; border-collapse: collapse; font-size: 13px; }
th { text-align: left; padding: 10px 12px; color: var(--muted); border-bottom: 1px solid var(--border); font-size: 11px; text-transform: uppercase; }
td { padding: 12px; border-bottom: 1px solid rgba(255,255,255,0.04); }
.tag { padding: 2px 8px; border-radius: 4px; font-size: 10px; font-weight: 700; text-transform: uppercase; }
.t-toll { background: rgba(59,130,246,0.2); color: var(--primary); }
.t-fine { background: rgba(239,68,68,0.2); color: var(--danger); }
.t-unreg { background: rgba(245,158,11,0.2); color: var(--warning); }
.t-ok { background: rgba(16,185,129,0.2); color: var(--accent); }

.flex-between { display: flex; justify-content: space-between; align-items: center; margin-bottom: 16px; }
</style>
</head>
<body>
<div class="container">
  <header>
    <div class="brand"><h1>🦅 Hawkeye <span style="font-size:14px; color:var(--muted); font-weight:600">SmartToll & Speed OS</span></h1></div>
    <div class="live"><div class="dot"></div> RADAR LIVE</div>
  </header>

  <div class="radar-hero">
    <div class="speed-display">
      <div style="font-size:12px; text-transform:uppercase; color:var(--muted); font-weight:700">Vehicle Passage Time</div>
      <div class="speed-val" id="timeVal">0.00</div>
      <div style="color:var(--muted); font-weight:600; margin-bottom:8px">SECONDS (<span id="spdVal">0.0</span> KM/H)</div>
      <div id="spdB" class="badge bg-success">CLEAR ROAD</div>
    </div>

    <div class="card" style="display:flex; flex-direction:column; justify-content:center">
      <h3 style="margin-bottom:12px; font-size:15px">⚙️ System Charges & Time Limit</h3>
      <div style="display:grid; grid-template-columns:1fr 1fr; gap:10px">
        <div class="field"><label>Toll Fee (৳)</label><input type="number" id="tollInput" placeholder="50"></div>
        <div class="field"><label>Fine Fee (৳)</label><input type="number" id="fineInput" placeholder="200"></div>
      </div>
      <div class="field">
        <label>Min Travel Time Threshold (sec)</label>
        <input type="number" id="limitInput" placeholder="2.0" step="0.1">
      </div>
      <button class="btn" onclick="updateSettings()">Save System Settings</button>
      <div style="margin-top:10px; font-size:11px; color:var(--muted)">Toll: <b id="currTollText" style="color:#fff">৳50</b> | Fine: <b id="currFineText" style="color:#fff">৳200</b> | Limit: <b id="currLimText" style="color:#fff">2.0s</b></div>
    </div>
  </div>

  <div class="grid">
    <div class="card">
      <h3 style="margin-bottom:16px">Register Vehicle</h3>
      <div class="field"><label>RFID UID</label><input id="uid" placeholder="04A1B2C3" style="text-transform:uppercase"></div>
      <div class="field"><label>Owner Name</label><input id="own" placeholder="John Doe"></div>
      <div class="field"><label>Initial Balance (৳)</label><input id="bal" type="number" placeholder="500"></div>
      <button class="btn" onclick="reg()">Register Account</button>
    </div>

    <div class="card" style="overflow-x:auto">
      <h3 style="margin-bottom:16px">Registered Accounts</h3>
      <table>
        <thead><tr><th>UID / Owner</th><th>Balance</th><th>Overspeeds</th><th>Top-up / Action</th></tr></thead>
        <tbody id="carsTb"></tbody>
      </table>
    </div>
  </div>

  <div class="card" style="overflow-x:auto; margin-bottom: 24px;">
    <h3 style="margin-bottom:16px">⚠️ Unregistered Scans</h3>
    <table>
      <thead><tr><th>UID</th><th>Balance</th><th>Total Scans</th><th>Violations</th><th>Action</th></tr></thead>
      <tbody id="unregTb"></tbody>
    </table>
  </div>

  <div class="card" style="overflow-x:auto">
    <div class="flex-between">
      <h3>Transaction & Speed Fine Logs</h3>
      <button class="btn-danger" onclick="clearLogs()">Clear All Logs</button>
    </div>
    <table>
      <thead><tr><th>Time</th><th>UID</th><th>Event Type</th><th>Speed / Passage Time</th><th>Amount</th><th>Balance</th><th>Action</th></tr></thead>
      <tbody id="logsTb"></tbody>
    </table>
  </div>
</div>

<script>
async function refresh(){
  try{
    const st = await (await fetch('/api/status')).json();
    const tm = st.lastTime.toFixed(2);
    const spd = st.lastSpeed.toFixed(1);
    
    document.getElementById('timeVal').textContent = tm;
    document.getElementById('spdVal').textContent = spd;
    document.getElementById('currTollText').textContent = '৳' + st.tollTk;
    document.getElementById('currFineText').textContent = '৳' + st.fineTk;
    document.getElementById('currLimText').textContent = st.minTimeSec.toFixed(1) + 's';

    const spdB = document.getElementById('spdB');
    if (st.pendingFine) {
      document.getElementById('timeVal').style.color = 'var(--danger)';
      spdB.className = 'badge bg-danger';
      spdB.textContent = '🚨 OVERSPEED DETECTED (< ' + st.minTimeSec.toFixed(1) + 's)';
    } else if (st.lastTime > 0) {
      document.getElementById('timeVal').style.color = 'var(--accent)';
      spdB.className = 'badge bg-success';
      spdB.textContent = 'NORMAL SPEED (≥ ' + st.minTimeSec.toFixed(1) + 's)';
    } else {
      document.getElementById('timeVal').style.color = 'var(--text)';
      spdB.className = 'badge bg-success';
      spdB.textContent = 'CLEAR ROAD';
    }

    // Registered Accounts
    const cars = await (await fetch('/api/cars')).json();
    document.getElementById('carsTb').innerHTML = cars.map(c=>`
      <tr>
        <td><b style="font-family:monospace;color:#60a5fa">${c.uid}</b><br><span style="font-size:12px;color:var(--muted)">${c.owner}</span></td>
        <td style="font-weight:700;color:${c.balance<0?'var(--danger)':'var(--accent)'}">৳${c.balance}</td>
        <td><span class="tag ${c.overspeedCount>0?'t-fine':'t-ok'}">${c.overspeedCount||0} Violations</span></td>
        <td>
          <div style="display:flex;gap:4px;align-items:center">
            <input type="number" id="t_${c.uid}" style="width:60px;padding:4px" placeholder="৳">
            <button onclick="topup('${c.uid}')" class="btn-sm">Add</button>
            <button onclick="deleteCar('${c.uid}')" class="btn-danger" style="padding:4px 6px">✕</button>
          </div>
        </td>
      </tr>`).join('');

    // Unregistered Scans
    const unreg = await (await fetch('/api/unregistered')).json();
    document.getElementById('unregTb').innerHTML = unreg.length === 0 ? 
      `<tr><td colspan="5" style="color:var(--muted);text-align:center">No unregistered cards scanned</td></tr>` :
      unreg.map(u=>`
      <tr>
        <td style="font-family:monospace;font-weight:700;color:var(--warning)">${u.uid}</td>
        <td style="font-weight:700;color:${(u.balance||0)<0?'var(--danger)':'var(--accent)'}">৳${u.balance||0}</td>
        <td>${u.scans} scan(s)</td>
        <td><span class="tag ${(u.overspeedCount||0)>0?'t-fine':'t-ok'}">${u.overspeedCount||0} Violations</span></td>
        <td>
          <div style="display:flex;gap:6px">
            <button onclick="quickRegister('${u.uid}')" class="btn-sm">Register UID</button>
            <button onclick="deleteUnregistered('${u.uid}')" class="btn-danger">Delete</button>
          </div>
        </td>
      </tr>`).join('');

    // Logs
    const logs = await (await fetch('/api/logs')).json();
    document.getElementById('logsTb').innerHTML = logs.slice(-20).reverse().map((l, idx)=>`
      <tr>
        <td style="color:var(--muted);font-size:12px">${Math.floor(l.ts/1000)}s</td>
        <td style="font-family:monospace;font-weight:700">${l.uid}</td>
        <td><span class="tag ${l.type.includes('fine')?'t-fine':(l.type.includes('unregistered')?'t-unreg':(l.type==='toll'?'t-toll':'t-ok'))}">${l.type.replace('_',' ').toUpperCase()}</span></td>
        <td>${l.speed > 0 ? `<b>${l.speed.toFixed(1)} km/h</b> (${l.time.toFixed(2)}s)` : '-'}</td>
        <td style="font-weight:700">৳${l.amount}</td>
        <td style="color:${l.balance<0?'var(--danger)':'var(--accent)'}">৳${l.balance}</td>
        <td><button onclick="deleteLog('${l.id}', ${logs.length - 1 - idx})" class="btn-danger">Delete</button></td>
      </tr>`).join('');
  }catch(e){}
}

async function updateSettings(){
  const minTimeSec = parseFloat(document.getElementById('limitInput').value);
  const tollTk = parseFloat(document.getElementById('tollInput').value);
  const fineTk = parseFloat(document.getElementById('fineInput').value);

  const payload = {};
  if(minTimeSec > 0) payload.minTimeSec = minTimeSec;
  if(tollTk >= 0) payload.tollTk = tollTk;
  if(fineTk >= 0) payload.fineTk = fineTk;

  await fetch('/api/settings', {method:'POST', body: JSON.stringify(payload)});
  document.getElementById('limitInput').value = '';
  document.getElementById('tollInput').value = '';
  document.getElementById('fineInput').value = '';
  refresh();
}

async function reg(){
  const uid = document.getElementById('uid').value.trim().toUpperCase();
  const owner = document.getElementById('own').value.trim();
  const balance = parseFloat(document.getElementById('bal').value) || 0;
  if(!uid || !owner) return alert('Enter UID & Owner');
  await fetch('/api/cars', {method:'POST', body: JSON.stringify({uid, owner, balance})});
  document.getElementById('uid').value=''; document.getElementById('own').value=''; document.getElementById('bal').value='';
  refresh();
}

function quickRegister(uid){
  document.getElementById('uid').value = uid;
  document.getElementById('own').focus();
}

async function topup(uid){
  const amount = parseFloat(document.getElementById('t_'+uid).value) || 0;
  if(!amount) return;
  await fetch('/api/topup', {method:'POST', body: JSON.stringify({uid, amount})});
  refresh();
}

async function deleteCar(uid){
  if(!confirm('Delete registered car ' + uid + '?')) return;
  await fetch('/api/cars/delete', {method:'POST', body: JSON.stringify({uid})});
  refresh();
}

async function deleteUnregistered(uid){
  await fetch('/api/unregistered/delete', {method:'POST', body: JSON.stringify({uid})});
  refresh();
}

async function deleteLog(id, index){
  await fetch('/api/logs/delete', {method:'POST', body: JSON.stringify({id, index})});
  refresh();
}

async function clearLogs(){
  if(!confirm('Clear all transaction logs?')) return;
  await fetch('/api/logs/clear', {method:'POST'});
  refresh();
}

refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleGetCars() {
  File f = LittleFS.open("/cars.json", "r");
  server.streamFile(f, "application/json");
  f.close();
}

void handleRegisterCar() {
  JsonDocument body;
  deserializeJson(body, server.arg("plain"));

  String uid = body["uid"].as<String>();
  String owner = body["owner"].as<String>();
  float inputBalance = body["balance"] | 0.0;

  // Check if this card was in unregistered list to preserve accrued debt/violations
  JsonDocument unregDoc;
  loadArray("/unregistered.json", unregDoc);
  JsonArray unreg = unregDoc.as<JsonArray>();

  float accruedBalance = 0.0;
  int accruedOverspeeds = 0;

  for (size_t i = 0; i < unreg.size(); i++) {
    if (unreg[i]["uid"].as<String>() == uid) {
      accruedBalance = unreg[i]["balance"] | 0.0;
      accruedOverspeeds = unreg[i]["overspeedCount"] | 0;
      unreg.remove(i);
      break;
    }
  }
  saveArray("/unregistered.json", unregDoc);

  // Add or update in cars.json
  JsonDocument doc;
  loadArray("/cars.json", doc);
  JsonArray cars = doc.as<JsonArray>();

  float finalBalance = inputBalance + accruedBalance;

  bool found = false;
  for (JsonObject car : cars) {
    if (car["uid"].as<String>() == uid) {
      car["owner"] = owner;
      car["balance"] = finalBalance;
      found = true;
      break;
    }
  }

  if (!found) {
    JsonObject car = cars.add<JsonObject>();
    car["uid"] = uid;
    car["owner"] = owner;
    car["balance"] = finalBalance;
    car["overspeedCount"] = accruedOverspeeds;
    car["totalFines"] = 0;
  }

  saveArray("/cars.json", doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleDeleteCar() {
  JsonDocument body;
  deserializeJson(body, server.arg("plain"));
  String uid = body["uid"].as<String>();

  JsonDocument doc;
  loadArray("/cars.json", doc);
  JsonArray cars = doc.as<JsonArray>();
  for (size_t i = 0; i < cars.size(); i++) {
    if (cars[i]["uid"].as<String>() == uid) {
      cars.remove(i);
      break;
    }
  }
  saveArray("/cars.json", doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleGetUnregistered() {
  File f = LittleFS.open("/unregistered.json", "r");
  server.streamFile(f, "application/json");
  f.close();
}

void handleDeleteUnregistered() {
  JsonDocument body;
  deserializeJson(body, server.arg("plain"));
  String uid = body["uid"].as<String>();

  JsonDocument doc;
  loadArray("/unregistered.json", doc);
  JsonArray unreg = doc.as<JsonArray>();
  for (size_t i = 0; i < unreg.size(); i++) {
    if (unreg[i]["uid"].as<String>() == uid) {
      unreg.remove(i);
      break;
    }
  }
  saveArray("/unregistered.json", doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleTopup() {
  JsonDocument body;
  deserializeJson(body, server.arg("plain"));
  String uid = body["uid"].as<String>();
  float amount = body["amount"] | 0;

  JsonDocument doc;
  loadArray("/cars.json", doc);
  JsonArray cars = doc.as<JsonArray>();
  for (JsonObject car : cars) {
    if (car["uid"].as<String>() == uid) {
      car["balance"] = car["balance"].as<float>() + amount;
      break;
    }
  }
  saveArray("/cars.json", doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleGetLogs() {
  File f = LittleFS.open("/logs.json", "r");
  server.streamFile(f, "application/json");
  f.close();
}

void handleDeleteLog() {
  JsonDocument body;
  deserializeJson(body, server.arg("plain"));
  int index = body["index"] | -1;

  JsonDocument doc;
  loadArray("/logs.json", doc);
  JsonArray logs = doc.as<JsonArray>();
  if (index >= 0 && index < (int)logs.size()) {
    logs.remove(index);
  }
  saveArray("/logs.json", doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleClearLogs() {
  JsonDocument doc;
  doc.to<JsonArray>();
  saveArray("/logs.json", doc);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStatus() {
  JsonDocument doc;
  doc["lastSpeed"] = lastSpeedKmh;
  doc["lastTime"] = lastTimeSec;
  doc["pendingFine"] = pendingFine;
  doc["minTimeSec"] = MIN_TIME_SEC;
  doc["tollTk"] = TOLL_TK;
  doc["fineTk"] = FINE_TK;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleUpdateSettings() {
  JsonDocument body;
  deserializeJson(body, server.arg("plain"));
  if (body["minTimeSec"].is<float>()) MIN_TIME_SEC = body["minTimeSec"].as<float>();
  if (body["tollTk"].is<float>()) TOLL_TK = body["tollTk"].as<float>();
  if (body["fineTk"].is<float>()) FINE_TK = body["fineTk"].as<float>();

  server.send(200, "application/json", "{\"ok\":true}");
}

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/cars", HTTP_GET, handleGetCars);
  server.on("/api/cars", HTTP_POST, handleRegisterCar);
  server.on("/api/cars/delete", HTTP_POST, handleDeleteCar);
  server.on("/api/unregistered", HTTP_GET, handleGetUnregistered);
  server.on("/api/unregistered/delete", HTTP_POST, handleDeleteUnregistered);
  server.on("/api/topup", HTTP_POST, handleTopup);
  server.on("/api/logs", HTTP_GET, handleGetLogs);
  server.on("/api/logs/delete", HTTP_POST, handleDeleteLog);
  server.on("/api/logs/clear", HTTP_POST, handleClearLogs);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_POST, handleUpdateSettings);
}

// ---------- SETUP / LOOP ----------
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO1, INPUT);
  pinMode(ECHO2, INPUT);

  SPI.begin();
  rfid.PCD_Init();

  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  
  ensureFile("/cars.json");
  ensureFile("/logs.json");
  ensureFile("/unregistered.json");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Hawkeye Dashboard ready at: http://");
  Serial.println(WiFi.localIP());

  setupRoutes();
  server.begin();
}

void loop() {
  server.handleClient();
  checkRFID();
  checkSpeed();
}