/* 
  ESP32 Solar Panel Dashboard (2 voltage sensors + INA219 + LDR + DHT11 + 3 MOSFETs)
  - AJAX dashboard at "/" uses /status JSON endpoint
  - Pins and scale values below; calibrate V_DIVIDER_SCALE to your resistors
*/

#include <Wire.h>
#include <Adafruit_INA219.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ======= USER CONFIG =======
const char* WIFI_SSID = "Wifi";
const char* WIFI_PASS = "settings";

#define DHTPIN 4
#define DHTTYPE DHT11

// ADC pins (ESP32 ADC range 0-4095)
const int CELL_A_PIN = 34; // physical sensor for Cell A
const int CELL_C_PIN = 33; // physical sensor for Cell C
const int LDR_PIN    = 32;

// MOSFET pins (outputs to gate via resistor)
const int MOSFET1_PIN = 25; // bypass for Cell A
const int MOSFET2_PIN = 26; // bypass for Cell B (inferred)
const int MOSFET3_PIN = 27; // bypass for Cell C

// Hardware constants - set according to your wiring
const float V_DIVIDER_SCALE = 6.0 / 4095.0; 
// Explanation: if your divider maps 0-6.0V -> 0-4095 ADC, this factor converts ADC->Volts.
// If your divider maps 12V->3.3V, use (12.0 / 4095.0). Calibrate by measuring known voltage.

const float V_CELL_MAX = 6.0;    // expected max voltage per cell (adjust if different)
const float OK_RATIO = 0.70;     // below 70% of expected -> fault
const int SAMPLE_MS = 1500;      // sensor read interval

// ======= Globals =======
Adafruit_INA219 ina219;
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);

float cellA_v = 0, cellB_v_est = 0, cellC_v = 0, cellD_v_est = 0;
int ldr_adc = 0;
float lightPct = 0;
float busV = 0, current_mA = 0, powerW = 0;
float tempC = 0;
bool bypassA=false, bypassB=false, bypassC=false;

// ======= Setup =======
void setup() {
  Serial.begin(115200);
  Wire.begin(21,22);
  ina219.begin();
  dht.begin();

  pinMode(MOSFET1_PIN, OUTPUT);
  pinMode(MOSFET2_PIN, OUTPUT);
  pinMode(MOSFET3_PIN, OUTPUT);
  digitalWrite(MOSFET1_PIN, LOW);
  digitalWrite(MOSFET2_PIN, LOW);
  digitalWrite(MOSFET3_PIN, LOW);

  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/status", handleStatusJson);
  server.begin();
}

// ======= Loop =======
unsigned long lastSample = 0;
void loop() {
  server.handleClient();
  if (millis() - lastSample >= SAMPLE_MS) {
    lastSample = millis();
    readSensorsAndDecide();
  }
}

// ======= Read sensors and decide =======
void readSensorsAndDecide() {
  // ADC reads
  int rawA = analogRead(CELL_A_PIN);
  int rawC = analogRead(CELL_C_PIN);
  ldr_adc = analogRead(LDR_PIN);

  // convert ADC -> volts (calibrate V_DIVIDER_SCALE to your divider)
  cellA_v = rawA * V_DIVIDER_SCALE;
  cellC_v = rawC * V_DIVIDER_SCALE;

  // LDR mapped to 0-100%
  lightPct = map(ldr_adc, 0, 4095, 0, 100);
  if (lightPct < 0) lightPct = 0;
  if (lightPct > 100) lightPct = 100;

  // INA219 total measures
  busV = ina219.getBusVoltage_V();            // total voltage across the string
  current_mA = ina219.getCurrent_mA();
  powerW = ina219.getPower_mW() / 1000.0;

  // DHT
  tempC = dht.readTemperature();
  if (isnan(tempC)) tempC = -999;

  // Estimate remaining two cells: B + D = busV - (A + C)
  float remainingVD = busV - (cellA_v + cellC_v);
  if (remainingVD < 0) remainingVD = 0; // safety

  // split evenly to get per-cell estimate (simple)
  cellB_v_est = remainingVD / 2.0;
  cellD_v_est = remainingVD / 2.0;

  // expected cell voltage based on light (simple linear model)
  float expected_cell_v = (lightPct / 100.0) * V_CELL_MAX;
  if (expected_cell_v < 0.05) expected_cell_v = 0.05; // avoid divide-by-zero

  // Decide statuses and bypass actions

  // Cell A (measured)
  if (cellA_v < expected_cell_v * OK_RATIO) {
    // Fault -> enable bypass
    bypassA = true;
    digitalWrite(MOSFET1_PIN, HIGH);
  } else {
    bypassA = false;
    digitalWrite(MOSFET1_PIN, LOW);
  }

  // Cell B (inferred) - we have MOSFET2 for B
  if (cellB_v_est < expected_cell_v * OK_RATIO) {
    bypassB = true;
    digitalWrite(MOSFET2_PIN, HIGH);
  } else {
    bypassB = false;
    digitalWrite(MOSFET2_PIN, LOW);
  }

  // Cell C (measured)
  if (cellC_v < expected_cell_v * OK_RATIO) {
    bypassC = true;
    digitalWrite(MOSFET3_PIN, HIGH);
  } else {
    bypassC = false;
    digitalWrite(MOSFET3_PIN, LOW);
  }

  // Print for debug
  Serial.printf("Light: %d%%, A:%.2fV, B~:%.2fV, C:%.2fV, D~:%.2fV, Bus:%.2fV, I:%.1fmA, P:%.2fW, T:%.1fC\n",
                (int)lightPct, cellA_v, cellB_v_est, cellC_v, cellD_v_est, busV, current_mA, powerW, tempC);
}

// ======= Web Handlers =======

void handleRoot() {
  // Serve HTML page - uses AJAX to call /status
  String page = R"rawliteral(
  <!doctype html>
  <html>
  <head>
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>Solar Panel Monitor</title>
    <style>
      body{font-family:Arial,Helvetica,sans-serif;background:#eef2f3;margin:0;padding:20px;color:#222}
      h1{ text-align:center; margin-bottom:6px }
      #panel { width: 420px; height:220px; margin: 12px auto; border-radius:8px; background: linear-gradient(180deg,#0f4 10%,#0a8 100%); padding:14px; box-shadow:0 6px 20px rgba(0,0,0,0.15); }
      .cells { display:grid; grid-template-columns: repeat(2,1fr); grid-gap:12px; height:100%; }
      .cell { border-radius:8px; display:flex; flex-direction:column; justify-content:center; align-items:center; color:#fff; font-weight:700; font-size:18px; box-shadow:0 4px 8px rgba(0,0,0,0.12); }
      .green{ background: linear-gradient(180deg,#2ecc71,#27ae60); }
      .red  { background: linear-gradient(180deg,#e74c3c,#c0392b); }
      .yellow{ background: linear-gradient(180deg,#f1c40f,#f39c12); color:#111; }
      .gray{ background:#95a5a6; }
      .label{ font-size:12px; opacity:0.9; font-weight:500; margin-top:6px; }
      .stats { width:380px; margin:16px auto; background:#fff; padding:12px 16px; border-radius:10px; box-shadow:0 6px 18px rgba(0,0,0,0.07); }
      .row{ display:flex; justify-content:space-between; padding:6px 0; border-bottom:1px dashed #eee; }
      .row:last-child{ border-bottom:0 }
      small { color:#666 }
    </style>
  </head>
  <body>
    <h1>Solar Panel Monitor (ESP32)</h1>

    <div id="panel">
      <div class="cells">
        <div id="cellA" class="cell gray"><div>A</div><div class="label" id="vA">-- V</div></div>
        <div id="cellB" class="cell gray"><div>B</div><div class="label" id="vB">-- V</div></div>
        <div id="cellC" class="cell gray"><div>C</div><div class="label" id="vC">-- V</div></div>
        <div id="cellD" class="cell gray"><div>D</div><div class="label" id="vD">-- V</div></div>
      </div>
    </div>

    <div class="stats">
      <div class="row"><div>Total Power</div><div id="power">-- W</div></div>
      <div class="row"><div>Efficiency</div><div id="eff">-- %</div></div>
      <div class="row"><div>Light Intensity</div><div id="light">-- %</div></div>
      <div class="row"><div>Total Voltage</div><div id="vbus">-- V</div></div>
      <div class="row"><div>Current</div><div id="i">-- mA</div></div>
      <div class="row"><div>Temperature</div><div id="t">-- °C</div></div>
    </div>

    <script>
      async function fetchStatus(){
        try{
          const r = await fetch('/status');
          const j = await r.json();
          // update cells
          document.getElementById('vA').innerText = j.cellA.toFixed(2) + ' V';
          document.getElementById('vC').innerText = j.cellC.toFixed(2) + ' V';
          document.getElementById('vB').innerText = j.cellB.toFixed(2) + ' V';
          document.getElementById('vD').innerText = j.cellD.toFixed(2) + ' V';

          // set classes: green / red / yellow
          setCellClass('cellA', j.statusA);
          setCellClass('cellB', j.statusB);
          setCellClass('cellC', j.statusC);
          setCellClass('cellD', j.statusD);

          document.getElementById('power').innerText = j.power.toFixed(2) + ' W';
          document.getElementById('eff').innerText = j.eff.toFixed(1) + ' %';
          document.getElementById('light').innerText = j.light + ' %';
          document.getElementById('vbus').innerText = j.busV.toFixed(2) + ' V';
          document.getElementById('i').innerText = j.current_mA.toFixed(1) + ' mA';
          document.getElementById('t').innerText = (j.tempC === -999 ? '--' : j.tempC.toFixed(1)) + ' °C';
        } catch(e){
          console.log('fetch error', e);
        }
      }

      function setCellClass(id, status){
        const el = document.getElementById(id);
        el.className = 'cell';
        if (status === 'ok') el.classList.add('green');
        else if (status === 'fault') el.classList.add('red');
        else if (status === 'inferred-fault') el.classList.add('yellow');
        else el.classList.add('gray');
      }

      // refresh loop
      setInterval(fetchStatus, 2000);
      fetchStatus();
    </script>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", page);
}

void handleStatusJson() {
  // Build JSON with current values and statuses
  StaticJsonDocument<512> doc;

  // statuses: "ok", "fault", "inferred-fault"
  String statusA = (cellA_v < (lightPct/100.0)*V_CELL_MAX * OK_RATIO) ? "fault" : "ok";
  String statusC = (cellC_v < (lightPct/100.0)*V_CELL_MAX * OK_RATIO) ? "fault" : "ok";

  // inferred statuses: yellow if below threshold
  String statusB = (cellB_v_est < (lightPct/100.0)*V_CELL_MAX * OK_RATIO) ? "inferred-fault" : "ok";
  String statusD = (cellD_v_est < (lightPct/100.0)*V_CELL_MAX * OK_RATIO) ? "inferred-fault" : "ok";

  // efficiency heuristic: expected_total_voltage * measured_current
  float expected_cell_v = (lightPct/100.0) * V_CELL_MAX;
  float expected_total_v = expected_cell_v * 4.0;
  float expected_power_est = expected_total_v * (current_mA / 1000.0); // V * A (very rough)
  float eff = (expected_power_est > 0.0001) ? (powerW / expected_power_est * 100.0) : 0.0;
  if (eff < 0) eff = 0;
  if (eff > 999) eff = 999;

  doc["cellA"] = cellA_v;
  doc["cellB"] = cellB_v_est;
  doc["cellC"] = cellC_v;
  doc["cellD"] = cellD_v_est;
  doc["statusA"] = statusA;
  doc["statusB"] = statusB;
  doc["statusC"] = statusC;
  doc["statusD"] = statusD;
  doc["light"] = (int)lightPct;
  doc["busV"] = busV;
  doc["current_mA"] = current_mA;
  doc["power"] = powerW;
  doc["tempC"] = tempC;
  doc["eff"] = eff;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}
