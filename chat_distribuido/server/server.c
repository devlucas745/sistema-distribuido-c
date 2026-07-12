/*
 * server.c
 * -----------------------------------------------------------------------
 * Servidor do Sistema de Chat Distribuído.
 *
 * Conceitos demonstrados:
 *   - Sockets TCP (comunicação cliente-servidor)
 *   - Threads (uma thread por cliente conectado)
 *   - Concorrência e Sincronização (pthread_mutex protegendo a lista
 *     global de clientes, que é lida/escrita por várias threads)
 *   - Broadcast (envio de mensagens para todos os membros de uma sala)
 *   - Login (identificação única de usuário)
 *   - Chat privado e transferência de arquivos (roteados pelo servidor)
 *
 * Uso: ./server [porta]
 * -----------------------------------------------------------------------
 */

#include "../common/common.h"
#include <stdarg.h>
#include <signal.h>

typedef struct {
    int fd;
    char username[USERNAME_MAX];
    char room[ROOM_MAX];
    int active;                 /* 1 = slot em uso */
} client_t;

static client_t *clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex;
static int server_fd_global = -1;

/* ------------------------------------------------------------------- */
/* Utilitários protegidos por mutex sobre a lista de clientes           */
/* ------------------------------------------------------------------- */

/* Adiciona cliente à lista global. Retorna 0 em sucesso, -1 se cheia. */
static int add_client(client_t *c) {
    int ret = -1;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == NULL) {
            clients[i] = c;
            ret = 0;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return ret;
}

static void remove_client(int fd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->fd == fd) {
            free(clients[i]);
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

/* Verifica se username já está em uso. Deve ser chamada com o mutex livre. */
static int username_exists(const char *username) {
    int found = 0;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && strcmp(clients[i]->username, username) == 0) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return found;
}

/* Envia uma mensagem de sistema formatada para todos em uma sala. */
static void broadcast_room(const char *room, const char *from_user,
                            const char *tag, const char *text, int exclude_fd) {
    char line[LINE_MAX_LEN];
    snprintf(line, sizeof(line), "%s|%s|%s|%s", tag, from_user, room, text);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->active &&
            strcmp(clients[i]->room, room) == 0 &&
            clients[i]->fd != exclude_fd) {
            send_line(clients[i]->fd, line);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

/* Encontra o fd de um usuário pelo nome. Retorna -1 se não encontrado. */
static int find_fd_by_username(const char *username) {
    int fd = -1;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && strcmp(clients[i]->username, username) == 0) {
            fd = clients[i]->fd;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return fd;
}

static void list_rooms_for(int fd) {
    char rooms[MAX_CLIENTS][ROOM_MAX];
    int count = 0;

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i] || !clients[i]->active) continue;
        int found = 0;
        for (int j = 0; j < count; j++) {
            if (strcmp(rooms[j], clients[i]->room) == 0) { found = 1; break; }
        }
        if (!found) {
            strncpy(rooms[count], clients[i]->room, ROOM_MAX - 1);
            rooms[count][ROOM_MAX - 1] = '\0';
            count++;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    char line[LINE_MAX_LEN];
    int off = snprintf(line, sizeof(line), "%s|", TAG_ROOMLIST);
    for (int i = 0; i < count && off < (int)sizeof(line) - 1; i++) {
        off += snprintf(line + off, sizeof(line) - off, "%s,", rooms[i]);
    }
    send_line(fd, line);
}

static void list_users_for(int fd, const char *room) {
    char line[LINE_MAX_LEN];
    int off = snprintf(line, sizeof(line), "%s|%s|", TAG_USERLIST, room);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->active && strcmp(clients[i]->room, room) == 0) {
            off += snprintf(line + off, sizeof(line) - off, "%s,", clients[i]->username);
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    send_line(fd, line);
}

/* ------------------------------------------------------------------- */
/* Parsing de comandos                                                  */
/* ------------------------------------------------------------------- */

/* Divide 'input' em até 'max_parts' tokens separados por espaço.
 * O último token mantém o restante da string (para mensagens com espaços). */
static int split_command(char *input, char *parts[], int max_parts) {
    int count = 0;
    char *p = input;
    while (count < max_parts - 1) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        parts[count++] = p;
        while (*p != ' ' && *p != '\0') p++;
        if (*p == '\0') break;
        *p = '\0';
        p++;
    }
    while (*p == ' ') p++;
    if (*p != '\0' && count < max_parts) {
        parts[count++] = p;
    }
    return count;
}

/* ------------------------------------------------------------------- */
/* Thread principal de atendimento a cada cliente                       */
/* ------------------------------------------------------------------- */

static void *handle_client(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    char line[LINE_MAX_LEN];

    /* ---------------------- LOGIN ---------------------- */
    send_line(fd, TAG_LOGIN_PROMPT "|Digite seu nome de usuario:");

    char username[USERNAME_MAX];
    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0) { close(fd); return NULL; }

        strncpy(username, line, USERNAME_MAX - 1);
        username[USERNAME_MAX - 1] = '\0';

        /* remove espaços e valida não-vazio */
        if (strlen(username) == 0) {
            send_line(fd, TAG_ERR "|Nome de usuario invalido. Tente novamente:");
            continue;
        }
        if (username_exists(username)) {
            send_line(fd, TAG_ERR "|Nome ja em uso. Escolha outro:");
            continue;
        }
        break;
    }

    client_t *me = calloc(1, sizeof(client_t));
    me->fd = fd;
    strncpy(me->username, username, USERNAME_MAX - 1);
    strncpy(me->room, DEFAULT_ROOM, ROOM_MAX - 1);
    me->active = 1;

    if (add_client(me) != 0) {
        send_line(fd, TAG_ERR "|Servidor cheio. Tente mais tarde.");
        free(me);
        close(fd);
        return NULL;
    }

    send_line(fd, TAG_OK "|Login realizado com sucesso!");
    send_linef(fd, "%s|Bem-vindo, %s! Voce esta na sala '%s'. Digite /help para comandos.",
               TAG_SYS, me->username, me->room);

    broadcast_room(me->room, "servidor", TAG_SYS, "entrou na sala", fd);
    printf("[+] %s conectou-se (fd=%d)\n", me->username, fd);

    /* ---------------------- LOOP PRINCIPAL ---------------------- */
    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0) break; /* desconectado */
        if (n == 0) continue;

        if (line[0] == '/') {
            char cmd_buf[LINE_MAX_LEN];
            strncpy(cmd_buf, line + 1, sizeof(cmd_buf) - 1);
            cmd_buf[sizeof(cmd_buf) - 1] = '\0';

            /* Extrai apenas o nome do comando (1a palavra); o restante da
             * linha é tratado à parte para preservar espaços em mensagens. */
            char *first_space = strchr(cmd_buf, ' ');
            char cmd_name[64];
            char *rest = cmd_buf + strlen(cmd_buf); /* aponta para '\0' por padrão */
            if (first_space) {
                size_t clen = (size_t)(first_space - cmd_buf);
                if (clen >= sizeof(cmd_name)) clen = sizeof(cmd_name) - 1;
                memcpy(cmd_name, cmd_buf, clen);
                cmd_name[clen] = '\0';
                rest = first_space + 1;
                while (*rest == ' ') rest++;
            } else {
                strncpy(cmd_name, cmd_buf, sizeof(cmd_name) - 1);
                cmd_name[sizeof(cmd_name) - 1] = '\0';
            }
            char *cmd = cmd_name;

            /* Para comandos com texto livre no final (mensagens), usamos
             * max_parts pequeno para não fragmentar o texto por espaços. */
            char *rest_parts[3];
            int rnp;
            if (strcmp(cmd, "msg") == 0 || strcmp(cmd, "w") == 0 || strcmp(cmd, "wenc") == 0) {
                rnp = split_command(rest, rest_parts, 2);   /* usuario, texto... */
            } else {
                rnp = split_command(rest, rest_parts, 3);   /* args curtos */
            }

            /* parts[0] = nome do comando; parts[1..] = argumentos */
            char *parts[4];
            parts[0] = cmd;
            for (int k = 0; k < rnp && k < 3; k++) parts[k + 1] = rest_parts[k];
            int np = rnp + 1;

            if (strcmp(cmd, "join") == 0 && np >= 2) {
                char old_room[ROOM_MAX];
                strncpy(old_room, me->room, ROOM_MAX - 1);
                broadcast_room(old_room, me->username, TAG_SYS, "saiu da sala", fd);

                pthread_mutex_lock(&clients_mutex);
                strncpy(me->room, parts[1], ROOM_MAX - 1);
                me->room[ROOM_MAX - 1] = '\0';
                pthread_mutex_unlock(&clients_mutex);

                send_linef(fd, "%s|servidor|%s|Voce entrou na sala '%s'", TAG_SYS, me->room, me->room);
                broadcast_room(me->room, me->username, TAG_SYS, "entrou na sala", fd);
            }
            else if (strcmp(cmd, "rooms") == 0) {
                list_rooms_for(fd);
            }
            else if (strcmp(cmd, "users") == 0) {
                list_users_for(fd, me->room);
            }
            else if ((strcmp(cmd, "msg") == 0 || strcmp(cmd, "w") == 0) && np >= 3) {
                int target_fd = find_fd_by_username(parts[1]);
                if (target_fd < 0) {
                    send_linef(fd, "%s|servidor|Usuario '%s' nao encontrado ou offline.", TAG_ERR, parts[1]);
                } else {
                    char pline[LINE_MAX_LEN];
                    snprintf(pline, sizeof(pline), "%s|%s|%s", TAG_PRIV, me->username, parts[2]);
                    send_line(target_fd, pline);
                    send_linef(fd, "%s|(privado para %s) %s", TAG_SYS, parts[1], parts[2]);
                }
            }
            else if (strcmp(cmd, "wenc") == 0 && np >= 3) {
                /* /wenc <usuario> <base64_da_mensagem_cifrada>
                 * O cliente já cifrou e codificou em base64 localmente. */
                int target_fd = find_fd_by_username(parts[1]);
                if (target_fd < 0) {
                    send_linef(fd, "%s|servidor|Usuario '%s' nao encontrado ou offline.", TAG_ERR, parts[1]);
                } else {
                    char pline[LINE_MAX_LEN];
                    snprintf(pline, sizeof(pline), "%s|%s|%s", TAG_PRIV_ENC, me->username, parts[2]);
                    send_line(target_fd, pline);
                    send_linef(fd, "%s|Mensagem cifrada enviada para %s.", TAG_SYS, parts[1]);
                }
            }
            else if (strcmp(cmd, "sendfile_start") == 0 && np >= 4) {
                /* /sendfile_start <usuario> <filename> <filesize> */
                int target_fd = find_fd_by_username(parts[1]);
                if (target_fd < 0) {
                    send_linef(fd, "%s|servidor|Usuario '%s' nao encontrado.", TAG_ERR, parts[1]);
                } else {
                    send_linef(target_fd, "%s|%s|%s|%s", TAG_FILE_START, me->username, parts[2], parts[3]);
                }
            }
            else if (strcmp(cmd, "filechunk") == 0 && np >= 2) {
                /* Roteia o chunk para o destinatário armazenado pelo cliente. */
                /* Formato: /filechunk <usuario> <base64> */
                if (np >= 3) {
                    int target_fd = find_fd_by_username(parts[1]);
                    if (target_fd >= 0) {
                        send_linef(target_fd, "%s|%s", TAG_FILE_CHUNK, parts[2]);
                    }
                }
            }
            else if (strcmp(cmd, "sendfile_end") == 0 && np >= 2) {
                int target_fd = find_fd_by_username(parts[1]);
                if (target_fd >= 0) {
                    send_linef(target_fd, "%s|fim", TAG_FILE_END);
                }
            }
            else if (strcmp(cmd, "help") == 0) {
                send_line(fd, TAG_SYS "|servidor|Comandos: /join <sala> /rooms /users /msg <user> <texto> "
                                       "/wenc <user> <b64> /sendfile <user> <caminho> /quit");
            }
            else if (strcmp(cmd, "quit") == 0) {
                break;
            }
            else {
                send_line(fd, TAG_ERR "|servidor|Comando desconhecido. Use /help.");
            }
        } else {
            /* Mensagem normal: broadcast para a sala atual */
            broadcast_room(me->room, me->username, TAG_MSG, line, fd);
        }
    }

    /* ---------------------- DESCONEXÃO ---------------------- */
    broadcast_room(me->room, me->username, TAG_SYS, "saiu do chat", fd);
    printf("[-] %s desconectou-se (fd=%d)\n", me->username, fd);
    remove_client(fd);
    close(fd);
    return NULL;
}

static void handle_sigint(int sig) {
    (void)sig;
    printf("\n[!] Encerrando servidor...\n");
    if (server_fd_global >= 0) close(server_fd_global);
    exit(0);
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGINT, handle_sigint);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* evita crash ao escrever em socket fechado */
#endif

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(1);
    }
#endif

    /* inicializa mutex (compatível POSIX/Win32 via common.h) */
    pthread_mutex_init(&clients_mutex, NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }
    server_fd_global = server_fd;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(server_fd, 32) < 0) {
        perror("listen");
        exit(1);
    }

    printf("=========================================\n");
    printf(" Servidor de Chat Distribuido iniciado\n");
    printf(" Porta: %d\n", port);
    printf("=========================================\n");

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &clen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        int *pfd = malloc(sizeof(int));
        *pfd = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, pfd) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(pfd);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
