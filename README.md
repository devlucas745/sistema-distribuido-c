# Sistema de Chat Distribuído (C / Sockets TCP)

Projeto em C que implementa um chat cliente-servidor com salas, mensagens
privadas, transferência de arquivos e criptografia básica.

## Conceitos demonstrados

| Conceito                  | Onde está no código                                             |
|----------------------------|------------------------------------------------------------------|
| Sockets TCP                | `socket()`, `bind()`, `listen()`, `accept()`, `connect()`         |
| Threads                    | uma `pthread` por cliente no servidor + thread de recepção no cliente |
| Comunicação Cliente-Servidor | protocolo de texto baseado em linhas, com tags `@@TAG@@`        |
| Concorrência                | múltiplos clientes atendidos simultaneamente por threads         |
| Sincronização (Mutex)      | `pthread_mutex_t clients_mutex` protege a lista global de clientes |
| Broadcast                  | `broadcast_room()` envia a mensagem a todos os membros da sala   |
| Login                      | usuário escolhe um nome único ao conectar                        |
| Criptografia                | cifra XOR + Base64 para mensagens privadas (`/wenc`, `/decrypt`) |
| Transferência de arquivos  | `/sendfile`, arquivo enviado em blocos Base64 roteados pelo servidor |
| Chat privado                | `/msg` (texto puro) e `/wenc` (texto cifrado)                    |

## Estrutura do projeto

```
chat_distribuido/
├── common/
│   └── common.h        # protocolo, base64, cifra XOR, leitura/escrita de linhas
├── server/
│   └── server.c        # servidor multi-thread
├── client/
│   └── client.c        # cliente com thread de recepção
├── Makefile
└── README.md
```

## Compilação

Requer `gcc` e a biblioteca pthreads (padrão em qualquer Linux/macOS).

```bash
make
```

Isso gera `bin/server` e `bin/client`.

## Executando

Em um terminal, inicie o servidor (porta padrão 5050):

```bash
./bin/server            # usa a porta 5050
./bin/server 6000       # ou especifique outra porta
```

Em outro(s) terminal(is), inicie um ou mais clientes:

```bash
./bin/client                       # conecta em 127.0.0.1:5050
./bin/client 127.0.0.1 6000        # ip e porta customizados
```

Ao conectar, o cliente pedirá um nome de usuário único. Depois disso, tudo o
que for digitado (sem `/` na frente) é enviado como mensagem para a sala
atual (por padrão, `geral`).

## Comandos disponíveis no cliente

| Comando                                | Descrição                                               |
|-----------------------------------------|-----------------------------------------------------------|
| `/join <sala>`                          | entra em uma sala (cria a sala se ela não existir)        |
| `/rooms`                                 | lista as salas ativas no servidor                          |
| `/users`                                 | lista os usuários na sala atual                             |
| `/msg <usuario> <mensagem>`              | envia mensagem privada em texto puro                        |
| `/wenc <usuario> <chave> <mensagem>`     | envia mensagem privada cifrada (XOR) com a chave informada  |
| `/decrypt <chave> <base64>`              | decifra uma mensagem cifrada recebida                       |
| `/sendfile <usuario> <caminho_arquivo>`  | envia um arquivo para outro usuário                          |
| `/help`                                  | mostra a lista de comandos                                   |
| `/quit`                                  | desconecta do servidor                                       |

Arquivos recebidos via `/sendfile` são salvos automaticamente na pasta
`received_files/`, criada no diretório onde o cliente está rodando.

## Exemplo de sessão

**Terminal 1 — servidor**
```
$ ./bin/server
=========================================
 Servidor de Chat Distribuido iniciado
 Porta: 5050
=========================================
[+] alice conectou-se (fd=4)
[+] bob conectou-se (fd=5)
```

**Terminal 2 — cliente "alice"**
```
$ ./bin/client
Conectado ao servidor 127.0.0.1:5050
Digite seu nome de usuario:
> alice
Login realizado com sucesso!
Digite /help para ver os comandos disponiveis.
> Ola pessoal!
> /msg bob oi, tudo bem?
> /sendfile bob /home/alice/foto.png
```

**Terminal 3 — cliente "bob"**
```
$ ./bin/client
Conectado ao servidor 127.0.0.1:5050
Digite seu nome de usuario:
> bob
Login realizado com sucesso!
> [geral] alice: Ola pessoal!
[privado de alice]: oi, tudo bem?
[arquivo] Recebendo 'foto.png' (204800 bytes) de alice...
[arquivo] Recebido com sucesso: received_files/foto.png
```

## Detalhes do protocolo

A comunicação usa linhas de texto terminadas em `\n`. Mensagens de controle
enviadas pelo servidor começam com uma tag entre `@@`, por exemplo:

```
@@MSG@@|alice|geral|Ola pessoal!
@@PRIV@@|alice|oi, tudo bem?
@@SYS@@|servidor|geral|entrou na sala
@@FSTART@@|alice|foto.png|204800
@@FCHUNK@@|<dados em base64>
@@FEND@@|fim
```

O cliente digita comandos começados por `/`, que o servidor interpreta e
roteia (broadcast para a sala, ou envio direto ao socket do destinatário no
caso de mensagens privadas e arquivos).

## Sobre a criptografia

A cifra utilizada (XOR de fluxo com Base64) é **apenas para fins didáticos**,
demonstrando o conceito de confidencialidade em mensagens privadas. Ela não
deve ser usada em um sistema real — para isso, seria necessário usar uma
biblioteca criptográfica robusta (ex: OpenSSL/libsodium) com um algoritmo
como AES-GCM e troca de chaves segura (Diffie-Hellman/TLS).

## Limpeza

```bash
make clean
```

Remove os binários compilados e a pasta `received_files/`.
