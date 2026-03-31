#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>
// #include <CommonCrypto/CommonDigest.h>
#include <openssl/md5.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

void parse_request(char *buffer, char **method, char **host, char **path, char **port, char **version);
void *handle_client(void *arg);
int is_valid_host(char *host);
void setup_proxy_socket(int *sockfd, struct sockaddr_in *address, int port);
int setup_client_socket(int *sockfd, struct sockaddr_in *server_addr, const char *host, int port);
int is_blocked(char *host);
int sockaddr_addr_equal(const struct addrinfo *a, const struct addrinfo *b);
void hash_string(char *input, char *output);
int check_cache_file(char *hash_string);
void cache_response(const char *hash_string, const char *data, size_t data_len);

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
    printf("[PROXY] Recevied request from client: \n%s\n", payload);

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

    /* *********************CACHE FILE CHECK AND SEND************************** */
    // create a cache key from the host and path
    char *dir = "./cache/";
    const int url_len = strlen(host) + strlen(path) + 2;
    char *url = malloc(url_len);
    snprintf(url, url_len, "%s/%s", host, path);
    printf("[PROXY] URL: '%s'\n", url);
    char hash[MD5_DIGEST_LENGTH * 2 + 1];
    hash_string(url, hash);
    printf("[PROXY] Filename Hash: '%s'\n", hash);
    free(url);
    char cache_path[(MD5_DIGEST_LENGTH * 2 + 1) + strlen(dir)];
    snprintf(cache_path, MD5_DIGEST_LENGTH * 2 + 1 + strlen(dir), "%s%s", dir, hash);
    printf("[PROXY] Cache path: '%s'\n", cache_path);

    if (check_cache_file(hash) == 0) {
        printf("[PROXY] Cache file exists, sending to client\n");
        // cache file exists, read the file and send it to the client
        FILE *file = fopen(cache_path, "r");
        if (file == NULL) {
            perror("[PROXY] fopen failed");
            return NULL;
        }

        // Determine cached payload length for Content-Length.
        if (fseek(file, 0, SEEK_END) != 0) {
            perror("[PROXY] fseek failed");
            fclose(file);
            return NULL;
        }
        long file_size = ftell(file);
        if (file_size < 0) file_size = 0;
        rewind(file);

        char cache_response[PAYLOAD_SIZE] = {0};
        snprintf(cache_response, PAYLOAD_SIZE,
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: %ld\r\n"
                "\r\n",
                file_size);
        printf("[PROXY] Cache response: \n%s\n", cache_response);

        // Send header first, then stream the cached file bytes.
        send(conn_fd, cache_response, strlen(cache_response), 0);

        char buf[PAYLOAD_SIZE];
        size_t nread;
        while ((nread = fread(buf, 1, sizeof(buf), file)) > 0) {
            send(conn_fd, buf, nread, 0);
        }
        fclose(file);
    } else {
        printf("[PROXY] Cache file does not exist, fetching from server\n");

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
        int first_chunk = 1;
        char *body = NULL;
        size_t body_len = 0;
        // read response from indicated HTTP server
        bzero(payload, PAYLOAD_SIZE);
        while ((n = read(server_fd, payload, PAYLOAD_SIZE)) > 0) {
            // get payload from response and cache it
            if (first_chunk) {
                first_chunk = 0;
                char *response_start = strstr(payload, "\r\n\r\n");
                body = response_start + 4;
                body_len = (size_t)(n - ((response_start + 4) - payload));
            } else {
                body = payload;
                body_len = n;
            }
            // cache the response
            cache_response(hash, body, body_len);
            // forward response to client
            send(conn_fd, payload, n, 0);
            bzero(payload, PAYLOAD_SIZE);
        }
    
        // close the second and client sockets
        close(server_fd);
    }

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
    struct hostent *he;

    if ((*sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[PROXY] socket failed");
        return -1;
    }
    he = gethostbyname(host);
    if (he == NULL) {
        herror("[PROXY] gethostbyname failed");
        close(*sockfd);
        return -1;
    }
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    memcpy(&address->sin_addr.s_addr, he->h_addr_list[0], he->h_length);
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

void hash_string(char *input, char *output) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5(input, strlen(input), digest);

    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(output + i * 2, "%02x", digest[i]);
    }
    
    output[MD5_DIGEST_LENGTH * 2] = '\0';
}

int check_cache_file(char *hash_string) {
    // 0 => cached file exists, 1 => not found, -1 => error
    DIR *dir = opendir("./cache");
    if (dir == NULL) {
        // cache directory does not exist, attempt to create it
        if (mkdir("./cache", 0755) != 0) {
            perror("[PROXY] mkdir failed");
            return -1;
        }
        return 1; // newly-created dir, file obviously not present
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Some filesystems don't fill d_type reliably; we keep this simple.
        if (entry->d_type == DT_REG && strcmp(entry->d_name, hash_string) == 0) {
            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    return 1;
}

void cache_response(const char *hash_string, const char *data, size_t data_len) {
    // Cache file path is "./cache/<md5-hex>".
    char path[512];
    snprintf(path, sizeof(path), "./cache/%s", hash_string);

    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        perror("[PROXY] cache_response fopen failed");
        return;
    }

    if (data_len > 0) {
        fwrite(data, 1, data_len, f);
    }

    fclose(f);
}
