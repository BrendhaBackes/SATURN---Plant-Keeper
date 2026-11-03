#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>

// ----------- Configurações -----------
const char* WIFI_SSID  = "AP_Luck";
const char* WIFI_SENHA = "ap-luck12345";

// Login da página
const char* WEB_USUARIO = "admin";
const char* WEB_SENHA   = "adm123";

const int PIN_RELE = 13;
const int PIN_DHT  = 19;
const int PIN_SOLO = 32;   // ADC1
const int PIN_LDR  = 15;   // ADC1
const int PIN_TRIG = 34;   // HC-SR04
const int PIN_ECHO = 33;   // HC-SR04 (usar divisor de tensão 5V -> 3,3V)
#define DHTTYPE DHT22

const bool RELE_ATIVO_LOW = true;                 // false se o seu relé for ativo em HIGH
const unsigned long TEMPO_MAX_BOMBA_MS = 60000;   // desliga sozinha após 60 s
const unsigned long INTERVALO_SENSORES_MS = 2500; // DHT22 aceita no máx. 1 leitura a cada 2 s

// Calibração do sensor de solo (valor ALTO = seco, BAIXO = molhado)
const int SOLO_SECO    = 3000;
const int SOLO_MOLHADO = 1300;

// Calibração do LDR (mais luz = valor maior)
const int LDR_ESCURO = 200;
const int LDR_CLARO  = 3800;

// Calibração do reservatório (distância do sensor até a superfície da água)
const float DIST_VAZIO_CM = 12.0;   // reservatório vazio (ajuste!)
const float DIST_CHEIO_CM = 4.0;    // reservatório cheio (mín. ~4 cm)
const int   CAPACIDADE_ML = 500;    // capacidade entre vazio e cheio (ajuste!)

// Proteção da bomba contra funcionar a seco
const bool  PROTEGER_BOMBA_NIVEL_BAIXO = true;
const int   NIVEL_MINIMO_BOMBA_PCT = 10;   // abaixo disso a bomba não liga

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

// ----------- Página HTML -----------
const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Estufa / Bomba</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #111; color: #eee; padding: 25px; }
    h1 { font-size: 1.5em; }
    .cards { display: flex; justify-content: center; gap: 15px; flex-wrap: wrap; margin: 20px 0; }
    .card { background: #1e1e1e; border-radius: 14px; padding: 18px 28px; min-width: 130px; }
    .valor { font-size: 2.2em; font-weight: bold; }
    .sub { font-size: 0.85em; color: #888; margin-top: 4px; }
    .rotulo { font-size: 0.9em; color: #aaa; }
    #estado { font-size: 2em; margin: 20px 0; font-weight: bold; }
    .on  { color: #3ddc84; }
    .off { color: #ff5252; }
    button { font-size: 1.4em; padding: 18px 40px; margin: 10px; border: none;
             border-radius: 12px; color: #fff; cursor: pointer; }
    #btnLigar    { background: #2e7d32; }
    #btnDesligar { background: #c62828; }
    #aviso { color: #ffb300; min-height: 1.4em; }
  </style>
</head>
<body>
  <h1>Monitoramento e Bomba</h1>

  <div class="cards">
    <div class="card"><div class="rotulo">Temperatura</div><div class="valor" id="temp">--</div></div>
    <div class="card"><div class="rotulo">Umidade do ar</div><div class="valor" id="umid">--</div></div>
    <div class="card"><div class="rotulo">Umidade do solo</div><div class="valor" id="solo">--</div></div>
    <div class="card"><div class="rotulo">Luminosidade</div><div class="valor" id="luz">--</div></div>
    <div class="card"><div class="rotulo">Reservatório</div><div class="valor" id="res">--</div><div class="sub" id="resMl"></div></div>
  </div>

  <h2>Bomba d'água</h2>
  <div id="estado">...</div>
  <div id="aviso"></div>
  <button id="btnLigar" onclick="enviar('ligar')">LIGAR</button>
  <button id="btnDesligar" onclick="enviar('desligar')">DESLIGAR</button>

  <script>
    function mostrarBomba(ligada) {
      const e = document.getElementById('estado');
      e.textContent = ligada ? 'LIGADA' : 'DESLIGADA';
      e.className = ligada ? 'on' : 'off';
    }
    function mostrarDados(d) {
      mostrarBomba(d.ligada);
      document.getElementById('temp').textContent =
        (d.temperatura === null) ? 'erro' : d.temperatura.toFixed(1) + ' °C';
      document.getElementById('umid').textContent =
        (d.umidade === null) ? 'erro' : d.umidade.toFixed(1) + ' %';
      document.getElementById('solo').textContent = d.solo + ' %';
      document.getElementById('luz').textContent = d.luminosidade + ' %';
      if (d.reservatorio === null) {
        document.getElementById('res').textContent = 'erro';
        document.getElementById('resMl').textContent = '';
      } else {
        document.getElementById('res').textContent = d.reservatorio + ' %';
        document.getElementById('resMl').textContent = '~' + d.volume_ml + ' ml';
      }
      document.getElementById('aviso').textContent =
        d.bloqueada ? 'Bomba bloqueada: nível do reservatório muito baixo' : '';
    }
    function enviar(acao) {
      fetch('/' + acao).then(r => r.json()).then(mostrarDados);
    }
    function atualizar() {
      fetch('/status').then(r => r.json()).then(mostrarDados).catch(() => {});
    }
    atualizar();
    setInterval(atualizar, 2000);
  </script>
</body>
</html>
)rawliteral";

// ----------- Funções -----------
bool nivelBaixo() {
  return PROTEGER_BOMBA_NIVEL_BAIXO && nivelReservatorio >= 0 &&
         nivelReservatorio < NIVEL_MINIMO_BOMBA_PCT;
}

void setBomba(bool ligar) {
  bombaLigada = ligar;
  bool nivel = RELE_ATIVO_LOW ? !ligar : ligar;
  digitalWrite(PIN_RELE, nivel ? HIGH : LOW);
  if (ligar) inicioBomba = millis();
  Serial.println(ligar ? ">>> BOMBA LIGADA" : ">>> BOMBA DESLIGADA");
}

bool autenticado() {
  if (!server.authenticate(WEB_USUARIO, WEB_SENHA)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

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

  // ordenação simples
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }

  return v[n / 2];
}

void lerReservatorio() {
  distanciaAgua = lerDistanciaMediana();

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

  if (isnan(t) || isnan(h)) {
    Serial.println("Falha na leitura do DHT22");
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
}

void lerLDR() {
  ldrBruto = lerAnalogicoMedio(PIN_LDR);
  luminosidade = constrain(map(ldrBruto, LDR_ESCURO, LDR_CLARO, 0, 100), 0, 100);
}

void lerSensores() {
  lerDHT();
  lerSolo();
  lerLDR();
  lerReservatorio();
  Serial.printf("Temp: %.1f C | Ar: %.1f %% | Solo: %d (%d %%) | LDR: %d (%d %%) | Agua: %.1f cm (%d %%, ~%d ml)\n",
                temperatura, umidade, soloBruto, umidadeSolo, ldrBruto, luminosidade,
                distanciaAgua, nivelReservatorio, volumeMl);
}

void enviarEstado() {
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
  json += ",\"reservatorio\":";
  json += (nivelReservatorio < 0) ? "null" : String(nivelReservatorio);
  json += ",\"volume_ml\":";
  json += String(volumeMl < 0 ? 0 : volumeMl);
  json += ",\"bloqueada\":";
  json += nivelBaixo() ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

void handleRaiz() { if (!autenticado()) return; server.send_P(200, "text/html", PAGINA); }

void handleLigar() {
  if (!autenticado()) return;
  if (nivelBaixo()) {
    Serial.println("Bomba bloqueada: nivel baixo");
    setBomba(false);
  } else {
    setBomba(true);
  }
  enviarEstado();
}

void handleDesligar() { if (!autenticado()) return; setBomba(false); enviarEstado(); }
void handleStatus()   { if (!autenticado()) return; enviarEstado(); }

// ----------- Setup -----------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_RELE, OUTPUT);
  setBomba(false);  // começa desligada

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  dht.begin();
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SOLO, ADC_11db);
  analogSetPinAttenuation(PIN_LDR, ADC_11db);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  Serial.print("Conectando ao Wi-Fi");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Conectado! Acesse: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.printf("Falhou. Status do Wi-Fi: %d\n", WiFi.status());
  }

  server.on("/", handleRaiz);
  server.on("/ligar", handleLigar);
  server.on("/desligar", handleDesligar);
  server.on("/status", handleStatus);
  server.begin();

  lerSensores();
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

  // Segurança: tempo máximo ligada
  if (bombaLigada && (agora - inicioBomba >= TEMPO_MAX_BOMBA_MS)) {
    Serial.println("Tempo maximo atingido, desligando.");
    setBomba(false);
  }

  // Segurança: reservatório quase vazio
  if (bombaLigada && nivelBaixo()) {
    Serial.println("Nivel baixo no reservatorio, desligando bomba.");
    setBomba(false);
  }

  // Se o Wi-Fi cair, desliga a bomba e tenta reconectar
  if (WiFi.status() != WL_CONNECTED) {
    if (bombaLigada) setBomba(false);
    WiFi.reconnect();
    delay(2000);
  }
}