# ECEN 602 MP2: TCP Simple Broadcast Chat (SBCP)

Chat server and client for SBCP version 3 over TCP, written in C. All
mandatory features and all bonus features are implemented: IPv4/IPv6,
Feature 1 (ACK, NAK, ONLINE, OFFLINE) and Feature 2 (IDLE).

## Team roles

- **Shih-Yu (Frank) Yang**: client (`src/client.c`), protocol header
  (`include/sbcp.h`), test report and screen captures.
- **Bozhou Chen (Kotori)**: server (`src/server.c`, `src/sbcp_codec.c`,
  `src/net.c`, `src/util.c`), Makefile, automated tests (`tests/`).

## Build and usage

```sh
make # builds ./server and ./client
./server <server_ip> <server_port> <max_clients>
./client <username> <server_ip> <server_port>
make test # automated tests, about 1.5 minutes
make clean # removes binaries and object files
```

```sh
./server 127.0.0.1 9000 3 # IPv4; ::1 for IPv6, :: for both
./client alice 127.0.0.1 9000
./client alice ecewkgsw05201.engr.tamu.edu 9000 # the course's reference server
```

In the client, type a line and press Enter to send it. Ctrl-D or Ctrl-C
leaves the chat. Host names such as `localhost` work for `server_ip`.

## Architecture

- **Server**: a single process with one `select()` loop over the listening
  socket(s) and every client socket; no `fork()` or threads. A table indexed
  by descriptor holds each client's state: username, joined/idle, receive
  buffer, output queue. Sockets are non-blocking and each client has an
  output queue, so a client that stops reading cannot stall the others.
  `SIGPIPE` is ignored and every `send()` uses `MSG_NOSIGNAL`. A client that
  leaves (EOF, reset, error) is removed at the end of the loop round, its name
  is freed and OFFLINE goes to the others.
- **Client**: `select()` over stdin and the socket. Bytes from the server are
  buffered and cut into messages by the Length field, so messages split or
  merged by TCP both work. Typed lines are read with `read()` and sent as
  SEND messages of at most 512 bytes. IDLE is sent once after 10 s without
  sending a chat message.
- **Both**: `getaddrinfo()` for IPv4 and IPv6; unknown or malformed messages
  are discarded, and a message whose bytes do not all arrive within 3 s is
  dropped.

The design sketch (data model, state machines, decision points) is in
`docs/DESIGN.md`, the test report in `docs/TEST_REPORT.md`.

## Files

| File | Contents |
|---|---|
| `src/server.c` | server |
| `src/client.c` | client |
| `include/sbcp.h` | message and attribute types, length limits |
| `src/sbcp_codec.c`, `include/sbcp_codec.h` | server's message builder, validator and stream reassembler |
| `src/net.c`, `src/util.c` | socket helpers, logging, argument parsing |
| `tests/test_sbcp.c` | automated tests (`make test`) |
| `tests/demo.sh` | opens the server and clients in tmux for the screen captures |
| `tests/sbcp_probe.c` | `make probe`: shows which message formats a server answers |

## Interpretations and errata

1. **Length fields** count the bytes after their own 4-byte header: the
   message Length is the size of its attributes, an attribute Length the size
   of its payload. JOIN `alice` is `01 82 00 09 | 00 02 00 05 | alice`. The
   handout ("length of the SBCP message") also allows counting the header;
   the course's reference server does not, so we follow it. Our server also
   accepts clients that count the header (learnt from their first message)
   and answers each client in its own format.
2. **ACK**: Client Count including the new client, then the other members'
   usernames in join order. The client also shows an ACK that carries its text
   in a Message attribute.
3. **NAK** reasons: `Username already exists`, `Server full (max N clients)`,
   `Invalid username (1-16 chars)`, `Missing username`. The server then closes
   the connection; the client prints the reason and exits with status 1.
4. **Usernames**: 1–16 bytes, case-sensitive, no control characters. A name
   is free again as soon as its owner leaves.
5. **Messages**: SEND or IDLE before JOIN, and a second JOIN, are discarded.
   The server discards a SEND longer than 512 bytes; the client splits longer
   lines (at UTF-8 character boundaries) and does not send empty lines.
6. **IDLE**: sent once after 10 s without sending a chat message (received
   messages do not reset the clock), and again only after the next SEND.
7. `max_clients` is 1–1000; idle members count toward it.
8. End of input (Ctrl-D, or the end of a piped file) leaves the chat.
9. **Control characters**: the client shows control characters in received
   text as `^X`, so no message can move the cursor or clear the screen, and
   drops the escape codes that arrow and other special keys put into a typed
   line.
10. **Malformed messages from a server** (a required attribute missing, empty
    or too long) are discarded by the client, as the handout asks.
11. **Known limits**: a message that arrives while you are typing redraws the
    prompt, so the half-typed text disappears from the screen; it is still
    sent when you press Enter. Connecting to an address that never answers
    (e.g. the campus server from off campus without VPN) waits for TCP's
    connect timeout, about 2 minutes; Ctrl-C quits sooner.
