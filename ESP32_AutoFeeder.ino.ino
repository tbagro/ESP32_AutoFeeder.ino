#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

// ——————————————
// 1. Constantes e caminhos
// ——————————————
const char* WIFI_SSID       = "SUA_SSID";
const char* WIFI_PASSWORD   = "SUA_SENHA";

const long  GMT_OFFSET_SEC     = -4 * 3600;
const int   DAYLIGHT_OFFSET_SEC= 0;
const char* NTP_SERVER         = "pool.ntp.org";

const char* CONFIG_PATH     = "/config.json";
const int   MAX_SLOTS       = 10;
const int   FEEDER_PIN      = 2;

// ——————————————
// 2. Servidor e agendamento
// ——————————————
WebServer server(80);

unsigned long intervalSec        = 0;  // intervalo em s (0 = desabilitado)
unsigned long lastIntervalTrigger= 0;  // millis() do último disparo

int        scheduleSec[MAX_SLOTS];
int        scheduleCount         = 0;
int        lastTriggerDay[MAX_SLOTS];

// ——————————————
// 3. Protótipos
// ——————————————
void    loadConfig();
void    saveConfig();
int     parseHHMMSS(const String&);
String  formatHHMMSS(int);
int     getSecOfDay();
int     getDayOfYear();
void    handleRoot();
void    handleSetInterval();
void    handleSetSchedules();

// ——————————————
// 4. Implementações
// ——————————————
void loadConfig() {
  if (!SPIFFS.begin(true)) {
    Serial.println("Erro ao montar SPIFFS");
    return;
  }
  if (!SPIFFS.exists(CONFIG_PATH)) return;

  File file = SPIFFS.open(CONFIG_PATH, FILE_READ);
  if (!file) {
    Serial.println("Falha ao abrir config.json");
    return;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, file)) {
    Serial.println("JSON inválido em config.json");
    file.close();
    return;
  }
  file.close();

  intervalSec = doc["interval"] | 0;
  if (intervalSec < 1) intervalSec = 0;

  scheduleCount = 0;
  for (JsonVariant v : doc["schedules"].as<JsonArray>()) {
    if (scheduleCount >= MAX_SLOTS) break;
    scheduleSec[scheduleCount]   = v.as<int>();
    lastTriggerDay[scheduleCount]= -1;
    scheduleCount++;
  }

  Serial.printf("Config carregada: intervalo=%lus, slots=%d\n",
                intervalSec, scheduleCount);
}

void saveConfig() {
  DynamicJsonDocument doc(512);
  doc["interval"] = (int)intervalSec;
  JsonArray arr = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount; i++) {
    arr.add(scheduleSec[i]);
  }

  File file = SPIFFS.open(CONFIG_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("Falha ao abrir config.json para escrita");
    return;
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("Config salva em SPIFFS");
}

int parseHHMMSS(const String &s) {
  int h, m, sec;
  if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec)==3 &&
      h>=0&&h<24 && m>=0&&m<60 && sec>=0&&sec<60) {
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
  struct tm tm;
  localtime_r(&now, &tm);
  return tm.tm_hour*3600 + tm.tm_min*60 + tm.tm_sec;
}

int getDayOfYear() {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  return tm.tm_yday;
}

// HTML simplificado omitido por brevidade; assume-se idêntico ao seu
// Página principal
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1.0">
  <title>Alimentador Automático</title>
  <style>
    body { font-family: sans-serif; margin: 1rem; max-width: 600px; }
    h1 { text-align: center; }
    .card { background: #f9f9f9; padding: 1rem; margin-bottom: 1rem; border-radius: 5px; box-shadow: 0 2px 4px rgba(0,0,0,0.1);}
    label { display: block; margin-bottom: .5rem; font-weight: bold; }
    input[type="time"] { width: 100%; padding: .5rem; margin-bottom: .5rem; box-sizing: border-box; }
    button { padding: .5rem 1rem; margin-right: .5rem; }
    .alert { padding: .5rem 1rem; margin-bottom: 1rem; border-radius: 3px; }
    .alert.success { background: #e0f7e9; color: #2e7d32; }
    .alert.error   { background: #fdecea; color: #c62828; }
    #scheduleList { list-style: none; padding: 0;}
    #scheduleList li { display: flex; justify-content: space-between; align-items: center; margin-bottom: .25rem;}
    #currentTime, #nextTrigger { text-align: center; margin-bottom: .5rem; }
  </style>
</head>
<body>
  <h1>Configuração do Alimentador</h1>
  <div id="alert"></div>

  <div id="currentTime"></div>
  <div class="card">
    <form id="intervalForm">
      <label for="interval">Intervalo (HH:MM:SS):</label>
      <input type="time" id="interval" name="interval" step="1" required value="%INTERVAL%">
      <button type="submit">Salvar Intervalo</button>
    </form>
  </div>

  <div class="card">
    <form id="scheduleForm">
      <label for="newSchedule">Adicionar horário (HH:MM:SS):</label>
      <input type="time" id="newSchedule" step="1">
      <button type="button" id="addSchedule">+ Adicionar</button>
    </form>
    <h2>Alimentação Manual</h2>
+   <button id="manualFeed">Alimentar Agora</button>
    <ul id="scheduleList"></ul>
    <button id="saveSchedules">Salvar Horários</button>
  </div>

  <div id="nextTrigger"></div>

<script>
  // Config carregada pelo handleRoot()
  const config = {
    interval: "%INTERVAL%",
    schedules: ["%SCHEDULES%"].filter(v => v && v !== "nenhum")
  };

  // Atualiza relógio
  function updateCurrentTime() {
    const now = new Date();
    document.getElementById('currentTime')
      .textContent = 'Hora atual: ' + now.toLocaleTimeString();
  }
  setInterval(updateCurrentTime, 1000);
  updateCurrentTime();

  // Renderiza lista de horários
  const listEl = document.getElementById('scheduleList');
  function renderList() {
    listEl.innerHTML = '';
    config.schedules.forEach((t, i) => {
      const li = document.createElement('li');
      li.textContent = t;
      const btn = document.createElement('button');
      btn.textContent = '–';
      btn.onclick = () => { config.schedules.splice(i, 1); renderList(); };
      li.appendChild(btn);
      listEl.appendChild(li);
    });
  }
  renderList();

  // Botão “Adicionar”
  document.getElementById('addSchedule').onclick = () => {
    const time = document.getElementById('newSchedule').value;
    if (time) {
      config.schedules.push(time);
      renderList();
      document.getElementById('newSchedule').value = '';
    }
  };

  // Função de alerta
  function showAlert(msg, type) {
    const a = document.getElementById('alert');
    a.textContent = msg;
    a.className = 'alert ' + type;
    setTimeout(() => { a.textContent = ''; a.className = ''; }, 3000);
  }

  // Submissões AJAX
  document.getElementById('intervalForm').onsubmit = e => {
    e.preventDefault();
    fetch('/setInterval', {
      method: 'POST',
      body: new URLSearchParams(new FormData(e.target))
    })
    .then(() => showAlert('Intervalo salvo com sucesso', 'success'))
    .catch(() => showAlert('Erro ao salvar intervalo', 'error'));
  };

  document.getElementById('saveSchedules').onclick = () => {
    const payload = 'schedules=' + encodeURIComponent(config.schedules.join(','));
    fetch('/setSchedules', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: payload
    })
    .then(() => showAlert('Horários salvos com sucesso', 'success'))
    .catch(() => showAlert('Erro ao salvar horários', 'error'));
  };

  // Alimentação manual
document.getElementById('manualFeed').onclick = () => {
  fetch('/feedNow', { method: 'POST' })
    .then(() => showAlert('Alimentação manual acionada', 'success'))
    .catch(() => showAlert('Falha na alimentação manual', 'error'));
};


  // Contador regressivo para próximo acionamento
  function updateNextTrigger() {
    const now = new Date();
    let next = null;

    // Próximos horários fixos
    config.schedules.forEach(t => {
      const [h, m, s] = t.split(':').map(n => +n);
      const d = new Date(now);
      d.setHours(h, m, s);
      if (d <= now) d.setDate(d.getDate() + 1);
      if (!next || d < next) next = d;
    });

    // Intervalo relativo
    if (config.interval && config.interval !== 'desabilitado') {
      const [h, m, s] = config.interval.split(':').map(n => +n);
      const ms = ((h*3600)+(m*60)+s)*1000;
      const d = new Date(now.getTime() + ms);
      if (!next || d < next) next = d;
    }

    if (next) {
      const diff = next - now;
      const hrs = String(Math.floor(diff/3600000)).padStart(2,'0');
      const mins = String(Math.floor((diff%3600000)/60000)).padStart(2,'0');
      const secs = String(Math.floor((diff%60000)/1000)).padStart(2,'0');
      document.getElementById('nextTrigger')
        .textContent = 'Próximo acionamento em: ' + hrs + ':' + mins + ':' + secs;
    }
  }
  setInterval(updateNextTrigger, 1000);
  updateNextTrigger();
</script>
</body>
</html>
)rawliteral";


void handleRoot() {
  // Recupera o template da PROGMEM
  String page = FPSTR(htmlPage);

  // Formata o intervalo atual ou "desabilitado"
  String intVal = (intervalSec > 0)
    ? formatHHMMSS(intervalSec)
    : String("desabilitado");

  // Constrói a lista de horários agendados ou "nenhum"
  String schedVal;
  for (int i = 0; i < scheduleCount; ++i) {
    if (i) schedVal += ", ";
    schedVal += formatHHMMSS(scheduleSec[i]);
  }
  if (schedVal.length() == 0) {
    schedVal = "nenhum";
  }

  // Substitui os marcadores no HTML
  page.replace("%INTERVAL%", intVal);
  page.replace("%SCHEDULES%", schedVal);

  // Envia resposta HTTP
  server.send(200, "text/html; charset=UTF-8", page);
}

void handleSetInterval() {
  if (server.hasArg("interval")) {
    int secs = parseHHMMSS(server.arg("interval"));
    intervalSec = (secs < 1 ? 1 : secs);
    lastIntervalTrigger = millis();
    saveConfig();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
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
      token.trim();
      int secs = parseHHMMSS(token);
      if (secs >= 0) {
        scheduleSec[scheduleCount]    = secs;
        lastTriggerDay[scheduleCount] = -1;
        scheduleCount++;
      }
      start = comma + 1;
    }
    saveConfig();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void setup(){
  Serial.begin(115200);
  pinMode(FEEDER_PIN, OUTPUT);
  digitalWrite(FEEDER_PIN, LOW);

  loadConfig();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi: " + WiFi.localIP().toString());
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/setInterval", HTTP_POST, handleSetInterval);
  server.on("/setSchedules", HTTP_POST, handleSetSchedules);
  // Rota para disparo manual
  server.on("/feedNow", HTTP_POST, [](){
  // aciona o dosador
    digitalWrite(FEEDER_PIN, HIGH);
    delay(500);                     // pulso de 500 ms (ajuste se necessário)
    digitalWrite(FEEDER_PIN, LOW);
    Serial.println("Disparo manual acionado");
    server.send(200, "text/plain", "OK");
  });

  server.begin();
  Serial.println("HTTP server iniciado em /");
}

void loop(){
  server.handleClient();

  unsigned long nowMillis = millis();
  int today    = getDayOfYear();
  int secOfDay = getSecOfDay();

  // Intervalo
  if (intervalSec > 0 &&
      nowMillis - lastIntervalTrigger >= intervalSec * 1000UL) {
    lastIntervalTrigger = nowMillis;
    digitalWrite(FEEDER_PIN, HIGH);
    delay(500);
    digitalWrite(FEEDER_PIN, LOW);
    Serial.println("Disparo por intervalo: " + formatHHMMSS(secOfDay));
  }

  // Horários fixos
  for (int i = 0; i < scheduleCount; i++) {
    if (secOfDay == scheduleSec[i] && lastTriggerDay[i] != today) {
      lastTriggerDay[i] = today;
      digitalWrite(FEEDER_PIN, HIGH);
      delay(500);
      digitalWrite(FEEDER_PIN, LOW);
      Serial.println("Disparo agendado: " + formatHHMMSS(secOfDay));
    }
  }

  delay(200);
}

