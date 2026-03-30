#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

void setup_client_socket(int *sockfd, struct sockaddr_in *server_addr, int port);

#define PAYLOAD_SIZE 1460

const char *test_requests[] = {
    // 0. absolute-form, simple host
    "GET http://example.com/ HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "Connection: close\r\n"
    "User-Agent: grading-script\r\n"
    "\r\n",

    // 1. absolute-form with blocked host
    "GET http://mail.yahoo.com/docs/page.html HTTP/1.1\r\n"
    "Host: mail.yahoo.com\r\n"
    "Connection: close\r\n"
    "User-Agent: grading-script\r\n"
    "\r\n",

    // 2. origin-form, simple root path
    "GET / HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "Connection: close\r\n"
    "User-Agent: grading-script\r\n"
    "\r\n",

    // 3. origin-form with blocked host
    "GET /docs/page.html HTTP/1.1\r\n"
    "Host: mail.yahoo.com\r\n"
    "Connection: close\r\n"
    "User-Agent: grading-script\r\n"
    "\r\n",

    // 4. absolute-form with explicit port
    "GET http://example.com:80/ HTTP/1.1\r\n"
    "Host: example.com:80\r\n"
    "Connection: close\r\n"
    "User-Agent: grading-script\r\n"
    "\r\n",

    // 5. absolute-form with explicit port and file
    "GET http://example.com:80/index.html HTTP/1.1\r\n"
    "Host: example.com:80\r\n"
    "Connection: close\r\n"
    "User-Agent: grading-script\r\n"
    "\r\n",

    // 6. another valid public host
    "GET http://man7.org/linux/man-pages/man7/socket.7.html HTTP/1.1\r\n"
    "Host: man7.org\r\n"
    "Connection: close\r\n"
    "User-Agent: grading-script\r\n"
    "\r\n",

};

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "[CLIENT] Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    char payload[PAYLOAD_SIZE] = {0};
    int sockfd, n;
    struct sockaddr_in server_addr = {0};
    const int port = atoi(argv[2]);

    
    const size_t request_count = sizeof(test_requests) / sizeof(test_requests[0]);
    for (size_t i = 0; i < request_count; i++) {
        setup_client_socket(&sockfd, &server_addr, port);
        printf("[CLIENT] sending request #%zu\n", i);
        send(sockfd, test_requests[i], strlen(test_requests[i]), 0);
        while ((n = read(sockfd, payload, PAYLOAD_SIZE)) > 0) {
            fwrite(payload, 1, n, stdout);
            bzero(payload, PAYLOAD_SIZE);
        }
        printf("\n[CLIENT] done request #%zu\n", i);
        close(sockfd);
    }

    return 0;
}

void setup_client_socket(int *sockfd, struct sockaddr_in *server_addr, int port) {
    if ((*sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[CLIENT] socket failed");
        exit(EXIT_FAILURE);
    }
    server_addr->sin_family = AF_INET;
    server_addr->sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr->sin_port = htons(port);
    if(connect(*sockfd, (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0) {
        perror("[CLIENT] connect failed");
        exit(EXIT_FAILURE);
    }
}