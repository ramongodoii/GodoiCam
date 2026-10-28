# GodoiCam

Use um celular **Android** como webcam de um PC com **Windows 11**. Gratuito, sem anúncios, sem marca d'água e sem telemetria. O código é aberto ([MIT](LICENSE)).

O vídeo vai do celular ao PC **só na sua rede** (Wi‑Fi ou cabo USB), cifrado. Não existe servidor nosso no meio.

```
celular  →  H.264  →  Wi‑Fi ou USB (AES‑256‑GCM)  →  PC  →  câmera virtual "GodoiCam"
                                                              Zoom, Teams, Meet, OBS, Discord…
```

**Versão atual: 0.1.0.** Código e instaladores: [github.com/ramongodoii/GodoiCam](https://github.com/ramongodoii/GodoiCam). O mapa do que ainda falta (áudio, Play Store…) está em [`docs/LANCAMENTO.md`](docs/LANCAMENTO.md).

## O que precisa

| | |
|---|---|
| Celular | Android 8 ou mais novo, com câmera |
| PC | **Windows 11** (a câmera virtual usa uma API que o Windows 10 não tem) |
| Rede | Mesmo Wi‑Fi **ou** cabo USB com Depuração USB ligada |

**Nesta versão não há áudio.** Só a imagem. O microfone do PC continua sendo o do PC.

## Instalar

Baixe a [Release mais recente](https://github.com/ramongodoii/GodoiCam/releases): o APK do celular e o ZIP do Windows.

### Celular

1. Instale `GodoiCam-0.1.0.apk`.
2. Abra o GodoiCam e aceite a permissão de **câmera**.

O Android avisa que o app não veio da Play Store. Isso é esperado.

Quem for compilar: o APK sai em `android/app/build/outputs/apk/release/app-release.apk`.

### PC

1. Baixe `GodoiCam-Setup.exe` na [Release](https://github.com/ramongodoii/GodoiCam/releases) e execute. Aceite o administrador (só nesta vez).
2. Se o SmartScreen bloquear, use **Mais informações → Executar assim mesmo**.
3. Quem preferir o ZIP antigo: extraia e rode `instalar.cmd` (mesmo efeito).

O instalador copia o app para `C:\Program Files\GodoiCam`, registra a câmera virtual, libera o firewall e cria o atalho no menu Iniciar. Nas próximas vezes, abra **GodoiCam** pelo menu Iniciar.

Para remover: `windows\desinstalar.cmd`.

**USB:** o PC precisa do `adb`. Se você tem o Android Studio, ele já encontra o SDK. Senão, baixe as [platform-tools](https://developer.android.com/tools/releases/platform-tools) e coloque a pasta como `adb` ao lado do `GodoiCam.exe` (fica `adb\adb.exe`). Não redistribuímos o `adb` da Google neste repositório.

## Usar

O passo a passo com imagens mentais da tela atual está em [`TUTORIAL.md`](TUTORIAL.md). Resumo:

1. Abra o GodoiCam no **celular** e no **PC** (mesma rede, ou o cabo).
2. Na **primeira vez**, o PC pede o código de 6 dígitos que aparece no celular. Digite e confirme.
3. Depois disso, basta abrir os dois: o PC conecta sozinho. USB tem preferência se o cabo estiver pronto.
4. No Zoom, Teams, Meet, OBS etc., escolha a câmera **GodoiCam**.

No celular: toque para focar, dois dedos para zoom, folha de **Ajustes** (resolução, qualidade, Modo Pro). **Tela preta** apaga a prévia e economiza bateria; a câmera continua. Para parar de verdade, **Desligar** ou a notificação.

No PC: o seletor no topo lista o celular (`[Wi‑Fi]` / cabo USB). O menu **☰** tem IP, USB, câmera do celular, iniciar com o Windows, reinstalar a câmera.

Atalhos: `GodoiCam.exe --usb` ou `GodoiCam.exe --ip 192.168.0.15`.

## Qualidade (taxa de bits)

| Qualidade | 1080p30 | 4K30 |
|---|---|---|
| Econômica | ~5 Mbps | ~20 Mbps |
| Normal | ~9 Mbps | ~35 Mbps |
| Alta (padrão) | ~15 Mbps | ~60 Mbps |
| Máxima | ~22 Mbps | ~80 Mbps |

Em Wi‑Fi 5 GHz ou USB, deixe **Alta**. Em Wi‑Fi 2,4 GHz congestionado, **Normal**. Se a rede não acompanhar, o celular baixa o bitrate sozinho para não atrasar.

Atraso típico no S24, 1080p30, Wi‑Fi 5 GHz: cerca de **100–160 ms** da captura até o PC.

## Privacidade

- A câmera só é usada neste aparelho, enquanto o app está ligado.
- O vídeo vai **somente** ao PC que você pareou, na rede local ou no cabo. Não sobe para a nuvem.
- Não há conta, anúncio, analytics nem chamada de internet nossa.
- O pareamento usa código de 6 dígitos + ECDH P‑256; a sessão inteira vai em AES‑256‑GCM. Detalhes: [`docs/PROTOCOLO.md`](docs/PROTOCOLO.md).
- Chaves no celular: armazenamento privado do app. No PC: `%APPDATA%\GodoiCam\pairs.dat` (DPAPI).
- O log `%APPDATA%\GodoiCam\log.txt` não guarda imagem nem chaves.

## Limites

- Só **vídeo** (sem microfone do celular).
- Só **Windows 11** no PC.
- USB pela **Depuração USB** (`adb`). Sem ela, use Wi‑Fi ou Ancoragem USB + IP.
- Primeira instalação no PC pede administrador.

## Compilar

**Android** (Android Studio / JDK 17 + SDK):

```bash
cd android
./gradlew assembleRelease
```

O APK sai em `android/app/build/outputs/apk/release/app-release.apk`.

Para um APK de distribuição (sempre o mesmo “app” no celular), crie `android/keystore.properties` a partir de `android/keystore.properties.example` e um `.jks`. Sem esse arquivo, o release é assinado com a chave de **debug** local — serve só na sua máquina.

**Windows** (Visual Studio 2022 com C++):

```bash
windows\build.cmd
```

Gera `windows\bin\GodoiCam.exe` e `windows\bin\GodoiCamVCam.dll`. O instalador único (`GodoiCam-Setup.exe`) sai com `windows\build-setup.cmd` (precisa do [Inno Setup 6](https://jrsoftware.org/isinfo.php)). Quem não for gerar o Setup pode continuar com `windows\instalar.cmd`.

## Estrutura do repositório

| Pasta | Conteúdo |
|---|---|
| `android/` | App Android (Kotlin, minSdk 26) |
| `windows/vcam/` | Câmera virtual (DLL do Frame Server) |
| `windows/app/` | GodoiCam.exe |
| `windows/common/` | Memória compartilhada entre exe e DLL |
| `docs/` | Protocolo, medições, roteiro, [lançamento](docs/LANCAMENTO.md) |

## Licença

[MIT](LICENSE). Copyright (c) 2026 Ramon Godoi.
