#ifdef ESP8266
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <LittleFS.h>
  #define FS LittleFS
  #define WebServer ESP8266WebServer
  #define STATUS_LED_PIN LED_BUILTIN
#else
  #include <WiFi.h>
  #include <WebServer.h>
  #include <SPIFFS.h>
  #define FS SPIFFS
  #define STATUS_LED_PIN 2
#endif

#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <time.h>

// =============== Configuration =============== //
const char* CONFIG_PATH = "/config.json";
const int MAX_SLOTS = 10;
const long GMT_OFFSET_SEC = -4 * 3600;    // UTC-4 (Cuiabá)
const int DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER = "pool.ntp.org";
const int FEED_COOLDOWN = 10;          // Seconds between feedings
const int MAX_FEED_DURATION = 300;        // 5 minutes max
const int WATCHDOG_TIMEOUT = 120000;      // 2 minutes

#ifdef ESP8266
const int FEEDER_PIN = 14;  // D5 on ESP8266
#else
const int FEEDER_PIN = 18;  // GPIO18 on ESP32
#endif

// =============== Data Structures =============== //
struct Schedule {
  int timeSec;        // Scheduled time in seconds since midnight
  int durationSec;      // Feeding duration in seconds
  int lastTriggerDay; // Last day this schedule was triggered (0-365)
};

// =============== Global State =============== //
WebServer server(80);
WiFiManager wifiManager;

Schedule schedules[MAX_SLOTS];
int scheduleCount = 0;
unsigned long manualDurationSec = 5;
char customSchedule[512] = "";
bool customEnabled = false;

volatile bool isFeeding = false;
unsigned long feedStartMs = 0;
unsigned long feedDurationMs = 0;
unsigned long lastTriggerMs = 0;
unsigned long lastWatchdogReset = 0;

// =============== HTML Page =============== //
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1.0">
  <title>Alimentador Automático</title>
  <style>
    body {
      font-family: system-ui, -apple-system, sans-serif;
      margin: 0;
      padding: 20px;
      max-width: 600px;
      margin: 0 auto;
      background: #f5f5f5;
    }
    h1 {
      text-align: center;
      color: #333;
      margin-bottom: 30px;
    }
    .card {
      background: #fff;
      padding: 20px;
      margin-bottom: 20px;
      border-radius: 8px;
      box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    label {
      display: block;
      margin-bottom: 8px;
      color: #555;
      font-weight: 500;
    }
    input[type=time], input[type=text] {
      width: 100%;
      padding: 10px;
      border: 1px solid #ddd;
      border-radius: 4px;
      margin-bottom: 10px;
    }
    textarea {
      width: 100%;
      padding: 10px;
      border: 1px solid #ddd;
      border-radius: 4px;
      min-height: 120px;
      resize: vertical;
    }
    button {
      width: 100%;
      padding: 12px;
      background: #2e7d32;
      color: white;
      border: none;
      border-radius: 4px;
      font-weight: 500;
      cursor: pointer;
      margin-bottom: 10px;
    }
    button:hover {
      background: #256427;
    }
    button.secondary {
      background: #666;
    }
    button.secondary:hover {
      background: #555;
    }
    ul {
      list-style: none;
      padding: 0;
    }
    li {
      margin-bottom: 8px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 8px;
      background: #f8f8f8;
      border-radius: 4px;
    }
    #currentTime {
      text-align: center;
      margin: 20px 0;
      font-size: 1.2em;
      color: #666;
    }
    #nextTrigger {
      text-align: center;
      margin: 20px 0;
      padding: 10px;
      background: #e8f5e9;
      border-radius: 4px;
      color: #2e7d32;
    }
    .status-indicator {
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 10px;
      margin-bottom: 20px;
    }
    .led {
      width: 15px;
      height: 15px;
      border-radius: 50%;
      background: #ccc;
    }
    .led.active {
      background: #4caf50;
      box-shadow: 0 0 10px #4caf50;
    }
    .led.feeding {
      background: #ff9800;
      box-shadow: 0 0 10px #ff9800;
      animation: pulse 1s infinite;
    }
    @keyframes pulse {
      0% { opacity: 1; }
      50% { opacity: 0.5; }
      100% { opacity: 1; }
    }
    .message {
      margin-top: 10px;
      padding: 10px;
      border-radius: 4px;
      text-align: center;
      font-weight: bold;
    }
    .message.success {
      background-color: #e6f4ea;
      color: #388e3c;
      border: 1px solid #66bb6a;
    }
    .message.error {
      background-color: #feebeb;
      color: #c62828;
      border: 1px solid #ef5350;
    }
  </style>
</head>
<body>
  <h1>Alimentador Automático</h1>

  <div class="status-indicator">
    <div class="led %STATUS_CLASS%"></div>
    <div id="currentTime"></div>
  </div>

  <div class="card">
    <button id="manualFeed">Alimentar Agora</button>
    <div id="manualFeedMessage" class="message"></div>
  </div>

  <form id="manualForm" class="card">
    <label>Duração do Pulso (HH:MM:SS)</label>
    <input type="time" id="manualInterval" step="1" value="%MANUAL%">
    <button type="submit">Salvar Duração</button>
    <div id="manualFormMessage" class="message"></div>
  </form>

  <div class="card">
    <form id="scheduleForm">
      <label>Novo Horário (HH:MM:SS)</label>
      <input type="time" id="newScheduleTime" step="1">
      <label>Duração (HH:MM:SS)</label>
      <input type="time" id="newScheduleInterval" step="1">
      <button type="button" id="addSchedule">+ Adicionar Agendamento</button>
    </form>
    <ul id="scheduleList"></ul>
    <button id="saveSchedules">Salvar Agendamentos</button>
    <div id="scheduleFormMessage" class="message"></div>
  </div>

  <div class="card">
    <label>Regras Avançadas</label>
    <textarea id="customRules" placeholder="Exemplo de regras:&#10;DH08:00 - Ligar diariamente às 8h&#10;DL20:00 - Desligar diariamente às 20h&#10;WH1 12:00 - Ligar toda segunda às 12h">%CUSTOM_RULES%</textarea>
    <button id="saveRules">Salvar Regras</button>
    <button id="toggleRules" class="secondary">%TOGGLE_BUTTON%</button>
    <div id="customRulesMessage" class="message"></div>
  </div>

  <div id="nextTrigger"></div>

<script>
  // Utility functions
  function pad(n) { return n.toString().padStart(2, '0'); }
  
  function parseHHMMSS(s) {
    const p = s.split(':').map(Number);
    return p[0] * 3600 + p[1] * 60 + p[2];
  }
  
  function formatHHMMSS(secs) {
    return [
      Math.floor(secs / 3600),
      Math.floor((secs % 3600) / 60),
      secs % 60
    ].map(pad).join(':');
  }

  // Update current time display
  function updateCurrentTime() {
    const now = new Date();
    document.getElementById('currentTime').textContent = now.toLocaleTimeString();
  }
  setInterval(updateCurrentTime, 1000);
  updateCurrentTime();

  // Load configuration
  let schedules = [];
  let manualIntervalSecs = parseHHMMSS("%MANUAL%");
  const rawSch = "%SCHEDULES%";
  
  if (rawSch) {
    schedules = rawSch.split(',').map(item => {
      const [t, i] = item.split('|');
      return { time: t, interval: i };
    });
  }

  // Render schedule list
  function renderSchedules() {
    const ul = document.getElementById('scheduleList');
    ul.innerHTML = '';
    
    schedules.forEach((o, i) => {
      const li = document.createElement('li');
      li.innerHTML = `
        <span>${o.time} (Duração: ${o.interval})</span>
        <button class="delete">×</button>
      `;
      li.querySelector('.delete').onclick = () => {
        schedules.splice(i, 1);
        renderSchedules();
        updateNextTrigger(); // Update next trigger time on deletion
      };
      ul.appendChild(li);
    });
  }
  renderSchedules();

  // Add new schedule
  document.getElementById('addSchedule').onclick = () => {
    const t = document.getElementById('newScheduleTime').value;
    const i = document.getElementById('newScheduleInterval').value;
    const timeRegex = /^([0-9]{2}):([0-9]{2}):([0-9]{2})$/;

    if (!timeRegex.test(t) || !timeRegex.test(i)) {
      showMessage('scheduleFormMessage', 'Por favor, use o formato HH:MM:SS', 'error');
      return;
    }

    if (t && i) {
      schedules.push({ time: t, interval: i });
      renderSchedules();
      updateNextTrigger(); // Update next trigger after adding
      document.getElementById('newScheduleTime').value = '';
      document.getElementById('newScheduleInterval').value = '';
      showMessage('scheduleFormMessage', 'Agendamento adicionado', 'success');
    } else {
      showMessage('scheduleFormMessage', 'Por favor, preencha horário e duração', 'error');
    }
  };

  // Save manual duration
  document.getElementById('manualForm').onsubmit = e => {
    e.preventDefault();
    const duration = document.getElementById('manualInterval').value;
    const timeRegex = /^([0-9]{2}):([0-9]{2}):([0-9]{2})$/;

    if (!timeRegex.test(duration)) {
      showMessage('manualFormMessage', 'Por favor, use o formato HH:MM:SS', 'error');
      return;
    }

    fetch('/setManualDuration', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `manualDuration=${duration}`
    })
    .then(response => {
      if (response.ok) {
        showMessage('manualFormMessage', 'Duração salva com sucesso!', 'success');
        manualIntervalSecs = parseHHMMSS(duration);
        updateNextTrigger();
      } else {
        showMessage('manualFormMessage', 'Falha ao salvar a duração', 'error');
        throw new Error('Falha ao salvar');
      }
    })
    .catch(error => {
      showMessage('manualFormMessage', 'Erro: ' + error.message, 'error');
    });
  };

  // Save schedules
  document.getElementById('saveSchedules').onclick = () => {
    if (schedules.length === 0) {
      showMessage('scheduleFormMessage', 'Nenhum agendamento para salvar', 'error');
      return;
    }
    const data = 'schedules=' + encodeURIComponent(
      schedules.map(o => `${o.time}|${o.interval}`).join(',')
    );
    
    fetch('/setSchedules', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: data
    })
    .then(response => {
      if (response.ok) {
        showMessage('scheduleFormMessage', 'Agendamentos salvos com sucesso!', 'success');
        updateNextTrigger();
      } else {
        showMessage('scheduleFormMessage', 'Falha ao salvar agendamentos', 'error');
        throw new Error('Falha ao salvar');
      }
    })
    .catch(error => {
      showMessage('scheduleFormMessage', 'Erro: ' + error.message, 'error');
    });
  };

  // Save custom rules
  document.getElementById('saveRules').onclick = () => {
    const rules = document.getElementById('customRules').value;
    
    fetch('/setCustomRules', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'rules=' + encodeURIComponent(rules)
    })
    .then(response => {
      if (response.ok) {
        showMessage('customRulesMessage', 'Regras salvas com sucesso!', 'success');
      } else {
        showMessage('customRulesMessage', 'Falha ao salvar regras', 'error');
        throw new Error('Falha ao salvar');
      }
    })
    .catch(error => {
      showMessage('customRulesMessage', 'Erro: ' + error.message, 'error');
    });
  };

  // Toggle custom rules
  document.getElementById('toggleRules').onclick = () => {
    fetch('/toggleCustomRules', { method: 'POST' })
      .then(response => {
        if (response.ok) {
          location.reload(); // Reload to update button text and status
        } else {
          showMessage('customRulesMessage', 'Falha ao alternar regras', 'error');
          throw new Error('Falha ao alternar regras');
        }
      })
      .catch(error => {
        showMessage('customRulesMessage', 'Erro: ' + error.message, 'error');
      });
  };

  // Manual feed
  document.getElementById('manualFeed').onclick = () => {
    fetch('/feedNow', { method: 'POST' })
      .then(response => {
        if (response.ok) {
          showMessage('manualFeedMessage', 'Alimentação iniciada!', 'success');
        } else {
          showMessage('manualFeedMessage', 'Aguarde o intervalo entre alimentações', 'error');
          throw new Error('Aguarde o intervalo entre alimentações');
        }
      })
      .catch(error => {
        showMessage('manualFeedMessage', 'Erro: ' + error.message, 'error');
      });
  };

  // Calculate next trigger time
  function updateNextTrigger() {
    const now = new Date();
    let next = null;
    
    // Check scheduled times
    schedules.forEach(o => {
      const [h, m, s] = o.time.split(':').map(Number);
      const d = new Date(now);
      d.setHours(h, m, s, 0);
      
      if (d <= now) d.setDate(d.getDate() + 1);
      if (!next || d < next) next = d;
    });
    
    // If no schedules, use manual interval
    if (!next && manualIntervalSecs > 0) {
      next = new Date(now.getTime() + manualIntervalSecs * 1000);
    }
    
    // Update display
    if (next) {
      const diff = next - now;
      const hrs = pad(Math.floor(diff / 3600000) % 24);
      const mins = pad(Math.floor((diff % 3600000) / 60000));
      const secs = pad(Math.floor((diff % 60000) / 1000));
      
      document.getElementById('nextTrigger').textContent = 
        `Próxima alimentação em: ${hrs}:${mins}:${secs}`;
    } else {
      document.getElementById('nextTrigger').textContent = 
        'Nenhum agendamento configurado';
    }
  }
  setInterval(updateNextTrigger, 1000);
  updateNextTrigger();

  function showMessage(elementId, message, type) {
    const element = document.getElementById(elementId);
    element.textContent = message;
    element.className = 'message ' + type;
    
    // Clear the message after a few seconds
    setTimeout(() => {
      element.textContent = '';
      element.className = 'message'; // Reset to default class
    }, 5000);
  }
</script>
</body>
</html>
)rawliteral";

// =============== Helper Functions =============== //
void formatHHMMSS(int secs, char* buf, size_t bufSize) {
  secs = abs(secs) % 86400;
  snprintf(buf, bufSize, "%02d:%02d:%02d", 
          secs / 3600, (secs % 3600) / 60, secs % 60);
}

String formatHHMMSS(int secs) {
  char buf[9];
  formatHHMMSS(secs, buf, sizeof(buf));
  return String(buf);
}

int parseHHMMSS(const String& s) {
  int h = 0, m = 0, sec = 0;
  if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3) {
    return (h >= 0 && h < 24 && m >= 0 && m < 60 && sec >= 0 && sec < 60) 
            ? h * 3600 + m * 60 + sec 
            : -1;
  }
  return -1;
}

int getCurrentDayOfYear() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  return timeinfo.tm_yday;
}

int getCurrentTimeInSec() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  return timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
}

String getCurrentDateTimeString() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

// =============== Configuration Management =============== //
void loadConfig() {
  Serial.println("Mounting filesystem...");
  
  #ifdef ESP8266
    if (!LittleFS.begin()) {
      Serial.println("Formatting LittleFS...");
      LittleFS.format();
      ESP.restart(); // Reboot after format
    }
  #else
    if (!SPIFFS.begin(true)) {
      Serial.println("SPIFFS mount failed, rebooting...");
      ESP.restart();
    }
  #endif

  if (!FS.exists(CONFIG_PATH)) {
    Serial.println("No config file found, using defaults");
    return;
  }

  File file = FS.open(CONFIG_PATH, "r");
  if (!file) {
    Serial.println("Failed to open config file");
    return;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print("Failed to parse config: ");
    Serial.println(error.c_str());
    return;
  }

  // Load configuration
  manualDurationSec = doc["manualDuration"] | manualDurationSec;
  scheduleCount = 0;

  JsonArray arr = doc["schedules"].as<JsonArray>();
  for (JsonObject o : arr) {
    if (scheduleCount >= MAX_SLOTS) break;
    schedules[scheduleCount] = {
      o["time"] | 0,
      o["duration"] | (int)manualDurationSec, // Use the parsed value
      -1
    };
    scheduleCount++;
  }

  strlcpy(customSchedule, doc["customSchedule"] | "", sizeof(customSchedule));
  customEnabled = doc["customEnabled"] | false;

  Serial.println("Configuration loaded:");
  Serial.printf("Manual duration: %d seconds\n", manualDurationSec);
  Serial.printf("Schedules count: %d\n", scheduleCount);
  Serial.printf("Custom rules: %s\n", customSchedule);
  Serial.printf("Custom rules enabled: %s\n", customEnabled ? "YES" : "NO");
}

void saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["manualDuration"] = manualDurationSec;

  JsonArray arr = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["time"] = schedules[i].timeSec;
    o["duration"] = schedules[i].durationSec;
  }

  doc["customSchedule"] = customSchedule;
  doc["customEnabled"] = customEnabled;

  File file = FS.open(CONFIG_PATH, "w");
  if (!file) {
    Serial.println("Failed to open config file for writing");
    return;
  }

  serializeJson(doc, file);
  file.close();
  Serial.println("Configuration saved");
}

// =============== Hardware Control =============== //
void setupHardware() {
  pinMode(FEEDER_PIN, OUTPUT);
  digitalWrite(FEEDER_PIN, LOW);
  
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH); // Active low on most boards
}

void updateStatusLED() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;

  if (isFeeding) {
    // Blink fast when feeding
    if (millis() - lastBlink > 200) {
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState);
      lastBlink = millis();
    }
  } else {
    // Solid on when connected, off when not
    digitalWrite(STATUS_LED_PIN, WiFi.status() == WL_CONNECTED ? LOW : HIGH);
  }
}

void emergencyStop() {
  digitalWrite(FEEDER_PIN, LOW);
  for (int i = 0; i < 5; i++) {
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(200);
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(200);
  }
  ESP.restart();
}

// =============== Network Setup =============== //
void setupNetwork() {
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(30);

  if (!wifiManager.autoConnect("PetFeederAP")) {
    Serial.println("Failed to connect to WiFi, rebooting...");
    emergencyStop();
  }

  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.print("Waiting for time sync...");
  
  for (int i = 0; i < 20; i++) {
    if (time(nullptr) > 1600000000) {
      Serial.println(" done!");
      Serial.print("Current time: ");
      Serial.println(getCurrentDateTimeString());
      return;
    }
    Serial.print(".");
    delay(500);
  }
  
  Serial.println(" failed!");
  emergencyStop();
}

// =============== Schedule Checking =============== //
void checkSchedules() {
  if (isFeeding || customEnabled) return;

  int today = getCurrentDayOfYear();
  int currentTime = getCurrentTimeInSec();

  for (int i = 0; i < scheduleCount; i++) {
    Schedule &s = schedules[i];
    if (currentTime == s.timeSec && s.lastTriggerDay != today) {
      if (millis() - lastTriggerMs >= FEED_COOLDOWN * 1000UL) {
        s.lastTriggerDay = today;
        isFeeding = true;
        feedStartMs = millis();
        feedDurationMs = min(s.durationSec, MAX_FEED_DURATION) * 1000UL;
        digitalWrite(FEEDER_PIN, HIGH);
        lastTriggerMs = millis();
        
        Serial.print("Triggering schedule #");
        Serial.print(i);
        Serial.print(" at ");
        Serial.print(formatHHMMSS(s.timeSec));
        Serial.print(" for ");
        Serial.print(s.durationSec);
        Serial.println(" seconds");
      }
    }
  }
}

String checkCustomRules() {
  if (!customEnabled || strlen(customSchedule) == 0) return "";

  static time_t lastCheck = 0;
  time_t now = time(nullptr);
  
  if (now - lastCheck < 10) return ""; // Check every 10 seconds
  lastCheck = now;

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char dt[17];
  snprintf(dt, sizeof(dt), "%04d-%02d-%02d %02d:%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min);

  String event = "";
  String sched(customSchedule);
  
  // Check for daily time triggers (DH = Daily High, DL = Daily Low)
  String hhmm = String(dt).substring(11); // Get "HH:MM" part
  if (sched.indexOf("DH" + hhmm) >= 0) {
    digitalWrite(FEEDER_PIN, HIGH);
    event = "DH" + hhmm;
  } else if (sched.indexOf("DL" + hhmm) >= 0) {
    digitalWrite(FEEDER_PIN, LOW);
    event = "DL" + hhmm;
  }
  
  if (!event.isEmpty()) {
    Serial.print("Custom rule triggered: ");
    Serial.println(event);
  }
  
  return event;
}

// =============== Web Server Handlers =============== //
void handleRoot() {
  String page = FPSTR(htmlPage);
  
  // Replace placeholders
  page.replace("%MANUAL%", formatHHMMSS(manualDurationSec));
  
  String scheduleList;
  for (int i = 0; i < scheduleCount; i++) {
    if (i > 0) scheduleList += ",";
    scheduleList += formatHHMMSS(schedules[i].timeSec);
    scheduleList += "|";
    scheduleList += formatHHMMSS(schedules[i].durationSec);
  }
  page.replace("%SCHEDULES%", scheduleList);
  
  page.replace("%CUSTOM_RULES%", customSchedule);
  page.replace("%TOGGLE_BUTTON%", customEnabled ? "Desativar Regras" : "Ativar Regras");
  page.replace("%STATUS_CLASS%", isFeeding ? "feeding" : (WiFi.status() == WL_CONNECTED ? "active" : ""));

  server.send(200, "text/html", page);
}

void handleFeedNow() {
  unsigned long now = millis();
  
  if (now - lastTriggerMs < FEED_COOLDOWN * 1000UL) {
    server.send(429, "text/plain", "Aguarde o intervalo entre alimentações");
    return;
  }

  isFeeding = true;
  feedStartMs = now;
  feedDurationMs = manualDurationSec * 1000UL;
  digitalWrite(FEEDER_PIN, HIGH);
  lastTriggerMs = now;
  
  Serial.print("Manual feed triggered for ");
  Serial.print(manualDurationSec);
  Serial.println(" seconds");
  
  server.send(200, "text/plain", "Alimentação iniciada");
}

void handleSetManualDuration() {
  if (server.hasArg("manualDuration")) {
    String duration = server.arg("manualDuration");
    int secs = parseHHMMSS(duration);
    
    if (secs > 0 && secs <= MAX_FEED_DURATION) {
      manualDurationSec = secs;
      saveConfig();
      server.send(200, "text/plain", "Duração salva");
      
      Serial.print("Manual duration set to ");
      Serial.print(manualDurationSec);
      Serial.println(" seconds");
    } else {
      server.send(400, "text/plain", "Duração inválida");
    }
  } else {
    server.send(400, "text/plain", "Parâmetro ausente");
  }
}

void handleSetSchedules() {
  if (server.hasArg("schedules")) {
    String scheduleStr = server.arg("schedules");
    scheduleCount = 0;
    
    if (scheduleStr.length() > 0) {
      int start = 0;
      while (start < scheduleStr.length() && scheduleCount < MAX_SLOTS) {
        int commaPos = scheduleStr.indexOf(',', start);
        if (commaPos < 0) commaPos = scheduleStr.length();
        
        String token = scheduleStr.substring(start, commaPos);
        int pipePos = token.indexOf('|');
        
        if (pipePos > 0) {
          int timeSec = parseHHMMSS(token.substring(0, pipePos));
          int durationSec = parseHHMMSS(token.substring(pipePos + 1));
          
          if (timeSec >= 0 && durationSec > 0 && durationSec <= MAX_FEED_DURATION) {
            schedules[scheduleCount] = {
              timeSec,
              durationSec,
              -1
            };
            scheduleCount++;
          }
        }
        
        start = commaPos + 1;
      }
    }
    
    saveConfig();
    server.send(200, "text/plain", "Agendamentos salvos");
    
    Serial.print("Saved ");
    Serial.print(scheduleCount);
    Serial.println(" schedules");
  } else {
    server.send(400, "text/plain", "Parâmetro ausente");
  }
}

void handleSetCustomRules() {
  if (server.hasArg("rules")) {
    String rules = server.arg("rules");
    strlcpy(customSchedule, rules.c_str(), sizeof(customSchedule));
    saveConfig();
    server.send(200, "text/plain", "Regras salvas");
    
    Serial.println("Custom rules updated:");
    Serial.println(customSchedule);
  } else {
    server.send(400, "text/plain", "Parâmetro ausente");
  }
}

void handleToggleCustomRules() {
  customEnabled = !customEnabled;
  saveConfig();
  server.send(200, "text/plain", customEnabled ? "Regras ativadas" : "Regras desativadas");
  
  Serial.print("Custom rules ");
  Serial.println(customEnabled ? "enabled" : "disabled");
}

// =============== Main Setup =============== //
void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting Pet Feeder...");
  
  setupHardware();
  loadConfig();
  setupNetwork();

  // Set up web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/feedNow", HTTP_POST, handleFeedNow);
  server.on("/setManualDuration", HTTP_POST, handleSetManualDuration);
  server.on("/setSchedules", HTTP_POST, handleSetSchedules);
  server.on("/setCustomRules", HTTP_POST, handleSetCustomRules);
  server.on("/toggleCustomRules", HTTP_POST, handleToggleCustomRules);
  
  server.begin();
  Serial.println("HTTP server started");
}

// =============== Main Loop =============== //
void loop() {
  server.handleClient();
  updateStatusLED();
  
  // Watchdog timer
  if (millis() - lastWatchdogReset > WATCHDOG_TIMEOUT) {
    Serial.println("Watchdog timeout, rebooting...");
    emergencyStop();
  }

  // Check for feeding completion
  if (isFeeding && millis() - feedStartMs >= feedDurationMs) {
    digitalWrite(FEEDER_PIN, LOW);
    isFeeding = false;
    Serial.println("Feeding completed");
  }

  // Check custom rules
  if (customEnabled && !isFeeding) {
    String event = checkCustomRules();
    if (!event.isEmpty()) {
      isFeeding = true;
      feedStartMs = millis();
      feedDurationMs = manualDurationSec * 1000UL;
      lastTriggerMs = millis();
    }
  }

  // Check scheduled feeds
  if (!isFeeding && !customEnabled) {
    checkSchedules();
  }

  lastWatchdogReset = millis();
  delay(10);
}