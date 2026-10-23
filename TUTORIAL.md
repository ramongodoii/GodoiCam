# Tutorial: usar o celular como webcam com o GodoiCan

São duas partes: o app no **celular** (você já instalou) e o app no **PC**. Os dois precisam estar abertos ao mesmo tempo.

## Parte 1: preparar o PC (só uma vez)

1. Abra a pasta `D:\GodoiCan\windows`.
2. Dê dois cliques em **`instalar.cmd`**.
3. O Windows pergunta "Deseja permitir que este aplicativo faça alterações?". Clique em **Sim**.
   - Isso instala a câmera virtual "GodoiCan", que é o que aparece no Zoom, Teams, Meet e OBS.
   - Sem esse passo, a conexão funciona, mas nenhum app de vídeo enxerga a câmera.
4. No final, o GodoiCan do PC abre sozinho. Nas próximas vezes, abra pelo menu Iniciar: digite **GodoiCan**.

## Parte 2: conectar pelo Wi‑Fi (primeira vez)

1. **Celular e PC na mesma rede Wi‑Fi** (o mesmo roteador).
2. **No celular:** abra o GodoiCan. No topo deve aparecer "Aguardando o PC" e o IP do celular (ex.: `192.168.1.6`).
3. **No PC:** abra o GodoiCan. Em **Celulares na rede** aparece o nome do celular (ex.: "S24 de Ramon"). Pode levar alguns segundos.
4. Clique no nome do celular e depois em **Conectar (Wi‑Fi)**.
5. **Olhe o celular:** aparece um cartão "Código de pareamento" com 6 números (ex.: `681 834`).
6. **No PC:** digite os 6 números no campo **Código de pareamento** e clique em **Parear** (ou aperte Enter).
   - Você tem cerca de 2 minutos. Se o tempo acabar ou errar o código, clique em **Conectar** de novo: o celular mostra um código novo.
7. Pronto: o PC mostra a imagem do celular e o status "Transmitindo de…". O celular mostra "Transmitindo para (nome do PC)".

**Das próximas vezes:** basta abrir o GodoiCan no celular e no PC. O PC reconecta sozinho, sem código.

## Parte 3: usar no Zoom, Teams, Meet, OBS…

1. Com os dois apps conectados, abra o app de vídeo.
2. Nas configurações de câmera/vídeo, escolha **GodoiCan**.
   - Zoom: Configurações → Vídeo → Câmera.
   - Teams: Configurações → Dispositivos → Câmera.
   - Google Meet: ⋮ → Configurações → Vídeo → Câmera.
   - OBS: Fontes → + → Dispositivo de captura de vídeo → GodoiCan.
3. Se o app de vídeo já estava aberto antes de instalar a câmera, feche e abra de novo.

## Alternativa: conectar pelo cabo USB (mais estável)

1. No celular, ative as **Opções do desenvolvedor**: Configurações → Sobre o telefone → Informações do software → toque **7 vezes** em **Número de compilação**.
2. Volte em Configurações → **Opções do desenvolvedor** → ligue **Depuração USB**.
3. Ligue o cabo no PC. No celular aparece "Permitir depuração USB?": marque **Sempre permitir** e toque em **Permitir**.
4. Abra o GodoiCan no celular e, no PC, clique em **Conectar por USB**.
5. Na primeira vez, digite o código de pareamento, igual ao Wi‑Fi.

## Se não conectar

| O que acontece | O que fazer |
|---|---|
| A lista "Celulares na rede" fica vazia | Confira se os dois estão no mesmo Wi‑Fi. Se o roteador bloquear, digite o IP que aparece no topo do celular em **Ou digite o IP** e clique em **Conectar IP**. |
| O celular está aberto mas o PC não acha | No celular, toque em **Desligar** e depois **Ligar**. Desative VPNs no PC (ex.: Radmin VPN) se continuar. |
| O código não aparece no celular | Deixe o GodoiCan aberto na tela do celular (o código também aparece numa notificação). |
| "Código incorreto" | Espere 3 segundos, clique em **Conectar** de novo e use o código novo. |
| Imagem de lado ou de cabeça para baixo | No celular: **Ajustes → Correção de rotação**. |
| A câmera GodoiCan não aparece no Zoom/Teams | Rode `instalar.cmd` de novo, clique em **Reinstalar câmera** no GodoiCan do PC e reabra o app de vídeo. |
| Imagem travando no Wi‑Fi | No celular, **Ajustes → Qualidade → Normal**, ou use o cabo USB. |

## Dicas no celular

- Toque na imagem para focar; toque duas vezes para voltar ao foco automático.
- Dois dedos para zoom.
- **Tela preta** apaga a tela e economiza bateria; a câmera continua funcionando. Toque duas vezes para voltar.
- Para parar, toque em **Desligar** ou use a notificação.
