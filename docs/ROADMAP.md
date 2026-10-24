# GodoiCan — roteiro de melhorias por fase

Objetivo: chegar à experiência do Iriun (abre o app no celular e já conecta; a janela do PC é só o vídeo), com menos atraso e mais qualidade. Uma fase por vez; cada fase só termina quando os testes dela passarem no **S24 (SM‑S921B)** e no PC real.

Legenda: **[Falta]** recurso ausente · **[Melhorar]** existe mas está ruim · **[Analisar]** precisa investigar antes de mexer · **[Testar]** critério de aceite.

---

## Como o Iriun se comporta (referência)

- **PC sem celular:** um painel simples, "No phone available", dicas (mesmo Wi‑Fi, app aberto) e duas opções: "Blank video when no connection" e "Start with my computer".
- **PC com celular:** a janela inteira vira o vídeo. No topo, um seletor com o aparelho ("Samsung SM‑S921B [WiFi]"); à esquerda, um menu ☰. Sem botões, sem formulários.
- **Celular pareado:** abriu o app → conectou. Nenhum clique no PC.

---

## Diagnóstico do que está ruim hoje

| Sintoma relatado | Causa provável no código |
|---|---|
| Câmera vira um quadradinho no celular | A saída é sempre 16:9 deitada. Com o celular em pé, a imagem é girada 90° e encaixada com faixas pretas (`GlRenderer.contentScale`, modo "ajustar"), e a prévia ainda é encaixada em outra caixa 16:9 (`letterbox`). Resultado: um retrato dentro de um paisagem dentro da tela em pé. |
| Botões saindo da tela | Barra inferior com 5 botões de texto num `HorizontalScrollView`; painel de ajustes com largura fixa de 340dp; layout pensado só para paisagem. |
| Vídeo pequeno no PC | A prévia é gerada em no máximo 640 px de largura e ocupa só a área à direita dos controles (`Session.cpp`, `main.cpp Layout`). |
| Ao girar, tudo pisca | (1) A Activity gira (`fullSensor`) e recria a Surface da prévia a cada giro; (2) a rotação de saída muda no meio do fluxo e a imagem alterna entre faixas laterais e tela cheia; (3) no PC, `UpdateStatus` apaga a prévia em qualquer estado diferente de "Transmitindo" e a pintura não usa buffer duplo. |
| Menu de ajustes lento/travando | `populateSettings` roda na thread principal e chama `CameraCaps.resolutions`, que monta a `MediaCodecList` inteira várias vezes; cada troca em um Spinner chama `restartPipeline` (fecha e reabre câmera e encoder); o painel ainda é repreenchido 600 ms depois. |
| Precisa clicar para conectar | O PC só reconecta sozinho ao "último celular"; o primeiro pareamento exige clicar em Conectar e digitar código. |

---

## Fase 0 — Medir antes de mexer

> **Status (23/10/2026): feita.** Ferramentas e primeiros números em `docs/MEDICOES.md`. Falta só a comparação vidro a vidro com o Iriun (precisa do celular apontado para a tela).

Sem números não dá para saber se uma mudança melhorou o atraso ou a qualidade.

- **[Falta]** Tela/linha de diagnóstico ligável (celular e PC): FPS da câmera, FPS enviado, bitrate real, quadros descartados, tempo de encode, RTT, atraso estimado, temperatura do celular.
- **[Falta]** Registro no celular equivalente ao `log.txt` do PC (sem imagem nem chaves), exportável por "Compartilhar".
- **[Analisar]** Capacidades reais do S24: tamanhos e FPS da câmera traseira/frontal/ultra‑angular, encoders (`c2.exynos`/`c2.qti`), perfis H.264/HEVC, suporte a `KEY_LOW_LATENCY`, intra‑refresh, CBR. Gerar um relatório pelo próprio app (botão "Copiar relatório do aparelho").
- **[Analisar]** Instalar a câmera virtual (`instalar.cmd`) e confirmar se "GodoiCan" aparece e mostra imagem no app Câmera do Windows, Zoom, Teams, Meet e OBS. **Ainda não foi testado.**
- **[Testar]** Atraso "vidro a vidro": filmar com outro celular, em câmera lenta, um cronômetro na tela do PC e a imagem dele passando pelo GodoiCan. Anotar p50/p95 em Wi‑Fi 5 GHz, 2,4 GHz e USB, a 720p30 e 1080p30. Fazer o mesmo com o Iriun para ter a referência.

**Pronto quando:** existe uma tabela de base (atraso, FPS, bitrate, temperatura em 10 min) para GodoiCan e Iriun no mesmo PC e celular.

---

## Fase 1 — Conexão sem cliques (estilo Iriun)

> **Status (23/10/2026): feita, testada no emulador.**
> - O PC conecta sozinho a qualquer celular pareado (USB > último usado > outro pareado), sem clique.
> - Celular não pareado sozinho na rede: o PC abre a tela do código uma vez.
> - Cabo USB detectado sozinho (`adb devices` a cada 3 s).
> - O celular se anuncia na rede a cada 2 s enquanto espera um PC; o PC reage na hora.
> - Queda curta: a câmera virtual segura o último quadro por 3 s e a sessão reconecta.
> - Testado: reconexão depois de reiniciar o app do celular; pareamento novo pela tela do PC.
> - **Pendente:** trocar de Wi‑Fi para USB sozinho quando o cabo é ligado no meio da transmissão; testar o anúncio no S24 real.

- **[Melhorar]** PC conecta sozinho a qualquer celular **pareado** que aparecer na rede ou no USB, sem depender de "último celular".
- **[Melhorar]** Primeiro pareamento sem botão: ao achar um celular não pareado, o PC já abre a etapa do código (campo grande, foco automático). No celular, o código aparece em tela cheia.
- **[Falta]** Detectar o cabo USB sozinho (`adb devices` periódico ou aviso de dispositivo) e preferir USB quando houver.
- **[Falta]** Celular anuncia "pronto" assim que abre; PC reage em até 2 s (hoje o broadcast é a cada 2 s; avaliar 1 s ou resposta ativa do celular).
- **[Melhorar]** Reconexão silenciosa: queda curta de Wi‑Fi não pode virar tela de erro; manter o último quadro por até 2 s e reconectar.
- **[Analisar]** VPNs no PC (há uma Radmin VPN ativa) e redes que bloqueiam broadcast: escolher a interface certa e cair para varredura da sub‑rede se o broadcast falhar.
- **[Testar]** Celular pareado: abrir o app → vídeo no PC em menos de 3 s, sem tocar no PC. Repetir 20 vezes.
- **[Testar]** Desligar e ligar o Wi‑Fi do celular, sair do alcance e voltar, trocar de USB para Wi‑Fi: volta sozinho.
- **[Testar]** Código errado, código expirado, dois PCs tentando o mesmo celular.

---

## Fase 2 — Interface do PC

> **Status (23/10/2026): feita.**
> - Janela = vídeo, redimensionável, buffer duplo (sem piscar). A prévia é gerada no tamanho exato da janela (bilinear), não mais 640 px.
> - Barra fina: menu ☰ + seletor ("S24 de Ramon [Wi‑Fi]", "[USB]", "Conectar pelo IP…") + estado com ponto colorido.
> - Tela de espera no estilo do Iriun; código de pareamento e IP num painel central.
> - Menu ☰: IP, USB, desconectar, conectar automaticamente, vídeo preto sem conexão, iniciar com o Windows, estatísticas na tela, (re)instalar câmera, esquecer celulares, pasta de diagnóstico, sair.
> - Fechar a janela deixa o app na bandeja; "Iniciar com o Windows" abre minimizado.
> - **Pendente:** desenho por GPU (Direct2D/D3D) se a CPU pesar em 4K; teste em monitor com escala 150%/200%.

- **[Melhorar]** Janela = vídeo. Ocupa toda a área, mantém proporção e redimensiona livremente. Desenho com buffer duplo (sem piscar).
- **[Melhorar]** Prévia em resolução cheia (ou proporcional ao tamanho da janela), não 640 px. Avaliar desenhar com Direct3D/Direct2D em vez de GDI.
- **[Falta]** Barra superior discreta: seletor de aparelho ("S24 de Ramon [Wi‑Fi]" / "[USB]") e menu ☰ com: resolução/FPS, espelhar, câmera, instalar câmera, esquecer aparelhos, iniciar com o Windows, sobre.
- **[Falta]** Tela "Nenhum celular" no estilo Iriun: mensagem clara + dicas + opções.
- **[Falta]** Ícone na bandeja do sistema, iniciar minimizado, opção "Iniciar com o Windows".
- **[Falta]** Opção "Vídeo preto sem conexão" (hoje a câmera virtual mostra a tela de espera do GodoiCan).
- **[Melhorar]** Estatísticas fora do caminho: só no menu ou numa sobreposição ligável.
- **[Testar]** Redimensionar, maximizar, monitor com escala 100%/150%/200%, girar o celular: nada pisca nem sai do lugar.

---

## Fase 3 — Interface do celular

> **Status (23/10/2026): feita, testada no emulador em retrato e paisagem.**
> - Prévia na tela inteira, sempre em pé; a área que vai para o PC fica clara e o resto escurecido.
> - Selo de estado no topo (toque mostra IP e estatísticas); 4 botões redondos com ícones; atalhos de zoom (0,6× / 1× / 2× / 3×…).
> - Ajustes numa folha inferior; "Desligar" foi para dentro dela; respeita barras e recorte da câmera.
> - **Pendente:** conferir no S24 com fonte grande e modo escuro.

- **[Melhorar]** Prévia em tela cheia, preenchendo a tela do celular (cortando bordas), em retrato e paisagem. A prévia mostra a câmera; o que vai para o PC aparece como uma moldura indicando a área enviada.
- **[Melhorar]** Barra de controles com ícones (não texto), que caiba em retrato e paisagem sem rolar: trocar câmera, lanterna, ajustes, tela preta. "Desligar" vira ícone ou fica na notificação.
- **[Melhorar]** Ajustes em uma folha inferior (bottom sheet) que se adapta à orientação, em vez do painel fixo de 340dp.
- **[Melhorar]** Status compacto: um selo "Conectado • S24 → RAMON • Wi‑Fi" e o IP só quando não há conexão.
- **[Falta]** Indicador de zoom com atalhos (0,6× / 1× / 2× / 3×) usando as lentes do S24.
- **[Testar]** S24 em retrato e paisagem, modo escuro, tamanho de fonte grande: nenhum botão cortado.

---

## Fase 4 — Rotação e sincronia

> **Status (23/10/2026): feita (rotação); sincronia de áudio fica para a Fase 8.**
> - Celular em pé: o PC recebe o 16:9 preenchido (sem faixas). Opção em Ajustes para voltar às faixas.
> - Ao girar, a prévia só muda de tamanho (a Surface não é recriada) e a imagem do PC só gira depois de 500 ms parado, com um quadro‑chave novo.
> - O PC não apaga mais a imagem em quedas curtas.
> - **Pendente:** girar 20 vezes numa chamada real no Teams com o S24.

Aqui "sincronia" = a imagem do PC acompanhar o celular sem tremer, travar ou atrasar, e (mais tarde) o áudio alinhado com o vídeo.

- **[Analisar]** Comportamento desejado ao girar. Proposta: a saída para o PC é sempre 16:9; celular em pé → corta o centro (preenche), sem faixas pretas. Opção para "retrato com faixas" para quem quiser.
- **[Melhorar]** Travar a orientação da Activity e girar só os controles (como os apps de câmera), para a Surface não ser recriada a cada giro.
- **[Melhorar]** Ao girar, trocar a rotação da saída numa transição limpa (um quadro‑chave novo) e com histerese maior, para não ficar alternando perto de 45°.
- **[Melhorar]** No PC, nunca apagar a prévia por mudança de estado transitória; só ao desconectar de verdade.
- **[Analisar]** Ritmo de entrega na câmera virtual: hoje ela espera o próximo quadro ou 1,5 intervalo. Verificar se Zoom/Teams recebem quadros regulares (sem "engasgos") e se o timestamp das amostras deveria vir do celular.
- **[Testar]** Girar o celular 20 vezes durante uma chamada no Teams: nenhuma piscada, nenhum quadro torto, nenhuma queda.

---

## Fase 5 — Diminuir o atraso

Cada item deve ser medido com a tabela da Fase 0 antes e depois.

- **[Analisar]** Onde o tempo está: captura → GPU → encode → rede → decode → câmera virtual → app. Adicionar timestamps por etapa.
- **[Melhorar]** Encoder: testar `KEY_LOW_LATENCY`, intra‑refresh (sem quadros‑chave gigantes), `KEY_OPERATING_RATE`, CBR vs VBR no S24.
- **[Melhorar]** Decodificação por hardware no PC (DXVA/D3D11) em vez do decodificador por software; hoje cada quadro passa por várias cópias na CPU (decode → memória compartilhada → câmera virtual → redimensionamento).
- **[Melhorar]** Entregar à câmera virtual exatamente a resolução que o app pediu (pedir ao celular a resolução certa) para evitar redimensionar na CPU.
- **[Analisar]** Transporte: TCP com fila curta (atual) vs UDP com descarte de pacotes atrasados. Manter TCP no USB; testar UDP no Wi‑Fi.
- **[Melhorar]** Buffers de socket menores e envio por quadro sem acumular; medir a fila real no celular.
- **[Testar]** Meta: p50 ≤ 100 ms e p95 ≤ 150 ms em 1080p30 no Wi‑Fi 5 GHz; menor no USB. Comparar com o Iriun.

---

## Fase 6 — Qualidade da imagem

- **[Analisar]** Comparar lado a lado com o Iriun no mesmo cenário (rosto, pouca luz, movimento, texto) gravando a saída das duas câmeras virtuais no OBS.
- **[Melhorar]** Usar o tamanho de sensor ideal (hoje escolhe o menor ≥ saída; avaliar um maior com redução na GPU para mais nitidez).
- **[Melhorar]** Shader com precisão alta nas coordenadas (`highp`) para não perder nitidez em 1080p/4K.
- **[Analisar]** Faixa e matriz de cor (BT.601/709, faixa limitada/cheia) entre encoder, decodificador e câmera virtual: imagem lavada ou saturada indica descompasso.
- **[Melhorar]** Bitrate padrão pela rede (USB/5 GHz/2,4 GHz) e HEVC opcional quando o PC tiver o decodificador.
- **[Melhorar]** Modos da câmera: `TEMPLATE_RECORD` vs `TEMPLATE_PREVIEW`, redução de ruído, nitidez, 60 fps real no S24.
- **[Testar]** 30 min em 1080p30 e 1080p60: FPS estável, sem aquecimento excessivo, sem quadros corrompidos.

---

## Fase 7 — Ajustes rápidos e completos

> **Adiantado na Fase 3:** capacidades da câmera/encoder em cache e qualidade muda o bitrate na hora (sem reiniciar). O resto continua pendente.

- **[Melhorar]** Calcular as capacidades da câmera e do encoder **uma vez** (em segundo plano, com cache) e nunca na thread principal.
- **[Melhorar]** Separar o que reinicia a câmera (câmera, resolução, FPS) do que é instantâneo (exposição, balanço de branco, espelho, zoom, qualidade via `setBitrate`). Qualidade não deve reiniciar nada.
- **[Melhorar]** Trocar resolução/câmera sem derrubar a conexão e sem tela preta longa (reaproveitar o encoder quando der).
- **[Falta]** Controles do Iriun Pro: ISO, tempo de exposição, foco manual, temperatura de cor em Kelvin.
- **[Falta]** Controle remoto pelo PC (menu ☰ do PC muda câmera, zoom, exposição no celular).
- **[Testar]** Abrir o menu e trocar cada opção: resposta em menos de 100 ms para ajustes instantâneos e menos de 1 s para troca de câmera/resolução.

---

## Fase 8 — Recursos que faltam

- **[Falta]** Áudio do microfone do celular (AAC/Opus), sincronizado com o vídeo, saindo num microfone virtual (VB‑CABLE ou driver próprio). Inclui medir o atraso A/V.
- **[Falta]** Celular aparece como "[USB]" ou "[Wi‑Fi]" e troca automática para USB quando o cabo é ligado.
- **[Falta]** Mais de um celular (seleção no PC).
- **[Falta]** Instalador único (um `.exe`/`.msi`) em vez de `instalar.cmd`, incluindo o `adb`.
- **[Analisar]** USB sem depuração USB (ancoragem USB ou modo acessório).

---

## Fase 9 — Bateria de testes final

- Matriz: Wi‑Fi 5 GHz / 2,4 GHz / USB × 720p30 / 1080p30 / 1080p60 × câmera traseira / frontal / ultra‑angular.
- Apps: app Câmera do Windows, Zoom, Teams, Google Meet (Chrome e Edge), OBS, Discord.
- Situações: tela do celular desligada, ligação recebida, bateria fraca, celular esquentando, Wi‑Fi caindo, PC hibernando e voltando, reinstalar o app.
- Segurança: outro PC na mesma rede não consegue ver a câmera; código errado bloqueia; "Esquecer" funciona dos dois lados.
- Sessão longa: 1 h em chamada sem travar, sem vazamento de memória, sem atraso crescente.

---

## Ordem sugerida

1. **Fase 0** (medir) — curta, mas destrava todo o resto.
2. **Fase 1 + Fase 2** (conectar sozinho e janela do PC só com vídeo) — é o que mais aproxima do Iriun no uso diário.
3. **Fase 3 + Fase 4** (tela do celular, rotação sem piscar).
4. **Fase 7** (ajustes rápidos).
5. **Fase 5** e **Fase 6** (atraso e qualidade, sempre comparando com o Iriun).
6. **Fase 8** e **Fase 9**.
