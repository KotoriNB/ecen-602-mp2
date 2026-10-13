#!/usr/bin/env bash
#
# demo.sh: scripted tmux sessions for the test-report screen captures.
# Each scenario opens the server and its clients side by side, types the
# messages for you, and leaves everything on screen for the capture.
#
# Usage: ./tests/demo.sh <scenario> [port]
#
# three: required case 1, three clients chatting
# dup: required case 2, duplicate username rejected
# reuse: required case 3, a username reused after its owner left
# full: required case 4, client beyond max_clients rejected
# idle: bonus, IDLE after 10 s of silence
# ipv6: bonus, IPv6 clients, plus IPv4 and IPv6 on one dual-stack server
# crash: robustness, a client killed with SIGKILL, then rejoining
#
# Detach with Ctrl-b d; the session is killed the next time the scenario runs.
# No tmux? Run the same steps by hand, one terminal per program (docs/TEST_REPORT.md).

set -u
cd "$(dirname "$0")/.." || exit 1

SCEN=${1:-}
PORT=${2:-9000}
S="sbcp-$SCEN"

if [ ! -x ./server ] || [ ! -x ./client ]; then
    echo "build first: make" >&2
    exit 1
fi
if ! command -v tmux >/dev/null 2>&1; then
    echo "tmux is not installed; follow the manual steps in docs/TEST_REPORT.md" >&2
    exit 1
fi

HOLD="; echo; read -r -p '[exited; Enter closes this pane] ' _"

# New session whose first pane (0) runs the server.
server() {
    tmux kill-session -t "$S" 2>/dev/null
    tmux new-session -d -s "$S" -x 220 -y 55 "./server $* $HOLD"
    sleep 0.6
}

# Next pane runs a client; panes are numbered in the order they were made.
# The client starts after the panes are retiled, so its first lines are not cut off.
client() {
    tmux split-window -t "$S" "sleep 0.3; ./client $* $HOLD"
    tmux select-layout -t "$S" tiled >/dev/null
    sleep 0.6
}

say() {
    tmux send-keys -t "$S:0.$1" "$2" Enter
    sleep 0.6
}

case "$SCEN" in
three)
    server 127.0.0.1 "$PORT" 3
    client alice 127.0.0.1 "$PORT"
    client bob 127.0.0.1 "$PORT"
    client carol 127.0.0.1 "$PORT"
    ( say 1 "hello everyone, alice here"
      say 2 "hi alice, bob here"
      say 3 "hey both, carol here" ) &
    ;;
dup)
    server 127.0.0.1 "$PORT" 3
    client alice 127.0.0.1 "$PORT"
    # rejected: name taken
    client alice 127.0.0.1 "$PORT"
    ( sleep 0.5; say 1 "I am still the only alice" ) &
    ;;
reuse)
    server 127.0.0.1 "$PORT" 3
    client alice 127.0.0.1 "$PORT"
    client bob 127.0.0.1 "$PORT"
    ( say 1 "alice signing off"
      # alice leaves (end of input)
      tmux send-keys -t "$S:0.1" C-d
      sleep 1.5
      tmux split-window -t "$S" "sleep 0.3; ./client alice 127.0.0.1 $PORT $HOLD"
      tmux select-layout -t "$S" tiled >/dev/null
      sleep 0.8
      say 3 "a new alice, same name" ) &
    ;;
full)
    server 127.0.0.1 "$PORT" 2
    client alice 127.0.0.1 "$PORT"
    client bob 127.0.0.1 "$PORT"
    # rejected: max_clients is 2
    client carol 127.0.0.1 "$PORT"
    ;;
idle)
    server 127.0.0.1 "$PORT" 3
    client alice 127.0.0.1 "$PORT"
    client bob 127.0.0.1 "$PORT"
    ( say 1 "going quiet now; watch for the IDLE notices in about 10 s"
      sleep 6
      say 2 "bob is still talking" ) &
    ;;
ipv6)
    # dual-stack: IPv6 and IPv4 clients
    server :: "$PORT" 4
    client alice ::1 "$PORT"
    client bob ::1 "$PORT"
    client carol 127.0.0.1 "$PORT"
    ( say 1 "hello over IPv6"
      say 3 "hello over IPv4, same room" ) &
    ;;
crash)
    server 127.0.0.1 "$PORT" 3
    client alice 127.0.0.1 "$PORT"
    client bob 127.0.0.1 "$PORT"
    # Inner sh records the client's own pid, so kill -9 hits the client itself.
    tmux split-window -t "$S" "sleep 0.3; sh -c 'echo \$\$ > /tmp/sbcp-demo-carol.pid; exec ./client carol 127.0.0.1 $PORT' $HOLD"
    tmux select-layout -t "$S" tiled >/dev/null
    sleep 0.6
    ( say 3 "carol is about to be killed"
      kill -9 "$(cat /tmp/sbcp-demo-carol.pid)" 2>/dev/null
      sleep 1
      tmux split-window -t "$S" "sleep 0.3; ./client carol 127.0.0.1 $PORT $HOLD"
      tmux select-layout -t "$S" tiled >/dev/null
      sleep 0.8
      say 4 "carol is back" ) &
    ;;
*)
    sed -n '3,18p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac

if [ -n "${TMUX:-}" ]; then
    tmux switch-client -t "$S"
else
    tmux attach -t "$S"
fi
