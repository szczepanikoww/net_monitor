#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <arpa/inet.h>

#define SCTP_PORT 9000
#define BUFFER_SIZE 4096

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <COMMAND> [ARG]\n", argv[0]);
        printf("Commands:\n");
        printf("  ADD <host>     - Add a host to the watchlist\n");
        printf("  REMOVE <host>  - Remove a host from the watchlist\n");
        printf("  STATUS         - Show current status of all hosts\n");
        printf("  LOG [N]        - Show last N events (default: 10)\n");
        return 1;
    }

    int sock = socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(SCTP_PORT);

    char message[BUFFER_SIZE] = "";
    strcat(message, argv[1]);
    if (argc >= 3) {
        strcat(message, " ");
        strcat(message, argv[2]);
    }

    int ret = sctp_sendmsg(sock, message, strlen(message), (struct sockaddr *)&server_addr, sizeof(server_addr), 0, 0, 0, 0, 0);
    if (ret < 0) {
        perror("Failed to send message");
        close(sock);
        return 1;
    }

    char buffer[BUFFER_SIZE];
    struct sctp_sndrcvinfo sri;
    int msg_flags = 0;
    socklen_t len = sizeof(server_addr);

    int n = sctp_recvmsg(sock, buffer, sizeof(buffer) - 1, (struct sockaddr *)&server_addr, &len, &sri, &msg_flags);
    if (n > 0) {
        buffer[n] = '\0';
        printf("%s", buffer);
    } else {
        perror("Failed to receive response");
    }

    close(sock);
    return 0;
}
