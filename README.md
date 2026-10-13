# ECEN 602 Machine Problem 2: TCP Simple Broadcast Chat (SBCP)

A chat server and client for the Simple Broadcast Chat Protocol, version 3,
over TCP. The server is one process that multiplexes every connection with
`select()`; the client multiplexes the keyboard and the server socket with
`select()`. All mandatory features and all three bonus features are
implemented: IPv4/IPv6, Feature 1 (ACK, NAK, ONLINE, OFFLINE) and Feature 2
(IDLE).

## Team and roles

| Member | Role |
|---|---|
| Shih-Yu (Frank) Yang | Client (`src/client.c`), shared protocol header (`include/sbcp.h`), test report and screen captures |
| Bozhou Chen (Kotori) | Server (`src/server.c`), server message codec (`src/sbcp_codec.c`), socket and logging helpers (`src/net.c`, `src/util.c`), Makefile, server tests (`tests/test_sbcp.c`) |

## Build and usage

```sh
make                 # builds ./server and ./client
make test            # all tests: 43 server + 34 client cases, about 1.5 min
make test-server     # server cases only, about 25 s
make test-client     # client cases only, about 75 s
make test-quick      # all but the slow timing cases, about 30 s
make probe           # tests/sbcp_probe: which byte layout does a server speak?
make clean           # removes binaries, object files and test logs
make dist            # clean, then ../ecen-602-mp2.tar.gz for Canvas

./server <server_ip> <server_port> <max_clients>
./client <username> <server_ip> <server_port>
```

```sh
./server 127.0.0.1 9000 3        # IPv4
./server ::1 9000 3              # IPv6 loopback
./server :: 9000 3               # every interface, IPv4 and IPv6 clients together
./client alice 127.0.0.1 9000
./client bob ::1 9000
```

In the client, type a line and press Enter to send it. Ctrl-D leaves the
chat (the server announces OFFLINE); Ctrl-C quits at once, which the server
treats the same way. Both programs also accept host names such as
`localhost` for `server_ip`.

`gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -g`; no warnings with gcc 11 and
13 or clang 18. Only libc and the POSIX socket API.

## Features

| Spec item | Where | Notes |
|---|---|---|
| JOIN / SEND / FWD | `src/client.c`, `src/server.c` | FWD carries Message then Username, as in the spec's attribute table |
| Client: prompt, `select()` over stdin and socket, discard unknown messages | `src/client.c` | prompt `name> ` on a terminal; received chat lines print as `name: text` |
| Server: IP/port/max from argv, FWD to all but sender, unique names, cleanup on unceremonious exit, discard unknown messages | `src/server.c` | |
| Length checked against bytes received | `src/client.c`, `src/sbcp_codec.c` | both sides cut the TCP byte stream into messages by the Length field, in the reference server's format (see Wire format) |
| Bonus: IPv4 and IPv6 | `src/net.c`, `src/client.c` | `getaddrinfo()` with `AF_UNSPEC`; `::` is dual-stack |
| Bonus Feature 1: ACK, NAK, ONLINE, OFFLINE | `src/server.c`, `src/client.c` | ACK count includes the new client, the name list excludes it |
| Bonus Feature 2: IDLE | `src/client.c`, `src/server.c` | once per 10 s of not sending; relayed with the idle user's name |

## Files

| File | Contents |
|---|---|
| `include/sbcp.h` | Shared protocol definitions: message and attribute types, length limits, header and attribute layouts |
| `src/client.c` | Client: connect, JOIN, `select()` loop, message display, idle timer |
| `src/server.c` | Server: event loop, JOIN/SEND/IDLE handling, broadcasts, cleanup |
| `src/sbcp_codec.c`, `include/sbcp_codec.h` | Server's message builder, validator and TCP stream reassembler |
| `src/net.c`, `include/net.h` | IPv4/IPv6 listen and connect, address formatting |
| `src/util.c`, `include/util.h` | Timestamped logging, strict number parsing, monotonic clock |
| `docs/DESIGN.md` | Conceptual model and design decisions (the required design sketch) |
| `docs/TEST_REPORT.md` | Test report: method, cases, screen captures |
| `tests/test_sbcp.c` | Test suite in C: 43 server cases and 34 client cases, non-zero exit on failure |
| `tests/demo.sh` | tmux scenarios for the screen captures (`./tests/demo.sh three`) |
| `tests/sbcp_probe.c` | Joins a server in each message format and reports which ones it answers |

## Architecture

```
server (one process)                          client
--------------------                          ------
socket/bind/listen on every address           getaddrinfo, connect, send JOIN
of server_ip (non-blocking)                   loop:
loop:                                           select(stdin, socket;
  run timers (partial messages, NAK linger)            timeout = rest of the 10 s idle clock)
  select(listen fds, client fds,                socket readable -> buffer -> whole messages -> print
                                                (an unfinished message is dropped after 3 s)
         write fds with queued output)          stdin readable  -> read() -> lines -> SEND (<= 512 B each)
  accept new clients                            10 s without sending -> IDLE (once)
  flush queued output                           NAK or server gone -> print, exit 1
  read -> reassemble -> JOIN/SEND/IDLE          stdin EOF -> exit 0
  reap: OFFLINE for leavers, close, free
```

Key decisions (details in `docs/DESIGN.md`):

- One `select()` loop, no `fork()`: every client shares one room, so the
  server keeps all state in one table indexed by descriptor.
- Server sockets are non-blocking with a per-client output queue. A client
  that stops reading costs memory, never time; past 1 MiB of backlog it is
  dropped. One stalled client cannot freeze the chat.
- `SIGPIPE` is ignored in both programs and the server passes `MSG_NOSIGNAL`
  to every `send()`: writing to a peer that reset the connection returns
  `EPIPE`, it does not kill the process (the MP1 deduction).
- A server-side client is never freed in the middle of a round. Errors only
  mark it dead; `reap()` announces OFFLINE and closes it at the end of the
  round.
- TCP is a byte stream: both programs buffer what they receive and cut it
  into messages by the Length field, so split and coalesced messages both
  work.
- The client reads stdin with `read()`, not `fgets()`: lines held in stdio's
  buffer are invisible to `select()`.

## Wire format

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Vrsn = 3, 9 bits| Type, 7 bits| Length: bytes after the header|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Attribute Type                | Length: payload bytes         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Attribute payload (Length bytes) ...                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

All fields are in network byte order. Each Length counts the bytes after its
own 4-byte header: the message Length is the size of all its attributes, an
attribute Length the size of its payload. This is the format of the course's
reference server (ecewkgsw05201.engr.tamu.edu:9000). `JOIN alice` on the wire:
`01 82 00 09 | 00 02 00 05 | 61 6c 69 63 65`. Strings carry no terminating
NUL; a trailing NUL from another implementation is accepted and stripped by
the server.

The server also understands two other encodings and answers each client in
its own: Length fields that also count the 4-byte header (the other reading
of the handout, learnt from the client's first message), and a C bit-field
struct written straight from an x86 host (first byte `03` instead of `01`).

## Robustness

The server keeps serving through: a client reset (RST) before, during or
after JOIN, including while the server has output queued for it; half-close;
text, HTTP or random bytes; wrong Length fields; unknown message types;
messages that never finish; a 60 KB message; a client that never reads while
20 MB flows past; more than `FD_SETSIZE` connections; running out of file
descriptors; a closed or unread stdout. `SIGINT`/`SIGTERM` close every
connection and exit 0. The client exits with a message and status 1, never a
signal, when it is refused or the server goes away.

## Testing

`make test` runs `tests/test_sbcp` (C), which starts the real binaries and
drives them over real sockets. The 43 server cases (U, A, B, C, D, F) use raw
connections with hand-built messages: unit tests of the codec, the bonus
features, byte-exact message checks in both Length formats, malformed input,
and disconnect and resource-exhaustion cases. The 34 client cases (I) run the real client against
the real server with raw sockets as the other members, or against a scripted
fake server for exact bytes and malformed input: join, ACK in both forms,
relays, NAKs, name reuse, messages arriving together or one byte at a time,
idle timing, long and UTF-8 lines, piped input, IPv6, the server dying
mid-send, argument errors, and the prompt on a pseudo-terminal.

| Environment | Server cases | Client cases |
|---|---|---|
| Ubuntu 24.04, gcc 13.3 / clang 18 (no IPv6) | 43/43 | 33/33, IPv6 case skipped |
| same, both programs built with AddressSanitizer + UndefinedBehaviorSanitizer | 43/43, no reports | 33/33, no reports |

The method, the case list and the screen captures are in
`docs/TEST_REPORT.md`.

## Interpretations and errata

1. Each Length counts the bytes after its own 4-byte header (message Length
   = all attributes, attribute Length = payload). The handout ("indicates the
   length of the SBCP message/attribute") allows this or counting the header;
   the course's reference server uses this one and drops a client whose
   Length counts the header. Our server accepts both, learns a client's
   format from its first message and answers in the same format.
2. A NAKed client gets the reason, then the server closes the connection
   (after the NAK is flushed, so it is never lost to an RST). The client
   prints the reason and exits with status 1.
3. NAK reasons: `Username already in use`, `Server full (max N clients)`,
   `Invalid username (1-16 chars)`, `Missing username`.
4. Usernames are 1-16 bytes, case-sensitive, without control characters.
5. SEND or IDLE before JOIN, and a second JOIN on a joined connection, are
   discarded (and logged); the connection stays open.
6. A line longer than 512 bytes is sent as several SENDs, cut on a UTF-8
   character boundary; the server discards any SEND whose text exceeds 512.
   Empty lines are not sent.
7. IDLE goes out once per idle spell: after 10 s without sending a chat
   message (messages received do not count), and again only after the user
   has sent something. The server ignores a repeated IDLE from an already
   idle client.
8. The ACK lists usernames in the order their owners joined.
9. `max_clients` must be 1-1000 (every connection must fit in `select()`'s
   `FD_SETSIZE`).
10. End of stdin (Ctrl-D, or the end of a piped file) leaves the chat. To keep
    a scripted client connected, keep its stdin open, e.g.
    `(echo hi; sleep 60) | ./client alice 127.0.0.1 9000`.
11. A client that connects and never JOINs is kept until it disconnects;
    there is no JOIN timeout.
12. Both programs discard a message whose bytes do not all arrive within 3 s
    (the handout's "wait, then discard" approach), so one broken message
    cannot corrupt the ones after it.
13. The client shows an ACK in either form the handout allows: Client Count
    and Username attributes, or a single Message attribute with the text.
