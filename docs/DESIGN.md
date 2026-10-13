# MP2 — Conceptual model and design sketch

## 1. What the service is

SBCP is a one-room chat over TCP. A client connects, sends JOIN with a
username, and from then on everything it SENDs is relayed by the server as
FWD to every other member. With the bonus features the server confirms a JOIN
with ACK (member count and names) or refuses it with NAK (reason), announces
arrivals and departures with ONLINE/OFFLINE, and relays IDLE when a member has
not sent anything for 10 seconds.

```
 client A ──JOIN/SEND/IDLE──▶ ┌──────────────┐ ──FWD/ONLINE/OFFLINE/IDLE──▶ client B
 client C ◀──ACK/NAK/FWD...── │ server: one  │ ──FWD/ONLINE/OFFLINE/IDLE──▶ client D
                              │ select() loop│
                              └──────────────┘
```

## 2. Data model

Both programs take the protocol numbers (message and attribute types, length
limits) from one header, `include/sbcp.h`. The client builds and reads
messages through its packed `struct sbcp_header` / `struct sbcp_attr` with
`htons()`/`ntohs()`; the server packs and unpacks fields with byte shifts in
`src/sbcp_codec.c`. Both produce the same network-byte-order bytes, with
Length fields that count the bytes after their own 4-byte header, as the
course's reference server does.

### Server

| State | Type | Lifetime |
|---|---|---|
| `clients[FD_SETSIZE]` | table of `struct client *`, indexed by descriptor | process |
| `listen_fds[]` | one listening socket per address of `server_ip` | process |
| `n_joined`, `max_clients` | room occupancy and capacity | process |
| `spare_fd` | `/dev/null` kept open, given up to shed a connection at EMFILE | process |
| `sig_pipe[2]` | self-pipe that wakes `select()` on SIGINT/SIGTERM | process |

`struct client`, one per connection:

| Field | Purpose |
|---|---|
| `fd`, `peer` | socket and printable address |
| `state` | NEW → JOINED, or NEW → REJECTED (see §3) |
| `name`, `join_seq` | username once joined; join order for the ACK list |
| `idle` | bonus: sent IDLE and nothing since |
| `wire` | format the peer speaks (byte layout; Length counting the header or not); replies use the same one |
| `rd` | reassembly buffer (2 KB) for bytes that are not a whole message yet |
| `out` | output queue: bytes the kernel would not take yet (≤ 1 MiB) |
| `dead`, `offline_due` | marked for removal; OFFLINE still owed to the room |
| `linger_until`, `shut_after_flush` | graceful close after a NAK |

### Client

`sockfd`; `recv_buf` (64 KB), the bytes from the server that do not form a
whole message yet (any SBCP message fits, an ACK can list 1000 names), and
`partial_since`, when that unfinished message began; `line_buf` (4 KB), typed
bytes whose newline has not arrived; `last_send` and `is_idle` for the idle
clock; `show_prompt`, whether stdin and stdout are terminals.

## 3. State machines

Server, per connection:

```
            accept()
               │
               ▼
          ┌─────────┐  JOIN ok: ACK to it, ONLINE to others   ┌─────────┐
          │   NEW   │ ───────────────────────────────────────▶│ JOINED  │◀─┐ SEND: FWD to others
          └─────────┘                                         └─────────┘──┘ IDLE: IDLE to others
            │    │ JOIN refused: NAK                               │
            │    ▼                                                 │ EOF, RST, send error,
            │ ┌──────────┐ client closes, or 3 s pass              │ backlog > 1 MiB
            │ │ REJECTED │──────────────┐                          ▼
            │ └──────────┘              │                     ┌─────────┐
            └── EOF / RST ──────────────┴────────────────────▶│  dead   │ reap(): OFFLINE (if it
                                                              └─────────┘ was JOINED), close, free
```

Client:

```
 connect, send JOIN ─▶ CHATTING ── ACK, FWD, ONLINE, OFFLINE, IDLE: print, stay
                          │
                          ├── NAK: print the reason, exit 1
                          ├── server closes or resets: print why, exit 1
                          └── stdin EOF: send the unfinished line, close, exit 0
```

Idle sub-state of a member: ACTIVE ──10 s without SEND──▶ IDLE (one IDLE sent)
──next SEND──▶ ACTIVE.

## 4. Message sequences

```
 alice            server             bob              carol (room full)
   │── JOIN alice ─▶│                  │                    │
   │◀─ ACK (2; bob) │── ONLINE alice ─▶│                    │
   │── SEND hi ────▶│── FWD hi, alice ▶│                    │
   │   (10 s quiet) │                  │                    │
   │── IDLE ───────▶│── IDLE alice ───▶│                    │
   │                │◀────────────────────────── JOIN carol │
   │                │── NAK "Server full (max 2 clients)" ─▶│ FIN after the NAK
   │ (Ctrl-C: FIN or RST)              │                    │
   ╳                │── OFFLINE alice ▶│                    │
```

## 5. Event loops

Server:

```
loop until SIGINT/SIGTERM:
    run_timers()        drop messages unfinished after 3 s; close NAKed clients after 3 s;
                        settle a first message's Length format after 250 ms of quiet
    reap()              OFFLINE for leavers, close and free dead clients
    rset = sig_pipe + listening sockets + every client; wset = clients with queued output
    select(rset, wset, timeout = nearest timer)
    accept_clients()    non-blocking; refuse fd >= FD_SETSIZE; shed one at EMFILE
    for each client: flush() if writable, on_readable() if readable
    reap()
```

`on_readable()`: one `recv()` into the reassembly buffer, then every complete
message goes to `dispatch()` → `handle_join()`, `handle_send()` or
`handle_idle()`; anything else is logged and discarded.

Client:

```
loop:
    select(socket, stdin; timeout = nearest of last_send + 10 s (unless idle)
                                    and partial_since + 3 s (if a message is unfinished))
    unfinished message older than 3 s → discarded
    10 s since the last SEND and not idle yet → IDLE
    socket: recv() into recv_buf → process_message() for each complete message
            (FWD/ACK/NAK/ONLINE/OFFLINE/IDLE shown, anything else discarded)
    stdin:  read() into line_buf → each complete line → SEND (≤ 512 bytes each)
            → last_send = now; EOF → leave
```

## 6. Major decision points

| # | Decision | Chosen | Rejected, and why |
|---|---|---|---|
| D1 | Concurrency | one `select()` loop | `fork()`: children cannot see each other's clients, a shared room needs IPC; threads: locking for no gain at this scale |
| D2 | Framing | buffer driven by the Length field, in both programs | "one `read()` = one message" loses messages when TCP splits or merges them |
| D3 | Bad input | discard, keep the connection | closing on the first bad byte makes one typo fatal; the handout says discard |
| D4 | Bad header | drop everything buffered | with the Length unreadable there is no next boundary to find |
| D5 | Oversized message | skip exactly its bytes (Length + 4) | keeps the next message; dropping the connection would punish a peer for one message |
| D6 | Writes | non-blocking + per-client queue, 1 MiB cap | a blocking `send()` to a client that stopped reading freezes every client |
| D7 | Disconnect during a broadcast | mark dead, reap at the end of the round | freeing inside the loop that is iterating the table is a use-after-free |
| D8 | SIGPIPE | `SIG_IGN` + `MSG_NOSIGNAL` | default action kills the server on the first write to a reset client |
| D9 | Refusing a JOIN | NAK, then FIN once the NAK is flushed, drain until the client closes (3 s cap) | `close()` right away sends RST if the client's bytes are unread, and RST can wipe out the NAK |
| D10 | Byte order | network order with explicit conversion: byte shifts (server), `htons()` on packed structs (client) | C bit-fields: their layout is implementation-defined (C11 §6.7.2.1) |
| D11 | Addresses | `getaddrinfo(AF_UNSPEC)` in both programs; the server binds every address of `server_ip`, `::` is dual-stack | `inet_pton` + `AF_INET` would be IPv4-only |
| D12 | Idle clock | time since the last SEND on `CLOCK_MONOTONIC`, one IDLE per idle spell | a fixed 10 s `select()` timeout restarts whenever a message arrives; wall-clock time jumps; repeating IDLE floods the room |
| D13 | stdin | `read()` into our own line buffer | `fgets()` hides buffered lines from `select()` |
| D14 | Other formats | server accepts little-endian bit-field peers and Length fields that count the header, answers in kind | interoperability with other teams; the reference server's format stays the default |
| D15 | Length fields | count the bytes after their own header, as the reference server on ecewkgsw05201 does; a client's format is learnt from its first message (after a whole message come the next header or nothing, inside a longer one come payload bytes) | counting the header, the other reading of the handout: the reference server then waits for 4 bytes that never come and drops the client |

## 7. Error policy

| Class | Examples | Action |
|---|---|---|
| Fatal at startup | bad arguments, `bind`/`listen` failure | message, exit 1 |
| One client's fault | EOF, `ECONNRESET`, `EPIPE`, backlog over 1 MiB | that client is dropped; OFFLINE if it had joined |
| One message's fault | bad version or Length, unknown type, bad attribute | message discarded and logged |
| Transient | `EINTR`, `EAGAIN`, `ECONNABORTED`, `EMFILE`, `ENOBUFS` | retry next round; nobody is dropped |
| Never | anything | the server does not exit because of a client |

## 8. Hazards and where they are handled

| Hazard | Handled in |
|---|---|
| Client resets while the server writes to it | `flush()`: `EPIPE`/`ECONNRESET` → `kill_client()` |
| Client resets while being iterated in a broadcast | `kill_client()` only marks; `reap()` frees later |
| Same name joins again right after the owner left | `find_joined()` ignores dead clients; `flush_offlines()` runs before the ONLINE |
| Message split across segments / several in one segment | server: `sbcp_reader_next()`; client: `handle_server_message()` |
| Message never completes | server: `run_timers()`; client: main loop; dropped after 3 s |
| Descriptor ≥ `FD_SETSIZE` | refused at `accept()` |
| Out of descriptors | spare-descriptor trick in `shed_connection()` |
| stdout is a full or closed pipe | `src/util.c`: zero-timeout `select()` before each log line; `SIGPIPE` ignored |
| Restart while old connections are in TIME_WAIT | `SO_REUSEADDR` |
| Signal between the flag check and `select()` | self-pipe wakes `select()` |
| Username with terminal escape codes | server rejects control characters in names |
| Spoofed sender | FWD always carries the name from the JOIN, never one from SEND |
