#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netdb.h>
#include "sbcp.h"

#define BUFFER_SIZE 65536
#define IDLE_TIMEOUT_SEC 10

/* Safely determine split point without breaking multi-byte UTF-8 sequences */
static size_t get_utf8_split_len(const char *str, size_t total_len, size_t max_chunk) {
    if (total_len <= max_chunk) {
        return total_len;
    }
    size_t take = max_chunk;
    /* Backtrack if the byte is a UTF-8 continuation byte (10xxxxxx) */
    while (take > 0 && (((unsigned char)str[take]) & 0xC0) == 0x80) {
        take--;
    }
    if (take == 0) {
        take = max_chunk;
    }
    return take;
}

/* Pack a JOIN message with username attribute */
static int pack_join(const char *username, uint8_t *buf, size_t buf_size) {
    size_t user_len = strlen(username);
    if (user_len < 1 || user_len > MAX_USERNAME_LEN) {
        return -1;
    }

    uint16_t attr_len = sizeof(struct sbcp_attr) + user_len;
    uint16_t total_len = sizeof(struct sbcp_header) + attr_len;

    if (total_len > buf_size) {
        return -1;
    }

    struct sbcp_header *hdr = (struct sbcp_header *)buf;
    hdr->vrsn_type = htons(MAKE_VRSN_TYPE(SBCP_VERSION, TYPE_JOIN));
    hdr->length = htons(total_len);

    struct sbcp_attr *attr = (struct sbcp_attr *)(buf + sizeof(struct sbcp_header));
    attr->type = htons(ATTR_USERNAME);
    attr->length = htons(attr_len);
    memcpy(attr->payload, username, user_len);

    return (int)total_len;
}

/* Pack a single SEND chunk message */
static int pack_send_chunk(const char *msg_text, size_t text_len, uint8_t *buf, size_t buf_size) {
    if (text_len < 1 || text_len > MAX_MESSAGE_LEN) {
        return -1;
    }

    uint16_t attr_len = sizeof(struct sbcp_attr) + text_len;
    uint16_t total_len = sizeof(struct sbcp_header) + attr_len;

    if (total_len > buf_size) {
        return -1;
    }

    struct sbcp_header *hdr = (struct sbcp_header *)buf;
    hdr->vrsn_type = htons(MAKE_VRSN_TYPE(SBCP_VERSION, TYPE_SEND));
    hdr->length = htons(total_len);

    struct sbcp_attr *attr = (struct sbcp_attr *)(buf + sizeof(struct sbcp_header));
    attr->type = htons(ATTR_MESSAGE);
    attr->length = htons(attr_len);
    memcpy(attr->payload, msg_text, text_len);

    return (int)total_len;
}

/* Pack an IDLE message (contains no attributes per specification) */
static int pack_idle(uint8_t *buf, size_t buf_size) {
    uint16_t total_len = sizeof(struct sbcp_header);
    if (total_len > buf_size) {
        return -1;
    }

    struct sbcp_header *hdr = (struct sbcp_header *)buf;
    hdr->vrsn_type = htons(MAKE_VRSN_TYPE(SBCP_VERSION, TYPE_IDLE));
    hdr->length = htons(total_len);

    return (int)total_len;
}

/* Transmit input line, splitting into chunks if length exceeds 512 bytes */
static int send_user_line(int sockfd, const char *line, size_t len, int *is_idle, time_t *last_user_time) {
    if (len == 0) {
        return 0;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t rem = len - offset;
        size_t chunk_len = get_utf8_split_len(line + offset, rem, MAX_MESSAGE_LEN);
        uint8_t send_buf[BUFFER_SIZE];
        int pkt_len = pack_send_chunk(line + offset, chunk_len, send_buf, sizeof(send_buf));

        if (pkt_len > 0) {
            if (send(sockfd, send_buf, pkt_len, 0) < 0) {
                perror("send failed");
                return -1;
            }
        }
        offset += chunk_len;
    }

    *is_idle = 0;
    *last_user_time = time(NULL);
    return 0;
}

/* Parse attributes and format output for a single complete SBCP message */
static int process_single_message(uint16_t type, const uint8_t *msg_buf, uint16_t total_len, const char *username) {
    size_t offset = sizeof(struct sbcp_header);
    char username_buf[MAX_USERNAME_LEN + 1];
    char message_buf[MAX_MESSAGE_LEN + 1];
    char reason_buf[MAX_REASON_LEN + 1];
    char user_list_buf[16384];
    uint32_t client_count = 0;
    int has_count = 0;

    username_buf[0] = '\0';
    message_buf[0] = '\0';
    reason_buf[0] = '\0';
    user_list_buf[0] = '\0';

    while (offset < total_len) {
        if (offset + sizeof(struct sbcp_attr) > total_len) {
            break;
        }

        struct sbcp_attr *attr = (struct sbcp_attr *)(msg_buf + offset);
        uint16_t attr_type = ntohs(attr->type);
        uint16_t attr_len = ntohs(attr->length);

        if (attr_len < sizeof(struct sbcp_attr) || offset + attr_len > total_len) {
            break;
        }

        size_t payload_len = attr_len - sizeof(struct sbcp_attr);

        switch (attr_type) {
            case ATTR_REASON:
                if (payload_len <= MAX_REASON_LEN) {
                    memcpy(reason_buf, attr->payload, payload_len);
                    reason_buf[payload_len] = '\0';
                }
                break;

            case ATTR_USERNAME:
                if (payload_len <= MAX_USERNAME_LEN) {
                    memcpy(username_buf, attr->payload, payload_len);
                    username_buf[payload_len] = '\0';

                    if (user_list_buf[0] != '\0') {
                        strncat(user_list_buf, ", ", sizeof(user_list_buf) - strlen(user_list_buf) - 1);
                    }
                    strncat(user_list_buf, username_buf, sizeof(user_list_buf) - strlen(user_list_buf) - 1);
                }
                break;

            case ATTR_CLIENT_COUNT:
                /* Support both 4-byte and 2-byte count formats */
                if (payload_len >= 4) {
                    uint32_t val32 = 0;
                    memcpy(&val32, attr->payload, 4);
                    client_count = ntohl(val32);
                    has_count = 1;
                } else if (payload_len >= 2) {
                    uint16_t val16 = 0;
                    memcpy(&val16, attr->payload, 2);
                    client_count = ntohs(val16);
                    has_count = 1;
                }
                break;

            case ATTR_MESSAGE:
                if (payload_len < sizeof(message_buf)) {
                    memcpy(message_buf, attr->payload, payload_len);
                    message_buf[payload_len] = '\0';
                }
                break;

            default:
                break;
        }

        offset += attr_len;
    }

    switch (type) {
        case TYPE_FWD:
            printf("%s: %s\n", username_buf, message_buf);
            break;

        case TYPE_ACK:
            if (has_count) {
                printf("[Server] Joined successfully. Client count: %u\n", client_count);
            } else {
                printf("[Server] Joined successfully.\n");
            }
            if (message_buf[0] != '\0') {
                printf("%s\n", message_buf);
            }
            if (user_list_buf[0] != '\0') {
                printf("[Server] Active users: %s\n", user_list_buf);
            }
            if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) {
                printf("%s> ", username);
                fflush(stdout);
            }
            break;

        case TYPE_NAK:
            printf("[Server Error] Join rejected. Reason: %s\n", reason_buf);
            return -1;

        case TYPE_ONLINE:
            printf("[Server] User <%s> is online.\n", username_buf);
            break;

        case TYPE_OFFLINE:
            printf("[Server] User <%s> is offline.\n", username_buf);
            break;

        case TYPE_IDLE:
            printf("%s is idle\n", username_buf);
            break;

        default:
            break;
    }

    fflush(stdout);
    return 0;
}

/* Manage TCP stream buffer, parse framing boundaries, and drop partial messages after 3s */
static int handle_server_stream(int sockfd, const char *username) {
    static uint8_t s_recv_buf[BUFFER_SIZE];
    static size_t s_recv_len = 0;
    static time_t s_partial_time = 0;

    ssize_t bytes_read = recv(sockfd, s_recv_buf + s_recv_len, sizeof(s_recv_buf) - s_recv_len, 0);
    if (bytes_read <= 0) {
        if (bytes_read == 0) {
            printf("\nServer closed the connection.\n");
        } else {
            perror("\nrecv error");
        }
        return -1;
    }
    s_recv_len += bytes_read;

    time_t now = time(NULL);
    if (s_partial_time == 0) {
        s_partial_time = now;
    }

    while (s_recv_len >= sizeof(struct sbcp_header)) {
        struct sbcp_header *hdr = (struct sbcp_header *)s_recv_buf;
        uint16_t vt = ntohs(hdr->vrsn_type);
        uint16_t version = GET_VERSION(vt);
        uint16_t type = GET_TYPE(vt);
        uint16_t total_len = ntohs(hdr->length);

        /* Resynchronize stream boundary on invalid header version or length */
        if (version != SBCP_VERSION || total_len < sizeof(struct sbcp_header)) {
            memmove(s_recv_buf, s_recv_buf + 1, s_recv_len - 1);
            s_recv_len--;
            s_partial_time = now;
            continue;
        }

        /* Wait for remaining message bytes or discard after 3-second timeout */
        if (s_recv_len < total_len) {
            if (now - s_partial_time >= 3) {
                s_recv_len = 0;
                s_partial_time = 0;
            }
            break;
        }

        if (process_single_message(type, s_recv_buf, total_len, username) < 0) {
            return -1;
        }

        memmove(s_recv_buf, s_recv_buf + total_len, s_recv_len - total_len);
        s_recv_len -= total_len;
        s_partial_time = (s_recv_len > 0) ? now : 0;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <username> <server_ip> <server_port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *username = argv[1];
    const char *server_ip = argv[2];
    int server_port = atoi(argv[3]);

    if (strlen(username) < 1 || strlen(username) > MAX_USERNAME_LEN) {
        fprintf(stderr, "Error: Username length must be between 1 and %d characters.\n", MAX_USERNAME_LEN);
        exit(EXIT_FAILURE);
    }

    /* Prevent process termination when writing to a closed socket */
    signal(SIGPIPE, SIG_IGN);

    /* Protocol-independent address resolution for IPv4, IPv6, and DNS names */
    struct addrinfo hints, *serv_info, *p;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", server_port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(server_ip, port_str, &hints, &serv_info);
    if (status != 0) {
        fprintf(stderr, "Error resolving address '%s': %s\n", server_ip, gai_strerror(status));
        exit(EXIT_FAILURE);
    }

    int sockfd = -1;
    for (p = serv_info; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) {
            continue;
        }
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(serv_info);

    if (sockfd < 0) {
        fprintf(stderr, "Error: Failed to connect to %s:%s\n", server_ip, port_str);
        exit(EXIT_FAILURE);
    }

    /* Send initial JOIN message */
    uint8_t send_buf[BUFFER_SIZE];
    int join_len = pack_join(username, send_buf, sizeof(send_buf));
    if (join_len < 0) {
        fprintf(stderr, "Error packing JOIN message.\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    if (send(sockfd, send_buf, join_len, 0) < 0) {
        perror("Failed to send JOIN message");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    fd_set read_fds;
    int max_fd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;
    int is_idle = 0;
    time_t last_user_time = time(NULL);

    static char stdin_buf[16384];
    static size_t stdin_len = 0;

    /* Main event multiplexing loop */
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(sockfd, &read_fds);

        time_t now = time(NULL);
        int elapsed = (int)(now - last_user_time);
        struct timeval tv;
        if (!is_idle) {
            int rem = IDLE_TIMEOUT_SEC - elapsed;
            if (rem < 0) rem = 0;
            tv.tv_sec = rem;
            tv.tv_usec = 0;
        } else {
            tv.tv_sec = 1;
            tv.tv_usec = 0;
        }

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (activity < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select error");
            break;
        }

        /* Check local user inactivity timeout (10 seconds) */
        now = time(NULL);
        if (!is_idle && (now - last_user_time >= IDLE_TIMEOUT_SEC)) {
            uint8_t idle_buf[sizeof(struct sbcp_header)];
            int idle_len = pack_idle(idle_buf, sizeof(idle_buf));
            if (idle_len > 0) {
                send(sockfd, idle_buf, idle_len, 0);
            }
            is_idle = 1;
        }

        /* Incoming message on server socket */
        if (FD_ISSET(sockfd, &read_fds)) {
            if (handle_server_stream(sockfd, username) < 0) {
                break;
            }
        }

        /* Standard input events (unbuffered read, split lines immediately) */
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            ssize_t n = read(STDIN_FILENO, stdin_buf + stdin_len, sizeof(stdin_buf) - stdin_len - 1);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (n == 0) {
                /* EOF detected (Ctrl-D or piped input finished) */
                if (stdin_len > 0) {
                    stdin_buf[stdin_len] = '\0';
                    while (stdin_len > 0 && (stdin_buf[stdin_len - 1] == '\n' || stdin_buf[stdin_len - 1] == '\r')) {
                        stdin_buf[--stdin_len] = '\0';
                    }
                    if (stdin_len > 0) {
                        send_user_line(sockfd, stdin_buf, stdin_len, &is_idle, &last_user_time);
                    }
                }
                break;
            }
            stdin_len += n;

            while (1) {
                char *nl = memchr(stdin_buf, '\n', stdin_len);
                if (!nl) {
                    break;
                }
                size_t line_len = nl - stdin_buf;
                char line[16384];
                memcpy(line, stdin_buf, line_len);
                line[line_len] = '\0';

                if (line_len > 0 && line[line_len - 1] == '\r') {
                    line[line_len - 1] = '\0';
                    line_len--;
                }

                size_t consumed = (nl + 1) - stdin_buf;
                memmove(stdin_buf, nl + 1, stdin_len - consumed);
                stdin_len -= consumed;

                if (line_len > 0) {
                    if (send_user_line(sockfd, line, line_len, &is_idle, &last_user_time) < 0) {
                        break;
                    }
                }

                if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) {
                    printf("%s> ", username);
                    fflush(stdout);
                }
            }
        }
    }

    close(sockfd);
    return 0;
}
