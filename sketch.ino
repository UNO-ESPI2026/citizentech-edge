/*
 * ============================================================
 *  AMPARO - Alerta climatico para idosos e pessoas com deficiencia
 *  Global Solution 2 - Edge Computing & Computer Systems - FIAP
 * ============================================================
 *  Equipe:
 *    - Bruce Li Yan Ting        RM 569168
 *    - Felipe Rorato Costa      RM 570419
 *    - Giovanna Andrade Parejas RM 569041
 *    - Laura Cardin Mirilli     RM 570841
 *
 *  Ciclo de borda: MEDIR -> DECIDIR -> ALERTAR -> AVISAR
 *  O ESP32 decide sozinho. A internet so serve para avisar a rede
 *  de apoio; se cair, os alertas locais (tela + som) continuam e os
 *  eventos ficam numa fila para serem enviados quando ela voltar.
 *
 *  Comunicacao: MQTT no padrao FIWARE
 *    publica  /TEF/amparo001/attrs  ->  "t|28.5|h|55.0|ic|29.1|..."
 *    escuta   /TEF/amparo001/cmd    <-  "amparo001@ajuda|"
 *    responde /TEF/amparo001/cmdexe ->  "amparo001@ajuda|ok"
 *
 *  Estados:
 *    NORMAL        indice de calor < 32 C
 *    ATENCAO       32 C <= indice de calor < 39 C
 *    PERIGO        indice de calor >= 39 C (pede "Estou bem")
 *    SEM_RESPOSTA  ficou em PERIGO sem apertar "Estou bem"
 *    SOS           botao "Pedir ajuda" segurado por 2 s
 *    ERRO_SENSOR   leituras invalidas seguidas
 *
 *  Limites e decisoes justificados no relatorio de Calculo (DPS):
 *    - faixas da NOAA (32 C e 39 C)
 *    - indice de calor com o criterio completo da NOAA (secao 3.1)
 *    - taxa calculada sobre a TEMPERATURA, nao sobre o indice (3.1)
 *    - projecao de 1 hora pela reta tangente (4.4)
 *    - sobe ou desce no maximo 1 nivel por vez (4.4)
 *    - graus-hora = exposicao acumulada acima de 32 C (secao 5)
 * ============================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

// ------------------------------------------------------------
// PINOS
// ------------------------------------------------------------
// Os pinos foram escolhidos na mesma ordem dos pinos de cada peca, para os
// fios irem retos no diagrama (sem cruzamentos).
const int PINO_DHT = 32;         // DHT22 (dados)
const int PINO_SIM_OFFLINE = 33; // chave so para a demonstracao no Wokwi
const int PINO_BTN_SOS = 26;     // botao vermelho "Pedir ajuda"
const int PINO_BTN_OK = 27;      // botao verde "Estou bem"
const int PINO_BUZZER = 13;
const int TFT_CS = 19;
const int TFT_RST = 18;
const int TFT_DC = 5;
const int TFT_MOSI = 17;
const int TFT_SCK = 16;
const int TFT_LED = 4;   // luz de fundo da tela
const int TFT_MISO = 23; // nao usado (a tela so recebe dados), mas o SPI pede um pino

// ------------------------------------------------------------
// LIMITES DE DECISAO (relatorio de Calculo)
// ------------------------------------------------------------
const float LIMITE_ATENCAO = 32.0;  // indice de calor em C (NOAA: cautela extrema)
const float LIMITE_PERIGO = 39.0;   // indice de calor em C (NOAA: perigo)
const float HISTERESE = 0.5;        // para descer de nivel, precisa ficar 0,5 C abaixo
const float HORIZONTE_H = 1.0;      // projecao de 1 hora (erro < 0,15 C, secao 4.4)
const float TAXA_MAX_C_POR_H = 1.5; // casa real esquenta no maximo ~1,2 C/h;
                                    // acima disso a variacao vem de ruido, nao do clima

// Faixa fisicamente possivel dentro de casa. Fora dela, a leitura e invalida
// (sensor com defeito, desconectado ou exposto ao sol/fogao).
const float TEMP_MIN_VALIDA = 0.0;
const float TEMP_MAX_VALIDA = 60.0;
const float UMID_MIN_VALIDA = 1.0;
const float UMID_MAX_VALIDA = 100.0;

// ------------------------------------------------------------
// TEMPOS (em millis)
// ------------------------------------------------------------
const unsigned long INTERVALO_AMOSTRA_MS = 2000; // DHT22 aceita 1 leitura a cada 2 s
// O relatorio usa uma decisao a cada 10 minutos. Na demonstracao, cada decisao
// acontece a cada 6 s, mas o calculo da taxa considera que passaram 10 minutos.
// Aparelho real: INTERVALO_DECISAO_MS = 600000.
const unsigned long INTERVALO_DECISAO_MS = 6000;
const float MINUTOS_POR_DECISAO = 10.0;
const unsigned long TEMPO_SEM_RESPOSTA_MS = 30000;     // real: 600000 (10 min)
const unsigned long INTERVALO_RECONFIRMAR_MS = 90000;  // real: 3600000 (pede de novo a cada 1 h)
const unsigned long INTERVALO_BIPE_ATENCAO_MS = 15000; // real: 60000
const unsigned long TEMPO_SEGURAR_SOS_MS = 2000;
const unsigned long INTERVALO_CONEXAO_MS = 5000;
const unsigned long INTERVALO_REINICIAR_WIFI_MS = 15000;
const unsigned long DEBOUNCE_MS = 50;
const int TAMANHO_MEDIA = 5;    // media movel de 5 amostras (10 s)
const int FALHAS_PARA_ERRO = 3; // 3 leituras invalidas seguidas

// ------------------------------------------------------------
// REDE E MQTT
// ------------------------------------------------------------
const char *WIFI_SSID = "Wokwi-GUEST"; // aparelho real: rede da casa (2,4 GHz)
const char *WIFI_SENHA = "";
const char *MQTT_BROKER = "3.93.68.31"; // servidor FIWARE
const int MQTT_PORTA = 1883;
const char *ID_DISPOSITIVO = "amparo001";
const char *ID_MQTT = "fiware_amparo001";              // nome unico da conexao no broker
const char *TOPICO_ATRIBUTOS = "/TEF/amparo001/attrs"; // leituras e eventos
const char *TOPICO_COMANDO = "/TEF/amparo001/cmd";     // comandos recebidos
const char *TOPICO_RESPOSTA = "/TEF/amparo001/cmdexe"; // confirmacao do comando

// ------------------------------------------------------------
// CORES (manual de marca do Amparo)
// ------------------------------------------------------------
#define COR_TEAL 0x0AEB  // #0E5E5B
#define COR_AMBAR 0xF547 // #F2A83B
#define COR_VERM 0xB123  // #B3261E
#define COR_TINTA 0x1925 // #1C2629
#define COR_CINZA 0x4ACB // #4E5A5D
#define COR_BRANCO 0xFFFF

// ------------------------------------------------------------
// ESTADOS
// ------------------------------------------------------------
enum Estado
{
  NORMAL,
  ATENCAO,
  PERIGO,
  SEM_RESPOSTA,
  SOS,
  ERRO_SENSOR
};
Estado estado = NORMAL;
int nivelClima = 0;              // 0 = normal, 1 = atencao, 2 = perigo (so o clima)
bool antecipado = false;         // nivel atual veio da projecao de 1 h?
bool aguardandoResposta = false; // em PERIGO, esperando "Estou bem"
unsigned long inicioEspera = 0;
unsigned long ultimaConfirmacao = 0;
bool ajudaACaminho = false; // a familia respondeu pelo app (comando "ajuda")

// ------------------------------------------------------------
// OBJETOS
// ------------------------------------------------------------
DHTesp dht;
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ------------------------------------------------------------
// DADOS DO SENSOR
// ------------------------------------------------------------
float bufTemp[TAMANHO_MEDIA];
float bufUmid[TAMANHO_MEDIA];
int idxBuf = 0, qtdBuf = 0;
int falhasSeguidas = 0;
float tempSuave = NAN, umidSuave = NAN, tempAnterior = NAN;
float icAtual = NAN, icProjetado = NAN, taxa = 0;
float grausHora = 0; // exposicao acumulada acima de 32 C (C.h)

// ------------------------------------------------------------
// CONTROLE DE TEMPO, BOTOES, BUZZER E REDE
// ------------------------------------------------------------
unsigned long ultimaAmostra = 0, ultimaDecisao = 0, ultimaConexao = 0;
unsigned long sosPressionadoDesde = 0;
bool sosDisparado = false;
int okLeituraAnterior = HIGH, okEstavel = HIGH;
unsigned long okMudouEm = 0;
bool buzzerLigado = false;
int buzzerFreq = 0;
bool online = false, simulandoOffline = false, wifiIniciado = false;
unsigned long ultimaTentativaWifi = 0;
bool redesenhar = true;

// Fila de eventos para enviar quando a internet voltar
const int TAMANHO_FILA = 10;
struct EventoPendente
{
  char nome[16];
  unsigned long quando;
  Estado estado;
  float ic;
};
EventoPendente fila[TAMANHO_FILA];
int filaInicio = 0, filaQtd = 0;

// Declaracoes antecipadas das funcoes (organizadas pelo ciclo de borda)
// MEDIR
bool lerSensor();
// DECIDIR
float indiceDeCalor(float tempC, float umid);
void calcularRisco(float dtHoras);
void decidirEstado(unsigned long agora);
void verificarSemResposta(unsigned long agora);
void confirmarEstouBem(unsigned long agora);
void verificarBotoes(unsigned long agora);
void mudarEstado(Estado novo);
// ALERTAR
void desenharTelaCompleta();
void desenharValores();
void tocarBuzzer(unsigned long agora);
void imprimirSerial();
// AVISAR
void verificarConexao();
void definirOnline(bool valor);
void publicarLeitura();
void publicarEvento(const char *evento);
void enviarEvento(const char *nome, unsigned long quando, Estado est, float ic, bool atrasado);
void enviarFila();
void receberComando(char *topico, byte *payload, unsigned int tamanho);

// ============================================================
// SETUP
// ============================================================
void setup()
{
  Serial.begin(115200);
  dht.setup(PINO_DHT, DHTesp::DHT22);
  pinMode(PINO_BUZZER, OUTPUT);
  pinMode(PINO_BTN_SOS, INPUT_PULLUP);
  pinMode(PINO_BTN_OK, INPUT_PULLUP);
  pinMode(PINO_SIM_OFFLINE, INPUT_PULLUP);

  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);                    // liga a luz de fundo
  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS); // SPI nos pinos escolhidos
  tft.begin();
  tft.setRotation(1); // tela deitada: 320 x 240

  WiFi.mode(WIFI_STA);
  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setCallback(receberComando); // comandos vindos do FIWARE
  mqtt.setSocketTimeout(3);         // evita travar muito tempo se o broker nao responder

  Serial.println("========================================");
  Serial.println(" AMPARO iniciado - funciona mesmo sem internet");
  Serial.println("========================================");
  // Nao esperamos o Wi-Fi aqui: o aparelho precisa alertar desde o primeiro segundo.
}

// ============================================================
// LOOP - so agenda as tarefas; nada bloqueia
// ============================================================
void loop()
{
  unsigned long agora = millis();

  verificarBotoes(agora);

  // MEDIR: amostra o sensor a cada 2 s (media movel)
  if (agora - ultimaAmostra >= INTERVALO_AMOSTRA_MS)
  {
    ultimaAmostra = agora;
    if (lerSensor() && !redesenhar)
      desenharValores();
  }

  // DECIDIR: a cada intervalo de decisao, com dados validos
  if (agora - ultimaDecisao >= INTERVALO_DECISAO_MS && qtdBuf > 0 && falhasSeguidas == 0)
  {
    ultimaDecisao = agora;
    calcularRisco(MINUTOS_POR_DECISAO / 60.0);
    decidirEstado(agora);
    imprimirSerial();
    publicarLeitura();
  }

  verificarSemResposta(agora);

  // ALERTAR: som e tela
  tocarBuzzer(agora);
  if (redesenhar)
  {
    desenharTelaCompleta();
    redesenhar = false;
  }

  // AVISAR: rede, sem travar o resto
  if (agora - ultimaConexao >= INTERVALO_CONEXAO_MS)
  {
    ultimaConexao = agora;
    verificarConexao();
  }
  if (online)
    mqtt.loop();
}

// ============================================================
// FUNCOES AUXILIARES
// ============================================================
const char *nomeEstado(Estado e)
{
  switch (e)
  {
  case NORMAL:
    return "NORMAL";
  case ATENCAO:
    return "ATENCAO";
  case PERIGO:
    return "PERIGO";
  case SEM_RESPOSTA:
    return "SEM_RESPOSTA";
  case SOS:
    return "SOS";
  default:
    return "ERRO_SENSOR";
  }
}

const char *eventoDoEstado(Estado e)
{
  switch (e)
  {
  case NORMAL:
    return "normal";
  case ATENCAO:
    return "atencao";
  default:
    return "perigo";
  }
}

Estado estadoDoNivel(int nivel)
{
  if (nivel >= 2)
    return PERIGO;
  if (nivel == 1)
    return ATENCAO;
  return NORMAL;
}

int nivelDoIC(float ic)
{
  if (ic >= LIMITE_PERIGO)
    return 2;
  if (ic >= LIMITE_ATENCAO)
    return 1;
  return 0;
}

float limiteDoNivel(int nivel)
{
  return (nivel >= 2) ? LIMITE_PERIGO : LIMITE_ATENCAO;
}

void mudarEstado(Estado novo)
{
  if (novo == estado)
    return;
  Serial.printf("[ESTADO] %s -> %s\n", nomeEstado(estado), nomeEstado(novo));
  estado = novo;
  // "Ajuda a caminho" so vale enquanto a pessoa esta em SOS ou sem resposta
  if (novo != SOS && novo != SEM_RESPOSTA)
    ajudaACaminho = false;
  redesenhar = true;
}

// ============================================================
// MEDIR
// ============================================================

// Le o DHT22, descarta leitura invalida e aplica a media movel.
// Retorna true se a leitura foi aceita.
bool lerSensor()
{
  TempAndHumidity d = dht.getTempAndHumidity();

  bool valida = !isnan(d.temperature) && !isnan(d.humidity) &&
                d.temperature >= TEMP_MIN_VALIDA && d.temperature <= TEMP_MAX_VALIDA &&
                d.humidity >= UMID_MIN_VALIDA && d.humidity <= UMID_MAX_VALIDA;

  if (!valida)
  {
    falhasSeguidas++;
    Serial.printf("[SENSOR] Leitura invalida descartada (%d/%d): T=%.1f UR=%.1f\n",
                  falhasSeguidas, FALHAS_PARA_ERRO, d.temperature, d.humidity);
    // SOS e SEM_RESPOSTA tem prioridade sobre o erro do sensor
    if (falhasSeguidas >= FALHAS_PARA_ERRO && estado != ERRO_SENSOR &&
        estado != SOS && estado != SEM_RESPOSTA)
    {
      mudarEstado(ERRO_SENSOR);
      publicarEvento("erro_sensor");
    }
    return false;
  }

  falhasSeguidas = 0;

  // Fila circular com as ultimas leituras validas
  bufTemp[idxBuf] = d.temperature;
  bufUmid[idxBuf] = d.humidity;
  idxBuf = (idxBuf + 1) % TAMANHO_MEDIA;
  if (qtdBuf < TAMANHO_MEDIA)
    qtdBuf++;

  // Media movel: suaviza ruidos e picos isolados
  float somaT = 0, somaU = 0;
  for (int i = 0; i < qtdBuf; i++)
  {
    somaT += bufTemp[i];
    somaU += bufUmid[i];
  }
  tempSuave = somaT / qtdBuf;
  umidSuave = somaU / qtdBuf;
  icAtual = indiceDeCalor(tempSuave, umidSuave);

  // Sensor voltou a funcionar
  if (estado == ERRO_SENSOR)
  {
    mudarEstado(estadoDoNivel(nivelClima));
    publicarEvento("sensor_ok");
  }
  return true;
}

// ============================================================
// DECIDIR
// ============================================================

// Indice de calor da NOAA com o criterio completo (relatorio, secao 2.1).
// Recebe e devolve em graus Celsius.
float indiceDeCalor(float tempC, float umid)
{
  float t = tempC * 1.8 + 32.0; // para Fahrenheit
  float simples = 0.5 * (t + 61.0 + (t - 68.0) * 1.2 + umid * 0.094);
  float ic;
  if ((simples + t) / 2.0 < 80.0)
  {
    ic = simples; // condicoes amenas: formula simples de Steadman
  }
  else
  {
    // condicoes quentes: regressao de Rothfusz
    ic = -42.379 + 2.04901523 * t + 10.14333127 * umid - 0.22475541 * t * umid - 0.00683783 * t * t - 0.05481717 * umid * umid + 0.00122874 * t * t * umid + 0.00085282 * t * umid * umid - 0.00000199 * t * t * umid * umid;
  }
  return (ic - 32.0) / 1.8; // de volta para Celsius
}

// Calcula a taxa de aquecimento, a projecao de 1 hora e os graus-hora.
void calcularRisco(float dtHoras)
{
  // Taxa sobre a TEMPERATURA, que e continua (o indice tem um salto em T*)
  if (isnan(tempAnterior))
    taxa = 0;
  else
    taxa = (tempSuave - tempAnterior) / dtHoras;
  taxa = constrain(taxa, -TAXA_MAX_C_POR_H, TAXA_MAX_C_POR_H);

  // Reta tangente: temperatura daqui a 1 hora, e so depois o indice de calor
  icProjetado = indiceDeCalor(tempSuave + taxa * HORIZONTE_H, umidSuave);

  // Soma de Riemann: excesso acima de 32 C vezes o intervalo
  grausHora += max(0.0f, icAtual - LIMITE_ATENCAO) * dtHoras;

  tempAnterior = tempSuave;
}

// Escolhe o novo nivel. Sobe ou desce no maximo 1 nivel por vez.
void decidirEstado(unsigned long agora)
{
  // Usa o pior entre o agora e a proxima hora: avisa antes de ficar perigoso
  float icRef = max(icAtual, icProjetado);
  int alvo = nivelDoIC(icRef);

  // Histerese: para descer, precisa ficar abaixo do limite com folga
  if (alvo < nivelClima && icRef >= limiteDoNivel(nivelClima) - HISTERESE)
  {
    alvo = nivelClima;
  }

  int novoNivel = nivelClima;
  if (alvo > nivelClima)
    novoNivel = nivelClima + 1;
  else if (alvo < nivelClima)
    novoNivel = nivelClima - 1;

  bool novoAntecipado = novoNivel > nivelDoIC(icAtual);
  if (novoAntecipado != antecipado)
  {
    antecipado = novoAntecipado;
    redesenhar = true;
  }
  nivelClima = novoNivel;

  // SOS, SEM_RESPOSTA e ERRO_SENSOR so saem pelos proprios caminhos
  if (estado == SOS || estado == SEM_RESPOSTA || estado == ERRO_SENSOR)
    return;

  Estado novo = estadoDoNivel(nivelClima);
  if (novo != estado)
  {
    if (novo == PERIGO)
    {
      aguardandoResposta = true; // pede "Estou bem"
      inicioEspera = agora;
    }
    else
    {
      aguardandoResposta = false;
    }
    mudarEstado(novo);
    publicarEvento(eventoDoEstado(novo));
  }
}

// Em PERIGO sem "Estou bem" por tempo demais -> SEM_RESPOSTA.
// Depois de confirmar, volta a perguntar de tempos em tempos.
void verificarSemResposta(unsigned long agora)
{
  if (estado != PERIGO)
    return;

  if (!aguardandoResposta && agora - ultimaConfirmacao >= INTERVALO_RECONFIRMAR_MS)
  {
    aguardandoResposta = true;
    inicioEspera = agora;
    redesenhar = true;
    Serial.println("[PERIGO] Perguntando de novo se a pessoa esta bem");
  }

  if (aguardandoResposta && agora - inicioEspera >= TEMPO_SEM_RESPOSTA_MS)
  {
    mudarEstado(SEM_RESPOSTA);
    publicarEvento("sem_resposta");
  }
}

// "Estou bem" foi apertado.
void confirmarEstouBem(unsigned long agora)
{
  Serial.println("[BOTAO] Estou bem");
  if (estado == PERIGO || estado == SEM_RESPOSTA || estado == SOS)
  {
    aguardandoResposta = false;
    ultimaConfirmacao = agora;
    redesenhar = true;
    mudarEstado(estadoDoNivel(nivelClima));
    publicarEvento("ok");
  }
}

// Le os dois botoes sem travar o loop.
void verificarBotoes(unsigned long agora)
{
  // Pedir ajuda: precisa ficar pressionado por 2 s (evita toque acidental)
  if (digitalRead(PINO_BTN_SOS) == LOW)
  {
    if (sosPressionadoDesde == 0)
    {
      sosPressionadoDesde = agora;
      Serial.println("[BOTAO] Pedir ajuda: segure por 2 segundos...");
    }
    else if (!sosDisparado && agora - sosPressionadoDesde >= TEMPO_SEGURAR_SOS_MS)
    {
      sosDisparado = true;
      aguardandoResposta = false;
      mudarEstado(SOS);
      publicarEvento("sos");
    }
  }
  else
  {
    if (sosPressionadoDesde != 0 && !sosDisparado)
    {
      Serial.println("[BOTAO] Pedir ajuda cancelado (soltou antes de 2 s)");
    }
    sosPressionadoDesde = 0;
    sosDisparado = false;
  }

  // Estou bem: com debounce de 50 ms
  int leitura = digitalRead(PINO_BTN_OK);
  if (leitura != okLeituraAnterior)
  {
    okMudouEm = agora;
    okLeituraAnterior = leitura;
  }
  if (agora - okMudouEm > DEBOUNCE_MS && leitura != okEstavel)
  {
    okEstavel = leitura;
    if (okEstavel == LOW)
      confirmarEstouBem(agora);
  }
}

// ============================================================
// ALERTAR (local - funciona sem internet)
// ============================================================

uint16_t corDeFundo()
{
  switch (estado)
  {
  case NORMAL:
    return COR_TEAL;
  case ATENCAO:
    return COR_AMBAR;
  case ERRO_SENSOR:
    return COR_CINZA;
  default:
    return COR_VERM; // PERIGO, SEM_RESPOSTA, SOS
  }
}

uint16_t corDoTexto()
{
  return (estado == ATENCAO) ? COR_TINTA : COR_BRANCO; // contraste com o fundo
}

void textoCentralizado(const char *texto, int y, int tamanho, uint16_t cor)
{
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(tamanho);
  tft.getTextBounds(texto, 0, y, &x1, &y1, &w, &h);
  tft.setCursor((320 - w) / 2, y);
  tft.setTextColor(cor);
  tft.print(texto);
}

// Linha grossa: varias linhas finas lado a lado
void linhaGrossa(int x0, int y0, int x1, int y1, int espessura, uint16_t cor)
{
  for (int i = -espessura / 2; i <= espessura / 2; i++)
  {
    tft.drawLine(x0 + i, y0, x1 + i, y1, cor);
    tft.drawLine(x0, y0 + i, x1, y1 + i, cor);
  }
}

void iconeCheck(int cx, int cy, uint16_t cor)
{
  linhaGrossa(cx - 32, cy, cx - 10, cy + 22, 8, cor);
  linhaGrossa(cx - 10, cy + 22, cx + 32, cy - 22, 8, cor);
}

void iconeSol(int cx, int cy, uint16_t cor)
{
  tft.fillCircle(cx, cy, 18, cor);
  for (int k = 0; k < 8; k++)
  {
    float a = k * PI / 4.0;
    linhaGrossa(cx + cos(a) * 26, cy + sin(a) * 26, cx + cos(a) * 38, cy + sin(a) * 38, 4, cor);
  }
}

void iconeX(int cx, int cy, uint16_t cor)
{
  linhaGrossa(cx - 26, cy - 26, cx + 26, cy + 26, 9, cor);
  linhaGrossa(cx - 26, cy + 26, cx + 26, cy - 26, 9, cor);
}

void iconeSirene(int cx, int cy, uint16_t cor, uint16_t fundo)
{
  tft.fillCircle(cx, cy + 12, 24, cor);
  tft.fillRect(cx - 25, cy + 13, 50, 26, fundo); // apaga a metade de baixo: vira cupula
  tft.fillRoundRect(cx - 34, cy + 14, 68, 12, 4, cor);
  linhaGrossa(cx, cy - 34, cx, cy - 22, 4, cor);
  linhaGrossa(cx - 34, cy - 20, cx - 26, cy - 12, 4, cor);
  linhaGrossa(cx + 34, cy - 20, cx + 26, cy - 12, 4, cor);
}

void iconeExclamacao(int cx, int cy, uint16_t cor)
{
  tft.fillRoundRect(cx - 7, cy - 34, 14, 46, 5, cor);
  tft.fillCircle(cx, cy + 26, 8, cor);
}

// Desenha a tela inteira: cor + icone + palavra + orientacao (nunca so a cor)
void desenharTelaCompleta()
{
  uint16_t fundo = corDeFundo();
  uint16_t texto = corDoTexto();
  tft.fillScreen(fundo);

  // Cabecalho: nome e situacao da conexao
  tft.setTextSize(2);
  tft.setTextColor(texto);
  tft.setCursor(8, 6);
  tft.print("Amparo");
  const char *rede = online ? "ONLINE" : "SEM INTERNET";
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(rede, 0, 6, &x1, &y1, &w, &h);
  tft.setCursor(312 - w, 6);
  tft.print(rede);
  tft.drawFastHLine(0, 26, 320, texto);

  // Icone, palavra e orientacao de cada estado
  const char *palavra = "";
  const char *linha1 = "";
  const char *linha2 = "";
  int cx = 160, cy = 74;

  switch (estado)
  {
  case NORMAL:
    iconeCheck(cx, cy, texto);
    palavra = "NORMAL";
    linha1 = "Tudo tranquilo";
    linha2 = antecipado ? "Previsao: mais calor em 1h" : "";
    break;
  case ATENCAO:
    iconeSol(cx, cy, texto);
    palavra = "ATENCAO";
    linha1 = "BEBA AGUA AGORA";
    linha2 = antecipado ? "Previsao: mais calor em 1h" : "Fique no lugar mais fresco";
    break;
  case PERIGO:
    iconeX(cx, cy, texto);
    palavra = "PERIGO";
    linha1 = aguardandoResposta ? "Aperte ESTOU BEM" : "Molhe pulsos e nuca";
    linha2 = antecipado ? "Previsao: mais calor em 1h" : "Beba agua aos poucos";
    break;
  case SEM_RESPOSTA:
    iconeSirene(cx, cy, texto, fundo);
    palavra = "SEM RESPOSTA";
    linha1 = ajudaACaminho ? "AJUDA A CAMINHO"
                           : (online ? "Avisando sua familia" : "SEM INTERNET");
    linha2 = online ? "Aperte ESTOU BEM" : "LIGUE 192 / CHAME VIZINHO";
    break;
  case SOS:
    iconeSirene(cx, cy, texto, fundo);
    palavra = "PEDIU AJUDA";
    linha1 = ajudaACaminho ? "AJUDA A CAMINHO"
                           : (online ? "Familia avisada" : "SEM INTERNET");
    linha2 = online ? "Fique onde esta" : "LIGUE 192 / CHAME VIZINHO";
    break;
  case ERRO_SENSOR:
    iconeExclamacao(cx, cy, texto);
    palavra = "VERIFICAR";
    linha1 = "Sensor com problema";
    linha2 = "Avise sua familia";
    break;
  }

  textoCentralizado(palavra, 122, 4, texto);
  textoCentralizado(linha1, 162, 2, texto);
  textoCentralizado(linha2, 184, 2, texto);
  desenharValores();
}

// Atualiza so a faixa de baixo com os numeros (sem piscar a tela toda)
void desenharValores()
{
  uint16_t fundo = corDeFundo();
  tft.fillRect(0, 210, 320, 30, fundo);
  char linha[40];
  if (isnan(tempSuave))
  {
    snprintf(linha, sizeof(linha), "Lendo sensor...");
  }
  else
  {
    snprintf(linha, sizeof(linha), "Casa %.1fC  Sente %.1fC", tempSuave, icAtual);
  }
  textoCentralizado(linha, 216, 2, corDoTexto());
}

// Padroes de som diferentes por estado, controlados por millis()
void tocarBuzzer(unsigned long agora)
{
  bool ligar = false;
  int freq = 0;

  switch (estado)
  {
  case NORMAL:
    break; // silencio
  case ATENCAO:
    ligar = (agora % INTERVALO_BIPE_ATENCAO_MS) < 200; // bipe curto de tempos em tempos
    freq = 1200;
    break;
  case PERIGO:
    if (aguardandoResposta)
    { // bipe repetido ate apertar "Estou bem"
      ligar = (agora % 1000) < 300;
      freq = 1800;
    }
    break;
  case SEM_RESPOSTA:
  case SOS:
    if (online)
    {
      // com internet: a familia ja foi avisada
      ligar = (estado == SOS) ? (agora % 2000) < 150 : (agora % 1000) < 500;
      freq = 2200;
    }
    else
    {
      // sem internet: alarme insistente para chamar quem esta perto
      ligar = (agora % 400) < 300;
      freq = ((agora % 800) < 400) ? 2600 : 2000;
    }
    break;
  case ERRO_SENSOR:
    ligar = (agora % 10000) < 150;
    freq = 800;
    break;
  }

  if (ligar)
  {
    if (!buzzerLigado || freq != buzzerFreq)
    {
      tone(PINO_BUZZER, freq);
      buzzerLigado = true;
      buzzerFreq = freq;
    }
  }
  else if (buzzerLigado)
  {
    noTone(PINO_BUZZER);
    buzzerLigado = false;
    buzzerFreq = 0;
  }
}

// Saida no Serial Monitor a cada decisao
void imprimirSerial()
{
  Serial.printf("[DECISAO] T=%.1fC UR=%.0f%% | IC=%.1fC | IC em 1h=%.1fC | taxa=%+.2fC/h | "
                "graus-hora=%.2f | nivel=%s%s | estado=%s | %s\n",
                tempSuave, umidSuave, icAtual, icProjetado, taxa, grausHora,
                nomeEstado(estadoDoNivel(nivelClima)), antecipado ? " (antecipado)" : "",
                nomeEstado(estado), online ? "online" : "OFFLINE");
}

// ============================================================
// AVISAR (remoto - so quando ha conexao)
// ============================================================

void definirOnline(bool valor)
{
  if (valor == online)
    return;
  online = valor;
  redesenhar = true;
  Serial.println(online ? "[REDE] ONLINE" : "[REDE] OFFLINE - alertas locais continuam funcionando");
  if (online)
    enviarFila();
}

// Conecta sem travar. A chave do pino 33 simula a queda de internet.
void verificarConexao()
{
  bool simular = digitalRead(PINO_SIM_OFFLINE) == LOW;
  if (simular != simulandoOffline)
  {
    simulandoOffline = simular;
    Serial.println(simular ? "[REDE] Simulando falta de internet"
                           : "[REDE] Simulacao desligada - reconectando");
  }

  if (simulandoOffline)
  {
    if (mqtt.connected())
      mqtt.disconnect();
    if (WiFi.status() == WL_CONNECTED)
      WiFi.disconnect();
    wifiIniciado = false;
    definirOnline(false);
    return;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    definirOnline(false);
    if (!wifiIniciado || millis() - ultimaTentativaWifi >= INTERVALO_REINICIAR_WIFI_MS)
    {
      WiFi.begin(WIFI_SSID, WIFI_SENHA, 6); // canal 6 acelera a conexao no Wokwi
      wifiIniciado = true;
      ultimaTentativaWifi = millis();
      Serial.println("[REDE] Conectando ao Wi-Fi...");
    }
    return;
  }

  if (!mqtt.connected())
  {
    definirOnline(false);
    if (mqtt.connect(ID_MQTT))
    {
      Serial.println("[REDE] MQTT conectado ao FIWARE");
      mqtt.subscribe(TOPICO_COMANDO); // passa a escutar os comandos
    }
    else
    {
      Serial.printf("[REDE] Falha no MQTT (codigo %d)\n", mqtt.state());
      return;
    }
  }
  definirOnline(true);
}

// Formato: atributo|valor|atributo|valor...
// t = temperatura, h = umidade, ic = indice de calor, icp = indice em 1 h,
// tx = taxa (C/h), gh = graus-hora, e = estado
void publicarLeitura()
{
  if (!online || isnan(tempSuave))
    return;
  char payload[160];
  snprintf(payload, sizeof(payload),
           "t|%.1f|h|%.1f|ic|%.1f|icp|%.1f|tx|%.2f|gh|%.2f|e|%s",
           tempSuave, umidSuave, icAtual, icProjetado, taxa, grausHora, nomeEstado(estado));
  mqtt.publish(TOPICO_ATRIBUTOS, payload);
  Serial.printf("[MQTT] %s -> %s\n", TOPICO_ATRIBUTOS, payload);
}

// ev = evento, ee = estado no momento do evento, eic = indice de calor nele,
// ha = ha quantos segundos aconteceu, atr = 1 se veio da fila (estava sem internet)
void enviarEvento(const char *nome, unsigned long quando, Estado est, float ic, bool atrasado)
{
  char payload[120];
  snprintf(payload, sizeof(payload),
           "ev|%s|ee|%s|eic|%.1f|ha|%lu|atr|%d",
           nome, nomeEstado(est), isnan(ic) ? 0.0 : ic,
           (millis() - quando) / 1000, atrasado ? 1 : 0);
  mqtt.publish(TOPICO_ATRIBUTOS, payload);
  Serial.printf("[MQTT] %s -> %s\n", TOPICO_ATRIBUTOS, payload);
}

// Comando do FIWARE chega como "amparo001@ajuda|".
// "ajuda" = a familia ou um voluntario confirmou pelo app que esta indo.
void receberComando(char *topico, byte *payload, unsigned int tamanho)
{
  String msg;
  for (unsigned int i = 0; i < tamanho; i++)
    msg += (char)payload[i];
  Serial.printf("[COMANDO] Recebido: %s\n", msg.c_str());

  String cmdAjuda = String(ID_DISPOSITIVO) + "@ajuda|";
  if (msg.startsWith(cmdAjuda))
  {
    if (estado == SOS || estado == SEM_RESPOSTA)
    {
      ajudaACaminho = true;
      redesenhar = true;
      tone(PINO_BUZZER, 1000, 300); // bipe de aviso: alguem respondeu
    }
    // Confirma ao IoT Agent que o comando foi executado
    String resposta = String(ID_DISPOSITIVO) + "@ajuda|ok";
    mqtt.publish(TOPICO_RESPOSTA, resposta.c_str());
  }
}

// Com internet: envia na hora. Sem internet: guarda na fila.
void publicarEvento(const char *evento)
{
  Serial.printf("[EVENTO] %s\n", evento);
  if (online && mqtt.connected())
  {
    enviarEvento(evento, millis(), estado, icAtual, false);
    return;
  }
  int pos = (filaInicio + filaQtd) % TAMANHO_FILA;
  if (filaQtd == TAMANHO_FILA)
  { // fila cheia: descarta o mais antigo
    filaInicio = (filaInicio + 1) % TAMANHO_FILA;
    filaQtd--;
  }
  strncpy(fila[pos].nome, evento, sizeof(fila[pos].nome) - 1);
  fila[pos].nome[sizeof(fila[pos].nome) - 1] = '\0';
  fila[pos].quando = millis();
  fila[pos].estado = estado; // guarda o estado e o indice do momento do evento
  fila[pos].ic = icAtual;
  filaQtd++;
  Serial.printf("[FILA] Sem internet - evento guardado (%d na fila)\n", filaQtd);
}

// Quando a internet volta, envia o que aconteceu enquanto estava fora
void enviarFila()
{
  if (filaQtd == 0)
    return;
  Serial.printf("[FILA] Enviando %d evento(s) guardado(s)\n", filaQtd);
  while (filaQtd > 0)
  {
    EventoPendente &e = fila[filaInicio];
    enviarEvento(e.nome, e.quando, e.estado, e.ic, true);
    filaInicio = (filaInicio + 1) % TAMANHO_FILA;
    filaQtd--;
  }
}