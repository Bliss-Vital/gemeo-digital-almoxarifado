# Gêmeo Digital · Almoxarifado

Gêmeo digital 3D de um almoxarifado fictício, com 8 estantes e 2.352 posições de caixas bin. Uma das caixas, a **PROTO-01**, é real: ela fica sobre uma célula de carga ligada a um ESP32 + HX711 e atualiza o estoque sozinha. A quantidade é calculada pelo peso (peso medido ÷ peso de uma peça). As outras caixas usam sensores simulados para mostrar como o sistema ficaria no almoxarifado inteiro.

**Abrir no navegador:** https://bliss-vital.github.io/gemeo-digital-almoxarifado/

> Projeto da disciplina Inovações para Engenharia (UFABC, 3Q.2026).

## O que dá para fazer

- Navegar pelo almoxarifado em 3D: arrastar para girar, botão direito para mover, roda para zoom e WASD para andar.
- Clicar numa caixa para ver item, quantidade, mínimo, valor e o histórico da quantidade.
- Acompanhar indicadores, alertas de reposição e as movimentações em tempo real.
- Buscar por item ou endereço (ex.: `rolamento`, `C-05`).
- Trocar as cores das caixas entre **Realista**, **Status** (OK / abaixo do mínimo / sem estoque) e **Ocupação**.
- Conectar a caixa protótipo por **USB**, **Wi‑Fi** ou usar o modo **Demonstração**, que funciona sem hardware.

Atalhos pelo endereço: `index.html#proto` vai direto para a caixa protótipo e `index.html#proto&demo` já abre no modo demonstração.

## Hardware da caixa protótipo

| De | Para |
|---|---|
| Célula de carga E+ (vermelho) | HX711 E+ |
| Célula de carga E− (preto) | HX711 E− |
| Célula de carga S+ (verde) | HX711 A+ |
| Célula de carga S− (branco) | HX711 A− |
| HX711 VCC / GND | ESP32 3V3 / GND |
| HX711 DT / SCK | ESP32 GPIO16 / GPIO4 |

1. Abra `esp32_balanca/esp32_balanca.ino` na Arduino IDE (placa **ESP32 Dev Module**) e instale a biblioteca **HX711 Arduino Library** (Bogdan Necula).
2. Se for usar Wi‑Fi, coloque o nome e a senha da rede no topo do código. Se o ESP32 não achar a rede, ele cria a própria rede `Almoxarifado-ESP32` (senha `12345678`) e responde em `192.168.4.1`.
3. Pelo Monitor Serial: envie `t` com a caixa vazia para zerar (tara) e depois `c500` com um peso conhecido de 500 g em cima para calibrar.
4. No painel, escolha **USB** (Chrome ou Edge) ou **Wi‑Fi**.

**Observações**
- A conexão por **Wi‑Fi só funciona abrindo o `index.html` baixado no computador**. No link publicado (https), o navegador bloqueia o acesso ao ESP32 pela rede. Por USB e no modo Demonstração, o link publicado funciona normalmente.
- Células de balança de banheiro não medem bem coisas leves. Use peças de pelo menos uns 50–100 g por unidade.

## Estrutura

```
index.html                    painel 3D (Three.js r128, sem instalação)
lib/                          Three.js e OrbitControls (funciona offline)
esp32_balanca/esp32_balanca.ino   firmware do ESP32 + HX711
```
