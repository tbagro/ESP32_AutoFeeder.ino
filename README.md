## Alimentador Automático com ESP32

Este repositório contém o firmware para um alimentador automático controlado por ESP32, com interface web responsiva para acionamento manual e agendado.

---

### Funcionalidades principais

- **Acionamento Manual**: botão na interface web para disparo imediato, com duração configurável e proteção contra múltiplos cliques.
- **Agendamento de Pulsos**: criação de múltiplos horários diários, cada um com duração específica.
- **Operação Não Bloqueante**: usa controle por `millis()` para manter o servidor responsivo, sem `delay()`.
- **Configuração Dinâmica**: parâmetros de pulso e agendamentos salvos em SPIFFS (`config.json`).
- **Relógio de Tempo Real**: sincronização via NTP para assegurar horários precisos.

---

### Componentes necessários

- **Placa**: ESP32 (DevKit ou similar)
- **Sensor / Atuador**: circuito de acionamento de motor ou solenóide no pino GPIO 18
- **Conexão Wi‑Fi**: SSID e senha configurados via WiFiManager

---

### Estrutura do repositório

```text
├── data/
│   └── config.json        # Exemplo de configuração inicial
├── src/
│   └── main.ino           # Código-fonte principal
└── README.md              # Este documento
```

---

### Instalação e uso

1. **Pré-requisitos**:
   - Arduino IDE com suporte a ESP32
   - Bibliotecas: `WiFi`, `WebServer`, `SPIFFS`, `ArduinoJson`, `WiFiManager`
2. **Gravar firmware**:
   - Abra `main.ino` na IDE
   - Ajuste `FEEDER_PIN` se necessário
   - Selecione placa ESP32 e porta correta
   - Carregue o código
3. **Configuração inicial**:
   - Na primeira execução, conecte-se ao ponto de acesso "AlimentadorAP"
   - Acesse `192.168.4.1` para inserir credenciais Wi‑Fi
   - O ESP32 reiniciará e conectará à rede configurada
4. **Acessar interface**:
   - Em um navegador, abra `http://<IP_do_ESP32>` para gerenciar pulso manual e agendamentos

---

### Personalização

- **Duração do pulso manual**: altere no campo de formulário e salve para atualizar em SPIFFS.
- **Adicionar/Remover Agendamentos**: use o formulário de horário e duração; atualizações também são persistidas.
- **Fuso Horário**: modifique `GMT_OFFSET_SEC` e `DAYLIGHT_OFFSET_SEC` conforme local.

---

### Arquivo de configuração (`config.json`)

```json
{
  "manualDuration": 5,
  "schedules": [
    { "time": 36000, "duration": 5 },   
    { "time": 43200, "duration": 10 }
  ]
}
```
- `manualDuration`: duração em segundos do pulso manual.
- `schedules`: lista de objetos com `time` (segundos desde meia-noite) e `duration` em segundos.
- interface homem maquina proposta
![image](https://github.com/user-attachments/assets/1b6b81a9-00d1-4dea-85ae-0f15d058e63f)


Versão compativel com sonoff 
![image](https://github.com/user-attachments/assets/4ddb7258-2778-4195-9d28-cc6b8e67064d)

---

### Licença

MIT © 2025

