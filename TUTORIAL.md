# Tutorial: celular como webcam com o GodoiCam

São duas partes: o app no **celular** e o app no **PC**. Os dois precisam estar abertos ao mesmo tempo. Instalação: veja o [README](README.md).

## Primeira vez no PC

1. Rode `GodoiCam-Setup.exe` da [Release](https://github.com/ramongodoii/GodoiCam/releases) (ou `instalar.cmd` se você compilou). Aceite o administrador.
2. O GodoiCam do PC abre sozinho. Depois, abra pelo menu Iniciar: **GodoiCam**.
3. Se o Windows disser que o aplicativo não é reconhecido: **Mais informações → Executar assim mesmo**.

Sem o `instalar.cmd`, a conexão até funciona, mas Zoom/Teams/OBS **não** vêem a câmera.

## Conectar pelo Wi‑Fi

1. Celular e PC no **mesmo Wi‑Fi**.
2. Abra o GodoiCam no celular (aceite a câmera) e no PC.
3. O PC tenta conectar sozinho. Na **primeira vez** aparece no PC: *Digite o código de 6 dígitos que apareceu no celular*. Olhe o celular, digite os números e clique em **Parear**.
4. Pronto: a janela do PC vira o vídeo. No seletor do topo o aparelho aparece como `… [Wi‑Fi]`.

**Das próximas vezes:** abra os dois apps. Não precisa de código.

Se o celular não aparecer: no PC, menu **☰ → Conectar pelo IP…** e use o IP que o celular mostra no selo do topo (toque no selo se o IP não estiver visível).

## Usar no Zoom, Teams, Meet, OBS

Com os dois conectados, no app de vídeo escolha a câmera **GodoiCam**.

- Zoom: Configurações → Vídeo → Câmera
- Teams: Configurações → Dispositivos → Câmera
- Google Meet: ⋮ → Configurações → Vídeo
- OBS: Fontes → + → Dispositivo de captura de vídeo

Se o app de vídeo já estava aberto antes de instalar a câmera, feche e abra de novo.

## Cabo USB (mais estável)

1. No celular: **Opções do desenvolvedor → Depuração USB**. (Sem essa tela: toque 7 vezes em **Número de compilação** em Sobre o telefone.)
2. Samsung: desligue o **Bloqueador automático** se a depuração não aparecer para o PC.
3. Ligue o cabo. No celular, **Permitir depuração USB** (marque *Sempre permitir*).
4. Abra o GodoiCam nos dois. O PC prefere o USB sozinho se a depuração estiver ok. Também dá: menu **☰ → Conectar pelo cabo USB**.

O PC precisa do `adb` (Android Studio ou pasta `adb\adb.exe` ao lado do GodoiCam.exe).

## No celular

- Toque na imagem para focar; toque duas vezes para o automático.
- Dois dedos: zoom. A área **clara** da prévia é o que o PC recebe.
- **Ajustes**: câmera, resolução, FPS, qualidade, preencher o quadro, Modo Pro (ISO, obturador, foco, Kelvin).
- **Tela preta**: some a prévia e baixa o brilho; a câmera segue. Toque duas vezes para voltar.
- Para parar: **Desligar** nos Ajustes ou na notificação.

## No PC

- Seletor no topo: celular na rede, cabo USB, *Conectar pelo IP…*
- Menu **☰**: desconectar, conectar automaticamente, vídeo preto sem celular, iniciar com o Windows, estatísticas, câmera do celular (zoom, lanterna, ISO…), reinstalar câmera, esquecer pareados, Sobre, sair.
- Fechar a janela deixa o app na bandeja. **Sair** no menu encerra de verdade.

## Se não conectar

| O que acontece | O que fazer |
|---|---|
| Nenhum celular no seletor | Mesmo Wi‑Fi? App aberto no celular? ☰ → IP com o endereço do selo. |
| Código não aparece | Deixe o GodoiCam **na tela** do celular (não só em segundo plano). |
| “Código incorreto” | O celular gera outro; use o novo. |
| USB não pega | Depuração USB ligada? Permissão “Permitir” aceita? Bloqueador automático do Samsung desligado? |
| GodoiCam não aparece no Zoom/Teams | Rode o Setup de novo (ou `instalar.cmd`), ☰ → Reinstalar câmera virtual, reabra o app de vídeo. |
| Imagem de lado | Ajustes do celular → Correção de rotação, ou desligue “Girar com o celular”. |
| Imagem travando no Wi‑Fi | Ajustes → Qualidade → Normal, ou use o cabo. |
