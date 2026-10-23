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
