#ifdef ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#define FS LittleFS
#define WebServer ESP8266WebServer
#else
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#define FS SPIFFS
#endif
#include <time.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>  // https://github.com/tzapu/WiFiManager

// --------------------------------------------------
// 1. Constantes e caminhos
// --------------------------------------------------
WiFiManager wifiManager;
const long  GMT_OFFSET_SEC      = -4 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER          = "pool.ntp.org";

const char* CONFIG_PATH         = "/config.json";
const int   MAX_SLOTS           = 10;
// Pino do alimentador: ESP32 usa GPIO18; ESP8266 (NodeMCU D5) usa GPIO14
#ifdef ESP8266
const int FEEDER_PIN = 14;  // D5 no ESP8266
#else
const int FEEDER_PIN = 18;  // GPIO18 no ESP32
#endif

// --------------------------------------------------
// 2. Servidor e agendamento
// --------------------------------------------------
WebServer server(80);

struct Schedule {
  int timeSec;
  int durationSec;
  int lastTriggerDay;
};
Schedule schedules[MAX_SLOTS];
int scheduleCount = 0;

// Duração do pulso manual (s)
unsigned long manualDurationSec = 5;

// Controle de pulso (manual ou agendado) sem bloqueio
bool isFeeding = false;
unsigned long feedStartMs = 0;
unsigned long feedDurationMs = 0;

// Timestamp do último acionamento (para respeitar intervalo)
unsigned long lastTriggerMs = 0;

// --------------------------------------------------
// 3. Protótipos
// --------------------------------------------------
void loadConfig();
void saveConfig();
int parseHHMMSS(const String&);
String formatHHMMSS(int);
int getSecOfDay();
int getDayOfYear();
void handleRoot();
void handleSetManualDuration();
void handleSetSchedules();
void handleFeedNow();

// --------------------------------------------------
// 4. Implementações
// --------------------------------------------------
void loadConfig() {
  // inicia sistema de arquivos
#ifdef ESP8266
  if (!FS.begin()) {
    Serial.println("Erro ao montar LittleFS");
    return;
  }
#else
  if (!FS.begin(true)) {
    Serial.println("Erro ao montar SPIFFS");
    return;
  }
#endif
  if (!FS.exists(CONFIG_PATH)) return;

  File file = FS.open(CONFIG_PATH, "r");
  if (!file) {
    Serial.println("Falha ao abrir config.json");
    return;
  }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, file)) {
    Serial.println("JSON inválido em config.json");
    file.close();
    return;
  }
  file.close();

  manualDurationSec = doc["manualDuration"] | manualDurationSec;
  scheduleCount = 0;
  for (JsonObject obj : doc["schedules"].as<JsonArray>()) {
    if (scheduleCount >= MAX_SLOTS) break;
    schedules[scheduleCount].timeSec      = obj["time"]     | 0;
    schedules[scheduleCount].durationSec  = obj["duration"] | manualDurationSec;
    schedules[scheduleCount].lastTriggerDay = -1;
    scheduleCount++;
  }

  Serial.printf("Config carregada: pulsoManual=%lus, agendamentos=%d\n",
                manualDurationSec, scheduleCount);
}

void saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["manualDuration"] = (int)manualDurationSec;
  JsonArray arr = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount; ++i) {
    JsonObject obj = arr.createNestedObject();
    obj["time"]     = schedules[i].timeSec;
    obj["duration"] = schedules[i].durationSec;
  }

  File file = FS.open(CONFIG_PATH, "w");
  if (!file) {
    Serial.println("Falha ao abrir config.json para escrita");
    return;
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("Config salva em SPIFFS");
}

int parseHHMMSS(const String &s) {
  int h=0, m=0, sec=0;
  if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3) {
    return h*3600 + m*60 + sec;
  }
  return -1;
}

String formatHHMMSS(int secs) {
  int h = secs/3600; secs %= 3600;
  int m = secs/60;   secs %= 60;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, secs);
  return String(buf);
}

int getSecOfDay() {
  time_t now = time(nullptr);
  struct tm tm; localtime_r(&now, &tm);
  return tm.tm_hour*3600 + tm.tm_min*60 + tm.tm_sec;
}

int getDayOfYear() {
  time_t now = time(nullptr);
  struct tm tm; localtime_r(&now, &tm);
  return tm.tm_yday;
}

// HTML com estado de botão desabilitado durante pulso
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1.0">
  <title>Alimentador Automático</title>
  <style>
    :root {
      --bg-card: #f9f9f9;
      --color-primary: #2e7d32;
      --color-error: #c62828;
      --radius: 5px;
      --spacing: 1rem;
      --font: Arial, sans-serif;
    }
    * { box-sizing: border-box; margin:0; padding:0; }
    body {
      font-family: var(--font);
      background:#fff; color:#333;
      padding:var(--spacing);
      max-width:600px; margin:0 auto;
    }
    header { text-align:center; margin-bottom:var(--spacing); }
    h1 { font-size:1.8rem; margin-bottom:.5rem; }
    #currentTime,#nextTrigger { font-weight:bold; margin-bottom:var(--spacing); }
    .card {
      background:var(--bg-card);
      border-radius:var(--radius);
      padding:var(--spacing);
      width:100%; margin-bottom:var(--spacing);
      box-shadow:0 2px 4px rgba(0,0,0,0.1);
    }
    form { display:flex; flex-direction:column; gap:.5rem; }
    label { font-weight:bold; }
    input[type=time] {
      padding:.5rem; border:1px solid #ccc;
      border-radius:var(--radius); width:100%;
    }
    button {
      padding:.6rem; border:none;
      border-radius:var(--radius);
      background:var(--color-primary);
      color:#fff; font-size:1rem;
      cursor:pointer; transition:opacity .2s;
      width:100%;
    }
    button:disabled { opacity:0.6; cursor:not-allowed; }
    ul { list-style:none; margin-top:.5rem; width:100%; }
    li {
      display:flex; justify-content:space-between;
      align-items:center; padding:.4rem 0;
      border-bottom:1px solid #e0e0e0;
    }
    .btn-remove {
      background:transparent;
      color:var(--color-error);
      font-weight:bold; width:2rem;
      height:2rem; line-height:2rem;
      text-align:center;
    }
  </style>
</head>
<body>
  <header>
    <h1>Alimentador Automático</h1>
    <div id="currentTime">Hora: --:--:--</div>
  </header>

  <section class="card">
    <button id="manualFeed">Alimentar Agora</button>
  </section>

  <section class="card">
    <form id="manualForm">
      <label for="manualDuration">Duração do Pulso (HH:MM:SS):</label>
      <input type="time" id="manualDuration" name="manualDuration" step="1" required value="%MANUAL%">
      <button type="submit">Salvar Duração</button>
    </form>
  </section>

  <section class="card">
    <form id="scheduleForm">
      <label for="newScheduleTime">Novo Horário (HH:MM:SS):</label>
      <input type="time" id="newScheduleTime" name="newScheduleTime" step="1">
      <label for="newScheduleDuration">Duração (HH:MM:SS):</label>
      <input type="time" id="newScheduleDuration" name="newScheduleDuration" step="1">
      <button type="button" id="addSchedule">+ Adicionar Agendamento</button>
    </form>
    <ul id="scheduleList"></ul>
    <button id="saveSchedules">Salvar Agendamentos</button>
  </section>

  <footer>
    <div id="nextTrigger">Próximo Acionamento: --:--:--</div>
  </footer>

  <script>
    // Carrega valores injetados por handleRoot()
    const rawManual = "%MANUAL%";
    const rawSchedules = "%SCHEDULES%";

    // Utilitários
    const pad = n => String(n).padStart(2,'0');
    const parseHHMMSS = s => {
      const [h,m,ses] = s.split(':').map(Number);
      return h*3600 + m*60 + ses;
    };
    const formatHHMMSS = secs => {
      const h = Math.floor(secs/3600);
      const m = Math.floor((secs%3600)/60);
      const s = secs%60;
      return [h,m,s].map(pad).join(':');
    };

    // Estado inicial
    let manualDurationSecs = parseHHMMSS(rawManual);
    document.getElementById('manualDuration').value = rawManual;
    const schedules = [];
    if (rawSchedules && rawSchedules !== 'nenhum') {
      rawSchedules.split(',').forEach(token => {
        const [time, duration] = token.split('|');
        schedules.push({ time, duration });
      });
    }

    // Atualiza exibição
    function renderSchedules() {
      const ul = document.getElementById('scheduleList'); ul.innerHTML = '';
      schedules.forEach((o,i) => {
        const li = document.createElement('li');
        li.textContent = `${o.time} (Duração: ${o.duration})`;
        const btn = document.createElement('button'); btn.textContent = '×';
        btn.className = 'btn-remove';
        btn.onclick = () => { schedules.splice(i,1); renderSchedules(); };
        li.appendChild(btn); ul.appendChild(li);
      });
    }
    renderSchedules();

    // Atualiza hora e próximo acionamento
    function updateCurrentTime() {
      document.getElementById('currentTime').textContent = 'Hora: ' + new Date().toLocaleTimeString();
    }
    function updateNextTrigger() {
      const now = new Date(); let next = null;
      schedules.forEach(o => {
        const [h,m,s] = o.time.split(':').map(Number);
        let d = new Date(now); d.setHours(h,m,s,0);
        if (d <= now) d.setDate(d.getDate()+1);
        if (!next || d < next) next = d;
      });
      if (next) {
        const diff = Math.max(0,(next - now)/1000);
        document.getElementById('nextTrigger').textContent = 'Próximo Acionamento: ' + formatHHMMSS(Math.floor(diff));
      }
    }
    setInterval(updateCurrentTime,1000);
    setInterval(updateNextTrigger,1000);
    updateCurrentTime(); updateNextTrigger();

    // Handlers
    document.getElementById('addSchedule').onclick = () => {
      const t = document.getElementById('newScheduleTime').value;
      const d = document.getElementById('newScheduleDuration').value;
      if (t && d) { schedules.push({time:t,duration:d}); renderSchedules(); }
    };

    document.getElementById('saveSchedules').onclick = () => {
      const payload = 'schedules=' + encodeURIComponent(
        schedules.map(o=>`${o.time}|${o.duration}`).join(',')
      );
      fetch('/setSchedules',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:payload});
    };

    const manualBtn = document.getElementById('manualFeed');
    manualBtn.onclick = () => {
      manualBtn.disabled = true;
      fetch('/feedNow',{method:'POST'})
        .then(()=>setTimeout(()=>{ manualBtn.disabled=false; }, manualDurationSecs*1000));
    };

    document.getElementById('manualForm').onsubmit = e => {
      e.preventDefault();
      const v = document.getElementById('manualDuration').value;
      fetch('/setManualDuration',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`manualDuration=${v}`})
        .then(()=>{ manualDurationSecs = parseHHMMSS(v); });
    };
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  String page = FPSTR(htmlPage);
  // Substitui valores
  String m = formatHHMMSS(manualDurationSec); page.replace("%MANUAL%", m);
  String sch;
  for (int i = 0; i < scheduleCount; ++i) {
    if (i) sch += ",";
    sch += formatHHMMSS(schedules[i].timeSec) + "|" + formatHHMMSS(schedules[i].durationSec);
  }
  page.replace("%SCHEDULES%", sch);
  server.send(200, "text/html; charset=UTF-8", page);
}

void handleSetManualDuration() {
  if (server.hasArg("manualDuration")) {
    int secs = parseHHMMSS(server.arg("manualDuration"));
    manualDurationSec = max(1, secs);
    Serial.printf("Pulso manual salvo: %lus\n", manualDurationSec);
    saveConfig();
  }
  server.sendHeader("Location","/"); server.send(302, "text/plain", "");
}

void handleSetSchedules() {
  if (server.hasArg("schedules")) {
    String list = server.arg("schedules");
    scheduleCount = 0;
    int start = 0;
    while (start < list.length() && scheduleCount < MAX_SLOTS) {
      int comma = list.indexOf(',', start);
      if (comma < 0) comma = list.length();
      String token = list.substring(start, comma);
      int sep = token.indexOf('|');
      if (sep>0) {
        int ts = parseHHMMSS(token.substring(0, sep));
        int di = parseHHMMSS(token.substring(sep+1));
        if (ts>=0 && di>0) {
          schedules[scheduleCount] = { ts, di, -1 };
          scheduleCount++;
        }
      }
      start = comma+1;
    }
    saveConfig();
  }
  server.sendHeader("Location","/"); server.send(302, "text/plain", "");
}

void handleFeedNow() {
  unsigned long now = millis();
  if (now - lastTriggerMs < manualDurationSec * 1000UL) {
    server.send(429, "text/plain", "Aguarde o intervalo mínimo");
    return;
  }
  // Inicia pulso sem bloqueio
  isFeeding = true;
  feedStartMs = now;
  feedDurationMs = manualDurationSec * 1000UL;
  digitalWrite(FEEDER_PIN, HIGH);
  lastTriggerMs = now;
  Serial.printf("Feed manual iniciado: duração %lums\n", feedDurationMs);
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  pinMode(FEEDER_PIN, OUTPUT);
  digitalWrite(FEEDER_PIN, LOW);
  loadConfig();

  wifiManager.setConfigPortalTimeout(180);
  if (!wifiManager.autoConnect("AlimentadorAP")) {
    Serial.println("Falha no WiFiManager, reiniciando...");
    delay(2000);
    ESP.restart();
  }

  Serial.println("Conectado: " + WiFi.localIP().toString());
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/setManualDuration", HTTP_POST, handleSetManualDuration);
  server.on("/setSchedules", HTTP_POST, handleSetSchedules);
  server.on("/feedNow", HTTP_POST, handleFeedNow);
  server.begin();
  Serial.println("HTTP server iniciado");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  // Gerencia término de pulso
  if (isFeeding && now - feedStartMs >= feedDurationMs) {
    digitalWrite(FEEDER_PIN, LOW);
    isFeeding = false;
    Serial.println("Pulso finalizado");
  }

  // Verifica agendamentos no momento exato (sem bloqueio)
  int today = getDayOfYear();
  int secOfDay = getSecOfDay();
  for (int i = 0; i < scheduleCount; ++i) {
    Schedule &s = schedules[i];
    if (secOfDay == s.timeSec && s.lastTriggerDay != today) {
      s.lastTriggerDay = today;
      // Evita cruzar intervalos muito próximos
      if (!isFeeding && now - lastTriggerMs >= s.durationSec * 1000UL) {
        isFeeding = true;
        feedStartMs = now;
        feedDurationMs = s.durationSec * 1000UL;
        digitalWrite(FEEDER_PIN, HIGH);
        lastTriggerMs = now;
        Serial.printf("Agendamento acionado: %s duração %lums\n",
                      formatHHMMSS(s.timeSec).c_str(), feedDurationMs);
      }
    }
  }
}
