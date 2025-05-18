#ifdef ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#define FS LittleFS
#define WebServer ESP8266WebServer
#else // ESP32
#include <WiFi.h>
#include <WebServer.h> // Para ESP32, WebServer.h é o padrão.
#include <SPIFFS.h>
#define FS SPIFFS
#endif

#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <time.h>
#include <TimeLib.h>
#include <WiFiUdp.h>

// Bibliotecas para o RTC DS3231
#include <Wire.h>
#include <RTClib.h> // Certifique-se de instalar a biblioteca RTClib da Adafruit

// =============== RTC DS3231 Object =============== //
RTC_DS3231 rtc;
bool rtcInitialized = false; // Flag para rastrear status da inicialização do RTC

// =============== Configuration =============== //
const char *CONFIG_PATH = "/config.json";
const int MAX_SLOTS = 10;
const long GMT_OFFSET_SEC = -4 * 3600;  // UTC-4 (Cuiabá). Ajuste conforme o seu fuso horário.
const int DAYLIGHT_OFFSET_SEC = 0;      // Horário de verão (0 se não aplicável)
const char *NTP_SERVER = "time.google.com";
const int FEED_COOLDOWN = 10;       // Segundos entre ativações
const int MAX_FEED_DURATION = 300;    // 5 minutos no máximo para ativação manual/agendada

// --- Pinos de Hardware ---
#ifdef SONOFF_BASIC
  const int STATUS_LED_PIN = 13; // LED no GPIO13 para Sonoff Basic
  const int BUTTON_PIN     = 0;   // Botão no GPIO0 para Sonoff Basic
  bool      lastButtonState = HIGH;
  uint32_t lastDebounceMs = 0;
  const uint32_t DEBOUNCE_DELAY = 50;
  int defaultFeederPin = 12; // Relé no GPIO12 para Sonoff Basic
#elif defined(ESP8266)
  const int STATUS_LED_PIN = LED_BUILTIN; // LED_BUILTIN (geralmente GPIO2 ou GPIO1 para ESP-01)
  const int BUTTON_PIN     = -1; // Sem botão por defeito
  int defaultFeederPin = 14; // Exemplo: D5 (GPIO14) no NodeMCU
#else // ESP32
  const int STATUS_LED_PIN = 2;  // LED_BUILTIN no ESP32 geralmente é o GPIO 2
  const int BUTTON_PIN     = -1; // Sem botão por defeito
  int defaultFeederPin = 5;  // Pino de exemplo para ESP32, verifique sua placa!
#endif

int currentFeederPin = defaultFeederPin;


// =============== Data Structures =============== //
struct Schedule {
  int timeSec;        // Hora do dia para o agendamento (segundos desde a meia-noite)
  int durationSec;    // Duração da ativação em segundos
  int lastTriggerDay; // Dia do ano em que foi acionado pela última vez (para evitar repetição no mesmo dia)
};

// =============== Global State =============== //

#if defined(ESP8266)
  ESP8266WebServer server(80);
#else // ESP32
  WebServer server(80);
#endif
WiFiManager wifiManager; // Gestor de WiFi para configuração fácil
WiFiUDP udp; // Para NTP

Schedule schedules[MAX_SLOTS]; // Array para armazenar os agendamentos
int scheduleCount = 0; // Número de agendamentos ativos
unsigned long manualDurationSec = 5; // Duração padrão para ativação manual (em segundos)
char customSchedule[512] = ""; // String para armazenar as regras personalizadas
bool customEnabled = false; // Flag para indicar se as regras personalizadas estão ativas
volatile bool isOutputActive = false; // Flag para indicar se o dispositivo/saída está ativo (HIGH)
unsigned long outputActivationStartMs = 0; // Timestamp (millis) de quando a ativação começou
unsigned long outputActivationDurationMs = 0; // Duração programada da ativação atual (em milissegundos)
unsigned long lastTriggerMs = 0; // Timestamp (millis) do último acionamento (para cooldown)

// Timestamps (time_t) para regras IH/IL. São atualizados quando as regras customizadas estão ativas.
time_t ruleHighDT = 0; // Timestamp de quando o pino foi para HIGH devido a uma regra customizada
time_t ruleLowDT = 0;  // Timestamp de quando o pino foi para LOW devido a uma regra customizada


String eventLog = ""; // String para acumular logs de eventos para a interface web
String hhmmStr(const time_t &t); // Declaração antecipada
String formatHHMMSS(int secs); // Declaração antecipada
String scheduleChk(const String &schedule, const byte &pin); // Declaração antecipada


// Converte um timestamp (time_t) para uma string "HH:MM:SS"
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
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Temporizador Inteligente</title>
  <style>
    body {
      font-family: system-ui, -apple-system, sans-serif;
      margin: 0 auto;
      padding: 20px;
      max-width: 600px;
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
    input[type="time"],
    input[type="text"],
    input[type="number"],
    textarea {
      width: 100%;
      padding: 10px;
      border: 1px solid #ddd;
      border-radius: 4px;
      margin-bottom: 10px;
      box-sizing: border-box;
      font-family: inherit;
    }
    button {
      width: 100%;
      padding: 12px;
      background: #2e7d32;
      color: #fff;
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
    button.warning {
      background: #d32f2f;
    }
    button.warning:hover {
      background: #c62828;
    }
    button:disabled {
      background: #ccc;
      cursor: not-allowed;
    }
    ul {
      list-style: none;
      padding: 0;
    }
    li {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 8px;
      background: #f8f8f8;
      border-radius: 4px;
      margin-bottom: 8px;
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
    .led.active { /* LED verde para WiFi conectado e dispositivo inativo */
      background: #4caf50;
      box-shadow: 0 0 10px #4caf50;
    }
    .led.feeding { /* LED laranja para dispositivo ativo (piscando) - 'feeding' é a classe CSS, mantida por simplicidade */
      background: #ff9800; /* Laranja para "ativo" */
      box-shadow: 0 0 10px #ff9800;
      animation: pulse 1s infinite;
    }
    @keyframes pulse {
      0%   { opacity: 1; }
      50%  { opacity: 0.5; }
      100% { opacity: 1; }
    }
    #currentTime,
    #wifiQuality {
      font-size: 1.2em;
      color: #666;
      margin-left: 8px;
    }
    #nextTrigger, #customRuleCountdown {
      text-align: center;
      margin: 10px 0;
      padding: 10px;
      background: #e8f5e9;
      border-radius: 4px;
      color: #2e7d32;
      font-size: 1.1em;
    }
     #customRuleCountdown {
        background: #fff3e0;
        color: #e65100;
        display: none;
     }
    #eventConsole {
      background: #111;
      color: #0f0;
      font-family: monospace;
      white-space: pre-wrap;
      padding: 10px;
      height: 120px;
      overflow-y: auto;
      border: 1px solid #444;
      border-radius: 4px;
      margin-top: 10px;
    }
    .message {
      margin-top: 10px;
      padding: 10px;
      border-radius: 4px;
      text-align: center;
      font-weight: bold;
      min-height: 1.5em;
    }
    .message.success {
      background: #e6f4ea;
      color: #388e3c;
      border: 1px solid #66bb6a;
    }
    .message.error {
      background: #feebeb;
      color: #c62828;
      border: 1px solid #ef5350;
    }
    details > summary {
      padding: 8px;
      background-color: #eee;
      border: 1px solid #ddd;
      border-radius: 4px;
      cursor: pointer;
      font-weight: 500;
      margin-top: 15px;
    }
    details > div {
      padding:10px;
      background-color:#f9f9f9;
      border:1px solid #eee;
      border-top: none;
      border-radius: 0 0 4px 4px;
      margin-bottom: 10px;
    }
    details ul { margin-left: 20px; list-style-type: disc; }
    details code { background-color: #e8e8e8; padding: 2px 4px; border-radius: 3px; }
    .pin-table { width: 100%; border-collapse: collapse; margin-top: 10px; }
    .pin-table th, .pin-table td { border: 1px solid #ddd; padding: 6px; text-align: left; }
    .pin-table th { background-color: #f2f2f2; }
  </style>
</head>
<body>
  <h1>Temporizador Inteligente</h1>

  <section class="status-indicator">
    <div class="led %STATUS_CLASS%"></div> <div id="currentTime">--:--:--</div>
    <div id="wifiQuality">Wi‑Fi: %WIFI_QUALITY%%</div>
  </section>

  <section class="card">
    <button id="manualActivateOutput">Ativar Saída Agora</button>
    <button id="manualDeactivateOutput" class="warning" style="display: none;">Desativar Saída</button>
    <div id="manualOutputMessage" class="message"></div>
  </section>

  <section class="card">
    <form id="manualDurationForm">
      <label for="manualInterval">Duração do Pulso/Ativação (HH:MM:SS)</label>
      <input type="time" id="manualInterval" step="1" value="%MANUAL%" />
      <button type="submit">Salvar Duração</button>
      <div id="manualDurationMessage" class="message"></div>
    </form>
  </section>

  <section class="card">
    <form id="outputPinForm">
      <label for="outputPinInput">Pino de Saída (GPIO):</label>
      <input type="number" id="outputPinInput" value="%OUTPUT_PIN_VALUE%" min="0" max="39">
      <button type="submit">Salvar Pino de Saída</button>
      <div id="outputPinMessage" class="message"></div>
    </form>
    <details>
        <summary>Ajuda: Pinos Wemos D1 Mini (ESP8266)</summary>
        <div>
            <p>Abaixo uma referência dos pinos do Wemos D1 Mini e seus GPIOs correspondentes. Insira o número GPIO no campo acima.</p>
            <table class="pin-table">
                <thead><tr><th>Pino (Silk)</th><th>GPIO</th><th>Observações</th></tr></thead>
                <tbody>
                    <tr><td>D0</td><td>16</td><td>LED integrado (em algumas versões), sem PWM/I2C/SPI, cuidado ao usar.</td></tr>
                    <tr><td>D1</td><td>5</td><td>SCL (I2C)</td></tr>
                    <tr><td>D2</td><td>4</td><td>SDA (I2C)</td></tr>
                    <tr><td>D3</td><td>0</td><td>Flash Mode (nível BAIXO no boot entra em modo flash).</td></tr>
                    <tr><td>D4</td><td>2</td><td>TXD1, LED integrado (azul).</td></tr>
                    <tr><td>D5</td><td>14</td><td>HSCLK (SPI)</td></tr>
                    <tr><td>D6</td><td>12</td><td>HMISO (SPI)</td></tr>
                    <tr><td>D7</td><td>13</td><td>HMOSI/RXD2 (SPI)</td></tr>
                    <tr><td>D8</td><td>15</td><td>HCS (SPI), deve estar em nível BAIXO no boot.</td></tr>
                    <tr><td>RX</td><td>3</td><td>RXD0</td></tr>
                    <tr><td>TX</td><td>1</td><td>TXD0 (Debug)</td></tr>
                </tbody>
            </table>
            <p><small><strong>Recomendação:</strong> Para saídas simples, D1, D2, D5, D6, D7 são geralmente seguros. Evite D0, D3, D4, D8, RX, TX a menos que saiba as implicações.</small></p>
            <p><small>Para placas ESP32, consulte o pinout específico da sua placa.</small></p>
        </div>
    </details>
  </section>

  <section class="card">
    <form id="scheduleForm">
      <label for="newScheduleTime">Novo Horário de Ativação (HH:MM:SS)</label>
      <input type="time" id="newScheduleTime" step="1" />
      <label for="newScheduleInterval">Duração da Ativação (HH:MM:SS)</label>
      <input type="time" id="newScheduleInterval" step="1" />
      <button type="button" id="addSchedule">+ Adicionar Agendamento</button>
    </form>
    <ul id="scheduleList"></ul>
    <button id="saveSchedules">Salvar Agendamentos</button>
    <div id="scheduleFormMessage" class="message"></div>
  </section>

  <section class="card">
    <label for="customRulesInput">Regras Avançadas (Editar):</label>
    <textarea id="customRulesInput" rows="4" placeholder="Ex: IH00:00:30 IL00:01:00">%CUSTOM_RULES%</textarea>
    <button id="saveRules">Salvar Regras</button>
    <button id="toggleRules" class="secondary">%TOGGLE_BUTTON%</button>
    <div id="customRulesMessage" class="message"></div>

    <details>
        <summary>Ajuda: Sintaxe das Regras Customizadas</summary>
        <div>
            <p><strong>Prefixos Principais:</strong></p>
            <ul>
                <li><code>DH HH:MM:SS</code>: <strong>Diário Alto (Ligar Saída)</strong> - Liga a saída todos os dias no horário especificado.</li>
                <li><code>DL HH:MM:SS</code>: <strong>Diário Baixo (Desligar Saída)</strong> - Desliga a saída todos os dias no horário especificado.</li>
                <li><code>WH d HH:MM:SS</code>: <strong>Semanal Alto (Ligar Saída)</strong> - Liga a saída no dia da semana <code>d</code> (1=Domingo, ..., 7=Sábado) no horário especificado.</li>
                <li><code>WL d HH:MM:SS</code>: <strong>Semanal Baixo (Desligar Saída)</strong> - Desliga a saída no dia da semana <code>d</code> no horário especificado.</li>
                <li><code>SH AAAA-MM-DD HH:MM</code>: <strong>Específico Alto (Ligar Saída)</strong> - Liga a saída na data e hora exatas. (Segundos não são usados aqui).</li>
                <li><code>SL AAAA-MM-DD HH:MM</code>: <strong>Específico Baixo (Desligar Saída)</strong> - Desliga a saída na data e hora exatas. (Segundos não são usados aqui).</li>
                <li><code>IH HH:MM:SS</code>: <strong>Intervalo Alto (Desligar após Ligado)</strong> - Se a saída estiver LIGADA, ela será DESLIGADA após o intervalo de tempo especificado.</li>
                <li><code>IL HH:MM:SS</code>: <strong>Intervalo Baixo (Ligar após Desligado)</strong> - Se a saída estiver DESLIGADA, ela será LIGADA após o intervalo de tempo especificado.</li>
            </ul>
            <p><small>Consulte a documentação completa para mais exemplos e detalhes.</small></p>
        </div>
    </details>

    <label for="eventConsole" style="margin-top:15px;">Console de Eventos:</label>
    <div id="eventConsole"></div>
  </section>

  <div id="nextTrigger">Carregando...</div>
  <div id="customRuleCountdown"></div>
<script>
  function pad(n){ return n.toString().padStart(2,'0'); }
  function parseHHMMSS_to_secs(s){ const p=s.split(':').map(Number); return p[0]*3600+p[1]*60+p[2]; }
  function formatHHMMSS(secs){
    if (isNaN(secs) || secs < 0) return "00:00:00";
    return [Math.floor(secs/3600),Math.floor((secs%3600)/60),secs%60].map(pad).join(':');
  }

  function updateWifiQuality() {
    fetch('/rssi')
      .then(res => res.json())
      .then(j => {
        document.getElementById('wifiQuality').textContent = 'Wi-Fi: ' + j.pct + '%';
      })
      .catch(_=> {
        document.getElementById('wifiQuality').textContent = 'Wi-Fi: --%';
      });
  }
  setInterval(updateWifiQuality, 5000);
  updateWifiQuality();

  function updateCurrentTime() {
    fetch('/time')
        .then(res => res.text())
        .then(timeStr => {
            document.getElementById('currentTime').textContent = timeStr;
        })
        .catch(_ => {
            document.getElementById('currentTime').textContent = '--:--:--';
        });
  }
  setInterval(updateCurrentTime, 1000);
  updateCurrentTime();

  let schedules = [], manualIntervalSecs = parseHHMMSS_to_secs("%MANUAL%");
  const rawSch = "%SCHEDULES%";
  if(rawSch) schedules = rawSch.split(',').map(t=>{const a=t.split('|');return{time:a[0],interval:a[1]};});

  function renderSchedules(){
    const ul=document.getElementById('scheduleList'); ul.innerHTML='';
    schedules.forEach((o,i)=>{
      const li=document.createElement('li');
      li.innerHTML=`<span>${o.time} (Duração: ${o.interval})</span><button class="delete" data-index="${i}">×</button>`;
      li.querySelector('.delete').onclick=(e)=>{
          schedules.splice(parseInt(e.target.dataset.index),1);
          renderSchedules();
          updateNextTrigger();
      };
      ul.appendChild(li);
    });
  }
  renderSchedules();

  document.getElementById('addSchedule').onclick=()=>{
    const t=document.getElementById('newScheduleTime').value, i=document.getElementById('newScheduleInterval').value;
    const rx=/^([0-9]{2}):([0-9]{2}):([0-9]{2})$/;
    if(!t || !i) {showMessage('scheduleFormMessage','Preencha horário e duração','error');return;}
    if(!rx.test(t)||!rx.test(i)){showMessage('scheduleFormMessage','Use formato HH:MM:SS','error');return;}
    if (schedules.length >= 10) {
        showMessage('scheduleFormMessage', 'Máximo de 10 agendamentos atingido', 'error'); return;
    }
    schedules.push({time:t,interval:i});renderSchedules();
    showMessage('scheduleFormMessage','Agendamento adicionado localmente. Clique em "Salvar Agendamentos".','success');
  };

  document.getElementById('manualDurationForm').onsubmit=e=>{ // ID do formulário atualizado
    e.preventDefault();
    const d=document.getElementById('manualInterval').value, rx=/^([0-9]{2}):([0-9]{2}):([0-9]{2})$/;
    if(!rx.test(d)){showMessage('manualDurationMessage','Use formato HH:MM:SS','error');return;} // ID da mensagem atualizado
    fetch('/setManualDuration',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`manualDuration=${d}`})
      .then(r=>{if(r.ok){showMessage('manualDurationMessage','Duração salva','success');manualIntervalSecs=parseHHMMSS_to_secs(d); } else {r.text().then(txt => showMessage('manualDurationMessage','Erro: ' + txt,'error'));}})
      .catch(_=>showMessage('manualDurationMessage','Erro ao salvar','error'));
  };

  document.getElementById('outputPinForm').onsubmit = e => { // ID do formulário atualizado
    e.preventDefault();
    const pin = document.getElementById('outputPinInput').value; // ID do input atualizado
    if (isNaN(parseInt(pin)) || parseInt(pin) < 0 ) {
      showMessage('outputPinMessage', 'Número do pino inválido', 'error'); // ID da mensagem atualizado
      return;
    }
    fetch('/setFeederPin', { // Endpoint mantido como /setFeederPin por simplicidade no backend
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: `feederPin=${pin}`
    })
    .then(r => {
      if (r.ok) {
        showMessage('outputPinMessage', 'Pino de saída salvo. Pode ser necessário reiniciar.', 'success');
      } else {
        r.text().then(txt => showMessage('outputPinMessage', 'Erro: ' + txt, 'error'));
      }
    })
    .catch(_ => showMessage('outputPinMessage', 'Erro ao salvar pino', 'error'));
  };

  document.getElementById('saveSchedules').onclick=()=>{
    const body='schedules='+encodeURIComponent(schedules.map(o=>`${o.time}|${o.interval}`).join(','));
    fetch('/setSchedules',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body})
      .then(r=>{if(r.ok){showMessage('scheduleFormMessage','Agendamentos salvos','success');updateNextTrigger();} else {r.text().then(txt => showMessage('scheduleFormMessage','Erro: ' + txt,'error'));}})
      .catch(_=>showMessage('scheduleFormMessage','Erro ao salvar','error'));
  };

  document.getElementById('saveRules').onclick=()=>{
    const rules=document.getElementById('customRulesInput').value;
    fetch('/setCustomRules',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'rules='+encodeURIComponent(rules)})
      .then(r=>{if(r.ok)showMessage('customRulesMessage','Regras salvas','success');else {r.text().then(txt => showMessage('customRulesMessage','Erro: ' + txt,'error'));}})
      .catch(_=>showMessage('customRulesMessage','Erro ao salvar','error'));
  };

  document.getElementById('toggleRules').onclick=()=>{
    fetch('/toggleCustomRules',{method:'POST'}).then(r=>{if(r.ok)location.reload();else {r.text().then(txt => showMessage('customRulesMessage','Erro: ' + txt,'error'));}}).catch(_=>showMessage('customRulesMessage','Erro ao alternar','error'));
  };

  const manualActivateButton = document.getElementById('manualActivateOutput'); // ID do botão atualizado
  const manualDeactivateButton = document.getElementById('manualDeactivateOutput'); // ID do botão atualizado

  manualActivateButton.onclick = () => {
    fetch('/feedNow', { method: 'POST' }) // Endpoint /feedNow mantido
        .then(r => {
            if (r.ok) {
                showMessage('manualOutputMessage', 'Saída ativada', 'success'); // ID da mensagem atualizado
            } else {
                return r.text().then(txt => { throw new Error(txt); });
            }
        })
        .catch(err => showMessage('manualOutputMessage', 'Erro: ' + err.message, 'error'))
        .finally(() => updateStatusAndRuleCountdown());
  };

  manualDeactivateButton.onclick = () => {
    fetch('/stopFeedNow', { method: 'POST' }) // Endpoint /stopFeedNow mantido
        .then(r => {
            if (r.ok) {
                showMessage('manualOutputMessage', 'Saída desativada.', 'success'); // ID da mensagem atualizado
            } else {
                 return r.text().then(txt => { throw new Error(txt); });
            }
        })
        .catch(err => showMessage('manualOutputMessage', 'Erro: ' + err.message, 'error'))
        .finally(() => updateStatusAndRuleCountdown());
  };

  let customRuleCountdownInterval = null;

  function updateStatusAndRuleCountdown() {
    fetch('/status')
        .then(res => res.json())
        .then(data => {
            if (data.is_feeding) { // is_feeding no backend representa isOutputActive
                manualDeactivateButton.style.display = 'block';
                manualActivateButton.disabled = true;
            } else {
                manualDeactivateButton.style.display = 'none';
                manualActivateButton.disabled = false;
            }

            if (customRuleCountdownInterval) clearInterval(customRuleCountdownInterval);
            const ruleCountdownElement = document.getElementById('customRuleCountdown');

            if (data.custom_rules_enabled && data.active_custom_rule !== "none" && data.custom_rule_time_remaining >= 0) {
                ruleCountdownElement.style.display = 'block';
                let totalSeconds = data.custom_rule_time_remaining;
                let ruleType = data.active_custom_rule;
                let ruleState = ruleType === "IH" ? "LIGADO" : "DESLIGADO";

                ruleCountdownElement.textContent = `Regra Ativa (${ruleType}): Tempo ${ruleState} restante: ${formatHHMMSS(totalSeconds)}`;

                customRuleCountdownInterval = setInterval(() => {
                    if (totalSeconds <= 0) {
                        clearInterval(customRuleCountdownInterval);
                        ruleCountdownElement.textContent = "Regra Ativa: Verificando...";
                        setTimeout(updateStatusAndRuleCountdown, 1500);
                        return;
                    }
                    totalSeconds--;
                    ruleCountdownElement.textContent = `Regra Ativa (${ruleType}): Tempo ${ruleState} restante: ${formatHHMMSS(totalSeconds)}`;
                }, 1000);
            } else {
                ruleCountdownElement.style.display = 'none';
            }
        })
        .catch(_ => {
            manualDeactivateButton.style.display = 'none';
            manualActivateButton.disabled = false;
            if (customRuleCountdownInterval) clearInterval(customRuleCountdownInterval);
            document.getElementById('customRuleCountdown').style.display = 'none';
        });
  }
  setInterval(updateStatusAndRuleCountdown, 2500);
  updateStatusAndRuleCountdown();

  let countdownInterval = null;
  function updateNextTrigger(){
    fetch('/nextTriggerTime')
        .then(res => res.text())
        .then(text => {
            if (countdownInterval) clearInterval(countdownInterval);
            const triggerElement = document.getElementById('nextTrigger');

            const match = text.match(/Próxima em: (\d{2}):(\d{2}):(\d{2})/);
            const durationMatch = text.match(/\(duração (\d{2}):(\d{2}):(\d{2})\)/);
            let originalSuffix = "";
            if (durationMatch) {
                originalSuffix = ` (duração ${durationMatch[1]}:${durationMatch[2]}:${durationMatch[3]})`;
            } else if (match) {
                const afterTime = text.substring(text.indexOf(match[0]) + match[0].length);
                if (afterTime.trim().length > 0) originalSuffix = afterTime;
            }

            if (match) {
                let hours = parseInt(match[1]);
                let minutes = parseInt(match[2]);
                let seconds = parseInt(match[3]);
                let totalSeconds = hours * 3600 + minutes * 60 + seconds;

                if (totalSeconds >= 0) {
                    triggerElement.textContent = `Próxima em: ${pad(hours)}:${pad(minutes)}:${pad(seconds)}${originalSuffix}`;
                    countdownInterval = setInterval(() => {
                        if (totalSeconds <= 0) {
                            clearInterval(countdownInterval);
                            triggerElement.textContent = "Verificando próximo agendamento...";
                            setTimeout(updateNextTrigger, 1500);
                            updateStatusAndRuleCountdown();
                            return;
                        }
                        totalSeconds--;
                        const h = Math.floor(totalSeconds / 3600);
                        const m = Math.floor((totalSeconds % 3600) / 60);
                        const s = totalSeconds % 60;
                        triggerElement.textContent = `Próxima em: ${pad(h)}:${pad(m)}:${pad(s)}${originalSuffix}`;
                    }, 1000);
                } else {
                     triggerElement.textContent = text;
                }
            } else {
                 triggerElement.textContent = text;
            }
        })
        .catch(_ => {
            if (countdownInterval) clearInterval(countdownInterval);
            document.getElementById('nextTrigger').textContent = 'Erro ao buscar próximo acionamento.';
        });
  }
  setInterval(updateNextTrigger, 30000);
  updateNextTrigger();

  function showMessage(id,msg,type){
    const e=document.getElementById(id);e.textContent=msg;e.className='message '+type;
    setTimeout(()=>{e.textContent='';e.className='message';},5000);
  }

  setInterval(() => {
    fetch('/events')
      .then(res => res.text())
      .then(txt => {
        if (txt && txt.trim().length > 0) {
          const con = document.getElementById('eventConsole');
          const needsScroll = con.scrollHeight - con.scrollTop === con.clientHeight;
          con.innerText += (con.innerText ? '\n' : '') + txt.trim();
          if(needsScroll) con.scrollTop = con.scrollHeight;
        }
      });
  }, 2000);
</script>
</body>
</html>
)rawliteral";


/*******************************************************************************
* FUNÇÕES AUXILIARES DE TEMPO E RTC
*******************************************************************************/

// Função para sincronizar a biblioteca TimeLib com o RTC DS3231
void syncTimeLibWithRTC() {
  if (!rtcInitialized) { 
    return;
  }

  DateTime nowRTC = rtc.now(); 
  if (nowRTC.year() < 2023 && nowRTC.year() > 1970) { 
      if (millis() % 60000 == 0) {
          Serial.printf("AVISO: RTC retornou data inválida no loop: %04d-%02d-%02d. Não sincronizando TimeLib com este valor.\n",
                        nowRTC.year(), nowRTC.month(), nowRTC.day());
      }
      return; 
  }
  setTime(nowRTC.unixtime()); 
}

String hhmmStr(const time_t &t) {
  String s;
  if (hour(t) < 10) s += '0';
  s += String(hour(t)) + ':';
  if (minute(t) < 10) s += '0';
  s += String(minute(t));
  return s;
}

void formatHHMMSS(int secs, char *buf, size_t bufSize) {
  secs = abs(secs); 
  snprintf(buf, bufSize, "%02d:%02d:%02d",
           secs / 3600,        
           (secs % 3600) / 60, 
           secs % 60);         
}

String formatHHMMSS(int secs) {
  char buf[9]; 
  formatHHMMSS(secs, buf, sizeof(buf));
  return String(buf);
}

int calculateDayOfYear(int y, int m, int d) {
  int daysInMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; 
  if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
    daysInMonth[2] = 29; 
  }
  int dayOfYear = 0;
  for (int i = 1; i < m; i++) { 
    dayOfYear += daysInMonth[i];
  }
  dayOfYear += d; 
  return dayOfYear;
}

int getCurrentDayOfYear() {
  time_t t = now(); 
  return calculateDayOfYear(year(t), month(t), day(t));
}


int getCurrentTimeInSec() {
  return hour() * 3600 + minute() * 60 + second(); 
}

String getCurrentDateTimeString() {
  time_t t = now(); 
  char buf[20]; 
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           year(t), month(t), day(t), hour(t), minute(t), second(t));
  return String(buf);
}

String getNextTriggerTimeString() {
    if (customEnabled) { 
        return "Regras personalizadas ativas.";
    }

    if (scheduleCount == 0) { 
        return "Nenhum agendamento configurado.";
    }

    time_t currentTimeLib = now(); 
    int currentDayOfYearVal = calculateDayOfYear(year(currentTimeLib), month(currentTimeLib), day(currentTimeLib));

    long minNextTriggerUnixtime = -1; 
    int nextDuration = 0; 

    for (int i = 0; i < scheduleCount; i++) {
        DateTime scheduleDateTimeToday(year(currentTimeLib), month(currentTimeLib), day(currentTimeLib),
                                       schedules[i].timeSec / 3600,
                                       (schedules[i].timeSec % 3600) / 60,
                                       schedules[i].timeSec % 60);

        time_t scheduleUnixtimeToday = scheduleDateTimeToday.unixtime(); 

        if (scheduleUnixtimeToday < currentTimeLib || schedules[i].lastTriggerDay == currentDayOfYearVal) {
            DateTime scheduleDateTimeTomorrow = scheduleDateTimeToday + TimeSpan(1,0,0,0); 
            if (minNextTriggerUnixtime == -1 || scheduleDateTimeTomorrow.unixtime() < minNextTriggerUnixtime) {
                minNextTriggerUnixtime = scheduleDateTimeTomorrow.unixtime();
                nextDuration = schedules[i].durationSec;
            }
        } else { 
            if (minNextTriggerUnixtime == -1 || scheduleUnixtimeToday < minNextTriggerUnixtime) {
                minNextTriggerUnixtime = scheduleUnixtimeToday;
                nextDuration = schedules[i].durationSec;
            }
        }
    }

    if (minNextTriggerUnixtime != -1) { 
        time_t diffSeconds = minNextTriggerUnixtime - currentTimeLib; 
        if (diffSeconds < 0) diffSeconds = 0; 

        char timeBuf[9]; 
        formatHHMMSS(diffSeconds, timeBuf, sizeof(timeBuf)); 

        char durationBuf[9]; 
        formatHHMMSS(nextDuration, durationBuf, sizeof(durationBuf)); 

        return "Próxima em: " + String(timeBuf) + " (duração " + String(durationBuf) + ")";
    }
    return "Nenhum agendamento futuro encontrado."; 
}


// =============== Schedule Checking =============== //
void checkSchedules() {
  if (isOutputActive || customEnabled) return; 

  int today = getCurrentDayOfYear(); 
  int currentTime = getCurrentTimeInSec(); 

  for (int i = 0; i < scheduleCount; i++) {
    Schedule &s = schedules[i];
    if (currentTime == s.timeSec && s.lastTriggerDay != today) {
      if (millis() - lastTriggerMs >= FEED_COOLDOWN * 1000UL || lastTriggerMs == 0) {
        s.lastTriggerDay = today; 
        String logRuleIntent = timeStr(now()) + " -> Agendamento #" + String(i) + " (" + formatHHMMSS(s.timeSec) + ") detectado para LIGAR SAÍDA.";
        Serial.println(logRuleIntent);
        eventLog += logRuleIntent + "\n";

        startOutput(s.durationSec); 

        saveConfig(); 
      } else {
         String logMsg = getCurrentDateTimeString() + " -> Agendamento #" + String(i) + " ignorado (cooldown)";
         Serial.println(logMsg);
         eventLog += logMsg + "\n";
      }
    }
  }
}

time_t parseDateTime(const String &s) {
  struct tm tm_struct; 
  if (sscanf(s.c_str(), " %4d-%2d-%2d %2d:%2d",
             &tm_struct.tm_year, &tm_struct.tm_mon, &tm_struct.tm_mday,
             &tm_struct.tm_hour, &tm_struct.tm_min)
      == 5) { 
    tm_struct.tm_year -= 1900; 
    tm_struct.tm_mon -= 1;    
    tm_struct.tm_sec = 0;     
    tm_struct.tm_isdst = -1;  
    return mktime(&tm_struct); 
  }
  return (time_t)0; 
}

int parseHHMMSS(const String &s) {
  int h = 0, m = 0, sec = 0;
  if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3) { 
    if (h >= 0 && h < 24 && m >= 0 && m < 60 && sec >= 0 && sec < 60) {
        return h * 3600 + m * 60 + sec; 
    }
  }
  return -1; 
}

int parseRuleTime(const String& rulePrefix, const String& rules) {
    int rulePos = rules.indexOf(rulePrefix); 
    if (rulePos != -1) {
        String timeStrVal = rules.substring(rulePos + rulePrefix.length(), rulePos + rulePrefix.length() + 8);
        int h = 0, m = 0, s_val = 0; 
        if (sscanf(timeStrVal.c_str(), "%d:%d:%d", &h, &m, &s_val) == 3) { 
            if (h >= 0 && h < 24 && m >= 0 && m < 60 && s_val >= 0 && s_val < 60) {
                return h * 3600 + m * 60 + s_val; 
            }
        }
    }
    return -1; 
}


String scheduleChk(const String &schedule, const byte &pin) {
  String event = "";    
  byte relay_val;       
  static time_t lastCheck = 0; 

  String dt_str = "";
  String s_temp = ""; 
  String timeOnlyStr = ""; 
  String dayOfWeekStr = ""; 

  if (schedule == "") { 
    return "";
  }

  time_t currentTime = now(); 

  if (currentTime - lastCheck < 1 && lastCheck != 0) return ""; 
  lastCheck = currentTime;

  char currentDtStr[17];
  sprintf(currentDtStr, "%04d-%02d-%02d %02d:%02d",
          year(currentTime), month(currentTime), day(currentTime),
          hour(currentTime), minute(currentTime));
  dt_str = String(currentDtStr);

  s_temp = "SH" + dt_str;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = HIGH; goto P_scheduleChk_Action; }
  s_temp = "SL" + dt_str;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = LOW; goto P_scheduleChk_Action; }

  timeOnlyStr = dt_str.substring(11); 

  s_temp = "DH" + timeOnlyStr;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = HIGH; goto P_scheduleChk_Action; }
  s_temp = "DL" + timeOnlyStr;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = LOW; goto P_scheduleChk_Action; }

  dayOfWeekStr = String(weekday(currentTime)) + " " + timeOnlyStr; 

  s_temp = "WH" + dayOfWeekStr;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = HIGH; goto P_scheduleChk_Action; }
  s_temp = "WL" + dayOfWeekStr;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = LOW; goto P_scheduleChk_Action; }


  if (digitalRead(pin) == HIGH && ruleHighDT != 0) { 
    int ihTime = parseRuleTime("IH", schedule); 
    if (ihTime != -1 && (currentTime - ruleHighDT >= ihTime)) { 
      event = "IH" + formatHHMMSS(ihTime);
      relay_val = LOW; 
      goto P_scheduleChk_Action;
    }
  }
  if (digitalRead(pin) == LOW && ruleLowDT != 0) { 
    int ilTime = parseRuleTime("IL", schedule); 
    if (ilTime != -1 && (currentTime - ruleLowDT >= ilTime)) { 
      event = "IL" + formatHHMMSS(ilTime);
      relay_val = HIGH; 
      goto P_scheduleChk_Action;
    }
  }

P_scheduleChk_Action: 
  if (event != "" && relay_val != digitalRead(pin)) { 
    String logRuleIntent = timeStr(currentTime) + " -> Regra " + event + " detectada para " + (relay_val == HIGH ? "LIGAR SAÍDA" : "DESLIGAR SAÍDA") + ".";
    Serial.println(logRuleIntent);
    eventLog += logRuleIntent + "\n";

    if (relay_val == HIGH) { 
      unsigned long durationToFeedSec = MAX_FEED_DURATION + 1; 
      int ihTime = parseRuleTime("IH", customSchedule); 
      if (ihTime != -1 && ihTime > 0) { 
        durationToFeedSec = ihTime; 
      }
      startOutput(durationToFeedSec); 
    } else { 
      stopOutput(); 
    }
    return event; 
  }
  return ""; 
}


// =============== Web Server Handlers =============== //
void handleRssi() {
  int rssi = WiFi.RSSI();
  int pct  = map(constrain(rssi, -90, -30), -90, -30, 0, 100); 
  String json = String("{\"rssi\":") + rssi + ",\"pct\":" + pct + "}";
  server.send(200, "application/json", json);
}

void handleTime() {
    server.send(200, "text/plain", timeStr(now()));
}

void handleNextTriggerTime() {
    server.send(200, "text/plain", getNextTriggerTimeString());
}

void handleSetOutputPin() { // Renomeado de handleSetFeederPin
  if (!server.hasArg("feederPin")) { // Parâmetro mantido como 'feederPin' para compatibilidade com JS existente
    server.send(400, "text/plain", "Parâmetro 'feederPin' ausente");
    return;
  }
  int newPin = server.arg("feederPin").toInt();

  bool isValidConfigPin = true;
#if defined(ESP8266)
  if (newPin < 0 || newPin > 16) isValidConfigPin = false;
  if (newPin == 1 || newPin == 3) isValidConfigPin = false; 
#elif defined(ESP32)
  if (newPin < 0 || newPin > 39) isValidConfigPin = false;
  if (newPin >=34 && newPin <=39) isValidConfigPin = false; 
#endif

  if (!isValidConfigPin) {
    server.send(400, "text/plain", "Número do pino inválido ou reservado.");
    return;
  }

  if (newPin != currentFeederPin) {
    if(isOutputActive) { 
        stopOutput();
        String logPinChange = timeStr(now()) + " -> Saída desativada devido à mudança de pino.";
        Serial.println(logPinChange);
        eventLog += logPinChange + "\n";
    }
    currentFeederPin = newPin;
    pinMode(currentFeederPin, OUTPUT);
    digitalWrite(currentFeederPin, LOW);
    Serial.printf("Pino de Saída alterado para GPIO: %d\n", currentFeederPin);
  }

  saveConfig();

  String logMsg = timeStr(now()) + " -> Pino de Saída definido para GPIO " + String(currentFeederPin);
  eventLog += logMsg + "\n";
  Serial.println(logMsg);

  server.send(200, "text/plain", "Pino de Saída salvo.");
}

void handleStatus() {
  DynamicJsonDocument doc(256);
  doc["is_feeding"] = isOutputActive; // 'is_feeding' no JSON representa isOutputActive
  doc["custom_rules_enabled"] = customEnabled;

  String activeRule = "none";
  long timeRemaining = -1;

  if (customEnabled) {
    time_t currentTime = now();
    if (isOutputActive && ruleHighDT != 0) {
      int ihTimeSec = parseRuleTime("IH", customSchedule);
      if (ihTimeSec != -1 && ihTimeSec > 0) {
        activeRule = "IH";
        timeRemaining = ihTimeSec - (currentTime - ruleHighDT);
        if (timeRemaining < 0) timeRemaining = 0;
      }
    } else if (!isOutputActive && ruleLowDT != 0) {
      int ilTimeSec = parseRuleTime("IL", customSchedule);
      if (ilTimeSec != -1 && ilTimeSec > 0) {
        activeRule = "IL";
        timeRemaining = ilTimeSec - (currentTime - ruleLowDT);
        if (timeRemaining < 0) timeRemaining = 0;
      }
    }
  }

  doc["active_custom_rule"] = activeRule;
  doc["custom_rule_time_remaining"] = timeRemaining;

  String jsonResponse;
  serializeJson(doc, jsonResponse);
  server.send(200, "application/json", jsonResponse);
}

void handleDeactivateOutputNow() { // Renomeado de handleStopFeedNow
  if (isOutputActive) { 
    String logIntent = timeStr(now()) + " -> Comando manual (web) recebido para DESATIVAR SAÍDA.";
    Serial.println(logIntent); eventLog += logIntent + "\n";
    stopOutput();
    server.send(200, "text/plain", "Saída desativada.");
  } else {
    server.send(400, "text/plain", "Nenhuma saída ativa para desativar.");
  }
}


void handleRoot() {
  String page = FPSTR(htmlPage);
  int rssi    = WiFi.RSSI();
  int quality = map(constrain(rssi, -90, -30), -90, -30, 0, 100);

  page.replace("%MANUAL%", formatHHMMSS(manualDurationSec));
  page.replace("%WIFI_QUALITY%", String(quality));
  page.replace("%OUTPUT_PIN_VALUE%", String(currentFeederPin)); // Placeholder atualizado

  String scheduleList_str;
  for (int i = 0; i < scheduleCount; i++) {
    if (i > 0) scheduleList_str += ",";
    scheduleList_str += formatHHMMSS(schedules[i].timeSec);
    scheduleList_str += "|";
    scheduleList_str += formatHHMMSS(schedules[i].durationSec);
  }
  page.replace("%SCHEDULES%", scheduleList_str);
  page.replace("%CUSTOM_RULES%", customSchedule);
  page.replace("%TOGGLE_BUTTON%", customEnabled ? "Desativar Regras" : "Ativar Regras");
  page.replace("%STATUS_CLASS%", isOutputActive ? "feeding" : (WiFi.status() == WL_CONNECTED ? "active" : "")); 

  server.send(200, "text/html", page);
}

void handleActivateOutputNow() { // Renomeado de handleFeedNow
  unsigned long current_millis = millis();

  if (isOutputActive) { 
    server.send(409, "text/plain", "Saída já está ativa");
    return;
  }
  if (current_millis - lastTriggerMs < FEED_COOLDOWN * 1000UL && lastTriggerMs != 0) {
    server.send(429, "text/plain", "Aguarde o intervalo (" + String(FEED_COOLDOWN) + "s) entre ativações");
    return;
  }

  String logIntent = timeStr(now()) + " -> Comando manual (web) recebido para ATIVAR SAÍDA.";
  Serial.println(logIntent); eventLog += logIntent + "\n";

  startOutput(manualDurationSec);
  server.send(200, "text/plain", "Saída ativada");
}


void handleSetManualDuration() {
  if (! server.hasArg("manualDuration")) {
    server.send(400, "text/plain", "Parâmetro 'manualDuration' ausente");
    return;
  }

  String duration_str = server.arg("manualDuration");
  int secs = parseHHMMSS(duration_str);

  if (secs <= 0 || secs > MAX_FEED_DURATION) {
    server.send(400, "text/plain", "Duração inválida (1s a " + formatHHMMSS(MAX_FEED_DURATION) + ")");
    return;
  }

  manualDurationSec = secs;
  saveConfig();

  String ts = timeStr(now());
  String ev = ts + " -> Duração manual de ativação definida para " + duration_str;
  eventLog += ev + "\n";
  Serial.println(ev);

  server.send(200, "text/plain", "Duração salva");
}


void handleSetSchedules() {
  if (server.hasArg("schedules")) {
    String scheduleStr_arg = server.arg("schedules");

    scheduleCount = 0;
    for(int k=0; k < MAX_SLOTS; ++k) {
        schedules[k].lastTriggerDay = -1;
    }

    if (scheduleStr_arg.length() > 0) {
      int start = 0;
      while (start < scheduleStr_arg.length() && scheduleCount < MAX_SLOTS) {
        int commaPos = scheduleStr_arg.indexOf(',', start);
        if (commaPos < 0) commaPos = scheduleStr_arg.length();

        String token = scheduleStr_arg.substring(start, commaPos);
        int pipePos = token.indexOf('|');

        if (pipePos > 0) {
          int timeSec_val = parseHHMMSS(token.substring(0, pipePos));
          int durationSec_val = parseHHMMSS(token.substring(pipePos + 1));

          if (timeSec_val >= 0 && durationSec_val > 0 && durationSec_val <= MAX_FEED_DURATION) {
            schedules[scheduleCount] = { timeSec_val, durationSec_val, -1 };
            scheduleCount++;
          } else {
             Serial.println("Agendamento inválido descartado: " + token);
          }
        }
        start = commaPos + 1;
      }
    }

    saveConfig();
    server.send(200, "text/plain", "Agendamentos salvos");

    String logMsg = timeStr(now()) + " -> " + String(scheduleCount) + " agendamentos salvos.";
    Serial.println(logMsg);
    eventLog += logMsg + "\n";

  } else {
    server.send(400, "text/plain", "Parâmetro 'schedules' ausente");
  }
}

void handleSetCustomRules() {
  if (!server.hasArg("rules")) {
    server.send(400, "text/plain", "Parâmetro 'rules' ausente");
    return;
  }

  String newRules = server.arg("rules");

  if (newRules.length() >= sizeof(customSchedule)) {
    server.send(400, "text/plain", "Regras muito longas (máx " + String(sizeof(customSchedule)-1) + " caracteres)");
    return;
  }

  strncpy(customSchedule, newRules.c_str(), sizeof(customSchedule) - 1);
  customSchedule[sizeof(customSchedule) - 1] = '\0';

  saveConfig();

  time_t t_now = now();
  String current_ts_log = timeStr(t_now);
  String log_prefix_rules_saved = current_ts_log + " -> Novas regras personalizadas salvas";

  if (customEnabled) {
    if (digitalRead(currentFeederPin) == LOW) {
      ruleLowDT = t_now;
      ruleHighDT = 0;
      String logMsgDetail = log_prefix_rules_saved + " (regras ATIVADAS). Saída DESLIGADA. (Re)iniciando contagem IL.";
      Serial.println(logMsgDetail);
      eventLog += logMsgDetail + "\n";
    } else {
      ruleHighDT = t_now;
      ruleLowDT = 0;
      String logMsgDetail = log_prefix_rules_saved + " (regras ATIVADAS). Saída LIGADA. (Re)iniciando contagem IH.";
      Serial.println(logMsgDetail);
      eventLog += logMsgDetail + "\n";
    }
  } else {
    ruleHighDT = 0; ruleLowDT = 0;
    String logMsgDetail = log_prefix_rules_saved + " (regras DESATIVADAS). Contadores IH/IL permanecem zerados.";
    Serial.println(logMsgDetail);
    eventLog += logMsgDetail + "\n";
  }

  server.send(200, "text/plain", "Regras salvas");
  String logContent = current_ts_log + " -> Conteúdo das regras atualizado para: \"" + String(customSchedule) + "\"";
  Serial.println(logContent);
  eventLog += logContent + "\n";
}


static bool notifiedStart = false;

void handleToggleCustomRules() {
  customEnabled = !customEnabled;
  saveConfig();
  server.send(200, "text/plain", customEnabled ? "Regras ativadas" : "Regras desativadas");

  Serial.print(timeStr(now()) + " -> Custom rules ");
  Serial.println(customEnabled ? "ENABLED" : "DISABLED");

  time_t t_now = now();
  String current_ts_log = timeStr(t_now);

  if (customEnabled) {
    if (digitalRead(currentFeederPin) == LOW) {
      ruleLowDT = t_now;
      ruleHighDT = 0;
      String logMsg = current_ts_log + " -> Regras avançadas ATIVADAS. Saída DESLIGADA. Iniciando contagem para IL (se houver).";
      Serial.println(logMsg);
      eventLog += logMsg + "\n";
    } else {
      ruleHighDT = t_now;
      ruleLowDT = 0;
      String logMsg = current_ts_log + " -> Regras avançadas ATIVADAS. Saída LIGADA. Iniciando contagem para IH (se houver).";
      Serial.println(logMsg);
      eventLog += logMsg + "\n";
    }
    notifiedStart = false;
  } else {
    ruleHighDT = 0;
    ruleLowDT = 0;
    String logMsg = current_ts_log + " -> Regras avançadas DESATIVADAS. Timers IH/IL zerados.";
    Serial.println(logMsg);
    eventLog += logMsg + "\n";
  }
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
        Serial.println("Still failed after format! Halting.");
        emergencyStop();
      }
    } else {
      Serial.println("Format failed! Halting.");
      emergencyStop();
    }
  }
#else // ESP32
  if (!FS.begin(true)) {
    Serial.println("SPIFFS mount failed, rebooting...");
    ESP.restart();
  }
#endif

  if (!FS.exists(CONFIG_PATH)) {
    Serial.println("No config file found, using defaults and creating one.");
    currentFeederPin = defaultFeederPin;
    manualDurationSec = 5;
    customSchedule[0] = '\0';
    customEnabled = false;
    scheduleCount = 0;
    saveConfig();
    return;
  }

  File file = FS.open(CONFIG_PATH, "r");
  if (!file) {
    Serial.println("Failed to open config file for reading. Using defaults.");
    currentFeederPin = defaultFeederPin;
    return;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    Serial.println("Using default config and attempting to re-save.");
    FS.remove(CONFIG_PATH);
    currentFeederPin = defaultFeederPin;
    manualDurationSec = 5;
    customSchedule[0] = '\0';
    customEnabled = false;
    scheduleCount = 0;
    saveConfig();
    return;
  }

  currentFeederPin = doc["feederPin"] | defaultFeederPin;
  manualDurationSec = doc["manualDuration"] | 5;

  const char *rules_ptr = doc["customSchedule"];
  if (rules_ptr) {
    strncpy(customSchedule, rules_ptr, sizeof(customSchedule) -1);
    customSchedule[sizeof(customSchedule) -1] = '\0';
  } else {
    customSchedule[0] = '\0';
  }
  customEnabled = doc["customEnabled"] | false;

  scheduleCount = 0;
  if (doc.containsKey("schedules")) {
    JsonArray arr = doc["schedules"].as<JsonArray>();
    for (JsonObject o : arr) {
      if (scheduleCount >= MAX_SLOTS) break;
      schedules[scheduleCount].timeSec = o["time"] | 0;
      schedules[scheduleCount].durationSec = o["duration"] | 5;
      schedules[scheduleCount].lastTriggerDay = o["lastTriggerDay"] | -1;
      scheduleCount++;
    }
  }

  Serial.println("Configuration loaded:");
  Serial.printf(" • Pino de Saída: GPIO %d\n", currentFeederPin);
  Serial.printf(" • Duração manual de ativação: %s\n", formatHHMMSS(manualDurationSec).c_str());
  Serial.printf(" • %d agendamentos:\n", scheduleCount);
  for (int i = 0; i < scheduleCount; i++) {
    char timeBuf[9], durationBuf[9];
    formatHHMMSS(schedules[i].timeSec, timeBuf, sizeof(timeBuf));
    formatHHMMSS(schedules[i].durationSec, durationBuf, sizeof(durationBuf));
    Serial.printf("    – %s por %s (Último dia acionado: %d)\n",
                  timeBuf, durationBuf, schedules[i].lastTriggerDay);
  }
  Serial.printf(" • Regras personalizadas: \"%s\" (%s)\n",
                customSchedule,
                customEnabled ? "enabled" : "disabled");
}

void saveConfig() {
  DynamicJsonDocument doc(2048);
  doc["feederPin"] = currentFeederPin;
  doc["manualDuration"] = manualDurationSec;
  doc["customSchedule"] = customSchedule;
  doc["customEnabled"] = customEnabled;

  JsonArray arr = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount && i < MAX_SLOTS; ++i) {
    JsonObject o = arr.createNestedObject();
    o["time"] = schedules[i].timeSec;
    o["duration"] = schedules[i].durationSec;
    o["lastTriggerDay"] = schedules[i].lastTriggerDay;
  }

  File file = FS.open(CONFIG_PATH, "w");
  if (!file) {
    Serial.println("Failed to open config file for writing");
    return;
  }
  if (serializeJson(doc, file) == 0) {
    Serial.println("Failed to write to config file");
  }
  file.close();
  Serial.println(timeStr(now()) + " -> Configuration saved.");
}


// =============== Hardware Control =============== //
void setupHardware() {
  pinMode(currentFeederPin, OUTPUT);
  digitalWrite(currentFeederPin, LOW);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH);

  #ifdef SONOFF_BASIC
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  #endif
}

void updateStatusLED() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  unsigned long current_millis = millis();

  if (isOutputActive) { 
    if (current_millis - lastBlink > 200) {
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState ? LOW : HIGH);
      lastBlink = current_millis;
    }
  } else { 
    if (WiFi.status() == WL_CONNECTED) { 
      digitalWrite(STATUS_LED_PIN, LOW);
    } else { 
      if (current_millis - lastBlink > 1000) {
        ledState = !ledState;
        digitalWrite(STATUS_LED_PIN, ledState ? LOW : HIGH);
        lastBlink = current_millis;
      }
    }
  }
}

void emergencyStop() {
  Serial.println("EMERGENCY STOP ACTIVATED!");
  digitalWrite(currentFeederPin, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  while(true) {
    digitalWrite(STATUS_LED_PIN, LOW); delay(100);
    digitalWrite(STATUS_LED_PIN, HIGH); delay(100);
  }
}

// =============== Network Setup =============== //
void setupNetwork() {
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(30);

  if (!wifiManager.autoConnect("TemporizadorAP")) { 
    Serial.println("Falha ao conectar ao WiFi e o portal de configuração expirou. Reiniciando...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi conectado!");
  Serial.print("IP address: "); Serial.println(WiFi.localIP());

  Serial.println("Configurando time para UTC (GMT+0) para sincronização NTP inicial...");
  configTime(0, 0, NTP_SERVER);

  Serial.print("Aguardando sincronização NTP (UTC)...");
  time_t utcTime = 0;
  int tentativasNTP = 0;
  bool ntpUtcSuccess = false;

  while (tentativasNTP < 60) {
    utcTime = time(nullptr);
    int current_year = year(utcTime);
    Serial.printf("\nTentativa NTP (UTC) %d: utcTime = %lu, ano = %d", tentativasNTP + 1, (unsigned long)utcTime, current_year);
    if (current_year > 2023) { // Ano válido para considerar NTP bem-sucedido
        ntpUtcSuccess = true;
        break;
    }
    Serial.print(".");
    delay(500);
    tentativasNTP++;
  }
  Serial.println();

  if (ntpUtcSuccess) {
    Serial.print("Sincronização NTP (UTC) concluída. Tempo UTC: ");
    char utcBuf[25];
    sprintf(utcBuf, "%04d-%02d-%02d %02d:%02d:%02d UTC", year(utcTime), month(utcTime), day(utcTime), hour(utcTime), minute(utcTime), second(utcTime));
    Serial.println(utcBuf);

    time_t localTime = utcTime + GMT_OFFSET_SEC + DAYLIGHT_OFFSET_SEC;
    setTime(localTime); // Define o tempo na TimeLib

    Serial.print("Hora local (NTP) definida na TimeLib: ");
    Serial.println(getCurrentDateTimeString());

    if (rtcInitialized) {
        rtc.adjust(DateTime(year(), month(), day(), hour(), minute(), second()));
        Serial.println("RTC DS3231 sincronizado COM a hora local obtida via NTP.");
    } else {
        Serial.println("RTC DS3231 não presente/inicializado. Hora do sistema definida apenas via NTP.");
    }
  } else {
    Serial.println("Falha ao obter hora válida do NTP (UTC) após várias tentativas.");
    if (rtcInitialized) {
        if (!rtc.lostPower()) { // RTC está presente E indica que tem hora válida
            Serial.println("NTP falhou. Usando hora do RTC DS3231 como fallback (RTC indica hora válida).");
            syncTimeLibWithRTC(); // Sincroniza TimeLib a partir do RTC
            Serial.print("Hora atual (RTC fallback): ");
            Serial.println(getCurrentDateTimeString());
        } else { // RTC está presente MAS indica perda de energia
            Serial.println("NTP falhou. RTC DS3231 presente, mas indica perda de energia (hora do RTC pode ser inválida).");
            Serial.println("Sistema operará sem hora sincronizada confiavelmente.");
            // Opcional: definir TimeLib para um valor padrão como 0 ou tempo de compilação
            // setTime(0); // Exemplo: reseta para o início da epoch Unix
        }
    } else { // NTP falhou E RTC não está presente/inicializado
        Serial.println("NTP falhou. RTC DS3231 não presente/inicializado.");
        Serial.println("Sistema operará sem hora sincronizada.");
        // Opcional: definir TimeLib para um valor padrão
        // setTime(0);
    }
  }
}

// Handler para obter os eventos acumulados para a interface web
void handleGetEvents() {
  if (eventLog.length() > 0) {
    server.send(200, "text/plain", eventLog);
    eventLog = "";
  } else {
    server.send(200, "text/plain", "");
  }
}


// =============== Main Setup =============== //
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== Iniciando Temporizador Inteligente ==="); 

  loadConfig();
  setupHardware();

  #if defined(ESP8266)
    Wire.begin(D2, D1);
  #elif defined(ESP32)
    Wire.begin();
  #else
    Wire.begin();
  #endif

  if (rtc.begin()) {
    rtcInitialized = true;
    Serial.println("RTC DS3231 encontrado e inicializado.");
    if (rtc.lostPower()) {
      Serial.println("RTC indica perda de energia (lostPower = true). Hora do RTC pode ser inválida até sincronização NTP.");
    } else {
      Serial.println("RTC com hora aparentemente válida (lostPower = false). Aguardando NTP para possível ajuste ou uso como fallback.");
      // Não sincronizar TimeLib com RTC aqui; NTP é prioritário.
      // A hora do RTC será usada como fallback em setupNetwork() se NTP falhar.
    }
  } else {
    Serial.println("RTC DS3231 não encontrado. O sistema dependerá exclusivamente do NTP para a hora.");
    rtcInitialized = false;
  }

  setupNetwork(); // Tenta NTP primeiro, depois RTC como fallback.

  // Inicializa timers de regras customizadas se estiverem ativas no boot
  if (customEnabled) {
      time_t t_now = now(); // 'now()' aqui usará o tempo já definido por NTP ou RTC fallback
      if (digitalRead(currentFeederPin) == LOW) {
          ruleLowDT = t_now; ruleHighDT = 0;
          Serial.println(timeStr(t_now) + " -> Inicialização: Regras custom ATIVADAS, Saída DESLIGADA. Contagem IL iniciada.");
      } else {
          ruleHighDT = t_now; ruleLowDT = 0;
          Serial.println(timeStr(t_now) + " -> Inicialização: Regras custom ATIVADAS, Saída LIGADA. Contagem IH iniciada.");
      }
  } else {
      ruleHighDT = 0; ruleLowDT = 0;
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/rssi", HTTP_GET, handleRssi);
  server.on("/time", HTTP_GET, handleTime);
  server.on("/nextTriggerTime", HTTP_GET, handleNextTriggerTime);
  server.on("/setFeederPin", HTTP_POST, handleSetOutputPin); // Endpoint JS usa 'feederPin', handler C++ renomeado
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/stopFeedNow", HTTP_POST, handleDeactivateOutputNow); // Endpoint JS usa 'stopFeedNow'
  server.on("/feedNow", HTTP_POST, handleActivateOutputNow);     // Endpoint JS usa 'feedNow'
  server.on("/setManualDuration", HTTP_POST, handleSetManualDuration);
  server.on("/setSchedules", HTTP_POST, handleSetSchedules);
  server.on("/setCustomRules", HTTP_POST, handleSetCustomRules);
  server.on("/toggleCustomRules", HTTP_POST, handleToggleCustomRules);
  server.on("/events", HTTP_GET, handleGetEvents);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Rota nao encontrada");
  });

  server.begin();
  Serial.println("Servidor HTTP iniciado.");

  Serial.println("--- Configuração Final e Status ---");
  Serial.printf("Hora atual do sistema: %s\n", getCurrentDateTimeString().c_str());
  Serial.printf("Pino de Saída: GPIO %d\n", currentFeederPin);
  Serial.printf("Duração manual de ativação: %s\n", formatHHMMSS(manualDurationSec).c_str());
  Serial.printf("Agendamentos: %d\n", scheduleCount);
  Serial.printf("Regras personalizadas: \"%s\" [%s]\n",
                customSchedule,
                customEnabled ? "ativadas" : "desativadas");
  if (customEnabled) {
    Serial.printf("  Timers de regra no boot: ruleHighDT=%lu, ruleLowDT=%lu\n", (unsigned long)ruleHighDT, (unsigned long)ruleLowDT);
  }
  Serial.println("===================================");
}


// =============== Main Loop =============== //
void loop() {
  if (rtcInitialized && (millis() % 300000 == 0)) { // A cada 5 minutos
    // A função syncTimeLibWithRTC() já tem uma verificação interna de validade do ano.
    // Se o RTC foi ajustado pelo NTP, esta sincronização mantém TimeLib alinhada com essa hora.
    // Se NTP falhou e RTC foi usado como fallback (e era válido), mantém TimeLib alinhada com RTC.
    // Se NTP falhou e RTC também estava inválido, a verificação no syncTimeLibWithRTC impedirá a sincronização.
    syncTimeLibWithRTC();
    Serial.println(timeStr(now()) + " -> Sincronização periódica TimeLib <- RTC realizada (se RTC válido).");
  } else if (!rtcInitialized && (millis() % 60000 == 0)) { // A cada minuto, se RTC não foi inicializado
    Serial.println(timeStr(now()) + " -> AVISO NO LOOP: RTC não foi inicializado. Sincronização periódica com RTC desativada.");
  }

  server.handleClient();
  updateStatusLED();

  #ifdef SONOFF_BASIC
    bool rawState = digitalRead(BUTTON_PIN);
    if (rawState != lastButtonState) {
      lastDebounceMs = millis();
    }
    if (millis() - lastDebounceMs > DEBOUNCE_DELAY) {
      static bool buttonPressHandled = false;
      if (rawState == LOW && !buttonPressHandled) {
        if (!isOutputActive && (millis() - lastTriggerMs >= FEED_COOLDOWN * 1000UL || lastTriggerMs == 0)) {
          String logIntent = timeStr(now()) + " -> Botão físico pressionado para LIGAR SAÍDA.";
          Serial.println(logIntent); eventLog += logIntent + "\n";
          startOutput(manualDurationSec);
        } else {
          String msg = timeStr(now()) + " -> Botão manual: cooldown ou saída já ativa.";
          Serial.println(msg); eventLog += msg + "\n";
        }
        buttonPressHandled = true;
      }
      if (rawState == HIGH) {
        buttonPressHandled = false;
      }
    }
    lastButtonState = rawState;
  #endif

  // Parada automática da ativação da saída por tempo (para durações definidas)
  if (isOutputActive && outputActivationDurationMs > 0 && outputActivationDurationMs <= (unsigned long)MAX_FEED_DURATION * 1000UL) {
      if (millis() - outputActivationStartMs >= outputActivationDurationMs) {
        String logIntent = timeStr(now()) + " -> Duração da ativação (" + formatHHMMSS(outputActivationDurationMs/1000) + ") expirou (via loop timer).";
        Serial.println(logIntent);
        eventLog += logIntent + "\n";
        stopOutput();
      }
  }


  if (customEnabled) {
    if (!notifiedStart) {
      String ts = timeStr(now());
      Serial.println(ts + " -> Loop: Regras customizadas ATIVAS. Processando scheduleChk.");
      eventLog += ts + " -> Loop: Regras customizadas ATIVAS. Processando scheduleChk.\n";
      notifiedStart = true;
    }
    scheduleChk(customSchedule, currentFeederPin);
  } else {
    if (notifiedStart) {
        String ts = timeStr(now());
        Serial.println(ts + " -> Loop: Regras customizadas DESATIVADAS. Processando checkSchedules.");
        eventLog += ts + " -> Loop: Regras customizadas DESATIVADAS. Processando checkSchedules.\n";
        notifiedStart = false;
    }
    if (!isOutputActive) { // Só verifica agendamentos normais se a saída não estiver ativa
        checkSchedules();
    }
  }

  yield();
}


// Inicia ativação da saída por 'durationSec' segundos
void startOutput(unsigned long durationSec) { 
  if (isOutputActive) {
    return;
  }

  unsigned long current_millis = millis();
  if (lastTriggerMs != 0 && (current_millis - lastTriggerMs < FEED_COOLDOWN * 1000UL)) {
    String cooldownMsg = timeStr(now()) + " -> Tentativa de LIGAR SAÍDA em cooldown ("+String(FEED_COOLDOWN)+"s). Ignorado.";
    Serial.println(cooldownMsg);
    eventLog += cooldownMsg + "\n";
    return;
  }

  isOutputActive = true; 
  outputActivationStartMs = current_millis;

  unsigned long actualDurationSec = durationSec;

  if (customEnabled) {
      int ihTimeSec = parseRuleTime("IH", customSchedule);
      if (ihTimeSec != -1 && ihTimeSec > 0) {
          actualDurationSec = min((unsigned long)ihTimeSec, durationSec);
      } else {
          if (durationSec == (MAX_FEED_DURATION + 1)) {
              actualDurationSec = MAX_FEED_DURATION + 1;
          } else {
              actualDurationSec = min(durationSec, (unsigned long)MAX_FEED_DURATION);
          }
      }
  } else {
      actualDurationSec = min(durationSec, (unsigned long)MAX_FEED_DURATION);
  }

  if (actualDurationSec == 0 && durationSec > 0) {
      actualDurationSec = 1;
  }
  if (actualDurationSec > (MAX_FEED_DURATION + 1) ) {
      actualDurationSec = MAX_FEED_DURATION + 1;
  }

  if (actualDurationSec > MAX_FEED_DURATION && actualDurationSec == (MAX_FEED_DURATION + 1)) {
      outputActivationDurationMs = actualDurationSec * 1000UL;
  } else if (actualDurationSec > 0) {
      outputActivationDurationMs = actualDurationSec * 1000UL;
  } else {
      outputActivationDurationMs = 0;
  }

  digitalWrite(currentFeederPin, HIGH);
  lastTriggerMs = current_millis;

  time_t currentTime = now();
  String logMsg = timeStr(currentTime) + " -> Saída LIGADA.";

  if (actualDurationSec > MAX_FEED_DURATION) {
    logMsg += " (Tempo indefinido, aguardando regra IH ou parada manual/outra regra).";
  } else if (actualDurationSec > 0) {
    logMsg += " Duração programada: " + formatHHMMSS(actualDurationSec) + ".";
  } else {
    logMsg += " (Duração programada: 0s, desligará imediatamente).";
  }


  if (customEnabled) {
      ruleHighDT = currentTime;
      ruleLowDT = 0;
      int ihTimeSec_log = parseRuleTime("IH", customSchedule);
      if (ihTimeSec_log != -1 && ihTimeSec_log > 0 && actualDurationSec == (unsigned long)ihTimeSec_log && actualDurationSec <= MAX_FEED_DURATION) {
          time_t predictedOffTime = currentTime + ihTimeSec_log;
          logMsg += " Desligamento por IH ("+formatHHMMSS(ihTimeSec_log)+") previsto para " + timeStr(predictedOffTime) + ".";
      } else if (actualDurationSec > 0 && actualDurationSec <= MAX_FEED_DURATION) {
          logMsg += " Desligamento por tempo (loop timer ou IH em scheduleChk) previsto para " + timeStr(currentTime + actualDurationSec) + ".";
      }
  } else if (actualDurationSec > 0 && actualDurationSec <= MAX_FEED_DURATION) {
      logMsg += " Desligamento por tempo (loop timer) previsto para " + timeStr(currentTime + actualDurationSec) + ".";
  }

  Serial.println(logMsg);
  eventLog += logMsg + "\n";

  if (outputActivationDurationMs == 0) {
      String immediateOffLog = timeStr(now()) + " -> Duração de 0s, desligando saída imediatamente em startOutput.";
      Serial.println(immediateOffLog);
      eventLog += immediateOffLog + "\n";
      stopOutput();
  }
}

// Interrompe ativação da saída imediatamente
void stopOutput() { 
  if (isOutputActive) { 
    digitalWrite(currentFeederPin, LOW);
    isOutputActive = false;
    outputActivationDurationMs = 0;
    time_t stopTime = now();
    String logMsg = timeStr(stopTime) + " -> Saída DESLIGADA.";

    if (customEnabled) {
      ruleLowDT = stopTime;
      ruleHighDT = 0;
      int ilTimeSec = parseRuleTime("IL", customSchedule);
      if (ilTimeSec != -1 && ilTimeSec > 0) {
        time_t predictedOnTime = stopTime + ilTimeSec;
        logMsg += " Próxima ativação por IL (" + formatHHMMSS(ilTimeSec) + ") prevista para " + timeStr(predictedOnTime) + ".";
      }
    }
    Serial.println(logMsg);
    eventLog += logMsg + "\n";
  }
}

