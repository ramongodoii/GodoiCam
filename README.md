# GodoiCan

Use o celular Android como webcam do PC com Windows 11, por Wi‑Fi ou cabo USB.
Uso pessoal e gratuito: sem anúncios, sem marca d'água, sem telemetria.

```
celular: Camera2 → GPU (gira/espelha/enquadra) → encoder H.264 de hardware
        → TCP cifrado (AES‑256‑GCM) por Wi‑Fi ou USB
PC:     decodificador H.264 do Windows → memória compartilhada
        → câmera virtual "GodoiCan" (Media Foundation) → Zoom, Teams, Meet, OBS, Discord…
```

## Estrutura

| Pasta | Conteúdo |
|---|---|
| `android/` | App Android (Kotlin, minSdk 26) |
| `windows/vcam/` | Câmera virtual: DLL carregada pelo Frame Server do Windows |
| `windows/app/` | GodoiCan.exe: descoberta, pareamento, recepção, decodificação, prévia |
| `windows/common/` | Contrato da memória compartilhada e conversões de imagem |
| `windows/test/` | `decodetest`: reproduz um fluxo gravado no decodificador |
| `docs/PROTOCOLO.md` | Protocolo de rede e criptografia |

## Compilar

**Android** (precisa do Android SDK e do JDK do Android Studio):

```bash
cd android && ./gradlew assembleRelease
```

O APK sai em `android/app/build/outputs/apk/release/app-release.apk` (assinado com a chave de debug local; serve para instalar no seu próprio celular).

**Windows** (precisa do Visual Studio 2022 com C++):

```bash
windows\build.cmd
```

Gera `windows\bin\GodoiCan.exe` e `windows\bin\GodoiCanVCam.dll`.

## Instalar

1. Celular: instale o APK (copie para o celular ou `adb install app-release.apk`) e abra o GodoiCan. Aceite a permissão de câmera.
2. PC: rode `windows\instalar.cmd` e aceite o pedido de administrador. Ele copia o app para `C:\Program Files\GodoiCan`, registra a câmera virtual, libera o app no firewall e cria o atalho no menu Iniciar.
3. Ao abrir o GodoiCan no PC pela primeira vez, a câmera virtual "GodoiCan" é criada. Ela continua disponível mesmo com o app fechado (mostra uma tela de espera).

Para remover tudo: `windows\desinstalar.cmd`.

## Usar

**Wi‑Fi** (PC e celular na mesma rede):

1. Abra o GodoiCan no celular. Ele mostra o IP no topo da tela.
2. No PC, o celular aparece em "Celulares na rede". Clique em **Conectar (Wi‑Fi)**. Se a lista ficar vazia (redes que bloqueiam broadcast), digite o IP e clique em **Conectar IP**.
3. Na primeira vez, o celular mostra um código de 6 dígitos. Digite no PC e clique em **Parear**. Depois disso, o PC reconecta sozinho.

**USB** (mais estável, menos atraso):

1. No celular, ative **Opções do desenvolvedor → Depuração USB** e conecte o cabo.
2. Autorize o PC no aviso que aparece no celular.
3. No PC, clique em **Conectar por USB**. O app usa o `adb` do Android SDK (ou coloque a pasta `platform-tools` como `adb\` ao lado do GodoiCan.exe).

Atalhos de linha de comando: `GodoiCan.exe --usb` ou `GodoiCan.exe --ip 192.168.0.15`.

Depois, no Zoom, Teams, Meet, OBS etc., escolha a câmera **GodoiCan**.

### No celular

- Toque na imagem para focar e medir a luz naquele ponto; toque duas vezes para voltar ao automático.
- Pinça com dois dedos para zoom (inclui a ultra‑angular em 0,5× nos aparelhos que a expõem assim).
- **Ajustes**: câmera, resolução (720p até 4K, conforme o aparelho), 24/30/60 fps, qualidade, espelhar, preencher o quadro, rotação, exposição, balanço de branco, travar exposição, estabilização e modo de pouca luz.
- **Tela preta**: apaga a prévia e baixa o brilho para economizar bateria. Toque duas vezes para voltar.
- O serviço continua com a tela desligada. Use **Desligar** (ou a notificação) para parar.

## Qualidade e atraso

| Qualidade | 1080p30 | 4K30 |
|---|---|---|
| Econômica | ~4 Mbps | ~15 Mbps |
| Normal | ~6 Mbps | ~25 Mbps |
| Alta (padrão) | ~10 Mbps | ~40 Mbps |
| Máxima | ~16 Mbps | ~62 Mbps |

- Sem B‑frames, prioridade de tempo real, decodificador do Windows em modo de baixa latência (cada quadro sai assim que chega).
- Se a rede não acompanha, o celular descarta quadros velhos, pede um quadro‑chave e baixa o bitrate; depois recupera aos poucos. A imagem fica atual em vez de atrasar.
- O PC mostra o atraso estimado da captura até a decodificação e o tempo de rede. Em Wi‑Fi de 5 GHz ou USB, prefira "Alta"; em Wi‑Fi de 2,4 GHz congestionado, "Normal".

## Segurança

- Pareamento com código de 6 dígitos mostrado só na tela do celular, sobre uma troca de chaves ECDH P‑256. Quem só escuta a rede não descobre a chave; um código errado encerra a tentativa e o celular gera outro.
- Toda a sessão (controle e vídeo) é cifrada e autenticada com AES‑256‑GCM, com chaves novas a cada conexão.
- As chaves ficam no armazenamento privado do app no celular e, no PC, em `%APPDATA%\GodoiCan\pairs.dat` protegido pelo DPAPI do Windows.
- Detalhes em `docs/PROTOCOLO.md`.

## Problemas comuns

- **O celular não aparece na lista**: alguns roteadores isolam os aparelhos ou bloqueiam broadcast. Use o IP mostrado no celular. Confira se a rede do Windows não está como "Pública" com o firewall bloqueando.
- **A câmera GodoiCan não aparece no app de vídeo**: rode `instalar.cmd` de novo, clique em **Reinstalar câmera** no GodoiCan e reinicie o app de vídeo.
- **Imagem de cabeça para baixo ou de lado**: em Ajustes, use **Correção de rotação** ou desligue **Girar com o celular**.
- **Diagnóstico**: o PC grava `%APPDATA%\GodoiCan\log.txt` (sem imagens nem chaves). Com a variável `GODOICAN_DUMP=arquivo`, grava o vídeo recebido para testar com `windows\bin\decodetest.exe arquivo largura altura`.

## Limites desta versão

- Só vídeo (o áudio do microfone fica para uma próxima etapa).
- Só Windows 11 no PC (a câmera virtual usa a API do Windows 11).
- USB via `adb` (exige depuração USB). Alternativa sem adb: ative o **Ancoragem USB** no celular e conecte pelo IP.
