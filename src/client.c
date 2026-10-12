#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include "sbcp.h"
#include <netdb.h>

#define BUFFER_SIZE 1024
#define IDLE_TIMEOUT_SEC 10

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
static int pack_send(const char *msg_text, uint8_t *buf, size_t buf_size) {
    size_t text_len = strlen(msg_text);
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

/* Parse and handle incoming message from server */
static int handle_server_message(int sockfd) {
    uint8_t recv_buf[BUFFER_SIZE];
    ssize_t bytes_read = recv(sockfd, recv_buf, sizeof(recv_buf), 0);

    if (bytes_read <= 0) {
        if (bytes_read == 0) {
            printf("\nServer closed the connection.\n");
        } else {
            perror("\nrecv error");
        }
        return -1;
    }

    /* Discard if smaller than SBCP header */
    if (bytes_read < (ssize_t)sizeof(struct sbcp_header)) {
        return 0;
    }

    struct sbcp_header *hdr = (struct sbcp_header *)recv_buf;
    uint16_t vt = ntohs(hdr->vrsn_type);
    uint16_t version = GET_VERSION(vt);
    uint16_t type = GET_TYPE(vt);
    uint16_t total_len = ntohs(hdr->length);

    /* Validate version and message length */
    if (version != SBCP_VERSION || total_len != bytes_read) {
        return 0;
    }

    /* Parse SBCP Attributes */
    size_t offset = sizeof(struct sbcp_header);
    char username_buf[MAX_USERNAME_LEN + 1];
    char message_buf[MAX_MESSAGE_LEN + 1];
    char reason_buf[MAX_REASON_LEN + 1];
    char user_list_buf[BUFFER_SIZE];
    uint16_t client_count = 0;
    int has_count = 0;

    memset(username_buf, 0, sizeof(username_buf));
    memset(message_buf, 0, sizeof(message_buf));
    memset(reason_buf, 0, sizeof(reason_buf));
    memset(user_list_buf, 0, sizeof(user_list_buf));

    while (offset < total_len) {
        if (offset + sizeof(struct sbcp_attr) > total_len) {
            break;
        }

        struct sbcp_attr *attr = (struct sbcp_attr *)(recv_buf + offset);
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
                if (payload_len >= sizeof(uint16_t)) {
                    client_count = ntohs(*(uint16_t *)(attr->payload));
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

    /* Process based on SBCP type */
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

    /*  addrinfo to support IPv4 and IPv6 */
    struct addrinfo hints, *serv_info, *p;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", server_port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      /* AF_UNSPEC dual support IPv4 與 IPv6 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP protocal */

    int status = getaddrinfo(server_ip, port_str, &hints, &serv_info);
    if (status != 0) {
        fprintf(stderr, "Error resolving address '%s': %s\n", server_ip, gai_strerror(status));
        exit(EXIT_FAILURE);
    }

    int sockfd = -1;
    /* go through all feedback useful address，until create a success connection */
    for (p = serv_info; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) {
            continue;
        }

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
            /* success to build connection */
            break;
        }

        close(sockfd);
        sockfd = -1;
    }

    /* release getaddrinfo memory struct */
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

    /* Main multiplexing event loop */
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(sockfd, &read_fds);

        struct timeval tv;
        tv.tv_sec = IDLE_TIMEOUT_SEC;
        tv.tv_usec = 0;

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (activity < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select error");
            break;
        }

        /* Timeout: 10 seconds without chat activity */
        if (activity == 0) {
            if (!is_idle) {
                int idle_len = pack_idle(send_buf, sizeof(send_buf));
                if (idle_len > 0) {
                    send(sockfd, send_buf, idle_len, 0);
                    is_idle = 1;
                }
            }
            continue;
        }

        /* Incoming message from server */
        if (FD_ISSET(sockfd, &read_fds)) {
            if (handle_server_message(sockfd) < 0) {
                break;
            }
        }

        /* User input from standard input */
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char input_buf[BUFFER_SIZE];
            if (fgets(input_buf, sizeof(input_buf), stdin) != NULL) {
                size_t len = strlen(input_buf);
                if (len > 0 && input_buf[len - 1] == '\n') {
                    input_buf[len - 1] = '\0';
                    len--;
                }

                if (len > 0) {
                    int send_len = pack_send(input_buf, send_buf, sizeof(send_buf));
                    if (send_len > 0) {
                        if (send(sockfd, send_buf, send_len, 0) < 0) {
                            perror("send failed");
                            break;
                        }
                        is_idle = 0;
                    }
                }
            } else {
                /* User typed EOF (Ctrl+D) */
                break;
            }
        }
    }

    close(sockfd);
    return 0;
}
