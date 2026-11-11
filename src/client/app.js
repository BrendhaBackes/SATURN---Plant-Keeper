// Cliente da API do ESP32 (SATURN – Plant Keeper).
// Consulta /status, /info e /log; envia /ligar, /desligar e /log/limpar.

const CHAVE_CFG = 'estufa_cfg';
const INTERVALO_MS = 2000;        // tempo entre atualizações dos sensores
const INTERVALO_INFO_MS = 10000;  // tempo entre atualizações de IP/MAC/hora
const INTERVALO_LOG_MS = 5000;    // tempo entre atualizações do log
const TIMEOUT_MS = 4000;          // tempo máximo de espera por resposta
const FALHAS_PARA_DESCONECTAR = 2;

let cfg = { ip: '10.118.158.1', usuario: 'admin', senha: 'admin' };
let falhas = 0;
let nivelLog = '';   // '', 'ERRO', 'AVISO' ou 'INFO'

const $ = (id) => document.getElementById(id);

// ----------- Configuração -----------
function carregarCfg() {
  try {
    const salvo = JSON.parse(localStorage.getItem(CHAVE_CFG));
    if (salvo) cfg = { ...cfg, ...salvo };
  } catch (e) { /* sem configuração salva */ }
  $('ip').value = cfg.ip;
  $('usuario').value = cfg.usuario;
  $('senha').value = cfg.senha;
}

function salvarCfg() {
  cfg = { ip: $('ip').value.trim(), usuario: $('usuario').value, senha: $('senha').value };
  try { localStorage.setItem(CHAVE_CFG, JSON.stringify(cfg)); } catch (e) {}
  falhas = 0;
  atualizar();
  atualizarInfo();
  atualizarLog();
}

// ----------- Comunicação com o ESP32 -----------
// formato: 'json' (padrão) ou 'texto'
async function chamar(rota, formato = 'json') {
  const controle = new AbortController();
  const timer = setTimeout(() => controle.abort(), TIMEOUT_MS * (formato === 'texto' ? 3 : 1));
  try {
    const resposta = await fetch('http://' + cfg.ip + rota, {
      headers: { 'Authorization': 'Basic ' + btoa(cfg.usuario + ':' + cfg.senha) },
      signal: controle.signal
    });
    if (resposta.status === 401) throw new Error('login');
    if (!resposta.ok) throw new Error('http ' + resposta.status);
    return formato === 'texto' ? await resposta.text() : await resposta.json();
  } finally {
    clearTimeout(timer);
  }
}

// ----------- Interface -----------
function mostrarConexao(ok, texto) {
  const e = $('conexao');
  e.textContent = texto;
  e.className = ok ? 'ok' : 'erro';
}

function mostrarBomba(ligada) {
  const e = $('estado');
  e.textContent = ligada ? 'LIGADA' : 'DESLIGADA';
  e.className = ligada ? 'on' : 'off';
}

function mostrarDados(d) {
  mostrarBomba(d.ligada);
  $('temp').textContent = (d.temperatura === null) ? 'erro' : d.temperatura.toFixed(1) + ' °C';
  $('umid').textContent = (d.umidade === null) ? 'erro' : d.umidade.toFixed(1) + ' %';
  $('solo').textContent = d.solo + ' %';
  $('luz').textContent = d.luminosidade + ' %';
  $('soloRaw').textContent = 'bruto: ' + d.solo_bruto;
  $('luzRaw').textContent = 'bruto: ' + d.ldr_bruto;

  if (d.reservatorio === null) {
    $('res').textContent = 'erro';
    $('resMl').textContent = '';
  } else {
    $('res').textContent = d.reservatorio + ' %';
    $('resMl').textContent = '~' + d.volume_ml + ' ml';
  }
  $('resDist').textContent = (d.dist_cm === null) ? 'dist: erro' : 'dist: ' + d.dist_cm.toFixed(1) + ' cm';
  $('aviso').textContent = d.bloqueada ? 'Bomba bloqueada: nível do reservatório muito baixo' : '';
}

function tratarErro(e) {
  if (e.message === 'login') {
    mostrarConexao(false, 'Usuário ou senha incorretos');
    return;
  }
  falhas++;
  if (falhas >= FALHAS_PARA_DESCONECTAR) mostrarConexao(false, 'Desconectado do ESP32');
}

// ----------- Ações -----------
async function atualizar() {
  if (!cfg.ip) { mostrarConexao(false, 'Informe o IP do ESP32'); return; }
  try {
    mostrarDados(await chamar('/status'));
    falhas = 0;
    mostrarConexao(true, 'Conectado a ' + cfg.ip);
  } catch (e) {
    tratarErro(e);
  }
}

async function enviar(acao) {
  if (!cfg.ip) return;
  try {
    mostrarDados(await chamar('/' + acao));
    falhas = 0;
    mostrarConexao(true, 'Conectado a ' + cfg.ip);
    atualizarLog();
  } catch (e) {
    tratarErro(e);
    $('aviso').textContent = 'Falha ao enviar o comando. Tente novamente.';
  }
}

// IP, MAC, sinal, hora e uptime do ESP32
async function atualizarInfo() {
  if (!cfg.ip) return;
  try {
    const i = await chamar('/info');
    const h = Math.floor(i.uptime_s / 3600), m = Math.floor((i.uptime_s % 3600) / 60);
    $('info').textContent = 'IP ' + i.ip + ' · MAC ' + i.mac + ' · ' + i.ssid + ' (' + i.rssi + ' dBm) · ' +
      i.hora + (i.hora_sincronizada ? '' : ' (sem NTP)') + ' · ligado há ' + h + ' h ' + m + ' min';
  } catch (e) { /* o estado de conexão é tratado em atualizar() */ }
}

// ----------- Log -----------
function montarLinhaLog(linha) {
  const partes = linha.split('|');
  if (partes.length < 3) return null;
  const tr = document.createElement('tr');
  const tdHora = document.createElement('td');
  tdHora.className = 'hora';
  tdHora.textContent = partes[0];
  const tdNivel = document.createElement('td');
  const tag = document.createElement('span');
  tag.className = 'nivel ' + partes[1];
  tag.textContent = partes[1];
  tdNivel.appendChild(tag);
  const tdMsg = document.createElement('td');
  tdMsg.textContent = partes.slice(2).join('|');   // textContent: sem risco de injetar HTML
  tr.append(tdHora, tdNivel, tdMsg);
  return tr;
}

async function atualizarLog() {
  if (!cfg.ip) return;
  const qtd = $('logQtd').value;
  try {
    const texto = await chamar('/log?n=' + qtd + (nivelLog ? '&nivel=' + nivelLog : ''), 'texto');
    const linhas = texto.split('\n').filter((l) => l.trim() !== '').reverse();   // mais novo primeiro
    const corpo = $('logCorpo');
    corpo.replaceChildren();
    linhas.forEach((l) => { const tr = montarLinhaLog(l); if (tr) corpo.appendChild(tr); });
    $('logResumo').textContent = linhas.length ? linhas.length + ' registro(s)' : 'Nenhum registro para este filtro';
  } catch (e) {
    $('logResumo').textContent = (e.message === 'login') ? 'Usuário ou senha incorretos' : 'Não foi possível ler o log';
  }
}

async function baixarLog() {
  try {
    const texto = await chamar('/log?n=1000', 'texto');
    const link = document.createElement('a');
    link.href = URL.createObjectURL(new Blob([texto], { type: 'text/plain;charset=utf-8' }));
    link.download = 'saturn-log.txt';
    link.click();
    URL.revokeObjectURL(link.href);
  } catch (e) {
    $('logResumo').textContent = 'Não foi possível baixar o log';
  }
}

async function limparLog() {
  if (!confirm('Apagar todo o log do ESP32?')) return;
  try {
    await chamar('/log/limpar');
    atualizarLog();
  } catch (e) {
    $('logResumo').textContent = 'Não foi possível limpar o log';
  }
}

// ----------- Início -----------
$('btnSalvar').addEventListener('click', salvarCfg);
$('btnLigar').addEventListener('click', () => enviar('ligar'));
$('btnDesligar').addEventListener('click', () => enviar('desligar'));
$('btnLogAtualizar').addEventListener('click', atualizarLog);
$('btnLogBaixar').addEventListener('click', baixarLog);
$('btnLogLimpar').addEventListener('click', limparLog);
$('logQtd').addEventListener('change', atualizarLog);
document.querySelectorAll('.logBarra button[data-nivel]').forEach((b) => {
  b.addEventListener('click', () => {
    nivelLog = b.dataset.nivel;
    document.querySelectorAll('.logBarra button[data-nivel]').forEach((x) => x.classList.toggle('ativo', x === b));
    atualizarLog();
  });
});

carregarCfg();
atualizar();
atualizarInfo();
atualizarLog();
setInterval(atualizar, INTERVALO_MS);
setInterval(atualizarInfo, INTERVALO_INFO_MS);
setInterval(atualizarLog, INTERVALO_LOG_MS);
