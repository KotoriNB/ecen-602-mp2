# ECEN 602 MP2 — Test Report

Team: Shih-Yu (Frank) Yang, Bozhou Chen (Kotori)
Test machine: _fill in: OS, kernel (`uname -r`), gcc version_

## 1. How we tested

1. **Automated test suite** (`make test`, source `tests/test_sbcp.c`, written
   in C, 75 cases). It starts the real `./server` and `./client` and talks to
   them over real sockets.
   - **41 server cases** (`make test-server`) use raw connections that send
     hand-written bytes: unit tests of the server's codec, the bonus features,
     byte-exact messages, malformed input, and disconnect and
     resource-exhaustion cases (RST at every stage, a client that never reads,
     more connections than `FD_SETSIZE`, running out of descriptors). After
     every case the suite checks that the server is still alive and exits with
     status 0 on SIGTERM.
   - **34 client cases** (`make test-client`) run the real client against the
     real server, with raw sockets as the other chat members, or against a
     scripted fake server when a case needs exact bytes or malformed input.
     Each case controls exactly what reaches the client (two messages in one
     TCP segment, a message one byte at a time, garbage, an unfinished
     message, a member chatting while the client stays silent) and sees
     exactly what the client sends. One case runs the client on a
     pseudo-terminal to check its prompt.
   Child processes get SIGPIPE and SIGINT back at their defaults, so a program
   that forgot to ignore SIGPIPE dies here exactly as it would for a grader.
2. **Manual runs with screen captures** for the handout's required cases and
   each bonus feature (section 2). `./tests/demo.sh <scenario>` opens the
   server and its clients side by side in tmux and types the messages; the
   same steps can be typed by hand in four terminals.
3. **Interoperability**: `tests/sbcp_probe <host> <port>` joins a server in
   both byte layouts and shows which one it answers (section 4).

## 2. Required test cases and bonus features

Each case: start the server in one terminal and the clients in others (or run
the `demo.sh` scenario), then take one screen capture showing every window.
The client shows a `name> ` prompt, prints server notices as `[Server] ...`
and chat lines as `name: text`; what you type appears after the prompt.

### Case 1 — normal operation, three clients (`./tests/demo.sh three`)

```
./server 127.0.0.1 9000 3
./client alice 127.0.0.1 9000      then type: hello everyone, alice here
./client bob   127.0.0.1 9000      then type: hi alice, bob here
./client carol 127.0.0.1 9000      then type: hey both, carol here
```

Expected: each client's ACK line shows the count including itself
(`[Server] Joined successfully. Client count: 1`, then 2, then 3) and the
members already present (`[Server] Active users: alice, bob` for carol);
earlier clients see `[Server] User <bob> is online.` and
`[Server] User <carol> is online.`; every message appears at the other two
clients as `alice: hello everyone, alice here`, never back at its sender. The
server log shows each JOIN, ACK/ONLINE, and each SEND with "FWD to 2
client(s)".

_Screen capture 1: here_

### Case 2 — duplicate username rejected (`./tests/demo.sh dup`)

```
./server 127.0.0.1 9000 3
./client alice 127.0.0.1 9000
./client alice 127.0.0.1 9000      (second alice)
```

Expected: the second client prints `[Server Error] Join rejected. Reason:
Username already in use` and exits (status 1); the server logs the rejection;
the first alice keeps chatting normally.

_Screen capture 2: here_

### Case 3 — a username is reusable after its owner leaves (`./tests/demo.sh reuse`)

```
./server 127.0.0.1 9000 3
./client alice 127.0.0.1 9000
./client bob   127.0.0.1 9000
alice presses Ctrl-D (or Ctrl-C)
./client alice 127.0.0.1 9000      (new alice)
```

Expected: bob sees `[Server] User <alice> is offline.`, then
`[Server] User <alice> is online.`; the new alice gets `Client count: 2` and
`Active users: bob` and can chat.

_Screen capture 3: here_

### Case 4 — client beyond max_clients rejected (`./tests/demo.sh full`)

```
./server 127.0.0.1 9000 2
./client alice 127.0.0.1 9000
./client bob   127.0.0.1 9000
./client carol 127.0.0.1 9000
```

Expected: carol prints `[Server Error] Join rejected. Reason: Server full
(max 2 clients)` and exits; alice and bob see nothing about carol. After bob
leaves, carol can join.

_Screen capture 4: here_

### Case 5a — bonus Feature 1: ACK, NAK, ONLINE, OFFLINE

Shown by cases 1-4: ACK (count and list) in case 1, NAK in cases 2 and 4,
ONLINE and OFFLINE in case 3. Byte-level checks: server cases A5 and B1.

### Case 5b — bonus Feature 2: IDLE (`./tests/demo.sh idle`)

```
./server 127.0.0.1 9000 3
./client alice 127.0.0.1 9000      type one message, then nothing
./client bob   127.0.0.1 9000      type something 6 s later
```

Expected: 10 s after alice's last message, bob shows `alice is idle`, even
though bob kept talking in between (receiving messages does not count as
activity); 10 s after bob's own last message, alice shows `bob is idle`. No
second IDLE until the user sends again (server case A6, client checks I9 and
I10 check timing and repetition).

_Screen capture 5b: here_

### Case 5c — bonus IPv4/IPv6 (`./tests/demo.sh ipv6`)

```
./server :: 9000 4                 (dual-stack)
./client alice ::1 9000
./client bob   ::1 9000
./client carol 127.0.0.1 9000
```

Expected: all three in one room; the server log shows `[::1]:port` for alice
and bob and `127.0.0.1:port` for carol. Also works with `./server ::1 9000 3`
(IPv6 only).

_Screen capture 5c: here_

### Case 6 — robustness: a client killed (`./tests/demo.sh crash`)

Carol's client is killed with `kill -9`; alice and bob see
`[Server] User <carol> is offline.`, the server logs the departure and keeps
running, and a new carol joins immediately.

_Screen capture 6: here_

### Case 7 — the automated tests

`make test` — capture the summary line.

_Screen capture 7: here_

## 3. Automated cases

### Server cases (`make test-server`)

| ID | Case | What it proves |
|---|---|---|
| U1 | encoder produces the exact bytes | JOIN, IDLE, ACK and the legacy JOIN match hand-computed bytes |
| U2 | validator rejects malformed messages | 10 malformed messages rejected; zero padding tolerated |
| U3 | reassembler | byte-by-byte, coalesced, garbage, oversized and partial input |
| U4 | text rules | NUL stripping, username rules, `^X` log sanitising |
| U5 | both header layouts | all 128 type values decode in both layouts |
| A5 | ACK/NAK/ONLINE/OFFLINE contents | counts, lists, reasons, FIN after NAK |
| A6 | IDLE relay | relayed with the sender's name, once, reset by SEND, not spoofable |
| A9 | `localhost` | server reachable over 127.0.0.1 and ::1 |
| B1 | golden bytes | every message type byte-for-byte |
| B2 | no spoofing | a Username inside SEND is ignored |
| B3 | coalesced messages | two SENDs, and JOIN+SEND, in one segment |
| B4 | split message | one SEND delivered one byte at a time |
| B5 | NUL-terminated strings | accepted and stripped |
| B6 | message lengths | 1 and 512 relayed; 513, empty and missing discarded; stream stays in sync |
| B7 | username rules | 16 bytes, spaces and UTF-8 accepted; 17 bytes, empty, control characters, embedded NUL, missing name refused |
| B8 | legacy layout | little-endian bit-field peers answered in kind; mixed room works |
| B10 | order | 300 messages arrive in order |
| B11 | ACK order | members listed in join order, not descriptor order |
| C1 | garbage | telnet text, HTTP, binary; connection still usable |
| C2 | unknown types | discarded; the message after them still relayed |
| C3 | server-only types from a client | FWD/ACK/NAK/ONLINE/OFFLINE ignored |
| C4 | bad lengths | Length < 4, Length < bytes, attribute defects |
| C5 | incomplete message | discarded after 3 s; other clients unaffected meanwhile |
| C6 | before JOIN | SEND and IDLE discarded; JOIN afterwards works |
| C7 | second JOIN | ignored; identity unchanged |
| C8 | oversized message | 60 000-byte message skipped; next one relayed |
| C9 | fuzz | 3600 random messages over 12 connections; chat still works |
| D1 | RST before JOIN | 40 resets, some mid-JOIN |
| D2 | RST after JOIN | OFFLINE sent, name and slot freed |
| D3 | RST with output queued | the SIGPIPE case: server survives, OFFLINE sent |
| D4 | JOIN then RST | 50 times; ONLINE/OFFLINE balanced; room count correct |
| D6 | half-close | treated as leaving; server closes its side |
| D7 | mass RST | 50 clients reset during a 200-message burst |
| D8 | slow reader | 20 MB relayed to others while one client never reads; it is dropped at 1 MiB |
| D9 | EMFILE | server run with 16 descriptors survives 40 connections |
| D10 | FD_SETSIZE | 1150 connections; those past 1023 refused; server fine |
| D11 | stdout trouble | stdout closed, or a pipe nobody reads |
| D13 | RST mid-message | cleanup and OFFLINE; partial message not relayed |
| D14 | churn | 300 connect/leave cycles; no descriptor leak |
| D15 | SIGTERM and restart | clean exit 0; restart on the same port at once |
| F1 | server arguments | bad port/max_clients/IP, port in use: exit 1 |

### Client cases (`make test-client`)

| ID | Case |
|---|---|
| I1 | first member gets ACK count 1 |
| I2 | ACK lists the members already present; they get ONLINE |
| I3 | FWD displayed; a typed line is relayed to the others |
| I4 | OFFLINE displayed when a member leaves |
| I5 | duplicate name: NAK shown, client exits |
| I6 | room full: NAK shown |
| I7 | messages arriving together in one TCP segment are all shown: two FWDs (5 rounds), ONLINE + FWD + OFFLINE, a message plus half of the next |
| I8 | two members leaving at once: both OFFLINEs, arriving in one segment, are shown |
| I9 | IDLE about 10 s after JOIN while another member keeps talking |
| I10 | IDLE about 10 s after JOIN in a quiet room |
| I11 | two lines arriving together on stdin are both sent at once |
| I12 | a 600-character line is sent in full (512 + 88) |
| I13 | IPv6: client connects to ::1 (skipped where the machine has no IPv6) |
| I14 | host name `localhost` accepted |
| I15 | server shut down: client reports it and exits |
| I16 | end of input (Ctrl-D): client leaves, others see OFFLINE |
| I17 | piped input: every line is delivered before the client exits |
| I18 | an ACK larger than 1024 bytes (55 members) is shown |
| I19 | a name is reusable by a new client after its owner leaves |
| I20 | client killed with SIGKILL: others see OFFLINE, name reusable |
| I21 | server killed while the client is sending: client exits by itself, no SIGPIPE |
| I22 | one IDLE per idle spell, and a new one 10 s after the next SEND |
| I23 | UTF-8 split on character boundaries, CRLF stripped, 5000 bytes without newline |
| I24 | exact JOIN, SEND and IDLE bytes; an empty line sends nothing |
| I25 | a message delivered one byte at a time is shown once complete |
| I26 | unknown types, garbage and an unfinished message are discarded; chat continues |
| I27 | bad arguments and refused connection: message and exit status 1 |
| I28 | an unfinished last line is sent at end of input |
| I29 | an ACK carrying its text in a Message attribute is shown |
| I30 | a 4-byte Client Count is read correctly |
| I31 | 512-byte and UTF-8 messages from others are shown intact |
| I32 | Ctrl-C (SIGINT) on a client: others see OFFLINE |
| I33 | on a terminal the client shows a prompt |
| I34 | own messages are not shown back as if someone else sent them |

Results:

| Environment | Server cases | Client cases |
|---|---|---|
| _our test machine (fill in)_ | _fill in_ | _fill in_ |
| Ubuntu 22.04, kernel 6.8, gcc 11.4, IPv6 | 41/41 pass | 34/34 pass |
| Ubuntu 24.04 container, gcc 13.3 / clang 18, no IPv6 | 41/41 pass | 33/33 pass, I13 skipped |
| same, client built with ASan + UBSan | | 33/33 pass, no reports |

## 4. Interoperability

`tests/sbcp_probe ecewkgsw05201.engr.tamu.edu 9000` (on campus or VPN) shows
which byte layout the reference server speaks:

_Paste the probe output here._

_Other teams' clients/servers tried, and results: fill in._
