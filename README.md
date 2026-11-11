# SATURN – Plant Keeper (ESP32 como servidor HTTP)

TDE da disciplina **Conectividade em Sistemas Ciberfísicos** — Engenharia de Software, PUCPR.

Regador inteligente para plantas em terraços. O ESP32 lê sensores de uma planta (temperatura, umidade do ar e do solo, luminosidade e nível do reservatório) e aciona uma **bomba d'água** por relé. Tudo é consultado e comandado pela rede, por uma interface web, e os eventos do sistema (erros, avisos e informações, com data e hora) ficam registrados em um **log na memória flash**, acessível pelo site.

## Integrantes

| Nome completo | RA | Responsabilidade principal |
|---|---|---|
| Lucas Franco | 40131281 | Hardware e prototipagem |
| Victor Zanoto Dias | 40134658 | Firmware (ESP32) |
| Eduardo Cavalheiro Dias | 40135009 | Interface web |
| Brendha Bilharba Backes | 40113248 | Qualidade e documentação |
| Alisson Pereira da Franca | 40083786 | Design estrutural |

## Arquitetura da solução

```
 +--------------------------------------+
 |  CLIENTE (src/client)                |
 |  index.html + app.js (navegador)     |
 +------------------+-------------------+
                    |  HTTP (GET) + JSON, Wi-Fi local
                    v
 +--------------------------------------------------------+
 |  ESP32 - servidor HTTP na porta 80 (src/esp32)         |
 |  /status  /ligar  /desligar  /info  /log  /log/limpar    |
 +------------+--------------------------+----------------+
              | leitura                  | acionamento
              v                          v
   DHT22, solo, LDR, HC-SR04      Relé -> Bomba d'água
              |
              v
   Log em arquivo (LittleFS) + hora por NTP
```

O ESP32 apenas **serve a API**. A interface fica em `src/client` e roda no navegador do PC ou do celular, na mesma rede Wi-Fi do ESP32.

## Hardware utilizado

- ESP32 DevKit (Wi-Fi nativo)
- Módulo relé 1 canal 5 V (SRD-05VDC), **ativo em LOW**
- Bomba d'água com fonte própria
- Sensor de temperatura e umidade DHT22
- Sensor de umidade do solo (saída analógica)
- Módulo LDR (luminosidade)
- Sensor ultrassônico HC-SR04 (nível do reservatório)
- Fonte de protoboard MB102, protoboard e jumpers
- Resistores para o divisor de tensão do ECHO (5 V → 3,3 V)

### Pinagem

| Componente | Pino do módulo | GPIO do ESP32 |
|---|---|---|
| Relé | IN | 13 |
| DHT22 | DATA | 26 |
| Sensor de solo | AO | 32 (ADC1) |
| LDR | AO | 34 (ADC1, somente entrada) |
| HC-SR04 | TRIG | 14 |
| HC-SR04 | ECHO | 33 (com divisor de tensão) |

Todos os GND ficam em comum. Esquemático e fotos: pasta `docs/`.

## API (HTTP)

Autenticação: **HTTP Basic** (usuário e senha definidos em `secrets.h`). A rota `/` é livre.

| Método | Rota | Descrição |
|---|---|---|
| GET | `/` | Teste: confirma que o ESP32 está no ar |
| GET | `/status` | Leituras dos sensores, estado da bomba e hora |
| GET | `/ligar` | Liga a bomba (bloqueada se o reservatório estiver abaixo de 10%) |
| GET | `/desligar` | Desliga a bomba |
| GET | `/info` | IP, MAC, Wi-Fi, sinal, hora, uptime e tamanho do log |
| GET | `/log` | Log do sistema em texto. Parâmetros: `?nivel=ERRO\|AVISO\|INFO` e `&n=200` (últimas N linhas, máx. 1000) |
| GET | `/log/limpar` | Apaga o log |

Resposta de `/status`, `/ligar` e `/desligar`:

```json
{
  "ligada": false,
  "temperatura": 24.5,
  "umidade": 60.1,
  "solo": 42,
  "luminosidade": 70,
  "solo_bruto": 2100,
  "ldr_bruto": 1500,
  "reservatorio": 80,
  "dist_cm": 6.4,
  "volume_ml": 800,
  "bloqueada": false,
  "hora": "2026-11-10 14:32:05"
}
```

`temperatura`, `umidade`, `reservatorio` e `dist_cm` retornam `null` quando a leitura falha.

Teste pelo terminal:

```bash
curl -u admin:SUA_SENHA http://IP_DO_ESP32/status
curl -u admin:SUA_SENHA http://IP_DO_ESP32/ligar
curl -u admin:SUA_SENHA http://IP_DO_ESP32/desligar
curl -u admin:SUA_SENHA http://IP_DO_ESP32/info
curl -u admin:SUA_SENHA "http://IP_DO_ESP32/log?nivel=ERRO&n=50"
```

### Log do sistema

O firmware grava cada evento no arquivo `/log.txt` da flash (LittleFS), com data e hora obtidas por **NTP** (fuso de Brasília). Quando o arquivo passa de 48 KB, ele vira `/log_old.txt` e um novo é iniciado. O log sobrevive a reinícios e pode ser lido em `/log` ou na seção **Log do sistema** da interface web (filtros por nível, download e botão para limpar).

Formato de cada linha:

```
2026-11-10 14:32:05|INFO|Bomba LIGADA (comando web 192.168.43.20)
```

Enquanto o NTP não sincronizou (por exemplo, hotspot sem internet), a data é substituída por `boot+123s` (segundos desde o boot).

| Nível | O que registra |
|---|---|
| **INFO** | Inicialização e motivo do reset, Wi-Fi conectado (IP, MAC, sinal), hora sincronizada, bomba ligada/desligada e a origem, resumo dos sensores a cada 5 min, valores normalizados |
| **AVISO** | Reservatório baixo, solo seco, temperatura alta, acionamento bloqueado, tempo máximo da bomba, queda do Wi-Fi, acesso negado (login inválido), reinício por queda de tensão (brownout) |
| **ERRO** | Falha do DHT22, do HC-SR04 ou do sensor de solo (3 leituras ruins seguidas), falha ao conectar no Wi-Fi, NTP não sincronizado |

Para testar rapidamente, reduza `LOG_INTERVALO_SENSORES_MS` no firmware.

### Segurança da bomba (no firmware)

- Desliga sozinha após 60 s ligada.
- Não liga (ou desliga) com o reservatório abaixo de 10%.
- Desliga se o Wi-Fi cair.

## Instalação e execução

### 1. Firmware (ESP32)

1. Instale a Arduino IDE com o suporte a ESP32 e a biblioteca **DHT sensor library** (Adafruit). `LittleFS`, `WiFi` e `WebServer` já vêm com o suporte a ESP32.
2. Na pasta `src/esp32/esp32_server/`, copie `secrets.example.h` para `secrets.h` e preencha o Wi-Fi e o login. O `secrets.h` está no `.gitignore`.
3. Abra `esp32_server.ino`, selecione a placa **ESP32 Dev Module** (esquema de partição padrão, que reserva espaço para o LittleFS) e a porta COM, e grave.
4. Abra o Serial Monitor (115200) e anote o **IP** exibido. Ele também aparece na linha `Wi-Fi conectado` do log.
5. Para a hora correta no log, a rede (ou o hotspot do celular) precisa ter acesso à internet.

### 2. Cliente (interface web)

1. Abra `src/client/index.html` no navegador (clique duas vezes no arquivo).
2. Informe o **IP do ESP32**, o usuário e a senha, e clique em **Conectar**. A página mostra os sensores, o estado da bomba, o IP/MAC do ESP32 e o log do sistema.
3. O PC/celular precisa estar na **mesma rede Wi-Fi** do ESP32.

> Abra o arquivo localmente (ou com `python -m http.server`). Não use `https://`, porque o navegador bloqueia chamadas para o ESP32 em `http://`.

## Notas técnicas

- **Relé ativo em LOW com ESP32 (3,3 V):** o módulo tem pull-up para 5 V, então um HIGH de 3,3 V não o desliga. Para ligar, o pino vai a LOW; para desligar, vira `INPUT` (flutuando), e o pull-up do módulo desliga o relé.
- **CORS:** o ESP32 envia os cabeçalhos CORS para o cliente, aberto de outra origem, conseguir chamar a API.
- **ADC e Wi-Fi:** os sensores analógicos usam pinos do ADC1, porque o ADC2 não funciona com o Wi-Fi ligado.

## Demonstração

_Adicionar aqui vídeo e screenshots do projeto funcionando._

## Estrutura do repositório

```
├── docs/                    # diagramas, esquemático e imagens
├── src/
│   ├── esp32/
│   │   └── esp32_server/
│   │       ├── esp32_server.ino
│   │       └── secrets.example.h
│   └── client/
│       ├── index.html
│       └── app.js
├── README.md
└── .gitignore
```
