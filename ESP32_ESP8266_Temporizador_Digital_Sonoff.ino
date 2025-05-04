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

#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <time.h>
#include <TimeLib.h>
#include <WiFiUdp.h>



// Se estiver usando PlatformIO com board sonoff_basic:
#if defined(ARDUINO_ESP8266_ESP12E)
  #ifndef SONOFF_BASIC
    #define SONOFF_BASIC
  #endif
#endif

// =============== Configuration =============== //
const char *CONFIG_PATH = "/config.json";
const int MAX_SLOTS = 10;
const long GMT_OFFSET_SEC = -4 * 3600;  // UTC-4 (Cuiabá)
const int DAYLIGHT_OFFSET_SEC = 0;
const char *NTP_SERVER = "pool.ntp.org";
const int FEED_COOLDOWN = 10;         // Seconds between feedings
const int MAX_FEED_DURATION = 300;    // 5 minutes max
const int WATCHDOG_TIMEOUT = 120000;  // 2 minutes

// Descomente para Sonoff Basic no Arduino IDE
#define SONOFF_BASIC
#ifdef SONOFF_BASIC
  // Sonoff Basic
  const int FEEDER_PIN     = 12; // relé interno do Sonoff (GPIO12)
  const int STATUS_LED_PIN = 13; // LED verde onboard (GPIO13)
  const int BUTTON_PIN     = 0;  // botão de flash/config (GPIO0)
  bool    lastButtonState = HIGH;
  uint32_t lastDebounceMs = 0;
  const uint32_t DEBOUNCE_DELAY = 50;  // ms
#elif defined(ESP8266)
  // qualquer outro módulo ESP8266
  const int FEEDER_PIN     = 14; // D5 on ESP8266
  const int STATUS_LED_PIN = LED_BUILTIN;
  const int BUTTON_PIN     = -1; // não usado
#else
  // ESP32 genérico
  const int FEEDER_PIN     = 18; // GPIO18 on ESP32
  const int STATUS_LED_PIN = 2;
  const int BUTTON_PIN     = -1; // não usado
#endif



// =============== Data Structures =============== //
struct Schedule {
  int timeSec;         // Scheduled time in seconds since midnight
  int durationSec;     // Feeding duration in seconds
  int lastTriggerDay;  // Last day this schedule was triggered (0-365)
};

// =============== Global State =============== //

WebServer server(80);
WiFiManager wifiManager;
WiFiUDP udp;

Schedule schedules[MAX_SLOTS];
int scheduleCount = 0;
unsigned long manualDurationSec = 5;
char customSchedule[512] = "";
// --- Estado geral de alimentação ---
bool customEnabled = false;
volatile bool isFeeding = false;
unsigned long feedStartMs = 0;
unsigned long feedDurationMs = 0;
unsigned long lastTriggerMs = 0;
unsigned long lastWatchdogReset = 0;

time_t timeNTP();
String currentSchedule = "";
String eventLog = "";
String hhmmStr(const time_t &t);
String scheduleChk(const String &schedule, const byte &pin);
String scheduleGet();
bool scheduleSet(const String &schedule);

// adicione esta função para formatar hh:mm:ss
String timeStr(const time_t &t) {
  String s;
  if (hour(t) < 10) s += '0';
  s += String(hour(t)) + ':';
  if (minute(t) < 10) s += '0';
  s += String(minute(t)) + ':';
  if (second(t) < 10) s += '0';
  s += String(second(t));
  return s;
}


// =============== HTML Page =============== //
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1.0">
  <title>Alimentador Automático</title>
  <style>
    body { font-family: system-ui, -apple-system, sans-serif; margin:0 auto; padding:20px; max-width:600px; background:#f5f5f5; }
    h1 { text-align:center; color:#333; margin-bottom:30px; }
    .card { background:#fff; padding:20px; margin-bottom:20px; border-radius:8px; box-shadow:0 2px 4px rgba(0,0,0,0.1); }
    label { display:block; margin-bottom:8px; color:#555; font-weight:500; }
    input[type=time], input[type=text] { width:100%; padding:10px; border:1px solid #ddd; border-radius:4px; margin-bottom:10px; }
    button { width:100%; padding:12px; background:#2e7d32; color:#fff; border:none; border-radius:4px; font-weight:500; cursor:pointer; margin-bottom:10px; }
    button:hover { background:#256427; }
    button.secondary { background:#666; }
    button.secondary:hover { background:#555; }
    ul { list-style:none; padding:0; }
    li { display:flex; justify-content:space-between; align-items:center; padding:8px; background:#f8f8f8; border-radius:4px; margin-bottom:8px; }
    #currentTime { text-align:center; margin:20px 0; font-size:1.2em; color:#666; }
    #nextTrigger { text-align:center; margin:20px 0; padding:10px; background:#e8f5e9; border-radius:4px; color:#2e7d32; }
    .status-indicator { display:flex; align-items:center; justify-content:center; gap:10px; margin-bottom:20px; }
    .led { width:15px; height:15px; border-radius:50%; background:#ccc; }
    .led.active { background:#4caf50; box-shadow:0 0 10px #4caf50; }
    .led.feeding { background:#ff9800; box-shadow:0 0 10px #ff9800; animation:pulse 1s infinite; }
    @keyframes pulse { 0%{opacity:1;}50%{opacity:0.5;}100%{opacity:1;} }
    #console { background:#111; color:#0f0; font-family:monospace; white-space:pre; padding:10px; height:120px; overflow-y:auto; border:1px solid #444; border-radius:4px; }
    #console:focus { outline:none; }
    .message { margin-top:10px; padding:10px; border-radius:4px; text-align:center; font-weight:bold; }
    .message.success { background:#e6f4ea; color:#388e3c; border:1px solid #66bb6a; }
    .message.error { background:#feebeb; color:#c62828; border:1px solid #ef5350; }
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
    <label>Regras Avançadas (console)</label>
    <div id="console" contenteditable="true">%CUSTOM_RULES%</div>
    <button id="saveRules">Salvar Regras</button>
    <button id="toggleRules" class="secondary">%TOGGLE_BUTTON%</button>
    <div id="customRulesMessage" class="message"></div>
  </div>

  <div id="nextTrigger"></div>

<script>
  function pad(n){ return n.toString().padStart(2,'0'); }
  function parseHHMMSS(s){ const p=s.split(':').map(Number); return p[0]*3600+p[1]*60+p[2]; }
  function formatHHMMSS(secs){ return [Math.floor(secs/3600),Math.floor((secs%3600)/60),secs%60].map(pad).join(':'); }

  function updateCurrentTime(){
    document.getElementById('currentTime').textContent = new Date().toLocaleTimeString();
  }
  setInterval(updateCurrentTime,1000);
  updateCurrentTime();

  let schedules = [], manualIntervalSecs = parseHHMMSS("%MANUAL%");
  const rawSch = "%SCHEDULES%";
  if(rawSch) schedules = rawSch.split(',').map(t=>{const a=t.split('|');return{time:a[0],interval:a[1]};});
  function renderSchedules(){
    const ul=document.getElementById('scheduleList'); ul.innerHTML='';
    schedules.forEach((o,i)=>{
      const li=document.createElement('li');
      li.innerHTML=`<span>${o.time} (Duração: ${o.interval})</span><button class="delete">×</button>`;
      li.querySelector('.delete').onclick=()=>{schedules.splice(i,1);renderSchedules();updateNextTrigger();};
      ul.appendChild(li);
    });
  }
  renderSchedules();

  document.getElementById('addSchedule').onclick=()=>{
    const t=document.getElementById('newScheduleTime').value, i=document.getElementById('newScheduleInterval').value;
    const rx=/^([0-9]{2}):([0-9]{2}):([0-9]{2})$/;
    if(!rx.test(t)||!rx.test(i)){showMessage('scheduleFormMessage','Use formato HH:MM:SS','error');return;}
    schedules.push({time:t,interval:i});renderSchedules();updateNextTrigger();
    document.getElementById('newScheduleTime').value='';document.getElementById('newScheduleInterval').value='';
    showMessage('scheduleFormMessage','Agendamento adicionado','success');
  };

  document.getElementById('manualForm').onsubmit=e=>{
    e.preventDefault();
    const d=document.getElementById('manualInterval').value, rx=/^([0-9]{2}):([0-9]{2}):([0-9]{2})$/;
    if(!rx.test(d)){showMessage('manualFormMessage','Use formato HH:MM:SS','error');return;}
    fetch('/setManualDuration',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`manualDuration=${d}`})
      .then(r=>{if(r.ok){showMessage('manualFormMessage','Duração salva','success');manualIntervalSecs=parseHHMMSS(d);updateNextTrigger();} else throw '';})
      .catch(_=>showMessage('manualFormMessage','Erro ao salvar','error'));
  };

  document.getElementById('saveSchedules').onclick=()=>{
    if(!schedules.length){showMessage('scheduleFormMessage','Nenhum agendamento','error');return;}
    const body='schedules='+encodeURIComponent(schedules.map(o=>`${o.time}|${o.interval}`).join(','));
    fetch('/setSchedules',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body})
      .then(r=>{if(r.ok){showMessage('scheduleFormMessage','Agendamentos salvos','success');updateNextTrigger();} else throw '';})
      .catch(_=>showMessage('scheduleFormMessage','Erro ao salvar','error'));
  };

  document.getElementById('saveRules').onclick=()=>{
    const rules=document.getElementById('console').innerText;
    fetch('/setCustomRules',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'rules='+encodeURIComponent(rules)})
      .then(r=>{if(r.ok)showMessage('customRulesMessage','Regras salvas','success');else throw '';})
      .catch(_=>showMessage('customRulesMessage','Erro ao salvar','error'));
  };

  document.getElementById('toggleRules').onclick=()=>{
    fetch('/toggleCustomRules',{method:'POST'}).then(r=>{if(r.ok)location.reload();else throw '';}).catch(_=>showMessage('customRulesMessage','Erro ao alternar','error'));
  };

  document.getElementById('manualFeed').onclick=()=>{
    fetch('/feedNow',{method:'POST'}).then(r=>{if(r.ok)showMessage('manualFeedMessage','Alimentação iniciada','success');else showMessage('manualFeedMessage','Aguarde intervalo','error');})
      .catch(_=>showMessage('manualFeedMessage','Erro ao alimentar','error'));
  };

  function updateNextTrigger(){
    const now=new Date(),pad=(n)=>n.toString().padStart(2,'0');
    let next=null;
    schedules.forEach(o=>{
      const [h,m,s]=o.time.split(':').map(Number),d=new Date(now);
      d.setHours(h,m,s,0); if(d<=now)d.setDate(d.getDate()+1); if(!next||d<next)next=d;
    });
    if(!next&&manualIntervalSecs>0)next=new Date(now.getTime()+manualIntervalSecs*1000);
    document.getElementById('nextTrigger').textContent = next
      ? `Próxima alimentação em: ${pad(Math.floor((next-now)/3600000)%24)}:${pad(Math.floor((next-now)%3600000/60000))}:${pad(Math.floor((next-now)%60000/1000))}`
      : 'Nenhum agendamento configurado';
  }
  setInterval(updateNextTrigger,1000);
  updateNextTrigger();

  function showMessage(id,msg,type){
    const e=document.getElementById(id);e.textContent=msg;e.className='message '+type;
    setTimeout(()=>{e.textContent='';e.className='message';},5000);
  }
    setInterval(() => {
    fetch('/events')
      .then(res => res.text())
      .then(txt => {
        if (txt) {
          const con = document.getElementById('console');
          con.innerText += (con.innerText ? '\n' : '') + txt;
          con.scrollTop = con.scrollHeight;
        }
      });
  }, 1000);
</script>
</body>
</html>
)rawliteral";


/******************************************************************************* 
* FUNÇÕES AUXILIARES DE NTP / TIME                                             
*******************************************************************************/
time_t timeNTP() {
  byte pkt[48] = { 0 };
  pkt[0] = 0b11100011;
  udp.begin(2390);
  udp.beginPacket("pool.ntp.br", 123);
  udp.write(pkt, 48);
  udp.endPacket();
  delay(500);
  if (udp.parsePacket()) {
    udp.read(pkt, 48);
    unsigned long secs = word(pkt[40], pkt[41]) << 16 | word(pkt[42], pkt[43]);
    secs -= 2208988800UL;
    secs += -4 * 3600;
    return secs;
  }
  return 0;
}

/******************************************************************************* 
* SCHEDULE CORE                                                                
*******************************************************************************/


String hhmmStr(const time_t &t) {
  String s;
  if (hour(t) < 10) s += '0';
  s += String(hour(t)) + ':';
  if (minute(t) < 10) s += '0';
  s += String(minute(t));
  return s;
}

void formatHHMMSS(int secs, char *buf, size_t bufSize) {
  secs = abs(secs) % 86400;
  snprintf(buf, bufSize, "%02d:%02d:%02d",
           secs / 3600, (secs % 3600) / 60, secs % 60);
}

String formatHHMMSS(int secs) {
  char buf[9];
  formatHHMMSS(secs, buf, sizeof(buf));
  return String(buf);
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

// Converte string “yyyy-mm-dd hh:mm” em time_t (segundos desde epoch)
time_t parseDateTime(const String &s) {
  struct tm tm;
  // Ex.: “2018-10-12 16:30”
  if (sscanf(s.c_str(), " %4d-%2d-%2d %2d:%2d",
             &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
             &tm.tm_hour, &tm.tm_min)
      == 5) {
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
  }
  return (time_t)0;
}

int parseHHMMSS(const String &s) {
  int h = 0, m = 0, sec = 0;
  if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3) {
    return (h >= 0 && h < 24 && m >= 0 && m < 60 && sec >= 0 && sec < 60)
             ? h * 3600 + m * 60 + sec
             : -1;
  }
  return -1;
}
String scheduleChk(const String &schedule, const byte &pin) {
  String event = "";
  byte relay;
  static time_t lastCheck = 0, highDT = now(), lowDT = now();
  if (schedule == "") {
    highDT = now();
    lowDT = now();
    return "";
  }
  if (now() - lastCheck < 10) return "";
  lastCheck = now();

  String dt = String(year(lastCheck)) + '-' + (month(lastCheck) < 10 ? "0" : "") + String(month(lastCheck)) + '-' + (day(lastCheck) < 10 ? "0" : "") + String(day(lastCheck)) + ' ' + (hour(lastCheck) < 10 ? "0" : "") + String(hour(lastCheck)) + ':' + (minute(lastCheck) < 10 ? "0" : "") + String(minute(lastCheck));

  String s = "SH" + dt;
  if (schedule.indexOf(s) != -1) {
    event = s;
    relay = HIGH;
    goto P;
  }
  s = "SL" + dt;
  if (schedule.indexOf(s) != -1) {
    event = s;
    relay = LOW;
    goto P;
  }

  dt = dt.substring(8);
  s = "MH" + dt;
  if (schedule.indexOf(s) != -1) {
    event = s;
    relay = HIGH;
    goto P;
  }
  s = "ML" + dt;
  if (schedule.indexOf(s) != -1) {
    event = s;
    relay = LOW;
    goto P;
  }

  dt = String(weekday(lastCheck)) + dt.substring(2);
  s = "WH" + dt;
  if (schedule.indexOf(s) != -1) {
    event = s;
    relay = HIGH;
    goto P;
  }
  s = "WL" + dt;
  if (schedule.indexOf(s) != -1) {
    event = s;
    relay = LOW;
    goto P;
  }

  dt = dt.substring(2);
  s = "DH" + dt;
  if (schedule.indexOf(s) != -1) {
    event = s;
    relay = HIGH;
    goto P;
  }
  s = "DL" + dt;
  if (schedule.indexOf(s) != -1) {
    event = s;
    relay = LOW;
    goto P;
  }

  s = "IH" + hhmmStr(lastCheck - highDT);
  if (schedule.indexOf(s) != -1 && digitalRead(pin)) {
    event = s;
    relay = LOW;
    goto P;
  }
  s = "IL" + hhmmStr(lastCheck - lowDT);
  if (schedule.indexOf(s) != -1 && !digitalRead(pin)) {
    event = s;
    relay = HIGH;
  }

P:
  if (event != "" && relay != digitalRead(pin)) {
    digitalWrite(pin, relay);
    if (relay) highDT = lastCheck;
    else lowDT = lastCheck;
    return event;
  }
  return "";
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

  // 1) Inicia o pulso
  isFeeding      = true;
  feedStartMs    = now;
  feedDurationMs = manualDurationSec * 1000UL;
  digitalWrite(FEEDER_PIN, HIGH);
  lastTriggerMs  = now;

  // 2) Prepara timestamp “HH:MM:SS”
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char ts[9];
  snprintf(ts, sizeof(ts), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

  // 3) Loga o evento no Serial
  Serial.print(ts);
  Serial.print(" -> Manual feed for ");
  Serial.print(manualDurationSec);
  Serial.println(" seconds");

  // 4) Armazena no buffer para o console web
  eventLog += String(ts) + " -> Manual feed\n";

  // 5) Responde ao cliente HTTP
  server.send(200, "text/plain", "Alimentação iniciada");
}


void handleSetManualDuration() {
  if (! server.hasArg("manualDuration")) {
    server.send(400, "text/plain", "Parâmetro ausente");
    return;
  }

  String duration = server.arg("manualDuration");
  int secs = parseHHMMSS(duration);

  if (secs <= 0 || secs > MAX_FEED_DURATION) {
    server.send(400, "text/plain", "Duração inválida");
    return;
  }

  // 1) Atualiza e persiste
  manualDurationSec = secs;
  saveConfig();

  // 2) Prepara carimbo de hora
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char ts[9];
  snprintf(ts, sizeof(ts), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

  // 3) Registra evento para console web e Serial
  String ev = String(ts) + " -> ManualDuration set to " + duration;
  eventLog += ev + "\n";
  Serial.println(ev);

  // 4) Resposta HTTP
  server.send(200, "text/plain", "Duração salva");
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
  // Verifica se veio o parâmetro "rules" no corpo da requisição
  if (!server.hasArg("rules")) {
    server.send(400, "text/plain", "Parâmetro 'rules' ausente");
    return;
  }

  // Lê as novas regras do console
  String newRules = server.arg("rules");

  // Garante que caiba no buffer
  if (newRules.length() >= sizeof(customSchedule)) {
    server.send(400, "text/plain", "Regras muito longas");
    return;
  }

  // Atualiza a variável em RAM
  newRules.toCharArray(customSchedule, sizeof(customSchedule));

  // Persiste todo o estado (manualDuration, schedules, customSchedule, customEnabled)
  saveConfig();

  // Reseta o estado interno do scheduler (limpa contadores de IH/IL)
  scheduleChk("", FEEDER_PIN);

  // Responde sucesso
  server.send(200, "text/plain", "Regras salvas");
  Serial.println("Custom rules updated");
}

void handleToggleCustomRules() {
  customEnabled = !customEnabled;
  saveConfig();
  server.send(200, "text/plain", customEnabled ? "Regras ativadas" : "Regras desativadas");

  Serial.print("Custom rules ");
  Serial.println(customEnabled ? "enabled" : "disabled");
}

// =============== Configuration Management =============== //

void loadConfig() {
  Serial.println("Mounting filesystem...");

#ifdef ESP8266
  if (!LittleFS.begin()) {
  Serial.println("LittleFS mount failed, formatting…");
    if (LittleFS.format()) {
      Serial.println("Format succeeded, mounting again…");
      if (!LittleFS.begin()) {
        Serial.println("Still failed after format!");
        // aqui você pode optar por prosseguir sem FS ou entrar em modo de erro
      }
    } else {
      Serial.println("Format failed!");
    }
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

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  // Manual duration
  manualDurationSec = doc["manualDuration"] | manualDurationSec;

  // Custom rules
  const char *rules = doc["customSchedule"] | "";
  strncpy(customSchedule, rules, sizeof(customSchedule));
  customEnabled = doc["customEnabled"] | customEnabled;

  // Schedules array
  scheduleCount = 0;
  if (doc.containsKey("schedules")) {
    for (JsonObject o : doc["schedules"].as<JsonArray>()) {
      if (scheduleCount >= MAX_SLOTS) break;
      schedules[scheduleCount].timeSec = o["time"] | schedules[scheduleCount].timeSec;
      schedules[scheduleCount].durationSec = o["duration"] | schedules[scheduleCount].durationSec;
      schedules[scheduleCount].lastTriggerDay = -1;
      scheduleCount++;
    }
  }

  // Debug print
  Serial.println("Configuration loaded:");
  Serial.printf(" • Manual duration: %lu s\n", manualDurationSec);
  Serial.printf(" • %d schedules:\n", scheduleCount);
  for (int i = 0; i < scheduleCount; i++) {
    Serial.printf("    – %02d:%02d:%02d for %02d:%02d:%02d\n",
                  schedules[i].timeSec / 3600, (schedules[i].timeSec % 3600) / 60, schedules[i].timeSec % 60,
                  schedules[i].durationSec / 3600, (schedules[i].durationSec % 3600) / 60, schedules[i].durationSec % 60);
  }
  Serial.printf(" • Custom rules: \"%s\" (%s)\n",
                customSchedule,
                customEnabled ? "enabled" : "disabled");
}

void saveConfig() {
  // 1) Monta o documento JSON
  DynamicJsonDocument doc(2048);
  doc["manualDuration"] = manualDurationSec;
  doc["customSchedule"] = customSchedule;
  doc["customEnabled"] = customEnabled;

  // 2) Array de schedules
  JsonArray arr = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount && i < MAX_SLOTS; ++i) {
    JsonObject o = arr.createNestedObject();
    o["time"] = schedules[i].timeSec;
    o["duration"] = schedules[i].durationSec;
  }

  // 3) Serializa em String para cálculo de tamanho
  String json;
  serializeJson(doc, json);

  // 4) Grava em arquivo temporário
  const char *tmpPath = "/config.tmp";
  File tmp = FS.open(tmpPath, "w");
  if (!tmp) {
    Serial.println("Erro: não foi possível abrir arquivo temporário para escrita");
    return;
  }
  size_t written = tmp.print(json);
  tmp.close();
  if (written != json.length()) {
    Serial.println("Erro: nem todos os bytes foram gravados no arquivo temporário");
    // opcional: FS.remove(tmpPath);
    return;
  }

  // 5) Remove o arquivo antigo e renomeia o temporário
  if (FS.exists(CONFIG_PATH) && !FS.remove(CONFIG_PATH)) {
    Serial.println("Aviso: falha ao remover config antigo");
  }
  if (!FS.rename(tmpPath, CONFIG_PATH)) {
    Serial.println("Erro: não foi possível renomear arquivo temporário");
    return;
  }

  Serial.println("Configuration saved successfully");
}


// =============== Hardware Control =============== //
void setupHardware() {
   pinMode(FEEDER_PIN, OUTPUT);
  digitalWrite(FEEDER_PIN, LOW);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH); // ativo baixo na maioria das placas

  #ifdef SONOFF_BASIC
  pinMode(BUTTON_PIN, INPUT_PULLUP);    // botão usa pull‑up interno
  #endif

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

void handleGetEvents() {
  server.send(200, "text/plain", eventLog);
  eventLog = "";
}


// =============== Main Setup =============== //
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== Starting Pet Feeder ===");

  // 1) Hardware básico: relé, LED, watchdog, etc.
  setupHardware();

  // 2) Monta e valida sistema de arquivos
  loadConfig();  // já faz LittleFS/SPIFFS.begin()

  // 3) Conecta Wi‑Fi e sincroniza NTP
  setupNetwork();  // inclui configTime() e espera sync

  // 4) Após hora válida, reset do scheduler de intervalos
  scheduleChk("", FEEDER_PIN);

  // 5) Rotas HTTP principais
  server.on("/", HTTP_GET, handleRoot);
  server.on("/feedNow", HTTP_POST, handleFeedNow);
  server.on("/setManualDuration", HTTP_POST, handleSetManualDuration);
  server.on("/setSchedules", HTTP_POST, handleSetSchedules);
  server.on("/setCustomRules", HTTP_POST, handleSetCustomRules);
  server.on("/toggleCustomRules", HTTP_POST, handleToggleCustomRules);
  server.on("/events", HTTP_GET, handleGetEvents);

  // 6) Rota catch‑all para 404
  server.onNotFound([]() {
    server.send(404, "text/plain", "Rota nao encontrada");
  });

  // 7) Inicia servidor
  server.begin();
  Serial.println("HTTP server started");

  // 8) Debug: exibe configuração carregada
  Serial.printf("Manual duration: %lus\n", manualDurationSec);
  Serial.printf("Schedules: %d slots\n", scheduleCount);
  Serial.printf("Custom rules: \"%s\" [%s]\n",
                customSchedule,
                customEnabled ? "enabled" : "disabled");
}


// =============== Main Loop =============== //
void loop() {
  // 1) Atendimento HTTP e pisca‑piscamento do LED
  server.handleClient();
  updateStatusLED();

  // --- leitura do botão manual ---
  #ifdef SONOFF_BASIC
    bool raw = digitalRead(BUTTON_PIN);
    if (raw != lastButtonState) {
      // botão mudou, reinicia debounce
      lastDebounceMs = millis();
    }
    if (millis() - lastDebounceMs > DEBOUNCE_DELAY) {
      // estado estável após debounce
      static bool buttonPressedHandled = false;
      if (raw == LOW && !buttonPressedHandled) {
        // borda de HIGH→LOW detectada: botão pressionado
        startFeeding(manualDurationSec);   // sua função que dispara a alimentação
        buttonPressedHandled = true;
      }
      if (raw == HIGH) {
        // soltou o botão, libera para próxima detecção
        buttonPressedHandled = false;
      }
    }
    lastButtonState = raw;
  #endif

  // --- fim leitura botão ---

  // 2) Verifica se o pulso de alimentação terminou
  if (isFeeding && millis() - feedStartMs >= feedDurationMs) {
    digitalWrite(FEEDER_PIN, LOW);
    isFeeding = false;
    Serial.println("Feeding completed");
  }

  // 3) Executa regras de scheduler (custom ou padrão)
  if (customEnabled) {
    // scheduleChk já faz digitalWrite(pin, relay)
    String ev = scheduleChk(customSchedule, FEEDER_PIN);
    if (ev.length()) {
      // imprime no serial
      Serial.print("Custom event: ");
      Serial.println(ev);

      // obtém timestamp “HH:MM:SS”
      time_t t = time(nullptr);
      struct tm tm;
      localtime_r(&t, &tm);
      char ts[9];
      snprintf(ts, sizeof(ts), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

      // armazena com timestamp no buffer de eventos
      eventLog += String(ts) + " -> " + ev + "\n";
    }
  }
  else {
    // modo padrão por horário fixo
    checkSchedules();
  }

  // 4) Reset do watchdog
  lastWatchdogReset = millis();

  // 5) Garanta que o sistema continue responsivo
  yield();
}


// Inicia alimentação por 'manualDurationSec' segundos
void startFeeding(unsigned long durationSec) {
  if (!isFeeding) {
    isFeeding = true;
    feedStartMs = millis();
    feedDurationMs = durationSec * 1000UL;
    digitalWrite(FEEDER_PIN, HIGH);
    lastTriggerMs = millis();
    Serial.print("Feeding started for ");
    Serial.print(durationSec);
    Serial.println(" seconds");
  }
}

// Interrompe alimentação imediatamente
void stopFeeding() {
  if (isFeeding) {
    digitalWrite(FEEDER_PIN, LOW);
    isFeeding = false;
    Serial.println("Feeding stopped by IL event");
  }
}
