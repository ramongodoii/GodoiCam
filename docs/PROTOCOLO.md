# Protocolo GodoiCan v1

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
| 0x01 HELLO | PC→Cel | `"GDCN"`, u16 versão=1, pcId[16], u8 flags (bit0 = forçar pareamento), pcNonce[16], pcPub[65] (P-256 não comprimido), str nomePC, u32 caps |
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

O PC manda PING a cada segundo. O celular encerra a sessão após 10 s sem receber nada.
