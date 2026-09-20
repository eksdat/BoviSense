/*
 * ============================================================================
 *  VENTILAÇÃO ADAPTATIVA PARA INSTALAÇÕES DE BOVINOS LEITEIROS — PROTÓTIPO
 *  ESP32 DevKit (30 pinos) | DHT11 | MQ-2 | OLED SSD1306 0,96" I2C | piezo
 *  Cooler de 3 fios acionado por transistor TIP122 (bateria 9 V ou step-up 12 V)
 *
 *  Telemetria (UC Sistemas de Comunicação e Redes):
 *   - Painel Web local (Access Point próprio, http://192.168.4.1)
 *   - Registro CSV pela Serial (115200) e download do histórico em /csv
 *   - Telegram: relatório de hora em hora, alertas e comandos (/status...)
 *   - Chamada de voz de alerta pelo serviço CallMeBot (opcional)
 *
 *  Compatível com Arduino-ESP32 core 2.0.x e 3.x.
 *  Bibliotecas (Gerenciador de Bibliotecas da IDE Arduino):
 *   Adafruit SSD1306, Adafruit GFX Library, DHT sensor library (Adafruit),
 *   Adafruit Unified Sensor, ArduinoJson (versão 7)
 *
 *  NOTAS DE VALIDADE CIENTÍFICA
 *   - O MQ-2 NÃO é sensor de amônia e este firmware NÃO converte leitura
 *     em ppm. IG = R0/Rs é um índice adimensional relativo ao ar de
 *     referência medido no início de cada sessão.
 *   - ITU: Kelly e Bond (1971) apud Azevedo et al. (2005), convertida p/ °C.
 *   - Limiares 72/75/79: Armstrong (1994) apud Azevedo et al. (2005) e
 *     valores críticos de Azevedo et al. (2005) para vacas mestiças HZ.
 *   - As funções críticas (leitura, decisão, acionamento e fail-safe)
 *     rodam no núcleo 1 e NÃO dependem da rede. Toda a comunicação com a
 *     internet roda numa tarefa separada no núcleo 0.
 * ============================================================================
 */
#include <Arduino.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "segredos.h"

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#define CORE3 1
#else
#define CORE3 0
#endif

// ============================ 0. HARDWARE DISPONÍVEL ========================
// SEM_ATUADOR = 1: sem transistor/MOSFET. O cooler fica ligado direto na
//   bateria de 9 V (sempre ligado) e o ESP32 opera em "modo sombra": calcula,
//   registra e mostra o nível que cada estratégia aplicaria, sem acionar nada.
// SEM_ATUADOR = 0: com transistor/MOSFET no GPIO26 (controle real por PWM).
#define SEM_ATUADOR   0
// MQ2_EM_3V3 = 1: MQ-2 alimentado em 3V3 e AO direto no GPIO34 (sem resistores).
//   Seguro para o ESP32, mas o aquecedor fica abaixo dos 5 V da folha de dados.
// MQ2_EM_3V3 = 0: MQ-2 em VIN (5 V) com divisor 10k/20k (configuração completa).
#define MQ2_EM_3V3    1

// ============================ 1. PINOS =====================================
#define PIN_DHT      4    // DATA do DHT11
#define PIN_MQ2      34   // AO do MQ-2 via divisor 10k/20k (ADC1, só entrada)
#define PIN_FAN      26   // base do TIP122 via 1 kOhm (sem uso se SEM_ATUADOR = 1)
#define PIN_BUZZER   25   // piezo
#define PIN_BOTAO    0    // botão BOOT da placa (troca de modo)
#define PIN_LED      2    // LED azul da placa (pulsa a cada amostra)
// OLED I2C: SDA = GPIO21, SCL = GPIO22 (padrão do ESP32)

// ============================ 2. SENSORES ==================================
#define DHT_TIPO        DHT11
#define OFFSET_T_C      0.0f   // correção do Ensaio 0 (referência - DHT11)
#define OFFSET_UR_PCT   0.0f
#if MQ2_EM_3V3
#define MQ2_R1_OHM      0.0f       // sem divisor: AO direto no GPIO34
#define MQ2_R2_OHM      1.0f
#define MQ2_VC_MV       3300.0f    // MQ-2 alimentado pelo 3V3
#else
#define MQ2_R1_OHM      10000.0f   // AO -> GPIO34
#define MQ2_R2_OHM      20000.0f   // GPIO34 -> GND
#define MQ2_VC_MV       5000.0f    // tensão no VCC do MQ-2 (medir no VIN)
#endif
#define MQ2_N_LEITURAS  16         // sobreamostragem do ADC

// ============================ 3. TEMPOS ====================================
#define T_AMOSTRA_MS       2500UL   // DHT11 aceita no máximo 1 leitura/s
#define N_MEDIA            4        // média móvel: 4 x 2,5 s = 10 s
#define T_AQUEC_S          180UL    // aquecimento do MQ-2 a cada energização
#define T_BASE_S           60UL     // aquisição da linha de base (R0)
#define T_DESCIDA_MS       60000UL  // tempo abaixo de (limiar - histerese)
#define T_PERMANENCIA_MS   60000UL  // permanência mínima em um nível
#define T_PARTIDA_MS       1500UL   // pulso de partida a 100 %
#define FALHAS_FAILSAFE    3        // leituras inválidas seguidas -> 100 %

// ============================ 4. CONTROLE ==================================
static float ITU_LIMIAR[3] = {72.0f, 75.0f, 79.0f};
#define ITU_HISTERESE   2.0f   // DHT11: maior que 1 degrau de quantização (~1,4)
static const uint8_t DUTY_NIVEL[4] = {0, 50, 75, 100};   // % por nível
static float IG_LIMIAR[2] = {1.5f, 2.5f};   // recalculados após linha de base
#define IG_HISTERESE    0.2f
#define FAN_PWM_HZ      1000    // 1 kHz: adequado ao TIP122 e ao diodo 1N4007
#define FAN_PWM_BITS    10
#define FAN_INVERTIDO   0       // 1 se o módulo ligar com nível BAIXO
// Potência por nível MEDIDA com medidor USB na saída do power bank que
// alimenta o conversor (Ensaio 0). Valores abaixo = estimativa linear inicial.
#define FAN_P_NOMINAL_W 2.4f    // tensão x corrente do cooler (substituir pelo valor medido)
static float P_NIVEL_W[4] = {0.0f, 0.50f * FAN_P_NOMINAL_W,
                             0.75f * FAN_P_NOMINAL_W, FAN_P_NOMINAL_W};

// ============================ 5. ALERTAS ===================================
#define ALERTA_ITU_S     120    // ITU >= 3º limiar por 2 min -> mensagem
#define LIGACAO_ITU_S    600    // ... por 10 min -> ligação
#define COOLDOWN_MSG_S   600    // intervalo mínimo entre mensagens iguais
#define COOLDOWN_LIG_S   1800   // intervalo mínimo entre ligações
#define LEMBRETE_BIP_S   30     // bip de lembrete enquanto houver alarme
#define TG_POLL_MS       5000   // consulta de comandos do Telegram

// ============================ TIPOS E ESTADO ===============================
enum Modo : uint8_t { MODO_CONTINUO = 0, MODO_LIGA_DESLIGA, MODO_ADAPTATIVO, MODO_DEMO };
static const char *NOME_MODO[] = {"CONTINUO", "LIGA-DESL", "ADAPTATIVO", "DEMO"};
enum Estado : uint8_t { EST_AQUEC = 0, EST_BASE, EST_OPER };

// Máquina de estágios com histerese e tempo mínimo (usada p/ ITU e gás)
struct Estagiador {
  const float *limiar; uint8_t n; float hist;
  uint8_t estagio; uint32_t ultimaTroca; bool abaixo; uint32_t abaixoDesde;
};

struct Metricas {
  uint32_t inicio_ms;
  double t_total_s, t_ligado_s, t_eq_s, t_itu_acima_s, carga_itu_min, energia_wh;
  double t_nivel_s[4], soma_itu;
  uint32_t n_partidas, n_trocas, n_itu;
  float itu_max, itu_min, ig_max;
};

struct Registro {   // histórico em RAM (download em /csv)
  uint32_t t; int16_t T10, U10, ITU10; uint16_t IG100, mv;
  uint8_t nivel, duty, modo, flags;
};
#define N_REG 1500   // ~62 min com amostra de 2,5 s
static Registro regs[N_REG];
static uint16_t regTopo = 0, regQtd = 0;

// bits de "flags"
#define F_DHT_FALHA   0x01
#define F_FAILSAFE    0x02
#define F_MQ_FALHA    0x04
#define F_PREPARO     0x08
#define F_ALARME_GAS  0x10
#define F_PARTIDA     0x20
#define F_UR_FAIXA    0x40
#define F_HORA_NTP    0x80

struct Retrato {  // cópia protegida por mutex para a tarefa de rede
  float T, U, itu, ig; uint8_t nivel, duty, modo, estado, flags;
  Metricas ensaio, hora, ensS[3], horS[3];
};

enum TipoSaida : uint8_t { SAI_MSG = 0, SAI_LIGACAO };
struct MsgSaida { uint8_t tipo; char texto[900]; };
enum TipoCmd : uint8_t { CMD_MODO = 0, CMD_NOVO_ENSAIO };
struct Comando { uint8_t tipo; int arg; };

// ============================ OBJETOS GLOBAIS ==============================
Adafruit_SSD1306 tela(128, 64, &Wire, -1);
DHT dht(PIN_DHT, DHT_TIPO);
WebServer servidor(80);
SemaphoreHandle_t mtx;
QueueHandle_t filaSaida, filaCmd;

static bool telaOk = false;
static Modo modo = MODO_ADAPTATIVO;
static Estado estado = EST_AQUEC;
static uint32_t tInicioEstado = 0, ultimaAmostra = 0;

static float bufT[N_MEDIA], bufU[N_MEDIA], bufR[N_MEDIA];
static uint8_t idxTU = 0, nTU = 0, idxR = 0, nR = 0;
static uint8_t falhasDHT = 0;
static float Traw = NAN, Uraw = NAN, Tf = NAN, Uf = NAN, itu = NAN, ig = NAN;
static uint32_t mqmV = 0;
static float R0rel = NAN, cvBase = 0, ituBase = NAN;
static double somaR = 0, somaR2 = 0, somaItuBase = 0; static uint32_t nBaseR = 0, nBaseItu = 0;
static float ituDemo[3] = {0, 0, 0};

static Estagiador stItu = {ITU_LIMIAR, 3, ITU_HISTERESE, 0, 0, false, 0};
static Estagiador stGas = {IG_LIMIAR, 2, IG_HISTERESE, 0, 0, false, 0};

static uint8_t nivel = 0, dutyAlvo = 0, dutyAnterior = 0, flags = 0;
static uint32_t partidaAte = 0;
static Metricas ensaio, hora;
// Métricas paralelas das estratégias E1 (contínua), E2 (liga-desliga) e E3
// (adaptativa), calculadas sobre os mesmos dados a cada amostra.
static Metricas ensS[3], horS[3];
static uint8_t dutyPrevS[3] = {0, 0, 0};
static Retrato retrato;

// alertas
static uint32_t ituCritDesde = 0; static bool ituCritAtivo = false, ituAvisou = false;
static bool gasAlarme = false, failsafeAtivo = false;
static uint32_t ultMsg[3] = {0, 0, 0}, ultLig[3] = {0, 0, 0};  // 0=ITU 1=gás 2=sensor
static uint32_t ultLembrete = 0;
static int ultimaHoraRelatorio = -1; static uint32_t ultRelatorioMs = 0;

// buzzer não bloqueante
static uint32_t bipFim = 0, bipProx = 0; static uint8_t bipsPend = 0;

// ============================ LEDC (PWM) ===================================
#define CANAL_FAN 0   // timer 0
#define CANAL_BUZ 2   // timer 1 (não interfere na frequência do ventilador)
static void pwmIniciar() {
#if CORE3
  ledcAttachChannel(PIN_FAN, FAN_PWM_HZ, FAN_PWM_BITS, CANAL_FAN);
  ledcAttachChannel(PIN_BUZZER, 2000, 10, CANAL_BUZ);
  ledcWrite(PIN_BUZZER, 0);
#else
  ledcSetup(CANAL_FAN, FAN_PWM_HZ, FAN_PWM_BITS); ledcAttachPin(PIN_FAN, CANAL_FAN);
  ledcSetup(CANAL_BUZ, 2000, 10); ledcAttachPin(PIN_BUZZER, CANAL_BUZ);
  ledcWrite(CANAL_BUZ, 0);
#endif
}
static void fanEscrever(uint8_t pct) {
  if (SEM_ATUADOR) pct = 0;   // nada ligado ao GPIO26: pino mantido em nível baixo
  uint32_t maxv = (1UL << FAN_PWM_BITS) - 1;
  uint32_t d = (uint32_t)pct * maxv / 100UL;
  if (FAN_INVERTIDO) d = maxv - d;
#if CORE3
  ledcWrite(PIN_FAN, d);
#else
  ledcWrite(CANAL_FAN, d);
#endif
}
static void buzzerTom(uint32_t f) {
#if CORE3
  if (f == 0) ledcWrite(PIN_BUZZER, 0); else ledcWriteTone(PIN_BUZZER, f);
#else
  if (f == 0) ledcWrite(CANAL_BUZ, 0); else ledcWriteTone(CANAL_BUZ, f);
#endif
}
static void bipsAgendar(uint8_t n) { bipsPend = n; bipProx = millis(); }
static void bipServico() {
  uint32_t agora = millis();
  if (bipFim && (int32_t)(agora - bipFim) >= 0) { buzzerTom(0); bipFim = 0; }
  if (!bipFim && bipsPend && (int32_t)(agora - bipProx) >= 0) {
    buzzerTom(2200); bipFim = agora + 150; bipsPend--; bipProx = agora + 400;
  }
}

// ============================ CÁLCULOS =====================================
// ITU (Kelly e Bond, 1971) convertido para °C; UR em %
static float calcITU(float T, float UR) {
  return (1.8f * T + 32.0f) - (0.55f - 0.0055f * UR) * (1.8f * T - 26.0f);
}
static float media(const float *b, uint8_t n) {
  float s = 0; for (uint8_t i = 0; i < n; i++) s += b[i]; return n ? s / n : NAN;
}
static uint32_t lerMQ2mV() {
  uint32_t s = 0;
  for (int i = 0; i < MQ2_N_LEITURAS; i++) s += analogReadMilliVolts(PIN_MQ2);
  return s / MQ2_N_LEITURAS;
}
static bool horaValida() { return time(nullptr) > 1700000000; }
static uint32_t carimbo() { return horaValida() ? (uint32_t)time(nullptr) : millis() / 1000UL; }

// Sobe imediatamente; desce um estágio por vez após T_DESCIDA_MS abaixo de
// (limiar - histerese) e respeitando a permanência mínima.
static uint8_t estagiar(Estagiador &e, float x, uint32_t agora) {
  uint8_t alvo = 0;
  for (uint8_t k = 0; k < e.n; k++) if (x >= e.limiar[k]) alvo = k + 1;
  if (alvo > e.estagio) { e.estagio = alvo; e.ultimaTroca = agora; e.abaixo = false; return e.estagio; }
  if (e.estagio > 0 && x < e.limiar[e.estagio - 1] - e.hist) {
    if (!e.abaixo) { e.abaixo = true; e.abaixoDesde = agora; }
    if (agora - e.abaixoDesde >= T_DESCIDA_MS && agora - e.ultimaTroca >= T_PERMANENCIA_MS) {
      e.estagio--; e.ultimaTroca = agora; e.abaixo = false;
    }
  } else e.abaixo = false;
  return e.estagio;
}
static void zerarEstagiador(Estagiador &e) { e.estagio = 0; e.ultimaTroca = millis(); e.abaixo = false; }

static void zerarMetricas(Metricas &m) {
  memset(&m, 0, sizeof(m)); m.inicio_ms = millis();
  m.itu_max = -1000; m.itu_min = 1000; m.ig_max = 0;
}
static void zerarEnsaio() { zerarMetricas(ensaio); for (int i = 0; i < 3; i++) zerarMetricas(ensS[i]); }
static void zerarHora() { zerarMetricas(hora); for (int i = 0; i < 3; i++) zerarMetricas(horS[i]); }
static void acumular(Metricas &m, float dt, uint8_t niv, uint8_t duty, bool partida, bool troca) {
  m.t_total_s += dt;
  m.t_nivel_s[niv] += dt;
  if (duty > 0) m.t_ligado_s += dt;
  m.t_eq_s += dt * duty / 100.0;                // tempo equivalente a plena carga
  m.energia_wh += P_NIVEL_W[niv] * dt / 3600.0;   // energia (estimada ou calibrada)
  if (partida) m.n_partidas++;
  if (troca) m.n_trocas++;
  if (!isnan(itu)) {
    m.n_itu++; m.soma_itu += itu;
    if (itu > m.itu_max) m.itu_max = itu;
    if (itu < m.itu_min) m.itu_min = itu;
    if (itu >= ITU_LIMIAR[0]) { m.t_itu_acima_s += dt; m.carga_itu_min += (itu - ITU_LIMIAR[0]) * dt / 60.0; }
  }
  if (!isnan(ig) && ig > m.ig_max) m.ig_max = ig;
}

// ============================ SAÍDAS DE REDE ===============================
static bool tgAtivo() { return strlen(TG_TOKEN) > 0 && strlen(TG_CHAT_ID) > 0; }
static bool ligacaoAtiva() { return strlen(CALLMEBOT_USER) > 0; }
static void enfileirar(uint8_t tipo, const char *txt) {
  if (tipo == SAI_MSG && !tgAtivo()) return;
  if (tipo == SAI_LIGACAO && !ligacaoAtiva()) return;
  MsgSaida m; m.tipo = tipo; strlcpy(m.texto, txt, sizeof(m.texto));
  xQueueSend(filaSaida, &m, 0);   // nunca bloqueia o controle
}
static void alertar(uint8_t canal, const char *msg, bool ligar) {
  uint32_t s = millis() / 1000UL;
  if (ultMsg[canal] == 0 || s - ultMsg[canal] >= COOLDOWN_MSG_S) { enfileirar(SAI_MSG, msg); ultMsg[canal] = s ? s : 1; }
  if (ligar && (ultLig[canal] == 0 || s - ultLig[canal] >= COOLDOWN_LIG_S)) { enfileirar(SAI_LIGACAO, msg); ultLig[canal] = s ? s : 1; }
}

static void textoMetricas(char *out, size_t n, const Metricas &m, const char *titulo) {
  // (em modo sombra, estas métricas descrevem a estratégia selecionada, não o cooler real)
  double min = m.t_total_s / 60.0;
  float pctAcima = m.t_total_s > 0 ? 100.0 * m.t_itu_acima_s / m.t_total_s : 0;
  float ituMed = m.n_itu ? m.soma_itu / m.n_itu : NAN;
  snprintf(out, n,
           "%s\nModo: %s | duracao: %.0f min\n"
           "ITU medio %.1f (min %.1f / max %.1f)\n"
           "Tempo com ITU >= %.0f: %.0f %%\n"
           "Carga termica acima do limiar: %.1f ITU.min\n"
           "Ventilador ligado: %.1f min (plena carga eq.: %.1f min)\n"
           "Energia (estimada/calibrada): %.2f Wh | partidas: %lu | trocas de nivel: %lu\n"
           "IG maximo: %.2f (indice relativo, nao e ppm)",
           titulo, NOME_MODO[modo], min, ituMed, m.n_itu ? m.itu_min : NAN, m.n_itu ? m.itu_max : NAN,
           ITU_LIMIAR[0], pctAcima, m.carga_itu_min, m.t_ligado_s / 60.0, m.t_eq_s / 60.0,
           m.energia_wh, (unsigned long)m.n_partidas, (unsigned long)m.n_trocas, m.ig_max);
}

// Comparação das estratégias sobre os mesmos dados (modo sombra quando SEM_ATUADOR)
static void textoEstrategias(char *out, size_t n, const Metricas *m) {
  double tot = m[0].t_total_s > 0 ? m[0].t_total_s : 1;
  snprintf(out, n,
           "%s\nPlena carga equivalente que cada estrategia usaria:\n"
           "E1 continua: %.1f min (%.0f %%)\nE2 liga-desliga: %.1f min (%.0f %%), partidas %lu, trocas %lu\n"
           "E3 adaptativa: %.1f min (%.0f %%), partidas %lu, trocas %lu\n"
           "Reducao estimada E3 x E1: %.0f %% (supoe potencia proporcional a razao ciclica)",
           SEM_ATUADOR ? "MODO SOMBRA: cooler sempre ligado; niveis calculados, nao aplicados." : "Estrategias calculadas sobre os mesmos dados:",
           m[0].t_eq_s / 60.0, 100.0 * m[0].t_eq_s / tot,
           m[1].t_eq_s / 60.0, 100.0 * m[1].t_eq_s / tot, (unsigned long)m[1].n_partidas, (unsigned long)m[1].n_trocas,
           m[2].t_eq_s / 60.0, 100.0 * m[2].t_eq_s / tot, (unsigned long)m[2].n_partidas, (unsigned long)m[2].n_trocas,
           m[0].t_eq_s > 0 ? 100.0 * (m[0].t_eq_s - m[2].t_eq_s) / m[0].t_eq_s : 0.0);
}

// ============================ AMOSTRAGEM E CONTROLE ========================
static void iniciarEstado(Estado e) { estado = e; tInicioEstado = millis(); }

static void registrar(uint32_t t) {
  Registro &r = regs[regTopo];
  r.t = t;
  r.T10 = isnan(Tf) ? INT16_MIN : (int16_t)lroundf(Tf * 10);
  r.U10 = isnan(Uf) ? INT16_MIN : (int16_t)lroundf(Uf * 10);
  r.ITU10 = isnan(itu) ? INT16_MIN : (int16_t)lroundf(itu * 10);
  r.IG100 = isnan(ig) ? 0xFFFF : (uint16_t)constrain(lroundf(ig * 100), 0, 65000);
  r.mv = (uint16_t)mqmV; r.nivel = nivel; r.duty = dutyAlvo; r.modo = modo; r.flags = flags;
  regTopo = (regTopo + 1) % N_REG; if (regQtd < N_REG) regQtd++;
}

static void imprimirCabecalhoCSV() {
  Serial.println(F("t,modo,estado,T_bruta_C,UR_bruta_pct,T_filt_C,UR_filt_pct,ITU,mq2_mV,IG,estagio_ITU,estagio_gas,nivel,duty_pct,flags"));
}

static void amostrar() {
  uint32_t agora = millis();
  float dt = ultimaAmostra ? (agora - ultimaAmostra) / 1000.0f : 0; ultimaAmostra = agora;
  flags = 0;

  // ---- DHT11 ----
  float t = dht.readTemperature(), u = dht.readHumidity();
  bool ok = !isnan(t) && !isnan(u) && t > -5 && t < 60 && u > 0 && u <= 100;
  if (ok) {
    Traw = t + OFFSET_T_C; Uraw = constrain(u + OFFSET_UR_PCT, 0.0f, 100.0f);
    bufT[idxTU] = Traw; bufU[idxTU] = Uraw; idxTU = (idxTU + 1) % N_MEDIA; if (nTU < N_MEDIA) nTU++;
    falhasDHT = 0;
    if (Uraw < 20 || Uraw > 90 || Traw > 50) flags |= F_UR_FAIXA;   // fora da faixa do DHT11
  } else { if (falhasDHT < 255) falhasDHT++; flags |= F_DHT_FALHA; }
  bool failsafe = falhasDHT >= FALHAS_FAILSAFE;
  if (failsafe) flags |= F_FAILSAFE;
  Tf = nTU ? media(bufT, nTU) : NAN; Uf = nTU ? media(bufU, nTU) : NAN;
  itu = (nTU && !failsafe) ? calcITU(Tf, Uf) : NAN;

  // ---- MQ-2: Rs/RL = (Vc - Vao)/Vao ; RL se cancela em IG = R0/Rs ----
  mqmV = lerMQ2mV();
  float vao = mqmV * (MQ2_R1_OHM + MQ2_R2_OHM) / MQ2_R2_OHM;
  bool mqOk = vao > 50.0f && vao < (MQ2_VC_MV - 50.0f) && mqmV < 3050;
  if (mqOk) { bufR[idxR] = (MQ2_VC_MV - vao) / vao; idxR = (idxR + 1) % N_MEDIA; if (nR < N_MEDIA) nR++; }
  else flags |= F_MQ_FALHA;
  float rsRel = nR ? media(bufR, nR) : NAN;
  ig = (!isnan(R0rel) && mqOk && rsRel > 0) ? R0rel / rsRel : NAN;

  // ---- estados: aquecimento -> linha de base -> operação ----
  uint8_t estItu = stItu.estagio, estGas = stGas.estagio;
  if (estado != EST_OPER) {
    flags |= F_PREPARO;
    if (estado == EST_AQUEC && agora - tInicioEstado >= T_AQUEC_S * 1000UL) {
      iniciarEstado(EST_BASE); somaR = somaR2 = somaItuBase = 0; nBaseR = nBaseItu = 0;
    } else if (estado == EST_BASE) {
      if (mqOk) { float r = (MQ2_VC_MV - vao) / vao; somaR += r; somaR2 += (double)r * r; nBaseR++; }
      if (!isnan(itu)) { somaItuBase += itu; nBaseItu++; }
      if (agora - tInicioEstado >= T_BASE_S * 1000UL) {
        if (nBaseR >= 5) {
          R0rel = somaR / nBaseR;
          double var = somaR2 / nBaseR - (double)R0rel * R0rel;
          cvBase = var > 0 ? sqrt(var) / R0rel : 0;
          IG_LIMIAR[0] = max(1.5f, 1.0f + 6.0f * cvBase);     // 6 desvios do ruído de base
          IG_LIMIAR[1] = max(2.5f, IG_LIMIAR[0] + 0.5f);
        }
        ituBase = nBaseItu ? somaItuBase / nBaseItu : NAN;
        for (int k = 0; k < 3; k++) ituDemo[k] = isnan(ituBase) ? ITU_LIMIAR[k] : ituBase + 1.0f + k;
        stItu.limiar = (modo == MODO_DEMO) ? ituDemo : ITU_LIMIAR;
        zerarEstagiador(stItu); zerarEstagiador(stGas);
        zerarEnsaio(); zerarHora();
        for (int i = 0; i < 3; i++) dutyPrevS[i] = 0;
        iniciarEstado(EST_OPER);
        Serial.printf("# BASE: R0/RL=%.3f cv=%.3f IG_limiares=%.2f/%.2f ITU_base=%.1f\n",
                      R0rel, cvBase, IG_LIMIAR[0], IG_LIMIAR[1], ituBase);
        char msg[200];
        snprintf(msg, sizeof(msg), "Sistema pronto. Modo %s. ITU de base %.1f. Limiares de IG %.2f / %.2f.",
                 NOME_MODO[modo], ituBase, IG_LIMIAR[0], IG_LIMIAR[1]);
        enfileirar(SAI_MSG, msg);
      }
    }
    nivel = failsafe ? 3 : 0;
  } else {
    // ---- decisão: as três estratégias são calculadas a cada amostra ----
    uint8_t nivS[3];
    nivS[0] = 3;                                                        // E1 contínua
    nivS[1] = ((!isnan(itu) && itu >= ITU_LIMIAR[0]) || (!isnan(ig) && ig >= IG_LIMIAR[0])) ? 3 : 0;  // E2
    if (!isnan(itu)) estItu = estagiar(stItu, itu, agora);             // sem leitura: mantém o estágio
    estGas = !isnan(ig) ? estagiar(stGas, ig, agora) : 0;
    uint8_t nGas = estGas == 0 ? 0 : (estGas == 1 ? 2 : 3);
    nivS[2] = max(estItu, nGas);                                        // E3 adaptativa (ou DEMO)
    if (failsafe) nivS[1] = nivS[2] = 3;
    nivel = (modo == MODO_CONTINUO) ? nivS[0] : (modo == MODO_LIGA_DESLIGA) ? nivS[1] : nivS[2];
    if (dt > 0 && dt < 30) {
      for (int i = 0; i < 3; i++) {
        uint8_t dS = DUTY_NIVEL[nivS[i]];
        acumular(ensS[i], dt, nivS[i], dS, dS > 0 && dutyPrevS[i] == 0, dS != dutyPrevS[i]);
        acumular(horS[i], dt, nivS[i], dS, dS > 0 && dutyPrevS[i] == 0, dS != dutyPrevS[i]);
        dutyPrevS[i] = dS;
      }
    }
  }

  // ---- atuação ----
  uint8_t novoDuty = DUTY_NIVEL[nivel];
  bool partida = (novoDuty > 0 && dutyAnterior == 0);
  bool troca = (novoDuty != dutyAnterior);
  if (partida) partidaAte = agora + T_PARTIDA_MS;
  if (SEM_ATUADOR && estado == EST_OPER && novoDuty > dutyAnterior) bipsAgendar(1);  // recomendação subiu
  dutyAlvo = novoDuty; dutyAnterior = novoDuty;
  if ((int32_t)(partidaAte - agora) > 0) flags |= F_PARTIDA;
  if (horaValida()) flags |= F_HORA_NTP;
  bool alarmeGasAgora = (estado == EST_OPER && modo >= MODO_ADAPTATIVO && estGas >= 2);
  if (alarmeGasAgora) flags |= F_ALARME_GAS;

  // ---- métricas (somente em operação) ----
  if (estado == EST_OPER && dt > 0 && dt < 30) {
    acumular(ensaio, dt, nivel, novoDuty, partida, troca);
    acumular(hora, dt, nivel, novoDuty, partida, troca);
  }

  // ---- alertas locais e remotos ----
  char msg[240];
  if (failsafe && !failsafeAtivo) {
    bipsAgendar(3);
    snprintf(msg, sizeof(msg), "FALHA no sensor de temperatura/umidade. Ventilacao forcada em 100%% (modo seguro).");
    alertar(2, msg, true);
  } else if (!failsafe && failsafeAtivo) {
    enfileirar(SAI_MSG, "Sensor de temperatura/umidade voltou a responder. Controle normal retomado.");
  }
  failsafeAtivo = failsafe;

  if (alarmeGasAgora && !gasAlarme) {
    bipsAgendar(3);
    snprintf(msg, sizeof(msg), "ALERTA de gases: indice relativo IG = %.2f (limiar %.2f). Ventilacao em 100%%.", ig, IG_LIMIAR[1]);
    alertar(1, msg, true);
  } else if (!alarmeGasAgora && gasAlarme) {
    enfileirar(SAI_MSG, "Indice relativo de gases voltou abaixo do limiar de alarme.");
  }
  gasAlarme = alarmeGasAgora;

  const float *lim = (modo == MODO_DEMO) ? ituDemo : ITU_LIMIAR;
  if (estado == EST_OPER && !isnan(itu) && itu >= lim[2]) {
    if (!ituCritAtivo) { ituCritAtivo = true; ituCritDesde = agora; }
    uint32_t dur = (agora - ituCritDesde) / 1000UL;
    if (dur >= ALERTA_ITU_S && !ituAvisou) {
      snprintf(msg, sizeof(msg), "ALERTA termico: ITU = %.1f (>= %.0f) ha %lu min. Ventilacao no nivel %u (%u%%).",
               itu, lim[2], (unsigned long)(dur / 60), nivel, novoDuty);
      alertar(0, msg, false); ituAvisou = true; bipsAgendar(2);
    }
    if (dur >= LIGACAO_ITU_S) {
      snprintf(msg, sizeof(msg), "Alerta termico persistente. ITU %.0f ha mais de %lu minutos.", itu, (unsigned long)(dur / 60));
      alertar(0, msg, true);
    }
  } else if (!isnan(itu) && itu < lim[2] - ITU_HISTERESE) {
    if (ituAvisou) enfileirar(SAI_MSG, "ITU voltou abaixo da faixa de estresse moderado.");
    ituCritAtivo = false; ituAvisou = false;
  }
  if ((failsafe || alarmeGasAgora) && millis() - ultLembrete > LEMBRETE_BIP_S * 1000UL) { bipsAgendar(1); ultLembrete = millis(); }

  // ---- relatório de hora em hora ----
  if (estado == EST_OPER) {
    bool enviar = false; char titulo[64];
    if (horaValida()) {
      time_t tt = time(nullptr); struct tm lt; localtime_r(&tt, &lt);
      if (ultimaHoraRelatorio < 0) ultimaHoraRelatorio = lt.tm_hour;
      else if (lt.tm_hour != ultimaHoraRelatorio) {
        ultimaHoraRelatorio = lt.tm_hour; enviar = true;
        snprintf(titulo, sizeof(titulo), "Relatorio horario - %02d:00 (%02d/%02d)", lt.tm_hour, lt.tm_mday, lt.tm_mon + 1);
      }
    } else if (millis() - ultRelatorioMs >= 3600000UL && hora.t_total_s >= 3500) {
      enviar = true; snprintf(titulo, sizeof(titulo), "Relatorio da ultima hora (sem relogio NTP)");
    }
    if (enviar) {
      char txt[900], est[420]; textoMetricas(txt, sizeof(txt), hora, titulo);
      textoEstrategias(est, sizeof(est), horS);
      strlcat(txt, "\n\n", sizeof(txt)); strlcat(txt, est, sizeof(txt));
      enfileirar(SAI_MSG, txt); ultRelatorioMs = millis(); zerarHora();
    }
  }

  // ---- registro (Serial CSV + RAM) ----
  uint32_t ts = carimbo();
  registrar(ts);
  Serial.printf("%lu,%s,%u,%.1f,%.1f,%.2f,%.2f,%.2f,%lu,%.3f,%u,%u,%u,%u,%u\n",
                (unsigned long)ts, NOME_MODO[modo], estado, Traw, Uraw, Tf, Uf, itu,
                (unsigned long)mqmV, ig, estItu, estGas, nivel, novoDuty, flags);

  // ---- retrato para a tarefa de rede ----
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
    retrato.T = Tf; retrato.U = Uf; retrato.itu = itu; retrato.ig = ig;
    retrato.nivel = nivel; retrato.duty = novoDuty; retrato.modo = modo; retrato.estado = estado;
    retrato.flags = flags; retrato.ensaio = ensaio; retrato.hora = hora;
    for (int i = 0; i < 3; i++) { retrato.ensS[i] = ensS[i]; retrato.horS[i] = horS[i]; }
    xSemaphoreGive(mtx);
  }
  digitalWrite(PIN_LED, !digitalRead(PIN_LED));
}

static void servicoVentilador() {
  uint32_t agora = millis();
  fanEscrever((dutyAlvo > 0 && (int32_t)(partidaAte - agora) > 0) ? 100 : dutyAlvo);
}

static void novoEnsaio() {
  char txt[900]; textoMetricas(txt, sizeof(txt), ensaio, "Resumo do ensaio encerrado");
  Serial.println(F("# ===== RESUMO DO ENSAIO =====")); Serial.println(txt);
  textoEstrategias(txt, sizeof(txt), ensS); Serial.println(txt);
  zerarEnsaio(); for (int i = 0; i < 3; i++) dutyPrevS[i] = 0;
  zerarEstagiador(stItu); zerarEstagiador(stGas);
  Serial.println(F("# ===== NOVO ENSAIO =====")); imprimirCabecalhoCSV();
}
static void definirModo(int m) {
  if (m < 0 || m > 3) return;
  modo = (Modo)m;
  stItu.limiar = (modo == MODO_DEMO) ? ituDemo : ITU_LIMIAR;
  Serial.printf("# MODO -> %s\n", NOME_MODO[modo]);
  novoEnsaio();
  bipsAgendar(m + 1);
}

// ============================ TELA =========================================
static void desenharTela() {
  if (!telaOk) return;
  tela.clearDisplay(); tela.setTextSize(1); tela.setTextColor(SSD1306_WHITE); tela.setCursor(0, 0);
  if (estado != EST_OPER) {
    uint32_t dec = (millis() - tInicioEstado) / 1000UL;
    uint32_t tot = (estado == EST_AQUEC) ? T_AQUEC_S : T_BASE_S;
    tela.println(estado == EST_AQUEC ? F("AQUECENDO MQ-2") : F("LINHA DE BASE (R0)"));
    tela.printf("faltam %lu s\n\n", (unsigned long)(tot > dec ? tot - dec : 0));
    if (!isnan(Tf)) tela.printf("T %.0fC  UR %.0f%%\n", Tf, Uf); else tela.println(F("DHT11 sem leitura"));
    tela.printf("MQ-2 %lu mV\n", (unsigned long)mqmV);
    if (estado == EST_AQUEC) tela.println(F("BOOT = pular aquec."));
  } else {
    if (!isnan(Tf)) tela.printf("T %.0fC UR %.0f%% %s\n", Tf, Uf, NOME_MODO[modo]);
    else tela.printf("T --  UR --  %s\n", NOME_MODO[modo]);
    if (!isnan(itu)) tela.printf("ITU %.1f  Nivel %u\n", itu, nivel); else tela.printf("ITU --  Nivel %u\n", nivel);
    if (!isnan(ig)) tela.printf("IG %.2f  MQ %lumV\n", ig, (unsigned long)mqmV); else tela.println(F("IG --"));
    tela.printf(SEM_ATUADOR ? "Recom %u%%" : "Vent %u%%", dutyAlvo);
    tela.drawRect(60, 24, 66, 7, SSD1306_WHITE); tela.fillRect(61, 25, 64 * dutyAlvo / 100, 5, SSD1306_WHITE);
    tela.setCursor(0, 34);
    if (WiFi.status() == WL_CONNECTED) tela.printf("Net %s\n", WiFi.localIP().toString().c_str());
    else tela.printf("AP %s\n", WiFi.softAPIP().toString().c_str());
    tela.printf("Ens %.0fmin E %.2fWh\n", ensaio.t_total_s / 60.0, ensaio.energia_wh);
    if (flags & F_FAILSAFE) tela.println(F("FALHA DHT -> 100%"));
    else if (flags & F_ALARME_GAS) tela.println(F("ALERTA: GASES"));
    else if (ituCritAtivo) tela.println(F("ALERTA: ITU ALTO"));
    else if (flags & F_UR_FAIXA) tela.println(F("fora da faixa DHT11"));
  }
  tela.display();
}

// ============================ WEB (painel local) ===========================
static const char PAGINA[] PROGMEM = R"HTML(<!doctype html><html lang="pt-br"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Ventilacao adaptativa</title>
<style>body{font-family:sans-serif;margin:16px;max-width:560px}.v{font-size:1.6em;font-weight:bold}
td{padding:3px 8px}button{margin:3px;padding:8px}</style></head><body>
<h2>Ventilacao adaptativa - bovinos</h2><table id="t"></table>
<p><button onclick="m(0)">Continuo</button><button onclick="m(1)">Liga-desliga</button>
<button onclick="m(2)">Adaptativo</button><button onclick="m(3)">Demo</button></p>
<p><button onclick="fetch('/novo')">Novo ensaio</button> <a href="/csv">Baixar CSV</a></p>
<p><small>IG e um indice relativo do MQ-2 (nao e concentracao em ppm).</small></p>
<script>function m(x){fetch('/modo?m='+x)}
function f(){fetch('/api').then(r=>r.json()).then(d=>{let h='';for(const k in d)h+='<tr><td>'+k+'</td><td class=v>'+d[k]+'</td></tr>';
document.getElementById('t').innerHTML=h})}setInterval(f,2500);f()</script></body></html>)HTML";

static void webApi() {
  char b[1200];
  snprintf(b, sizeof(b),
           "{\"modo\":\"%s\",\"estado\":%u,\"T_C\":%.1f,\"UR_pct\":%.1f,\"ITU\":%.1f,\"IG\":%.2f,"
           "\"mq2_mV\":%lu,\"nivel\":%u,\"ventilador_pct\":%u,\"flags\":%u,"
           "\"ensaio_min\":%.1f,\"ligado_min\":%.1f,\"plena_carga_eq_min\":%.1f,\"energia_Wh\":%.3f,"
           "\"partidas\":%lu,\"trocas\":%lu,\"t_ITU_acima_min\":%.1f,\"carga_ITU_min\":%.1f,\"hora_NTP\":%s,"
           "\"atuador\":\"%s\",\"E1_eq_min\":%.1f,\"E2_eq_min\":%.1f,\"E3_eq_min\":%.1f,\"E2_trocas\":%lu,\"E3_trocas\":%lu}",
           NOME_MODO[modo], estado, Tf, Uf, itu, ig, (unsigned long)mqmV, nivel, dutyAlvo, flags,
           ensaio.t_total_s / 60.0, ensaio.t_ligado_s / 60.0, ensaio.t_eq_s / 60.0, ensaio.energia_wh,
           (unsigned long)ensaio.n_partidas, (unsigned long)ensaio.n_trocas, ensaio.t_itu_acima_s / 60.0,
           ensaio.carga_itu_min, horaValida() ? "true" : "false", SEM_ATUADOR ? "sombra" : "real",
           ensS[0].t_eq_s / 60.0, ensS[1].t_eq_s / 60.0, ensS[2].t_eq_s / 60.0,
           (unsigned long)ensS[1].n_trocas, (unsigned long)ensS[2].n_trocas);
  String s(b); s.replace("nan", "null");
  servidor.send(200, "application/json", s);
}
static void webCsv() {
  servidor.setContentLength(CONTENT_LENGTH_UNKNOWN);
  servidor.sendHeader("Content-Disposition", "attachment; filename=ventilacao.csv");
  servidor.send(200, "text/csv", "");
  servidor.sendContent("t,modo,T_C,UR_pct,ITU,IG,mq2_mV,nivel,duty_pct,flags\n");
  char linha[128]; String bloco;
  uint16_t ini = (regQtd == N_REG) ? regTopo : 0;
  for (uint16_t i = 0; i < regQtd; i++) {
    const Registro &r = regs[(ini + i) % N_REG];
    snprintf(linha, sizeof(linha), "%lu,%s,%.1f,%.1f,%.1f,%.2f,%u,%u,%u,%u\n", (unsigned long)r.t, NOME_MODO[r.modo],
             r.T10 == INT16_MIN ? NAN : r.T10 / 10.0, r.U10 == INT16_MIN ? NAN : r.U10 / 10.0,
             r.ITU10 == INT16_MIN ? NAN : r.ITU10 / 10.0, r.IG100 == 0xFFFF ? NAN : r.IG100 / 100.0,
             r.mv, r.nivel, r.duty, r.flags);
    bloco += linha;
    if (bloco.length() > 1200) { servidor.sendContent(bloco); bloco = ""; }
  }
  if (bloco.length()) servidor.sendContent(bloco);
  servidor.sendContent("");
}

// ============================ TAREFA DE REDE (núcleo 0) ====================
static String urlCodificar(const char *s) {
  String o; const char *hex = "0123456789ABCDEF";
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else if (c == ' ') o += '+';
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}
static bool tgEnviar(const char *texto) {
  WiFiClientSecure cli; cli.setInsecure();   // limitação discutida no artigo (sem validação de certificado)
  HTTPClient http; http.setTimeout(10000);
  String url = String("https://api.telegram.org/bot") + TG_TOKEN + "/sendMessage";
  if (!http.begin(cli, url)) return false;
  http.addHeader("Content-Type", "application/json");
  JsonDocument doc; doc["chat_id"] = TG_CHAT_ID; doc["text"] = texto;
  String corpo; serializeJson(doc, corpo);
  int cod = http.POST(corpo);
  if (cod != 200) {
    String r = cod > 0 ? http.getString() : String("sem resposta (DNS/TLS/internet)");
    Serial.printf("# TELEGRAM: erro HTTP %d -> %s\n", cod, r.substring(0, 160).c_str());
    if (cod == 400) Serial.println(F("# TELEGRAM: 'chat not found' = TG_CHAT_ID errado ou voce nao enviou /start ao bot"));
    if (cod == 401 || cod == 404) Serial.println(F("# TELEGRAM: token invalido ou revogado (TG_TOKEN)"));
  }
  http.end();
  return cod == 200;
}
static bool fazerLigacao(const char *texto) {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http; http.setTimeout(20000);
  String url = String("https://api.callmebot.com/start.php?user=") + urlCodificar(CALLMEBOT_USER) +
               "&text=" + urlCodificar(texto) + "&lang=" + CALLMEBOT_LANG + "&rpt=2";
  if (!http.begin(cli, url)) return false;
  int cod = http.GET(); http.end();
  return cod == 200;
}
static void tgResponder(const char *cmd) {
  static char txt[900]; static Retrato r;
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
  r = retrato; xSemaphoreGive(mtx);
  if (!strncmp(cmd, "/status", 7)) {
    snprintf(txt, sizeof(txt), "Status agora\nModo: %s | estado: %u\nT %.0f C | UR %.0f %%\nITU %.1f | IG %.2f\n"
             "Ventilacao: nivel %u (%u %%)\nEnsaio: %.0f min, energia %.2f Wh, partidas %lu",
             NOME_MODO[r.modo], r.estado, r.T, r.U, r.itu, r.ig, r.nivel, r.duty,
             r.ensaio.t_total_s / 60.0, r.ensaio.energia_wh, (unsigned long)r.ensaio.n_partidas);
    tgEnviar(txt);
  } else if (!strncmp(cmd, "/relatorio", 10)) {
    textoMetricas(txt, sizeof(txt), r.hora, "Hora corrente (parcial)"); tgEnviar(txt);
    textoMetricas(txt, sizeof(txt), r.ensaio, "Ensaio atual"); tgEnviar(txt);
    textoEstrategias(txt, sizeof(txt), r.ensS); tgEnviar(txt);
  } else if (!strncmp(cmd, "/modo", 5)) {
    int m = atoi(cmd + 5);
    if (m >= 0 && m <= 3 && strlen(cmd) > 5) { Comando c = {CMD_MODO, m}; xQueueSend(filaCmd, &c, 0);
      snprintf(txt, sizeof(txt), "Modo alterado para %s (novo ensaio iniciado).", NOME_MODO[m]); tgEnviar(txt); }
    else tgEnviar("Use /modo 0 (continuo), /modo 1 (liga-desliga), /modo 2 (adaptativo) ou /modo 3 (demo).");
  } else if (!strncmp(cmd, "/novoensaio", 11)) {
    Comando c = {CMD_NOVO_ENSAIO, 0}; xQueueSend(filaCmd, &c, 0); tgEnviar("Novo ensaio iniciado.");
  } else if (!strncmp(cmd, "/ligar", 6)) {
    if (ligacaoAtiva()) { tgEnviar("Fazendo ligacao de teste..."); fazerLigacao("Teste do alerta do prototipo de ventilacao."); }
    else tgEnviar("Ligacao desativada (CALLMEBOT_USER vazio em segredos.h).");
  } else {
    tgEnviar("Comandos: /status  /relatorio  /modo N  /novoensaio  /ligar");
  }
}
static void tgConsultar(int64_t &offset) {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http; http.setTimeout(10000);
  char url[220];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?timeout=0&limit=5&offset=%lld",
           TG_TOKEN, (long long)offset);
  if (!http.begin(cli, url)) return;
  int cod = http.GET();
  if (cod != 200) { http.end(); return; }
  String resp = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return;
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    int64_t id = u["update_id"] | (int64_t)0;
    offset = id + 1;
    int64_t chat = u["message"]["chat"]["id"] | (int64_t)0;
    const char *texto = u["message"]["text"] | "";
    char chatStr[24]; snprintf(chatStr, sizeof(chatStr), "%lld", (long long)chat);
    if (strcmp(chatStr, TG_CHAT_ID) != 0) continue;   // só aceita o chat autorizado
    if (texto[0] == '/') tgResponder(texto);
  }
}
static void tarefaRede(void *) {
  bool ntp = false, estavaConectado = false, saudou = false; uint32_t ultPoll = 0, ultTentativa = 0; int64_t offset = 0;
  if (strlen(WIFI_SSID)) { WiFi.begin(WIFI_SSID, WIFI_SENHA); ultTentativa = millis(); }
  else Serial.println(F("# Wi-Fi: WIFI_SSID vazio -> sem internet (so painel local)"));
  for (;;) {
    bool conectado = strlen(WIFI_SSID) && WiFi.status() == WL_CONNECTED;
    if (conectado && !estavaConectado) {
      Serial.printf("# Wi-Fi: CONECTADO a '%s' | IP %s | sinal %d dBm\n", WIFI_SSID, WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (tgAtivo() && !saudou) {
        char m[160]; snprintf(m, sizeof(m), "Prototipo conectado (IP %s). Aquecendo o MQ-2; o controle comeca em ~4 min. Envie /status.", WiFi.localIP().toString().c_str());
        saudou = tgEnviar(m);
        Serial.println(saudou ? F("# TELEGRAM: mensagem de teste ENVIADA") : F("# TELEGRAM: mensagem de teste FALHOU (veja o erro acima)"));
      }
    }
    if (!conectado && estavaConectado) Serial.println(F("# Wi-Fi: conexao perdida"));
    estavaConectado = conectado;
    if (strlen(WIFI_SSID) && !conectado && millis() - ultTentativa > 20000) {
      Serial.printf("# Wi-Fi: sem conexao com '%s' (status %d). A rede precisa ser 2,4 GHz; confira nome e senha.\n", WIFI_SSID, (int)WiFi.status());
      WiFi.reconnect(); ultTentativa = millis();
    }
    if (conectado && !ntp) { configTzTime("<-03>3", "a.st1.ntp.br", "pool.ntp.org"); ntp = true; }
    MsgSaida m;
    if (conectado && xQueueReceive(filaSaida, &m, 0) == pdTRUE) {
      bool ok = (m.tipo == SAI_MSG) ? tgEnviar(m.texto) : fazerLigacao(m.texto);
      Serial.printf("# REDE: %s %s\n", m.tipo == SAI_MSG ? "mensagem" : "ligacao", ok ? "enviada" : "FALHOU");
    }
    if (conectado && tgAtivo() && millis() - ultPoll > TG_POLL_MS) { ultPoll = millis(); tgConsultar(offset); }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================ BOTÃO E SERIAL ===============================
static void servicoBotao() {
  static bool anterior = true; static uint32_t tPress = 0;
  bool solto = digitalRead(PIN_BOTAO);
  if (anterior && !solto) tPress = millis();
  if (!anterior && solto) {
    uint32_t d = millis() - tPress;
    if (d > 2000) { novoEnsaio(); bipsAgendar(2); }
    else if (d > 50) {
      if (estado == EST_AQUEC) { Serial.println(F("# aquecimento pulado pelo usuario")); tInicioEstado = millis() - T_AQUEC_S * 1000UL; }
      else definirModo((modo + 1) % 4);
    }
  }
  anterior = solto;
}
static void servicoSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'm') { delay(20); int m = Serial.read() - '0'; definirModo(m); }
  else if (c == 'r') novoEnsaio();
  else if (c == 'p') { static char t[900]; textoMetricas(t, sizeof(t), ensaio, "Ensaio atual"); Serial.println(t);
                       textoEstrategias(t, sizeof(t), ensS); Serial.println(t); }
  else if (c == 'k' && estado == EST_AQUEC) tInicioEstado = millis() - T_AQUEC_S * 1000UL;
  else if (c == 'h') Serial.println(F("# comandos: m0..m3 (modo), r (novo ensaio), p (metricas), k (pular aquecimento)"));
}

// ============================ SETUP / LOOP =================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT); pinMode(PIN_BOTAO, INPUT_PULLUP);
  pwmIniciar(); fanEscrever(0);
  dht.begin();
  Wire.begin(21, 22);
  delay(300);

  // ---------------- AUTOTESTE DE HARDWARE (resultado no Monitor Serial) ----------------
  Serial.println(F("\n# ================= AUTOTESTE ================="));
  uint8_t nI2C = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("# I2C: dispositivo encontrado em 0x%02X\n", a); nI2C++; }
  }
  if (!nI2C) Serial.println(F("# I2C: NENHUM dispositivo. Confira no OLED: GND->GND, VDD->3V3, SCK->GPIO22, SDA->GPIO21"));
  telaOk = tela.begin(SSD1306_SWITCHCAPVCC, 0x3C) || tela.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  Serial.printf("# OLED: %s\n", telaOk ? "OK" : "NAO respondeu (sinal: 2 bipes longos)");
  if (telaOk) {
    tela.clearDisplay(); tela.setTextSize(1); tela.setTextColor(SSD1306_WHITE);
    tela.setCursor(0, 0); tela.println(F("AUTOTESTE...")); tela.display();
  }
  delay(1500);   // o DHT11 precisa de ~1 s após energizar
  float tT = dht.readTemperature(), uT = dht.readHumidity();
  bool dhtOk = !isnan(tT) && !isnan(uT);
  if (dhtOk) Serial.printf("# DHT11: OK (T = %.1f C, UR = %.0f %%)\n", tT, uT);
  else Serial.println(F("# DHT11: SEM LEITURA (sinal: 3 bipes curtos). Confira: + ->3V3, - ->GND, OUT ->GPIO4"));
  uint32_t mvT = lerMQ2mV();
  Serial.printf("# MQ-2: %lu mV no GPIO34 %s\n", (unsigned long)mvT,
                mvT < 30 ? "(quase zero: confira AO->GPIO34 e VCC/GND)" : "(OK; aquece nos proximos minutos)");
  Serial.println(F("# Ventilador: fica DESLIGADO durante aquecimento (180 s) e linha de base (60 s)."));
  if (tgAtivo()) {
    bool idOk = true;
    const char *id = TG_CHAT_ID;
    for (const char *p = id; *p; p++) if (!(isdigit((unsigned char)*p) || (*p == '-' && p == id))) idOk = false;
    Serial.println(idOk ? F("# Telegram: TG_CHAT_ID com formato valido")
                        : F("# Telegram: TG_CHAT_ID INVALIDO. Nao e o telefone: e um numero como 123456789 (veja LEIA-ME)"));
  } else Serial.println(F("# Telegram: desativado (TG_TOKEN ou TG_CHAT_ID vazio)"));
  Serial.println(F("# ============================================="));
  if (!telaOk) { for (int i = 0; i < 2; i++) { buzzerTom(700); delay(700); buzzerTom(0); delay(250); } }
  if (!dhtOk) { for (int i = 0; i < 3; i++) { buzzerTom(2200); delay(120); buzzerTom(0); delay(150); } }
  if (telaOk) {
    tela.clearDisplay(); tela.setCursor(0, 0);
    tela.println(F("AUTOTESTE"));
    tela.printf("DHT11: %s\n", dhtOk ? "OK" : "FALHOU");
    tela.printf("MQ-2: %lu mV\n", (unsigned long)mvT);
    tela.println(F("\nAquecendo MQ-2..."));
    tela.display(); delay(2500);
  }
  mtx = xSemaphoreCreateMutex();
  filaSaida = xQueueCreate(6, sizeof(MsgSaida));
  filaCmd = xQueueCreate(4, sizeof(Comando));
  zerarEnsaio(); zerarHora();

  WiFi.mode(strlen(WIFI_SSID) ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_SENHA);
  servidor.on("/", []() { servidor.send_P(200, "text/html", PAGINA); });
  servidor.on("/api", webApi);
  servidor.on("/csv", webCsv);
  servidor.on("/modo", []() { definirModo(servidor.arg("m").toInt()); servidor.send(200, "text/plain", "ok"); });
  servidor.on("/novo", []() { novoEnsaio(); servidor.send(200, "text/plain", "ok"); });
  servidor.begin();
  xTaskCreatePinnedToCore(tarefaRede, "rede", 16384, nullptr, 1, nullptr, 0);

  Serial.println(F("# Ventilacao adaptativa - ESP32 | DHT11 | MQ-2"));
  Serial.printf("# Painel local: rede %s -> http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  imprimirCabecalhoCSV();
  iniciarEstado(EST_AQUEC);
  bipsAgendar(1);
}

void loop() {
  uint32_t agora = millis();
  if (agora - ultimaAmostra >= T_AMOSTRA_MS || ultimaAmostra == 0) { amostrar(); desenharTela(); }
  else if (estado != EST_OPER) { static uint32_t u = 0; if (agora - u > 1000) { u = agora; desenharTela(); } }
  Comando c;
  while (xQueueReceive(filaCmd, &c, 0) == pdTRUE) {
    if (c.tipo == CMD_MODO) definirModo(c.arg); else novoEnsaio();
  }
  servicoVentilador();
  servicoBotao();
  servicoSerial();
  bipServico();
  servidor.handleClient();
  delay(2);
}