/*
 * TDE - Conectividade em Sistemas Ciberfisicos (PUCPR)
 * SATURN - Plant Keeper: ESP32 operando como SERVIDOR HTTP (API REST) com LOG em arquivo
 *
 * Rotas (todas com HTTP Basic, exceto "/"):
 *   GET /               -> teste (sem login)
 *   GET /status         -> leituras dos sensores + estado da bomba (JSON)
 *   GET /ligar          -> liga a bomba   (retorna o estado em JSON)
 *   GET /desligar       -> desliga a bomba (retorna o estado em JSON)
 *   GET /info           -> IP, MAC, Wi-Fi, hora, uptime e uso do log (JSON)
 *   GET /log            -> log do sistema em texto. Parametros opcionais:
 *                            ?nivel=ERRO | AVISO | INFO   (filtra por nivel)
 *                            &n=200                       (ultimas N linhas, max. 1000)
 *   GET /log/limpar     -> apaga o log
 *
 * Formato de cada linha do log (arquivo /log.txt na flash, via LittleFS):
 *   2026-11-10 14:32:05|INFO|Bomba LIGADA (comando web 192.168.43.20)
 * Enquanto a hora nao foi sincronizada por NTP, a linha usa "boot+123s" (segundos desde o boot).
 *
 * Bibliotecas: DHT sensor library (Adafruit). LittleFS, WiFi e WebServer ja vem com o ESP32.
 * Particao: use o esquema padrao da placa ("Default 4MB with spiffs"), que reserva espaco para o LittleFS.
 */
#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <LittleFS.h>
#include <time.h>
#include <stdarg.h>
#include <esp_system.h>

// ----------- Configurações -----------
// Credenciais ficam em secrets.h (NÃO vai para o GitHub). Copie secrets.example.h para secrets.h.
// Define: WIFI_SSID, WIFI_SENHA, WEB_USUARIO, WEB_SENHA
#include "secrets.h"

const int PIN_RELE = 13;
const int PIN_DHT  = 26;
const int PIN_SOLO = 32;   // ADC1
const int PIN_LDR  = 34;   // ADC1 (somente entrada)
const int PIN_TRIG = 14;   // HC-SR04
const int PIN_ECHO = 33;   // HC-SR04 (usar divisor de tensão 5V -> 3,3V)
#define DHTTYPE DHT22

const bool RELE_ATIVO_LOW = true;                 // false se o seu relé for ativo em HIGH
const unsigned long TEMPO_MAX_BOMBA_MS = 60000;   // desliga sozinha após 60 s
const unsigned long INTERVALO_SENSORES_MS = 2500; // DHT22 aceita no máx. 1 leitura a cada 2 s

// Calibração do sensor de solo (valor ALTO = seco, BAIXO = molhado)
const int SOLO_SECO    = 3000;
const int SOLO_MOLHADO = 1300;

// Calibração do LDR (sensor INVERTIDO: escuro = valor ALTO, claro = valor BAIXO)
const int LDR_ESCURO = 3900;
const int LDR_CLARO  = 150;

// Calibração do reservatório (distância do sensor até a superfície da água)
const float DIST_VAZIO_CM = 18.0;   // reservatório vazio (ajuste!)
const float DIST_CHEIO_CM = 6.0;    // reservatório cheio (mín. ~4 cm)
const int   CAPACIDADE_ML = 700;    // capacidade entre vazio e cheio

// Proteção da bomba contra funcionar a seco
const bool  PROTEGER_BOMBA_NIVEL_BAIXO = true;
const int   NIVEL_MINIMO_BOMBA_PCT = 10;   // abaixo disso a bomba não liga

// ----------- Configurações do LOG e do horário -----------
const char* LOG_ARQ = "/log.txt";          // arquivo atual
const char* LOG_ANT = "/log_old.txt";      // arquivo anterior (rotação)
const size_t LOG_MAX_BYTES = 48 * 1024;    // ao passar disso, o arquivo atual vira o "old"
const unsigned long LOG_INTERVALO_SENSORES_MS = 300000;  // INFO periódico dos sensores: 5 min (use 30000 para testar)

const long GMT_OFFSET_S = -3 * 3600;       // Brasília (UTC-3)
const char* NTP_1 = "pool.ntp.org";
const char* NTP_2 = "time.google.com";

// Limites dos AVISOS (com histerese: só avisa ao entrar na condição e informa ao sair)
const int   AVISO_RESERV_PCT = 20;   // reservatório baixo abaixo disso (normaliza em +5)
const int   AVISO_SOLO_PCT   = 20;   // solo seco abaixo disso (normaliza em +5)
const float AVISO_TEMP_C     = 35.0; // temperatura alta acima disso (normaliza em -2)

// ----------- Variáveis -----------
WebServer server(80);
DHT dht(PIN_DHT, DHTTYPE);

bool bombaLigada = false;
unsigned long inicioBomba = 0;

float temperatura = NAN;
float umidade = NAN;
int soloBruto = 0;
int umidadeSolo = 0;
int ldrBruto = 0;
int luminosidade = 0;
float distanciaAgua = -1;   // cm (-1 = leitura inválida)
int nivelReservatorio = -1; // % (-1 = leitura inválida)
int volumeMl = -1;
unsigned long ultimaLeituraSensores = 0;

// Estado do log / horário / Wi-Fi
bool fsOk = false;
bool horaSincronizada = false;
bool ntpErroLogado = false;
bool wifiCaido = false;
unsigned long quedaWifiInicio = 0;
unsigned long ultimaTentativaWifi = 0;
volatile int motivoDesconexao = 0;       // preenchido pelo evento do Wi-Fi
unsigned long ultimoLogSensores = 0;
unsigned long ultimoAvisoAuth = 0;

// Controle de falhas e alertas (para registrar só quando o estado muda)
struct Falha { bool ativa = false; uint8_t cont = 0; };
Falha falhaDht, falhaSolo, falhaDist;
bool avisoReserv = false, avisoSolo = false, avisoTemp = false;

// ----------- LOG -----------
bool horaValida() { return time(nullptr) > 1700000000; }   // passou de nov/2023 = NTP já ajustou

void montarTimestamp(char* buf, size_t n) {
  if (horaValida()) {
    time_t agora = time(nullptr);
    struct tm t;
    localtime_r(&agora, &t);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &t);
  } else {
    snprintf(buf, n, "boot+%lus", millis() / 1000);
  }
}

void registrar(const char* nivel, const char* fmt, ...) {
  char msg[180];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  for (char* p = msg; *p; p++) if (*p == '|' || *p == '\n' || *p == '\r') *p = ' ';

  char ts[24];
  montarTimestamp(ts, sizeof(ts));
  Serial.printf("[%s] %s: %s\n", ts, nivel, msg);

  if (!fsOk) return;
  File f = LittleFS.open(LOG_ARQ, FILE_APPEND);
  if (!f) return;
  if (f.size() > LOG_MAX_BYTES) {          // rotação: guarda só o arquivo anterior
    f.close();
    LittleFS.remove(LOG_ANT);
    LittleFS.rename(LOG_ARQ, LOG_ANT);
    f = LittleFS.open(LOG_ARQ, FILE_APPEND);
    if (!f) return;
  }
  f.printf("%s|%s|%s\n", ts, nivel, msg);
  f.close();
}
#define LOG_INFO(...)  registrar("INFO",  __VA_ARGS__)
#define LOG_AVISO(...) registrar("AVISO", __VA_ARGS__)
#define LOG_ERRO(...)  registrar("ERRO",  __VA_ARGS__)

size_t tamanhoLog() {
  size_t total = 0;
  const char* arqs[2] = { LOG_ANT, LOG_ARQ };
  for (int i = 0; i < 2; i++) {
    File f = LittleFS.open(arqs[i], "r");
    if (f) { total += f.size(); f.close(); }
  }
  return total;
}

// Registra uma falha só depois de 'limite' leituras ruins seguidas, e informa quando normaliza
void reportarFalha(Falha& f, bool falhou, uint8_t limite, const char* nome) {
  if (falhou) {
    if (f.cont < 255) f.cont++;
    if (!f.ativa && f.cont >= limite) {
      f.ativa = true;
      LOG_ERRO("Falha na leitura: %s", nome);
    }
  } else {
    f.cont = 0;
    if (f.ativa) {
      f.ativa = false;
      LOG_INFO("Leitura normalizada: %s", nome);
    }
  }
}

// Aviso com histerese: avisa ao entrar na condição e informa ao sair
void condicao(bool& ativa, bool entrar, bool sair, const char* texto, float valor) {
  if (!ativa && entrar) {
    ativa = true;
    LOG_AVISO("%s (%.1f)", texto, valor);
  } else if (ativa && sair) {
    ativa = false;
    LOG_INFO("Normalizado: %s (%.1f)", texto, valor);
  }
}

// ----------- Funções do sistema -----------
bool nivelBaixo() {
  return PROTEGER_BOMBA_NIVEL_BAIXO && nivelReservatorio >= 0 &&
         nivelReservatorio < NIVEL_MINIMO_BOMBA_PCT;
}

// Aciona o relé respeitando o nível lógico do ESP32 (3,3 V).
// Módulos ativos em LOW têm pull-up interno para 5 V, então um HIGH de 3,3 V
// NÃO desliga o relé. Solução: para desligar, o pino fica flutuando (INPUT)
// e o pull-up do módulo leva o IN a 5 V. Para ligar, o pino vai a LOW.
void acionarRele(bool ligar) {
  if (RELE_ATIVO_LOW) {
    if (ligar) {
      digitalWrite(PIN_RELE, LOW);   // define o nível antes de virar saída (sem pulso falso)
      pinMode(PIN_RELE, OUTPUT);
    } else {
      pinMode(PIN_RELE, INPUT);      // flutuando = relé desligado
    }
  } else {
    pinMode(PIN_RELE, OUTPUT);
    digitalWrite(PIN_RELE, ligar ? HIGH : LOW);
  }
}

void setBomba(bool ligar, const char* origem) {
  bool mudou = (ligar != bombaLigada);
  unsigned long duracaoS = (millis() - inicioBomba) / 1000;
  bombaLigada = ligar;
  acionarRele(ligar);
  if (ligar) inicioBomba = millis();
  if (mudou) {
    if (ligar) LOG_INFO("Bomba LIGADA (%s)", origem);
    else       LOG_INFO("Bomba DESLIGADA (%s), ficou ligada %lu s", origem, duracaoS);
  }
}

// IP do cliente que fez a requisição atual
String ipCliente() { return server.client().remoteIP().toString(); }

// CORS: permite que o cliente (index.html aberto no PC/celular) acesse a API do ESP32.
void enviarCabecalhosCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
}

// Autenticação HTTP Basic (o cliente envia o cabeçalho Authorization)
bool autenticado() {
  if (!server.authenticate(WEB_USUARIO, WEB_SENHA)) {
    // Limita o registro (o cliente consulta a cada 2 s e encheria o log)
    if (millis() - ultimoAvisoAuth > 30000 || ultimoAvisoAuth == 0) {
      ultimoAvisoAuth = millis();
      LOG_AVISO("Acesso negado em %s (login invalido) do IP %s", server.uri().c_str(), ipCliente().c_str());
    }
    enviarCabecalhosCors();
    server.send(401, "application/json", "{\"erro\":\"nao autorizado\"}");
    return false;
  }
  return true;
}

// ----------- Sensores -----------
// Média de 10 leituras analógicas para reduzir ruído
int lerAnalogicoMedio(int pino) {
  long soma = 0;
  for (int i = 0; i < 10; i++) {
    soma += analogRead(pino);
    delay(2);
  }
  return soma / 10;
}

// Uma leitura do HC-SR04 em cm, ou -1 se falhar
float lerDistanciaCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long duracao = pulseIn(PIN_ECHO, HIGH, 12000);  // timeout ~2 m
  if (duracao == 0) return -1;
  return duracao * 0.0343 / 2.0;
}

// Mediana de 7 leituras: ignora valores fora do padrão (ondas, gotas)
float lerDistanciaMediana() {
  float v[7];
  int n = 0;
  for (int i = 0; i < 7; i++) {
    float d = lerDistanciaCm();
    if (d > 0) v[n++] = d;
    delay(30);
  }
  if (n < 3) return -1;   // poucas leituras válidas

  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }

  return v[n / 2];
}

void lerReservatorio() {
  distanciaAgua = lerDistanciaMediana();
  reportarFalha(falhaDist, distanciaAgua < 0, 3, "sensor de nivel HC-SR04");

  if (distanciaAgua < 0) {
    nivelReservatorio = -1;
    volumeMl = -1;
    return;
  }

  float pct = (DIST_VAZIO_CM - distanciaAgua) * 100.0 / (DIST_VAZIO_CM - DIST_CHEIO_CM);
  pct = constrain(pct, 0.0, 100.0);
  nivelReservatorio = (int)(pct + 0.5);
  volumeMl = (int)(pct * CAPACIDADE_ML / 100.0);
}

void lerDHT() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  bool falhou = isnan(t) || isnan(h);
  reportarFalha(falhaDht, falhou, 3, "sensor de temperatura/umidade DHT22");

  if (falhou) {
    temperatura = NAN;
    umidade = NAN;
    return;
  }
  temperatura = t;
  umidade = h;
}

void lerSolo() {
  soloBruto = lerAnalogicoMedio(PIN_SOLO);
  umidadeSolo = constrain(map(soloBruto, SOLO_SECO, SOLO_MOLHADO, 0, 100), 0, 100);
  // valor travado em 0 ou no fundo de escala sugere sensor desconectado ou em curto
  reportarFalha(falhaSolo, soloBruto <= 5 || soloBruto >= 4090, 3, "sensor de umidade do solo (fora da faixa, desconectado?)");
}

void lerLDR() {
  ldrBruto = lerAnalogicoMedio(PIN_LDR);
  luminosidade = constrain(map(ldrBruto, LDR_ESCURO, LDR_CLARO, 0, 100), 0, 100);
}

// Gera AVISOS quando os valores saem da faixa desejada
void avaliarAlertas() {
  if (nivelReservatorio >= 0)
    condicao(avisoReserv, nivelReservatorio < AVISO_RESERV_PCT, nivelReservatorio >= AVISO_RESERV_PCT + 5,
             "Reservatorio com nivel baixo (%)", nivelReservatorio);
  if (!falhaSolo.ativa)
    condicao(avisoSolo, umidadeSolo < AVISO_SOLO_PCT, umidadeSolo >= AVISO_SOLO_PCT + 5,
             "Solo seco, umidade (%)", umidadeSolo);
  if (!isnan(temperatura))
    condicao(avisoTemp, temperatura > AVISO_TEMP_C, temperatura <= AVISO_TEMP_C - 2,
             "Temperatura alta (C)", temperatura);
}

void lerSensores() {
  lerDHT();
  lerSolo();
  lerLDR();
  lerReservatorio();
  avaliarAlertas();
}

// INFO periódico com o resumo das leituras
void registrarResumoSensores() {
  char t[12], u[12], r[12];
  if (isnan(temperatura)) strcpy(t, "erro"); else snprintf(t, sizeof(t), "%.1f", temperatura);
  if (isnan(umidade))     strcpy(u, "erro"); else snprintf(u, sizeof(u), "%.1f", umidade);
  if (nivelReservatorio < 0) strcpy(r, "erro"); else snprintf(r, sizeof(r), "%d", nivelReservatorio);
  LOG_INFO("Sensores: temp=%s C, ar=%s %%, solo=%d %%, luz=%d %%, reservatorio=%s %%, bomba=%s",
           t, u, umidadeSolo, luminosidade, r, bombaLigada ? "ligada" : "desligada");
}

// ----------- Respostas HTTP -----------
void enviarEstado() {
  char ts[24];
  montarTimestamp(ts, sizeof(ts));
  String json = "{\"ligada\":";
  json += bombaLigada ? "true" : "false";
  json += ",\"temperatura\":";
  json += isnan(temperatura) ? "null" : String(temperatura, 1);
  json += ",\"umidade\":";
  json += isnan(umidade) ? "null" : String(umidade, 1);
  json += ",\"solo\":";
  json += String(umidadeSolo);
  json += ",\"luminosidade\":";
  json += String(luminosidade);
  json += ",\"solo_bruto\":";
  json += String(soloBruto);
  json += ",\"ldr_bruto\":";
  json += String(ldrBruto);
  json += ",\"reservatorio\":";
  json += (nivelReservatorio < 0) ? "null" : String(nivelReservatorio);
  json += ",\"dist_cm\":";
  json += (distanciaAgua < 0) ? "null" : String(distanciaAgua, 1);
  json += ",\"volume_ml\":";
  json += String(volumeMl < 0 ? 0 : volumeMl);
  json += ",\"bloqueada\":";
  json += nivelBaixo() ? "true" : "false";
  json += ",\"hora\":\"";
  json += ts;
  json += "\"}";
  enviarCabecalhosCors();
  server.send(200, "application/json", json);
}

// Rota "/" sem login: serve para testar se o ESP32 está no ar
void handleRaiz() {
  enviarCabecalhosCors();
  server.send(200, "application/json",
    "{\"dispositivo\":\"SATURN Plant Keeper (ESP32)\",\"rotas\":[\"/status\",\"/ligar\",\"/desligar\",\"/info\",\"/log\",\"/log/limpar\"]}");
}

// IP, MAC, Wi-Fi, hora e uso do log
void handleInfo() {
  if (!autenticado()) return;
  char ts[24];
  montarTimestamp(ts, sizeof(ts));
  bool conectado = (WiFi.status() == WL_CONNECTED);
  String json = "{\"wifi\":\"";
  json += conectado ? "conectado" : "desconectado";
  json += "\",\"ssid\":\"";
  json += WIFI_SSID;
  json += "\",\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\",\"mac\":\"";
  json += WiFi.macAddress();
  json += "\",\"rssi\":";
  json += String(conectado ? WiFi.RSSI() : 0);
  json += ",\"hora\":\"";
  json += ts;
  json += "\",\"hora_sincronizada\":";
  json += horaSincronizada ? "true" : "false";
  json += ",\"uptime_s\":";
  json += String(millis() / 1000);
  json += ",\"heap_livre\":";
  json += String(ESP.getFreeHeap());
  json += ",\"log_bytes\":";
  json += String(tamanhoLog());
  json += "}";
  enviarCabecalhosCors();
  server.send(200, "application/json", json);
}

bool linhaPassaFiltro(const String& linha, const String& nivel) {
  if (nivel.length() == 0) return true;
  return linha.indexOf("|" + nivel + "|") >= 0;
}

// /log?nivel=ERRO&n=100 -> texto, uma linha por registro, do mais antigo ao mais novo
void handleLog() {
  if (!autenticado()) return;
  String nivel = server.arg("nivel");
  nivel.toUpperCase();
  int n = server.hasArg("n") ? server.arg("n").toInt() : 200;
  if (n <= 0 || n > 1000) n = 200;

  const char* arqs[2] = { LOG_ANT, LOG_ARQ };
  int total = 0;
  for (int i = 0; i < 2; i++) {            // 1ª passada: conta as linhas que passam no filtro
    File f = LittleFS.open(arqs[i], "r");
    if (!f) continue;
    while (f.available()) {
      String l = f.readStringUntil('\n');
      if (l.length() && linhaPassaFiltro(l, nivel)) total++;
    }
    f.close();
  }
  int pular = total > n ? total - n : 0;

  enviarCabecalhosCors();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");
  int idx = 0;
  String buf;
  for (int i = 0; i < 2; i++) {            // 2ª passada: envia só as últimas N
    File f = LittleFS.open(arqs[i], "r");
    if (!f) continue;
    while (f.available()) {
      String l = f.readStringUntil('\n');
      if (!l.length() || !linhaPassaFiltro(l, nivel)) continue;
      if (idx++ < pular) continue;
      buf += l;
      buf += '\n';
      if (buf.length() > 1024) { server.sendContent(buf); buf = ""; }
    }
    f.close();
  }
  if (buf.length()) server.sendContent(buf);
  server.sendContent("");
}

void handleLogLimpar() {
  if (!autenticado()) return;
  LittleFS.remove(LOG_ARQ);
  LittleFS.remove(LOG_ANT);
  LOG_INFO("Log apagado pelo IP %s", ipCliente().c_str());
  enviarCabecalhosCors();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Preflight CORS (OPTIONS) e rotas inexistentes
void handleNaoEncontrado() {
  enviarCabecalhosCors();
  if (server.method() == HTTP_OPTIONS) {
    server.send(204, "text/plain", "");
    return;
  }
  server.send(404, "application/json", "{\"erro\":\"rota nao encontrada\"}");
}

void handleLigar() {
  if (!autenticado()) return;
  if (nivelBaixo()) {
    LOG_AVISO("Acionamento bloqueado: reservatorio com %d %% (minimo %d %%), pedido do IP %s",
              nivelReservatorio, NIVEL_MINIMO_BOMBA_PCT, ipCliente().c_str());
    setBomba(false, "bloqueio por nivel baixo");
  } else {
    setBomba(true, ("comando web " + ipCliente()).c_str());
  }
  enviarEstado();
}

void handleDesligar() {
  if (!autenticado()) return;
  setBomba(false, ("comando web " + ipCliente()).c_str());
  enviarEstado();
}

void handleStatus() { if (!autenticado()) return; enviarEstado(); }

// ----------- Wi-Fi e horário -----------
void iniciarNtp() {
  configTime(GMT_OFFSET_S, 0, NTP_1, NTP_2);   // a sincronização acontece em segundo plano
}

void verificarNtp() {
  if (horaSincronizada) return;
  if (horaValida()) {
    horaSincronizada = true;
    char ts[24];
    montarTimestamp(ts, sizeof(ts));
    LOG_INFO("Hora sincronizada por NTP: %s", ts);
    return;
  }
  if (WiFi.status() == WL_CONNECTED && !ntpErroLogado && millis() > 40000) {
    ntpErroLogado = true;
    LOG_ERRO("NTP nao sincronizou (rede sem internet?). Os registros usam o tempo desde o boot");
  }
}

void verificarWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiCaido) {
      wifiCaido = false;
      LOG_INFO("Wi-Fi reconectado apos %lu s. IP %s", (millis() - quedaWifiInicio) / 1000,
               WiFi.localIP().toString().c_str());
    }
    return;
  }
  // Sem Wi-Fi: desliga a bomba (segurança) e tenta reconectar a cada 5 s
  if (!wifiCaido) {
    wifiCaido = true;
    quedaWifiInicio = millis();
    LOG_AVISO("Wi-Fi desconectado (motivo %d). Tentando reconectar", (int)motivoDesconexao);
  }
  if (bombaLigada) setBomba(false, "queda do Wi-Fi");
  if (millis() - ultimaTentativaWifi > 5000) {
    ultimaTentativaWifi = millis();
    WiFi.reconnect();
  }
}

const char* descreverReset(esp_reset_reason_t r, bool& grave) {
  grave = false;
  switch (r) {
    case ESP_RST_POWERON:  return "energia ligada";
    case ESP_RST_SW:       return "reinicio por software";
    case ESP_RST_EXT:      return "botao de reset";
    case ESP_RST_DEEPSLEEP: return "saida do deep sleep";
    case ESP_RST_BROWNOUT: grave = true; return "QUEDA DE TENSAO (brownout): verifique a alimentacao";
    case ESP_RST_PANIC:    grave = true; return "falha do programa (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      grave = true; return "watchdog (travamento)";
    default:               return "motivo desconhecido";
  }
}

// ----------- Setup -----------
void setup() {
  Serial.begin(115200);

  setBomba(false, "inicializacao");  // começa desligada (pino do relé flutuando)

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  dht.begin();
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SOLO, ADC_11db);
  analogSetPinAttenuation(PIN_LDR, ADC_11db);

  fsOk = LittleFS.begin(true);   // true = formata na primeira vez
  if (!fsOk) Serial.println("ERRO: falha ao montar o LittleFS, o log em arquivo esta desativado");

  bool grave;
  esp_reset_reason_t motivo = esp_reset_reason();
  const char* descricao = descreverReset(motivo, grave);
  if (grave) LOG_AVISO("Sistema reiniciado: %s", descricao);
  else       LOG_INFO("Sistema iniciado: %s", descricao);

  WiFi.mode(WIFI_STA);
  WiFi.onEvent([](WiFiEvent_t evento, WiFiEventInfo_t info) {
    motivoDesconexao = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  Serial.print("Conectando ao Wi-Fi");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  iniciarNtp();
  if (WiFi.status() == WL_CONNECTED) {
    LOG_INFO("Wi-Fi conectado em %s. IP %s, MAC %s, sinal %d dBm", WIFI_SSID,
             WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(), WiFi.RSSI());
    Serial.print("Acesse: http://");
    Serial.println(WiFi.localIP());
  } else {
    LOG_ERRO("Falha ao conectar ao Wi-Fi %s na inicializacao (status %d)", WIFI_SSID, (int)WiFi.status());
    wifiCaido = true;
    quedaWifiInicio = millis();
  }

  server.on("/", HTTP_GET, handleRaiz);
  server.on("/ligar", HTTP_GET, handleLigar);
  server.on("/desligar", HTTP_GET, handleDesligar);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/log/limpar", HTTP_GET, handleLogLimpar);
  server.onNotFound(handleNaoEncontrado);
  server.begin();
  LOG_INFO("Servidor HTTP iniciado na porta 80");

  lerSensores();
  registrarResumoSensores();
  ultimoLogSensores = millis();
}

// ----------- Loop -----------
void loop() {
  server.handleClient();

  unsigned long agora = millis();

  // Leitura periódica dos sensores
  if (agora - ultimaLeituraSensores >= INTERVALO_SENSORES_MS) {
    ultimaLeituraSensores = agora;
    lerSensores();
  }

  // Resumo periódico dos sensores no log
  if (agora - ultimoLogSensores >= LOG_INTERVALO_SENSORES_MS) {
    ultimoLogSensores = agora;
    registrarResumoSensores();
  }

  // Segurança: tempo máximo ligada
  if (bombaLigada && (agora - inicioBomba >= TEMPO_MAX_BOMBA_MS)) {
    LOG_AVISO("Tempo maximo de %lu s atingido", TEMPO_MAX_BOMBA_MS / 1000);
    setBomba(false, "tempo maximo");
  }

  // Segurança: reservatório quase vazio
  if (bombaLigada && nivelBaixo()) {
    LOG_AVISO("Reservatorio com %d %%: bomba sera desligada para nao funcionar a seco", nivelReservatorio);
    setBomba(false, "nivel baixo");
  }

  verificarWifi();
  verificarNtp();
}
