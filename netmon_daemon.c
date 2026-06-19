#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>

#define MAX_TARGETS 50
#define SCTP_PORT 9000
#define BUFFER_SIZE 4096
#define MAX_LOG_ENTRIES 100

/* Structure representing a single monitored host */
struct Target {
    char hostname[256];
    int is_active;
    int last_status;        /* -1 = unknown, 0 = down, 1 = up */
    int port80_open;        /* Result of port 80 check */
    int port443_open;       /* Result of port 443 check */
    double latency_ms;      /* TCP connection latency in milliseconds */
};

/* Structure representing a single log entry stored in memory */
struct LogEntry {
    time_t timestamp;
    char message[512];
};

struct Target watchlist[MAX_TARGETS];
struct LogEntry event_log[MAX_LOG_ENTRIES];
int log_head = 0;   /* Index where the next log entry will be written */
int log_count = 0;  /* Total number of log entries stored */

/* Add a new entry to the in-memory circular event log and also write to syslog */
void add_log_entry(int priority, const char *fmt, ...) {
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Write to syslog as before */
    syslog(priority, "%s", msg);

    /* Also store in circular in-memory log for the LOG command */
    event_log[log_head].timestamp = time(NULL);
    strncpy(event_log[log_head].message, msg, sizeof(event_log[log_head].message) - 1);
    event_log[log_head].message[sizeof(event_log[log_head].message) - 1] = '\0';
    log_head = (log_head + 1) % MAX_LOG_ENTRIES;
    if (log_count < MAX_LOG_ENTRIES) log_count++;
}

/* Standard double-fork daemonization */
void daemonize() {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    if (setsid() < 0) exit(EXIT_FAILURE);
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    openlog("netmon_daemon", LOG_PID, LOG_DAEMON);
}

/*
 * Check if a specific port is reachable on the given hostname.
 * Returns 1 if the port is open, 0 otherwise.
 * If latency_out is not NULL, stores the connection time in milliseconds.
 */
int check_port(const char *hostname, const char *port, double *latency_out) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    add_log_entry(LOG_DEBUG, "DNS: Resolving %s for port %s...", hostname, port);

    if (getaddrinfo(hostname, port, &hints, &res) != 0) {
        add_log_entry(LOG_WARNING, "DNS: Failed to resolve %s", hostname);
        return 0;
    }

    /* Log the resolved IP address */
    char ip_str[INET6_ADDRSTRLEN];
    if (res->ai_family == AF_INET) {
        struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
        inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
    } else {
        struct sockaddr_in6 *addr = (struct sockaddr_in6 *)res->ai_addr;
        inet_ntop(AF_INET6, &addr->sin6_addr, ip_str, sizeof(ip_str));
    }
    add_log_entry(LOG_INFO, "DNS: %s resolved to %s", hostname, ip_str);

    int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) {
        add_log_entry(LOG_ERR, "SOCKET: Failed to create TCP socket for %s:%s", hostname, port);
        freeaddrinfo(res);
        return 0;
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    /* Measure connection time */
    struct timeval start, end;
    gettimeofday(&start, NULL);

    add_log_entry(LOG_DEBUG, "PROBE: Connecting to %s (%s) port %s...", hostname, ip_str, port);
    connect(sockfd, res->ai_addr, res->ai_addrlen);

    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLOUT;

    int ret = poll(&pfd, 1, 1000);
    int status = 0;

    if (ret > 0) {
        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error == 0) {
            status = 1;
            gettimeofday(&end, NULL);
            if (latency_out) {
                *latency_out = (end.tv_sec - start.tv_sec) * 1000.0
                             + (end.tv_usec - start.tv_usec) / 1000.0;
            }
            add_log_entry(LOG_INFO, "PROBE: %s:%s is OPEN (%.1fms)",
                hostname, port, latency_out ? *latency_out : 0.0);
        } else {
            add_log_entry(LOG_INFO, "PROBE: %s:%s is CLOSED (connection refused)",
                hostname, port);
        }
    } else if (ret == 0) {
        add_log_entry(LOG_INFO, "PROBE: %s:%s TIMEOUT (no response in 1000ms)",
            hostname, port);
    } else {
        add_log_entry(LOG_WARNING, "PROBE: %s:%s poll() error", hostname, port);
    }

    close(sockfd);
    freeaddrinfo(res);
    return status;
}

/*
 * Check a host on both port 80 and port 443.
 * Returns 1 (UP) if at least one port is open, 0 (DOWN) otherwise.
 */
int check_host(struct Target *target) {
    double latency80 = -1, latency443 = -1;

    target->port80_open  = check_port(target->hostname, "80",  &latency80);
    target->port443_open = check_port(target->hostname, "443", &latency443);

    /* Pick the best (lowest) latency from the open ports */
    if (target->port80_open && target->port443_open) {
        target->latency_ms = (latency80 < latency443) ? latency80 : latency443;
    } else if (target->port80_open) {
        target->latency_ms = latency80;
    } else if (target->port443_open) {
        target->latency_ms = latency443;
    } else {
        target->latency_ms = -1;
    }

    return (target->port80_open || target->port443_open) ? 1 : 0;
}

static int cycle_number = 0; /* Global cycle counter */

/* Periodically check all active targets and log the results */
void process_targets() {
    /* Count how many hosts are on the watchlist */
    int active_count = 0;
    for (int i = 0; i < MAX_TARGETS; i++)
        if (watchlist[i].is_active) active_count++;

    if (active_count == 0) return; /* Nothing to do */

    cycle_number++;
    add_log_entry(LOG_INFO, "--- CYCLE #%d: Checking %d host(s) ---", cycle_number, active_count);

    int total_up = 0, total_down = 0;

    for (int i = 0; i < MAX_TARGETS; i++) {
        if (watchlist[i].is_active) {
            add_log_entry(LOG_INFO, "SCAN: Starting check for %s...", watchlist[i].hostname);
            int current_status = check_host(&watchlist[i]);

            /* Build a ports string, e.g. "[80,443]" */
            char ports[32] = "";
            if (watchlist[i].port80_open && watchlist[i].port443_open)
                strcpy(ports, "[80,443]");
            else if (watchlist[i].port80_open)
                strcpy(ports, "[80]");
            else if (watchlist[i].port443_open)
                strcpy(ports, "[443]");
            else
                strcpy(ports, "[-]");

            if (current_status) total_up++; else total_down++;

            /* Detect status change (UP->DOWN or DOWN->UP) */
            if (current_status != watchlist[i].last_status) {
                const char *old_st = (watchlist[i].last_status == 1) ? "UP"
                                   : ((watchlist[i].last_status == 0) ? "DOWN" : "UNKNOWN");
                const char *new_st = current_status ? "UP" : "DOWN";
                if (current_status == 1) {
                    add_log_entry(LOG_WARNING,
                        "ALERT: Host %s changed %s -> %s %s (%.1fms)",
                        watchlist[i].hostname, old_st, new_st, ports, watchlist[i].latency_ms);
                } else {
                    add_log_entry(LOG_WARNING,
                        "ALERT: Host %s changed %s -> %s %s",
                        watchlist[i].hostname, old_st, new_st, ports);
                }
                watchlist[i].last_status = current_status;
            }

            /* Periodic logging — log every cycle regardless of change */
            if (current_status == 1) {
                add_log_entry(LOG_INFO,
                    "RESULT: %s is UP %s (%.1fms)",
                    watchlist[i].hostname, ports, watchlist[i].latency_ms);
            } else {
                add_log_entry(LOG_INFO,
                    "RESULT: %s is DOWN %s",
                    watchlist[i].hostname, ports);
            }
        }
    }

    add_log_entry(LOG_INFO, "--- CYCLE #%d COMPLETE: %d UP, %d DOWN ---", cycle_number, total_up, total_down);
}

/* Handle an incoming SCTP message from the client */
void handle_sctp_message(int sock) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    struct sctp_sndrcvinfo sri;
    int msg_flags = 0;

    int n = sctp_recvmsg(sock, buffer, sizeof(buffer) - 1, (struct sockaddr *)&client_addr, &len, &sri, &msg_flags);
    if (n <= 0) return;
    buffer[n] = '\0';

    /* Log the incoming client connection and raw command */
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    add_log_entry(LOG_INFO, "CLIENT: Received command \"%s\" from %s:%d",
        buffer, client_ip, ntohs(client_addr.sin_port));

    char command[16], arg[256];
    int parsed = sscanf(buffer, "%15s %255s", command, arg);
    char response[BUFFER_SIZE] = "OK\n";

    if (strcmp(command, "ADD") == 0 && parsed == 2) {
        /* Check for duplicates first */
        for (int i = 0; i < MAX_TARGETS; i++) {
            if (watchlist[i].is_active && strcmp(watchlist[i].hostname, arg) == 0) {
                snprintf(response, sizeof(response), "ERROR: Host %s already on watchlist\n", arg);
                goto send_response;
            }
        }
        int added = 0;
        for (int i = 0; i < MAX_TARGETS; i++) {
            if (!watchlist[i].is_active) {
                strncpy(watchlist[i].hostname, arg, 255);
                watchlist[i].is_active = 1;
                watchlist[i].last_status = -1;
                watchlist[i].port80_open = 0;
                watchlist[i].port443_open = 0;
                watchlist[i].latency_ms = -1;
                added = 1;
                add_log_entry(LOG_INFO, "Added host %s to watchlist", arg);
                break;
            }
        }
        if (!added) snprintf(response, sizeof(response), "ERROR: Watchlist full\n");

    } else if (strcmp(command, "REMOVE") == 0 && parsed == 2) {
        int removed = 0;
        for (int i = 0; i < MAX_TARGETS; i++) {
            if (watchlist[i].is_active && strcmp(watchlist[i].hostname, arg) == 0) {
                watchlist[i].is_active = 0;
                removed = 1;
                add_log_entry(LOG_INFO, "Removed host %s from watchlist", arg);
                break;
            }
        }
        if (!removed) snprintf(response, sizeof(response), "ERROR: Host not found\n");

    } else if (strcmp(command, "STATUS") == 0) {
        /* Enhanced STATUS output with ports and latency */
        strcpy(response, "Watchlist Status:\n");
        for (int i = 0; i < MAX_TARGETS; i++) {
            if (watchlist[i].is_active) {
                char line[512];
                const char *st = (watchlist[i].last_status == 1) ? "UP"
                               : ((watchlist[i].last_status == 0) ? "DOWN" : "UNKNOWN");

                /* Build ports info */
                char ports[32] = "";
                if (watchlist[i].port80_open && watchlist[i].port443_open)
                    strcpy(ports, "[80,443]");
                else if (watchlist[i].port80_open)
                    strcpy(ports, "[80]");
                else if (watchlist[i].port443_open)
                    strcpy(ports, "[443]");
                else if (watchlist[i].last_status != -1)
                    strcpy(ports, "[-]");

                /* Format with latency if available */
                if (watchlist[i].latency_ms > 0) {
                    snprintf(line, sizeof(line), "- %s: %s %s (%.1fms)\n",
                        watchlist[i].hostname, st, ports, watchlist[i].latency_ms);
                } else {
                    snprintf(line, sizeof(line), "- %s: %s %s\n",
                        watchlist[i].hostname, st, ports);
                }
                strncat(response, line, sizeof(response) - strlen(response) - 1);
            }
        }

    } else if (strcmp(command, "LOG") == 0) {
        /* LOG command: return the last N events from in-memory log */
        int count = 10; /* default: last 10 entries */
        if (parsed == 2) {
            int requested = atoi(arg);
            if (requested > 0 && requested <= MAX_LOG_ENTRIES)
                count = requested;
        }
        if (count > log_count) count = log_count;

        if (count == 0) {
            strcpy(response, "Event Log: (empty)\n");
        } else {
            strcpy(response, "Event Log:\n");
            /* Walk backwards from the most recent entry */
            int start_idx = (log_head - count + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
            for (int i = 0; i < count; i++) {
                int idx = (start_idx + i) % MAX_LOG_ENTRIES;
                char line[600];
                struct tm *tm_info = localtime(&event_log[idx].timestamp);
                char time_str[32];
                strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
                snprintf(line, sizeof(line), "  [%s] %s\n", time_str, event_log[idx].message);
                strncat(response, line, sizeof(response) - strlen(response) - 1);
            }
        }

    } else {
        snprintf(response, sizeof(response), "ERROR: Invalid command. Use: ADD, REMOVE, STATUS, LOG\n");
    }

send_response:
    sctp_sendmsg(sock, response, strlen(response), (struct sockaddr *)&client_addr, len, sri.sinfo_ppid, sri.sinfo_flags, sri.sinfo_stream, 0, 0);
}

int main() {
    daemonize();
    add_log_entry(LOG_INFO, "========================================");
    add_log_entry(LOG_INFO, "Network Monitor Daemon started (PID: %d)", getpid());
    add_log_entry(LOG_INFO, "Max watchlist capacity: %d hosts", MAX_TARGETS);
    add_log_entry(LOG_INFO, "Monitoring ports: 80, 443");
    add_log_entry(LOG_INFO, "========================================");

    for (int i = 0; i < MAX_TARGETS; i++) watchlist[i].is_active = 0;

    add_log_entry(LOG_INFO, "INIT: Creating SCTP socket (SOCK_SEQPACKET)...");
    int sctp_sock = socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP);
    if (sctp_sock < 0) {
        syslog(LOG_ERR, "Failed to create SCTP socket");
        exit(EXIT_FAILURE);
    }
    add_log_entry(LOG_INFO, "INIT: SCTP socket created (fd=%d)", sctp_sock);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SCTP_PORT);

    add_log_entry(LOG_INFO, "INIT: Binding to 0.0.0.0:%d...", SCTP_PORT);
    if (bind(sctp_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "Failed to bind SCTP socket");
        exit(EXIT_FAILURE);
    }
    add_log_entry(LOG_INFO, "INIT: Bind successful");

    add_log_entry(LOG_INFO, "INIT: Starting to listen for SCTP connections...");
    if (listen(sctp_sock, 5) < 0) {
        syslog(LOG_ERR, "Failed to listen on SCTP socket");
        exit(EXIT_FAILURE);
    }
    add_log_entry(LOG_INFO, "INIT: Listening on SCTP port %d. Waiting for commands...", SCTP_PORT);

    struct pollfd fds[1];
    fds[0].fd = sctp_sock;
    fds[0].events = POLLIN;

    while (1) {
        int ret = poll(fds, 1, 5000);
        if (ret > 0 && (fds[0].revents & POLLIN)) {
            handle_sctp_message(sctp_sock);
        } else if (ret == 0) {
            process_targets();
        }
    }

    close(sctp_sock);
    closelog();
    return 0;
}
