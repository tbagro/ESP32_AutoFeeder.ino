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

# Guia de Regras Personalizadas do Alimentador Inteligente

As regras personalizadas oferecem um controle avançado sobre o funcionamento do seu alimentador inteligente, permitindo programar horários e comportamentos específicos para ligar ou desligar o dispositivo.

## Sintaxe Básica das Regras

As regras são definidas por prefixos que indicam o tipo de acionamento, seguidos por informações de tempo. Múltiplas regras podem ser inseridas, uma por linha ou separadas por espaços.

**Prefixos Principais:**

* `DH HH:MM:SS`: **Diário Alto (Ligar)** - Liga o alimentador todos os dias no horário especificado.
* `DL HH:MM:SS`: **Diário Baixo (Desligar)** - Desliga o alimentador todos os dias no horário especificado.
* `WH d HH:MM:SS`: **Semanal Alto (Ligar)** - Liga o alimentador no dia da semana `d` (1=Domingo, ..., 7=Sábado) no horário especificado.
* `WL d HH:MM:SS`: **Semanal Baixo (Desligar)** - Desliga o alimentador no dia da semana `d` no horário especificado.
* `SH AAAA-MM-DD HH:MM`: **Específico Alto (Ligar)** - Liga o alimentador na data e hora exatas. (Segundos não são usados aqui).
* `SL AAAA-MM-DD HH:MM`: **Específico Baixo (Desligar)** - Desliga o alimentador na data e hora exatas. (Segundos não são usados aqui).
* `IH HH:MM:SS`: **Intervalo Alto (Desligar após Ligado)** - Se o alimentador estiver LIGADO, ele será DESLIGADO após o intervalo de tempo especificado.
* `IL HH:MM:SS`: **Intervalo Baixo (Ligar após Desligado)** - Se o alimentador estiver DESLIGADO, ele será LIGADO após o intervalo de tempo especificado.

**Observação sobre Duração:**
* Quando uma regra `DH`, `WH`, ou `SH` liga o alimentador, ele permanecerá ligado até que uma regra de desligamento (`DL`, `WL`, `SL`, `IH`) seja acionada, ou até que a duração máxima de segurança seja atingida (se aplicável e configurado no firmware, por exemplo, `MAX_FEED_DURATION` para alimentações manuais/agendadas, mas regras `IH` podem definir durações específicas).
* A regra `IL` liga o alimentador. A duração que ele permanecerá ligado é determinada pela próxima regra `IH` (se houver) ou por outras regras de desligamento. Se nenhuma regra `IH` for definida para limitar, ele pode permanecer ligado por um tempo padrão longo (definido no firmware, como `MAX_FEED_DURATION + 1`) ou até ser desligado por outra regra.

## Exemplos Práticos de Regras

Aqui estão alguns exemplos de como você pode combinar essas regras para criar programações úteis:

---

**Exemplo 1: Alimentação Diária Simples**

* **Objetivo:** Ligar o alimentador todos os dias às 08:00:00 e desligar automaticamente após 15 segundos.
* **Regras:**
    ```
    DH08:00:00
    IH00:00:15
    ```
* **Explicação:**
    * `DH08:00:00`: Liga o alimentador às 8 da manhã todos os dias.
    * `IH00:00:15`: Assim que o alimentador for ligado (por `DH` ou manualmente), esta regra conta 15 segundos e então o desliga.

---

**Exemplo 2: Alimentação Duas Vezes ao Dia com Durações Diferentes**

* **Objetivo:**
    * Ligar às 07:00:00 por 10 segundos.
    * Ligar novamente às 19:00:00 por 20 segundos.
* **Regras:**
    ```
    DH07:00:00 IH00:00:10
    DH19:00:00 IH00:00:20
    ```
* **Explicação:**
    * O sistema processa as regras. Quando for 07:00:00, `DH07:00:00` liga. A regra `IH00:00:10` é a mais relevante para o estado "ligado" e o desligará após 10 segundos.
    * Quando for 19:00:00, `DH19:00:00` liga. A regra `IH00:00:20` se torna a regra de intervalo ativa e o desligará após 20 segundos.
    * **Importante:** Se você tiver múltiplas regras `IH` ou `IL`, o sistema considera a primeira que encontrar na string de regras que se aplica ao estado atual. Para durações diferentes em horários diferentes, é mais seguro definir a `IH` junto com a `DH` correspondente ou garantir que as regras `IH` sejam específicas o suficiente ou que a lógica do firmware priorize a `IH` mais curta se múltiplas estiverem presentes (o código atual usa a primeira `IH` encontrada). A forma mais robusta é como no exemplo, agrupando-as implicitamente pela ordem ou, se o firmware suportar, por alguma sintaxe de agrupamento (não presente neste sistema).
    * *Alternativa mais explícita (se o sistema sempre usar a primeira IH/IL encontrada):*
        ```
        DH07:00:00 IH00:00:10 DL07:00:10
        DH19:00:00 IH00:00:20 DL19:00:20
        ```
        Neste caso, `IH` ainda controlaria o desligamento, mas `DL` serviria como um backup ou para clareza. O sistema atual já lida bem com `DH` + `IH`.

---

**Exemplo 3: Ciclo Contínuo (Ligar/Desligar em Loop)**

* **Objetivo:** Ligar o alimentador por 30 segundos, depois desligar por 1 minuto, e repetir este ciclo continuamente.
* **Regras:**
    ```
    IL00:01:00 IH00:00:30
    ```
* **Explicação:**
    * Se o alimentador estiver DESLIGADO, `IL00:01:00` espera 1 minuto e então LIGA o alimentador.
    * Assim que o alimentador LIGA, `IH00:00:30` espera 30 segundos e então DESLIGA o alimentador.
    * Uma vez DESLIGADO, a regra `IL00:01:00` entra em ação novamente, criando o ciclo.
    * **Para iniciar o ciclo:** Se o alimentador estiver desligado quando as regras forem ativadas, o ciclo `IL` começará. Se estiver ligado, `IH` atuará primeiro.

---

**Exemplo 4: Alimentação nos Dias de Semana e Diferente no Fim de Semana**

* **Objetivo:**
    * Durante a semana (Segunda a Sexta): Ligar às 07:30:00 por 10 segundos.
    * No Sábado e Domingo: Ligar às 09:00:00 por 25 segundos.
* **Regras:**
    ```
    WH2 07:30:00 IH00:00:10  # Segunda
    WH3 07:30:00 IH00:00:10  # Terça
    WH4 07:30:00 IH00:00:10  # Quarta
    WH5 07:30:00 IH00:00:10  # Quinta
    WH6 07:30:00 IH00:00:10  # Sexta
    WH1 09:00:00 IH00:00:25  # Domingo
    WH7 09:00:00 IH00:00:25  # Sábado
    ```
* **Explicação:**
    * `WHd HH:MM:SS` liga nos dias da semana especificados (2=Seg, 3=Ter, ..., 7=Sáb, 1=Dom).
    * A regra `IH` associada (ou a primeira `IH` global se não houver uma específica por linha) controlará a duração. Para clareza, é bom ter uma `IH` por linha ou uma `IH` global que se aplique a todas. O sistema atual usará a primeira `IH` que encontrar na string de regras.
    * *Consideração:* Se você tiver uma `IH` global, como `IH00:00:15`, e uma regra `WH7 09:00:00`, ele ligaria no sábado às 09:00 e desligaria após 15 segundos (a `IH` global). Se você quer durações diferentes, precisa de uma forma de associar a `IH` correta ou garantir que não haja uma `IH` global conflitante. A maneira como o código atual processa `startFeeding` com `customEnabled` tentará usar a `IH` das regras para determinar a duração, o que deve funcionar bem com o exemplo acima.

---

**Exemplo 5: Ligar em uma Data Específica (Evento Único)**

* **Objetivo:** Ligar o alimentador no dia 25 de Dezembro de 2025 às 10:00 por 30 segundos.
* **Regras:**
    ```
    SH2025-12-25 10:00 IH00:00:30
    ```
* **Explicação:**
    * `SH2025-12-25 10:00` liga o alimentador na data e hora especificadas.
    * `IH00:00:30` o desligará após 30 segundos.
    * Após esta data/hora, a regra `SH` não será mais acionada (a menos que o ano não seja verificado e se torne um evento anual, o que depende da implementação exata do `scheduleChk`). O `scheduleChk` atual usa o ano.

---

**Exemplo 6: Desligar Durante a Madrugada (Período de Inatividade)**

* **Objetivo:** Garantir que o alimentador esteja desligado entre 01:00:00 e 05:00:00.
* **Regras:**
    ```
    DL01:00:00
    DH05:00:00 IH00:00:05 # Exemplo: Liga por 5s às 05:00, se desejado. Ou apenas DL.
    ```
* **Explicação:**
    * `DL01:00:00`: Desliga o alimentador à 01:00 da manhã.
    * Se você quiser que ele permaneça desligado até as 05:00, esta regra é suficiente.
    * Se você quiser que ele ligue às 05:00, adicione `DH05:00:00` (e opcionalmente uma `IH` para controlar a duração).
    * Se outra regra tentar ligar o alimentador entre 01:00 e 05:00 (ex: uma `IL` que coincide), a `DL01:00:00` não impedirá ativamente, mas se o alimentador for ligado, ele será desligado novamente à 01:00 do dia seguinte. Para um "bloqueio" real, a lógica precisaria ser mais complexa, talvez com flags ou prioridades de regras (não implementado de forma simples).

---

**Exemplo 7: Combinando Regras Diárias e de Intervalo**

* **Objetivo:** Ligar todos os dias às 06:00:00. Se for desligado manualmente ou por outra regra, esperar 5 minutos e ligar novamente. Sempre que ligar, ficar ativo por 10 segundos.
* **Regras:**
    ```
    DH06:00:00
    IL00:05:00
    IH00:00:10
    ```
* **Explicação:**
    * `DH06:00:00`: Liga às 06:00.
    * `IH00:00:10`: Desliga após 10 segundos sempre que for ligado.
    * `IL00:05:00`: Se, por qualquer motivo, o alimentador estiver desligado, esta regra tentará religá-lo após 5 minutos.
    * **Cuidado:** Esta combinação pode levar a um ciclo se não houver outras regras de desligamento mais específicas. Após a `IH` desligar, a `IL` vai querer ligar novamente após 5 minutos.

---
Versão compativel com sonoff 
![image](https://github.com/user-attachments/assets/4ddb7258-2778-4195-9d28-cc6b8e67064d)

---

### Licença

MIT © 2025

