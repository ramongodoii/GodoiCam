# Fase 0 — Medições e diagnóstico

Base para comparar cada melhoria das próximas fases. Tudo medido no **Samsung S24 (SM‑S921B)** e no PC do Ramon.

## Ferramentas

| Ferramenta | Para quê | Como usar |
|---|---|---|
| `windows\medir-atraso-godoican.cmd` | Atraso vidro a vidro da câmera GodoiCan, lida como um app de vídeo lê | Abra o GodoiCan no celular e no PC. Rode o arquivo; a tela pisca preto/branco. Encoste a câmera traseira do celular na tela (a tela deve ocupar a imagem toda). Espere 30 medições. |
| `windows\medir-atraso-iriun.cmd` | O mesmo, para o Iriun | Feche o GodoiCan nos dois lados, abra o Iriun e repita. |
| `%APPDATA%\GodoiCan\atraso.txt` | Resultado de cada medição (p50, p95, todos os valores) | Gerado pelas duas ferramentas acima. |
| `%APPDATA%\GodoiCan\stats.csv` | Uma linha por segundo da transmissão: FPS da câmera e enviado, Mbps, quadros descartados, fila, tempo de codificação, temperatura, FPS e tempo de decodificação no PC, atraso captura→PC, RTT, câmera virtual em uso | Gerado sozinho pelo GodoiCan do PC. Abre no Excel (separador `;`). |
| `%APPDATA%\GodoiCan\relatorio-<celular>.txt` | Capacidades do celular: câmeras, lentes, FPS, resoluções, encoders H.264/HEVC/AV1, recursos de baixa latência | Enviado pelo celular ao conectar. Também em Ajustes → Diagnóstico → Compartilhar relatório. |
| `windows\bin\camprobe.exe list` / `read <nome>` | Abre qualquer câmera pelo Media Foundation e mostra formatos, tempo para o 1º quadro e FPS | Diagnóstico da câmera virtual. |
| `GODOICAN_DUMP=arquivo` + `decodetest.exe` | Grava o vídeo recebido e reproduz no decodificador | Depurar problemas de imagem. |

> Celular e PC precisam estar com a versão nova (Fase 0) para gerar `relatorio-*.txt` e as colunas `cel_*` do `stats.csv`.

## Resultados até agora

### Câmera virtual (Windows 11) — verificada em 23/10/2026

- Instalada com `instalar.cmd`; aparece como **"GodoiCan (Câmera Virtual do Windows)"**.
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

### Vidro a vidro (preencher)

| Cenário | GodoiCan p50 | GodoiCan p95 | Iriun p50 | Iriun p95 |
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

## Achados de diagnóstico

1. **Câmera virtual OK** nos dois caminhos (Media Foundation e DirectShow). Não é a causa dos problemas relatados.
2. Os problemas de interface, rotação e menu lento têm causa no código (ver `docs/ROADMAP.md`, seção "Diagnóstico") e não dependem de medição.
3. Atraso e qualidade só podem ser comparados com o Iriun depois da tabela "vidro a vidro" acima.
