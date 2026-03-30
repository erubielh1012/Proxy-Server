#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>

void parse_request(char *buffer, char **method, char **host, char **path, char **port, char **version);
void *handle_client(void *arg);
int is_valid_host(char *host);
void setup_proxy_socket(int *sockfd, struct sockaddr_in *address, int port);
int setup_client_socket(int *sockfd, struct sockaddr_in *server_addr, const char *host, int port);
int is_blocked(char *host);
int sockaddr_addr_equal(const struct addrinfo *a, const struct addrinfo *b);

#define PAYLOAD_SIZE 1460

struct args_t {
    int conn_fd;
    int timeout;
};

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "[PROXY] Usage: %s <port> <timeout>\n", argv[0]);
        return 1;
    }

    const int port = atoi(argv[1]);
    const int timeout = atoi(argv[2]);
    if (timeout <= 0) {
        fprintf(stderr, "[PROXY] Timeout must be greater than 0\n");
        return 1;
    }
    if (port <= 0) {
        fprintf(stderr, "[PROXY] Port must be greater than 0\n");
        return 1;
    }

    int proxy_fd, conn_fd;
    struct sockaddr_in proxy_address = {0};
    int addrlen = sizeof(proxy_address);

    // Create socket file descriptor
    setup_proxy_socket(&proxy_fd, &proxy_address, port);

    // Accept incoming connections
    for (;;) {
        if ((conn_fd = accept(proxy_fd, (struct sockaddr *)&proxy_address, (socklen_t*)&addrlen)) < 0) {
            perror("[PROXY] accept failed");
            exit(EXIT_FAILURE);
        }
        // for each client connection, handle it in a new thread
        pthread_t thread;
        struct args_t *args = malloc(sizeof(struct args_t));
        if (!args) {
            perror("[PROXY] malloc failed");
            close(conn_fd);
            continue;
        }
        *args = (struct args_t){
            .conn_fd = conn_fd,
            .timeout = timeout,
        };
        if (pthread_create(&thread, NULL, handle_client, (void *)args) != 0) {
            perror("Error: pthread_create failed\n");
            close(conn_fd);
            free(args);
            continue;
        }

        pthread_detach(thread);
    }
}

void *handle_client(void *arg) {
    struct args_t *args = (struct args_t *)arg;
    int conn_fd = args->conn_fd;
    int timeout_secs = args->timeout;
    free(arg);

    char payload[PAYLOAD_SIZE] = {0};
    char *method, *host, *path, *port, *version;
    int server_fd, n;
    struct sockaddr_in server_address = {0};

    // set up timeout if client does not send request within 2 seconds
    struct timeval timeout;
    timeout.tv_sec = timeout_secs;
    timeout.tv_usec = 0;
    setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // read request from client
    if (read(conn_fd, payload, PAYLOAD_SIZE) <= 0) {
        printf("[PROXY] Failed to read request from client\n");
        send(conn_fd, "HTTP/1.1 500 Internal Server Error\r\n", 43, 0);
        close(conn_fd);
        return NULL;
    }

    /* 
    parse request from client.
    format needs to be as such:
        GET http://www.yahoo.com/ HTTP/1.1
    where GET is the only needed method to be supported.
    */
    parse_request(payload, &method, &host, &path, &port, &version);

    if ((host == NULL) || (path == NULL) || (port == NULL) || (version == NULL)) {
        printf("[PROXY] Invalid request in handle_client\n");
        char *msg = "HTTP/1.1 400 Bad Request: Null values in request\r\n";
        send(conn_fd, msg, strlen(msg), 0);
        close(conn_fd);
        return NULL;
    }

    // check if method is GET
    if (strcmp(method, "GET") != 0) {
        send(conn_fd, "HTTP/1.1 405 Method Not Allowed\r\n", 31, 0);
        close(conn_fd);
        return NULL;
    }
    // check if version is HTTP/1.1
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
        send(conn_fd, "HTTP/1.1 505 HTTP Version Not Supported\r\n", 43, 0);
        close(conn_fd);
        return NULL;
    }
    // printf("[PROXY] Checkpoint, the method and version are valid\n");
    // check if url is valid
    if (!is_valid_host(host)) {
        send(conn_fd, "HTTP/1.1 400 Bad Request: Invalid host\r\n", 43, 0);
        close(conn_fd);
        return NULL;
    }
    printf("[PROXY] Host: '%s' is valid\n", host);

    // check if host is blocked
    if (is_blocked(host)) {
        printf("[PROXY] Host: '%s' is blocked\n", host);
        const char *msg = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        send(conn_fd, msg, strlen(msg), 0);
        close(conn_fd);
        return NULL;
    }

    // create second socket connection to indicated HTTP server
    if (setup_client_socket(&server_fd, &server_address, host, atoi(port)) < 0) {
        printf("[PROXY] Failed to setup client socket\n");
        const char *msg = "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n";
        send(conn_fd, msg, strlen(msg), 0);
        close(conn_fd);
        return NULL;
    }

    // setup a timeout if the server does not respond within 2 seconds
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    /*
        construct a new HTTP request and forward to indicated HTTP server
        format needs to be as such:
            GET /<path> HTTP/1.1\r\n
            Host: <host>\r\n
            Connection: close\r\n
            \r\n
        where <path> is the path of the url and <host> is the host of the url.

        For example, if the url is http://www.yahoo.com/news/articles/iranian-military-mocks-trumps-claim-042851486.html,
        then the path is /news/articles/iranian-military-mocks-trumps-claim-042851486.html and the host is www.yahoo.com.
        Then the payload should be:
            GET /news/articles/iranian-military-mocks-trumps-claim-042851486.html HTTP/1.1\r\n
            Host: www.yahoo.com\r\n
            Connection: close\r\n
            \r\n
    */
    char new_payload[PAYLOAD_SIZE] = {0};
    snprintf(new_payload, PAYLOAD_SIZE,
        "GET /%s HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port);
    printf("[PROXY] Request payload: \n%s\n", new_payload);
    // forward request to indicated HTTP server
    send(server_fd, new_payload, strlen(new_payload), 0);

    printf("[PROXY] Forwarding request to server\n");
    // read response from indicated HTTP server
    bzero(payload, PAYLOAD_SIZE);
    while ((n = read(server_fd, payload, PAYLOAD_SIZE)) > 0) {
        // forward response to client
        send(conn_fd, payload, n, 0);
        bzero(payload, PAYLOAD_SIZE);
    }

    if (n == 0) {
        printf("[PROXY] Server closed connection\n");
    } else if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("[PROXY] Server response timed out\n");
        } else {
            perror("[PROXY] read failed");
        }
    }

    // close the second and client sockets
    close(server_fd);
    close(conn_fd);
    return NULL;
}

void parse_request(char *buffer, char **method, char **host, char **path, char **port, char **version) {
    *method = *host = *path = *port = *version = NULL;
    char *headers = NULL;
    
    if (!buffer) return;

    // find end of request line first before modifying the buffer wtih strtok
    char *line_end = strstr(buffer, "\r\n"); 
    if (line_end) {
        *line_end = '\0'; // isolate the request line
        headers = line_end + 2; // headers start after the request line
    }

    // split request line into method, url, and version
    char *token = strtok(buffer, " \t");
    *method = token;
    token = strtok(NULL, " \t");
    char *url = token;
    token = strtok(NULL, " \t");
    *version = token;

    if ((*method == NULL) || (url == NULL) || (*version == NULL)) {
        printf("[PROXY] Invalid request in parse_request\n");
        return;
    }

    /* 
        split url into host and path
        There is an absolute-form:
            http://<host>[:port]/<path>
        and a origin-form:
            /<path>
    */
    char *target = url;
    if (strncmp(target, "http://", 7) == 0) {
        // host is part of the url so we need to extract everything after http://
        char *authority = target + 7;
        char *slash = strchr(authority, '/');
        
        if (slash) {
            *slash = '\0';
            *path = slash+1;
        } else {
            *path = "";
        }
        
        
        char *colon = strchr(authority, ':');
        if (colon) {
            *colon = '\0';
            *host = authority;
            *port = colon + 1;
            if (**port == '\0') *port = "80";
        } else {
            *host = authority;
            *port = "80";
        }
        // printf("[PROXY] Host: '%s'\n", *host);
        // printf("[PROXY] Path: '%s'\n", *path);
        // printf("[PROXY] Port: '%s'\n", *port);
    } else if (target[0] == '/') {
        // host must come from Host header
        *path = "";
        *port = "80";

        if (!headers) {
            printf("[PROXY] No headers found\n");
            return;
        }

        char *h = headers;
        while (h) {
            // find the next \r\n to isolate the Host header
            char *next = strstr(h, "\r\n");
            if (!next) break;
            
            if (strncasecmp(h, "host:", 5) == 0) {
                char *host_start = h + 5;
                // account for spaces after the colon
                while (*host_start == ' ' || *host_start == '\t') host_start++;

                *next = '\0';

                char *colon = strchr(host_start, ':');
                if (colon) {
                    *colon = '\0';
                    *host = host_start;
                    *port = colon + 1;
                    if (**port == '\0') *port = "80";
                } else {
                    *host = host_start;
                    *port = "80";
                }
                break;
            }
            h = next + 2; // move to next header
        }
    }

    // printf("[PROXY] Method: '%s',\n\tHost: '%s',\n\tPath: '%s',\n\tPort: '%s',\n\tVersion: '%s'\n", *method, *host, *path, *port, *version);
    return;
}

int is_valid_host(char *host) {
    struct addrinfo hints, *result;
    int status;

    if (host == NULL || *host == '\0') return 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    status = getaddrinfo(host, NULL, &hints, &result);
    if (status != 0) {
        return 0;
    }
    freeaddrinfo(result);
    return 1;
}

void setup_proxy_socket(int *sockfd, struct sockaddr_in *address, int port) {
    if ((*sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[PROXY] socket failed");
        exit(EXIT_FAILURE);
    }

    /* setsockopt: Handy debugging trick that lets 
    * us rerun the server immediately after we kill it; 
    * otherwise we have to wait about 20 secs. 
    * Eliminates "ERROR on binding: Address already in use" error. 
    * 
    * SO_REUSEADDR is a socket option that allows the socket to be 
    * reused immediately after it is closed (without waiting for 
    * the TIME_WAIT state to expire)
    */
    int optval = 1;
    setsockopt(*sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    // Set address family, address, and port
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_ANY);
    address->sin_port = htons(port);

    // Bind socket to port
    if (bind(*sockfd, (struct sockaddr *)address, sizeof(*address)) < 0) {
        perror("[PROXY] bind failed");
        exit(EXIT_FAILURE);
    }
    // Listen for incoming connections
    if (listen(*sockfd, 3) < 0) {
        perror("[PROXY] listen failed");
        exit(EXIT_FAILURE);
    }
}

int setup_client_socket(int *sockfd, struct sockaddr_in *address, const char *host, int port) {
    struct hostent *server;

    if ((*sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[PROXY] socket failed");
        return -1;
    }
    server = gethostbyname(host);
    if (server == NULL) {
        herror("[PROXY] gethostbyname failed");
        close(*sockfd);
        return -1;
    }
    address->sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&address->sin_addr.s_addr, server->h_length);
    address->sin_port = htons(port);
    if (connect(*sockfd, (struct sockaddr *)address, sizeof(*address)) < 0) {
        perror("[PROXY] connect failed");
        close(*sockfd);
        return -1;
    }
    return 0;
}

int sockaddr_addr_equal(const struct addrinfo *a, const struct addrinfo *b) {
    if (a->ai_family != b->ai_family) {
        return 0;
    }

    if (a->ai_family == AF_INET) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)a->ai_addr;
        const struct sockaddr_in *sb = (const struct sockaddr_in *)b->ai_addr;
        return memcmp(&sa->sin_addr, &sb->sin_addr, sizeof(struct in_addr)) == 0;
    }

    if (a->ai_family == AF_INET6) {
        const struct sockaddr_in6 *sa = (const struct sockaddr_in6 *)a->ai_addr;
        const struct sockaddr_in6 *sb = (const struct sockaddr_in6 *)b->ai_addr;
        return memcmp(&sa->sin6_addr, &sb->sin6_addr, sizeof(struct in6_addr)) == 0;
    }

    return 0;
}

int is_blocked(char *host) {
    struct addrinfo hints, *host_res, *block_res;
    struct addrinfo *ha, *ba;
    int status;
    char line[512];

    // get the IP address of the host
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // a list of matching IP addresses for host are returned in host_res
    status = getaddrinfo(host, NULL, &hints, &host_res);
    if (status != 0) {
        return 0;
    }

    // open the blocklist file
    FILE *file = fopen("blocklist", "r");
    if (file == NULL) {
        perror("[PROXY] fopen failed");
        freeaddrinfo(host_res);
        return 0;
    }

    // read the blocklist file line by line
    // for each line, get the IP address of the blocklist host
    // compare the IP addresses of the host and the blocklist host
    // if they match, return 1
    // if they don't match, continue
    // if the end of the file is reached, return 0
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        status = getaddrinfo(line, NULL, &hints, &block_res);
        if (status != 0) {
            continue;
        }

        for (ha = host_res; ha != NULL; ha = ha->ai_next) {
            for (ba = block_res; ba != NULL; ba = ba->ai_next) {
                if (sockaddr_addr_equal(ha, ba)) {
                    freeaddrinfo(host_res);
                    freeaddrinfo(block_res);
                    fclose(file);
                    return 1;
                }
            }
        }
        freeaddrinfo(block_res);
    }

    fclose(file);
    freeaddrinfo(host_res);
    return 0;
}
