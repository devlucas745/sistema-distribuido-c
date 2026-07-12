/*
 * client.c
 * -----------------------------------------------------------------------
 * Cliente do Sistema de Chat Distribuído.
 *
 * - Uma thread fica bloqueada lendo mensagens do servidor e imprimindo
 *   na tela (inclusive tratando protocolo de arquivo/cripto).
 * - A thread principal lê o que o usuário digita no terminal e envia.
 *
 * Uso: ./client [ip] [porta]
 * -----------------------------------------------------------------------
 */

#include "../common/common.h"
#include <stdarg.h>
#include <sys/stat.h>

static int sock_fd = -1;
static char my_username[USERNAME_MAX] = "";

/* Estado de recepção de arquivo em andamento */
static FILE *recv_file_fp = NULL;
static char recv_filename[600];

/* ------------------------------------------------------------------- */
/* Ajuda a dividir uma linha de protocolo "A|B|C" em partes              */
/* ------------------------------------------------------------------- */
static int split_pipe(char *input, char *parts[], int max_parts) {
    int count = 0;
    char *p = input;
    parts[count++] = p;
    while (*p && count < max_parts) {
        if (*p == '|') {
            *p = '\0';
            parts[count++] = p + 1;
        }
        p++;
    }
    return count;
}

/* ------------------------------------------------------------------- */
/* Thread de recepção: lê continuamente do servidor e imprime            */
/* ------------------------------------------------------------------- */
static void *receiver_thread(void *arg) {
    (void)arg;
    char line[LINE_MAX_LEN];

    for (;;) {
        int n = read_line(sock_fd, line, sizeof(line));
        if (n <= 0) {
            printf("\n[!] Conexao com o servidor perdida.\n");
            exit(0);
        }

        char copy[LINE_MAX_LEN];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        char *parts[6];
        int np = split_pipe(copy, parts, 6);
        char *tag = parts[0];

        if (strcmp(tag, TAG_MSG) == 0 && np >= 4) {
            /* from|room|texto */
            printf("\r[%s] %s: %s\n> ", parts[2], parts[1], parts[3]);
            fflush(stdout);
        }
        else if (strcmp(tag, TAG_PRIV) == 0 && np >= 3) {
            printf("\r[privado de %s]: %s\n> ", parts[1], parts[2]);
            fflush(stdout);
        }
        else if (strcmp(tag, TAG_PRIV_ENC) == 0 && np >= 3) {
            printf("\r[privado CIFRADO de %s] (use /decrypt <chave> para ler)\n"
                   "  base64: %s\n> ", parts[1], parts[2]);
            fflush(stdout);
        }
        else if (strcmp(tag, TAG_SYS) == 0 && np >= 2) {
            /* pode vir como SYS|texto  ou SYS|from|room|texto */
            if (np >= 4) {
                printf("\r*** [%s] %s %s ***\n> ", parts[2], parts[1], parts[3]);
            } else {
                printf("\r*** %s ***\n> ", parts[1]);
            }
            fflush(stdout);
        }
        else if (strcmp(tag, TAG_ERR) == 0 && np >= 2) {
            printf("\r[ERRO] %s\n> ", parts[np - 1]);
            fflush(stdout);
        }
        else if (strcmp(tag, TAG_ROOMLIST) == 0 && np >= 2) {
            printf("\rSalas ativas: %s\n> ", parts[1]);
            fflush(stdout);
        }
        else if (strcmp(tag, TAG_USERLIST) == 0 && np >= 3) {
            printf("\rUsuarios na sala '%s': %s\n> ", parts[1], parts[2]);
            fflush(stdout);
        }
        else if (strcmp(tag, TAG_FILE_START) == 0 && np >= 4) {
            char safe_name[256];
            snprintf(safe_name, sizeof(safe_name), "%s", parts[2]);
            mkdir(RECEIVED_FILES_DIR, 0755);
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", RECEIVED_FILES_DIR, safe_name);
            recv_file_fp = fopen(path, "wb");
            strncpy(recv_filename, path, sizeof(recv_filename) - 1);
            printf("\r[arquivo] Recebendo '%s' (%s bytes) de %s...\n> ",
                   parts[2], parts[3], parts[1]);
            fflush(stdout);
        }
        else if (strcmp(tag, TAG_FILE_CHUNK) == 0 && np >= 2) {
            if (recv_file_fp) {
                unsigned char raw[LINE_MAX_LEN];
                size_t rn = base64_decode(parts[1], raw);
                fwrite(raw, 1, rn, recv_file_fp);
            }
        }
        else if (strcmp(tag, TAG_FILE_END) == 0) {
            if (recv_file_fp) {
                fclose(recv_file_fp);
                recv_file_fp = NULL;
                printf("\r[arquivo] Recebido com sucesso: %s\n> ", recv_filename);
                fflush(stdout);
            }
        }
        else if (strcmp(tag, TAG_OK) == 0 && np >= 2) {
            printf("\r%s\n> ", parts[1]);
            fflush(stdout);
        }
        else {
            /* fallback: imprime cru */
            printf("\r%s\n> ", line);
            fflush(stdout);
        }
    }
    return NULL;
}

/* Envia um arquivo local para outro usuário, em blocos base64. */
static void send_file(const char *target_user, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("[erro] Nao foi possivel abrir o arquivo: %s\n", path);
        return;
    }
    struct stat st;
    stat(path, &st);
    long filesize = st.st_size;

    /* extrai apenas o nome do arquivo (sem diretórios) */
    const char *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;

    char line[LINE_MAX_LEN];
    snprintf(line, sizeof(line), "/sendfile_start %s %s %ld", target_user, filename, filesize);
    send_line(sock_fd, line);

    unsigned char raw[FILE_CHUNK_RAW_SIZE];
    char b64[FILE_CHUNK_RAW_SIZE * 2];
    size_t n;
    long total = 0;
    while ((n = fread(raw, 1, sizeof(raw), fp)) > 0) {
        base64_encode(raw, n, b64);
        char chunk_line[LINE_MAX_LEN];
        snprintf(chunk_line, sizeof(chunk_line), "/filechunk %s %s", target_user, b64);
        send_line(sock_fd, chunk_line);
        total += (long)n;
    }
    fclose(fp);

    snprintf(line, sizeof(line), "/sendfile_end %s", target_user);
    send_line(sock_fd, line);

    printf("[arquivo] Envio concluido: %s (%ld bytes)\n", filename, total);
}

/* Envia uma mensagem privada cifrada com XOR + base64 */
static void send_encrypted(const char *target_user, const char *key, const char *text) {
    unsigned char cipher[LINE_MAX_LEN];
    size_t tlen = strlen(text);
    xor_crypt(key, (const unsigned char *)text, tlen, cipher);

    char b64[LINE_MAX_LEN * 2];
    base64_encode(cipher, tlen, b64);

    char line[LINE_MAX_LEN];
    snprintf(line, sizeof(line), "/wenc %s %s", target_user, b64);
    send_line(sock_fd, line);
}

/* Decifra uma string base64 recebida usando XOR + a chave informada */
static void decrypt_and_print(const char *key, const char *b64) {
    unsigned char raw[LINE_MAX_LEN];
    size_t rn = base64_decode(b64, raw);
    unsigned char plain[LINE_MAX_LEN];
    xor_crypt(key, raw, rn, plain);
    plain[rn] = '\0';
    printf("[decifrado]: %s\n", plain);
}

int main(int argc, char *argv[]) {
    const char *ip = "127.0.0.1";
    int port = DEFAULT_PORT;
    if (argc >= 2) ip = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    /* Inicializa Winsock no Windows */
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(1);
    }
#endif

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Endereco IP invalido: %s\n", ip);
        exit(1);
    }

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }

    printf("Conectado ao servidor %s:%d\n", ip, port);

    /* ------------------- LOGIN ------------------- */
    char line[LINE_MAX_LEN];
    for (;;) {
        int n = read_line(sock_fd, line, sizeof(line));
        if (n <= 0) { printf("Servidor encerrou a conexao.\n"); exit(1); }

        char copy[LINE_MAX_LEN];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *parts[4];
        int np = split_pipe(copy, parts, 4);

        if (strcmp(parts[0], TAG_LOGIN_PROMPT) == 0 || strcmp(parts[0], TAG_ERR) == 0) {
            printf("%s\n> ", parts[np - 1]);
            fflush(stdout);
            if (!fgets(my_username, sizeof(my_username), stdin)) exit(0);
            my_username[strcspn(my_username, "\r\n")] = '\0';
            send_line(sock_fd, my_username);
        } else if (strcmp(parts[0], TAG_OK) == 0) {
            printf("%s\n", parts[1]);
            break;
        }
    }

    /* ------------------- inicia thread de recepção ------------------- */
    pthread_t tid;
    pthread_create(&tid, NULL, receiver_thread, NULL);

    printf("Digite /help para ver os comandos disponiveis.\n> ");
    fflush(stdout);

    char input[LINE_MAX_LEN];
    while (fgets(input, sizeof(input), stdin)) {
        input[strcspn(input, "\r\n")] = '\0';
        if (strlen(input) == 0) { printf("> "); fflush(stdout); continue; }

        if (strncmp(input, "/sendfile ", 10) == 0) {
            char user[USERNAME_MAX], path[400];
            if (sscanf(input + 10, "%31s %399s", user, path) == 2) {
                send_file(user, path);
            } else {
                printf("Uso: /sendfile <usuario> <caminho_do_arquivo>\n");
            }
        }
        else if (strncmp(input, "/wenc ", 6) == 0) {
            char user[USERNAME_MAX], key[128], text[LINE_MAX_LEN];
            if (sscanf(input + 6, "%31s %127s %[^\n]", user, key, text) == 3) {
                send_encrypted(user, key, text);
            } else {
                printf("Uso: /wenc <usuario> <chave> <mensagem>\n");
            }
        }
        else if (strncmp(input, "/decrypt ", 9) == 0) {
            char key[128], b64[LINE_MAX_LEN];
            if (sscanf(input + 9, "%127s %s", key, b64) == 2) {
                decrypt_and_print(key, b64);
            } else {
                printf("Uso: /decrypt <chave> <base64>\n");
            }
        }
        else if (strcmp(input, "/quit") == 0) {
            send_line(sock_fd, "/quit");
            break;
        }
        else {
            send_line(sock_fd, input);
        }
        printf("> ");
        fflush(stdout);
    }

    close(sock_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    printf("Desconectado.\n");
    return 0;
}
