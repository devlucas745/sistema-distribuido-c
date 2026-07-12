/*
 * common.h
 * -----------------------------------------------------------------------
 * Definições compartilhadas entre servidor e cliente do Chat Distribuído.
 *
 * Contém:
 *   - Constantes do protocolo
 *   - Prefixos de mensagens de controle (protocolo de texto sobre TCP)
 *   - Funções utilitárias: leitura de linha em socket, base64, cifra XOR
 * -----------------------------------------------------------------------
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

/* Platform-specific headers */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <windows.h>
typedef int ssize_t;
#ifndef close
#define close(s) closesocket(s)
#endif
#include <direct.h>
#include <process.h>
#define mkdir(path, mode) _mkdir(path)
#ifndef socklen_t
typedef int socklen_t;
#endif

/* Minimal pthread compatibility layer for Windows (used by this project). */
typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;

static inline int pthread_mutex_init(pthread_mutex_t *m, void *attr) {
    (void)attr;
    InitializeCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) { EnterCriticalSection(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { LeaveCriticalSection(m); return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { DeleteCriticalSection(m); return 0; }

struct _pthread_start { void *(*start_routine)(void *); void *arg; };
static unsigned __stdcall _pthread_trampoline(void *v) {
    struct _pthread_start *ts = (struct _pthread_start *)v;
    ts->start_routine(ts->arg);
    free(ts);
    return 0;
}
static inline int pthread_create(pthread_t *thread, const void *attr,
                                 void *(*start_routine)(void *), void *arg) {
    (void)attr;
    struct _pthread_start *ts = malloc(sizeof(*ts));
    if (!ts) return -1;
    ts->start_routine = start_routine;
    ts->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, _pthread_trampoline, ts, 0, NULL);
    if (h == 0) { free(ts); return -1; }
    *thread = (HANDLE)h;
    return 0;
}
static inline int pthread_detach(pthread_t thread) { return CloseHandle(thread) ? 0 : -1; }

#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#endif

/* ------------------------- Constantes gerais ------------------------- */

#define DEFAULT_PORT        5050
#define MAX_CLIENTS         100
#define USERNAME_MAX        32
#define ROOM_MAX            32
#define LINE_MAX_LEN        4096      /* tamanho máximo de uma linha do protocolo */
#define DEFAULT_ROOM         "geral"
#define FILE_CHUNK_RAW_SIZE  512      /* bytes brutos lidos por bloco de arquivo */
#define RECEIVED_FILES_DIR   "received_files"

/* ---------------------- Prefixos do protocolo ------------------------
 * Todas as linhas de controle (não digitadas diretamente pelo usuário)
 * começam com "@@" para não se confundirem com texto de chat normal.
 * Formato geral:  @@TAG@@|campo1|campo2|...
 * --------------------------------------------------------------------- */

#define TAG_OK            "@@OK@@"
#define TAG_ERR           "@@ERR@@"
#define TAG_SYS           "@@SYS@@"          /* mensagem de sistema (join/leave/etc) */
#define TAG_MSG           "@@MSG@@"          /* mensagem de sala: from|room|texto */
#define TAG_PRIV          "@@PRIV@@"         /* mensagem privada: from|texto */
#define TAG_PRIV_ENC      "@@PRIVENC@@"      /* mensagem privada cifrada: from|base64 */
#define TAG_FILE_START    "@@FSTART@@"       /* from|filename|filesize */
#define TAG_FILE_CHUNK    "@@FCHUNK@@"       /* base64 do bloco */
#define TAG_FILE_END      "@@FEND@@"         /* fim da transferência */
#define TAG_LOGIN_PROMPT  "@@LOGIN@@"
#define TAG_ROOMLIST      "@@ROOMS@@"
#define TAG_USERLIST      "@@USERS@@"

/* ----------------------------------------------------------------------
 * send_line: envia uma string terminada em '\n' pelo socket, garantindo
 * que toda a mensagem seja transmitida (lida com envios parciais).
 * -------------------------------------------------------------------- */
static inline int send_line(int fd, const char *text) {
    size_t len = strlen(text);
    char *buf = malloc(len + 2);
    if (!buf) return -1;
    memcpy(buf, text, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';

    size_t total_sent = 0;
    size_t to_send = len + 1;
    int ret = 0;
    while (total_sent < to_send) {
        ssize_t n = send(fd, buf + total_sent, to_send - total_sent, 0);
        if (n <= 0) { ret = -1; break; }
        total_sent += (size_t)n;
    }
    free(buf);
    return ret;
}

/* Envia mensagem formatada (estilo printf) terminada em '\n' */
static inline int send_linef(int fd, const char *fmt, ...) {
    char buf[LINE_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return send_line(fd, buf);
}

/* ----------------------------------------------------------------------
 * read_line: lê uma linha (até '\n' ou EOF) de um socket TCP, byte a
 * byte, protegendo contra estouro de buffer. Retorna o número de bytes
 * lidos (sem o '\n'), 0 se a conexão foi fechada, ou -1 em erro.
 * -------------------------------------------------------------------- */
static inline int read_line(int fd, char *out, size_t max_len) {
    size_t i = 0;
    char c;
    while (i < max_len - 1) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) {
            /* conexão fechada pelo peer */
            if (i == 0) return 0;
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\n') break;
        if (c == '\r') continue; /* ignora CR (compatibilidade Windows) */
        out[i++] = c;
    }
    out[i] = '\0';
    return (int)i;
}

/* ======================================================================
 *                              BASE64
 * Usado para transportar dados binários (arquivos, texto cifrado) de
 * forma segura dentro do protocolo baseado em linhas de texto.
 * ====================================================================== */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Codifica 'in_len' bytes de 'in' em base64. 'out' deve ter espaço para
 * pelo menos 4*ceil(in_len/3) + 1 bytes. Retorna o tamanho da string gerada. */
static inline size_t base64_encode(const unsigned char *in, size_t in_len, char *out) {
    size_t i, j = 0;
    for (i = 0; i + 2 < in_len; i += 3) {
        unsigned int n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[j++] = b64_table[(n >> 18) & 0x3F];
        out[j++] = b64_table[(n >> 12) & 0x3F];
        out[j++] = b64_table[(n >> 6) & 0x3F];
        out[j++] = b64_table[n & 0x3F];
    }
    size_t rem = in_len - i;
    if (rem == 1) {
        unsigned int n = in[i] << 16;
        out[j++] = b64_table[(n >> 18) & 0x3F];
        out[j++] = b64_table[(n >> 12) & 0x3F];
        out[j++] = '=';
        out[j++] = '=';
    } else if (rem == 2) {
        unsigned int n = (in[i] << 16) | (in[i + 1] << 8);
        out[j++] = b64_table[(n >> 18) & 0x3F];
        out[j++] = b64_table[(n >> 12) & 0x3F];
        out[j++] = b64_table[(n >> 6) & 0x3F];
        out[j++] = '=';
    }
    out[j] = '\0';
    return j;
}

static inline int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodifica string base64 'in' para bytes brutos em 'out'.
 * 'out' deve ter espaço suficiente (aprox. 3*len(in)/4).
 * Retorna a quantidade de bytes decodificados. */
static inline size_t base64_decode(const char *in, unsigned char *out) {
    size_t len = strlen(in);
    size_t j = 0;
    int vals[4];
    size_t vi = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '=' || in[i] == '\0') break;
        int v = b64_val(in[i]);
        if (v < 0) continue;
        vals[vi++] = v;
        if (vi == 4) {
            out[j++] = (unsigned char)((vals[0] << 2) | (vals[1] >> 4));
            out[j++] = (unsigned char)(((vals[1] & 0xF) << 4) | (vals[2] >> 2));
            out[j++] = (unsigned char)(((vals[2] & 0x3) << 6) | vals[3]);
            vi = 0;
        }
    }
    if (vi >= 2) {
        out[j++] = (unsigned char)((vals[0] << 2) | (vals[1] >> 4));
        if (vi >= 3)
            out[j++] = (unsigned char)(((vals[1] & 0xF) << 4) | (vals[2] >> 2));
    }
    return j;
}

/* ======================================================================
 *                        CIFRA XOR (educacional)
 * Cifra simples de fluxo usada para demonstrar o conceito de mensagens
 * privadas cifradas. NÃO é criptografia forte — serve apenas como
 * exercício didático sobre confidencialidade em um chat.
 * ====================================================================== */

static inline void xor_crypt(const char *key, const unsigned char *in,
                              size_t in_len, unsigned char *out) {
    size_t klen = strlen(key);
    if (klen == 0) klen = 1;
    for (size_t i = 0; i < in_len; i++) {
        out[i] = in[i] ^ (unsigned char)key[i % klen];
    }
}

#endif /* COMMON_H */
