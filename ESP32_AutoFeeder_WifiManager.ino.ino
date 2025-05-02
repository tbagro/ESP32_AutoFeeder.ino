#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <SPIFFS.h>
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
const int   FEEDER_PIN          = 18;

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
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1.0">
  <title>Alimentador Automático</title>
  <style>
    body{font-family:sans-serif;margin:1rem;max-width:600px;}
    h1{text-align:center;}
    .card{background:#f9f9f9;padding:1rem;margin:1rem 0;border-radius:5px;}
    button{padding:.5rem 1rem;margin:0.5rem 0;}
    button:disabled{opacity:0.5;}
    input[type=time]{width:100%;padding:.5rem;margin:.5rem 0;}
    #currentTime,#nextTrigger{text-align:center;font-weight:bold;}
  </style>
</head>
<body>
  <h1>Configuração do Alimentador</h1>
  <div id="currentTime"></div>
  <div class="card">
    <button id="manualFeed">Alimentar Agora</button>
  </div>
  <div class="card">
    <form id="manualForm">
      <label>Pulso manual (HH:MM:SS):</label>
      <input type="time" id="manualDuration" step="1" value="%MANUAL%">
      <button type="submit">Salvar Pulso</button>
    </form>
  </div>
  <div class="card">
    <form id="scheduleForm">
      <label>Novo horário</label>
      <input type="time" id="newScheduleTime" step="1">
      <label>Duração (HH:MM:SS)</label>
      <input type="time" id="newScheduleDuration" step="1">
      <button type="button" id="addSchedule">+ Adicionar</button>
    </form>
    <ul id="scheduleList"></ul>
    <button id="saveSchedules">Salvar Agendamentos</button>
  </div>
  <div id="nextTrigger"></div>

<script>
  // Utilitários
  function pad(n){return n.toString().padStart(2,'0');}
  function parseHHMMSS(s){
    const p=s.split(':').map(Number);
    return p[0]*3600 + p[1]*60 + p[2];
  }
  function formatHHMMSS(secs){
    return [Math.floor(secs/3600),Math.floor(secs%3600/60),secs%60]
      .map(pad).join(':');
  }

  // Estado
  let manualDurationSecs;
  let schedules = [];
  let manualBtnEnabled = true;

  // Atualiza hora atual
  function updateCurrentTime(){
    document.getElementById('currentTime').textContent =
      'Hora: ' + new Date().toLocaleTimeString();
  }
  setInterval(updateCurrentTime,1000);
  updateCurrentTime();

  // Próximo acionamento
  function updateNextTrigger(){
    const now=new Date(); let next=null;
    schedules.forEach(o=>{
      const [h,m,s]=o.time.split(':').map(Number);
      const d=new Date(now); d.setHours(h,m,s,0);
      if(d<=now) d.setDate(d.getDate()+1);
      if(!next||d<next) next=d;
    });
    if(!next){ // fallback pelo pulso manual
      next=new Date(now.getTime()+manualDurationSecs*1000);
    }
    const diff=next-now;
    document.getElementById('nextTrigger').textContent =
      'Próximo: ' + formatHHMMSS(
        Math.floor(diff/3600000)*3600 +
        Math.floor((diff%3600000)/60000)*60 +
        Math.floor((diff%60000)/1000)
      );
  }
  setInterval(updateNextTrigger,1000);

  // Carrega config (substituído pelo handleRoot)
  const rawManual="%MANUAL%";
  manualDurationSecs=parseHHMMSS(rawManual);
  document.getElementById('manualDuration').value=rawManual;
  const rawSched="%SCHEDULES%";
  if(rawSched) rawSched.split(',').forEach(item=>{
    const [t,d]=item.split('|'); schedules.push({time:t,duration:d});
  });

  // Renderiza lista
  function renderList(){
    const ul=document.getElementById('scheduleList'); ul.innerHTML='';
    schedules.forEach((o,i)=>{
      const li=document.createElement('li');
      li.textContent=`${o.time} (Duração: ${o.duration})`;
      const btn=document.createElement('button'); btn.textContent='–';
      btn.onclick=()=>{ schedules.splice(i,1); renderList(); };
      li.appendChild(btn);
      ul.appendChild(li);
    });
  }
  renderList();

  // Feed manual (não bloqueante)
  document.getElementById('manualFeed').onclick = ()=>{
    if(!manualBtnEnabled) return;
    manualBtnEnabled=false;
    const btn=document.getElementById('manualFeed');
    btn.disabled=true;
    fetch('/feedNow',{method:'POST'})
      .then(r=>{
        setTimeout(()=>{ btn.disabled=false; manualBtnEnabled=true; }, manualDurationSecs*1000);
      });
  };

  // Submissões
  document.getElementById('manualForm').onsubmit=e=>{
    e.preventDefault();
    const v=document.getElementById('manualDuration').value;
    fetch('/setManualDuration',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:`manualDuration=${v}`
    }).then(()=>{ manualDurationSecs=parseHHMMSS(v); });
  };
  document.getElementById('addSchedule').onclick=()=>{
    const t=document.getElementById('newScheduleTime').value;
    const d=document.getElementById('newScheduleDuration').value;
    if(t&&d){ schedules.push({time:t,duration:d}); renderList(); }
  };
  document.getElementById('saveSchedules').onclick=()=>{
    const payload=schedules.map(o=>`${o.time}|${o.duration}`).join(',');
    fetch('/setSchedules',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:`schedules=${encodeURIComponent(payload)}`
    });
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
