/*
 * Ventilação adaptativa para bovinos leiteiros.
 * ESP32 DevKit 30 pinos | DHT11 | MQ-2 | buzzer | TIP122. Sem display.
 *
 * Núcleo 1: amostragem, decisão, acionamento e modo seguro.
 * Núcleo 0: Wi-Fi, painel web e Telegram, por fila.
 *
 * Arduino-ESP32 2.0.x ou 3.x.
 * Bibliotecas: DHT sensor library, Adafruit Unified Sensor, ArduinoJson 7.
 *
 * Procedência dos parâmetros: ITU de Kelly e Bond (1971) apud Azevedo et al.
 * (2005); limiares 72/75/79 de Armstrong (1994) apud Azevedo et al. (2005).
 * O MQ-2 não mede NH3 e nada vira ppm: IG = R0/Rs é índice relativo.
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

// Tem que corresponder à fiação: trocar um sem o outro falseia o índice de
// gás, ou leva 5 V ao D34.
//   1 = VCC no 3V3, AO direto no D34. Montagem atual. O aquecedor fica com
//       44 % da potência de catálogo, declarado como limitação no artigo.
//   0 = VCC no VIN (5 V), AO por divisor de dois resistores de 10 kΩ.
#define MQ2_EM_3V3    1

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
// AO -> 10k -> ponto de medida (D34) -> 10k -> GND. Divide por 2: os 5 V
// chegam ao pino como 2,5 V. Com 10k/20k seriam 3,33 V, no limite.
#define MQ2_R1_OHM 10000.0f
#define MQ2_R2_OHM 10000.0f
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
#define LEMBRETE_ITU_S   600
#define COOLDOWN_MSG_S   600
#define LEMBRETE_BIP_S   30
#define TG_POLL_MS       4000

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
enum TipoSaida : uint8_t { SAI_MSG = 0 };
struct MsgSaida { uint8_t tipo; char texto[900]; };
enum TipoCmd : uint8_t { CMD_MODO = 0, CMD_NOVO_ENSAIO, CMD_SIMULAR, CMD_PARAR_SIM, CMD_RECAL_GAS, CMD_ADOTAR_CHAT };
struct Comando { uint8_t tipo; int arg; };

// ============================ OBJETOS GLOBAIS ==============================
DHT dht(PIN_DHT, DHT_TIPO);
WebServer servidor(80);
Preferences prefs;
SemaphoreHandle_t mtx, mtxCred;
QueueHandle_t filaSaida, filaCmd;

// Escritas pelo painel (núcleo 1), lidas pela tarefa de rede (núcleo 0).
// String realoca ao ser reatribuída, então o núcleo 0 só lê as cópias r0*,
// renovadas sob mutex no início de cada volta.
static String wifiSsid, wifiSenha, tgToken, tgChat;
static String r0Ssid, r0Senha, r0Token, r0Chat;
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
static char gasMotivo[72] = "aquecendo o sensor";   // por que o indice de gas nao esta disponivel
static uint8_t baseGasTent = 0;                     // tentativas de linha de base
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
static uint32_t ultMsg[3] = {0, 0, 0};
static uint32_t ultLembrete = 0;
static int ultimaHoraRelatorio = -1; static uint32_t ultRelatorioMs = 0;
static uint32_t simAte = 0; static bool simAnterior = false;
static uint32_t bipFim = 0, bipProx = 0; static uint8_t bipsPend = 0;
static uint32_t descartadas = 0;   // alertas perdidos por fila cheia

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
// Só a estratégia aplicada move o ventilador, e só ela altera o ambiente.
// Daí as métricas de ITU não serem acumuladas para as outras duas: sairiam
// iguais às dela e insinuariam uma comparação de conforto que não existe.
static void acumular(Metricas &m, float dt, uint8_t niv, uint8_t duty,
                     bool partida, bool troca, bool aplicada) {
  m.t_total_s += dt; m.t_nivel_s[niv] += dt;
  if (duty > 0) m.t_ligado_s += dt;
  m.t_eq_s += dt * duty / 100.0;
  m.energia_wh += P_NIVEL_W[niv] * dt / 3600.0;
  if (partida) m.n_partidas++;
  if (troca) m.n_trocas++;
  if (!aplicada) return;
  if (!isnan(itu)) {
    m.n_itu++; m.soma_itu += itu;
    if (itu > m.itu_max) m.itu_max = itu;
    if (itu < m.itu_min) m.itu_min = itu;
    if (itu >= ITU_LIMIAR[0]) { m.t_itu_acima_s += dt; m.carga_itu_min += (itu - ITU_LIMIAR[0]) * dt / 60.0; }
  }
  if (!isnan(ig) && ig > m.ig_max) m.ig_max = ig;
}

// ============================ CALIBRAÇÃO (memória permanente) ==============
// A NVS aceita texto vazio como valor legítimo, e getString só devolve o padrão
// quando a chave não existe. Um clique em salvar com o campo em branco gravava
// "" e sombreava o segredos.h para sempre, inclusive depois de regravar o
// firmware, porque a NVS sobrevive à gravação. Aqui vazio significa "nada
// gravado": vale o segredos.h. credDoArquivo guarda a procedência para o
// autoteste dizer de onde veio cada credencial.
//
// Quando o segredos.h muda, vale o que acabou de ser editado. A chave "o_..."
// guarda o valor do segredos.h visto na inicialização anterior; se o valor
// compilado agora é outro (ou se a chave não existe, caso de quem vem de uma
// versão sem essa regra), o que estava gravado pelo painel é descartado. Sem
// isto, um token antigo colado no painel continuava valendo depois de o
// segredos.h receber o token novo, e todo envio voltava 404.
static bool credDoArquivo[3] = {false, false, false};
static uint8_t credTrocada = 0;   // bit i: o segredos.h mudou e substituiu o valor do painel
static String credGravada(const char *chave, const char *padrao, int qual = -1, bool padraoUtil = true) {
  char kOrig[16]; snprintf(kOrig, sizeof(kOrig), "o_%s", chave);
  bool viuAntes = prefs.isKey(kOrig);
  String orig = viuAntes ? prefs.getString(kOrig, "") : String();
  if (!viuAntes || orig != padrao) {
    if (*padrao && padraoUtil && prefs.isKey(chave)) {
      prefs.remove(chave);
      if (qual >= 0) credTrocada |= 1 << qual;
    }
    prefs.putString(kOrig, padrao);
  }
  String v = prefs.isKey(chave) ? prefs.getString(chave, "") : String();
  if (v.length()) return v;
  if (qual >= 0) credDoArquivo[qual] = (*padrao != 0);
  return String(padrao);
}
static bool idChatValido(const char *id);
static bool ehIdDoBot(const String &chat, const String &tok);
static void carregarCalibracao() {
  prefs.begin("bovisense", false);   // escrita: credGravada registra o segredos.h visto
  calOffT = prefs.getFloat("offT", 0.0f); calOffU = prefs.getFloat("offU", 0.0f);
  wifiSsid = credGravada("wifi_s", WIFI_SSID, 0); wifiSenha = credGravada("wifi_p", WIFI_SENHA);
  tgToken = credGravada("tg_t", TG_TOKEN, 1);
  // Um TG_CHAT_ID inutilizável (vazio ou o id do próprio bot) não apaga o chat
  // vinculado automaticamente.
  tgChat = credGravada("tg_c", TG_CHAT_ID, 2, idChatValido(TG_CHAT_ID) && !ehIdDoBot(String(TG_CHAT_ID), tgToken));
  prefs.end();
}
// Apagar a chave, em vez de gravar vazio, é o que faz "esquecer" significar
// "volta ao valor do segredos.h" em vez de "fica cego para ele".
static void gravarCred(const char *chave, const String &v) {
  if (v.length()) prefs.putString(chave, v);
  else            prefs.remove(chave);
}
// A rede escolhida no painel fica gravada; o segredos.h serve só de valor inicial
static void salvarRede(const String &ssid, const String &senha) {
  xSemaphoreTake(mtxCred, portMAX_DELAY);
  wifiSsid = ssid; wifiSenha = senha;
  xSemaphoreGive(mtxCred);
  prefs.begin("bovisense", false); gravarCred("wifi_s", ssid); gravarCred("wifi_p", senha); prefs.end();
  pedidoReconectar = true;
}
// Campo em branco apaga a chave na NVS, e na próxima inicialização volta a valer
// o segredos.h. A RAM precisa seguir a mesma regra já agora: copiar o vazio
// desligava o Telegram até reiniciar quem salvasse só o chat_id, com o token
// vindo do segredos.h.
static void salvarTelegram(const String &tok, const String &chat) {
  xSemaphoreTake(mtxCred, portMAX_DELAY);
  tgToken = tok.length() ? tok : String(TG_TOKEN);
  tgChat  = chat.length() ? chat : String(TG_CHAT_ID);
  xSemaphoreGive(mtxCred);
  prefs.begin("bovisense", false); gravarCred("tg_t", tok); gravarCred("tg_c", chat); prefs.end();
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
// Sem R0 o índice de gás fica indisponível pelo resto da sessão, então a
// falha é relatada em vez de silenciosa.
static bool definirLimiaresGas() {
  if (nBaseR < 5) {
    snprintf(gasMotivo, sizeof(gasMotivo),
             "linha de base falhou: so %lu leituras validas de %lu",
             (unsigned long)nBaseR, (unsigned long)(T_BASE_S * 1000UL / T_AMOSTRA_MS));
    return false;
  }
  R0rel = somaR / nBaseR;
  double var = somaR2 / nBaseR - (double)R0rel * R0rel;
  cvBase = var > 0 ? sqrt(var) / R0rel : 0;
  IG_LIMIAR[0] = max(1.5f, 1.0f + 6.0f * cvBase);
  IG_LIMIAR[1] = max(2.5f, IG_LIMIAR[0] + 0.5f);
  gasMotivo[0] = 0; baseGasTent = 0;
  return true;
}
static void enfileirar(uint8_t tipo, const char *txt);   // definida adiante
// Repete a linha de base até três vezes antes de desistir do canal de gás.
static void concluirBaseGas(const char *origem) {
  if (definirLimiaresGas()) {
    Serial.printf("# MQ-2 (%s): R0/RL=%.3f cv=%.3f G1=%.2f G2=%.2f\n",
                  origem, R0rel, cvBase, IG_LIMIAR[0], IG_LIMIAR[1]);
    return;
  }
  if (++baseGasTent < 3) {
    Serial.printf("# MQ-2 (%s): %s. Tentando de novo por %lu s.\n",
                  origem, gasMotivo, (unsigned long)T_BASE_S);
    iniciarRecalGas();
  } else {
    snprintf(gasMotivo, sizeof(gasMotivo), "sem leitura valida: confira VCC e AO do MQ-2");
    Serial.printf("# MQ-2: INDICE DE GAS DESATIVADO (%s). O controle segue so pelo ITU.\n", gasMotivo);
    enfileirar(SAI_MSG, "Atencao: o sensor de gas nao respondeu. O controle segue so por temperatura e umidade.");
  }
}

// ============================ SAÍDAS DE REDE ===============================
static bool idChatValido(const char *id) {
  if (!*id) return false;
  for (const char *p = id; *p; p++) if (!(isdigit((unsigned char)*p) || (*p == '-' && p == id))) return false;
  return true;
}
// O número antes dos dois-pontos do token é o id do próprio bot, e é o engano
// mais comum ao preencher o TG_CHAT_ID. Bot não manda mensagem para bot: o
// Telegram recusa e nada chega. Esse número é tratado como chat não preenchido.
static bool ehIdDoBot(const String &chat, const String &tok) {
  int p = tok.indexOf(':');
  return p > 0 && (int)chat.length() == p && tok.startsWith(chat);
}
static bool chatIdValido() { return idChatValido(tgChat.c_str()) && !ehIdDoBot(tgChat, tgToken); }
static bool tgTokenOk() { return tgToken.length() > 20; }
// O Telegram respondeu 400 ou 403 ao enviar para o chat configurado: o número
// tem formato certo mas não serve. Só o núcleo 0 escreve.
static volatile bool tgChatRecusado = false;
// Chat a vincular: o núcleo 0 escreve antes de enfileirar CMD_ADOTAR_CHAT e o
// núcleo 1 lê depois de retirar o comando da fila, que faz a sincronização.
static char chatAdotar[24] = "";
// Nome de usuário do bot (getMe). Escrito uma vez no núcleo 0; o painel só lê.
static char botUser[40] = "";
// --- lado do núcleo 0: só estas funções e cópias podem ser usadas lá ---
static bool r0TokenOk()   { return r0Token.length() > 20; }
static bool r0ChatOk()    { return idChatValido(r0Chat.c_str()) && !ehIdDoBot(r0Chat, r0Token) && !tgChatRecusado; }
static bool r0Ativo()     { return r0TokenOk() && r0ChatOk(); }
static void sincronizarCredenciais() {
  if (xSemaphoreTake(mtxCred, pdMS_TO_TICKS(20)) != pdTRUE) return;
  r0Ssid = wifiSsid; r0Senha = wifiSenha;
  r0Token = tgToken; r0Chat = tgChat;
  xSemaphoreGive(mtxCred);
}
static bool tgAtivo() { return tgTokenOk() && chatIdValido(); }
// Código HTTP do último envio. Escrito no núcleo 0, lido no núcleo 1: int
// alinhado de 32 bits, escrita atômica, dispensa mutex. 0 = nada enviado ainda.
static volatile int tgUltimoCod = 0;
// Credencial nova gravada pelo painel: a tarefa de rede refaz a apresentação
// para que o resultado apareça em segundos, não na hora cheia.
static volatile bool tgReapresentar = false;
// Conferir o formato do token e do chat_id não diz nada sobre a API aceitá-los,
// então o painel exige resposta 200.
static bool tgOk() { return tgAtivo() && tgUltimoCod == 200; }
// Grava só o chat_id. O token continua vindo de onde vinha (segredos.h ou
// painel): gravá-lo aqui o congelaria na NVS por cima do segredos.h.
static void salvarChat(const String &chat) {
  xSemaphoreTake(mtxCred, portMAX_DELAY);
  tgChat = chat;
  xSemaphoreGive(mtxCred);
  prefs.begin("bovisense", false); gravarCred("tg_c", chat); prefs.end();
  tgUltimoCod = 0; tgReapresentar = true;
  Serial.printf("# TELEGRAM: chat_id %s gravado na memoria do ESP32 (vale mesmo apos regravar o firmware)\n", chat.c_str());
}
// Buffer estático: só webApi() chama isto, sempre no núcleo 1.
static const char *tgMotivo() {
  static char m[80];
  if (!tgTokenOk())    return "token do bot não preenchido";
  if (ehIdDoBot(tgChat, tgToken))
    return "o chat_id preenchido é o número do próprio bot (o início do token). Mande qualquer mensagem ao bot pelo seu Telegram e ele se vincula ao seu chat";
  if (!chatIdValido()) return "nenhum chat vinculado. Mande qualquer mensagem ao bot pelo seu Telegram e ele se vincula ao seu chat";
  switch (tgUltimoCod) {
    case 0:   return "aguardando o primeiro envio";
    case 200: return "";
    case 400: return "erro 400: chat_id errado ou conversa nunca iniciada. Mande qualquer mensagem ao bot e ele se vincula ao seu chat";
    case 403: return "erro 403: o bot foi bloqueado nesse chat. Desbloqueie e mande qualquer mensagem ao bot";
    case 401:
    case 404: return "erro 401/404 (token inválido ou revogado no BotFather)";
    default:
      if (tgUltimoCod < 0) return "sem resposta (sem internet, ou falha de DNS ou TLS)";
      snprintf(m, sizeof(m), "o Telegram recusou o envio (HTTP %d)", tgUltimoCod);
      return m;
  }
}
static void enfileirar(uint8_t tipo, const char *txt) {
  if (tipo == SAI_MSG && !tgAtivo()) return;
  MsgSaida m; m.tipo = tipo; strlcpy(m.texto, txt, sizeof(m.texto));
  // Um envio lento (DNS, TLS) segura a tarefa de rede por alguns segundos; sem
  // espera, os alertas gerados nesse intervalo sumiriam sem rastro.
  if (xQueueSend(filaSaida, &m, pdMS_TO_TICKS(250)) != pdTRUE) {
    descartadas++;
    Serial.printf("# REDE: fila cheia, alerta DESCARTADO (%lu no total): %.60s\n",
                  (unsigned long)descartadas, m.texto);
  }
}
static void alertar(uint8_t canal, const char *msg) {
  uint32_t s = millis() / 1000UL;
  if (ultMsg[canal] == 0 || s - ultMsg[canal] >= COOLDOWN_MSG_S) { enfileirar(SAI_MSG, msg); ultMsg[canal] = s ? s : 1; }
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
           "Decisao das tres estrategias sobre as MESMAS leituras:\nE1 continua: %.1f min eq. (%.0f %%)\n"
           "E2 liga-desliga: %.1f min eq. (%.0f %%), trocas %lu\nE3 adaptativa: %.1f min eq. (%.0f %%), trocas %lu\n"
           "Diferenca de acionamento E3 x E1: %.0f %%\n"
           "So E%d acionou o ventilador: isto compara a decisao, nao o conforto nem a energia medida.",
           m[0].t_eq_s / 60.0, 100.0 * m[0].t_eq_s / tot, m[1].t_eq_s / 60.0, 100.0 * m[1].t_eq_s / tot,
           (unsigned long)m[1].n_trocas, m[2].t_eq_s / 60.0, 100.0 * m[2].t_eq_s / tot, (unsigned long)m[2].n_trocas,
           m[0].t_eq_s > 0 ? 100.0 * (m[0].t_eq_s - m[2].t_eq_s) / m[0].t_eq_s : 0.0,
           modo == MODO_CONTINUO ? 1 : (modo == MODO_LIGA_DESLIGA ? 2 : 3));
}

// ============================ SIMULAÇÃO DE EMERGÊNCIA ======================
static void iniciarSimulacao(const char *origem) {
  simAte = millis() + T_SIM_MS; if (!simAte) simAte = 1;
  bipsAgendar(6);
  char m[320];
  snprintf(m, sizeof(m), "[SIMULACAO] Emergencia no BoviSense (acionada por %s): ITU critico e alerta de gases "
           "simulados por %lu s. Ventilacao forcada em 100%%. Dados reais nao foram alterados.", origem, T_SIM_MS / 1000UL);
  enfileirar(SAI_MSG, m);
  Serial.printf("# SIMULACAO iniciada por %s\n", origem);
}
static void pararSimulacao() { if (simAte) { simAte = 0; } }

// ============================ AMOSTRAGEM E CONTROLE ========================
static void iniciarEstado(Estado e) { estado = e; tInicioEstado = millis(); }

// ============================ RETOMADA APÓS REINÍCIO =======================
// Um pânico ou watchdog reinicia o ESP32 em segundos, mas o 3V3 não cai e o
// aquecedor do MQ-2 continua quente. Repetir os 240 s de aquecimento e linha
// de base deixaria o ventilador parado por 4 min por causa de uma falha que
// nada tem a ver com o ambiente. A RAM do RTC sobrevive a esses reinícios
// (não a um desligamento nem ao botão EN), e guarda o necessário para voltar
// direto ao controle: estratégia, linha de base do MQ-2 e ITU de referência.
struct Retomada { uint32_t magica; uint8_t modo; float R0rel, cvBase, ituBase, g1, g2; uint32_t soma; };
RTC_NOINIT_ATTR static Retomada rtcRet;
#define RET_MAGICA 0xB0515E01UL
static uint32_t somaRet(const Retomada &r) {
  const uint8_t *p = (const uint8_t *)&r; uint32_t h = 2166136261UL;
  for (size_t i = 0; i < offsetof(Retomada, soma); i++) { h ^= p[i]; h *= 16777619UL; }
  return h;
}
static void guardarRetomada() {
  if (estado != EST_OPER || isnan(R0rel)) return;
  Retomada r; memset(&r, 0, sizeof(r));
  r.magica = RET_MAGICA; r.modo = modo; r.R0rel = R0rel; r.cvBase = cvBase; r.ituBase = ituBase;
  r.g1 = IG_LIMIAR[0]; r.g2 = IG_LIMIAR[1]; r.soma = somaRet(r);
  rtcRet = r;
}
static const char *textoReinicio(esp_reset_reason_t m) {
  switch (m) {
    case ESP_RST_PANIC:    return "panico (erro de software)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_SW:       return "reinicio por software";
    case ESP_RST_BROWNOUT: return "queda de tensao";
    default:               return "outro";
  }
}
// Devolve true se retomou; nesse caso o estado já é EST_OPER.
static bool tentarRetomada(char *motivo, size_t n) {
  esp_reset_reason_t m = esp_reset_reason();
  bool quente = m == ESP_RST_PANIC || m == ESP_RST_INT_WDT || m == ESP_RST_TASK_WDT || m == ESP_RST_WDT ||
                m == ESP_RST_SW || m == ESP_RST_BROWNOUT;
  bool valido = rtcRet.magica == RET_MAGICA && rtcRet.soma == somaRet(rtcRet) && rtcRet.modo <= MODO_DEMO &&
                isfinite(rtcRet.R0rel) && rtcRet.R0rel > 0;
  snprintf(motivo, n, "%s", textoReinicio(m));
  if (!quente || !valido) { rtcRet.magica = 0; return false; }
  modo = (Modo)rtcRet.modo; R0rel = rtcRet.R0rel; cvBase = rtcRet.cvBase; ituBase = rtcRet.ituBase;
  IG_LIMIAR[0] = rtcRet.g1; IG_LIMIAR[1] = rtcRet.g2;
  for (int k = 0; k < 3; k++) ituDemo[k] = isnan(ituBase) ? ITU_LIMIAR[k] : ituBase + 1.0f + k;
  stItu.limiar = limiaresAtivos();
  zerarEstagiador(stItu); zerarEstagiador(stGas);
  iniciarEstado(EST_OPER);
  return true;
}
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
  else if (recalGasAte) { recalGasAte = 0; concluirBaseGas("recalibracao"); }
  ig = (!recal && !isnan(R0rel) && mqOk && rsRel > 0) ? R0rel / rsRel : NAN;
  if (isnan(ig) && baseGasTent < 3) {
    if (recal) snprintf(gasMotivo, sizeof(gasMotivo), "medindo o ar de referencia");
    else if (estado != EST_OPER) snprintf(gasMotivo, sizeof(gasMotivo), "aquecendo o sensor");
    else if (!mqOk) snprintf(gasMotivo, sizeof(gasMotivo), "leitura fora de faixa: %lu mV no D34", (unsigned long)mqmV);
  }

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
        concluirBaseGas("linha de base");
        ituBase = nBaseItu ? somaItuBase / nBaseItu : NAN;
        for (int k = 0; k < 3; k++) ituDemo[k] = isnan(ituBase) ? ITU_LIMIAR[k] : ituBase + 1.0f + k;
        stItu.limiar = limiaresAtivos();
        zerarEstagiador(stItu); zerarEstagiador(stGas); zerarEnsaio(); zerarHora();
        iniciarEstado(EST_OPER);
        guardarRetomada();
        Serial.printf("# BASE: ITU_base=%.1f\n", ituBase);
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
        acumular(ensS[i], dt, nivS[i], dS, dS > 0 && dutyPrevS[i] == 0, dS != dutyPrevS[i], false);
        acumular(horS[i], dt, nivS[i], dS, dS > 0 && dutyPrevS[i] == 0, dS != dutyPrevS[i], false);
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
    acumular(ensaio, dt, nivel, novoDuty, partida, troca, true);
    acumular(hora, dt, nivel, novoDuty, partida, troca, true);
  }

  // ---- alertas reais ----
  char msg[260];
  if (failsafe && !failsafeAtivo) {
    bipsAgendar(3);
    alertar(2, "FALHA no sensor de temperatura/umidade. Ventilacao forcada em 100% (modo seguro).");
  } else if (!failsafe && failsafeAtivo) enfileirar(SAI_MSG, "Sensor de temperatura/umidade voltou a responder.");
  failsafeAtivo = failsafe;
  if (alarmeGasAgora && !gasAlarme) {
    bipsAgendar(3);
    snprintf(msg, sizeof(msg), "ALERTA de gases: IG = %.2f (limiar %.2f). Ventilacao em 100%%.", ig, IG_LIMIAR[1]);
    alertar(1, msg);
  } else if (!alarmeGasAgora && gasAlarme) enfileirar(SAI_MSG, "Indice relativo de gases voltou ao normal.");
  gasAlarme = alarmeGasAgora;
  const float *lim = limiaresAtivos();
  if (estado == EST_OPER && !isnan(itu) && itu >= lim[2]) {
    if (!ituCritAtivo) { ituCritAtivo = true; ituCritDesde = agora; }
    uint32_t dur = (agora - ituCritDesde) / 1000UL;
    if (dur >= ALERTA_ITU_S && !ituAvisou) {
      snprintf(msg, sizeof(msg), "ALERTA termico: ITU %.1f (>= %.0f) ha %lu min. Ventilacao em %u%%.", itu, lim[2], (unsigned long)(dur / 60), novoDuty);
      alertar(0, msg); ituAvisou = true; bipsAgendar(2);
    }
    if (dur >= LEMBRETE_ITU_S) { snprintf(msg, sizeof(msg), "Alerta termico persistente. ITU %.0f ha mais de %lu minutos.", itu, (unsigned long)(dur / 60)); alertar(0, msg); }
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
  static uint32_t ultRet = 0;
  if (estado == EST_OPER && millis() - ultRet >= 30000UL) { guardarRetomada(); ultRet = millis(); }
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

// dutyTeste >= 0 ignora o controle e aplica a razão cíclica pedida pelo
// monitor serial. Serve ao Ensaio 0(b): achar a menor que mantém o cooler
// girando. Volta ao controle normal com d-1 ou ao trocar de modo.
static int8_t dutyTeste = -1;
static void servicoVentilador() {
  if (dutyTeste >= 0) { fanEscrever((uint8_t)dutyTeste); return; }
  uint32_t a = millis();
  fanEscrever((dutyAlvo > 0 && (int32_t)(partidaAte - a) > 0) ? 100 : dutyAlvo);
}
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
  guardarRetomada();
  Serial.printf("# MODO -> %s\n", NOME_MODO[modo]);
  novoEnsaio(); bipsAgendar(m + 1);
}

// ============================ PAINEL WEB ====================================
// Duas vistas na mesma página: operação (padrão, só leitura) e técnico
// (estratégias, ensaios, métricas, calibração e rede).
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
#msg.erro{color:var(--al)}#msg.ok{color:var(--ac)}
.tec{display:none}.tec.ab{display:block}
/* Heurística 1: dado velho não pode se passar por atual. Passados 8 s sem
   resposta, as leituras esmaecem e a tarja de desconexão aparece. */
body.velho .num,body.velho dd,body.velho .steps div{opacity:.4}
#link{display:none;padding:11px 16px;background:var(--mut);color:#fff;font-size:14px;font-weight:600}
body.velho #link{display:block}
.idade{display:block;font-size:12px;font-weight:400;color:var(--mut)}
.ref{font-size:12px;color:var(--mut);font-weight:400}
button:disabled{opacity:.5;cursor:default}
.olho{width:auto;padding:8px 10px;font-size:13px}
</style></head><body><div class="sheet">

<header><div class="wm">BoviSense</div>
<div class="sts"><b id="ph">--:--</b> &nbsp; <span id="pw">rede</span> &nbsp; <span id="pt">Telegram</span>
<span class="idade" id="idade">conectando ao aparelho</span></div></header>

<div id="link" role="status">Sem resposta do aparelho. Os valores abaixo são os últimos recebidos e podem estar velhos.</div>
<div id="aviso" role="alert"></div>

<section>
<p class="k">Índice de temperatura e umidade</p>
<div class="read"><div class="num" id="itu">--</div>
<div class="state" id="stw"><b id="cat">Lendo sensores</b><span id="est">iniciando</span></div></div>
</section>

<section>
<dl><dt>Temperatura</dt><dd><span id="t">--</span><u>°C</u></dd>
<dt>Umidade relativa</dt><dd><span id="u">--</span><u>%</u></dd>
<dt>Gases, índice relativo</dt><dd><span id="g">--</span></dd></dl>
<p class="hint" id="gm"></p>
<p class="hint">O índice de gases compara o ar com o de referência medido na partida: 1,00 é igual à referência. Não é concentração em ppm.</p>
</section>

<section>
<p class="k" id="vl">Ventilação</p>
<div class="steps"><div id="n0">desligada</div><div id="n1">50%</div><div id="n2">75%</div><div id="n3">100%</div></div>
<p class="hint" id="porque"></p>
</section>

<section>
<p class="k">Últimos 10 minutos</p>
<canvas id="c" width="900" height="260"></canvas>
<p class="hint">Linha cheia: índice de temperatura e umidade, escala de 60 a 90. Área sombreada: ventilação. Tracejados: os três limiares.</p>
</section>

<button class="tog" id="tg" onclick="tec()" aria-expanded="false" aria-controls="box">Modo técnico &#9662;</button>

<div class="tec" id="box">
<section>
<p class="k">Estratégia de controle</p><div id="modos"></div>
<p class="hint">Trocar a estratégia inicia um ensaio novo. O modo demonstração usa limiares relativos ao ambiente e não serve para coletar dados.</p>
</section>

<section>
<p class="k">Ensaio</p>
<button onclick="perigo(this,'/novo','descartar o ensaio atual?')">Iniciar ensaio</button><a href="/csv"><button type="button">Baixar CSV</button></a>
<button class="al" onclick="acao('/simular',this)">Simular emergência</button><button onclick="acao('/parar',this)">Parar simulação</button>
<p class="hint">A simulação dura 60 s, envia mensagem ao Telegram, e suas amostras ficam marcadas e fora das métricas.</p>
<table id="tab"></table>
<p class="hint">As três últimas linhas aplicam as três estratégias às mesmas leituras, mas só a estratégia ativa aciona o ventilador e altera o ambiente. Elas comparam a <b>decisão</b>, não o conforto nem a energia medida: para isso são os ensaios reais, um por estratégia.</p>
</section>

<section>
<p class="k">Calibração</p>
<p class="hint" style="margin-top:0">Deixe o DHT11 cinco minutos ao lado de um termo-higrômetro e informe os valores da referência.</p>
<label>Temperatura de referência (°C) <span class="ref" id="rt"></span><input id="tr" type="number" step="0.1" min="0" max="50" inputmode="decimal"></label>
<label>Umidade de referência (%) <span class="ref" id="ru"></span><input id="ur" type="number" step="1" min="5" max="100" inputmode="numeric"></label><br>
<button class="on" onclick="cal(this)">Salvar calibração</button><button onclick="perigo(this,'/cal/zerar','apagar a calibração?')">Zerar</button>
<button onclick="acao('/cal/gas',this)">Refazer base do MQ-2</button>
<p class="hint" id="calinfo"></p>
</section>

<section>
<p class="k">Rede e Telegram</p>
<p id="wst" style="margin:0 0 4px;font-weight:600">—</p>
<p id="tgst" class="hint" style="margin:0 0 10px"></p>
<button onclick="redes(this)">Procurar redes</button><br>
<label>Rede<select id="ssid"><option value="">procure as redes primeiro</option></select></label>
<label>Senha<input id="pw2" type="password" autocomplete="off"></label><button type="button" class="olho" onclick="olho(this)">mostrar</button><br>
<button class="on" onclick="conectar(this)">Conectar</button><button onclick="perigo(this,'/wifi/esquecer','esquecer a rede?')">Esquecer</button>
<p class="hint">Redes de 5 GHz não aparecem: o ESP32 não as enxerga.</p>
<label>Token do bot<input id="tk" size="24" style="width:230px" autocomplete="off"></label>
<label>chat_id<input id="ci" inputmode="numeric" pattern="-?[0-9]*" autocomplete="off"></label><br>
<button class="on" onclick="salvarTg(this)">Salvar Telegram</button>
<p class="hint">O chat_id não é o telefone nem o número do início do token. Deixe vazio e mande qualquer mensagem ao bot pelo seu Telegram: ele se vincula ao seu chat sozinho. Estes campos só aceitam alterações de quem está na rede BoviSense.</p>
</section>
</div>

<p id="msg" style="padding:0 16px 20px"></p>
</div><script>
var M=['Contínua','Liga-desliga','Adaptativa','Demonstração'];
var CV=['var(--c0)','var(--c1)','var(--c2)','var(--c3)'];
var modo=-1,aberto=false,ultOk=0,confirmando=null,tMsg=0;
function $(i){return document.getElementById(i)}
function f(x,d){if(x===null||x===undefined||isNaN(x))return'--';
return Number(x).toFixed(d===undefined?1:d).replace('.',',')}
function tec(){aberto=!aberto;$('box').className='tec'+(aberto?' ab':'');
$('tg').innerHTML='Modo técnico '+(aberto?'&#9652;':'&#9662;');
$('tg').setAttribute('aria-expanded',aberto?'true':'false');
try{localStorage.setItem('bs_tec',aberto?'1':'')}catch(e){}}
try{if(localStorage.getItem('bs_tec'))tec()}catch(e){}

// H9: a mensagem distingue sucesso de falha pela cor e some sozinha, para que
// um aviso velho não seja lido como estado atual.
function diz(t,erro){var m=$('msg');m.textContent=t;m.className=erro?'erro':'ok';tMsg=t?Date.now():0}
setInterval(function(){if(tMsg&&Date.now()-tMsg>6000)diz('',false)},1000);

// H1: enquanto a requisição não volta, o botão fica inativo e o painel diz que
// está enviando, em vez de parecer que o toque não funcionou.
function acao(u,bt){
if(bt)bt.disabled=true;diz('Enviando...',false);
fetch(u).then(function(r){return r.text()}).then(function(t){diz(t,false);atual()})
.catch(function(){diz('Sem resposta do aparelho. Confira se você continua na rede BoviSense.',true)})
.then(function(){if(bt)bt.disabled=false})}

// H3 e H5: ação destrutiva não acontece no primeiro toque. O próprio botão
// vira a pergunta, e um clique em qualquer outro lugar desiste.
function limparConfirma(){if(!confirmando)return;
confirmando.textContent=confirmando.getAttribute('data-rot');
confirmando.classList.remove('al');confirmando=null}
function perigo(bt,u,aviso){
if(confirmando===bt){var r=bt.getAttribute('data-rot');limparConfirma();bt.textContent=r;acao(u,bt);return}
limparConfirma();confirmando=bt;
if(!bt.getAttribute('data-rot'))bt.setAttribute('data-rot',bt.textContent);
bt.textContent='Confirmar: '+aviso;bt.classList.add('al');
diz('Toque de novo para confirmar, ou em qualquer outro ponto para desistir.',false)}
document.addEventListener('click',function(ev){if(confirmando&&ev.target!==confirmando)limparConfirma()},true);

function olho(bt){var c=$('pw2');var v=c.type==='password';
c.type=v?'text':'password';bt.textContent=v?'ocultar':'mostrar'}

// H5: a faixa é conferida aqui, antes de ir ao aparelho, para o usuário não
// descobrir o limite pela recusa.
function cal(bt){var t=parseFloat($('tr').value),u=parseFloat($('ur').value);
if(isNaN(t)||isNaN(u)){diz('Informe os dois valores do termo-higrômetro de referência.',true);return}
if(t<0||t>50){diz('A temperatura de referência precisa estar entre 0 e 50 °C.',true);return}
if(u<5||u>100){diz('A umidade de referência precisa estar entre 5 e 100 %.',true);return}
acao('/cal/th?t='+t+'&u='+u,bt)}

function redes(bt){if(bt)bt.disabled=true;diz('Procurando redes...',false);
fetch('/redes').then(function(r){return r.json()}).then(function(l){
$('ssid').innerHTML=l.length?l.map(function(n){return'<option>'+n.s+'</option>'}).join('')
:'<option value="">nenhuma rede de 2,4 GHz encontrada</option>';
diz(l.length?l.length+' redes encontradas.':'Nenhuma rede encontrada. Lembre que o ESP32 não enxerga 5 GHz.',!l.length)})
.catch(function(){diz('Falha ao procurar redes.',true)})
.then(function(){if(bt)bt.disabled=false})}

function conectar(bt){if(!$('ssid').value){diz('Escolha uma rede antes de conectar.',true);return}
acao('/wifi?s='+encodeURIComponent($('ssid').value)+'&p='+encodeURIComponent($('pw2').value),bt)}

function salvarTg(bt){var c=$('ci').value.trim();
if(c&&!/^-?[0-9]+$/.test(c)){diz('O chat_id é só números. Deixe vazio e mande qualquer mensagem ao bot pelo seu Telegram: ele se vincula sozinho.',true);return}
if(!$('tk').value.trim()&&!c){diz('Preencha ao menos o token do bot.',true);return}
acao('/tg?t='+encodeURIComponent($('tk').value.trim())+'&c='+encodeURIComponent(c),bt)}

// Trocar de estratégia descarta o ensaio em curso, então entra como destrutiva.
function botoes(){$('modos').innerHTML=M.map(function(n,i){
return'<button class="'+(i==modo?'on':'')+'" data-rot="'+n+'" onclick="perigo(this,\'/modo?m='+i
+'\',\'trocar para '+n+' e comecar outro ensaio?\')">'+n+'</button>'}).join('')}
// H1: a idade do último dado fica à vista. Passados 8 s sem resposta o painel
// esmaece as leituras e abre a tarja, para ninguém decidir por número velho.
var LIM=[72,75,79];
function frescor(){var s=ultOk?Math.round((Date.now()-ultOk)/1000):-1,velho=(s<0||s>8);
document.body.classList.toggle('velho',velho);
$('idade').textContent=s<0?'ainda sem dados do aparelho':(velho?'sem atualizar há '+s+' s':'atualizado agora')}
setInterval(frescor,1000);

// H10: o painel explica a decisão em vez de só mostrar o nível, que foi o que
// mais gerou dúvida: ventilador em 100 % com o ambiente parecendo ameno.
function explica(d){
if(d.catn>3)return'Sem leitura válida do DHT11: por segurança a ventilação vai a 100 % até as leituras voltarem.';
if(d.nivel===0)return'ITU abaixo de 72: ambiente em conforto térmico, ventilação desligada.';
return'Pelo índice, para descer um nível o ITU precisa ficar abaixo de '+(LIM[d.nivel-1]-2)
+' por 60 s seguidos. A histerese existe para o ventilador não ficar ligando e desligando perto do limiar.'}

function atual(){fetch('/api').then(function(r){return r.json()}).then(function(d){
ultOk=Date.now();frescor();
$('itu').textContent=f(d.ITU);$('t').textContent=f(d.T);$('u').textContent=f(d.UR,0);$('g').textContent=f(d.IG,2);
$('cat').textContent=d.cat;$('est').textContent=d.estado;$('porque').textContent=explica(d);
$('rt').textContent=isNaN(d.T)?'':'(o sensor lê '+f(d.T)+' °C agora)';
$('ru').textContent=isNaN(d.UR)?'':'(o sensor lê '+f(d.UR,0)+' % agora)';
$('gm').textContent=d.gmot?'Índice de gás indisponível — '+d.gmot+'.':'';
$('stw').style.borderLeftColor=d.catn<4?CV[d.catn]:'var(--mut)';
$('vl').textContent=d.rec?'Ventilação recomendada':'Ventilação';
for(var i=0;i<4;i++)$('n'+i).className=(i==d.nivel?'on':'');
$('ph').textContent=d.hora;$('pw').textContent=d.wifi?'rede ✓':'rede ✗';
$('pt').textContent=d.tg?'Telegram ✓':'Telegram ✗';$('wst').textContent=d.wst;
$('tgst').textContent=(d.tg?'Telegram entregando normalmente.':'Telegram sem entregar: '+(d.tgmot||'não configurado')+'.')+(d.tgbot?' Bot: @'+d.tgbot+'.':'');
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
}).catch(function(){$('est').textContent='sem conexão com o aparelho';frescor()})}
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
s?x.lineTo(X(k),Y(v)):x.moveTo(X(k),Y(v));s=1});x.stroke()})
.catch(function(){})}
setInterval(atual,2500);setInterval(graf,10000);atual();graf();
</script></body></html>)HTML";

static const char *NOME_CAT_WEB[4] = {"Conforto térmico", "Estresse brando", "Atenção: próximo do crítico", "Estresse moderado"};
static void webApi() {
  static char b[2000]; char estTxt[80], hr[8] = "--:--", alerta[120] = "";
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
           "\"s\":[%.1f,%.1f,%.1f],\"cal\":{\"t\":%.1f,\"u\":%.1f,\"g1\":%.2f,\"g2\":%.2f},\"wst\":\"%s\",\"gmot\":\"%s\","
           "\"tgmot\":\"%s\",\"tgbot\":\"%s\"}",
           estTxt, modo, Tf, Uf, itu, ig, (unsigned long)mqmV, nivel, dutyAlvo, SEM_ATUADOR ? "true" : "false",
           c < 4 ? NOME_CAT_WEB[c] : "Sem leitura", c < 4 ? c : 9, alerta, wifiOk ? "true" : "false", tgOk() ? "true" : "false", hr,
           ensaio.t_total_s / 60.0, ensaio.t_itu_acima_s / 60.0, ensaio.carga_itu_min, ensaio.t_eq_s / 60.0, ensaio.energia_wh,
           (unsigned long)ensaio.n_partidas, (unsigned long)ensaio.n_trocas,
           ensS[0].t_eq_s / 60.0, ensS[1].t_eq_s / 60.0, ensS[2].t_eq_s / 60.0, calOffT, calOffU, IG_LIMIAR[0], IG_LIMIAR[1], rede, isnan(ig) ? gasMotivo : "", tgMotivo(), botUser);
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

// ---- controle de acesso -----------------------------------------------------
// Leitura (/, /api, /hist, /csv) é livre. Tudo que muda estado exige cliente
// em 192.168.4.x, a rede do próprio aparelho: na rede da fazenda qualquer um
// dispararia uma emergência simulada ou trocaria a estratégia num ensaio.
static bool clienteLocal() { IPAddress ip = servidor.client().remoteIP(); return ip[0] == 192 && ip[1] == 168 && ip[2] == 4; }
static bool recusaRemoto() {
  if (clienteLocal()) return false;
  Serial.printf("# ACESSO RECUSADO: %s pediu %s\n",
                servidor.client().remoteIP().toString().c_str(), servidor.uri().c_str());
  servidor.send(403, "text/plain; charset=utf-8",
                "Recusado: conecte o celular à rede do aparelho para alterar qualquer coisa.");
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
  String tok = servidor.arg("t"), chat = servidor.arg("c");
  for (unsigned i = 0; i < chat.length(); i++)
    if (!isdigit((unsigned char)chat[i]) && !(chat[i] == '-' && i == 0)) {
      servidor.send(200, "text/plain; charset=utf-8", "O chat_id é só números (não é o telefone). Deixe vazio e mande qualquer mensagem ao bot: ele se vincula sozinho."); return;
    }
  salvarTelegram(tok, chat);
  tgUltimoCod = 0;         // não manter no painel o erro da credencial antiga
  tgReapresentar = true;
  servidor.send(200, "text/plain; charset=utf-8", "Telegram salvo na memória do ESP32.");
}

// ============================ TELEGRAM (núcleo 0) ===========================
// Cada sessão TLS ocupa cerca de 40 kB de memória. O HTTPClient mantém a
// conexão aberta após end() para reaproveitá-la, e uma resposta enviada de
// dentro da leitura de mensagens abria uma segunda sessão com a primeira ainda
// viva: faltava memória e o envio voltava -1. setReuse(false) fecha cada pedido.
static int tgPost(const char *metodo, const String &corpo, String *resp = nullptr) {
  String url = String("https://api.telegram.org/bot") + r0Token + "/" + metodo;
  int cod = -1;
  for (int tent = 0; tent < 2; tent++) {
    WiFiClientSecure cli; cli.setInsecure();   // limitação discutida no artigo
    HTTPClient http; http.setTimeout(10000); http.setReuse(false);
    if (!http.begin(cli, url)) return -1;
    http.addHeader("Content-Type", "application/json");
    cod = http.POST(corpo);
    if (resp && cod > 0) *resp = http.getString();
    if (cod != 200) {
      String r = cod > 0 ? (resp ? *resp : http.getString()) : String("sem resposta (DNS/TLS/internet)");
      Serial.printf("# TELEGRAM %s: erro HTTP %d -> %s\n", metodo, cod, r.substring(0, 160).c_str());
      if (cod < 0) Serial.printf("#   memoria livre %u B, maior bloco %u B\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      if (cod == 401 || cod == 404) Serial.println(F("# TELEGRAM: token invalido ou revogado (TG_TOKEN)"));
    }
    http.end(); cli.stop();
    // -1 é falha ao conectar: nada chegou ao Telegram, então repetir não duplica mensagem.
    if (cod != HTTPC_ERROR_CONNECTION_REFUSED) break;
    vTaskDelay(pdMS_TO_TICKS(700));
  }
  return cod;
}
static int tgEnviarCod(const char *chat, const char *texto) {
  JsonDocument doc; doc["chat_id"] = chat; doc["text"] = texto;
  String corpo; serializeJson(doc, corpo);
  return tgPost("sendMessage", corpo);
}
static bool tgEnviarPara(const char *chat, const char *texto) { return tgEnviarCod(chat, texto) == 200; }
// Só o envio ao chat configurado alimenta o indicador do painel: responder a
// outro chat com sucesso não prova que os alertas estão chegando.
static bool tgEnviar(const char *texto) {
  int cod = tgEnviarCod(r0Chat.c_str(), texto);
  tgUltimoCod = cod;
  if (cod == 400 || cod == 403) {
    tgChatRecusado = true;
    Serial.printf("# TELEGRAM: o Telegram recusou o chat_id %s. Mande qualquer mensagem ao bot pelo seu Telegram: ele se vincula ao seu chat.\n", r0Chat.c_str());
  } else if (cod == 200) tgChatRecusado = false;
  return cod == 200;
}
// Descobre o @ do bot para dizer, no monitor e no painel, com quem conversar.
static void tgQuemSou() {
  String r; int cod = tgPost("getMe", "{}", &r);
  if (cod != 200) { tgUltimoCod = cod; return; }
  JsonDocument d;
  if (deserializeJson(d, r)) return;
  const char *u = d["result"]["username"] | "";
  if (!*u) return;
  snprintf(botUser, sizeof(botUser), "%s", u);
  Serial.printf("# TELEGRAM: token aceito. O bot e @%s: no celular, abra t.me/%s e toque em INICIAR\n", u, u);
}
static void tgMenu() {
  tgPost("setMyCommands", F("{\"commands\":["
    "{\"command\":\"status\",\"description\":\"Leituras atuais\"},"
    "{\"command\":\"relatorio\",\"description\":\"Metricas da hora e do ensaio\"},"
    "{\"command\":\"simular\",\"description\":\"Simular emergencia (apresentacao)\"},"
    "{\"command\":\"parar\",\"description\":\"Encerrar a simulacao\"},"
    "{\"command\":\"modo\",\"description\":\"Estrategia: /modo 0, 1, 2 ou 3\"},"
    "{\"command\":\"novoensaio\",\"description\":\"Iniciar novo ensaio\"},"
    "{\"command\":\"calibrar_gas\",\"description\":\"Recalibrar o MQ-2 em ar limpo\"},"
    "{\"command\":\"ajuda\",\"description\":\"Lista de comandos\"}]}"));
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
  } else {
    tgEnviar("BoviSense - comandos:\n/status - leituras atuais\n/relatorio - metricas\n/simular - emergencia de teste (60 s)\n"
             "/parar - encerra a simulacao\n/modo N - estrategia (0 continua, 1 liga-desliga, 2 adaptativa, 3 demonstracao)\n"
             "/novoensaio - zera as metricas\n/calibrar_gas - linha de base do MQ-2");
  }
}
// Último chat vinculado automaticamente, para não vincular de novo a cada
// mensagem enquanto o núcleo 1 ainda não gravou. Só o núcleo 0 usa.
static char chatAdotado[24] = "";
static void tgConsultar(int64_t &offset) {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http; http.setTimeout(10000); http.setReuse(false);
  char url[220];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?timeout=0&limit=5&offset=%lld", r0Token.c_str(), (long long)offset);
  if (!http.begin(cli, url)) return;
  int cod = http.GET();
  static int codAnt = 200;                   // imprime cada tipo de falha uma vez, não a cada 4 s
  if (cod != 200) {
    http.end();
    if (cod == 401 || cod == 404) tgUltimoCod = cod;
    if (cod != codAnt) {
      if (cod == 401 || cod == 404) Serial.println(F("# TELEGRAM: token invalido ou revogado (TG_TOKEN)"));
      else if (cod == 409) Serial.println(F("# TELEGRAM: o bot tem webhook ativo, que impede ler mensagens. Removendo o webhook..."));
      else Serial.printf("# TELEGRAM: leitura de mensagens falhou (HTTP %d)\n", cod);
    }
    if (cod == 409) tgPost("deleteWebhook", "{}");
    codAnt = cod;
    return;
  }
  codAnt = 200;
  String resp = http.getString(); http.end();
  cli.stop();                                // fecha a sessão antes de responder: duas não cabem juntas
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return;
  time_t agora = time(nullptr);
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    offset = (u["update_id"] | (int64_t)0) + 1;
    int64_t chat = u["message"]["chat"]["id"] | (int64_t)0;
    const char *texto = u["message"]["text"] | "";
    long data = u["message"]["date"] | 0L;
    if (!chat) continue;
    char chatStr[24]; snprintf(chatStr, sizeof(chatStr), "%lld", (long long)chat);
    char m[260];
    if (r0Chat != chatStr) {
      if (!r0ChatOk()) {
        // Nenhum chat utilizável: vazio, número do próprio bot, ou recusado
        // pelo Telegram. Quem escreve ao bot passa a receber os alertas, sem
        // regravar o firmware. Só acontece enquanto não há chat que funcione.
        if (strcmp(chatAdotado, chatStr)) {
          strlcpy(chatAdotado, chatStr, sizeof(chatAdotado));
          strlcpy(chatAdotar, chatStr, sizeof(chatAdotar));
          enviarCmd(CMD_ADOTAR_CHAT, 0);
          snprintf(m, sizeof(m), "Pronto! Este chat foi vinculado ao BoviSense (chat_id %s).\nOs alertas e as respostas aos comandos chegam aqui.\nToque em / para ver os comandos.", chatStr);
          tgEnviarPara(chatStr, m);
          Serial.printf("# TELEGRAM: chat %s VINCULADO automaticamente (era '%s', inutilizavel)\n", chatStr, r0Chat.c_str());
        }
        continue;
      }
      // Já há um chat que funciona: a outro chat, só informa o número.
      snprintf(m, sizeof(m), "Este chat nao esta autorizado no BoviSense.\nSeu chat_id e: %s\nPara receber os alertas aqui, cole esse numero no painel (cartao Rede e Telegram).", chatStr);
      tgEnviarPara(chatStr, m);
      Serial.printf("# TELEGRAM: mensagem do chat %s (nao autorizado; o vinculado e %s)\n", chatStr, r0Chat.c_str());
      continue;
    }
    // Mensagem do chat configurado: a conversa existe, então o envio volta a valer.
    if (tgChatRecusado) { tgChatRecusado = false; tgReapresentar = true; }
    // Comando enviado antes desta inicialização não é executado: um /simular
    // antigo dispararia a emergência ao ligar o aparelho, e uma reinicialização
    // entre a resposta e a confirmação da leitura repetiria a resposta.
    if (texto[0] != '/') continue;
    long inicio = agora > 1700000000 ? (long)agora - (long)(millis() / 1000UL) : 0;
    if (inicio && data > 0 && data < inicio - 5) {
      Serial.printf("# TELEGRAM: comando anterior a esta inicializacao ignorado: %.30s\n", texto);
      continue;
    }
    if (!inicio && data > 0 && agora > 1700000000 && (long)(agora - data) > 120) continue;
    // O mesmo comando de novo em menos de 20 s é quase sempre um toque repetido
    // enquanto o bot ainda não tinha respondido o primeiro.
    static char ultCmd[24] = ""; static long ultCmdData = 0;
    if (!strncmp(ultCmd, texto, sizeof(ultCmd) - 1) && data > 0 && data - ultCmdData < 20) {
      Serial.printf("# TELEGRAM: comando repetido ignorado: %.30s\n", texto);
      continue;
    }
    strlcpy(ultCmd, texto, sizeof(ultCmd)); ultCmdData = data;
    Serial.printf("# TELEGRAM: comando recebido: %.30s\n", texto);
    tgResponder(texto);
  }
}
static void tarefaRede(void *) {
  sincronizarCredenciais();
  bool ntp = false, antes = false, saudou = false, menu = false; uint32_t ultPoll = 0, ultTent = 0, desdeCon = 0; int64_t offset = 0;
  uint32_t proxSaudacao = 0;
  if (r0Ssid.length()) { WiFi.begin(r0Ssid.c_str(), r0Senha.c_str()); ultTent = millis(); }
  else Serial.println(F("# Wi-Fi: sem rede escolhida -> so painel local. Escolha a rede no painel."));
  for (;;) {
    sincronizarCredenciais();
    bool con = r0Ssid.length() && WiFi.status() == WL_CONNECTED;
    wifiOk = con;
    if (con && !antes) {
      desdeCon = millis();
      snprintf(wifiDiag, sizeof(wifiDiag), "conectado");
      Serial.printf("# Wi-Fi: CONECTADO a '%s' | IP %s | sinal %d dBm\n", r0Ssid.c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (!ntp) {
        // Arduino-ESP32 3.3.12: na primeira consulta, NetworkManager::hostByName
        // chama dns_clear_cache() sem a trava do lwIP. Se o SNTP estiver
        // resolvendo o servidor de hora nesse instante, o retorno dele roda
        // fora da trava, abre um socket UDP e o ESP32 aborta (assert em
        // udp_new_ip_type, registro de 16/11, pilha decodificada até
        // tgPost). Fazer essa primeira consulta antes de ligar o SNTP fecha
        // a janela: depois dela o cache só volta a ser limpo se todas as
        // interfaces perderem o IP.
        IPAddress ipTg; int rDns = WiFi.hostByName("api.telegram.org", ipTg);
        Serial.printf("# DNS: api.telegram.org -> %s\n", rDns == 1 ? ipTg.toString().c_str() : "falhou (segue assim mesmo)");
        configTzTime("<-03>3", "a.st1.ntp.br", "pool.ntp.org"); ntp = true;
      }
    }
    // Fora do bloco de transição: o token quase sempre é colado no painel com o
    // Wi-Fi já conectado, e ali não há transição nenhuma para disparar o envio.
    if (tgReapresentar) { tgReapresentar = false; saudou = false; menu = false; proxSaudacao = 0; tgChatRecusado = false; chatAdotado[0] = 0; }
    if (con && r0TokenOk() && !menu) { tgQuemSou(); tgMenu(); menu = true; }
    bool tokenRecusado = tgUltimoCod == 401 || tgUltimoCod == 404;   // só muda ao trocar a credencial
    if (con && r0Ativo() && !saudou && !tokenRecusado && (int32_t)(millis() - proxSaudacao) >= 0) {
      proxSaudacao = millis() + 15000;   // sem isto, uma falha repetiria a cada 50 ms
      char m[260];
      snprintf(m, sizeof(m), "BoviSense ligado e conectado a internet (IP %s).\nAquecendo o MQ-2: o controle comeca em cerca de 4 min.\n"
               "Painel local: rede 'BoviSense' -> http://192.168.4.1\nToque em / para ver os comandos.", WiFi.localIP().toString().c_str());
      saudou = tgEnviar(m);
      // -4, -5 e -11: o pedido saiu e a resposta se perdeu. A mensagem
      // provavelmente chegou; reenviar a cada 15 s duplicaria a saudação.
      if (!saudou && (tgUltimoCod == HTTPC_ERROR_NOT_CONNECTED || tgUltimoCod == HTTPC_ERROR_CONNECTION_LOST ||
                      tgUltimoCod == HTTPC_ERROR_READ_TIMEOUT)) saudou = true;
      Serial.println(saudou ? F("# TELEGRAM: mensagem de inicializacao ENVIADA") : F("# TELEGRAM: mensagem de inicializacao FALHOU (veja o erro acima)"));
    }
    if (!con && antes) Serial.println(F("# Wi-Fi: conexao perdida"));
    antes = con;
    if (r0Ssid.length() && !con && millis() - ultTent > 30000) {
      Serial.printf("# Wi-Fi: sem conexao com '%s' | motivo %u: %s\n", r0Ssid.c_str(), wifiMotivo, textoMotivo(wifiMotivo));
      snprintf(wifiDiag, sizeof(wifiDiag), "falha %u", wifiMotivo);
      WiFi.disconnect(false, false); delay(100); WiFi.begin(r0Ssid.c_str(), r0Senha.c_str()); ultTent = millis();
    }
    if (pedidoReconectar) {
      pedidoReconectar = false; saudou = false; menu = false; proxSaudacao = 0; wifiMotivo = 0;
      WiFi.disconnect(false, false); vTaskDelay(pdMS_TO_TICKS(300));
      snprintf(wifiDiag, sizeof(wifiDiag), "conectando...");
      if (r0Ssid.length()) { WiFi.begin(r0Ssid.c_str(), r0Senha.c_str()); ultTent = millis(); Serial.printf("# Wi-Fi: conectando a '%s' (escolhida no painel)\n", r0Ssid.c_str()); }
    }
    MsgSaida m;
    if (con && xQueueReceive(filaSaida, &m, 0) == pdTRUE) {
      bool ok = tgEnviar(m.texto);
      Serial.printf("# REDE: mensagem %s\n", ok ? "enviada" : "FALHOU");
    }
    // Espera o relógio (NTP) para poder descartar comandos antigos; sem NTP, segue após 20 s.
    bool relogio = time(nullptr) > 1700000000 || millis() - desdeCon > 20000;
    if (con && r0TokenOk() && relogio && !tokenRecusado && millis() - ultPoll > TG_POLL_MS) { ultPoll = millis(); tgConsultar(offset); }
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
  if (c == 'm') { delay(20); definirModo(Serial.read() - '0'); dutyTeste = -1; }
  else if (c == 'd') {
    int v = Serial.parseInt();
    dutyTeste = (v >= 0 && v <= 100) ? (int8_t)v : -1;
    if (dutyTeste >= 0) Serial.printf("# TESTE: ventilador forcado em %d %% (d-1 volta ao controle)\n", dutyTeste);
    else Serial.println(F("# TESTE: encerrado, o controle voltou a decidir"));
  }
  else if (c == 'r') novoEnsaio();
  else if (c == 'p') { static char t[900]; textoMetricas(t, sizeof(t), ensaio, "Ensaio atual"); Serial.println(t); textoEstrategias(t, sizeof(t), ensS); Serial.println(t); }
  else if (c == 'k' && estado == EST_AQUEC) tInicioEstado = millis() - T_AQUEC_S * 1000UL;
  else if (c == 's') iniciarSimulacao("monitor serial");
  else if (c == 'x') pararSimulacao();
  else if (c == 'h') Serial.println(F("# comandos: m0..m3 modo | d0..d100 forca o ventilador (d-1 solta) | r novo ensaio | p metricas | k pular aquecimento | s simular | x parar"));
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
  if (!tgTokenOk()) Serial.println(F("# Telegram: desativado (TG_TOKEN vazio)"));
  else if (ehIdDoBot(tgChat, tgToken))
    Serial.printf("# Telegram: o chat_id %s e o numero do PROPRIO BOT (inicio do token), nao o seu.\n"
                  "#   Nao precisa regravar: mande qualquer mensagem ao bot pelo seu Telegram e ele se vincula ao seu chat.\n", tgChat.c_str());
  else if (!chatIdValido())
    Serial.println(F("# Telegram: nenhum chat_id. Mande qualquer mensagem ao bot pelo seu Telegram e ele se vincula ao seu chat."));
  else Serial.printf("# Telegram: token e chat_id %s com formato valido (a entrega e confirmada apos o Wi-Fi)\n", tgChat.c_str());
  Serial.printf("# Procedencia: rede=%s | token=%s | chat_id=%s\n",
                credDoArquivo[0] ? "segredos.h" : "painel/NVS",
                credDoArquivo[1] ? "segredos.h" : "painel/NVS",
                credDoArquivo[2] ? "segredos.h" : "painel/NVS");
  if (credTrocada) Serial.printf("# segredos.h alterado desde a ultima gravacao: substituiu o que estava salvo pelo painel em%s%s%s\n",
                                 credTrocada & 1 ? " rede" : "", credTrocada & 2 ? " token" : "", credTrocada & 4 ? " chat_id" : "");
  if (tgTokenOk()) {
    int p = tgToken.indexOf(':');
    Serial.printf("# Token em uso: bot %s (o numero antes dos dois-pontos)\n", p > 0 ? tgToken.substring(0, p).c_str() : "?? (sem dois-pontos: token mal colado)");
  }
  Serial.println(F("# Ventilador fica DESLIGADO no aquecimento (180 s) e na linha de base (60 s)."));
  Serial.println(F("# BOOT: toque = proximo modo (ou pula aquecimento) | segurar 3 s = simular emergencia"));
  Serial.println(F("# Sem display: acompanhe pelo painel web, pelo Telegram e por este monitor."));
  Serial.println(F("# ========================================================"));
  if (!dhtOk) for (int i = 0; i < 3; i++) { buzzerTom(2200); delay(120); buzzerTom(0); delay(150); }
  if (!mqOk)  for (int i = 0; i < 2; i++) { buzzerTom(700);  delay(700); buzzerTom(0); delay(250); }
  if (dhtOk && mqOk) { buzzerTom(1800); delay(90); buzzerTom(0); delay(70); buzzerTom(2600); delay(140); buzzerTom(0); }

  mtx = xSemaphoreCreateMutex();
  mtxCred = xSemaphoreCreateMutex();
  filaSaida = xQueueCreate(12, sizeof(MsgSaida));
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
  servidor.on("/modo", []() { if (recusaRemoto()) return; definirModo(servidor.arg("m").toInt()); servidor.send(200, "text/plain; charset=utf-8", "Estratégia alterada; novo ensaio iniciado."); });
  servidor.on("/novo", []() { if (recusaRemoto()) return; novoEnsaio(); servidor.send(200, "text/plain; charset=utf-8", "Novo ensaio iniciado."); });
  servidor.on("/simular", []() { if (recusaRemoto()) return; iniciarSimulacao("painel web"); servidor.send(200, "text/plain; charset=utf-8", "Simulação iniciada: mensagem enviada ao Telegram."); });
  servidor.on("/parar", []() { if (recusaRemoto()) return; pararSimulacao(); servidor.send(200, "text/plain; charset=utf-8", "Simulação encerrada."); });
  servidor.on("/cal/th", []() { if (recusaRemoto()) return; char m[160]; calibrarTU(servidor.arg("t").toFloat(), servidor.arg("u").toFloat(), m, sizeof(m)); servidor.send(200, "text/plain; charset=utf-8", m); });
  servidor.on("/cal/zerar", []() { if (recusaRemoto()) return; zerarCalibracao(); servidor.send(200, "text/plain; charset=utf-8", "Calibração do DHT11 zerada."); });
  servidor.on("/redes", webRedes);
  servidor.on("/wifi", webWifi);
  servidor.on("/wifi/esquecer", webEsquecer);
  servidor.on("/tg", webTg);
  servidor.on("/cal/gas", []() {
    if (recusaRemoto()) return;
    if (estado != EST_OPER) { servidor.send(200, "text/plain; charset=utf-8", "Aguarde o fim do aquecimento."); return; }
    iniciarRecalGas(); servidor.send(200, "text/plain; charset=utf-8", "Recalibrando o MQ-2 por 60 s: mantenha o ar limpo."); });
  servidor.begin();
  xTaskCreatePinnedToCore(tarefaRede, "rede", 16384, nullptr, 1, nullptr, 0);

  Serial.printf("# Painel: conecte-se a rede '%s' e abra http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  char motivoRst[48];
  if (tentarRetomada(motivoRst, sizeof(motivoRst))) {
    Serial.printf("# RETOMADA: reinicio inesperado (%s). Estrategia %s e linha de base do MQ-2 restauradas;\n"
                  "#   controle ativo sem repetir o aquecimento (o MQ-2 continuou alimentado).\n", motivoRst, NOME_MODO[modo]);
    char m[220];
    snprintf(m, sizeof(m), "BoviSense reiniciou sozinho (%s) e retomou o controle em segundos, na estrategia %s, sem repetir o aquecimento.",
             motivoRst, NOME_MODO[modo]);
    enfileirar(SAI_MSG, m);
    imprimirCabecalhoCSV();
  } else {
    imprimirCabecalhoCSV();
    iniciarEstado(EST_AQUEC);
  }
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
    else if (c.tipo == CMD_ADOTAR_CHAT) salvarChat(String(chatAdotar));
  }
  servicoVentilador(); servicoBotao(); servicoSerial(); bipServico();
  servidor.handleClient();
  delay(2);
}
