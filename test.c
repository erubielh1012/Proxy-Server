#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

static int sockaddr_addr_equal(const struct addrinfo *a, const struct addrinfo *b) {
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
    char host_ip[INET6_ADDRSTRLEN];
    char line[512];
    void *addr_ptr;

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

int main() {
    char *hosts[5] = {
        "mail.yahoo.com",
        "www.yahoo.com",
        "facebook.com",
        "google.com",
    };
    for (int i = 0; i < 5; i++) {
        if (is_blocked(hosts[i])) {
            printf("[PROXY] Host: '%s' is blocked\n", hosts[i]);
        } else {
            printf("[PROXY] Host: '%s' is not blocked\n", hosts[i]);
        }
    }
    return 0;
}
