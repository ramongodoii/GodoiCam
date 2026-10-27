# GodoiCam — até poder postar (open source, grátis)

Objetivo: qualquer pessoa com **Android 8+** e **Windows 11** baixar, instalar e usar de graça, com o código aberto.

Este arquivo é o mapa. Atualize as caixas quando algo terminar. O `docs/ROADMAP.md` continua sendo o roteiro de **recursos**; isto aqui é o que impede (ou não) a **postagem**.

Última revisão: 27/10/2026 (licença MIT, README/tutorial, versão 0.1.0, keystore local).

---

## O que já está pronto para o público

O app **já funciona** no uso diário (testado no S24 + Windows 11):

- Celular Android como webcam do PC, Wi‑Fi ou cabo USB
- Câmera virtual **GodoiCam** no Zoom, Teams, Meet, OBS, Discord, app Câmera
- Conexão sozinha depois do primeiro pareamento (código de 6 dígitos)
- Vídeo cifrado (ECDH + AES‑256‑GCM); sem anúncios, sem marca d'água, sem telemetria
- Atraso ~100–160 ms em 1080p30 no Wi‑Fi 5 GHz
- Modo Pro no celular e controle pelo PC
- Sem servidor na internet: só rede local / USB

Isso basta para um **v0.1 no GitHub**. Não precisa igualar o Iriun, nem áudio, nem Play Store, para a primeira postagem.

---

## Definição de “pode postar”

Pode abrir o repositório e publicar a **Release v0.1.0** quando **todos** os bloqueios da seção A estiverem marcados. Play Store, F‑Droid, instalador MSI e áudio ficam para depois (seção C).

---

## A — Bloqueios (sem isso não posta)

### A1. Licença e identidade

- [x] Arquivo `LICENSE` na raiz (MIT, copyright Ramon Godoi 2026).
- [x] Nome do autor no `LICENSE` e no README.
- [x] README diz com clareza: gratuito, sem telemetria, o que a câmera faz, Windows 11 only.

### A2. Repositório público

- [ ] Git remoto (GitHub é o caminho mais simples). Hoje **não há `git remote`**.
- [ ] Commit de tudo o que está só na máquina (renomeação GodoiCan→GodoiCam, Modo Pro, USB, qualidade, etc.). **Ainda não está commitado.**
- [x] `.gitignore` cobrindo build, `.idea`, `local.properties`, `*.jks`, `*.keystore` (não vazar chave de assinatura).
- [ ] Nada de `%APPDATA%\GodoiCam\` (log, `stats.csv`, `pairs.dat`, relatório do S24) no git.

### A3. Quem baixa consegue instalar (sem Visual Studio)

Hoje só quem **compila** usa. Um desconhecido precisa de arquivos prontos:

- [ ] **Release no GitHub** com APK + ZIP do Windows (`GodoiCam.exe`, DLL, `instalar.cmd`, `desinstalar.cmd`).
- [x] Keystore de release local (`android/godoicam-release.jks` + `keystore.properties`, fora do git). O Gradle usa isso se o arquivo existir; senão cai na chave debug.
- [x] README de instalação **sem** caminho `D:\GodoiCam`. Tutorial alinhado à UI atual.
- [x] Aviso no README: o Windows SmartScreen pode recusar o `.exe` não assinado.

### A4. Honestidade (limites que o usuário precisa ver na primeira tela do README)

- [x] Só **vídeo**. Sem microfone do celular.
- [x] PC: **Windows 11**.
- [x] USB exige **Depuração USB**.
- [x] Primeira instalação do PC pede **administrador**.
- [x] Tabela de bitrate do README conferida (~15 Mbps em 1080p30 Alta).

### A5. Segurança e privacidade (o código já é razoável; falta escrever)

- [x] Texto curto de privacidade no README.
- [ ] Conferir que `log.txt` e o relatório do aparelho **não** gravam imagem nem chaves (hoje a intenção é essa; vale uma passagem no código antes da tag).
- [ ] Pasta `docs/` de medições pode citar o S24, mas **não** precisa ir para a Release; se for no git, tire nomes pessoais demais (“PC do Ramon”) ou deixe só como laboratório.

---

## B — Deveria estar no v0.1 (não bloqueia o git init, bloqueia uma Release “boa”)

- [x] **Sobre** no menu do PC e nos Ajustes do celular (versão + MIT). Link do repositório quando o GitHub existir.
- [x] `CHANGELOG.md` com o que o v0.1 faz.
- [ ] Capturas de tela (celular + janela do PC + GodoiCam no OBS) na pasta `docs/screenshots/` e no README.
- [x] `adb` explicado no README (SDK ou pasta `adb\`; não redistribuímos o binário da Google).
- [ ] Aviso na tela do PC quando o celular está no cabo **sem** Depuração USB (código já existe em `System.cpp` / `main.cpp`; **ainda não está na pasta instalada** `C:\Program Files\GodoiCam` se o `instalar.cmd` não rodou depois do build).
- [x] Tutorial alinhado à UI atual (seletor no topo, conexão automática, menu ☰).
- [ ] Teste mínimo de “estranho na primeira vez”:
  - [ ] Instalar APK num Android que não seja o S24
  - [ ] `instalar.cmd` num PC Win11 limpo (ou outro usuário)
  - [ ] Wi‑Fi: abrir os dois apps → código → vídeo
  - [ ] Escolher GodoiCam no app Câmera do Windows e no OBS
  - [ ] USB com depuração ligada
- [x] Versão **0.1.0** no Android e no manifesto do exe.

---

## C — Depois da postagem (não atrasa o GitHub)

| Item | Por que espera |
|---|---|
| Áudio (microfone virtual) | Grande, Fase 8; o Iriun também vende isso à parte. v0.1 é só webcam. |
| Instalador único `.exe`/`.msi` | Facilita, mas `instalar.cmd` + ZIP resolve o v0.1. |
| USB sem Depuração USB | Depende de modo acessório/MTP; incerto. |
| Vários celulares ao mesmo tempo | O seletor já troca de aparelho; duas câmeras virtuais é outro projeto. |
| Play Store | Conta de desenvolvedor, política de privacidade **em URL**, Data safety, revisão. Primeiro o GitHub. |
| F‑Droid | Exige licença, build reproduzível, sem binários proprietários. Natural depois do GitHub. |
| Assinatura Authenticode (Windows) | Some o SmartScreen; custa certificado. |
| HEVC, UDP, 60 fps perfeito, chiado da frontal | Qualidade (Fases 5–6); não impede usar. |
| Comparação vidro a vidro com o Iriun | Laboratório interno (`docs/MEDICOES.md`). |
| CI (GitHub Actions compilando APK e exe) | Conforto para quem contribui. |

---

## Ordem de trabalho até a Release v0.1.0

Faça **nesta ordem**. Uma caixa por vez; não misturar com áudio nem Play Store.

1. ~~Licença MIT e copyright Ramon.~~
2. ~~LICENSE, README, TUTORIAL, bitrate, limites, privacidade.~~
3. ~~`.gitignore` (jks/keystore fora do git).~~
4. ~~Keystore de release local.~~
5. Commit único (ou poucos) da linha atual GodoiCam.
6. Criar o repositório no GitHub e dar `git push`.
7. Gerar APK + `windows\build.cmd` + ZIP, tag `v0.1.0`, GitHub Release.
8. Teste “estranho na primeira vez” (seção B).
9. Anúncio: README é a loja. Sem Play Store neste passo.

Quando o item 7 estiver feito, **pode postar**.

---

## Estado do código (27/10/2026)

| Área | Situação |
|---|---|
| Licença | MIT (`LICENSE`) |
| Git remote | **Nenhum** |
| Trabalho local não commitado | Muito (rename, Pro, USB, qualidade, docs de lançamento…) |
| APK release | Keystore local (fora do git); falta a Release pública |
| Instalador Windows | `instalar.cmd` (pede admin); não é MSI |
| `adb` | Não vem no ZIP; procura SDK ou pasta `adb\` |
| Windows 10 / macOS / iOS | Fora de escopo |
| Áudio | Não existe |
| Telemetria / anúncio / conta | Não existe (bom) |
| Permissões Android | Câmera, rede, serviço em primeiro plano, notificação. Sem localização. `allowBackup=false` |
| Criptografia | Pareamento com código + ECDH P‑256 + AES‑GCM (`docs/PROTOCOLO.md`) |

---

## Decisões que só você pode tomar

1. Licença: MIT ou GPL‑3?
2. Conta/organização no GitHub e se o repositório é `godoicam` público já no v0.1.
3. Play Store neste ano ou só GitHub?
4. Copyright: seu nome legal ou “GodoiCam authors”?

Com as respostas de 1 e 2 dá para executar os passos 2–7.
