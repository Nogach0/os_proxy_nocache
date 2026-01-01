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

#define PORT 80
#define BUFFER_SIZE 4096 // размер буфера в байтах (4 КБ)

// структура для передачи параметров в поток
typedef struct {
    int client_fd;
} thread_data_t;

// поток, обрабатывающий одно клиентское соединение
void *handle_connection(void *arg) {
    thread_data_t *data = (thread_data_t *)arg;
    int client_fd = data->client_fd;  // сокет для общения с клиентом (браузер / curl)
    free(data);

    char buffer[BUFFER_SIZE];

    // читаем HTTP-запрос от клиента (строка запроса + заголовки)
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        // клиент ничего не прислал или сразу закрыл соединение
        close(client_fd);
        return NULL;
    }
    buffer[bytes_read] = '\0';  // делаем C-строку для удобства парсинга

    // достанем первую строку запроса (Request-Line) для логов
    char request_line[512] = {0};
    char *req_end = strstr(buffer, "\r\n");
    if (req_end != NULL) {
        size_t len = (size_t)(req_end - buffer);
        if (len >= sizeof(request_line))
            len = sizeof(request_line) - 1;
        memcpy(request_line, buffer, len);
        request_line[len] = '\0';
    } else {
        strcpy(request_line, "(no request line found)");
    }

    // ищем заголовок Host
    char *host_start = strstr(buffer, "Host: ");
    if (!host_start) {
        const char *msg = "HTTP/1.0 400 Bad Request\r\n\r\nInvalid request (no Host header)\n";
        (void)write(client_fd, msg, strlen(msg));
        fprintf(stderr, "[proxy] bad request from client, no Host header, request=\"%s\"\n",
                request_line);
        close(client_fd);
        return NULL;
    }

    host_start += 6; // пропускаем "Host: "
    char *host_end = strstr(host_start, "\r\n");
    if (!host_end) {
        const char *msg = "HTTP/1.0 400 Bad Request\r\n\r\nInvalid Host header\n";
        (void)write(client_fd, msg, strlen(msg));
        fprintf(stderr, "[proxy] invalid Host header, request=\"%s\"\n", request_line);
        close(client_fd);
        return NULL;
    }

    // вырезаем имя хоста
    char host[256];
    int host_len = (int)(host_end - host_start);
    if (host_len > (int)sizeof(host) - 1) {
        host_len = (int)sizeof(host) - 1;
    }
    strncpy(host, host_start, (size_t)host_len);
    host[host_len] = '\0';

    // ЛОГ: что пришло от клиента
    printf("[proxy] new request: \"%s\", Host: \"%s\"\n", request_line, host);

    // --- ДЕГРАДАЦИЯ ДО HTTP/1.0 И ПРОСЬБА ЗАКРЫТЬ СОЕДИНЕНИЕ НА СТОРОНЕ ORIGIN ---

    // 1) Меняем версию в Request-Line: HTTP/1.1 -> HTTP/1.0
    if (req_end) {
        char *version_pos = strstr(buffer, "HTTP/1.1");
        if (version_pos && version_pos < req_end) {
            memcpy(version_pos, "HTTP/1.0", strlen("HTTP/1.0"));
        }
    }

    // 2) Гарантируем, что к origin уйдет Connection: close
    char *headers_end = strstr(buffer, "\r\n\r\n");
    if (headers_end) {
        // ищем существующий Connection:
        char *conn_hdr = NULL;
        char *search_from = req_end ? req_end + 2 : buffer; // после первой строки

        conn_hdr = strstr(search_from, "Connection:");
        if (conn_hdr && conn_hdr < headers_end) {
            // есть Connection: <что-то> - переписываем на close
            char *line_end = strstr(conn_hdr, "\r\n");
            if (line_end) {
                char *p = conn_hdr + strlen("Connection:");
                // пропускаем пробелы
                while (p < line_end && (*p == ' ' || *p == '\t')) {
                    p++;
                }
                const char *val = "close";
                size_t val_len = strlen(val);
                size_t remain = (size_t)(line_end - p);
                if (val_len <= remain) {
                    memcpy(p, val, val_len);
                    // остаток строки забьём пробелами
                    for (size_t i = val_len; i < remain; i++) {
                        p[i] = ' ';
                    }
                }
            }
        } else {
            // Connection: нет - вставим "Connection: close" перед \r\n\r\n
            const char *conn_line = "Connection: close\r\n";
            size_t conn_len = strlen(conn_line);
            size_t tail_len = (size_t)(bytes_read - (headers_end - buffer));

            if ((size_t)bytes_read + conn_len < BUFFER_SIZE) {
                memmove(headers_end + conn_len, headers_end, tail_len);
                memcpy(headers_end, conn_line, conn_len);
                bytes_read += (ssize_t)conn_len;
                buffer[bytes_read] = '\0';
                headers_end = strstr(buffer, "\r\n\r\n"); // обновили конец заголовков
            } else {
                fprintf(stderr, "[proxy] warning: no space to add Connection: close header\n");
            }
        }
    }

    // --- КОНЕЦ ПРАВКИ ЗАПРОСА ---

    // DNS-резолв хоста
    struct hostent *server = gethostbyname(host);
    if (!server) {
        const char *msg = "HTTP/1.0 502 Bad Gateway\r\n\r\nHost resolution failed\n";
        (void)write(client_fd, msg, strlen(msg));
        fprintf(stderr, "[proxy] gethostbyname(\"%s\") failed: h_errno=%d\n", host, h_errno);
        close(client_fd);
        return NULL;
    }

    // создаём TCP-сокет до origin-сервера
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[proxy] socket to origin failed");
        close(client_fd);
        return NULL;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], (size_t)server->h_length);

    // ЛОГ: куда коннектимся
    printf("[proxy] connecting to origin %s:%d\n",
           inet_ntoa(server_addr.sin_addr),
           ntohs(server_addr.sin_port));

    if (connect(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[proxy] connect to origin failed");
        close(server_fd);
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

int main(void) {
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
    server_addr.sin_port = htons(PORT);        // порт 80

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

    printf("HTTP proxy listening on port %d (HTTP/1.0 style, no cache)...\n", PORT);

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

        int err = pthread_create(&tid, NULL, handle_connection, data);
        if (err != 0) {
            fprintf(stderr, "[proxy] pthread_create failed: %s\n", strerror(err));
            close(client_fd);
            free(data);
        } else {
            // поток отсоединяем: не нужно join, ресурсы освобождаются при завершении
            pthread_detach(tid);
        }
    }

    // сюда по-нормальному не дойдём (Ctrl+C в терминале)
    close(server_fd);
    return 0;
}