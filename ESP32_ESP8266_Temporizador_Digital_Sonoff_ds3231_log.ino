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
const int FEED_COOLDOWN = 10;       // Segundos entre alimentações
const int MAX_FEED_DURATION = 300;    // 5 minutos no máximo para alimentação manual/agendada
// const unsigned long CUSTOM_RULE_DEFAULT_ON_DURATION_MS = (MAX_FEED_DURATION + 60) * 1000UL; // Não usado diretamente

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
  int durationSec;    // Duração da alimentação em segundos
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
unsigned long manualDurationSec = 5; // Duração padrão para alimentação manual (em segundos)
char customSchedule[512] = ""; // String para armazenar as regras personalizadas
bool customEnabled = false; // Flag para indicar se as regras personalizadas estão ativas
volatile bool isFeeding = false; // Flag para indicar se o alimentador está ativo (HIGH)
unsigned long feedStartMs = 0; // Timestamp (millis) de quando a alimentação começou
unsigned long feedDurationMs = 0; // Duração programada da alimentação atual (em milissegundos)
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
    textarea { /* Estilo adicionado para textarea */
      width: 100%;
      padding: 10px;
      border: 1px solid #ddd;
      border-radius: 4px;
      margin-bottom: 10px;
      box-sizing: border-box;
      font-family: inherit; /* Garante que a textarea use a mesma fonte do corpo */
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
    #eventConsole { /* ID alterado de #console para #eventConsole */
      background: #111;
      color: #0f0;
      font-family: monospace;
      white-space: pre-wrap;
      padding: 10px;
      height: 120px;
      overflow-y: auto;
      border: 1px solid #444;
      border-radius: 4px;
      margin-top: 10px; /* Espaçamento adicionado */
    }
    /* #eventConsole:focus { outline: none; } Não é mais necessário pois não é editável */
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
  </style>
</head>
<body>
  <h1>Temporizador Inteligente</h1>

  <section class="status-indicator">
    <div class="led %STATUS_CLASS%"></div>
    <div id="currentTime">--:--:--</div>
    <div id="wifiQuality">Wi‑Fi: %WIFI_QUALITY%%</div>
  </section>

  <section class="card">
    <button id="manualFeed">Ativar Agora</button>
    <button id="stopFeed" class="warning" style="display: none;">Parar Alimentação</button>
    <div id="manualFeedMessage" class="message"></div>
  </section>

  <section class="card">
    <form id="manualForm">
      <label for="manualInterval">Duração do Pulso (HH:MM:SS)</label>
      <input type="time" id="manualInterval" step="1" value="%MANUAL%" />
      <button type="submit">Salvar Duração</button>
      <div id="manualFormMessage" class="message"></div>
    </form>
  </section>

  <section class="card"> <form id="feederPinForm">
      <label for="feederPin">Pino do Alimentador (GPIO):</label>
      <input type="number" id="feederPin" value="%FEEDER_PIN_VALUE%" min="0" max="39"> <button type="submit">Salvar Pino do Alimentador</button>
      <div id="feederPinFormMessage" class="message"></div>
    </form>
  </section>

  <section class="card">
    <form id="scheduleForm">
      <label for="newScheduleTime">Novo Horário (HH:MM:SS)</label>
      <input type="time" id="newScheduleTime" step="1" />
      <label for="newScheduleInterval">Duração (HH:MM:SS)</label>
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
    <label for="eventConsole" style="margin-top:15px;">Console de Eventos:</label>
    <div id="eventConsole"></div> </section>

  <div id="nextTrigger">Carregando...</div>
  <div id="customRuleCountdown"></div>
<script>
  // Função para adicionar zero à esquerda se n < 10
  function pad(n){ return n.toString().padStart(2,'0'); }
  // Converte HH:MM:SS para segundos totais
  function parseHHMMSS_to_secs(s){ const p=s.split(':').map(Number); return p[0]*3600+p[1]*60+p[2]; }
  // Formata segundos totais para HH:MM:SS
  function formatHHMMSS(secs){
    if (isNaN(secs) || secs < 0) return "00:00:00"; // Lida com valores inválidos
    return [Math.floor(secs/3600),Math.floor((secs%3600)/60),secs%60].map(pad).join(':');
  }

  // Atualiza a qualidade do WiFi periodicamente
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
  setInterval(updateWifiQuality, 5000); // A cada 5 segundos
  updateWifiQuality(); // Chamada inicial

  // Atualiza a hora atual periodicamente
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
  setInterval(updateCurrentTime, 1000); // A cada 1 segundo
  updateCurrentTime(); // Chamada inicial

  // Carrega agendamentos e duração manual dos placeholders
  let schedules = [], manualIntervalSecs = parseHHMMSS_to_secs("%MANUAL%");
  const rawSch = "%SCHEDULES%"; // Placeholder para agendamentos
  if(rawSch) schedules = rawSch.split(',').map(t=>{const a=t.split('|');return{time:a[0],interval:a[1]};});

  // Renderiza a lista de agendamentos na UI
  function renderSchedules(){
    const ul=document.getElementById('scheduleList'); ul.innerHTML='';
    schedules.forEach((o,i)=>{
      const li=document.createElement('li');
      li.innerHTML=`<span>${o.time} (Duração: ${o.interval})</span><button class="delete" data-index="${i}">×</button>`;
      li.querySelector('.delete').onclick=(e)=>{ // Botão de apagar agendamento
          schedules.splice(parseInt(e.target.dataset.index),1);
          renderSchedules();
          updateNextTrigger(); // Atualiza info do próximo acionamento
      };
      ul.appendChild(li);
    });
  }
  renderSchedules(); // Chamada inicial

  // Adiciona um novo agendamento (localmente na UI)
  document.getElementById('addSchedule').onclick=()=>{
    const t=document.getElementById('newScheduleTime').value, i=document.getElementById('newScheduleInterval').value;
    const rx=/^([0-9]{2}):([0-9]{2}):([0-9]{2})$/; // Regex para HH:MM:SS
    if(!t || !i) {showMessage('scheduleFormMessage','Preencha horário e duração','error');return;}
    if(!rx.test(t)||!rx.test(i)){showMessage('scheduleFormMessage','Use formato HH:MM:SS','error');return;}
    if (schedules.length >= 10) { // Limite de MAX_SLOTS
        showMessage('scheduleFormMessage', 'Máximo de 10 agendamentos atingido', 'error'); return;
    }
    schedules.push({time:t,interval:i});renderSchedules();
    showMessage('scheduleFormMessage','Agendamento adicionado localmente. Clique em "Salvar Agendamentos".','success');
  };

  // Salva a duração manual no servidor
  document.getElementById('manualForm').onsubmit=e=>{
    e.preventDefault();
    const d=document.getElementById('manualInterval').value, rx=/^([0-9]{2}):([0-9]{2}):([0-9]{2})$/;
    if(!rx.test(d)){showMessage('manualFormMessage','Use formato HH:MM:SS','error');return;}
    fetch('/setManualDuration',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`manualDuration=${d}`})
      .then(r=>{if(r.ok){showMessage('manualFormMessage','Duração salva','success');manualIntervalSecs=parseHHMMSS_to_secs(d); } else {r.text().then(txt => showMessage('manualFormMessage','Erro: ' + txt,'error'));}})
      .catch(_=>showMessage('manualFormMessage','Erro ao salvar','error'));
  };

  // Salva o pino do alimentador no servidor
  document.getElementById('feederPinForm').onsubmit = e => {
    e.preventDefault();
    const pin = document.getElementById('feederPin').value;
    if (isNaN(parseInt(pin)) || parseInt(pin) < 0 ) {
      showMessage('feederPinFormMessage', 'Número do pino inválido', 'error');
      return;
    }
    fetch('/setFeederPin', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: `feederPin=${pin}`
    })
    .then(r => {
      if (r.ok) {
        showMessage('feederPinFormMessage', 'Pino do alimentador salvo. Pode ser necessário reiniciar.', 'success');
      } else {
        r.text().then(txt => showMessage('feederPinFormMessage', 'Erro: ' + txt, 'error'));
      }
    })
    .catch(_ => showMessage('feederPinFormMessage', 'Erro ao salvar pino', 'error'));
  };

  // Salva todos os agendamentos no servidor
  document.getElementById('saveSchedules').onclick=()=>{
    const body='schedules='+encodeURIComponent(schedules.map(o=>`${o.time}|${o.interval}`).join(','));
    fetch('/setSchedules',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body})
      .then(r=>{if(r.ok){showMessage('scheduleFormMessage','Agendamentos salvos','success');updateNextTrigger();} else {r.text().then(txt => showMessage('scheduleFormMessage','Erro: ' + txt,'error'));}})
      .catch(_=>showMessage('scheduleFormMessage','Erro ao salvar','error'));
  };

  // Salva as regras personalizadas no servidor
  document.getElementById('saveRules').onclick=()=>{
    const rules=document.getElementById('customRulesInput').value; // MODIFICADO: lê do textarea
    fetch('/setCustomRules',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'rules='+encodeURIComponent(rules)})
      .then(r=>{if(r.ok)showMessage('customRulesMessage','Regras salvas','success');else {r.text().then(txt => showMessage('customRulesMessage','Erro: ' + txt,'error'));}})
      .catch(_=>showMessage('customRulesMessage','Erro ao salvar','error'));
  };

  // Alterna o estado das regras personalizadas (ativar/desativar)
  document.getElementById('toggleRules').onclick=()=>{
    fetch('/toggleCustomRules',{method:'POST'})
      .then(r=>{if(r.ok)location.reload();else {r.text().then(txt => showMessage('customRulesMessage','Erro: ' + txt,'error'));}}) // Recarrega a página para refletir o estado
      .catch(_=>showMessage('customRulesMessage','Erro ao alternar','error'));
  };

  // Botões de controle manual da alimentação
  const manualFeedButton = document.getElementById('manualFeed');
  const stopFeedButton = document.getElementById('stopFeed');

  manualFeedButton.onclick = () => {
    fetch('/feedNow', { method: 'POST' })
        .then(r => {
            if (r.ok) {
                showMessage('manualFeedMessage', 'Alimentação iniciada', 'success');
            } else {
                return r.text().then(txt => { throw new Error(txt); }); // Propaga o erro
            }
        })
        .catch(err => showMessage('manualFeedMessage', 'Erro: ' + err.message, 'error'))
        .finally(() => updateStatusAndRuleCountdown()); // Atualiza status independentemente do resultado
  };

  stopFeedButton.onclick = () => {
    fetch('/stopFeedNow', { method: 'POST' })
        .then(r => {
            if (r.ok) {
                showMessage('manualFeedMessage', 'Alimentação interrompida.', 'success');
            } else {
                 return r.text().then(txt => { throw new Error(txt); });
            }
        })
        .catch(err => showMessage('manualFeedMessage', 'Erro: ' + err.message, 'error'))
        .finally(() => updateStatusAndRuleCountdown());
  };

  let customRuleCountdownInterval = null; // Intervalo para o contador regressivo de regras custom

  // Atualiza o status do alimentador e o contador de regras personalizadas
  function updateStatusAndRuleCountdown() {
    fetch('/status') // Endpoint que retorna o estado atual
        .then(res => res.json())
        .then(data => {
            // Atualiza botões de controle manual
            if (data.is_feeding) {
                stopFeedButton.style.display = 'block';
                manualFeedButton.disabled = true;
            } else {
                stopFeedButton.style.display = 'none';
                manualFeedButton.disabled = false;
            }

            // Limpa contador anterior
            if (customRuleCountdownInterval) clearInterval(customRuleCountdownInterval);
            const ruleCountdownElement = document.getElementById('customRuleCountdown');

            // Se regras customizadas estão ativas e há uma regra IH/IL com tempo restante
            if (data.custom_rules_enabled && data.active_custom_rule !== "none" && data.custom_rule_time_remaining >= 0) {
                ruleCountdownElement.style.display = 'block';
                let totalSeconds = data.custom_rule_time_remaining;
                let ruleType = data.active_custom_rule;
                // Determina se o tempo restante é para LIGADO (IH) ou DESLIGADO (IL)
                let ruleState = ruleType === "IH" ? "LIGADO" : "DESLIGADO";

                ruleCountdownElement.textContent = `Regra Ativa (${ruleType}): Tempo ${ruleState} restante: ${formatHHMMSS(totalSeconds)}`;

                // Inicia um novo contador regressivo
                customRuleCountdownInterval = setInterval(() => {
                    if (totalSeconds <= 0) {
                        clearInterval(customRuleCountdownInterval);
                        ruleCountdownElement.textContent = "Regra Ativa: Verificando...";
                        setTimeout(updateStatusAndRuleCountdown, 1500); // Reconsulta o status
                        return;
                    }
                    totalSeconds--;
                    ruleCountdownElement.textContent = `Regra Ativa (${ruleType}): Tempo ${ruleState} restante: ${formatHHMMSS(totalSeconds)}`;
                }, 1000);
            } else {
                ruleCountdownElement.style.display = 'none'; // Esconde o contador
            }
        })
        .catch(_ => { // Em caso de erro na consulta de status
            stopFeedButton.style.display = 'none';
            manualFeedButton.disabled = false;
            if (customRuleCountdownInterval) clearInterval(customRuleCountdownInterval);
            document.getElementById('customRuleCountdown').style.display = 'none';
        });
  }
  setInterval(updateStatusAndRuleCountdown, 2500); // Atualiza status a cada 2.5 segundos
  updateStatusAndRuleCountdown(); // Chamada inicial

  let countdownInterval = null; // Intervalo para o contador regressivo do próximo agendamento
  // Atualiza a informação do próximo acionamento agendado
  function updateNextTrigger(){
    fetch('/nextTriggerTime')
        .then(res => res.text())
        .then(text => {
            if (countdownInterval) clearInterval(countdownInterval); // Limpa contador anterior
            const triggerElement = document.getElementById('nextTrigger');

            // Tenta extrair o tempo HH:MM:SS do texto de resposta
            const match = text.match(/Próxima em: (\d{2}):(\d{2}):(\d{2})/);
            const durationMatch = text.match(/\(duração (\d{2}):(\d{2}):(\d{2})\)/);
            let originalSuffix = ""; // Para manter o texto após o tempo (ex: duração)
            if (durationMatch) {
                originalSuffix = ` (duração ${durationMatch[1]}:${durationMatch[2]}:${durationMatch[3]})`;
            } else if (match) { // Se não há match de duração, pega o que vem depois do tempo
                const afterTime = text.substring(text.indexOf(match[0]) + match[0].length);
                if (afterTime.trim().length > 0) originalSuffix = afterTime;
            }

            if (match) { // Se um tempo HH:MM:SS foi encontrado
                let hours = parseInt(match[1]);
                let minutes = parseInt(match[2]);
                let seconds = parseInt(match[3]);
                let totalSeconds = hours * 3600 + minutes * 60 + seconds;

                if (totalSeconds >= 0) { // Se o tempo é válido
                    triggerElement.textContent = `Próxima em: ${pad(hours)}:${pad(minutes)}:${pad(seconds)}${originalSuffix}`;
                    // Inicia contador regressivo
                    countdownInterval = setInterval(() => {
                        if (totalSeconds <= 0) {
                            clearInterval(countdownInterval);
                            triggerElement.textContent = "Verificando próximo agendamento...";
                            setTimeout(updateNextTrigger, 1500); // Reconsulta
                            updateStatusAndRuleCountdown(); // Atualiza status geral também
                            return;
                        }
                        totalSeconds--;
                        const h = Math.floor(totalSeconds / 3600);
                        const m = Math.floor((totalSeconds % 3600) / 60);
                        const s = totalSeconds % 60;
                        triggerElement.textContent = `Próxima em: ${pad(h)}:${pad(m)}:${pad(s)}${originalSuffix}`;
                    }, 1000);
                } else { // Tempo inválido ou já passou
                     triggerElement.textContent = text;
                }
            } else { // Nenhum tempo HH:MM:SS encontrado, mostra o texto como está
                 triggerElement.textContent = text;
            }
        })
        .catch(_ => { // Erro ao buscar próximo acionamento
            if (countdownInterval) clearInterval(countdownInterval);
            document.getElementById('nextTrigger').textContent = 'Erro ao buscar próximo acionamento.';
        });
  }
  setInterval(updateNextTrigger, 30000); // Atualiza a cada 30 segundos (o contador interno é a cada 1s)
  updateNextTrigger(); // Chamada inicial

  // Mostra uma mensagem temporária na UI
  function showMessage(id,msg,type){
    const e=document.getElementById(id);e.textContent=msg;e.className='message '+type;
    setTimeout(()=>{e.textContent='';e.className='message';},5000); // Limpa após 5 segundos
  }

  // Busca e exibe logs de eventos do servidor
  setInterval(() => {
    fetch('/events')
      .then(res => res.text())
      .then(txt => {
        if (txt && txt.trim().length > 0) { // Se houver texto de log
          const con = document.getElementById('eventConsole'); // MODIFICADO: usa eventConsole
          const needsScroll = con.scrollHeight - con.scrollTop === con.clientHeight; // Verifica se o scroll está no fim
          con.innerText += (con.innerText ? '\n' : '') + txt.trim(); // Adiciona o novo log
          if(needsScroll) con.scrollTop = con.scrollHeight; // Mantém o scroll no fim se já estava
        }
      });
  }, 2000); // Busca logs a cada 2 segundos
</script>
</body>
</html>
)rawliteral";


/*******************************************************************************
* FUNÇÕES AUXILIARES DE TEMPO E RTC
*******************************************************************************/

// Função para sincronizar a biblioteca TimeLib com o RTC DS3231
void syncTimeLibWithRTC() {
  if (!rtcInitialized) { // Verifica se o RTC foi inicializado
    return;
  }

  DateTime nowRTC = rtc.now(); // Lê a hora atual do RTC
  // Verifica se o ano lido do RTC é inválido (ex: bateria do RTC falhou e resetou para um ano antigo)
  // A condição nowRTC.year() > 1970 é para evitar problemas com unixtime = 0
  if (nowRTC.year() < 2023 && nowRTC.year() > 1970) { // Considera anos válidos a partir de 2023
      // Loga um aviso apenas uma vez por minuto para não inundar o serial
      if (millis() % 60000 == 0) {
          Serial.printf("AVISO: RTC retornou data inválida no loop: %04d-%02d-%02d. Não sincronizando TimeLib com este valor.\n",
                        nowRTC.year(), nowRTC.month(), nowRTC.day());
      }
      return; // Não atualiza TimeLib se a data do RTC for suspeita
  }
  setTime(nowRTC.unixtime()); // Define a hora da TimeLib com base no unixtime do RTC
}

// Formata um timestamp (time_t) para uma string HH:MM (não mais usado extensivamente, preferir timeStr ou formatHHMMSS)
String hhmmStr(const time_t &t) {
  String s;
  if (hour(t) < 10) s += '0';
  s += String(hour(t)) + ':';
  if (minute(t) < 10) s += '0';
  s += String(minute(t));
  return s;
}

// Formata uma quantidade de segundos para uma string HH:MM:SS em um buffer fornecido
void formatHHMMSS(int secs, char *buf, size_t bufSize) {
  secs = abs(secs); // Garante que os segundos sejam positivos para formatação
  snprintf(buf, bufSize, "%02d:%02d:%02d",
           secs / 3600,        // Horas
           (secs % 3600) / 60, // Minutos
           secs % 60);         // Segundos
}

// Formata uma quantidade de segundos para uma string HH:MM:SS (retorna String)
String formatHHMMSS(int secs) {
  char buf[9]; // Buffer para "HH:MM:SS\0"
  formatHHMMSS(secs, buf, sizeof(buf));
  return String(buf);
}

// Calcula o dia do ano (1-365 ou 1-366 para ano bissexto)
int calculateDayOfYear(int y, int m, int d) {
  int daysInMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; // Dias em cada mês
  // Verifica se é ano bissexto
  if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
    daysInMonth[2] = 29; // Fevereiro tem 29 dias
  }
  int dayOfYear = 0;
  for (int i = 1; i < m; i++) { // Soma os dias dos meses anteriores
    dayOfYear += daysInMonth[i];
  }
  dayOfYear += d; // Adiciona os dias do mês atual
  return dayOfYear;
}

// Obtém o dia do ano atual
int getCurrentDayOfYear() {
  time_t t = now(); // Obtém o tempo atual da TimeLib
  return calculateDayOfYear(year(t), month(t), day(t));
}


// Obtém a hora atual em segundos desde a meia-noite
int getCurrentTimeInSec() {
  return hour() * 3600 + minute() * 60 + second(); // Usa funções da TimeLib
}

// Obtém a data e hora atuais formatadas como string "YYYY-MM-DD HH:MM:SS"
String getCurrentDateTimeString() {
  time_t t = now(); // Obtém o tempo atual da TimeLib
  char buf[20]; // Buffer para "YYYY-MM-DD HH:MM:SS\0"
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           year(t), month(t), day(t), hour(t), minute(t), second(t));
  return String(buf);
}

// Retorna uma string descrevendo o próximo acionamento agendado
String getNextTriggerTimeString() {
    if (customEnabled) { // Se regras personalizadas estão ativas, elas têm prioridade
        return "Regras personalizadas ativas.";
    }

    if (scheduleCount == 0) { // Se não há agendamentos
        return "Nenhum agendamento configurado.";
    }

    time_t currentTimeLib = now(); // Hora atual da TimeLib
    // Dia do ano atual, para verificar se um agendamento já ocorreu hoje
    int currentDayOfYearVal = calculateDayOfYear(year(currentTimeLib), month(currentTimeLib), day(currentTimeLib));

    long minNextTriggerUnixtime = -1; // Unixtime do próximo acionamento (inicializado como inválido)
    int nextDuration = 0; // Duração do próximo acionamento

    // Itera sobre todos os agendamentos configurados
    for (int i = 0; i < scheduleCount; i++) {
        // Cria um objeto DateTime para o horário do agendamento no dia de HOJE
        DateTime scheduleDateTimeToday(year(currentTimeLib), month(currentTimeLib), day(currentTimeLib),
                                       schedules[i].timeSec / 3600,
                                       (schedules[i].timeSec % 3600) / 60,
                                       schedules[i].timeSec % 60);

        time_t scheduleUnixtimeToday = scheduleDateTimeToday.unixtime(); // Converte para unixtime

        // Se o horário do agendamento para hoje já passou OU se já foi acionado hoje
        if (scheduleUnixtimeToday < currentTimeLib || schedules[i].lastTriggerDay == currentDayOfYearVal) {
            // Considera o agendamento para o dia de AMANHÃ
            DateTime scheduleDateTimeTomorrow = scheduleDateTimeToday + TimeSpan(1,0,0,0); // Adiciona 1 dia
            if (minNextTriggerUnixtime == -1 || scheduleDateTimeTomorrow.unixtime() < minNextTriggerUnixtime) {
                minNextTriggerUnixtime = scheduleDateTimeTomorrow.unixtime();
                nextDuration = schedules[i].durationSec;
            }
        } else { // O agendamento para hoje ainda não ocorreu e não foi acionado hoje
            if (minNextTriggerUnixtime == -1 || scheduleUnixtimeToday < minNextTriggerUnixtime) {
                minNextTriggerUnixtime = scheduleUnixtimeToday;
                nextDuration = schedules[i].durationSec;
            }
        }
    }

    if (minNextTriggerUnixtime != -1) { // Se um próximo acionamento foi encontrado
        time_t diffSeconds = minNextTriggerUnixtime - currentTimeLib; // Diferença em segundos até o próximo acionamento
        if (diffSeconds < 0) diffSeconds = 0; // Garante que não seja negativo

        char timeBuf[9]; // Buffer para HH:MM:SS
        formatHHMMSS(diffSeconds, timeBuf, sizeof(timeBuf)); // Formata o tempo restante

        char durationBuf[9]; // Buffer para HH:MM:SS
        formatHHMMSS(nextDuration, durationBuf, sizeof(durationBuf)); // Formata a duração

        return "Próxima em: " + String(timeBuf) + " (duração " + String(durationBuf) + ")";
    }
    return "Nenhum agendamento futuro encontrado."; // Caso nenhum agendamento válido seja encontrado
}


// =============== Schedule Checking =============== //
// Verifica os agendamentos padrão (não as regras personalizadas)
void checkSchedules() {
  if (isFeeding || customEnabled) return; // Não verifica se já está alimentando ou se regras customizadas estão ativas

  int today = getCurrentDayOfYear(); // Dia do ano atual
  int currentTime = getCurrentTimeInSec(); // Hora atual em segundos desde meia-noite

  for (int i = 0; i < scheduleCount; i++) {
    Schedule &s = schedules[i];
    // Verifica se é o horário do agendamento e se ainda não foi acionado hoje
    if (currentTime == s.timeSec && s.lastTriggerDay != today) {
      // Verifica o cooldown para evitar acionamentos repetidos muito rapidamente
      if (millis() - lastTriggerMs >= FEED_COOLDOWN * 1000UL || lastTriggerMs == 0) {
        s.lastTriggerDay = today; // Marca como acionado hoje
        // Log da intenção da regra de agendamento
        String logRuleIntent = timeStr(now()) + " -> Agendamento #" + String(i) + " (" + formatHHMMSS(s.timeSec) + ") detectado para LIGAR.";
        Serial.println(logRuleIntent);
        eventLog += logRuleIntent + "\n";

        startFeeding(s.durationSec); // Inicia a alimentação

        saveConfig(); // Salva o estado (lastTriggerDay)
      } else {
         String logMsg = getCurrentDateTimeString() + " -> Agendamento #" + String(i) + " ignorado (cooldown)";
         Serial.println(logMsg);
         eventLog += logMsg + "\n";
      }
    }
  }
}

// Converte uma string "YYYY-MM-DD HH:MM" para time_t
time_t parseDateTime(const String &s) {
  struct tm tm_struct; // Estrutura para armazenar data/hora
  // Tenta parsear a string
  if (sscanf(s.c_str(), " %4d-%2d-%2d %2d:%2d",
             &tm_struct.tm_year, &tm_struct.tm_mon, &tm_struct.tm_mday,
             &tm_struct.tm_hour, &tm_struct.tm_min)
      == 5) { // Se 5 campos foram lidos com sucesso
    tm_struct.tm_year -= 1900; // Ano é desde 1900
    tm_struct.tm_mon -= 1;    // Mês é 0-11
    tm_struct.tm_sec = 0;     // Segundos não são parseados, define como 0
    tm_struct.tm_isdst = -1;  // Deixa a libc determinar se é horário de verão
    return mktime(&tm_struct); // Converte para time_t
  }
  return (time_t)0; // Retorna 0 em caso de erro
}

// Converte uma string "HH:MM:SS" para segundos desde a meia-noite
int parseHHMMSS(const String &s) {
  int h = 0, m = 0, sec = 0;
  // Tenta parsear a string
  if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3) { // Se 3 campos foram lidos
    // Valida os ranges
    if (h >= 0 && h < 24 && m >= 0 && m < 60 && sec >= 0 && sec < 60) {
        return h * 3600 + m * 60 + sec; // Retorna segundos totais
    }
  }
  return -1; // Retorna -1 se inválido ou erro de parse
}

// Parseia o tempo de uma regra IH ou IL (formato HH:MM:SS) de uma string de regras
int parseRuleTime(const String& rulePrefix, const String& rules) {
    int rulePos = rules.indexOf(rulePrefix); // Encontra a posição do prefixo (ex: "IH")
    if (rulePos != -1) {
        // Extrai a substring do tempo (ex: "00:00:30" de "IH00:00:30")
        // rulePrefix.length() é 2 para "IH" ou "IL"
        // HH:MM:SS é 8 caracteres
        String timeStrVal = rules.substring(rulePos + rulePrefix.length(), rulePos + rulePrefix.length() + 8);
        int h = 0, m = 0, s_val = 0; // Renomeado 's' para 's_val' para evitar conflito com parâmetro da função externa se houvesse
        if (sscanf(timeStrVal.c_str(), "%d:%d:%d", &h, &m, &s_val) == 3) { // Parseia HH:MM:SS
            // Valida os ranges
            if (h >= 0 && h < 24 && m >= 0 && m < 60 && s_val >= 0 && s_val < 60) {
                return h * 3600 + m * 60 + s_val; // Retorna em segundos
            }
        }
    }
    return -1; // Retorna -1 se não encontrado ou erro de parse
}


// Verifica e processa as regras personalizadas (customSchedule)
String scheduleChk(const String &schedule, const byte &pin) {
  String event = "";    // Evento que causou a ação (para log)
  byte relay_val;       // Valor desejado para o relé (HIGH ou LOW)
  static time_t lastCheck = 0; // Para evitar checagens muito frequentes (a cada segundo no máximo)

  // String para data/hora atual formatada
  String dt_str = "";
  String s_temp = ""; // String temporária para construir padrões de regra (renomeada de 's')
  String timeOnlyStr = ""; // String para HH:MM
  String dayOfWeekStr = ""; // String para DiaDaSemana HH:MM

  if (schedule == "") { // Se não há regras personalizadas, não faz nada
    return "";
  }

  time_t currentTime = now(); // Hora atual

  // Limita a frequência de checagem para evitar sobrecarga (ex: a cada 1 segundo)
  if (currentTime - lastCheck < 1 && lastCheck != 0) return ""; // lastCheck!=0 para permitir a primeira execução
  lastCheck = currentTime;

  // Formata a data e hora atual para "YYYY-MM-DD HH:MM" (minutos são a menor granularidade para estas regras)
  char currentDtStr[17];
  sprintf(currentDtStr, "%04d-%02d-%02d %02d:%02d",
          year(currentTime), month(currentTime), day(currentTime),
          hour(currentTime), minute(currentTime));
  dt_str = String(currentDtStr);

  // --- Verificação de regras baseadas em data/hora específica ---
  // SHYYYY-MM-DD HH:MM (Specific High)
  s_temp = "SH" + dt_str;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = HIGH; goto P_scheduleChk_Action; }
  // SLYYYY-MM-DD HH:MM (Specific Low)
  s_temp = "SL" + dt_str;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = LOW; goto P_scheduleChk_Action; }

  timeOnlyStr = dt_str.substring(11); // Extrai "HH:MM"

  // --- Verificação de regras baseadas em hora do dia (Diário) ---
  // DHMM:HH (Daily High)
  s_temp = "DH" + timeOnlyStr;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = HIGH; goto P_scheduleChk_Action; }
  // DLMM:HH (Daily Low)
  s_temp = "DL" + timeOnlyStr;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = LOW; goto P_scheduleChk_Action; }

  // --- Verificação de regras baseadas em dia da semana e hora ---
  // weekday() retorna 1 para Domingo, ..., 7 para Sábado (TimeLib)
  // WHw HH:MM (Weekly High - w é dia da semana)
  // WLw HH:MM (Weekly Low - w é dia da semana)
  dayOfWeekStr = String(weekday(currentTime)) + " " + timeOnlyStr; // Ex: "2 08:30" para Segunda 08:30

  s_temp = "WH" + dayOfWeekStr;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = HIGH; goto P_scheduleChk_Action; }
  s_temp = "WL" + dayOfWeekStr;
  if (schedule.indexOf(s_temp) != -1) { event = s_temp; relay_val = LOW; goto P_scheduleChk_Action; }


  // --- Verificação de regras de Intervalo (IH/IL) ---
  // Estas regras dependem de `currentTime` (que tem precisão de segundos) e dos timers `ruleHighDT`/`ruleLowDT`.
  // IH (Interval High): Se o pino está HIGH, desliga após um intervalo.
  if (digitalRead(pin) == HIGH && ruleHighDT != 0) { // Se pino está LIGADO e ruleHighDT foi definido
    int ihTime = parseRuleTime("IH", schedule); // Parseia o tempo de IH (ex: "IH00:00:30")
    if (ihTime != -1 && (currentTime - ruleHighDT >= ihTime)) { // Se regra IH existe e o tempo passou
      event = "IH" + formatHHMMSS(ihTime);
      relay_val = LOW; // Ação: DESLIGAR
      goto P_scheduleChk_Action;
    }
  }
  // IL (Interval Low): Se o pino está LOW, liga após um intervalo.
  if (digitalRead(pin) == LOW && ruleLowDT != 0) { // Se pino está DESLIGADO e ruleLowDT foi definido
    int ilTime = parseRuleTime("IL", schedule); // Parseia o tempo de IL (ex: "IL00:00:30")
    if (ilTime != -1 && (currentTime - ruleLowDT >= ilTime)) { // Se regra IL existe e o tempo passou
      event = "IL" + formatHHMMSS(ilTime);
      relay_val = HIGH; // Ação: LIGAR
      goto P_scheduleChk_Action;
    }
  }

P_scheduleChk_Action: // Ponto de ação para todas as regras
  if (event != "" && relay_val != digitalRead(pin)) { // Se um evento foi definido E o estado desejado é diferente do atual
    // Log da intenção da regra
    String logRuleIntent = timeStr(currentTime) + " -> Regra " + event + " detectada para " + (relay_val == HIGH ? "LIGAR" : "DESLIGAR") + ".";
    Serial.println(logRuleIntent);
    eventLog += logRuleIntent + "\n";

    if (relay_val == HIGH) { // Se a ação é LIGAR
      unsigned long durationToFeedSec = MAX_FEED_DURATION + 1; // Duração padrão: "até outra regra desligar" ou IH
      int ihTime = parseRuleTime("IH", customSchedule); // Verifica se há uma regra IH para limitar a duração
      if (ihTime != -1 && ihTime > 0) { // Se IH existe e é maior que 0
        durationToFeedSec = ihTime; // A duração será o tempo de IH
      }
      // Se ihTime for 0 ou -1, durationToFeedSec permanece MAX_FEED_DURATION + 1,
      // o que significa que ficará ligado até outra regra (DL, WL, SL, IH válida) o desligar, ou manualmente.
      startFeeding(durationToFeedSec); // Inicia a alimentação
    } else { // relay_val == LOW, a ação é DESLIGAR
      stopFeeding(); // Para a alimentação
    }
    return event; // Retorna o evento que causou a ação
  }
  return ""; // Nenhuma regra acionada ou pino já no estado desejado
}


// =============== Web Server Handlers =============== //
// Retorna informações de RSSI do WiFi em JSON
void handleRssi() {
  int rssi = WiFi.RSSI();
  int pct  = map(constrain(rssi, -90, -30), -90, -30, 0, 100); // Mapeia RSSI para porcentagem (0-100)
  String json = String("{\"rssi\":") + rssi + ",\"pct\":" + pct + "}";
  server.send(200, "application/json", json);
}

// Retorna a hora atual formatada
void handleTime() {
    server.send(200, "text/plain", timeStr(now()));
}

// Retorna a string do próximo acionamento
void handleNextTriggerTime() {
    server.send(200, "text/plain", getNextTriggerTimeString());
}

// Define o pino GPIO para o alimentador
void handleSetFeederPin() {
  if (!server.hasArg("feederPin")) {
    server.send(400, "text/plain", "Parâmetro 'feederPin' ausente");
    return;
  }
  int newPin = server.arg("feederPin").toInt();

  // Validação básica do pino
  bool isValidConfigPin = true;
#if defined(ESP8266)
  if (newPin < 0 || newPin > 16) isValidConfigPin = false;
  if (newPin == 1 || newPin == 3) isValidConfigPin = false; // TX, RX (Serial)
  // GPIO 0, 2, 15 são usados para boot. Cuidado.
  // GPIO 4, 5 são SCL, SDA padrão para I2C.
#elif defined(ESP32)
  if (newPin < 0 || newPin > 39) isValidConfigPin = false;
  if (newPin >=34 && newPin <=39) isValidConfigPin = false; // Pinos apenas de entrada
  // GPIO 6-11 são usados pela flash SPI.
#endif

  if (!isValidConfigPin) {
    server.send(400, "text/plain", "Número do pino inválido ou reservado.");
    return;
  }

  if (newPin != currentFeederPin) {
    // Se estiver a alimentar, para antes de mudar o pino
    if(isFeeding) {
        stopFeeding();
        String logPinChange = timeStr(now()) + " -> Alimentação interrompida devido à mudança de pino.";
        Serial.println(logPinChange);
        eventLog += logPinChange + "\n";
    }
    currentFeederPin = newPin;
    pinMode(currentFeederPin, OUTPUT);    // Define o novo pino como saída
    digitalWrite(currentFeederPin, LOW);  // Garante que comece desligado
    Serial.printf("Pino do alimentador alterado para GPIO: %d\n", currentFeederPin);
  }

  saveConfig(); // Salva a nova configuração

  String logMsg = timeStr(now()) + " -> Pino do alimentador definido para GPIO " + String(currentFeederPin);
  eventLog += logMsg + "\n";
  Serial.println(logMsg);

  server.send(200, "text/plain", "Pino do alimentador salvo.");
}

// Retorna o status atual do alimentador e regras personalizadas em JSON
void handleStatus() {
  DynamicJsonDocument doc(256); // JSON para a resposta
  doc["is_feeding"] = isFeeding;
  doc["custom_rules_enabled"] = customEnabled;

  String activeRule = "none"; // Regra ativa ("IH", "IL", ou "none")
  long timeRemaining = -1;    // Tempo restante para a regra ativa em segundos

  if (customEnabled) {
    time_t currentTime = now();
    if (isFeeding && ruleHighDT != 0) { // Se está alimentando e ruleHighDT está setado (timing IH)
      int ihTimeSec = parseRuleTime("IH", customSchedule);
      if (ihTimeSec != -1 && ihTimeSec > 0) { // Considera IH > 0
        activeRule = "IH";
        timeRemaining = ihTimeSec - (currentTime - ruleHighDT);
        if (timeRemaining < 0) timeRemaining = 0;
      }
    } else if (!isFeeding && ruleLowDT != 0) { // Se não está alimentando e ruleLowDT está setado (timing IL)
      int ilTimeSec = parseRuleTime("IL", customSchedule);
      if (ilTimeSec != -1 && ilTimeSec > 0) { // Considera IL > 0
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

// Para a alimentação manualmente via web
void handleStopFeedNow() {
  if (isFeeding) {
    String logIntent = timeStr(now()) + " -> Comando /stopFeedNow (web) recebido.";
    Serial.println(logIntent); eventLog += logIntent + "\n";
    stopFeeding();
    server.send(200, "text/plain", "Alimentação interrompida.");
  } else {
    server.send(400, "text/plain", "Nenhuma alimentação ativa para interromper.");
  }
}


// Handler para a página principal HTML
void handleRoot() {
  String page = FPSTR(htmlPage); // Carrega o HTML da PROGMEM
  int rssi    = WiFi.RSSI();
  int quality = map(constrain(rssi, -90, -30), -90, -30, 0, 100); // Qualidade do WiFi

  // Substitui os placeholders no HTML com os valores atuais
  page.replace("%MANUAL%", formatHHMMSS(manualDurationSec));
  page.replace("%WIFI_QUALITY%", String(quality));
  page.replace("%FEEDER_PIN_VALUE%", String(currentFeederPin));

  String scheduleList_str; // String para a lista de agendamentos (renomeada de scheduleList)
  for (int i = 0; i < scheduleCount; i++) {
    if (i > 0) scheduleList_str += ",";
    scheduleList_str += formatHHMMSS(schedules[i].timeSec);
    scheduleList_str += "|";
    scheduleList_str += formatHHMMSS(schedules[i].durationSec);
  }
  page.replace("%SCHEDULES%", scheduleList_str);
  page.replace("%CUSTOM_RULES%", customSchedule); // Preenche o textarea com as regras atuais
  page.replace("%TOGGLE_BUTTON%", customEnabled ? "Desativar Regras" : "Ativar Regras");
  page.replace("%STATUS_CLASS%", isFeeding ? "feeding" : (WiFi.status() == WL_CONNECTED ? "active" : ""));

  server.send(200, "text/html", page); // Envia a página
}

// Inicia a alimentação manualmente via web
void handleFeedNow() {
  unsigned long current_millis = millis();

  if (isFeeding) {
    server.send(409, "text/plain", "Já está alimentando");
    return;
  }
  if (current_millis - lastTriggerMs < FEED_COOLDOWN * 1000UL && lastTriggerMs != 0) {
    server.send(429, "text/plain", "Aguarde o intervalo (" + String(FEED_COOLDOWN) + "s) entre alimentações");
    return;
  }

  String logIntent = timeStr(now()) + " -> Comando /feedNow (web manual) recebido.";
  Serial.println(logIntent); eventLog += logIntent + "\n";

  unsigned long durationToFeed = manualDurationSec;
  // Se regras customizadas estão ativas, startFeeding irá considerar a regra IH, se houver,
  // para limitar a duração manual, se IH for menor.
  startFeeding(durationToFeed);
  server.send(200, "text/plain", "Alimentação iniciada");
}


// Define a duração manual da alimentação
void handleSetManualDuration() {
  if (! server.hasArg("manualDuration")) {
    server.send(400, "text/plain", "Parâmetro 'manualDuration' ausente");
    return;
  }

  String duration_str = server.arg("manualDuration"); // Renomeada de 'duration'
  int secs = parseHHMMSS(duration_str);

  if (secs <= 0 || secs > MAX_FEED_DURATION) {
    server.send(400, "text/plain", "Duração inválida (1s a " + formatHHMMSS(MAX_FEED_DURATION) + ")");
    return;
  }

  manualDurationSec = secs;
  saveConfig();

  String ts = timeStr(now());
  String ev = ts + " -> Duração manual definida para " + duration_str;
  eventLog += ev + "\n";
  Serial.println(ev);

  server.send(200, "text/plain", "Duração salva");
}


// Define os agendamentos
void handleSetSchedules() {
  if (server.hasArg("schedules")) {
    String scheduleStr_arg = server.arg("schedules"); // Renomeada de scheduleStr

    scheduleCount = 0;
    for(int k=0; k < MAX_SLOTS; ++k) {
        schedules[k].lastTriggerDay = -1; // Limpa lastTriggerDay para todos os slots
    }

    if (scheduleStr_arg.length() > 0) {
      int start = 0;
      while (start < scheduleStr_arg.length() && scheduleCount < MAX_SLOTS) {
        int commaPos = scheduleStr_arg.indexOf(',', start);
        if (commaPos < 0) commaPos = scheduleStr_arg.length();

        String token = scheduleStr_arg.substring(start, commaPos);
        int pipePos = token.indexOf('|');

        if (pipePos > 0) {
          int timeSec_val = parseHHMMSS(token.substring(0, pipePos)); // Renomeada de timeSec
          int durationSec_val = parseHHMMSS(token.substring(pipePos + 1)); // Renomeada de durationSec

          if (timeSec_val >= 0 && durationSec_val > 0 && durationSec_val <= MAX_FEED_DURATION) {
            schedules[scheduleCount] = { timeSec_val, durationSec_val, -1 }; // lastTriggerDay é resetado
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

// Define as regras personalizadas
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

  saveConfig(); // Salva o novo customSchedule e o estado customEnabled existente.

  time_t t_now = now();
  String current_ts_log = timeStr(t_now);
  String log_prefix_rules_saved = current_ts_log + " -> Novas regras personalizadas salvas";

  if (customEnabled) {
    if (digitalRead(currentFeederPin) == LOW) {
      ruleLowDT = t_now;
      ruleHighDT = 0;
      String logMsgDetail = log_prefix_rules_saved + " (regras ATIVADAS). Dispositivo DESLIGADO. (Re)iniciando contagem IL.";
      Serial.println(logMsgDetail);
      eventLog += logMsgDetail + "\n";
    } else {
      ruleHighDT = t_now;
      ruleLowDT = 0;
      String logMsgDetail = log_prefix_rules_saved + " (regras ATIVADAS). Dispositivo LIGADO. (Re)iniciando contagem IH.";
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


static bool notifiedStart = false; // Flag para log de início de contagem de regras customizadas no loop

// Alterna o estado das regras personalizadas (ativado/desativado)
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
      String logMsg = current_ts_log + " -> Regras avançadas ATIVADAS. Dispositivo DESLIGADO. Iniciando contagem para IL (se houver).";
      Serial.println(logMsg);
      eventLog += logMsg + "\n";
    } else {
      ruleHighDT = t_now;
      ruleLowDT = 0;
      String logMsg = current_ts_log + " -> Regras avançadas ATIVADAS. Dispositivo LIGADO. Iniciando contagem para IH (se houver).";
      Serial.println(logMsg);
      eventLog += logMsg + "\n";
    }
    notifiedStart = false; // Permite log de "contagem iniciada" no loop
  } else {
    // Ao desativar, zera os timers para evitar que um valor antigo seja usado se reativado
    ruleHighDT = 0;
    ruleLowDT = 0;
    String logMsg = current_ts_log + " -> Regras avançadas DESATIVADAS. Timers IH/IL zerados.";
    Serial.println(logMsg);
    eventLog += logMsg + "\n";
  }
}

// =============== Configuration Management =============== //

// Carrega a configuração do arquivo JSON no LittleFS/SPIFFS
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
  if (!FS.begin(true)) { // true para formatar se a montagem falhar
    Serial.println("SPIFFS mount failed, rebooting...");
    ESP.restart();
  }
#endif

  if (!FS.exists(CONFIG_PATH)) {
    Serial.println("No config file found, using defaults and creating one.");
    currentFeederPin = defaultFeederPin;
    manualDurationSec = 5; // Default
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
    // Outros defaults já estão definidos globalmente ou serão aplicados abaixo
    return;
  }

  DynamicJsonDocument doc(2048); // Aumentado para segurança
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
  manualDurationSec = doc["manualDuration"] | 5; // Default 5s

  const char *rules_ptr = doc["customSchedule"]; // Renomeado de 'rules'
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
  Serial.printf(" • Feeder Pin: GPIO %d\n", currentFeederPin);
  Serial.printf(" • Manual duration: %s\n", formatHHMMSS(manualDurationSec).c_str());
  Serial.printf(" • %d schedules:\n", scheduleCount);
  for (int i = 0; i < scheduleCount; i++) {
    char timeBuf[9], durationBuf[9];
    formatHHMMSS(schedules[i].timeSec, timeBuf, sizeof(timeBuf));
    formatHHMMSS(schedules[i].durationSec, durationBuf, sizeof(durationBuf));
    Serial.printf("    – %s for %s (Last triggered day: %d)\n",
                  timeBuf, durationBuf, schedules[i].lastTriggerDay);
  }
  Serial.printf(" • Custom rules: \"%s\" (%s)\n",
                customSchedule,
                customEnabled ? "enabled" : "disabled");
}

// Salva a configuração atual no arquivo JSON
void saveConfig() {
  DynamicJsonDocument doc(2048); // Aumentado para segurança
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
// Configura os pinos de hardware
void setupHardware() {
  pinMode(currentFeederPin, OUTPUT);
  digitalWrite(currentFeederPin, LOW); // Garante que comece desligado

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH); // LED desligado (LOW acende para LED_BUILTIN comum)

  #ifdef SONOFF_BASIC
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  #endif
}

// Atualiza o LED de status para indicar o estado do dispositivo
void updateStatusLED() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  unsigned long current_millis = millis();

  if (isFeeding) { // Alimentando: pisca rapidamente (ex: a cada 200ms)
    if (current_millis - lastBlink > 200) {
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState ? LOW : HIGH); // Ajuste LOW/HIGH conforme seu LED
      lastBlink = current_millis;
    }
  } else { // Não alimentando
    if (WiFi.status() == WL_CONNECTED) { // WiFi conectado: LED aceso (ou apagado, dependendo da lógica)
      digitalWrite(STATUS_LED_PIN, LOW); // Ex: LOW para aceso (comum para LED_BUILTIN)
    } else { // WiFi desconectado: pisca lentamente (ex: a cada 1000ms)
      if (current_millis - lastBlink > 1000) {
        ledState = !ledState;
        digitalWrite(STATUS_LED_PIN, ledState ? LOW : HIGH);
        lastBlink = current_millis;
      }
    }
  }
}

// Função de parada de emergência (ex: falha crítica no filesystem)
void emergencyStop() {
  Serial.println("EMERGENCY STOP ACTIVATED!");
  digitalWrite(currentFeederPin, LOW); // Garante que o alimentador esteja desligado
  pinMode(STATUS_LED_PIN, OUTPUT);
  while(true) { // Loop infinito piscando o LED rapidamente
    digitalWrite(STATUS_LED_PIN, LOW); delay(100);
    digitalWrite(STATUS_LED_PIN, HIGH); delay(100);
  }
}

// =============== Network Setup =============== //
// Configura a rede WiFi e sincroniza o tempo (NTP e RTC)
void setupNetwork() {
  wifiManager.setConfigPortalTimeout(180); // Portal de configuração expira em 3 minutos
  wifiManager.setConnectTimeout(30);       // Tenta conectar por 30 segundos

  if (!wifiManager.autoConnect("PetFeederAP")) { // Tenta conectar, se falhar, inicia AP "PetFeederAP"
    Serial.println("Falha ao conectar ao WiFi e o portal de configuração expirou. Reiniciando...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi conectado!");
  Serial.print("IP address: "); Serial.println(WiFi.localIP());

  Serial.println("Configurando time para UTC (GMT+0) para sincronização NTP inicial...");
  configTime(0, 0, NTP_SERVER); // GMT+0, sem DST, servidor NTP

  Serial.print("Aguardando sincronização NTP (UTC)...");
  time_t utcTime = 0;
  int tentativasNTP = 0;
  bool ntpUtcSuccess = false;

  while (tentativasNTP < 60) { // Tenta por ~30 segundos
    utcTime = time(nullptr);
    int current_year = year(utcTime);
    Serial.printf("\nTentativa NTP (UTC) %d: utcTime = %lu, ano = %d", tentativasNTP + 1, (unsigned long)utcTime, current_year);
    if (current_year > 2023) { // Ano válido (ex: > 2023)
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

    time_t localTime = utcTime + GMT_OFFSET_SEC + DAYLIGHT_OFFSET_SEC; // Calcula tempo local
    setTime(localTime); // Define o tempo local na TimeLib

    Serial.print("Hora local calculada e definida na TimeLib: ");
    Serial.println(getCurrentDateTimeString());

    if (rtcInitialized) { // Se RTC ok, ajusta-o com o tempo NTP
        rtc.adjust(DateTime(year(), month(), day(), hour(), minute(), second()));
        Serial.println("RTC DS3231 sincronizado com a hora local (NTP).");
    } else {
        Serial.println("RTC não inicializado, não foi possível sincronizar com NTP.");
    }
  } else { // Falha NTP
    Serial.println("Falha ao obter hora válida do NTP (UTC) após várias tentativas!");
    Serial.print("Último valor de utcTime: "); Serial.println(utcTime);
    Serial.print("Último ano obtido (UTC): "); Serial.println(year(utcTime));

    if (rtcInitialized && !rtc.lostPower()) { // Se RTC ok e com bateria
        Serial.println("Usando hora do RTC pois NTP falhou e RTC tem hora válida.");
        syncTimeLibWithRTC();
        Serial.print("Hora atual (RTC): "); Serial.println(getCurrentDateTimeString());
    } else {
        Serial.println("NTP falhou E RTC sem hora válida (ou não encontrado). Sistema sem hora correta!");
    }
  }
}

// Handler para obter os eventos acumulados para a interface web
void handleGetEvents() {
  if (eventLog.length() > 0) {
    server.send(200, "text/plain", eventLog);
    eventLog = ""; // Limpa o log após enviar
  } else {
    server.send(200, "text/plain", "");
  }
}


// =============== Main Setup =============== //
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== Iniciando Pet Feeder com DS3231 ===");

  loadConfig();
  setupHardware();

  #if defined(ESP8266) // Pinos I2C para ESP8266 (NodeMCU/Wemos D1 Mini)
    Wire.begin(D2, D1); // D2 (GPIO4) = SDA, D1 (GPIO5) = SCL. Ajuste se necessário.
  #elif defined(ESP32) // Pinos I2C padrão para ESP32
    Wire.begin();       // GPIO21 (SDA), GPIO22 (SCL)
  #else
    Wire.begin();       // Padrão Arduino
  #endif

  if (rtc.begin()) {
    rtcInitialized = true;
    Serial.println("RTC DS3231 encontrado e inicializado.");
    if (rtc.lostPower()) {
      Serial.println("RTC perdeu energia (lostPower = true). A hora será ajustada via NTP se disponível.");
    } else {
      Serial.println("RTC com hora válida (lostPower = false). Sincronizando relógio do sistema (TimeLib) com RTC.");
      syncTimeLibWithRTC();
      Serial.print("Hora inicial (do RTC): "); Serial.println(getCurrentDateTimeString());
    }
  } else {
    Serial.println("Erro: RTC DS3231 não encontrado! Verifique a fiação. rtcInitialized = false");
    rtcInitialized = false;
  }

  setupNetwork(); // Conecta WiFi, sincroniza NTP e RTC

  // Inicializa timers de regras customizadas se estiverem ativas no boot
  if (customEnabled) {
      time_t t_now = now();
      if (digitalRead(currentFeederPin) == LOW) {
          ruleLowDT = t_now; ruleHighDT = 0;
          Serial.println(timeStr(t_now) + " -> Inicialização: Regras custom ATIVADAS, Disp. DESLIGADO. Contagem IL iniciada.");
      } else {
          ruleHighDT = t_now; ruleLowDT = 0;
          Serial.println(timeStr(t_now) + " -> Inicialização: Regras custom ATIVADAS, Disp. LIGADO. Contagem IH iniciada.");
      }
  } else {
      ruleHighDT = 0; ruleLowDT = 0;
  }

  // Configuração dos handlers do servidor web
  server.on("/", HTTP_GET, handleRoot);
  server.on("/rssi", HTTP_GET, handleRssi);
  server.on("/time", HTTP_GET, handleTime);
  server.on("/nextTriggerTime", HTTP_GET, handleNextTriggerTime);
  server.on("/setFeederPin", HTTP_POST, handleSetFeederPin);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/stopFeedNow", HTTP_POST, handleStopFeedNow);
  server.on("/feedNow", HTTP_POST, handleFeedNow);
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
  Serial.printf("Pino do Alimentador: GPIO %d\n", currentFeederPin);
  Serial.printf("Duração manual: %s\n", formatHHMMSS(manualDurationSec).c_str());
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
  // Sincronização periódica com RTC (ex: a cada 5 minutos)
  if (rtcInitialized && (millis() % 300000 == 0)) {
    syncTimeLibWithRTC();
    Serial.println(timeStr(now()) + " -> Sincronização periódica TimeLib <- RTC realizada.");
  } else if (!rtcInitialized && (millis() % 60000 == 0)) { // Avisa se RTC não está ok
    Serial.println(timeStr(now()) + " -> AVISO NO LOOP: RTC não foi inicializado. Sincronização periódica desativada.");
  }

  server.handleClient();
  updateStatusLED();

  #ifdef SONOFF_BASIC // Lógica do botão para Sonoff Basic
    bool rawState = digitalRead(BUTTON_PIN); // Renomeado de 'raw'
    if (rawState != lastButtonState) {
      lastDebounceMs = millis();
    }
    if (millis() - lastDebounceMs > DEBOUNCE_DELAY) {
      static bool buttonPressHandled = false;
      if (rawState == LOW && !buttonPressHandled) { // Botão pressionado
        if (!isFeeding && (millis() - lastTriggerMs >= FEED_COOLDOWN * 1000UL || lastTriggerMs == 0)) {
          String logIntent = timeStr(now()) + " -> Botão físico pressionado para LIGAR.";
          Serial.println(logIntent); eventLog += logIntent + "\n";
          startFeeding(manualDurationSec);
        } else {
          String msg = timeStr(now()) + " -> Botão manual: cooldown ou alimentador já ativo.";
          Serial.println(msg); eventLog += msg + "\n";
        }
        buttonPressHandled = true;
      }
      if (rawState == HIGH) { // Botão liberado
        buttonPressHandled = false;
      }
    }
    lastButtonState = rawState;
  #endif

  // Parada automática da alimentação por tempo (para durações definidas)
  if (isFeeding && feedDurationMs > 0 && feedDurationMs <= (unsigned long)MAX_FEED_DURATION * 1000UL) {
      if (millis() - feedStartMs >= feedDurationMs) {
        String logIntent = timeStr(now()) + " -> Duração da alimentação (" + formatHHMMSS(feedDurationMs/1000) + ") expirou (via loop timer).";
        Serial.println(logIntent);
        eventLog += logIntent + "\n";
        stopFeeding();
      }
  }


  // Processamento de regras (agendadas ou personalizadas)
  if (customEnabled) {
    if (!notifiedStart) {
      String ts = timeStr(now());
      Serial.println(ts + " -> Loop: Regras customizadas ATIVAS. Processando scheduleChk.");
      eventLog += ts + " -> Loop: Regras customizadas ATIVAS. Processando scheduleChk.\n";
      notifiedStart = true;
    }
    scheduleChk(customSchedule, currentFeederPin);
  } else { // Regras customizadas desativadas
    if (notifiedStart) {
        String ts = timeStr(now());
        Serial.println(ts + " -> Loop: Regras customizadas DESATIVADAS. Processando checkSchedules.");
        eventLog += ts + " -> Loop: Regras customizadas DESATIVADAS. Processando checkSchedules.\n";
        notifiedStart = false;
    }
    if (!isFeeding) { // Só verifica agendamentos normais se não estiver alimentando
        checkSchedules();
    }
  }

  yield(); // Importante para ESP8266
}


// Inicia alimentação por 'durationSec' segundos
// Se durationSec > MAX_FEED_DURATION, considera-se "ligar até outra regra desligar" (para regras customizadas)
void startFeeding(unsigned long durationSec) {
  if (isFeeding) {
    return; // Já está alimentando
  }

  unsigned long current_millis = millis();
  if (lastTriggerMs != 0 && (current_millis - lastTriggerMs < FEED_COOLDOWN * 1000UL)) {
    String cooldownMsg = timeStr(now()) + " -> Tentativa de LIGAR em cooldown ("+String(FEED_COOLDOWN)+"s). Ignorado.";
    Serial.println(cooldownMsg);
    eventLog += cooldownMsg + "\n";
    return;
  }

  isFeeding = true;
  feedStartMs = current_millis;

  unsigned long actualDurationSec = durationSec;

  if (customEnabled) {
      int ihTimeSec = parseRuleTime("IH", customSchedule);
      if (ihTimeSec != -1 && ihTimeSec > 0) { // Se IH existe e é válida (>0)
          // A duração real será o MENOR entre o solicitado (durationSec) e o tempo de IH.
          // Se durationSec for MAX_FEED_DURATION+1 (sinal de "ligar por regra"),
          // então actualDurationSec será ihTimeSec.
          // Se durationSec for manual (ex: 5s) e ihTimeSec for 30s, actualDurationSec será 5s.
          // Se durationSec for manual (ex: 60s) e ihTimeSec for 30s, actualDurationSec será 30s.
          actualDurationSec = min((unsigned long)ihTimeSec, durationSec);
      } else { // Sem regra IH válida, ou IH=0
          // Se durationSec é o sinalizador de "ligar por regra custom sem IH"
          if (durationSec == (MAX_FEED_DURATION + 1)) {
              actualDurationSec = MAX_FEED_DURATION + 1; // Mantém "infinito"
          } else { // Duração manual/agendada normal, limita pela constante global
              actualDurationSec = min(durationSec, (unsigned long)MAX_FEED_DURATION);
          }
      }
  } else { // Regras customizadas desativadas, só agendamentos/manual
      actualDurationSec = min(durationSec, (unsigned long)MAX_FEED_DURATION);
  }

  // Garante pelo menos 1s se a intenção era ligar (durationSec > 0) mas actualDurationSec calculou 0
  if (actualDurationSec == 0 && durationSec > 0) {
      actualDurationSec = 1;
  }
  // Cap final, exceto para o sinalizador "infinito"
  if (actualDurationSec > (MAX_FEED_DURATION + 1) ) {
      actualDurationSec = MAX_FEED_DURATION + 1;
  }


  // Define feedDurationMs para o temporizador do loop
  if (actualDurationSec > MAX_FEED_DURATION && actualDurationSec == (MAX_FEED_DURATION + 1)) {
      // Sinalizador "infinito": o temporizador do loop não deve desligar, apenas as regras IH em scheduleChk.
      // No entanto, para segurança, podemos definir um tempo muito longo ou 0 para que o loop não interfira.
      // Se definirmos 0, a condição `millis() - feedStartMs >= feedDurationMs` no loop não desligará.
      // Mas isso requer que `feedDurationMs > 0` na condição do loop para parada automática.
      // Vamos manter a lógica original: o loop timer vai tentar desligar após MAX_FEED_DURATION+1 segundos.
      // A regra IH em scheduleChk é o principal mecanismo de desligamento para durações indefinidas.
      feedDurationMs = actualDurationSec * 1000UL;
  } else if (actualDurationSec > 0) { // Duração finita e válida
      feedDurationMs = actualDurationSec * 1000UL;
  } else { // Duração é 0 (ou se tornou 0 e não foi corrigida para 1s)
      feedDurationMs = 0; // Não deve ficar ligado
      // Se feedDurationMs for 0, o loop principal o desligará quase imediatamente.
      // Se actualDurationSec foi 0, mas durationSec original era >0, já foi corrigido para 1s.
      // Este caso (actualDurationSec = 0) só ocorreria se durationSec original fosse 0.
  }


  digitalWrite(currentFeederPin, HIGH);
  lastTriggerMs = current_millis;

  time_t currentTime = now();
  String logMsg = timeStr(currentTime) + " -> Alimentador LIGADO.";

  if (actualDurationSec > MAX_FEED_DURATION) { // Sinalizador "infinito"
    logMsg += " (Tempo indefinido, aguardando regra IH ou parada manual/outra regra).";
  } else if (actualDurationSec > 0) {
    logMsg += " Duração programada: " + formatHHMMSS(actualDurationSec) + ".";
  } else { // actualDurationSec == 0
    logMsg += " (Duração programada: 0s, desligará imediatamente).";
  }


  if (customEnabled) {
      ruleHighDT = currentTime; // Registra que o pino foi para HIGH sob regras custom
      ruleLowDT = 0;            // Zera o timer de LOW
      int ihTimeSec_log = parseRuleTime("IH", customSchedule); // Para log
      if (ihTimeSec_log != -1 && ihTimeSec_log > 0 && actualDurationSec == (unsigned long)ihTimeSec_log && actualDurationSec <= MAX_FEED_DURATION) {
          // Se a duração foi efetivamente definida pela regra IH (e não é "infinita")
          time_t predictedOffTime = currentTime + ihTimeSec_log;
          logMsg += " Desligamento por IH ("+formatHHMMSS(ihTimeSec_log)+") previsto para " + timeStr(predictedOffTime) + ".";
      } else if (actualDurationSec > 0 && actualDurationSec <= MAX_FEED_DURATION) { // Duração finita, não "infinita"
          logMsg += " Desligamento por tempo (loop timer ou IH em scheduleChk) previsto para " + timeStr(currentTime + actualDurationSec) + ".";
      }
  } else if (actualDurationSec > 0 && actualDurationSec <= MAX_FEED_DURATION) { // Agendamento/manual com duração finita
      logMsg += " Desligamento por tempo (loop timer) previsto para " + timeStr(currentTime + actualDurationSec) + ".";
  }

  Serial.println(logMsg);
  eventLog += logMsg + "\n";

  // Se a duração for 0, desliga imediatamente (embora o loop também o faria)
  if (feedDurationMs == 0) {
      String immediateOffLog = timeStr(now()) + " -> Duração de 0s, desligando imediatamente em startFeeding.";
      Serial.println(immediateOffLog);
      eventLog += immediateOffLog + "\n";
      stopFeeding();
  }
}

// Interrompe alimentação imediatamente
void stopFeeding() {
  if (isFeeding) {
    digitalWrite(currentFeederPin, LOW);
    isFeeding = false;
    feedDurationMs = 0; // Zera a duração programada para evitar re-trigger no loop
    time_t stopTime = now();
    String logMsg = timeStr(stopTime) + " -> Alimentador DESLIGADO.";

    if (customEnabled) {
      ruleLowDT = stopTime; // Registra que o pino foi para LOW sob regras custom
      ruleHighDT = 0;       // Zera o timer de HIGH
      int ilTimeSec = parseRuleTime("IL", customSchedule);
      if (ilTimeSec != -1 && ilTimeSec > 0) { // Se IL existe e é válida
        time_t predictedOnTime = stopTime + ilTimeSec;
        logMsg += " Próxima ativação por IL (" + formatHHMMSS(ilTimeSec) + ") prevista para " + timeStr(predictedOnTime) + ".";
      }
    }
    Serial.println(logMsg);
    eventLog += logMsg + "\n";
  }
}

