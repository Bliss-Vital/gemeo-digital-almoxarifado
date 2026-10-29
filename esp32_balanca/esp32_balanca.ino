/*
  GÊMEO DIGITAL DO ALMOXARIFADO — Caixa inteligente (protótipo PROTO-01)
  ESP32 + HX711 + células de carga aproveitadas da balança de banheiro

  ---------------------------------------------------------------------------
  LIGAÇÕES
  Fios das células de carga (os que estavam soldados na placa TB-400V20):
    E+ (vermelho) -> E+ do HX711
    E- (preto)    -> E- do HX711
    S+ (verde)    -> A+ do HX711
    S- (branco)   -> A- do HX711
    (os fios V+ / V- da placa original eram da pilha: não usar)

  HX711 -> ESP32
    VCC -> 3V3
    GND -> GND
    DT  -> GPIO 16
    SCK -> GPIO 4

  Dica de montagem: mantenha a plataforma/vidro da própria balança e apoie a
  caixa em cima dela — as células precisam continuar nos 4 cantos.
  ---------------------------------------------------------------------------
  ARDUINO IDE
    Placa: "ESP32 Dev Module" (pacote "esp32" da Espressif)
    Biblioteca: "HX711 Arduino Library" (Bogdan Necula) — Gerenciador de Bibliotecas

  COMO O PAINEL RECEBE OS DADOS (o mesmo código atende os dois jeitos)
    1) Cabo USB: uma linha JSON a cada 500 ms na Serial (115200 baud)
    2) Wi-Fi:    http://<IP>/dados  (JSON)   e   http://<IP>/tara
       Se não conseguir entrar na rede configurada abaixo, o ESP32 cria a
       própria rede "Almoxarifado-ESP32" (senha 12345678) -> http://192.168.4.1

  COMANDOS PELO MONITOR SERIAL (ou pelos botões do painel)
    t      -> tara: zera a balança com a caixa vazia em cima
    c500   -> calibra com um peso conhecido em cima (ex.: 500 g)
    i      -> mostra IP e fator de calibração
  Tara e calibração ficam salvas na memória do ESP32.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "HX711.h"

// ===================== CONFIGURAÇÃO =====================
const char* WIFI_SSID  = "NOME_DA_SUA_REDE";
const char* WIFI_SENHA = "SENHA_DA_SUA_REDE";
const char* AP_SSID    = "Almoxarifado-ESP32";
const char* AP_SENHA   = "12345678";
const char* ID_CAIXA   = "PROTO-01";

const int PINO_DT  = 16;
const int PINO_SCK = 4;

// Variação máxima (g) dentro da janela de leituras para considerar o peso estável.
// Células de balança de banheiro oscilam mais: se nunca ficar "estável", aumente.
const float LIMIAR_ESTAVEL_G = 8.0;
// ========================================================

HX711 balanca;
WebServer servidor(80);
Preferences prefs;

float fator = 1.0;  // calibrado com o comando c<gramas>
bool hx711Ok = false;
String ipAtual = "";

const int N_MEDIA = 10;  // ~1 s de leituras a 10 amostras/s
float janela[N_MEDIA];
int nJanela = 0, iJanela = 0;
float pesoAtual = 0;
bool estavel = false;
unsigned long ultimoEnvio = 0;

void zerarJanela() {
  nJanela = 0;
  iJanela = 0;
}

void lerBalanca() {
  if (!hx711Ok || !balanca.is_ready()) return;
  janela[iJanela] = balanca.get_units(1);
  iJanela = (iJanela + 1) % N_MEDIA;
  if (nJanela < N_MEDIA) nJanela++;

  float soma = 0, mn = janela[0], mx = janela[0];
  for (int i = 0; i < nJanela; i++) {
    soma += janela[i];
    mn = fminf(mn, janela[i]);
    mx = fmaxf(mx, janela[i]);
  }
  pesoAtual = soma / nJanela;
  estavel = (nJanela == N_MEDIA) && (mx - mn) <= LIMIAR_ESTAVEL_G;
}

String json() {
  char buf[220];
  snprintf(buf, sizeof(buf),
           "{\"id\":\"%s\",\"peso_g\":%.1f,\"estavel\":%s,\"ip\":\"%s\",\"uptime_s\":%lu}",
           ID_CAIXA, pesoAtual, estavel ? "true" : "false", ipAtual.c_str(), millis() / 1000);
  return String(buf);
}

void tarar() {
  if (!hx711Ok) return;
  balanca.tare(15);
  prefs.putLong("offset", balanca.get_offset());
  zerarJanela();
  Serial.println("# Tara feita: caixa vazia = 0 g");
}

void calibrar(float gramas) {
  if (!hx711Ok || gramas <= 0) return;
  balanca.set_scale(1.0);
  float bruto = balanca.get_units(20);  // leitura bruta já descontada a tara
  fator = bruto / gramas;
  if (fator == 0) fator = 1.0;
  balanca.set_scale(fator);
  prefs.putFloat("fator", fator);
  zerarJanela();
  Serial.printf("# Calibrado com %.0f g: fator = %.4f (salvo)\n", gramas, fator);
}

void info() {
  Serial.printf("# Caixa %s | IP %s | fator %.4f | HX711 %s\n",
                ID_CAIXA, ipAtual.c_str(), fator, hx711Ok ? "ok" : "NAO ENCONTRADO");
}

void lerComandos() {
  static String linha;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      linha.trim();
      if (linha == "t") tarar();
      else if (linha.startsWith("c")) calibrar(linha.substring(1).toFloat());
      else if (linha == "i") info();
      linha = "";
    } else {
      linha += c;
    }
  }
}

// ---------- Wi-Fi e servidor HTTP ----------
void cabecalhosCORS() {
  // permite que o painel (aberto como arquivo no PC) leia os dados
  servidor.sendHeader("Access-Control-Allow-Origin", "*");
  servidor.sendHeader("Access-Control-Allow-Private-Network", "true");
  servidor.sendHeader("Cache-Control", "no-store");
}

void rotaDados() {
  cabecalhosCORS();
  servidor.send(200, "application/json", json());
}

void rotaTara() {
  tarar();
  cabecalhosCORS();
  servidor.send(200, "application/json", "{\"ok\":true}");
}

void rotaOpcoes() {
  cabecalhosCORS();
  servidor.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  servidor.sendHeader("Access-Control-Allow-Headers", "*");
  servidor.send(204);
}

void rotaInicio() {
  servidor.send(200, "text/plain; charset=utf-8",
                String("Caixa inteligente ") + ID_CAIXA + "\nDados: /dados\nTara: /tara\n\n" + json());
}

void conectarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  Serial.printf("# Conectando ao Wi-Fi \"%s\"", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    ipAtual = WiFi.localIP().toString();
    Serial.printf("# Wi-Fi conectado. Endereco para o painel: %s\n", ipAtual.c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_SENHA);
    ipAtual = WiFi.softAPIP().toString();
    Serial.printf("# Rede nao encontrada. Conecte o PC na rede \"%s\" (senha %s) e use %s\n",
                  AP_SSID, AP_SENHA, ipAtual.c_str());
  }
  if (MDNS.begin("almoxarifado")) Serial.println("# Tambem responde em http://almoxarifado.local");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("# Caixa inteligente %s iniciando\n", ID_CAIXA);

  prefs.begin("balanca", false);
  fator = prefs.getFloat("fator", 1.0);

  balanca.begin(PINO_DT, PINO_SCK);
  hx711Ok = balanca.wait_ready_timeout(2000);
  if (!hx711Ok) {
    Serial.println("# ERRO: HX711 nao respondeu. Confira VCC, GND, DT (GPIO16) e SCK (GPIO4).");
  } else {
    balanca.set_scale(fator);
    if (prefs.isKey("offset")) balanca.set_offset(prefs.getLong("offset"));
    else tarar();
    if (fator == 1.0) {
      Serial.println("# ATENCAO: balanca ainda nao calibrada. Coloque um peso conhecido e envie c<gramas>, ex.: c500");
    }
  }

  conectarWifi();
  servidor.on("/", HTTP_GET, rotaInicio);
  servidor.on("/dados", HTTP_GET, rotaDados);
  servidor.on("/dados", HTTP_OPTIONS, rotaOpcoes);
  servidor.on("/tara", HTTP_GET, rotaTara);
  servidor.on("/tara", HTTP_OPTIONS, rotaOpcoes);
  servidor.begin();
  info();
}

void loop() {
  servidor.handleClient();
  lerComandos();
  lerBalanca();
  if (millis() - ultimoEnvio >= 500) {
    ultimoEnvio = millis();
    Serial.println(json());
  }
}
