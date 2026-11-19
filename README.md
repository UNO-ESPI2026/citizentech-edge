# Amparo - Edge Computing & Computer Systems

> **CitizenTech IoT: Monitoramento Inteligente com ESP32**
> Global Solution 2 | Engenharia de Software | FIAP

O **sensor** do Amparo é um aparelho que fica dentro da casa de um idoso ou de uma pessoa com deficiência. Ele mede o calor do ambiente, decide sozinho quando a situação ficou perigosa, alerta quem está em casa com tela e som e avisa a família pela internet. Se a internet cair, ele **continua alertando** e guarda os avisos para enviar quando a conexão voltar.

```
MEDIR  →  DECIDIR  →  ALERTAR  →  AVISAR
DHT22     ESP32       Tela + buzzer   MQTT (FIWARE) → Node-RED → Painel da família
```

---

## Links da entrega

- **Simulação no Wokwi:** [Wokwi](https://wokwi.com/projects/480413696426722305)
- **Vídeo no YouTube:** [Youtube](https://www.youtube.com/watch?v=1i8AsZRlyl8)

---

## Ficha da CitizenTech

| | |
|---|---|
| **Nome** | Amparo |
| **Cidade e bairros** | São Paulo - Sapopemba, Jardim Pantanal e Parque Brasilândia (bairros de exemplo) |
| **Cidadão beneficiado** | Idosos e pessoas com deficiência, principalmente quem mora sozinho ou tem a família longe |
| **Problema, em uma frase** | Durante ondas de calor e chuvas fortes, idosos e pessoas com deficiência muitas vezes não recebem o alerta a tempo e, quando passam mal, nem sempre conseguem pedir ajuda. |
| **Como funciona** | Um dispositivo dentro de casa mede temperatura e umidade; o sistema combina essas leituras com a previsão de chuva, classifica o risco em NORMAL, ATENÇÃO ou PERIGO, orienta a pessoa e, se ela não responder ou pedir ajuda, avisa a família e os voluntários do bairro. |
| **O que cada parte faz** | **Python:** decisão de risco, recomendações, checagem e rede de apoio. **Edge:** dispositivo ESP32 que mede e alerta com luz e som. **Front-end / Web:** aplicação web acessível. **Cálculo:** relatório que justifica a fórmula e os limites. **Software Design:** jornada e requisitos. **Storytelling:** pitch. |
| **Repositório da solução** | https://github.com/GiovannaParejas/amparo |
| **Repositório da matéria** | https://github.com/UNO-ESPI2026/citizentech-edge |
| **Indicador de impacto** | Percentual de alertas confirmados pela pessoa, número de pedidos atendidos pela rede, tempo entre o pedido de ajuda e a chegada de alguém e pessoas encaminhadas a abrigos durante eventos extremos. |
| **ODS conectados** | 3 (Saúde e bem-estar), 10 (Redução das desigualdades), 11 (Cidades e comunidades sustentáveis) e 13 (Ação contra a mudança global do clima) |

---

## Arquitetura em 3 camadas

```
┌──────────────────────┐     MQTT (UltraLight)     ┌──────────────────────┐   WebSocket + HTTP   ┌──────────────────────┐
│  1. DEVICE IoT       │  /TEF/amparo001/attrs ──▶ │  2. BACK-END         │ ──────────────────▶ │  3. APLICAÇÃO        │
│  ESP32 + DHT22       │                           │  Broker FIWARE       │                     │  Painel da família   │
│  Tela ILI9341        │ ◀── /TEF/amparo001/cmd    │  3.93.68.31:1883     │ ◀── POST /ajuda     │  /amparo             │
│  Buzzer + 2 botões   │  /TEF/amparo001/cmdexe ─▶ │  + Node-RED          │                     │  (HTML + Chart.js)   │
└──────────────────────┘                           └──────────────────────┘                     └──────────────────────┘
   decide sozinho,                                    traduz, guarda histórico,                    mostra estado, gráfico,
   funciona offline                                   repassa comandos                             avisos e "Estou indo"
```

O ESP32 **não depende** das camadas 2 e 3 para proteger a pessoa. A internet serve só para **avisar** a rede de apoio.

---

## Circuito

Simulado no **Wokwi** com ESP32. Os pinos foram escolhidos na mesma ordem dos pinos de cada peça, para os fios irem retos no diagrama.

| Componente | Função | Pino(s) no ESP32 |
|---|---|---|
| **DHT22** | Mede temperatura e umidade (precisas para o índice de calor) | Dados → **GPIO 32** · VCC → 3V3 · GND |
| **Tela TFT ILI9341** (320 × 240, SPI) | Mostra estado com cor, ícone, palavra e orientação em letras grandes | CS → **19** · RST → **18** · D/C → **5** · MOSI → **17** · SCK → **16** · LED → **4** |
| **Buzzer** | Alerta sonoro com padrão diferente para cada estado | **GPIO 13** |
| **Botão vermelho "PEDIR AJUDA"** | SOS: precisa segurar 2 s (evita toque acidental) | **GPIO 26** (INPUT_PULLUP) |
| **Botão verde "ESTOU BEM"** | Confirma que a pessoa está bem durante o PERIGO | **GPIO 27** (INPUT_PULLUP) |
| **Chave deslizante** | *Só para a demonstração:* simula a queda da internet | **GPIO 33** (INPUT_PULLUP) |

> Os botões e a chave usam o resistor de pull-up interno do ESP32 e ligam o pino ao GND quando acionados (nível LOW = pressionado).

### Bibliotecas (`libraries.txt`)

```
Adafruit GFX Library
Adafruit ILI9341
DHT sensor library for ESPx
PubSubClient
```

---

## Lógica de borda

Todas as decisões são justificadas no relatório de Cálculo (DPS).

### Medir: leitura válida e suavização

- **Amostragem a cada 2 s** (limite do DHT22), controlada por `millis()`.
- **Leitura inválida é descartada:** `NaN`, temperatura fora de 0–60 °C ou umidade fora de 1–100 %.
- **3 leituras inválidas seguidas** → estado `ERRO_SENSOR`.
- **Média móvel de 5 amostras** (10 s) para suavizar ruídos e picos isolados.

### Decidir: índice de calor e previsão

| Conceito | Como é calculado |
|---|---|
| **Índice de calor (sensação térmica)** | Critério completo da **NOAA**: fórmula simples de Steadman em condições amenas e regressão de **Rothfusz** quando passa de 80 °F |
| **Taxa de aquecimento** | Calculada sobre a **temperatura** (contínua), não sobre o índice, que tem um salto na troca de fórmula. Limitada a ±1,5 °C/h para ignorar ruído |
| **Previsão de 1 hora** | Reta tangente: `T + taxa × 1 h`, e só depois o índice de calor. Erro < 0,15 °C |
| **Graus-hora** | Soma de Riemann da exposição acumulada acima de 32 °C |

### Limites (faixas da NOAA)

| Índice de calor | Nível |
|---|---|
| < 32 °C | NORMAL |
| 32 °C a 39 °C | ATENÇÃO (cautela extrema) |
| ≥ 39 °C | PERIGO |

Regras extras para evitar alarmes falsos ou tardios:

- **Antecipação:** a decisão usa o **pior valor entre agora e a próxima hora**, então o aparelho avisa *antes* de ficar perigoso.
- **Histerese de 0,5 °C:** para descer de nível, o índice precisa ficar 0,5 °C abaixo do limite. Evita ficar piscando entre dois estados.
- **Um nível por vez:** sobe ou desce no máximo um nível a cada decisão.

### Tempos: demonstração × aparelho real

Para caber no vídeo, os tempos foram encurtados, mas o cálculo da taxa considera que passaram 10 minutos a cada decisão.

| Constante | Demonstração | Aparelho real |
|---|---|---|
| `INTERVALO_DECISAO_MS` | 6 s | 10 min |
| `TEMPO_SEM_RESPOSTA_MS` | 30 s | 10 min |
| `INTERVALO_RECONFIRMAR_MS` | 90 s | 1 h |
| `INTERVALO_BIPE_ATENCAO_MS` | 15 s | 1 min |

---

## Estados de operação

Cada estado tem **cor + ícone + palavra + orientação + som**, para que ninguém dependa só da cor.

| Estado | Quando acontece | Tela | Buzzer |
|---|---|---|---|
| **NORMAL** | Índice < 32 °C | Verde-azulado · ✔ · "Tudo tranquilo" | Silêncio |
| **ATENÇÃO** | 32 °C ≤ índice < 39 °C | Âmbar · sol · "BEBA ÁGUA AGORA" | Bipe curto a cada 15 s (1200 Hz) |
| **PERIGO** | Índice ≥ 39 °C | Vermelho · ✖ · "Aperte ESTOU BEM" | Bipe a cada 1 s (1800 Hz) até confirmar |
| **SEM RESPOSTA** | Ficou em PERIGO sem apertar "Estou bem" | Vermelho · sirene · "Avisando sua família" | Alarme contínuo |
| **SOS** | Botão "Pedir ajuda" segurado por 2 s | Vermelho · sirene · "Família avisada" | Bipe a cada 2 s |
| **ERRO_SENSOR** | 3 leituras inválidas seguidas | Cinza · ! · "Sensor com problema" | Bipe grave a cada 10 s |

**Comportamento sem internet:** em SOS e SEM RESPOSTA, a tela passa a mostrar **"SEM INTERNET · LIGUE 192 / CHAME VIZINHO"** e o buzzer vira um **alarme insistente** (alternando 2000/2600 Hz) para chamar quem estiver por perto. Os eventos vão para uma **fila de até 10 itens** e são enviados quando a internet volta, marcados como atrasados.

**Prioridades:** SOS e SEM RESPOSTA só saem pelo botão "Estou bem" e têm prioridade sobre o erro do sensor. No PERIGO, depois de confirmar, o aparelho volta a perguntar de tempos em tempos.

---

## Comunicação MQTT (FIWARE / UltraLight)

Broker FIWARE: `3.93.68.31:1883` · ID do dispositivo: `amparo001`

| Tópico | Direção | Exemplo |
|---|---|---|
| `/TEF/amparo001/attrs` | ESP32 → broker (leitura) | `t\|28.5\|h\|55.0\|ic\|29.1\|icp\|30.2\|tx\|0.30\|gh\|0.00\|e\|NORMAL` |
| `/TEF/amparo001/attrs` | ESP32 → broker (evento) | `ev\|sos\|ee\|SOS\|eic\|39.2\|ha\|0\|atr\|0` |
| `/TEF/amparo001/cmd` | broker → ESP32 | `amparo001@ajuda\|` |
| `/TEF/amparo001/cmdexe` | ESP32 → broker | `amparo001@ajuda\|ok` |

**Atributos da leitura:** `t` temperatura · `h` umidade · `ic` índice de calor · `icp` índice previsto em 1 h · `tx` taxa (°C/h) · `gh` graus-hora · `e` estado

**Atributos do evento:** `ev` nome do evento · `ee` estado no momento · `eic` índice de calor no momento · `ha` há quantos segundos aconteceu · `atr` 1 se veio da fila offline

**Eventos possíveis:** `normal`, `atencao`, `perigo`, `sem_resposta`, `sos`, `ok`, `erro_sensor`, `sensor_ok`

**Comando `ajuda`:** quando alguém da família aperta "Estou indo" no painel, o ESP32 recebe o comando, mostra **"AJUDA A CAMINHO"** na tela, toca um bipe de aviso e confirma em `/cmdexe`.

---

## Painel da família (Node-RED)

O fluxo do Node-RED (`amparo-node-red.json`) faz quatro coisas:

1. **Recebe os dados** do broker FIWARE e traduz o formato UltraLight para JSON, separando leituras de eventos.
2. **Guarda o histórico** (últimas 300 leituras e 50 eventos) e envia tudo ao painel por **WebSocket** (`/ws/amparo`).
3. **Serve a página** em `http://localhost:1880/amparo` e o histórico em `/amparo/historico`.
4. **Envia o comando "ajuda"** ao aparelho quando o botão "Estou indo" chama `POST /amparo/ajuda`.

### O que o painel mostra

- **Estado atual** da pessoa com etiqueta, ícone, título e orientação.
- **Agora na casa dela:** temperatura, umidade, sensação térmica, previsão para 1 hora com ritmo de aquecimento e exposição ao calor (graus-hora).
- **Gráfico** da sensação térmica e da temperatura com as linhas de 32 °C e 39 °C.
- **Últimos avisos**, com selo para eventos enviados depois de uma queda de internet.
- **Faixa de alerta** vermelha e pulsante em SOS e SEM RESPOSTA, com botões "Ligar para ela", "Ligar 192", "Estou indo" e "Estou ciente".
- **Selos de conexão** do painel e do aparelho (sem leitura há mais de 30 s = aparelho sem conexão).
- **Acessibilidade:** fonte Atkinson Hyperlegible, modo alto contraste, foco visível, som de alerta opcional e respeito a `prefers-reduced-motion`.

---

## Como executar

### 1. Simulação no Wokwi

1. Abra o link público do projeto.
2. Clique em **▶ Start**. O aparelho começa a alertar na hora, sem esperar o Wi-Fi.
3. Abra o **Serial Monitor** para acompanhar as decisões:

```
[DECISAO] T=44.8C UR=50% | IC=58.3C | IC em 1h=58.3C | taxa=+0.00C/h | graus-hora=4.38 | nivel=PERIGO | estado=PERIGO | online
[MQTT] /TEF/amparo001/attrs -> t|44.8|h|50.0|ic|58.3|icp|58.3|tx|0.00|gh|4.38|e|PERIGO
```

### 2. Node-RED e painel

1. Instale e inicie o Node-RED (`npm install -g node-red` e depois `node-red`).
2. Em `http://localhost:1880`, vá em **Menu → Importar** e cole o conteúdo de `amparo-node-red.json`.
3. Clique em **Deploy**.
4. Abra **`http://localhost:1880/amparo`**.

### 3. Testar sem o Wokwi

A aba **Amparo** do Node-RED tem botões de teste que usam o mesmo formato UltraLight do ESP32:

| Botão | O que faz |
|---|---|
| **Simular leitura** | Gera uma leitura falsa; a cada clique a temperatura sobe 1,5 °C (de 26 °C até 37 °C) |
| **Simular SOS** | Envia um pedido de ajuda falso |
| **Enviar "ajuda a caminho"** | Publica o comando `ajuda` no broker |
| **Limpar histórico** | Apaga leituras e eventos guardados (útil antes de gravar o vídeo) |

---

## Roteiro de demonstração

Para mostrar todos os estados na simulação:

| # | Ação no Wokwi | Resultado esperado |
|---|---|---|
| 1 | Clique no DHT22 e ajuste para **25 °C / 50 %** | **NORMAL** · tela verde-azulada, silêncio |
| 2 | Suba para **~34 °C** | **ATENÇÃO** · tela âmbar, bipe curto |
| 3 | Suba para **~42 °C** | **PERIGO** · tela vermelha, bipe repetido, pede "Estou bem" |
| 4 | Espere 30 s sem apertar nada | **SEM RESPOSTA** · sirene, painel mostra faixa vermelha |
| 5 | Aperte **ESTOU BEM** | Volta para PERIGO confirmado, evento `ok` no painel |
| 6 | Segure **PEDIR AJUDA** por 2 s | **SOS** · painel mostra "Pedido de ajuda" |
| 7 | No painel, clique em **Estou indo** | Tela do ESP32 mostra **"AJUDA A CAMINHO"** |
| 8 | Ligue a **chave deslizante** | **SEM INTERNET** · alarme insistente, "LIGUE 192 / CHAME VIZINHO", eventos vão para a fila |
| 9 | Desligue a chave | Fila enviada, painel mostra eventos com selo "Enviado depois" |
| 10 | Ajuste a umidade para **0 %** (leitura inválida) | **ERRO_SENSOR** · tela cinza; voltar a umidade → `sensor_ok` |
| 11 | Baixe a temperatura aos poucos | Desce **um nível por vez**, respeitando a histerese |

---

## Estrutura dos arquivos

```
citizentech-edge/
├── sketch.ino                  # Firmware do ESP32 (medir, decidir, alertar, avisar)
├── diagram.json                # Circuito do Wokwi
├── libraries.txt               # Bibliotecas usadas
├── amparo-node-red.json        # Fluxo do Node-RED (MQTT → histórico → painel)
├── index.html                  # Painel da família
├── wokwi.txt                   # Link para o Wokwi
└── README.md
```

## Equipe

| Nome | RM |
|---|---|
| Bruce Li Yan Ting | 569168 |
| Felipe Rorato Costa | 570419 |
| Giovanna Andrade Parejas | 569041 |
| Laura Cardin Mirilli | 570841 |

<sub>Protótipo acadêmico com dados simulados | Global Solution 2 | Edge Computing & Computer Systems | FIAP | 2026</sub>
