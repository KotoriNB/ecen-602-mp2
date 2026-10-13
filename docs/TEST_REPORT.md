# ECEN 602 MP2 — Test Report

Team: Shih-Yu (Frank) Yang, Bozhou Chen (Kotori)
Test machine: _fill in: OS, `uname -r`, `gcc --version`_

## 1. How we tested

1. **Required cases by hand** (section 2): the server and each client in its
   own terminal, with a screen capture per case. `./tests/demo.sh <case>` sets
   up the same windows in tmux.
2. **Against the course's reference server** (section 3), with its log.
3. **Automated tests** (section 4): `make test`.

## 2. Required test cases

Server: `./server 127.0.0.1 9000 3` unless stated. Clients:
`./client <name> 127.0.0.1 9000`.

| # | Case | Steps | Expected |
|---|---|---|---|
| 1 | Normal operation, three clients (`demo.sh three`) | start alice, bob, carol; each types a line | each line appears at the other two as `alice: ...`; ACK counts 1, 2, 3 with the earlier members listed; earlier members see `<bob> is online` |
| 2 | Duplicate username rejected (`demo.sh dup`) | with alice in, start a second alice | it prints `Reason: Username already exists` and exits; the first alice is unaffected |
| 3 | Username reused (`demo.sh reuse`) | alice presses Ctrl-D, then a new alice starts | the others see `<alice> is offline`, then `<alice> is online`; the new alice gets an ACK |
| 4 | Room full (`demo.sh full`) | `./server 127.0.0.1 9000 2`, start three clients | the third prints `Reason: Server full (max 2 clients)` and exits |
| 5a | Bonus: IPv6 (`demo.sh ipv6`) | `./server ::1 9000 3`, clients with `::1` | chat works as in case 1 |
| 5b | Bonus: ACK, NAK, ONLINE, OFFLINE | cases 1–4 | count and member list on join, NAK reasons, online/offline notices |
| 5c | Bonus: IDLE (`demo.sh idle`) | bob stays silent for 10 s | the others see `bob is idle` once; it repeats only after bob sends again and is silent another 10 s |

_Screen captures: one per case, here._

## 3. Against the course's reference server

`ecewkgsw05201.engr.tamu.edu:9000`, on the campus network or VPN. Its log is
at `http://ecewkgsw05201.engr.tamu.edu:9002/`.

```sh
./client kotori_a ecewkgsw05201.engr.tamu.edu 9000      # terminal 1
./client kotori_b ecewkgsw05201.engr.tamu.edu 9000      # terminal 2
```

| Step | Expected in the clients | Expected in the server log |
|---|---|---|
| A joins, then B | A: `Client count: 1`; B: `Client count: 2`, `Active users: kotori_a`; A: `<kotori_b> is online` | `JOIN (version: 3, length: 12)`, `USERNAME (length: 8)`, `New user`, `Sent ACK`, `Sent ONLINE` |
| A types `hello` | B: `kotori_a: hello` | `SEND (… length: 9)`, then `FWD` to B |
| a second `kotori_a` | `Reason: Username already exists`, exits | `Username kotori_a already exists`, `NAK` |
| B silent for 10 s | A: `kotori_b is idle` | `IDLE (version: 3, length: 0)` from B, `IDLE` to A |
| Ctrl-D in B | A: `<kotori_b> is offline` | `Handler for kotori_b disconnected`, `OFFLINE` |

Run on Oct 12, 18:50–18:51: join, ACK with count and name, ONLINE, chat in
both directions, IDLE and OFFLINE all appear in the log as expected.

_Screen captures of both clients and the matching log lines, here._

## 4. Automated tests

`make test` runs `tests/test_sbcp` (C). It starts the real `./server` and
`./client` and drives them over real sockets; `./tests/test_sbcp --list`
names every case.

- **Server, 44 cases**: raw connections send hand-built bytes. Exact bytes of
  every message, including the reference server's session from its log;
  messages split into single bytes or merged in one segment; garbage,
  unknown types, bad Length fields, messages that never finish; JOIN rules and
  NAK reasons; clients that reset before, during and after JOIN, also while
  the server is writing to them (the SIGPIPE case); a client that never reads;
  running out of descriptors; IPv6; Length fields that count the header.
- **Client, 35 cases**: the real client against the real server or a scripted
  server. ACK, NAK, FWD, ONLINE, OFFLINE and IDLE shown, also as the reference
  server sends them; exact JOIN, SEND and IDLE bytes; IDLE after 10 s while
  others keep talking; long, UTF-8 and piped input; messages arriving together
  or one byte at a time; IPv6; server shutdown and crash; bad arguments; the
  prompt on a terminal.

| Machine | Result |
|---|---|
| _test machine (fill in)_ | _last line of `make test`_ |
