/*
 * ============================================================================
 *  BoviSense — monitoramento ambiental e ventilação adaptativa para bovinos
 *  ESP32 DevKit 30 pinos | DHT11 | MQ-2 | buzzer | TIP122
 *
 *  - Painel web BoviSense (rede "BoviSense", http://192.168.4.1), com duas
 *    vistas: OPERAÇÃO (só o que o usuário final precisa ver) e TÉCNICO
 *    (perfil de teste: estratégias, ensaios, métricas, calibração e rede)
 *  - Telegram: mensagem ao ligar, relatório de hora em hora, alertas,
 *    comandos pelo menu "/" e simulação de emergência com ligação de voz
 *  - Calibração: offsets do DHT11 contra referência (gravados na memória),
 *    ADC com média aparada, linha de base do MQ-2 com correção lenta de deriva
 *
 *  Sem display: a interface local é o painel web. O diagnóstico de
 *  inicialização sai pelo Monitor Serial e por códigos de bipe no buzzer.
 *
 *  Compatível com Arduino-ESP32 core 2.0.x e 3.x. Bibliotecas:
 *    DHT sensor library, Adafruit Unified Sensor, ArduinoJson (v7).
 *
 *  VALIDADE CIENTÍFICA: o MQ-2 não mede NH3 e nada aqui é convertido em ppm;
 *  IG = R0/Rs é um índice relativo. ITU: Kelly e Bond (1971) apud Azevedo
 *  et al. (2005). Limiares 72/75/79: Armstrong (1994) apud Azevedo et al.
 *  (2005) e valores críticos de Azevedo et al. (2005). Eventos simulados são
 *  marcados no registro e NÃO entram nas métricas dos ensaios.
 * ============================================================================
 */
#include <Arduino.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <time.h>
#include "segredos.h"

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#define CORE3 1
#else
#define CORE3 0
#endif

// ============================ 0. HARDWARE ==================================
#define SEM_ATUADOR   0   // 1 = sem TIP122: só recomenda o nível (modo sombra)
#define MQ2_EM_3V3    1   // 1 = MQ-2 no 3V3, AO direto; 0 = MQ-2 no VIN com divisor 10k/20k

// ============================ 1. PINOS =====================================
// Na placa, o número depois do "D" é o número do GPIO (D4 = GPIO4).
#define PIN_DHT      4    // D4  -> OUT do DHT11
#define PIN_MQ2      34   // D34 -> AO do MQ-2 (entrada analógica ADC1)
#define PIN_FAN      26   // D26 -> resistor 1 kΩ -> base do TIP122
#define PIN_BUZZER   25   // D25 -> + do buzzer
#define PIN_BOTAO    0    // botão BOOT da placa
#define PIN_LED      2    // LED azul da placa

// ============================ 2. SENSORES ==================================
#define DHT_TIPO        DHT11
#define OFFSET_T_C      0.0f   // offsets fixos (além da calibração salva pelo painel)
#define OFFSET_UR_PCT   0.0f
#if MQ2_EM_3V3
#define MQ2_R1_OHM 0.0f
#define MQ2_R2_OHM 1.0f
#define MQ2_VC_MV  3300.0f
#else
#define MQ2_R1_OHM 10000.0f
#define MQ2_R2_OHM 20000.0f
#define MQ2_VC_MV  5000.0f
#endif
#define ADC_N           32       // leituras por amostra (média aparada)
#define SALTO_T_MAX     3.0f     // rejeição de pico do DHT11 (°C entre amostras)
#define SALTO_U_MAX     10.0f    // (% UR entre amostras)
#define TAU_DERIVA_S    1800.0f  // correção lenta da linha de base do MQ-2

// ============================ 3. TEMPOS ====================================
#define T_AMOSTRA_MS       2500UL
#define N_MEDIA            4
#define T_AQUEC_S          180UL
#define T_BASE_S           60UL
#define T_DESCIDA_MS       60000UL
#define T_PERMANENCIA_MS   60000UL
#define T_PARTIDA_MS       1500UL
#define FALHAS_FAILSAFE    3
#define T_SIM_MS           60000UL   // duração da emergência simulada
#define T_BOTAO_SIM_MS     3000UL    // segurar BOOT 3 s = simular emergência

// ============================ 4. CONTROLE ==================================
static float ITU_LIMIAR[3] = {72.0f, 75.0f, 79.0f};
#define ITU_HISTERESE   2.0f     // DHT11 (1 °C de resolução ≈ 1,4 ITU); DHT22: 1.0f
static const uint8_t DUTY_NIVEL[4] = {0, 50, 75, 100};
static float IG_LIMIAR[2] = {1.5f, 2.5f};
#define IG_HISTERESE    0.2f
#define FAN_PWM_HZ      1000
#define FAN_PWM_BITS    10
#define FAN_INVERTIDO   0
#define FAN_P_NOMINAL_W 2.4f     // substituir pela potência medida (Ensaio 0)
static float P_NIVEL_W[4] = {0.0f, 0.50f * FAN_P_NOMINAL_W, 0.75f * FAN_P_NOMINAL_W, FAN_P_NOMINAL_W};

// ============================ 5. ALERTAS ===================================
#define ALERTA_ITU_S     120
#define LIGACAO_ITU_S    600
#define COOLDOWN_MSG_S   600
#define COOLDOWN_LIG_S   1800
#define LEMBRETE_BIP_S   30
#define TG_POLL_MS       4000
#define INTERVALO_LIG_SIM_MS 120000UL  // no máximo 1 ligação simulada a cada 2 min

// ============================ TIPOS ========================================
enum Modo : uint8_t { MODO_CONTINUO = 0, MODO_LIGA_DESLIGA, MODO_ADAPTATIVO, MODO_DEMO };
static const char *NOME_MODO[] = {"CONTINUO", "LIGA-DESLIGA", "ADAPTATIVO", "DEMO"};
enum Estado : uint8_t { EST_AQUEC = 0, EST_BASE, EST_OPER };

struct Estagiador { const float *limiar; uint8_t n; float hist; uint8_t estagio; uint32_t ultimaTroca; bool abaixo; uint32_t abaixoDesde; };
struct Metricas {
  uint32_t inicio_ms;
  double t_total_s, t_ligado_s, t_eq_s, t_itu_acima_s, carga_itu_min, energia_wh, t_nivel_s[4], soma_itu;
  uint32_t n_partidas, n_trocas, n_itu;
  float itu_max, itu_min, ig_max;
};
struct Registro { uint32_t t; int16_t T10, U10, ITU10; uint16_t IG100, mv; uint8_t nivel, duty, modo; uint16_t flags; };
#define N_REG 1500
static Registro regs[N_REG];
static uint16_t regTopo = 0, regQtd = 0;

#define F_DHT_FALHA   0x001
#define F_FAILSAFE    0x002
#define F_MQ_FALHA    0x004
#define F_PREPARO     0x008
#define F_ALARME_GAS  0x010
#define F_PARTIDA     0x020
#define F_UR_FAIXA    0x040
#define F_HORA_NTP    0x080
#define F_SIMULACAO   0x100
#define F_RECAL_GAS   0x200

struct Retrato { float T, U, itu, ig; uint8_t nivel, duty, modo, estado; uint16_t flags; bool sim; Metricas ensaio, hora, ensS[3], horS[3]; };
enum TipoSaida : uint8_t { SAI_MSG = 0, SAI_LIGACAO };
struct MsgSaida { uint8_t tipo; char texto[900]; };
enum TipoCmd : uint8_t { CMD_MODO = 0, CMD_NOVO_ENSAIO, CMD_SIMULAR, CMD_PARAR_SIM, CMD_RECAL_GAS };
struct Comando { uint8_t tipo; int arg; };

// ============================ OBJETOS GLOBAIS ==============================
DHT dht(PIN_DHT, DHT_TIPO);
WebServer servidor(80);
Preferences prefs;
SemaphoreHandle_t mtx;
QueueHandle_t filaSaida, filaCmd;

static String wifiSsid, wifiSenha, tgToken, tgChat, cbUser;   // gravadas na memória e editáveis pelo painel
static volatile bool pedidoReconectar = false;
static bool wifiOk = false;
static volatile uint8_t wifiMotivo = 0;   // último motivo de desconexão informado pelo driver
static char wifiDiag[40] = "-";
static Modo modo = MODO_ADAPTATIVO;
static Estado estado = EST_AQUEC;
static uint32_t tInicioEstado = 0, ultimaAmostra = 0;

static float bufT[N_MEDIA], bufU[N_MEDIA], bufR[N_MEDIA];
static uint8_t idxTU = 0, nTU = 0, idxR = 0, nR = 0, rejeicoes = 0;
static uint8_t falhasDHT = 0;
static float Traw = NAN, Uraw = NAN, Tf = NAN, Uf = NAN, itu = NAN, ig = NAN, tLongo = NAN, uLongo = NAN;
static uint32_t mqmV = 0;
static float R0rel = NAN, cvBase = 0, ituBase = NAN, calOffT = 0, calOffU = 0;
static double somaR = 0, somaR2 = 0, somaItuBase = 0; static uint32_t nBaseR = 0, nBaseItu = 0;
static uint32_t recalGasAte = 0;
static float ituDemo[3] = {0, 0, 0};

static Estagiador stItu = {ITU_LIMIAR, 3, ITU_HISTERESE, 0, 0, false, 0};
static Estagiador stGas = {IG_LIMIAR, 2, IG_HISTERESE, 0, 0, false, 0};

static uint8_t nivel = 0, dutyAlvo = 0, dutyAnterior = 0;
static uint16_t flags = 0;
static uint32_t partidaAte = 0;
static Metricas ensaio, hora, ensS[3], horS[3];
static uint8_t dutyPrevS[3] = {0, 0, 0};
static Retrato retrato;

static uint32_t ituCritDesde = 0; static bool ituCritAtivo = false, ituAvisou = false;
static bool gasAlarme = false, failsafeAtivo = false;
static uint32_t ultMsg[3] = {0, 0, 0}, ultLig[3] = {0, 0, 0};
static uint32_t ultLembrete = 0;
static int ultimaHoraRelatorio = -1; static uint32_t ultRelatorioMs = 0;
static uint32_t simAte = 0, ultLigSim = 0; static bool simAnterior = false;
static uint32_t bipFim = 0, bipProx = 0; static uint8_t bipsPend = 0;

// ============================ PWM (LEDC) ===================================
#define CANAL_FAN 0
#define CANAL_BUZ 2
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
  if (SEM_ATUADOR) pct = 0;
  uint32_t maxv = (1UL << FAN_PWM_BITS) - 1, d = (uint32_t)pct * maxv / 100UL;
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
static void bipsAgendar(uint8_t n) { if (n > bipsPend) bipsPend = n; bipProx = millis(); }
static void bipServico() {
  uint32_t agora = millis();
  if (bipFim && (int32_t)(agora - bipFim) >= 0) { buzzerTom(0); bipFim = 0; }
  if (!bipFim && bipsPend && (int32_t)(agora - bipProx) >= 0) { buzzerTom(2200); bipFim = agora + 150; bipsPend--; bipProx = agora + 350; }
}

// ============================ CÁLCULOS =====================================
static float calcITU(float T, float UR) { return (1.8f * T + 32.0f) - (0.55f - 0.0055f * UR) * (1.8f * T - 26.0f); }
static float media(const float *b, uint8_t n) { float s = 0; for (uint8_t i = 0; i < n; i++) s += b[i]; return n ? s / n : NAN; }
// Média aparada: ordena ADC_N leituras e usa a metade central (remove picos do Wi-Fi)
static uint32_t lerMQ2mV() {
  uint16_t v[ADC_N];
  for (int i = 0; i < ADC_N; i++) { v[i] = analogReadMilliVolts(PIN_MQ2); delayMicroseconds(200); }
  for (int i = 1; i < ADC_N; i++) { uint16_t x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
  uint32_t s = 0; for (int i = ADC_N / 4; i < 3 * ADC_N / 4; i++) s += v[i];
  return s / (ADC_N / 2);
}
static bool horaValida() { return time(nullptr) > 1700000000; }
static uint32_t carimbo() { return horaValida() ? (uint32_t)time(nullptr) : millis() / 1000UL; }
static bool simulando() { return simAte && (int32_t)(simAte - millis()) > 0; }
static const float *limiaresAtivos() { return (modo == MODO_DEMO) ? ituDemo : ITU_LIMIAR; }
static uint8_t categoria(float x) {
  if (isnan(x)) return 255;
  const float *l = limiaresAtivos(); uint8_t c = 0;
  for (int k = 0; k < 3; k++) if (x >= l[k]) c = k + 1;
  return c;
}
static const char *NOME_CAT[4] = {"Conforto termico", "Estresse brando", "Atencao: quase critico", "Estresse moderado"};

static uint8_t estagiar(Estagiador &e, float x, uint32_t agora) {
  uint8_t alvo = 0;
  for (uint8_t k = 0; k < e.n; k++) if (x >= e.limiar[k]) alvo = k + 1;
  if (alvo > e.estagio) { e.estagio = alvo; e.ultimaTroca = agora; e.abaixo = false; return e.estagio; }
  if (e.estagio > 0 && x < e.limiar[e.estagio - 1] - e.hist) {
    if (!e.abaixo) { e.abaixo = true; e.abaixoDesde = agora; }
    if (agora - e.abaixoDesde >= T_DESCIDA_MS && agora - e.ultimaTroca >= T_PERMANENCIA_MS) { e.estagio--; e.ultimaTroca = agora; e.abaixo = false; }
  } else e.abaixo = false;
  return e.estagio;
}
static void zerarEstagiador(Estagiador &e) { e.estagio = 0; e.ultimaTroca = millis(); e.abaixo = false; }
static void zerarMetricas(Metricas &m) { memset(&m, 0, sizeof(m)); m.inicio_ms = millis(); m.itu_max = -1000; m.itu_min = 1000; }
static void zerarEnsaio() { zerarMetricas(ensaio); for (int i = 0; i < 3; i++) { zerarMetricas(ensS[i]); dutyPrevS[i] = 0; } }
static void zerarHora() { zerarMetricas(hora); for (int i = 0; i < 3; i++) zerarMetricas(horS[i]); }
static void acumular(Metricas &m, float dt, uint8_t niv, uint8_t duty, bool partida, bool troca) {
  m.t_total_s += dt; m.t_nivel_s[niv] += dt;
  if (duty > 0) m.t_ligado_s += dt;
  m.t_eq_s += dt * duty / 100.0;
  m.energia_wh += P_NIVEL_W[niv] * dt / 3600.0;
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

// ============================ CALIBRAÇÃO (memória permanente) ==============
static void carregarCalibracao() {
  prefs.begin("bovisense", true);
  calOffT = prefs.getFloat("offT", 0.0f); calOffU = prefs.getFloat("offU", 0.0f);
  wifiSsid = prefs.getString("wifi_s", WIFI_SSID); wifiSenha = prefs.getString("wifi_p", WIFI_SENHA);
  tgToken = prefs.getString("tg_t", TG_TOKEN); tgChat = prefs.getString("tg_c", TG_CHAT_ID);
  cbUser = prefs.getString("cb_u", CALLMEBOT_USER);
  prefs.end();
}
// A rede escolhida no painel fica gravada; o segredos.h serve só de valor inicial
static void salvarRede(const String &ssid, const String &senha) {
  wifiSsid = ssid; wifiSenha = senha;
  prefs.begin("bovisense", false); prefs.putString("wifi_s", ssid); prefs.putString("wifi_p", senha); prefs.end();
  pedidoReconectar = true;
}
static void salvarTelegram(const String &tok, const String &chat, const String &user) {
  tgToken = tok; tgChat = chat; cbUser = user;
  prefs.begin("bovisense", false); prefs.putString("tg_t", tok); prefs.putString("tg_c", chat); prefs.putString("cb_u", user); prefs.end();
}
static void salvarCalibracao() {
  prefs.begin("bovisense", false);
  prefs.putFloat("offT", calOffT); prefs.putFloat("offU", calOffU);
  prefs.end();
}
// Ajusta os offsets para que a média de 60 s do DHT11 coincida com a referência
static bool calibrarTU(float tRef, float uRef, char *msg, size_t n) {
  if (isnan(tLongo) || isnan(uLongo)) { snprintf(msg, n, "Sem leituras suficientes do DHT11."); return false; }
  if (tRef < 0 || tRef > 50 || uRef < 5 || uRef > 100) { snprintf(msg, n, "Referencia fora da faixa (T 0-50 C, UR 5-100 %%)."); return false; }
  float dT = tRef - tLongo, dU = uRef - uLongo;
  if (fabsf(calOffT + dT) > 10 || fabsf(calOffU + dU) > 25) { snprintf(msg, n, "Correcao grande demais: confira o sensor e a referencia."); return false; }
  calOffT += dT; calOffU += dU;
  for (int i = 0; i < N_MEDIA; i++) { bufT[i] += dT; bufU[i] += dU; }
  tLongo += dT; uLongo += dU; salvarCalibracao();
  snprintf(msg, n, "Calibrado: offset T %+.1f C, UR %+.1f %% (salvo na memoria).", calOffT, calOffU);
  return true;
}
static void zerarCalibracao() {
  for (int i = 0; i < N_MEDIA; i++) { bufT[i] -= calOffT; bufU[i] -= calOffU; }
  if (!isnan(tLongo)) { tLongo -= calOffT; uLongo -= calOffU; }
  calOffT = calOffU = 0; salvarCalibracao();
}
static void iniciarRecalGas() { recalGasAte = millis() + T_BASE_S * 1000UL; somaR = somaR2 = 0; nBaseR = 0; zerarEstagiador(stGas); }
static void definirLimiaresGas() {
  if (nBaseR < 5) return;
  R0rel = somaR / nBaseR;
  double var = somaR2 / nBaseR - (double)R0rel * R0rel;
  cvBase = var > 0 ? sqrt(var) / R0rel : 0;
  IG_LIMIAR[0] = max(1.5f, 1.0f + 6.0f * cvBase);
  IG_LIMIAR[1] = max(2.5f, IG_LIMIAR[0] + 0.5f);
}

// ============================ SAÍDAS DE REDE ===============================
static bool chatIdValido() {
  const char *id = tgChat.c_str(); if (!*id) return false;
  for (const char *p = id; *p; p++) if (!(isdigit((unsigned char)*p) || (*p == '-' && p == id))) return false;
  return true;
}
static bool tgTokenOk() { return tgToken.length() > 20; }
static bool tgAtivo() { return tgTokenOk() && chatIdValido(); }
static bool ligacaoAtiva() { return cbUser.length() > 1; }
static void enfileirar(uint8_t tipo, const char *txt) {
  if (tipo == SAI_MSG && !tgAtivo()) return;
  if (tipo == SAI_LIGACAO && !ligacaoAtiva()) return;
  MsgSaida m; m.tipo = tipo; strlcpy(m.texto, txt, sizeof(m.texto));
  xQueueSend(filaSaida, &m, 0);
}
static void alertar(uint8_t canal, const char *msg, bool ligar) {
  uint32_t s = millis() / 1000UL;
  if (ultMsg[canal] == 0 || s - ultMsg[canal] >= COOLDOWN_MSG_S) { enfileirar(SAI_MSG, msg); ultMsg[canal] = s ? s : 1; }
  if (ligar && (ultLig[canal] == 0 || s - ultLig[canal] >= COOLDOWN_LIG_S)) { enfileirar(SAI_LIGACAO, msg); ultLig[canal] = s ? s : 1; }
}
static void textoMetricas(char *out, size_t n, const Metricas &m, const char *titulo) {
  float pctAcima = m.t_total_s > 0 ? 100.0 * m.t_itu_acima_s / m.t_total_s : 0;
  snprintf(out, n,
           "%s\nModo: %s | duracao: %.0f min\nITU medio %.1f (min %.1f / max %.1f)\n"
           "Tempo com ITU >= %.0f: %.0f %%\nCarga termica acima do limiar: %.1f ITU.min\n"
           "Ventilador ligado: %.1f min (plena carga eq.: %.1f min)\n"
           "Energia (estimada/calibrada): %.2f Wh | partidas: %lu | trocas: %lu\n"
           "IG maximo: %.2f (indice relativo, nao e ppm)",
           titulo, NOME_MODO[modo], m.t_total_s / 60.0, m.n_itu ? m.soma_itu / m.n_itu : NAN,
           m.n_itu ? m.itu_min : NAN, m.n_itu ? m.itu_max : NAN, ITU_LIMIAR[0], pctAcima, m.carga_itu_min,
           m.t_ligado_s / 60.0, m.t_eq_s / 60.0, m.energia_wh, (unsigned long)m.n_partidas, (unsigned long)m.n_trocas, m.ig_max);
}
static void textoEstrategias(char *out, size_t n, const Metricas *m) {
  double tot = m[0].t_total_s > 0 ? m[0].t_total_s : 1;
  snprintf(out, n,
           "Comparacao das estrategias (mesmos dados):\nE1 continua: %.1f min eq. (%.0f %%)\n"
           "E2 liga-desliga: %.1f min eq. (%.0f %%), trocas %lu\nE3 adaptativa: %.1f min eq. (%.0f %%), trocas %lu\n"
           "Reducao estimada E3 x E1: %.0f %%",
           m[0].t_eq_s / 60.0, 100.0 * m[0].t_eq_s / tot, m[1].t_eq_s / 60.0, 100.0 * m[1].t_eq_s / tot,
           (unsigned long)m[1].n_trocas, m[2].t_eq_s / 60.0, 100.0 * m[2].t_eq_s / tot, (unsigned long)m[2].n_trocas,
           m[0].t_eq_s > 0 ? 100.0 * (m[0].t_eq_s - m[2].t_eq_s) / m[0].t_eq_s : 0.0);
}

// ============================ SIMULAÇÃO DE EMERGÊNCIA ======================
static void iniciarSimulacao(const char *origem) {
  simAte = millis() + T_SIM_MS; if (!simAte) simAte = 1;
  bipsAgendar(6);
  char m[320];
  snprintf(m, sizeof(m), "[SIMULACAO] Emergencia no BoviSense (acionada por %s): ITU critico e alerta de gases "
           "simulados por %lu s. Ventilacao forcada em 100%%. Dados reais nao foram alterados.", origem, T_SIM_MS / 1000UL);
  enfileirar(SAI_MSG, m);
  if (ultLigSim == 0 || millis() - ultLigSim > INTERVALO_LIG_SIM_MS) {
    enfileirar(SAI_LIGACAO, "Atencao. Emergencia simulada no BoviSense. Temperatura critica e gases detectados. Ventilacao em cem por cento.");
    ultLigSim = millis() ? millis() : 1;
  }
  Serial.printf("# SIMULACAO iniciada por %s\n", origem);
}
static void pararSimulacao() { if (simAte) { simAte = 0; } }

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

  // ---- DHT11 com rejeição de picos ----
  float t = dht.readTemperature(), u = dht.readHumidity();
  bool ok = !isnan(t) && !isnan(u) && t > -5 && t < 60 && u > 0 && u <= 100;
  if (ok) {
    float tc = t + OFFSET_T_C + calOffT, uc = constrain(u + OFFSET_UR_PCT + calOffU, 0.0f, 100.0f);
    bool salto = nTU >= N_MEDIA && (fabsf(tc - Tf) > SALTO_T_MAX || fabsf(uc - Uf) > SALTO_U_MAX);
    if (salto && rejeicoes < 2) { rejeicoes++; flags |= F_DHT_FALHA; }
    else {
      if (salto) { nTU = 0; idxTU = 0; }   // mudança real e persistente: reinicia a média
      rejeicoes = 0; Traw = tc; Uraw = uc;
      bufT[idxTU] = tc; bufU[idxTU] = uc; idxTU = (idxTU + 1) % N_MEDIA; if (nTU < N_MEDIA) nTU++;
      falhasDHT = 0;
      if (uc < 20 || uc > 90 || tc > 50) flags |= F_UR_FAIXA;
    }
  } else { if (falhasDHT < 255) falhasDHT++; flags |= F_DHT_FALHA; }
  bool failsafe = falhasDHT >= FALHAS_FAILSAFE;
  if (failsafe) flags |= F_FAILSAFE;
  Tf = nTU ? media(bufT, nTU) : NAN; Uf = nTU ? media(bufU, nTU) : NAN;
  itu = (nTU && !failsafe) ? calcITU(Tf, Uf) : NAN;
  if (!isnan(Tf) && dt > 0) {   // médias de 60 s usadas na calibração
    float a = fminf(1.0f, dt / 60.0f);
    tLongo = isnan(tLongo) ? Tf : tLongo + a * (Tf - tLongo);
    uLongo = isnan(uLongo) ? Uf : uLongo + a * (Uf - uLongo);
  }

  // ---- MQ-2: Rs/RL = (Vc - Vao)/Vao ; IG = R0/Rs (RL se cancela) ----
  mqmV = lerMQ2mV();
  float vao = mqmV * (MQ2_R1_OHM + MQ2_R2_OHM) / MQ2_R2_OHM;
  bool mqOk = vao > 30.0f && vao < (MQ2_VC_MV - 30.0f) && mqmV < 3100;
  float rInst = mqOk ? (MQ2_VC_MV - vao) / vao : NAN;
  if (mqOk) { bufR[idxR] = rInst; idxR = (idxR + 1) % N_MEDIA; if (nR < N_MEDIA) nR++; } else flags |= F_MQ_FALHA;
  float rsRel = nR ? media(bufR, nR) : NAN;
  bool recal = recalGasAte && (int32_t)(recalGasAte - agora) > 0;
  if (recal) { flags |= F_RECAL_GAS; if (mqOk) { somaR += rInst; somaR2 += (double)rInst * rInst; nBaseR++; } }
  else if (recalGasAte) { recalGasAte = 0; definirLimiaresGas(); Serial.printf("# MQ-2 recalibrado: cv=%.3f G1=%.2f G2=%.2f\n", cvBase, IG_LIMIAR[0], IG_LIMIAR[1]); }
  ig = (!recal && !isnan(R0rel) && mqOk && rsRel > 0) ? R0rel / rsRel : NAN;

  uint8_t estItu = stItu.estagio, estGas = stGas.estagio;
  bool sim = simulando();
  if (sim) flags |= F_SIMULACAO;

  if (estado != EST_OPER) {
    flags |= F_PREPARO;
    if (estado == EST_AQUEC && agora - tInicioEstado >= T_AQUEC_S * 1000UL) {
      iniciarEstado(EST_BASE); somaR = somaR2 = somaItuBase = 0; nBaseR = nBaseItu = 0;
    } else if (estado == EST_BASE) {
      if (mqOk) { somaR += rInst; somaR2 += (double)rInst * rInst; nBaseR++; }
      if (!isnan(itu)) { somaItuBase += itu; nBaseItu++; }
      if (agora - tInicioEstado >= T_BASE_S * 1000UL) {
        definirLimiaresGas();
        ituBase = nBaseItu ? somaItuBase / nBaseItu : NAN;
        for (int k = 0; k < 3; k++) ituDemo[k] = isnan(ituBase) ? ITU_LIMIAR[k] : ituBase + 1.0f + k;
        stItu.limiar = limiaresAtivos();
        zerarEstagiador(stItu); zerarEstagiador(stGas); zerarEnsaio(); zerarHora();
        iniciarEstado(EST_OPER);
        Serial.printf("# BASE: R0/RL=%.3f cv=%.3f G1=%.2f G2=%.2f ITU_base=%.1f\n", R0rel, cvBase, IG_LIMIAR[0], IG_LIMIAR[1], ituBase);
        char msg[220];
        snprintf(msg, sizeof(msg), "BoviSense pronto. Controle %s ativo. ITU agora %.1f (%s).", NOME_MODO[modo], itu,
                 categoria(itu) < 4 ? NOME_CAT[categoria(itu)] : "sem leitura");
        enfileirar(SAI_MSG, msg);
      }
    }
    nivel = (failsafe || sim) ? 3 : 0;
  } else {
    // ---- as três estratégias são calculadas a cada amostra ----
    uint8_t nivS[3];
    nivS[0] = 3;
    nivS[1] = ((!isnan(itu) && itu >= ITU_LIMIAR[0]) || (!isnan(ig) && ig >= IG_LIMIAR[0])) ? 3 : 0;
    if (!isnan(itu)) estItu = estagiar(stItu, itu, agora);
    estGas = !isnan(ig) ? estagiar(stGas, ig, agora) : 0;
    uint8_t nGas = estGas == 0 ? 0 : (estGas == 1 ? 2 : 3);
    nivS[2] = max(estItu, nGas);
    if (failsafe) nivS[1] = nivS[2] = 3;
    nivel = (modo == MODO_CONTINUO) ? nivS[0] : (modo == MODO_LIGA_DESLIGA) ? nivS[1] : nivS[2];
    if (sim) nivel = 3;
    if (!sim && dt > 0 && dt < 30) {   // eventos simulados não entram nas métricas
      for (int i = 0; i < 3; i++) {
        uint8_t dS = DUTY_NIVEL[nivS[i]];
        acumular(ensS[i], dt, nivS[i], dS, dS > 0 && dutyPrevS[i] == 0, dS != dutyPrevS[i]);
        acumular(horS[i], dt, nivS[i], dS, dS > 0 && dutyPrevS[i] == 0, dS != dutyPrevS[i]);
        dutyPrevS[i] = dS;
      }
    }
    // correção lenta da deriva da linha de base do MQ-2 (só em ar "limpo")
    if (!isnan(ig) && fabsf(ig - 1.0f) < 0.10f && estGas == 0 && !sim && dt > 0)
      R0rel += (rsRel - R0rel) * (dt / TAU_DERIVA_S);
  }

  // ---- atuação ----
  uint8_t novoDuty = DUTY_NIVEL[nivel];
  bool partida = (novoDuty > 0 && dutyAnterior == 0), troca = (novoDuty != dutyAnterior);
  if (partida) partidaAte = agora + T_PARTIDA_MS;
  if (SEM_ATUADOR && estado == EST_OPER && novoDuty > dutyAnterior && !sim) bipsAgendar(1);
  dutyAlvo = novoDuty; dutyAnterior = novoDuty;
  if ((int32_t)(partidaAte - agora) > 0) flags |= F_PARTIDA;
  if (horaValida()) flags |= F_HORA_NTP;
  bool alarmeGasAgora = (estado == EST_OPER && modo >= MODO_ADAPTATIVO && estGas >= 2);
  if (alarmeGasAgora) flags |= F_ALARME_GAS;
  if (estado == EST_OPER && !sim && dt > 0 && dt < 30) {
    acumular(ensaio, dt, nivel, novoDuty, partida, troca);
    acumular(hora, dt, nivel, novoDuty, partida, troca);
  }

  // ---- alertas reais ----
  char msg[260];
  if (failsafe && !failsafeAtivo) {
    bipsAgendar(3);
    alertar(2, "FALHA no sensor de temperatura/umidade. Ventilacao forcada em 100% (modo seguro).", true);
  } else if (!failsafe && failsafeAtivo) enfileirar(SAI_MSG, "Sensor de temperatura/umidade voltou a responder.");
  failsafeAtivo = failsafe;
  if (alarmeGasAgora && !gasAlarme) {
    bipsAgendar(3);
    snprintf(msg, sizeof(msg), "ALERTA de gases: IG = %.2f (limiar %.2f). Ventilacao em 100%%.", ig, IG_LIMIAR[1]);
    alertar(1, msg, true);
  } else if (!alarmeGasAgora && gasAlarme) enfileirar(SAI_MSG, "Indice relativo de gases voltou ao normal.");
  gasAlarme = alarmeGasAgora;
  const float *lim = limiaresAtivos();
  if (estado == EST_OPER && !isnan(itu) && itu >= lim[2]) {
    if (!ituCritAtivo) { ituCritAtivo = true; ituCritDesde = agora; }
    uint32_t dur = (agora - ituCritDesde) / 1000UL;
    if (dur >= ALERTA_ITU_S && !ituAvisou) {
      snprintf(msg, sizeof(msg), "ALERTA termico: ITU %.1f (>= %.0f) ha %lu min. Ventilacao em %u%%.", itu, lim[2], (unsigned long)(dur / 60), novoDuty);
      alertar(0, msg, false); ituAvisou = true; bipsAgendar(2);
    }
    if (dur >= LIGACAO_ITU_S) { snprintf(msg, sizeof(msg), "Alerta termico persistente. ITU %.0f ha mais de %lu minutos.", itu, (unsigned long)(dur / 60)); alertar(0, msg, true); }
  } else if (!isnan(itu) && itu < lim[2] - ITU_HISTERESE) {
    if (ituAvisou) enfileirar(SAI_MSG, "ITU voltou abaixo da faixa de estresse moderado.");
    ituCritAtivo = false; ituAvisou = false;
  }
  if ((failsafe || alarmeGasAgora) && millis() - ultLembrete > LEMBRETE_BIP_S * 1000UL) { bipsAgendar(1); ultLembrete = millis(); }
  if (sim) bipsAgendar(2);
  if (!sim && simAnterior) { simAte = 0; enfileirar(SAI_MSG, "[SIMULACAO] encerrada. BoviSense voltou ao controle normal."); Serial.println(F("# SIMULACAO encerrada")); }
  simAnterior = sim;

  // ---- relatório de hora em hora ----
  if (estado == EST_OPER) {
    bool enviar = false; char titulo[64];
    if (horaValida()) {
      time_t tt = time(nullptr); struct tm lt; localtime_r(&tt, &lt);
      if (ultimaHoraRelatorio < 0) ultimaHoraRelatorio = lt.tm_hour;
      else if (lt.tm_hour != ultimaHoraRelatorio) {
        ultimaHoraRelatorio = lt.tm_hour; enviar = true;
        snprintf(titulo, sizeof(titulo), "BoviSense - relatorio das %02d:00 (%02d/%02d)", lt.tm_hour, lt.tm_mday, lt.tm_mon + 1);
      }
    } else if (millis() - ultRelatorioMs >= 3600000UL && hora.t_total_s >= 3500) {
      enviar = true; snprintf(titulo, sizeof(titulo), "BoviSense - relatorio da ultima hora");
    }
    if (enviar) {
      static char txt[900]; char est[400];
      textoMetricas(txt, sizeof(txt), hora, titulo); textoEstrategias(est, sizeof(est), horS);
      strlcat(txt, "\n\n", sizeof(txt)); strlcat(txt, est, sizeof(txt));
      enfileirar(SAI_MSG, txt); ultRelatorioMs = millis(); zerarHora();
    }
  }

  // ---- registro ----
  uint32_t ts = carimbo();
  registrar(ts);
  Serial.printf("%lu,%s,%u,%.1f,%.1f,%.2f,%.2f,%.2f,%lu,%.3f,%u,%u,%u,%u,%u\n", (unsigned long)ts, NOME_MODO[modo], estado,
                Traw, Uraw, Tf, Uf, itu, (unsigned long)mqmV, ig, estItu, estGas, nivel, novoDuty, flags);
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
    retrato.T = Tf; retrato.U = Uf; retrato.itu = itu; retrato.ig = ig; retrato.nivel = nivel; retrato.duty = novoDuty;
    retrato.modo = modo; retrato.estado = estado; retrato.flags = flags; retrato.sim = sim;
    retrato.ensaio = ensaio; retrato.hora = hora;
    for (int i = 0; i < 3; i++) { retrato.ensS[i] = ensS[i]; retrato.horS[i] = horS[i]; }
    xSemaphoreGive(mtx);
  }
  digitalWrite(PIN_LED, !digitalRead(PIN_LED));
}

static void servicoVentilador() { uint32_t a = millis(); fanEscrever((dutyAlvo > 0 && (int32_t)(partidaAte - a) > 0) ? 100 : dutyAlvo); }
static void novoEnsaio() {
  static char txt[900];
  textoMetricas(txt, sizeof(txt), ensaio, "Resumo do ensaio encerrado");
  Serial.println(F("# ===== RESUMO DO ENSAIO =====")); Serial.println(txt);
  textoEstrategias(txt, sizeof(txt), ensS); Serial.println(txt);
  zerarEnsaio(); zerarEstagiador(stItu); zerarEstagiador(stGas);
  Serial.println(F("# ===== NOVO ENSAIO =====")); imprimirCabecalhoCSV();
}
static void definirModo(int m) {
  if (m < 0 || m > 3) return;
  modo = (Modo)m; stItu.limiar = limiaresAtivos();
  Serial.printf("# MODO -> %s\n", NOME_MODO[modo]);
  novoEnsaio(); bipsAgendar(m + 1);
}

// ============================ PAINEL WEB BoviSense ==========================
// Duas vistas na mesma página: OPERAÇÃO (padrão, só leitura) e TÉCNICO
// (perfil de teste: estratégias, ensaios, métricas, calibração e rede).
static const char PAGINA[] PROGMEM = R"HTML(<!doctype html><html lang="pt-br"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>BoviSense</title><style>
:root{--pg:#f7f6f2;--bg:#fff;--tx:#191d1b;--mut:#6c7470;--ln:#e3e6e1;--ac:#2f6f4a;--al:#b3261e;
--c0:#2f7d4f;--c1:#a9760c;--c2:#bd5a1c;--c3:#bb3024}
@media(prefers-color-scheme:dark){:root{--pg:#121513;--bg:#1a1e1c;--tx:#e9ede9;--mut:#949c97;--ln:#2a302c;--ac:#5aa97b;
--c0:#4f9e6e;--c1:#c9971f;--c2:#d9793a;--c3:#d4564a}}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--pg);color:var(--tx);
font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;font-variant-numeric:tabular-nums}
.sheet{max-width:620px;margin:0 auto;background:var(--bg);min-height:100vh;border-left:1px solid var(--ln);border-right:1px solid var(--ln)}
@media(max-width:640px){.sheet{border:0}}
header{display:flex;justify-content:space-between;align-items:baseline;gap:12px;
padding:14px 16px;border-bottom:2px solid var(--tx)}
.wm{font-size:17px;font-weight:700;letter-spacing:.02em}
.sts{font-size:13px;color:var(--mut);text-align:right}
.sts b{font-weight:600;color:var(--tx)}
section{padding:16px;border-bottom:1px solid var(--ln)}
.k{font-size:12px;font-weight:600;letter-spacing:.09em;text-transform:uppercase;color:var(--mut);margin:0 0 8px}
.read{display:flex;align-items:flex-end;gap:16px;flex-wrap:wrap}
.num{font-size:60px;font-weight:700;line-height:.95;letter-spacing:-.02em}
.state{border-left:4px solid var(--mut);padding-left:12px;min-width:170px;flex:1}
.state b{display:block;font-size:17px;line-height:1.3}
.state span{font-size:13px;color:var(--mut)}
dl{margin:0;display:grid;grid-template-columns:1fr auto;gap:0}
dt,dd{margin:0;padding:9px 0;border-bottom:1px solid var(--ln)}
dt{color:var(--mut)}dd{text-align:right;font-weight:600}
dd u{font-weight:400;color:var(--mut);text-decoration:none;font-size:14px;margin-left:3px}
dl>:nth-last-child(-n+2){border-bottom:0}
.steps{display:grid;grid-template-columns:repeat(4,1fr);gap:4px;margin-top:10px}
.steps div{border:1px solid var(--ln);border-radius:3px;padding:9px 0;text-align:center;font-size:14px;color:var(--mut)}
.steps div.on{background:var(--ac);border-color:var(--ac);color:#fff;font-weight:700}
.hint{font-size:13px;color:var(--mut);margin:8px 0 0}
canvas{width:100%;height:160px;display:block}
#aviso{display:none;padding:13px 16px;background:var(--al);color:#fff;font-weight:600;border-bottom:1px solid var(--ln)}
button{font:inherit;font-size:15px;padding:9px 14px;border:1px solid var(--ln);border-radius:4px;
background:var(--bg);color:var(--tx);cursor:pointer;margin:0 4px 6px 0}
button:hover{border-color:var(--mut)}
button.on{background:var(--ac);border-color:var(--ac);color:#fff;font-weight:600}
button.al{border-color:var(--al);color:var(--al);font-weight:600}
.tog{width:100%;margin:0;border:0;border-radius:0;padding:14px 16px;text-align:left;
color:var(--mut);font-size:14px;background:transparent}
.tog:hover{color:var(--tx);border:0}
input,select{font:inherit;font-size:15px;padding:8px;border:1px solid var(--ln);border-radius:4px;
background:var(--bg);color:var(--tx);max-width:100%}
input{width:104px}select{width:190px}
label{display:inline-block;font-size:13px;color:var(--mut);margin:6px 10px 6px 0}
label input,label select{display:block;margin-top:3px}
table{width:100%;border-collapse:collapse;font-size:15px}
td{padding:8px 0;border-bottom:1px solid var(--ln)}td:last-child{text-align:right;font-weight:600}
tr:last-child td{border-bottom:0}
#msg{margin:10px 0 0;font-size:14px;font-weight:600;min-height:20px}
.tec{display:none}.tec.ab{display:block}
</style></head><body><div class="sheet">

<header><div class="wm">BoviSense</div>
<div class="sts"><b id="ph">--:--</b> &nbsp; <span id="pw">rede</span> &nbsp; <span id="pt">Telegram</span></div></header>

<div id="aviso"></div>

<section>
<p class="k">Índice de temperatura e umidade</p>
<div class="read"><div class="num" id="itu">--</div>
<div class="state" id="stw"><b id="cat">Lendo sensores</b><span id="est">iniciando</span></div></div>
</section>

<section>
<dl><dt>Temperatura</dt><dd><span id="t">--</span><u>°C</u></dd>
<dt>Umidade relativa</dt><dd><span id="u">--</span><u>%</u></dd>
<dt>Gases, índice relativo</dt><dd><span id="g">--</span></dd></dl>
<p class="hint">O índice de gases compara o ar com o de referência medido na partida: 1,00 é igual à referência. Não é concentração em ppm.</p>
</section>

<section>
<p class="k" id="vl">Ventilação</p>
<div class="steps"><div id="n0">desligada</div><div id="n1">50%</div><div id="n2">75%</div><div id="n3">100%</div></div>
</section>

<section>
<p class="k">Últimos 10 minutos</p>
<canvas id="c" width="900" height="260"></canvas>
<p class="hint">Linha cheia: índice de temperatura e umidade, escala de 60 a 90. Área sombreada: ventilação. Tracejados: os três limiares.</p>
</section>

<button class="tog" id="tg" onclick="tec()">Modo técnico &#9662;</button>

<div class="tec" id="box">
<section>
<p class="k">Estratégia de controle</p><div id="modos"></div>
<p class="hint">Trocar a estratégia inicia um ensaio novo. O modo demonstração usa limiares relativos ao ambiente e não serve para coletar dados.</p>
</section>

<section>
<p class="k">Ensaio</p>
<button onclick="acao('/novo')">Iniciar ensaio</button><a href="/csv"><button>Baixar CSV</button></a>
<button class="al" onclick="acao('/simular')">Simular emergência</button><button onclick="acao('/parar')">Parar simulação</button>
<p class="hint">A simulação dura 60 s, dispara Telegram e ligação, e suas amostras ficam marcadas e fora das métricas.</p>
<table id="tab"></table>
</section>

<section>
<p class="k">Calibração</p>
<p class="hint" style="margin-top:0">Deixe o DHT11 cinco minutos ao lado de um termo-higrômetro e informe os valores da referência.</p>
<label>Temperatura de referência (°C)<input id="tr" type="number" step="0.1"></label>
<label>Umidade de referência (%)<input id="ur" type="number" step="1"></label><br>
<button class="on" onclick="cal()">Salvar calibração</button><button onclick="acao('/cal/zerar')">Zerar</button>
<button onclick="acao('/cal/gas')">Refazer base do MQ-2</button>
<p class="hint" id="calinfo"></p>
</section>

<section>
<p class="k">Rede e Telegram</p>
<p id="wst" style="margin:0 0 10px;font-weight:600">—</p>
<button onclick="redes()">Procurar redes</button><br>
<label>Rede<select id="ssid"></select></label>
<label>Senha<input id="pw2" type="password"></label><br>
<button class="on" onclick="conectar()">Conectar</button><button onclick="acao('/wifi/esquecer')">Esquecer</button>
<p class="hint">Redes de 5 GHz não aparecem: o ESP32 não as enxerga.</p>
<label>Token do bot<input id="tk" size="24" style="width:230px"></label>
<label>chat_id<input id="ci"></label>
<label>CallMeBot<input id="cb" placeholder="@usuario"></label><br>
<button class="on" onclick="salvarTg()">Salvar Telegram</button>
<p class="hint">O chat_id é um número, não o telefone: envie /start ao bot e ele responde com o seu. Estes campos só aceitam alterações de quem está na rede BoviSense.</p>
</section>
</div>

<p id="msg" style="padding:0 16px 20px"></p>
</div><script>
var M=['Contínua','Liga-desliga','Adaptativa','Demonstração'];
var CV=['var(--c0)','var(--c1)','var(--c2)','var(--c3)'];
var modo=-1,aberto=false;
function $(i){return document.getElementById(i)}
function f(x,d){if(x===null||x===undefined||isNaN(x))return'--';
return Number(x).toFixed(d===undefined?1:d).replace('.',',')}
function tec(){aberto=!aberto;$('box').className='tec'+(aberto?' ab':'');
$('tg').innerHTML='Modo técnico '+(aberto?'&#9652;':'&#9662;');
try{localStorage.setItem('bs_tec',aberto?'1':'')}catch(e){}}
try{if(localStorage.getItem('bs_tec'))tec()}catch(e){}
function acao(u){fetch(u).then(function(r){return r.text()}).then(function(t){$('msg').textContent=t;atual()})
.catch(function(){$('msg').textContent='Sem resposta do BoviSense.'})}
function cal(){acao('/cal/th?t='+$('tr').value+'&u='+$('ur').value)}
function redes(){$('msg').textContent='Procurando redes...';
fetch('/redes').then(function(r){return r.json()}).then(function(l){
$('ssid').innerHTML=l.map(function(n){return'<option>'+n.s+'</option>'}).join('');
$('msg').textContent=l.length+' redes encontradas.'}).catch(function(){$('msg').textContent='Falha na busca.'})}
function conectar(){acao('/wifi?s='+encodeURIComponent($('ssid').value)+'&p='+encodeURIComponent($('pw2').value))}
function salvarTg(){acao('/tg?t='+encodeURIComponent($('tk').value)+'&c='+encodeURIComponent($('ci').value)
+'&u='+encodeURIComponent($('cb').value))}
function botoes(){$('modos').innerHTML=M.map(function(n,i){
return'<button class="'+(i==modo?'on':'')+'" onclick="acao(\'/modo?m='+i+'\')">'+n+'</button>'}).join('')}
function atual(){fetch('/api').then(function(r){return r.json()}).then(function(d){
$('itu').textContent=f(d.ITU);$('t').textContent=f(d.T);$('u').textContent=f(d.UR,0);$('g').textContent=f(d.IG,2);
$('cat').textContent=d.cat;$('est').textContent=d.estado;
$('stw').style.borderLeftColor=d.catn<4?CV[d.catn]:'var(--mut)';
$('vl').textContent=d.rec?'Ventilação recomendada':'Ventilação';
for(var i=0;i<4;i++)$('n'+i).className=(i==d.nivel?'on':'');
$('ph').textContent=d.hora;$('pw').textContent=d.wifi?'rede ✓':'rede ✗';
$('pt').textContent=d.tg?'Telegram ✓':'Telegram ✗';$('wst').textContent=d.wst;
if(d.modo!=modo){modo=d.modo;botoes()}
var a=$('aviso');a.style.display=d.alerta?'block':'none';a.textContent=d.alerta;
var e=d.ens;$('tab').innerHTML=[['Duração',f(e.min,0)+' min'],
['Tempo com ITU ≥ 72',f(e.acima)+' min'],['Carga térmica acima de 72',f(e.carga)+' ITU·min'],
['Plena carga equivalente',f(e.eq)+' min'],['Energia estimada',f(e.E,2)+' Wh'],
['Partidas / trocas de nível',e.part+' / '+e.troc],
['Plena carga eq. contínua',f(d.s[0])+' min'],['Plena carga eq. liga-desliga',f(d.s[1])+' min'],
['Plena carga eq. adaptativa',f(d.s[2])+' min']]
.map(function(r){return'<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>'}).join('');
$('calinfo').textContent='Correção atual do DHT11: '+f(d.cal.t)+' °C e '+f(d.cal.u)
+' % · limiares de gás G1 '+f(d.cal.g1,2)+' e G2 '+f(d.cal.g2,2)+'.';
}).catch(function(){$('est').textContent='sem conexão com o BoviSense'})}
function graf(){fetch('/hist').then(function(r){return r.json()}).then(function(h){
var cv=$('c'),x=cv.getContext('2d'),W=cv.width,H=cv.height,n=h.i.length;
x.clearRect(0,0,W,H);if(n<2)return;
var cs=getComputedStyle(document.body);
var tx=cs.getPropertyValue('--tx'),ac=cs.getPropertyValue('--ac'),mu=cs.getPropertyValue('--mut');
var X=function(k){return k*W/(n-1)},Y=function(v){return H-(Math.min(90,Math.max(60,v))-60)/30*H};
x.fillStyle=ac;x.globalAlpha=.18;x.beginPath();x.moveTo(0,H);
h.d.forEach(function(v,k){x.lineTo(X(k),H-v/100*H)});x.lineTo(W,H);x.fill();x.globalAlpha=1;
x.setLineDash([6,6]);x.strokeStyle=mu;x.lineWidth=1.5;x.globalAlpha=.65;
x.font='500 20px system-ui,sans-serif';x.fillStyle=mu;x.textBaseline='bottom';
[72,75,79].forEach(function(v){var y=Y(v);x.beginPath();x.moveTo(34,y);x.lineTo(W,y);x.stroke();
x.fillText(v,0,y+8)});x.globalAlpha=1;
x.setLineDash([]);x.strokeStyle=tx;x.lineWidth=3;x.lineJoin='round';x.lineCap='round';
x.beginPath();var s=0;h.i.forEach(function(v,k){if(v===null){s=0;return}
s?x.lineTo(X(k),Y(v)):x.moveTo(X(k),Y(v));s=1});x.stroke()})}
setInterval(atual,2500);setInterval(graf,10000);atual();graf();
</script></body></html>)HTML";

static const char *NOME_CAT_WEB[4] = {"Conforto térmico", "Estresse brando", "Atenção: próximo do crítico", "Estresse moderado"};
static void webApi() {
  static char b[1600]; char estTxt[80], hr[8] = "--:--", alerta[120] = "";
  uint32_t dec = (millis() - tInicioEstado) / 1000UL;
  if (estado == EST_AQUEC) snprintf(estTxt, sizeof(estTxt), "Aquecendo o MQ-2: faltam %lu s", (unsigned long)(T_AQUEC_S > dec ? T_AQUEC_S - dec : 0));
  else if (estado == EST_BASE) snprintf(estTxt, sizeof(estTxt), "Medindo o ar de referência: faltam %lu s", (unsigned long)(T_BASE_S > dec ? T_BASE_S - dec : 0));
  else if (recalGasAte) snprintf(estTxt, sizeof(estTxt), "Operando · recalibrando o MQ-2");
  else snprintf(estTxt, sizeof(estTxt), "Operando · amostra a cada 2,5 s");
  if (horaValida()) { time_t tt = time(nullptr); struct tm lt; localtime_r(&tt, &lt); snprintf(hr, sizeof(hr), "%02d:%02d", lt.tm_hour, lt.tm_min); }
  if (simulando()) snprintf(alerta, sizeof(alerta), "🚨 SIMULAÇÃO DE EMERGÊNCIA (%lu s) · dados reais não afetados", (unsigned long)((simAte - millis()) / 1000UL));
  else if (flags & F_FAILSAFE) snprintf(alerta, sizeof(alerta), "Falha no sensor de temperatura/umidade: ventilação forçada em 100 %%");
  else if (flags & F_ALARME_GAS) snprintf(alerta, sizeof(alerta), "Alerta de gases: ventilação em 100 %%");
  else if (ituCritAtivo) snprintf(alerta, sizeof(alerta), "ITU na faixa de estresse moderado");
  char rede[100];
  if (wifiOk) snprintf(rede, sizeof(rede), "Conectado a %s (IP %s)", wifiSsid.c_str(), WiFi.localIP().toString().c_str());
  else if (!wifiSsid.length()) snprintf(rede, sizeof(rede), "Nenhuma rede escolhida: só painel local");
  else snprintf(rede, sizeof(rede), "Tentando %s: %s", wifiSsid.c_str(), wifiDiag);
  uint8_t c = categoria(itu);
  snprintf(b, sizeof(b),
           "{\"estado\":\"%s\",\"modo\":%u,\"T\":%.1f,\"UR\":%.0f,\"ITU\":%.1f,\"IG\":%.2f,\"mv\":%lu,\"nivel\":%u,\"duty\":%u,"
           "\"rec\":%s,\"cat\":\"%s\",\"catn\":%u,\"alerta\":\"%s\",\"wifi\":%s,\"tg\":%s,\"hora\":\"%s\","
           "\"ens\":{\"min\":%.1f,\"acima\":%.1f,\"carga\":%.1f,\"eq\":%.1f,\"E\":%.3f,\"part\":%lu,\"troc\":%lu},"
           "\"s\":[%.1f,%.1f,%.1f],\"cal\":{\"t\":%.1f,\"u\":%.1f,\"g1\":%.2f,\"g2\":%.2f},\"wst\":\"%s\"}",
           estTxt, modo, Tf, Uf, itu, ig, (unsigned long)mqmV, nivel, dutyAlvo, SEM_ATUADOR ? "true" : "false",
           c < 4 ? NOME_CAT_WEB[c] : "Sem leitura", c < 4 ? c : 9, alerta, wifiOk ? "true" : "false", tgAtivo() ? "true" : "false", hr,
           ensaio.t_total_s / 60.0, ensaio.t_itu_acima_s / 60.0, ensaio.carga_itu_min, ensaio.t_eq_s / 60.0, ensaio.energia_wh,
           (unsigned long)ensaio.n_partidas, (unsigned long)ensaio.n_trocas,
           ensS[0].t_eq_s / 60.0, ensS[1].t_eq_s / 60.0, ensS[2].t_eq_s / 60.0, calOffT, calOffU, IG_LIMIAR[0], IG_LIMIAR[1], rede);
  String s(b); s.replace("nan", "null");
  servidor.send(200, "application/json; charset=utf-8", s);
}
static void webHist() {
  uint16_t n = regQtd < 240 ? regQtd : 240;
  String a; a.reserve(n * 12 + 32); a = "{\"i\":[";
  String d = "],\"d\":[";
  for (uint16_t k = 0; k < n; k++) {
    const Registro &r = regs[(regTopo + N_REG - n + k) % N_REG];
    if (k) { a += ','; d += ','; }
    if (r.ITU10 == INT16_MIN) a += "null"; else a += String(r.ITU10 / 10.0f, 1);
    d += String(r.duty);
  }
  a += d + "]}";
  servidor.send(200, "application/json", a);
}
static void webCsv() {
  servidor.setContentLength(CONTENT_LENGTH_UNKNOWN);
  servidor.sendHeader("Content-Disposition", "attachment; filename=bovisense.csv");
  servidor.send(200, "text/csv", "");
  servidor.sendContent("t,modo,T_C,UR_pct,ITU,IG,mq2_mV,nivel,duty_pct,flags\n");
  char linha[128]; String bloco;
  uint16_t ini = (regQtd == N_REG) ? regTopo : 0;
  for (uint16_t i = 0; i < regQtd; i++) {
    const Registro &r = regs[(ini + i) % N_REG];
    snprintf(linha, sizeof(linha), "%lu,%s,%.1f,%.1f,%.1f,%.2f,%u,%u,%u,%u\n", (unsigned long)r.t, NOME_MODO[r.modo],
             r.T10 == INT16_MIN ? NAN : r.T10 / 10.0, r.U10 == INT16_MIN ? NAN : r.U10 / 10.0,
             r.ITU10 == INT16_MIN ? NAN : r.ITU10 / 10.0, r.IG100 == 0xFFFF ? NAN : r.IG100 / 100.0, r.mv, r.nivel, r.duty, r.flags);
    bloco += linha;
    if (bloco.length() > 1200) { servidor.sendContent(bloco); bloco = ""; }
  }
  if (bloco.length()) servidor.sendContent(bloco);
  servidor.sendContent("");
}

// ============================ DIAGNÓSTICO DO WI-FI =========================
static const char *textoMotivo(uint8_t m) {
  switch (m) {
    case 2: case 15: case 202: case 204: return "senha errada (ou rede WPA3/empresarial)";
    case 201: return "rede nao encontrada (nome errado, 5 GHz ou fora de alcance)";
    case 203: return "roteador recusou a associacao (filtro MAC/limite de clientes)";
    case 210: return "seguranca incompativel (use WPA2 no roteador)";
    case 0: return "ainda tentando";
    default: return "outro motivo (veja o codigo)";
  }
}
static const char *textoAuth(int a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "aberta"; case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA"; case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2"; case WIFI_AUTH_WPA2_ENTERPRISE: return "EMPRESARIAL";
    default: return "WPA3/outra";
  }
}
// Varre as redes 2,4 GHz visíveis e diz se a rede configurada aparece
static void diagnosticarWiFi() {
  if (!wifiSsid.length()) { snprintf(wifiDiag, sizeof(wifiDiag), "sem SSID"); Serial.println(F("# Wi-Fi: nenhuma rede escolhida. Use o painel (rede BoviSense) ou o segredos.h.")); return; }
  Serial.println(F("# Wi-Fi: procurando redes 2,4 GHz..."));
  int n = WiFi.scanNetworks(false, true);
  bool achou = false, parecida = false; int canal = 0, auth = -1, rssi = 0;
  for (int i = 0; i < n; i++) {
    String nome = WiFi.SSID(i);
    if (i < 12) Serial.printf("#   %-24s %4d dBm  canal %2d  %s\n", nome.length() ? nome.c_str() : "(oculta)", (int)WiFi.RSSI(i), (int)WiFi.channel(i), textoAuth(WiFi.encryptionType(i)));
    if (nome == wifiSsid) { achou = true; canal = (int)WiFi.channel(i); auth = WiFi.encryptionType(i); rssi = (int)WiFi.RSSI(i); }
    else { String a = nome; a.trim(); a.toLowerCase(); String b = wifiSsid; b.trim(); b.toLowerCase(); if (a == b) parecida = true; }
  }
  WiFi.scanDelete();
  if (achou) {
    Serial.printf("# Wi-Fi: rede '%s' ENCONTRADA (canal %d, %d dBm, %s)\n", wifiSsid.c_str(), canal, rssi, textoAuth(auth));
    if (auth == WIFI_AUTH_WPA2_ENTERPRISE) Serial.println(F("# Wi-Fi: rede EMPRESARIAL (login com usuario, ex.: faculdade). O ESP32 nao entra: use o roteador do celular."));
    if (rssi < -80) Serial.println(F("# Wi-Fi: sinal fraco: aproxime o ESP32 do roteador."));
    snprintf(wifiDiag, sizeof(wifiDiag), "rede ok c%d %ddBm", canal, rssi);
  } else if (parecida) {
    Serial.printf("# Wi-Fi: existe rede com nome PARECIDO. O nome e exato (maiusculas, espacos): '%s'\n", wifiSsid.c_str());
    snprintf(wifiDiag, sizeof(wifiDiag), "nome diferente");
  } else {
    Serial.printf("# Wi-Fi: '%s' NAO aparece. Causas: rede so 5 GHz, nome errado ou longe demais.\n", wifiSsid.c_str());
    Serial.println(F("#   iPhone: ative 'Maximizar compatibilidade'. Android: roteador em banda 2,4 GHz."));
    snprintf(wifiDiag, sizeof(wifiDiag), "rede nao achada");
  }
}

// ---- configuração de rede e Telegram pelo painel (só pela rede local BoviSense)
static bool clienteLocal() { IPAddress ip = servidor.client().remoteIP(); return ip[0] == 192 && ip[1] == 168 && ip[2] == 4; }
static bool recusaRemoto() {
  if (clienteLocal()) return false;
  servidor.send(403, "text/plain; charset=utf-8", "Só pelo celular conectado à rede BoviSense.");
  return true;
}
static void webRedes() {
  if (recusaRemoto()) return;
  int n = WiFi.scanNetworks(false, true);
  String j = "[";
  int k = 0;
  for (int i = 0; i < n && k < 20; i++) {
    String nome = WiFi.SSID(i);
    if (!nome.length()) continue;
    if (k++) j += ',';
    nome.replace("\\", "\\\\"); nome.replace("\"", "\\\"");
    j += "{\"s\":\"" + nome + "\",\"r\":" + String((int)WiFi.RSSI(i)) + ",\"c\":" + String((int)WiFi.channel(i)) +
         ",\"e\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
  }
  WiFi.scanDelete();
  servidor.send(200, "application/json; charset=utf-8", j + "]");
}
static void webWifi() {
  if (recusaRemoto()) return;
  String ssid = servidor.arg("s"), senha = servidor.arg("p");
  if (!ssid.length()) { servidor.send(200, "text/plain; charset=utf-8", "Escolha uma rede na lista."); return; }
  salvarRede(ssid, senha);
  servidor.send(200, "text/plain; charset=utf-8", "Rede " + ssid + " salva. Conectando: acompanhe o status no cartão.");
}
static void webEsquecer() {
  if (recusaRemoto()) return;
  salvarRede("", "");
  servidor.send(200, "text/plain; charset=utf-8", "Rede esquecida. O painel local continua funcionando.");
}
static void webTg() {
  if (recusaRemoto()) return;
  String tok = servidor.arg("t"), chat = servidor.arg("c"), user = servidor.arg("u");
  for (unsigned i = 0; i < chat.length(); i++)
    if (!isdigit((unsigned char)chat[i]) && !(chat[i] == '-' && i == 0)) {
      servidor.send(200, "text/plain; charset=utf-8", "O chat_id é só números (não é o telefone). Deixe vazio, envie /start ao bot e ele responde com o número."); return;
    }
  salvarTelegram(tok, chat, user);
  servidor.send(200, "text/plain; charset=utf-8", "Telegram salvo na memória do ESP32.");
}

// ============================ TELEGRAM (núcleo 0) ===========================
static String urlCodificar(const char *s) {
  String o; const char *hex = "0123456789ABCDEF";
  for (; *s; s++) { uint8_t c = (uint8_t)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c; else if (c == ' ') o += '+';
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; } }
  return o;
}
static int tgPost(const char *metodo, const String &corpo, String *resp = nullptr) {
  WiFiClientSecure cli; cli.setInsecure();   // limitação discutida no artigo
  HTTPClient http; http.setTimeout(10000);
  String url = String("https://api.telegram.org/bot") + tgToken + "/" + metodo;
  if (!http.begin(cli, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  int cod = http.POST(corpo);
  if (resp && cod > 0) *resp = http.getString();
  if (cod != 200) {
    String r = cod > 0 ? (resp ? *resp : http.getString()) : String("sem resposta (DNS/TLS/internet)");
    Serial.printf("# TELEGRAM %s: erro HTTP %d -> %s\n", metodo, cod, r.substring(0, 160).c_str());
    if (cod == 400) Serial.println(F("# TELEGRAM: 'chat not found' = TG_CHAT_ID errado ou falta enviar /start ao bot"));
    if (cod == 401 || cod == 404) Serial.println(F("# TELEGRAM: token invalido ou revogado (TG_TOKEN)"));
  }
  http.end();
  return cod;
}
static bool tgEnviarPara(const char *chat, const char *texto) {
  JsonDocument doc; doc["chat_id"] = chat; doc["text"] = texto;
  String corpo; serializeJson(doc, corpo);
  return tgPost("sendMessage", corpo) == 200;
}
static bool tgEnviar(const char *texto) { return tgEnviarPara(tgChat.c_str(), texto); }
static void tgMenu() {
  tgPost("setMyCommands", F("{\"commands\":["
    "{\"command\":\"status\",\"description\":\"Leituras atuais\"},"
    "{\"command\":\"relatorio\",\"description\":\"Metricas da hora e do ensaio\"},"
    "{\"command\":\"simular\",\"description\":\"Simular emergencia (apresentacao)\"},"
    "{\"command\":\"parar\",\"description\":\"Encerrar a simulacao\"},"
    "{\"command\":\"modo\",\"description\":\"Estrategia: /modo 0, 1, 2 ou 3\"},"
    "{\"command\":\"novoensaio\",\"description\":\"Iniciar novo ensaio\"},"
    "{\"command\":\"calibrar_gas\",\"description\":\"Recalibrar o MQ-2 em ar limpo\"},"
    "{\"command\":\"ligar\",\"description\":\"Testar a ligacao de alerta\"},"
    "{\"command\":\"ajuda\",\"description\":\"Lista de comandos\"}]}"));
}
static bool fazerLigacao(const char *texto) {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http; http.setTimeout(25000);
  String url = String("https://api.callmebot.com/start.php?user=") + urlCodificar(cbUser.c_str()) +
               "&text=" + urlCodificar(texto) + "&lang=" + CALLMEBOT_LANG + "&rpt=2";
  if (!http.begin(cli, url)) return false;
  int cod = http.GET();
  if (cod != 200) Serial.printf("# LIGACAO: erro HTTP %d (autorizou o @CallMeBot_txtbot? tem @usuario no Telegram?)\n", cod);
  http.end();
  return cod == 200;
}
static void enviarCmd(uint8_t tipo, int arg) { Comando c = {tipo, arg}; xQueueSend(filaCmd, &c, 0); }
static void tgResponder(const char *cmd) {
  static char txt[900]; static Retrato r;
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
  r = retrato; xSemaphoreGive(mtx);
  if (!strncmp(cmd, "/status", 7)) {
    uint8_t c = categoria(r.itu);
    snprintf(txt, sizeof(txt), "BoviSense agora\nModo: %s%s\nT %.1f C | UR %.0f %%\nITU %.1f: %s\nGases (IG): %.2f\nVentilacao: %u %%\nEnsaio: %.0f min | energia %.2f Wh",
             NOME_MODO[r.modo], r.sim ? " (SIMULACAO ATIVA)" : "", r.T, r.U, r.itu, c < 4 ? NOME_CAT[c] : "sem leitura", r.ig, r.duty,
             r.ensaio.t_total_s / 60.0, r.ensaio.energia_wh);
    tgEnviar(txt);
  } else if (!strncmp(cmd, "/relatorio", 10)) {
    textoMetricas(txt, sizeof(txt), r.ensaio, "Ensaio atual"); tgEnviar(txt);
    textoEstrategias(txt, sizeof(txt), r.ensS); tgEnviar(txt);
  } else if (!strncmp(cmd, "/modo", 5)) {
    const char *p = cmd + 5; while (*p && !isdigit((unsigned char)*p)) p++;
    int m = *p ? atoi(p) : -1;
    if (m >= 0 && m <= 3) { enviarCmd(CMD_MODO, m); snprintf(txt, sizeof(txt), "Modo alterado para %s (novo ensaio iniciado).", NOME_MODO[m]); tgEnviar(txt); }
    else tgEnviar("Use /modo 0 (continua), /modo 1 (liga-desliga), /modo 2 (adaptativa) ou /modo 3 (demonstracao).");
  } else if (!strncmp(cmd, "/novoensaio", 11)) { enviarCmd(CMD_NOVO_ENSAIO, 0); tgEnviar("Novo ensaio iniciado.");
  } else if (!strncmp(cmd, "/simular", 8)) { enviarCmd(CMD_SIMULAR, 0);
  } else if (!strncmp(cmd, "/parar", 6)) { enviarCmd(CMD_PARAR_SIM, 0);
  } else if (!strncmp(cmd, "/calibrar_gas", 13)) { enviarCmd(CMD_RECAL_GAS, 0); tgEnviar("Recalibrando o MQ-2 por 60 s. Mantenha o ar limpo.");
  } else if (!strncmp(cmd, "/ligar", 6)) {
    if (ligacaoAtiva()) { tgEnviar("Fazendo ligacao de teste..."); fazerLigacao("Teste da ligacao de alerta do BoviSense."); }
    else tgEnviar("Ligacao desativada: preencha o usuario do CallMeBot no painel ou no segredos.h.");
  } else {
    tgEnviar("BoviSense - comandos:\n/status - leituras atuais\n/relatorio - metricas\n/simular - emergencia de teste (60 s, com ligacao)\n"
             "/parar - encerra a simulacao\n/modo N - estrategia (0 continua, 1 liga-desliga, 2 adaptativa, 3 demonstracao)\n"
             "/novoensaio - zera as metricas\n/calibrar_gas - linha de base do MQ-2\n/ligar - testa a ligacao");
  }
}
static void tgConsultar(int64_t &offset) {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http; http.setTimeout(10000);
  char url[220];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?timeout=0&limit=5&offset=%lld", tgToken.c_str(), (long long)offset);
  if (!http.begin(cli, url)) return;
  int cod = http.GET();
  if (cod != 200) { if (cod == 401 || cod == 404) Serial.println(F("# TELEGRAM: token invalido")); http.end(); return; }
  String resp = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return;
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    offset = (u["update_id"] | (int64_t)0) + 1;
    int64_t chat = u["message"]["chat"]["id"] | (int64_t)0;
    const char *texto = u["message"]["text"] | "";
    if (!chat) continue;
    char chatStr[24]; snprintf(chatStr, sizeof(chatStr), "%lld", (long long)chat);
    if (tgChat != chatStr) {   // ajuda na configuração: informa o chat_id a quem escreveu
      char m[240]; snprintf(m, sizeof(m), "Este chat ainda nao esta autorizado no BoviSense.\nSeu chat_id e: %s\nCole esse numero no painel do BoviSense (cartao Rede e Telegram) ou em TG_CHAT_ID no segredos.h.", chatStr);
      tgEnviarPara(chatStr, m);
      Serial.printf("# TELEGRAM: mensagem do chat %s (nao autorizado). Use esse numero em TG_CHAT_ID.\n", chatStr);
      continue;
    }
    if (texto[0] == '/') tgResponder(texto);
  }
}
static void tarefaRede(void *) {
  bool ntp = false, antes = false, saudou = false, menu = false; uint32_t ultPoll = 0, ultTent = 0; int64_t offset = 0;
  if (wifiSsid.length()) { WiFi.begin(wifiSsid.c_str(), wifiSenha.c_str()); ultTent = millis(); }
  else Serial.println(F("# Wi-Fi: sem rede escolhida -> so painel local. Escolha a rede no painel."));
  for (;;) {
    bool con = wifiSsid.length() && WiFi.status() == WL_CONNECTED;
    wifiOk = con;
    if (con && !antes) {
      snprintf(wifiDiag, sizeof(wifiDiag), "conectado");
      Serial.printf("# Wi-Fi: CONECTADO a '%s' | IP %s | sinal %d dBm\n", wifiSsid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (!ntp) { configTzTime("<-03>3", "a.st1.ntp.br", "pool.ntp.org"); ntp = true; }
      if (tgTokenOk() && !menu) { tgMenu(); menu = true; }
      if (tgAtivo() && !saudou) {
        char m[260];
        snprintf(m, sizeof(m), "BoviSense ligado e conectado a internet (IP %s).\nAquecendo o MQ-2: o controle comeca em cerca de 4 min.\n"
                 "Painel local: rede 'BoviSense' -> http://192.168.4.1\nToque em / para ver os comandos.", WiFi.localIP().toString().c_str());
        saudou = tgEnviar(m);
        Serial.println(saudou ? F("# TELEGRAM: mensagem de inicializacao ENVIADA") : F("# TELEGRAM: mensagem de inicializacao FALHOU (veja o erro acima)"));
      }
    }
    if (!con && antes) Serial.println(F("# Wi-Fi: conexao perdida"));
    antes = con;
    if (wifiSsid.length() && !con && millis() - ultTent > 30000) {
      Serial.printf("# Wi-Fi: sem conexao com '%s' | motivo %u: %s\n", wifiSsid.c_str(), wifiMotivo, textoMotivo(wifiMotivo));
      snprintf(wifiDiag, sizeof(wifiDiag), "falha %u", wifiMotivo);
      WiFi.disconnect(false, false); delay(100); WiFi.begin(wifiSsid.c_str(), wifiSenha.c_str()); ultTent = millis();
    }
    if (pedidoReconectar) {
      pedidoReconectar = false; saudou = false; menu = false; wifiMotivo = 0;
      WiFi.disconnect(false, false); vTaskDelay(pdMS_TO_TICKS(300));
      snprintf(wifiDiag, sizeof(wifiDiag), "conectando...");
      if (wifiSsid.length()) { WiFi.begin(wifiSsid.c_str(), wifiSenha.c_str()); ultTent = millis(); Serial.printf("# Wi-Fi: conectando a '%s' (escolhida no painel)\n", wifiSsid.c_str()); }
    }
    MsgSaida m;
    if (con && xQueueReceive(filaSaida, &m, 0) == pdTRUE) {
      bool ok = (m.tipo == SAI_MSG) ? tgEnviar(m.texto) : fazerLigacao(m.texto);
      Serial.printf("# REDE: %s %s\n", m.tipo == SAI_MSG ? "mensagem" : "ligacao", ok ? "enviada" : "FALHOU");
    }
    if (con && tgTokenOk() && millis() - ultPoll > TG_POLL_MS) { ultPoll = millis(); tgConsultar(offset); }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================ BOTÃO E SERIAL ===============================
static void servicoBotao() {
  static bool anterior = true; static uint32_t tPress = 0; static bool avisou = false;
  bool solto = digitalRead(PIN_BOTAO);
  if (anterior && !solto) { tPress = millis(); avisou = false; }
  if (!solto && !avisou && millis() - tPress > T_BOTAO_SIM_MS) { bipsAgendar(1); avisou = true; }  // já pode soltar
  if (!anterior && solto) {
    uint32_t d = millis() - tPress;
    if (d >= T_BOTAO_SIM_MS) { if (simulando()) pararSimulacao(); else iniciarSimulacao("botao BOOT"); }
    else if (d > 50) {
      if (estado == EST_AQUEC) { Serial.println(F("# aquecimento pulado")); tInicioEstado = millis() - T_AQUEC_S * 1000UL; }
      else definirModo((modo + 1) % 4);
    }
  }
  anterior = solto;
}
static void servicoSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'm') { delay(20); definirModo(Serial.read() - '0'); }
  else if (c == 'r') novoEnsaio();
  else if (c == 'p') { static char t[900]; textoMetricas(t, sizeof(t), ensaio, "Ensaio atual"); Serial.println(t); textoEstrategias(t, sizeof(t), ensS); Serial.println(t); }
  else if (c == 'k' && estado == EST_AQUEC) tInicioEstado = millis() - T_AQUEC_S * 1000UL;
  else if (c == 's') iniciarSimulacao("monitor serial");
  else if (c == 'x') pararSimulacao();
  else if (c == 'h') Serial.println(F("# comandos: m0..m3 modo | r novo ensaio | p metricas | k pular aquecimento | s simular | x parar"));
}

// ============================ SETUP / LOOP =================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT); pinMode(PIN_BOTAO, INPUT_PULLUP);
  pwmIniciar(); fanEscrever(0);
  dht.begin(); delay(300);
  carregarCalibracao();

  Serial.println(F("\n# ================ BoviSense - AUTOTESTE ================"));
  delay(1200);
  float tT = dht.readTemperature(), uT = dht.readHumidity(); bool dhtOk = !isnan(tT) && !isnan(uT);
  if (dhtOk) Serial.printf("# DHT11: OK (T = %.1f C, UR = %.0f %%) | calibracao salva: T %+.1f, UR %+.1f\n", tT, uT, calOffT, calOffU);
  else Serial.println(F("# DHT11: SEM LEITURA (3 bipes curtos). +->3V3, - ->GND, OUT->D4"));
  uint32_t mvT = lerMQ2mV();
  bool mqOk = (mvT >= 20) && (mvT <= 3200);
  if (mqOk) Serial.printf("# MQ-2: OK (%lu mV no D34)\n", (unsigned long)mvT);
  else if (mvT < 20) Serial.printf("# MQ-2: %lu mV, quase zero (2 bipes longos). Confira AO->D34 e VCC->3V3.\n", (unsigned long)mvT);
  else Serial.printf("# MQ-2: %lu mV, no limite do ADC (2 bipes longos). O VCC esta no VIN/5V? Passe para 3V3.\n", (unsigned long)mvT);
  if (tgTokenOk()) Serial.println(chatIdValido() ? F("# Telegram: token e chat_id com formato valido")
                                                   : F("# Telegram: TG_CHAT_ID invalido/vazio. Envie /start ao bot: ele responde com o seu chat_id."));
  else Serial.println(F("# Telegram: desativado (TG_TOKEN vazio)"));
  Serial.printf("# Ligacao de alerta: %s\n", ligacaoAtiva() ? "configurada" : "desativada (CALLMEBOT_USER vazio)");
  Serial.println(F("# Ventilador fica DESLIGADO no aquecimento (180 s) e na linha de base (60 s)."));
  Serial.println(F("# BOOT: toque = proximo modo (ou pula aquecimento) | segurar 3 s = simular emergencia"));
  Serial.println(F("# Sem display: acompanhe pelo painel web, pelo Telegram e por este monitor."));
  Serial.println(F("# ========================================================"));
  if (!dhtOk) for (int i = 0; i < 3; i++) { buzzerTom(2200); delay(120); buzzerTom(0); delay(150); }
  if (!mqOk)  for (int i = 0; i < 2; i++) { buzzerTom(700);  delay(700); buzzerTom(0); delay(250); }
  if (dhtOk && mqOk) { buzzerTom(1800); delay(90); buzzerTom(0); delay(70); buzzerTom(2600); delay(140); buzzerTom(0); }

  mtx = xSemaphoreCreateMutex();
  filaSaida = xQueueCreate(6, sizeof(MsgSaida));
  filaCmd = xQueueCreate(6, sizeof(Comando));
  zerarEnsaio(); zerarHora();

  WiFi.mode(WIFI_AP_STA);
  wifi_country_t pais; memset(&pais, 0, sizeof(pais));   // Brasil: canais 1 a 13
  strcpy(pais.cc, "BR"); pais.schan = 1; pais.nchan = 13; pais.max_tx_power = 20; pais.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&pais);
  WiFi.setSleep(false);
  WiFi.onEvent([](arduino_event_id_t e, arduino_event_info_t info) {
    if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) wifiMotivo = info.wifi_sta_disconnected.reason;
  });
  diagnosticarWiFi();
  WiFi.softAP(AP_SSID, AP_SENHA);
  servidor.on("/", []() { servidor.send_P(200, "text/html; charset=utf-8", PAGINA); });
  servidor.on("/api", webApi);
  servidor.on("/hist", webHist);
  servidor.on("/csv", webCsv);
  servidor.on("/modo", []() { definirModo(servidor.arg("m").toInt()); servidor.send(200, "text/plain; charset=utf-8", "Estratégia alterada; novo ensaio iniciado."); });
  servidor.on("/novo", []() { novoEnsaio(); servidor.send(200, "text/plain; charset=utf-8", "Novo ensaio iniciado."); });
  servidor.on("/simular", []() { iniciarSimulacao("painel web"); servidor.send(200, "text/plain; charset=utf-8", "Simulação iniciada: Telegram e ligação disparados."); });
  servidor.on("/parar", []() { pararSimulacao(); servidor.send(200, "text/plain; charset=utf-8", "Simulação encerrada."); });
  servidor.on("/cal/th", []() { char m[160]; calibrarTU(servidor.arg("t").toFloat(), servidor.arg("u").toFloat(), m, sizeof(m)); servidor.send(200, "text/plain; charset=utf-8", m); });
  servidor.on("/cal/zerar", []() { zerarCalibracao(); servidor.send(200, "text/plain; charset=utf-8", "Calibração do DHT11 zerada."); });
  servidor.on("/redes", webRedes);
  servidor.on("/wifi", webWifi);
  servidor.on("/wifi/esquecer", webEsquecer);
  servidor.on("/tg", webTg);
  servidor.on("/cal/gas", []() {
    if (estado != EST_OPER) { servidor.send(200, "text/plain; charset=utf-8", "Aguarde o fim do aquecimento."); return; }
    iniciarRecalGas(); servidor.send(200, "text/plain; charset=utf-8", "Recalibrando o MQ-2 por 60 s: mantenha o ar limpo."); });
  servidor.begin();
  xTaskCreatePinnedToCore(tarefaRede, "rede", 16384, nullptr, 1, nullptr, 0);

  Serial.printf("# Painel: conecte-se a rede '%s' e abra http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  imprimirCabecalhoCSV();
  iniciarEstado(EST_AQUEC);
  bipsAgendar(1);
}

void loop() {
  uint32_t agora = millis();
  if (agora - ultimaAmostra >= T_AMOSTRA_MS || ultimaAmostra == 0) amostrar();
  Comando c;
  while (xQueueReceive(filaCmd, &c, 0) == pdTRUE) {
    if (c.tipo == CMD_MODO) definirModo(c.arg);
    else if (c.tipo == CMD_NOVO_ENSAIO) novoEnsaio();
    else if (c.tipo == CMD_SIMULAR) iniciarSimulacao("Telegram");
    else if (c.tipo == CMD_PARAR_SIM) pararSimulacao();
    else if (c.tipo == CMD_RECAL_GAS && estado == EST_OPER) iniciarRecalGas();
  }
  servicoVentilador(); servicoBotao(); servicoSerial(); bipServico();
  servidor.handleClient();
  delay(2);
}
