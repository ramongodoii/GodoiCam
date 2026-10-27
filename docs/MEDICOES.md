# Fase 0 — Medições e diagnóstico

Base para comparar cada melhoria das próximas fases. Tudo medido no **Samsung S24 (SM‑S921B)** e no PC do Ramon.

## Ferramentas

| Ferramenta | Para quê | Como usar |
|---|---|---|
| `windows\medir-atraso-godoicam.cmd` | Atraso vidro a vidro da câmera GodoiCam, lida como um app de vídeo lê | Abra o GodoiCam no celular e no PC. Rode o arquivo; a tela pisca preto/branco. Encoste a câmera traseira do celular na tela (a tela deve ocupar a imagem toda). Espere 30 medições. |
| `windows\medir-atraso-iriun.cmd` | O mesmo, para o Iriun | Feche o GodoiCam nos dois lados, abra o Iriun e repita. |
| `%APPDATA%\GodoiCam\atraso.txt` | Resultado de cada medição (p50, p95, todos os valores) | Gerado pelas duas ferramentas acima. |
| `%APPDATA%\GodoiCam\stats.csv` | Uma linha por segundo da transmissão: FPS da câmera e enviado, Mbps, quadros descartados, fila, tempo de codificação, temperatura, FPS e tempo de decodificação no PC, atraso captura→PC, RTT, câmera virtual em uso | Gerado sozinho pelo GodoiCam do PC. Abre no Excel (separador `;`). |
| `%APPDATA%\GodoiCam\relatorio-<celular>.txt` | Capacidades do celular: câmeras, lentes, FPS, resoluções, encoders H.264/HEVC/AV1, recursos de baixa latência | Enviado pelo celular ao conectar. Também em Ajustes → Diagnóstico → Compartilhar relatório. |
| `windows\bin\camprobe.exe list` / `read <nome>` | Abre qualquer câmera pelo Media Foundation e mostra formatos, tempo para o 1º quadro e FPS | Diagnóstico da câmera virtual. |
| `GODOICAM_DUMP=arquivo` + `decodetest.exe` | Grava o vídeo recebido e reproduz no decodificador | Depurar problemas de imagem. |

> Celular e PC precisam estar com a versão nova (Fase 0) para gerar `relatorio-*.txt` e as colunas `cel_*` do `stats.csv`.

## Resultados até agora

### Câmera virtual (Windows 11) — verificada em 23/10/2026

- Instalada com `instalar.cmd`; aparece como **"GodoiCam (Câmera Virtual do Windows)"**.
- **Media Foundation** (Teams, Meet no Chrome/Edge, app Câmera): abre em 33 ms, 1º quadro em 41 ms, **30,8 fps**, maior intervalo entre quadros 49 ms. Oferece NV12 em 640×360 até 3840×2160, 30 e 60 fps, e RGB32 em 720p/1080p.
- **DirectShow** (OBS, Zoom, ffmpeg): funciona; com o S24 conectado entregou a imagem real em 1920×1080. Também oferece YUY2 (conversão automática do Windows).
- Observação: o nome tem um espaço especial ("do Windows"); programas que pedem o nome digitado (ffmpeg) precisam do ID do dispositivo.

### Transmissão S24 → PC (sem medidor ainda)

- Encoder usado: `c2.exynos.h264.encoder` (hardware), 1920×1080 a 30 fps, ~10 Mbps (qualidade "Alta").
- Emulador (referência, encoder em software): 25 fps, atraso captura→PC ~69 ms, RTT 7 ms.

### S24 real, Wi‑Fi, 1080p30 "Alta" — 23/10/2026 18:34 (20 s de `stats.csv`)

| Etapa | Média | Mín–máx |
|---|---|---|
| FPS enviado / recebido no PC | 29,1 / 29,1 | 17–31 |
| Bitrate real | 9,4 Mbps | 4,9–10,8 |
| **Captura → saída do encoder (celular)** | **86 ms** | 65–97 |
| Rede (RTT) | 7 ms | 3–15 |
| Decodificação no PC (software) | 9,9 ms | 7,8–11,9 |
| **Captura → decodificado no PC** | **100 ms** | 78–112 |
| Temperatura da bateria | 37,0 °C | — |
| Quadros descartados | 0 | — |

**Conclusão:** quase todo o atraso está no celular, entre a exposição do sensor e a saída do encoder (86 de 100 ms). Rede e PC somam ~15 ms. A Fase 5 deve atacar primeiro o encoder e o pipeline da câmera.

### Relatório do S24 (resumo)

- Android 16 (API 36), Exynos 2400 (s5e9945).
- Câmeras: traseira principal (id 0, zoom 0,6×–10× via câmera lógica com físicas 2/5/6), ultra‑angular (id 2), frontal (ids 1 e 3).
- 1080p e 720p até 60 fps; 1440p e 4K até 30 fps; relógio REALTIME (o atraso medido é confiável).
- Encoders de hardware: `c2.exynos.h264.encoder`, `c2.exynos.hevc.encoder` e as variantes **`.wfd`** (feitas para espelhamento de tela, normalmente de baixa latência). Nenhum anuncia `FEATURE_LowLatency` nem intra‑refresh. Suportam VBR e CBR até 240 Mbps.

### Incidente 23/10/2026 21:43 — atraso de segundos depois das Fases 3/4

- Celular normal: 30 fps capturados e codificados, codificação ~90 ms (igual à base).
- Rede: RTT de **3 a 13 s** (base: 7 ms); PC recebendo em rajadas (1–111 fps).
- Celular a **43 °C, estado térmico 3 (severo)**; base era 37 °C.
- Fila do app sempre 0: o atraso estava escondido no buffer de envio do sistema (vários MB), então o app nunca descartava quadros nem baixava o bitrate a tempo.
- **Correção:** buffer de envio do socket em 64 KB (~50 ms a 10 Mbps), descarte de quadros com mais de 200 ms na fila e aviso de congestionamento quando um envio leva mais de 150 ms (baixa bitrate + quadro‑chave).

### Incidente 24/10/2026 — atraso grande, FPS caindo e oscilando no Wi‑Fi

Diagnóstico pelo código (sem `stats.csv` desta sessão):

1. **PC atrasava o próprio recebimento.** Uma única thread lia o socket, decifrava, decodificava (~10 ms), copiava para a câmera virtual e convertia a prévia para BGRA no tamanho da janela (CPU, 10–40 ms com a janela grande ou maximizada). Passando de 33 ms por quadro, os dados se acumulavam no buffer de recepção de 1 MB (~0,8 s a 10 Mbps) sem nenhum dos lados perceber.
2. **Controle de congestionamento do celular disparava à toa.** Com 2 quadros na fila já cortava FPS (× 2/3) e bitrate (× 3/4) ao mesmo tempo, e só recuperava após 3 s calmos. O quadro‑chave a cada 2 s (grande) quase sempre gerava um novo corte: FPS em serra entre 10 e 30.
3. **Sem medida real da rede.** Com 64 KB de buffer de envio, picos de RTT do Wi‑Fi limitavam a vazão abaixo do bitrate; sem confirmação do PC, o celular não distinguia rede lenta de PC lento.

**Correção:** confirmação por quadro (`FRAME_ACK`) e janela de envio de ~200 ms no celular; congestionamento corta bitrate antes de FPS; quadro‑chave só a pedido (intervalo de 10 s); câmera com `TEMPLATE_PREVIEW`; ritmo de FPS reduzido sem buracos; no PC, rede, decodificação e prévia em threads separadas, com salto até o quadro‑chave se a decodificação atrasar mais de 150 ms. Detalhes em `docs/PROTOCOLO.md`.

**A medir no S24:** `stats.csv` ganhou `cel_rede_ms` (envio → confirmação) e `pc_descartados`. Esperado no Wi‑Fi 5 GHz: `cel_rede_ms` < 30, `cel_env_fps` estável em 30, `pc_descartados` 0, `atraso_captura_ate_pc_ms` < 100.

**Medido no S24 (24/10/2026, Wi‑Fi 5 GHz):** usuário confirmou "sem delay".

| Modo | `cel_rede_ms` p50 | Codificação p50 | Decodificação PC | Atraso captura→PC p50 | Observação |
|---|---|---|---|---|---|
| 1080p30 | 7 ms | ~98 ms | ~9 ms (CPU) | 115 ms | estável |
| 1080p60 | baixo | — | — | — | estável |
| 4K30 | — | ~160 ms | 20–64 ms (CPU) | — | 183 quadros descartados no PC ("decodificação atrasada") |

- `TEMPLATE_PREVIEW` não ganhou nada (~98 ms de codificação contra ~86 ms com RECORD): voltou para `TEMPLATE_RECORD`.
- No primeiro minuto, soluços do Wi‑Fi (RTT até 714 ms com RSSI −24) ao trocar de câmera; o bitrate levou ~15 s para voltar de 2,5 a 10 Mbps. Agora sobe 35 %/s quando `cel_rede_ms` < 30 (10 %/s nos demais casos).

### Decodificação por hardware no PC — 24/10/2026

D3D11/DXVA na placa de vídeo com maior memória dedicada (RTX 4060), com volta automática para a CPU se a placa recusar a configuração ou falhar 5 vezes seguidas. Banco de teste: `decodetest.exe` com 180 quadros sintéticos (ffmpeg `testsrc2`, High, 20 Mbps).

| Resolução | CPU (média) | GPU (média) |
|---|---|---|
| 3840×2160 | 21,6 ms | 8,2 ms |
| 1920×1080 | 9,6 ms | 3,0 ms |

A imagem decodificada é idêntica nas duas (mesma soma de conferência no quadro 100). O primeiro quadro na GPU leva ~50 ms (aquecimento). O app mostra "(GPU)" ou "(CPU)" nas estatísticas e o `stats.csv` ganhou a coluna `pc_gpu`.

**Medido no S24 (25/10/2026 18:05, Wi‑Fi 5 GHz, `pc_gpu` = 1):**

| Modo | Decodificação PC | Codificação celular | Atraso captura→PC | `cel_rede_ms` | `pc_descartados` |
|---|---|---|---|---|---|
| 1080p30 | ~5 ms | 80–90 ms | ~100 ms (79–142) | 7–37 | 0 |
| 4K30 | 10–18 ms (antes 20–64) | 105–115 ms | 125–200 ms | 6–70 | 0 (antes 183) |

- **Bug achado:** no 4K o bitrate‑alvo caiu de 31,85 para 0,05 Mbps sem congestionamento. A recuperação calculava `bitrate × 135` em `Int` e estourava acima de ~16 Mbps (com o passo antigo de 110 %, acima de ~19,5 Mbps). Com o bitrate no chão, o celular passou a cortar FPS (30 → 16). Corrigido com a conta em `Long`.
- No 4K, o maior pedaço do atraso agora é a codificação no celular (~110 ms).

### Vidro a vidro (preencher)

| Cenário | GodoiCam p50 | GodoiCam p95 | Iriun p50 | Iriun p95 |
|---|---|---|---|---|
| Wi‑Fi, 1080p30 | | | | |
| Wi‑Fi, 720p30 | | | | |
| USB, 1080p30 | | | | |

### Sustentação 10 min, 1080p30 Wi‑Fi (preencher a partir do `stats.csv`)

| Métrica | Início | 10 min |
|---|---|---|
| FPS enviado / recebido | | |
| Codificação (ms) | | |
| Atraso captura→PC (ms) | | |
| Temperatura do celular (°C) | | |
| Quadros descartados (total) | | |

### Incidente 25/10/2026 22:01 — travamento ao tentar matar o chiado

Tentativa: filtro bilateral 5×5 na GPU (25 leituras da textura OES, captura 4K) + "pouca luz" forçada (`[15,30]` fps) + NR `HIGH_QUALITY`.

| Métrica | Antes (21:49, só bitrate 15 Mbps) | Depois (22:01, filtro + pouca luz) |
|---|---|---|
| FPS da câmera | 30 | 14–31 (oscilando) |
| FPS enviado | 30 | **6–14** (alvo caiu para 10) |
| Atraso captura→PC | ~160 ms | **125–939 ms** |
| `cel_rede_ms` | ~10 | **200–640** |
| Bitrate alvo | 14,9 Mbps | 3,7 Mbps (corte de congestionamento) |

**Conclusão:** o chiado da frontal é ruído de sensor; filtro pesado na GPU não cabe neste pipeline (a thread GL alimenta o encoder). Revertido. Captura em pé passa a 1440p, não 4K. NR fica em `FAST`.

## Achados de diagnóstico

1. **Câmera virtual OK** nos dois caminhos (Media Foundation e DirectShow). Não é a causa dos problemas relatados.
2. Os problemas de interface, rotação e menu lento têm causa no código (ver `docs/ROADMAP.md`, seção "Diagnóstico") e não dependem de medição.
3. Atraso e qualidade só podem ser comparados com o Iriun depois da tabela "vidro a vidro" acima.
