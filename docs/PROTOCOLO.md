# Protocolo GodoiCam v1

Celular = servidor. PC = cliente. Tudo em big-endian.

## Portas

| Porta | Uso |
|---|---|
| TCP 47180 | Sessão (pareamento, controle e vídeo) |
| UDP 47181 | Descoberta na rede local |

USB: o PC executa `adb forward tcp:47180 tcp:47180` e conecta em `127.0.0.1:47180`.
O protocolo é o mesmo, inclusive autenticação.

## Descoberta (UDP)

- PC → broadcast `GDCN?1`
- Celular → resposta unicast `GDCN!1|<phoneIdHex>|<tcpPort>|<nome>`

## Enquadramento (TCP)

```
u8  tipo
u32 tamanho
..  payload (cifrado após autenticação: ciphertext || tag GCM de 16 bytes)
```

Strings: `u16 tamanho` + UTF-8.

## Handshake (texto claro)

| Tipo | Direção | Conteúdo |
|---|---|---|
| 0x01 HELLO | PC→Cel | `"GDCN"`, u16 versão=1, pcId[16], u8 flags (bit0 = forçar pareamento), pcNonce[16], pcPub[65] (P-256 não comprimido), str nomePC, u32 caps (bit0 = o PC envia FRAME_ACK) |
| 0x02 CHALLENGE | Cel→PC | phoneId[16], phoneNonce[16], str nomeCel (já pareados) |
| 0x03 PAIR_CHALLENGE | Cel→PC | phoneId[16], phoneNonce[16], phonePub[65], str nomeCel (precisa de código) |
| 0x04 AUTH | PC→Cel | mac[32] |
| 0x05 AUTH_OK | Cel→PC | mac[32] |
| 0x06 ERROR | ambos | u8 código, texto UTF-8 |

Códigos de erro: 1 não pareado, 2 código errado, 3 ocupado, 4 versão, 5 autenticação, 6 tempo esgotado.

### Já pareados (chave K de 32 bytes guardada nos dois lados)

```
ids = pcId || phoneId || pcNonce || phoneNonce
AUTH    = HMAC(K, "GDCN-auth-pc"    || ids)
AUTH_OK = HMAC(K, "GDCN-auth-phone" || ids)
```

Se o PC não tem chave para o `phoneId` recebido, envia ERROR 1 e reconecta com a flag de forçar pareamento.

### Pareamento

O celular sorteia um código de 6 dígitos e mostra na tela. O usuário digita no PC.

```
Z = ECDH-P256(pcPriv, phonePub)            (coordenada X, 32 bytes)
P = SHA256("GDCN-pair" || Z || código ASCII)
T = pcPub || phonePub || pcNonce || phoneNonce || pcId || phoneId
AUTH    = HMAC(P, "GDCN-pair-pc"    || T)
AUTH_OK = HMAC(P, "GDCN-pair-phone" || T)
K       = HMAC(P, "GDCN-pair-key"   || T)
```

Código errado: o celular encerra e sorteia outro código (uma tentativa por código).

### Chaves de sessão

```
k_cel→pc = HMAC(K, "GDCN-key-p2c" || pcNonce || phoneNonce)
k_pc→cel = HMAC(K, "GDCN-key-c2p" || pcNonce || phoneNonce)
```

AES-256-GCM, nonce = 4 bytes zero || u64 contador (começa em 0, um por mensagem e direção),
AAD = byte do tipo.

## Mensagens cifradas

| Tipo | Direção | Conteúdo |
|---|---|---|
| 0x10 CONFIG | Cel→PC | u16 codec (1 = H.264), u16 largura, u16 altura, u16 fps, u32 bitrate, str descrição |
| 0x11 VIDEO | Cel→PC | u8 flags (1 = quadro-chave, 2 = SPS/PPS), i64 pts µs, dados H.264 Annex-B |
| 0x12 PING | PC→Cel | i64 tempo do PC µs |
| 0x13 PONG | Cel→PC | i64 tempo do PC µs, i64 tempo do celular µs (mesmo relógio do pts) |
| 0x14 KEYFRAME_REQ | PC→Cel | vazio |
| 0x15 BYE | ambos | vazio |
| 0x16 REPORT | Cel→PC | texto UTF-8 (relatório do aparelho) |
| 0x17 STATS | Cel→PC | estatísticas de 1 s; campos novos só são acrescentados no fim |
| 0x18 FRAME_ACK | PC→Cel | i64 pts do VIDEO recebido (não enviado para SPS/PPS) |
| 0x19 CONTROL | PC→Cel | u8 chave, u32 valor com sinal (controle remoto da câmera, abaixo) |
| 0x1A CAMERA_STATE | Cel→PC | estado da câmera (abaixo); enviado ao iniciar a sessão, ao abrir a câmera e a cada ajuste |

Quem não conhece um tipo o ignora, então PC e celular de versões diferentes continuam funcionando (só sem o controle remoto).

### Controle remoto da câmera (CONTROL)

| Chave | Valor |
|---|---|
| 1 trocar câmera | ignorado |
| 2 lanterna | 0 / 1 |
| 3 zoom | zoom × 100 |
| 4 compensação de exposição | passos da câmera (ver `passos por EV`) |
| 5 ISO | 0 = automático |
| 6 tempo de exposição | µs; 0 = automático |
| 7 foco | dioptrias × 100 (0 = infinito); negativo = automático |
| 8 temperatura de cor | Kelvin; 0 = automático |
| 9 tudo automático | ignorado (exposição, foco e branco voltam ao automático) |

O Camera2 não combina ISO manual com obturador automático: com qualquer um dos dois manual, a exposição automática desliga e o outro fica no último valor automático.

### CAMERA_STATE

```
str  nome da câmera ("Traseira", "Frontal"…)
u8   número de câmeras
u8   flags: bit0 tem lanterna, bit1 lanterna ligada, bit2 aceita ISO/obturador manual,
            bit3 aceita foco manual, bit4 aceita Kelvin, bit5 foco calibrado (dioptrias reais)
u16  zoom × 100, u16 zoom mínimo × 100, u16 zoom máximo × 100
i16  compensação atual, i16 mínima, i16 máxima (passos), u16 passos por EV
u16  ISO (0 = automático), u16 ISO mínimo, u16 ISO máximo
u32  exposição µs (0 = automático), u32 mínima, u32 máxima (já limitada a um quadro do FPS atual)
u16  foco × 100 (0xFFFF = automático), u16 foco mais próximo × 100 (0 = foco fixo)
u16  Kelvin (0 = automático)
```

O PC manda PING a cada segundo. O celular encerra a sessão após 10 s sem receber nada.

### Controle de congestionamento (com caps bit0)

O PC confirma cada VIDEO assim que o lê do socket, antes de decodificar. O celular:

- limita o vídeo em trânsito (enviado e ainda não confirmado) a ~200 ms do bitrate atual (mínimo 192 KB). O quadro que não cabe espera na fila do app, onde pode ser descartado; o buffer do sistema fica em 1 MB só para não limitar a vazão;
- mede o atraso envio → confirmação. Média suavizada acima de 120 ms, ou quadro esperando mais de 100 ms pela janela, conta como congestionamento leve (bitrate × 0,8). Quadros descartados (fila com 8 ou espera acima de 300 ms) contam como severo (bitrate × 0,6 e quadro-chave). O FPS só cai quando o bitrate já está no piso (1/4 do alvo);
- recupera depois de 2 s sem congestionamento: +5 fps por segundo até o alvo, depois bitrate +10% por segundo (+35% quando o atraso até o PC está abaixo de 30 ms).

Sem o bit (PC antigo), o celular usa o modo anterior: buffer de envio de 64 KB e congestionamento pelo tamanho da fila.

No PC, se um quadro espera mais de 150 ms para ser decodificado (ou há mais de 6 na fila), ele pula até o próximo quadro-chave e pede um, para voltar ao tempo real.
