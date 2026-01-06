#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

#define DEFAULT_PROXY_PORT 80
#define ORIGIN_HTTP_PORT 80
#define BUFFER_SIZE 4096 // размер буфера в байтах (4 КБ)
#define REQUEST_LINE_MAX 1024
#define HOST_HEADER_PREFIX "Host: "
#define HOST_MAX_LEN 253 // погуглил FQDN не превышает 253 (полное имя)

static const char RESP_400_NO_HOST[] =
    "HTTP/1.0 400 Bad Request\r\n"
    "\r\n"
    "Invalid request (no Host header)\n";

static const char RESP_400_BAD_HOST_HEADER[] =
    "HTTP/1.0 400 Bad Request\r\n"
    "\r\n"
    "Invalid Host header\n";

static const char RESP_502_RESOLVE_FAILED[] =
    "HTTP/1.0 502 Bad Gateway\r\n"
    "\r\n"
    "Host resolution failed\n";

static const char RESP_413_HEADERS_TOO_LARGE[] =
    "HTTP/1.0 413 Request Entity Too Large\r\n"
    "\r\n"
    "Header section is too large\n";

// структура для передачи параметров в поток
typedef struct {
    int client_fd;
} thread_data_t;

// деградация до http/1.0 и гарантированный коннект
static void normalize_request_to_http_1_0(char *buffer, ssize_t *bytes_read)
{
    // ищем конец первой строки и конец заголовков
    char *req_end = strstr(buffer, "\r\n");
    char *headers_end = strstr(buffer, "\r\n\r\n");

    if (!req_end || !headers_end) {
        return; // ничего не делаем, запрос кривой - пригодится выше
    }

    // 1) HTTP/1.1 -> HTTP/1.0 в Request-Line
    char *version_pos = strstr(buffer, "HTTP/1.1");
    if (version_pos && version_pos < req_end) {
        memcpy(version_pos, "HTTP/1.0", strlen("HTTP/1.0"));
    }

    // 2) Обеспечить Connection: close к origin
    char *search_from = req_end + 2; // сразу после первой строки
    char *conn_hdr = strstr(search_from, "Connection:");
    if (conn_hdr && conn_hdr < headers_end) {
        // есть Connection, переписываем на close
        char *line_end = strstr(conn_hdr, "\r\n");
        if (!line_end) {
            return;
        }
        char *p = conn_hdr + strlen("Connection:");
        while (p < line_end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        const char *val = "close";
        size_t val_len = strlen(val);
        size_t remain = (size_t)(line_end - p);
        if (val_len <= remain) {
            memcpy(p, val, val_len);
            for (size_t i = val_len; i < remain; i++) {
                p[i] = ' ';
            }
        }
    } 
    else {
        // Connection: нет, вставим "Connection: close" перед \r\n\r\n
        const char *conn_line = "Connection: close\r\n";
        size_t conn_len = strlen(conn_line);
        size_t tail_len = (size_t)(*bytes_read - (headers_end - buffer));

        if ((size_t)(*bytes_read) + conn_len < BUFFER_SIZE) {
            memmove(headers_end + conn_len, headers_end, tail_len);
            memcpy(headers_end, conn_line, conn_len);
            *bytes_read += (ssize_t)conn_len;
            buffer[*bytes_read] = '\0';
        } 
        else {
            fprintf(stderr, "[proxy] warning: no space to add Connection: close header\n");
        }
    }
}

// поток, обрабатывающий одно клиентское соединение
void *handle_connection(void *arg) {
    thread_data_t *data = (thread_data_t *)arg;
    int client_fd = data->client_fd;  // сокет для общения с клиентом (браузер / curl)
    free(data);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = 0;

    /*
    * Читаем HTTP-запрос от клиента
    * http идет поверх tcp-потока, поэтому реально один read может дать только
    * часть заголовка. Поэтому читаем в цикле до \r\n\r\n или до заполнения буффера
    */
    while (1) {
        ssize_t r = read(client_fd, buffer + bytes_read, sizeof(buffer) - 1 - bytes_read);
        if (r < 0) {
            if (errno == EINTR) {
                continue; // повторяем после прерывания
            }
            perror("[proxy] read request from client failed");
            close(client_fd);
            return NULL;
        }
        if (r == 0) {
            // клиент закрыл соединение, так и не прислав полный заголовок
            close(client_fd);
            return NULL;
        }

        bytes_read += r;
        buffer[bytes_read] = '\0';

        // конец заголовков найден
        if (strstr(buffer, "\r\n\r\n") != NULL) {
            break;
        }

        // переполнение буфера, заголовки слишком большие
        if (bytes_read >= (ssize_t)sizeof(buffer) - 1) {
            (void)write(client_fd, RESP_413_HEADERS_TOO_LARGE, strlen(RESP_413_HEADERS_TOO_LARGE));
            fprintf(stderr, "[proxy] client headers too large\n");
            close(client_fd);
            return NULL;
        }
    }
    

    // достанем первую строку запроса (Request-Line) для логов
    char request_line[REQUEST_LINE_MAX] = {0};
    char *req_end = strstr(buffer, "\r\n");
    if (req_end != NULL) {
        size_t len = (size_t)(req_end - buffer);
        if (len >= sizeof(request_line))
            len = sizeof(request_line) - 1;
        memcpy(request_line, buffer, len);
        request_line[len] = '\0';
    } 
    else {
        strcpy(request_line, "(no request line found)");
    }

    // ищем заголовок Host
    char *host_start = strstr(buffer, HOST_HEADER_PREFIX);
    if (!host_start) {
        (void)write(client_fd, RESP_400_NO_HOST, strlen(RESP_400_NO_HOST));
        fprintf(stderr, "[proxy] bad request from client, no Host header, request=\"%s\"\n",
                request_line);
        close(client_fd);
        return NULL;
    }

    host_start += strlen(HOST_HEADER_PREFIX); // пропускаем "Host: "
    char *host_end = strstr(host_start, "\r\n");
    if (!host_end) {
        (void)write(client_fd, RESP_400_BAD_HOST_HEADER, strlen(RESP_400_BAD_HOST_HEADER));
        fprintf(stderr, "[proxy] invalid Host header, request=\"%s\"\n", request_line);
        close(client_fd);
        return NULL;
    }

    // вырезаем имя хоста
    char host[HOST_MAX_LEN + 1];
    int host_len = (int)(host_end - host_start);

    if (host_len > HOST_MAX_LEN) {
        host_len =  HOST_MAX_LEN;
    }
    memcpy(host, host_start, (size_t)host_len);
    host[host_len] = '\0';

    // ЛОГ: что пришло от клиента
    printf("[proxy] new request: \"%s\", Host: \"%s\"\n", request_line, host);

    normalize_request_to_http_1_0(buffer, &bytes_read); // деградируем

    /* DNS через getaddrinfo (вместо устаревшего gethostbyname) */
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    char port_str[6];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      /* IPv4 или IPv6 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */

    snprintf(port_str, sizeof(port_str), "%d", ORIGIN_HTTP_PORT);

    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0) {
        (void)write(client_fd, RESP_502_RESOLVE_FAILED, strlen(RESP_502_RESOLVE_FAILED));
        fprintf(stderr, "[proxy] getaddrinfo(\"%s\") failed: %s\n", host, gai_strerror(gai));
        close(client_fd);
        return NULL;
    }

    int server_fd = -1;
    char addr_str[INET6_ADDRSTRLEN];

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        server_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (server_fd < 0) {
            continue;
        }

        if (connect(server_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            void *addr_ptr = NULL;
            if (rp->ai_family == AF_INET) {
                addr_ptr = &((struct sockaddr_in *)rp->ai_addr)->sin_addr;
            } 
            else if (rp->ai_family == AF_INET6) {
                addr_ptr = &((struct sockaddr_in6 *)rp->ai_addr)->sin6_addr;
            }
            if (addr_ptr) {
                inet_ntop(rp->ai_family, addr_ptr, addr_str, sizeof(addr_str));
                printf("[proxy] connecting to origin %s:%d\n", addr_str, ORIGIN_HTTP_PORT);
            }
            break;
        }

        close(server_fd);
        server_fd = -1;
    }

    freeaddrinfo(res);

    if (server_fd < 0) {
        (void)write(client_fd,
                    RESP_502_RESOLVE_FAILED,
                    strlen(RESP_502_RESOLVE_FAILED));
        fprintf(stderr, "[proxy] cannot connect to origin for host \"%s\"\n", host);
        close(client_fd);
        return NULL;
    }

    // пересылаем (уже подправленный) HTTP-запрос целиком на сервер
    ssize_t bytes_written = write(server_fd, buffer, (size_t)bytes_read);
    if (bytes_written < 0) {
        perror("[proxy] write request to origin failed");
        close(server_fd);
        close(client_fd);
        return NULL;
    }

    // читаем ответ от сервера и транзитом отправляем клиенту
    long long total_from_server = 0;
    long long total_to_client = 0;

    while ((bytes_read = read(server_fd, buffer, sizeof(buffer))) > 0) {
        total_from_server += bytes_read;

        ssize_t w = write(client_fd, buffer, (size_t)bytes_read);
        if (w < 0) {
            perror("[proxy] write response to client failed");
            break;
        }
        total_to_client += w;
    }

    if (bytes_read < 0) {
        // ошибка чтения с сервера
        perror("[proxy] read from origin failed");
    }

    printf("[proxy] finished: host=%s, bytes_from_server=%lld, bytes_to_client=%lld\n",
           host, total_from_server, total_to_client);

    close(server_fd);
    close(client_fd);
    return NULL;
}

int main(int argc, char **argv) {
    int listen_port = DEFAULT_PROXY_PORT;

    if (argc >= 2) {
        char *end = NULL;
        long p = strtol(argv[1], &end, 10);
        if (*end != '\0' || p <= 0 || p > 65535) {
            fprintf(stderr, "Usage: %s [listen_port]\n", argv[0]);
            fprintf(stderr, "listen_port must be in range 1..65535\n");
            return EXIT_FAILURE;
        }
        listen_port = (int)p;
    }

    // создаём серверный TCP-сокет, на котором прокси будет слушать клиентов
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // SO_REUSEADDR позволяет сразу перезапускать прокси на том же порту
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;          // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;  // слушать на всех интерфейсах
    server_addr.sin_port = htons(listen_port); // либо дефолт порт либо который задали

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // backlog = 128: очередь ожидающих соединений (не лимит параллельных клиентов)
    if (listen(server_fd, 128) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        perror("pthread_attr_init");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
        perror("pthread_attr_setdetachstate");
        pthread_attr_destroy(&attr);
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("HTTP proxy listening on port %d (HTTP/1.0 style, no cache)...\n", listen_port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // блокирующий accept: ждём нового клиента
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        printf("[proxy] new connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        // создаём поток для обработки соединения
        pthread_t tid;
        thread_data_t *data = malloc(sizeof(thread_data_t));
        if (!data) {
            fprintf(stderr, "[proxy] malloc failed for thread_data_t\n");
            close(client_fd);
            continue;
        }
        data->client_fd = client_fd;

        int err = pthread_create(&tid, &attr, handle_connection, data);
        if (err != 0) {
            fprintf(stderr, "[proxy] pthread_create failed: %s\n", strerror(err));
            close(client_fd);
            free(data);
        }
    }

    // сюда по-нормальному не дойдём (Ctrl+C в терминале)
    close(server_fd);
    return 0;
}
