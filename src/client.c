#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include "sbcp.h"

#define BUFFER_SIZE 1024
#define LINE_BUF_SIZE 4096      /* the longest line a terminal passes on in one read */
#define IDLE_TIMEOUT_SEC 10
#define RECV_BUF_SIZE 65536     /* any SBCP message fits: Length is 16 bits */
#define PARTIAL_TIMEOUT_MS 3000 /* an unfinished message is discarded after this */

/* TCP is a byte stream: bytes from the server that do not form a whole message yet */
static uint8_t recv_buf[RECV_BUF_SIZE];
static size_t recv_len;
static long long partial_since = -1;    /* when the unfinished message began, -1 if none */

static int show_prompt;                 /* stdin and stdout are terminals */
static const char *my_name;

/* Typed bytes whose '\n' has not arrived yet */
static char line_buf[LINE_BUF_SIZE];
static size_t line_len;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void prompt(void) {
    if (show_prompt) {
        printf("%s> ", my_name);
        fflush(stdout);
    }
}

/* Helper function to pack a JOIN message */
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

    /* 1. Header */
    struct sbcp_header *hdr = (struct sbcp_header *)buf;
    hdr->vrsn_type = htons(MAKE_VRSN_TYPE(SBCP_VERSION, TYPE_JOIN));
    hdr->length = htons(total_len);

    /* 2. Username Attribute */
    struct sbcp_attr *attr = (struct sbcp_attr *)(buf + sizeof(struct sbcp_header));
    attr->type = htons(ATTR_USERNAME);
    attr->length = htons(attr_len);
    memcpy(attr->payload, username, user_len);

    return (int)total_len;
}

/* Helper function to pack a SEND message */
static int pack_send(const char *msg_text, size_t text_len, uint8_t *buf, size_t buf_size) {
    if (text_len < 1 || text_len > MAX_MESSAGE_LEN) {
        return -1;
    }

    uint16_t attr_len = sizeof(struct sbcp_attr) + text_len;
    uint16_t total_len = sizeof(struct sbcp_header) + attr_len;

    if (total_len > buf_size) {
        return -1;
    }

    /* 1. Header */
    struct sbcp_header *hdr = (struct sbcp_header *)buf;
    hdr->vrsn_type = htons(MAKE_VRSN_TYPE(SBCP_VERSION, TYPE_SEND));
    hdr->length = htons(total_len);

    /* 2. Message Attribute */
    struct sbcp_attr *attr = (struct sbcp_attr *)(buf + sizeof(struct sbcp_header));
    attr->type = htons(ATTR_MESSAGE);
    attr->length = htons(attr_len);
    memcpy(attr->payload, msg_text, text_len);

    return (int)total_len;
}

/* Helper function to pack an IDLE message (no attributes) */
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

/* Send one typed line as SEND messages of at most MAX_MESSAGE_LEN bytes each.
 * Returns 1 if something was sent, 0 for an empty line, -1 on error. */
static int send_text(int sockfd, const char *text, size_t len) {
    uint8_t send_buf[BUFFER_SIZE];
    int sent = 0;

    if (len > 0 && text[len - 1] == '\r') {
        len--;
    }

    while (len > 0) {
        size_t chunk = len;
        if (chunk > MAX_MESSAGE_LEN) {
            chunk = MAX_MESSAGE_LEN;
            /* never cut a UTF-8 character in half */
            while (chunk > MAX_MESSAGE_LEN - 3 && ((unsigned char)text[chunk] & 0xC0) == 0x80) {
                chunk--;
            }
        }

        int send_len = pack_send(text, chunk, send_buf, sizeof(send_buf));
        if (send_len < 0 || send(sockfd, send_buf, send_len, 0) < 0) {
            perror("send failed");
            return -1;
        }
        sent = 1;
        text += chunk;
        len -= chunk;
    }
    return sent;
}

/* Parse and handle one complete message from the server */
static int process_message(const uint8_t *msg, size_t total_len) {
    const struct sbcp_header *hdr = (const struct sbcp_header *)msg;
    uint16_t type = GET_TYPE(ntohs(hdr->vrsn_type));

    /* Parse SBCP Attributes */
    size_t offset = sizeof(struct sbcp_header);
    char username_buf[MAX_USERNAME_LEN + 1];
    char message_buf[MAX_MESSAGE_LEN + 1];
    char reason_buf[MAX_REASON_LEN + 1];
    char user_list_buf[BUFFER_SIZE];
    unsigned int client_count = 0;
    int has_count = 0;

    memset(username_buf, 0, sizeof(username_buf));
    memset(message_buf, 0, sizeof(message_buf));
    memset(reason_buf, 0, sizeof(reason_buf));
    memset(user_list_buf, 0, sizeof(user_list_buf));

    while (offset < total_len) {
        if (offset + sizeof(struct sbcp_attr) > total_len) {
            break;
        }

        const struct sbcp_attr *attr = (const struct sbcp_attr *)(msg + offset);
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

                    if (strlen(user_list_buf) > 0) {
                        strncat(user_list_buf, ", ", sizeof(user_list_buf) - strlen(user_list_buf) - 1);
                    }
                    strncat(user_list_buf, username_buf, sizeof(user_list_buf) - strlen(user_list_buf) - 1);
                }
                break;

            case ATTR_CLIENT_COUNT:
                if (payload_len == 1) {
                    client_count = (unsigned char)attr->payload[0];
                    has_count = 1;
                } else if (payload_len == 2) {
                    uint16_t count;
                    memcpy(&count, attr->payload, sizeof(count));
                    client_count = ntohs(count);
                    has_count = 1;
                } else if (payload_len == 4) {
                    uint32_t count;
                    memcpy(&count, attr->payload, sizeof(count));
                    client_count = ntohl(count);
                    has_count = 1;
                }
                break;

            case ATTR_MESSAGE:
                if (payload_len <= MAX_MESSAGE_LEN) {
                    memcpy(message_buf, attr->payload, payload_len);
                    message_buf[payload_len] = '\0';
                }
                break;

            default:
                break;
        }

        offset += attr_len;
    }

    /* Process based on SBCP type; on a terminal, a notice replaces the prompt line */
    if (show_prompt) {
        printf("\r\033[K");
    }
    switch (type) {
        case TYPE_FWD:
            printf("%s: %s\n", username_buf, message_buf);
            break;

        case TYPE_ACK:
            if (has_count) {
                printf("[Server] Joined successfully. Client count: %u\n", client_count);
                if (strlen(user_list_buf) > 0) {
                    printf("[Server] Active users: %s\n", user_list_buf);
                }
            } else if (strlen(message_buf) > 0) {
                /* the ACK may carry the count and the names as one string */
                printf("[Server] Joined successfully. %s\n", message_buf);
            } else {
                printf("[Server] Joined successfully.\n");
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
    prompt();
    return 0;
}

/* Read from the server and handle every complete message received so far:
 * one recv() can hold part of a message, or several messages. */
static int handle_server_message(int sockfd) {
    ssize_t bytes_read = recv(sockfd, recv_buf + recv_len, sizeof(recv_buf) - recv_len, 0);

    if (bytes_read <= 0) {
        if (bytes_read == 0) {
            printf("\nServer closed the connection.\n");
        } else {
            perror("\nrecv error");
        }
        return -1;
    }
    recv_len += (size_t)bytes_read;

    size_t offset = 0;
    int rc = 0;
    while (rc == 0 && recv_len - offset >= sizeof(struct sbcp_header)) {
        struct sbcp_header hdr;
        memcpy(&hdr, recv_buf + offset, sizeof(hdr));
        uint16_t version = GET_VERSION(ntohs(hdr.vrsn_type));
        uint16_t total_len = ntohs(hdr.length);

        /* Not an SBCP header: the next message boundary is lost, so drop what we have */
        if (version != SBCP_VERSION || total_len < sizeof(struct sbcp_header)) {
            offset = recv_len;
            break;
        }
        if (recv_len - offset < total_len) {
            break;      /* the rest of this message has not arrived yet */
        }

        rc = process_message(recv_buf + offset, total_len);
        offset += total_len;
    }

    memmove(recv_buf, recv_buf + offset, recv_len - offset);
    recv_len -= offset;

    if (recv_len == 0) {
        partial_since = -1;
    } else if (offset > 0 || partial_since < 0) {
        partial_since = now_ms();
    }
    return rc;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <username> <server_ip> <server_port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *username = argv[1];
    my_name = username;
    const char *server_ip = argv[2];
    const char *server_port = argv[3];

    if (strlen(username) < 1 || strlen(username) > MAX_USERNAME_LEN) {
        fprintf(stderr, "Error: Username length must be between 1 and %d characters.\n", MAX_USERNAME_LEN);
        exit(EXIT_FAILURE);
    }

    char *end;
    long port = strtol(server_port, &end, 10);
    if (*server_port == '\0' || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "Error: Invalid port: %s\n", server_port);
        exit(EXIT_FAILURE);
    }

    /* A dead server must show up as a send() error, not kill us with SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* IPv4 or IPv6: try each address the server_ip argument resolves to */
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    int gai = getaddrinfo(server_ip, server_port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "Error: Invalid IP address: %s (%s)\n", server_ip, gai_strerror(gai));
        exit(EXIT_FAILURE);
    }

    int sockfd = -1;
    int saved_errno = 0;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        sockfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sockfd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect(sockfd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        saved_errno = errno;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd < 0) {
        errno = saved_errno;
        perror("Connection to server failed");
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
    int status = EXIT_SUCCESS;
    long long last_send = now_ms();     /* the idle clock starts at JOIN */

    show_prompt = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    prompt();

    /* Main multiplexing event loop */
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(sockfd, &read_fds);

        /* Idle = no chat message sent for 10 s; messages received do not count.
         * select() also wakes up to discard an unfinished message. */
        struct timeval tv;
        struct timeval *timeout = NULL;
        long long now = now_ms();
        long long wait = 0;
        int timed = 0;
        if (!is_idle) {
            wait = last_send + IDLE_TIMEOUT_SEC * 1000LL - now;
            timed = 1;
        }
        if (partial_since >= 0) {
            long long w = partial_since + PARTIAL_TIMEOUT_MS - now;
            if (!timed || w < wait) {
                wait = w;
            }
            timed = 1;
        }
        if (timed) {
            if (wait < 0) {
                wait = 0;
            }
            tv.tv_sec = wait / 1000;
            tv.tv_usec = (wait % 1000) * 1000;
            timeout = &tv;
        }

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, timeout);

        if (activity < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select error");
            status = EXIT_FAILURE;
            break;
        }

        if (partial_since >= 0 && now_ms() - partial_since >= PARTIAL_TIMEOUT_MS) {
            recv_len = 0;
            partial_since = -1;
        }

        if (!is_idle && now_ms() - last_send >= IDLE_TIMEOUT_SEC * 1000LL) {
            int idle_len = pack_idle(send_buf, sizeof(send_buf));
            if (idle_len > 0 && send(sockfd, send_buf, idle_len, 0) < 0) {
                perror("send failed");
                status = EXIT_FAILURE;
                break;
            }
            is_idle = 1;
        }

        if (activity == 0) {
            continue;
        }

        /* Incoming message from server */
        if (FD_ISSET(sockfd, &read_fds)) {
            if (handle_server_message(sockfd) < 0) {
                status = EXIT_FAILURE;
                break;
            }
        }

        /* User input: read() instead of fgets(), because lines that stdio buffers
         * are invisible to select() and would wait for the next keystroke */
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            ssize_t n = read(STDIN_FILENO, line_buf + line_len, sizeof(line_buf) - line_len);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("read error");
                status = EXIT_FAILURE;
                break;
            }
            if (n == 0) {
                /* User typed EOF (Ctrl+D): send an unfinished line, then leave */
                if (line_len > 0 && send_text(sockfd, line_buf, line_len) < 0) {
                    status = EXIT_FAILURE;
                }
                break;
            }
            line_len += (size_t)n;

            int sent = 0;
            int failed = 0;
            size_t start = 0;
            for (size_t i = 0; i < line_len && !failed; i++) {
                if (line_buf[i] == '\n') {
                    int r = send_text(sockfd, line_buf + start, i - start);
                    failed = (r < 0);
                    sent |= (r > 0);
                    start = i + 1;
                }
            }
            /* A full buffer without '\n' is sent as it is */
            if (!failed && start == 0 && line_len == sizeof(line_buf)) {
                int r = send_text(sockfd, line_buf, line_len);
                failed = (r < 0);
                sent |= (r > 0);
                start = line_len;
            }
            memmove(line_buf, line_buf + start, line_len - start);
            line_len -= start;

            if (failed) {
                status = EXIT_FAILURE;
                break;
            }
            if (sent) {
                last_send = now_ms();
                is_idle = 0;
            }
            if (start > 0) {
                prompt();
            }
        }
    }

    close(sockfd);
    return status;
}
