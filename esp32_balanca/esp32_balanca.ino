/*
  GÊMEO DIGITAL DO ALMOXARIFADO — Caixa inteligente (protótipo PROTO-01)
  ESP32 + HX711 + células de carga aproveitadas da balança de banheiro

  Passo a passo da montagem, da gravação e dos testes: MONTAGEM.md

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
    Bibliotecas (Gerenciador de Bibliotecas):
      "HX711 Arduino Library" (Bogdan Necula)
      "PubSubClient" (Nick O'Leary)

  COMO O PAINEL RECEBE OS DADOS (o mesmo código atende os três jeitos)
    1) Cabo USB:   uma linha JSON a cada 500 ms na Serial (115200 baud)
    2) Internet:   o ESP32 publica a leitura no servidor MQTT público
                   broker.hivemq.com, no canal CANAL (abaixo). No painel:
                   Wi-Fi -> código da caixa. Funciona no link publicado, de
                   qualquer lugar (até pelo celular).
    3) Rede local: http://<IP>/dados (JSON), /tara e /cmd?c=<comando>.
                   Só com o index.html aberto do computador.
                   Sem rede, o ESP32 cria a própria rede "Almoxarifado-ESP32"
                   (senha 12345678) e responde em http://192.168.4.1

  COMANDOS PELO MONITOR SERIAL (115200 baud, final de linha "Nova linha")
    t                     -> tara: zera a balança com a caixa vazia em cima
    c500                  -> calibra com um peso conhecido em cima (ex.: 500 g)
    w NOME DA REDE|SENHA  -> troca a rede Wi-Fi sem regravar o código
    w                     -> esquece a rede salva e volta a usar a do código
    i                     -> mostra rede, IP, canal, calibração e item
    item SKU;g;min;cap    -> item guardado na caixa (o painel envia sozinho)
  Tudo fica salvo na memória do ESP32 e continua lá depois de desligar.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include "HX711.h"

// ===================== CONFIGURAÇÃO =====================
// Rede Wi-Fi com internet (o roteador de casa ou o roteador do celular).
// Dá para trocar depois pelo Monitor Serial com o comando w, sem regravar.
const char* WIFI_SSID  = "NOME_DA_SUA_REDE";
const char* WIFI_SENHA = "SENHA_DA_SUA_REDE";

// Código da caixa na internet: é o que se digita no painel (Wi-Fi -> código).
// Quem souber o código vê a caixa ao vivo; troque se quiser um canal só seu
// (use só letras sem acento, números e hífen).
const char* CANAL         = "ufabc-almox-g5";
const char* MQTT_SERVIDOR = "broker.hivemq.com";
const uint16_t MQTT_PORTA = 1883;

const char* AP_SSID  = "Almoxarifado-ESP32";
const char* AP_SENHA = "12345678";
const char* ID_CAIXA = "PROTO-01";

const int PINO_DT  = 16;
const int PINO_SCK = 4;

// Variação máxima (g) dentro da janela de leituras para considerar o peso estável.
// Células de balança de banheiro oscilam mais: se nunca ficar "estável", aumente.
const float LIMIAR_ESTAVEL_G = 8.0;
// ========================================================

HX711 balanca;
WebServer servidor(80);
Preferences prefs;
WiFiClient redeMqtt;
PubSubClient mqtt(redeMqtt);

float fator = 1.0;  // calibrado com o comando c<gramas>
bool hx711Ok = false;

String redeSsid, redeSenha, ipAtual = "";
bool temRede = false;  // há uma rede configurada (no código ou pelo comando w)
bool modoAP = false;   // sem rede: o ESP32 está com a própria rede no ar
unsigned long ultimaTentativaWifi = 0;

String topicoDados, topicoStatus, topicoCmd;
unsigned long ultimaTentativaMqtt = 0, ultimoMqtt = 0;

// Item guardado na caixa. O painel envia ao conectar; assim quem abrir o
// painel em outro computador ou celular vê a mesma contagem.
String itemSku = "";
float pesoUn = 0;  // gramas por peça (0 = ainda não configurado)
int itemMin = 0, itemCap = 0;

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
  char buf[320];
  int n = snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"peso_g\":%.1f,\"estavel\":%s",
                   ID_CAIXA, pesoAtual, estavel ? "true" : "false");
  if (pesoUn > 0) {
    long qtd = lroundf(fmaxf(0, pesoAtual) / pesoUn);
    n += snprintf(buf + n, sizeof(buf) - n, ",\"qtd\":%ld,\"item\":\"%s\",\"g_un\":%.1f,\"min\":%d,\"cap\":%d",
                  qtd, itemSku.c_str(), pesoUn, itemMin, itemCap);
  }
  snprintf(buf + n, sizeof(buf) - n, ",\"ip\":\"%s\",\"uptime_s\":%lu}", ipAtual.c_str(), millis() / 1000);
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
  if (fabsf(bruto) < 50) {
    balanca.set_scale(fator);
    Serial.println("# Nao percebi o peso. Ele esta em cima da caixa? Confira os fios S+/S- (A+/A-).");
    return;
  }
  fator = bruto / gramas;
  balanca.set_scale(fator);
  prefs.putFloat("fator", fator);
  zerarJanela();
  Serial.printf("# Calibrado com %.0f g: fator = %.4f (salvo)\n", gramas, fator);
}

// item SKU;g;min;cap   ex.: item ROL-6205;128;5;20
void configurarItem(String args) {
  int a = args.indexOf(';'), b = args.indexOf(';', a + 1), c = args.indexOf(';', b + 1);
  float g = args.substring(a + 1, b).toFloat();
  if (a < 1 || b < 0 || c < 0 || g <= 0) {
    Serial.println("# Use: item SKU;peso_por_peca_g;minimo;capacidade");
    return;
  }
  String sku = "";
  for (int i = 0; i < a && sku.length() < 24; i++) {
    char ch = args[i];
    if (isalnum(ch) || ch == '-' || ch == '_') sku += ch;  // só o que pode ir no JSON sem escapar
  }
  itemSku = sku;
  pesoUn = roundf(g * 10) / 10;
  itemMin = max(0, (int)args.substring(b + 1, c).toInt());
  itemCap = max(1, (int)args.substring(c + 1).toInt());
  prefs.putString("item", itemSku);
  prefs.putFloat("g_un", pesoUn);
  prefs.putInt("min", itemMin);
  prefs.putInt("cap", itemCap);
  Serial.printf("# Item %s: %.1f g por peca, minimo %d, capacidade %d (salvo)\n",
                itemSku.c_str(), pesoUn, itemMin, itemCap);
}

void trocarRede(String args) {
  args.trim();
  if (args.length() == 0) {
    prefs.remove("ssid");
    prefs.remove("senha");
    Serial.println("# Rede salva apagada. Reiniciando com a rede escrita no codigo...");
  } else {
    int s = args.indexOf('|');
    String ssid = s < 0 ? args : args.substring(0, s);
    String senha = s < 0 ? "" : args.substring(s + 1);
    prefs.putString("ssid", ssid);
    prefs.putString("senha", senha);
    Serial.printf("# Rede \"%s\" salva. Reiniciando...\n", ssid.c_str());
  }
  delay(300);
  ESP.restart();
}

void info() {
  Serial.printf("# Caixa %s | HX711 %s | fator %.4f%s\n", ID_CAIXA,
                hx711Ok ? "ok" : "NAO ENCONTRADO", fator, fator == 1.0 ? " (sem calibrar)" : "");
  Serial.printf("# Rede %s | IP %s | internet: %s (codigo \"%s\")\n",
                modoAP ? AP_SSID : redeSsid.c_str(), ipAtual.c_str(),
                mqtt.connected() ? "conectada" : "desconectada", CANAL);
  if (pesoUn > 0) {
    Serial.printf("# Item %s: %.1f g por peca, minimo %d, capacidade %d\n",
                  itemSku.c_str(), pesoUn, itemMin, itemCap);
  } else {
    Serial.println("# Item ainda nao configurado (o painel envia ao conectar)");
  }
}

// remoto = veio pela rede (painel no Wi-Fi/internet). Pela rede só passam a tara
// e o item; calibração e troca de Wi-Fi só pelo cabo USB.
void executar(String cmd, bool remoto) {
  cmd.trim();
  if (cmd.length() == 0) return;
  if (cmd == "t") tarar();
  else if (cmd.startsWith("item ")) configurarItem(cmd.substring(5));
  else if (remoto) return;
  else if (cmd == "i") info();
  else if (cmd == "w" || cmd.startsWith("w ")) trocarRede(cmd.substring(1));
  else if (cmd.startsWith("c")) calibrar(cmd.substring(1).toFloat());
  else Serial.println("# Comando desconhecido. Use t, c500, w REDE|SENHA ou i");
}

void lerComandos() {
  static String linha;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      executar(linha, false);
      linha = "";
    } else if (linha.length() < 120) {
      linha += c;
    }
  }
}

// ---------- Wi-Fi e servidor HTTP (rede local) ----------
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

void rotaCmd() {
  executar(servidor.arg("c"), true);
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

void redeConectada() {
  modoAP = false;
  ipAtual = WiFi.localIP().toString();
  Serial.printf("# Wi-Fi conectado em \"%s\". IP %s\n", redeSsid.c_str(), ipAtual.c_str());
}

void conectarWifi() {
  redeSsid = prefs.isKey("ssid") ? prefs.getString("ssid") : String(WIFI_SSID);
  redeSenha = prefs.isKey("senha") ? prefs.getString("senha") : String(WIFI_SENHA);
  temRede = redeSsid.length() > 0 && redeSsid != "NOME_DA_SUA_REDE";

  if (temRede) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(redeSsid.c_str(), redeSenha.c_str());
    Serial.printf("# Conectando ao Wi-Fi \"%s\"", redeSsid.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
      delay(400);
      Serial.print(".");
    }
    Serial.println();
  }

  if (WiFi.status() == WL_CONNECTED) {
    redeConectada();
  } else {
    // sem rede: cria a própria e continua tentando a configurada a cada 30 s
    modoAP = true;
    WiFi.mode(temRede ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(AP_SSID, AP_SENHA);
    ipAtual = WiFi.softAPIP().toString();
    if (temRede) Serial.printf("# Nao consegui entrar em \"%s\" (tento de novo a cada 30 s).", redeSsid.c_str());
    else Serial.print("# Nenhuma rede Wi-Fi configurada.");
    Serial.printf(" Rede propria \"%s\" (senha %s) -> http://%s\n", AP_SSID, AP_SENHA, ipAtual.c_str());
    if (!temRede) Serial.println("# Para usar a internet, envie: w NOME DA REDE|SENHA");
  }
  if (MDNS.begin("almoxarifado")) Serial.println("# Tambem responde em http://almoxarifado.local");
}

void manterWifi() {
  if (!modoAP || !temRede) return;
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.softAPdisconnect(true);  // a rede configurada voltou: desliga a rede própria
    redeConectada();
  } else if (millis() - ultimaTentativaWifi >= 30000) {
    ultimaTentativaWifi = millis();
    WiFi.begin(redeSsid.c_str(), redeSenha.c_str());
  }
}

// ---------- Internet (MQTT) ----------
void aoReceberMqtt(char* topico, byte* dados, unsigned int n) {
  String cmd;
  for (unsigned int i = 0; i < n && i < 120; i++) cmd += (char)dados[i];
  executar(cmd, true);
}

void manterMqtt() {
  if (modoAP || WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  // tenta a cada 10 s para não travar a leitura da balança
  if (ultimaTentativaMqtt && millis() - ultimaTentativaMqtt < 10000) return;
  ultimaTentativaMqtt = millis();
  String id = String("caixa-") + CANAL + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  // se a caixa cair, o servidor avisa o painel com "offline"
  if (mqtt.connect(id.c_str(), topicoStatus.c_str(), 0, true, "offline")) {
    mqtt.publish(topicoStatus.c_str(), "online", true);
    mqtt.subscribe(topicoCmd.c_str());
    Serial.printf("# Internet ok. No painel: Wi-Fi -> codigo \"%s\"\n", CANAL);
  } else {
    Serial.printf("# Sem acesso a %s (estado %d). Nova tentativa em 10 s.\n", MQTT_SERVIDOR, mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("# Caixa inteligente %s iniciando\n", ID_CAIXA);

  prefs.begin("balanca", false);
  fator = prefs.getFloat("fator", 1.0);
  if (prefs.isKey("g_un")) {
    itemSku = prefs.getString("item", "");
    pesoUn = prefs.getFloat("g_un", 0);
    itemMin = prefs.getInt("min", 0);
    itemCap = prefs.getInt("cap", 1);
  }

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
  servidor.on("/cmd", HTTP_GET, rotaCmd);
  servidor.on("/cmd", HTTP_OPTIONS, rotaOpcoes);
  servidor.begin();

  String base = String("gemeo-almox/") + CANAL + "/";
  topicoDados = base + "dados";
  topicoStatus = base + "status";
  topicoCmd = base + "cmd";
  mqtt.setServer(MQTT_SERVIDOR, MQTT_PORTA);
  mqtt.setCallback(aoReceberMqtt);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(4);

  info();
}

void loop() {
  servidor.handleClient();
  lerComandos();
  lerBalanca();
  manterWifi();
  manterMqtt();

  unsigned long agora = millis();
  if (agora - ultimoEnvio >= 500) {
    ultimoEnvio = agora;
    Serial.println(json());
  }
  if (mqtt.connected() && agora - ultimoMqtt >= 1000) {
    ultimoMqtt = agora;
    mqtt.publish(topicoDados.c_str(), json().c_str());
  }
}
