#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

void setup_server_socket(int *sockfd, struct sockaddr_in *address, int port);

#define PAYLOAD_SIZE 1460

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "[SERVER] Usage: %s <port>\n", argv[0]);
        return 1;
    }

    const int port = atoi(argv[1]);
    int server_fd;
    struct sockaddr_in address = {0};

    // Listen on a port.
    setup_server_socket(&server_fd, &address, port);

    /*
    Accept client connections.
    Read HTTP request.
    Determine requested resource.
    Build HTTP response.
    Send response back.
    Close connection or keep alive if supported.
*/

    const int port = atoi(argv[1]);

    int server_fd, new_socket;
    struct sockaddr_in address = {0};
    int addrlen = sizeof(address);
    char buffer[PAYLOAD_SIZE] = {0};
}

void setup_server_socket(int *sockfd, struct sockaddr_in *address, int port) {
    if ((*sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[SERVER] socket failed");
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
        perror("[SERVER] bind failed");
        exit(EXIT_FAILURE);
    }
    // Listen for incoming connections
    if (listen(*sockfd, 3) < 0) {
        perror("[SERVER] listen failed");
        exit(EXIT_FAILURE);
    }
}