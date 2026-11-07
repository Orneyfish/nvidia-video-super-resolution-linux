#!/bin/bash
# GPU-free ctest for nvvsr_webd --idle-exit and systemd socket activation. usage: idle_exit.sh <nvvsr_webd>; exit 77 = skip.
bin=$1
command -v systemd-socket-activate >/dev/null || { echo "SKIP: systemd-socket-activate not found"; exit 77; }
tmp=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$tmp"' EXIT
fails=0
check() { local name=$1; shift; if "$@"; then echo "PASS $name"; else echo "FAIL $name"; fails=$((fails + 1)); fi; }
alive() { kill -0 "$1" 2>/dev/null; }
exits_within() { for _ in $(seq $(($2 * 10))); do alive "$1" || return 0; sleep 0.1; done; return 1; }
exits_clean() { wait "$1"; [ $? = 0 ]; }

# 1. socket activation: the daemon starts on our first connection, stays while it is open, exits 0 once it is closed
for try in 1 2 3 4 5; do   # a random port may be taken
  port=$((20000 + RANDOM % 20000))
  systemd-socket-activate -l 127.0.0.1:$port "$bin" --idle-exit 2 >"$tmp/act.out" 2>&1 &
  pid=$!
  for _ in $(seq 30); do grep -q "Listening on" "$tmp/act.out" && break; alive $pid || break; sleep 0.1; done
  alive $pid && break
done
check "activation: socket is up" alive $pid
exec 9<>/dev/tcp/127.0.0.1/$port
sleep 3.5   # longer than --idle-exit
check "activation: daemon took fd 3 (LISTENING $port)" grep -q "^LISTENING $port\$" "$tmp/act.out"
check "activation: still alive while a connection is open" alive $pid
exec 9>&-
check "activation: exits within 6 s of the last close" exits_within $pid 6
check "activation: exit status 0" exits_clean $pid

# 2. no activation: with nobody connecting the daemon leaves by itself
"$bin" 0 --idle-exit 2 >"$tmp/plain.out" 2>&1 &
pid=$!
sleep 0.5
check "plain: running" alive $pid
check "plain: exits by itself within 6 s" exits_within $pid 6
check "plain: exit status 0" exits_clean $pid

# 3. a socket that is not loopback is refused
systemd-socket-activate -l 0.0.0.0:$port "$bin" --idle-exit 2 >"$tmp/any.out" 2>&1 &
pid=$!
sleep 0.5
exec 9<>/dev/tcp/127.0.0.1/$port
exec 9>&-
exits_within $pid 3
wait $pid; rc=$?
check "non-loopback socket: refused (status 1)" test $rc = 1

# 4. a client dribbling the handshake one byte every 3 s (each recv would restart a per-recv timeout) is cut at the 10 s deadline
trap '' PIPE   # a write after the daemon closed must not kill this script
"$bin" 0 --idle-exit 60 >"$tmp/slow.out" 2>&1 &
pid=$!
for _ in $(seq 30); do grep -q "^LISTENING" "$tmp/slow.out" && break; sleep 0.1; done
port=$(sed -n 's/^LISTENING //p' "$tmp/slow.out")
dribble_closed_s() {   # prints seconds until the daemon closed; 99 = still open after 6 bytes (18 s)
  local req='GET /vsr' t0=$SECONDS i
  exec 8<>/dev/tcp/127.0.0.1/$port
  for ((i = 0; i < ${#req}; i++)); do
    printf '%s' "${req:i:1}" >&8
    read -t 3 -u 8 -n 1; [ $? = 142 ] || { echo $((SECONDS - t0)); exec 8>&-; return; }   # 142 = timeout, anything else = closed
  done
  echo 99; exec 8>&-
}
closed=$(dribble_closed_s)
check "slow handshake: closed within 12 s (took ${closed} s)" test "$closed" -le 12
kill $pid; wait $pid 2>/dev/null

echo "idle_exit: $fails failed"
[ $fails = 0 ]
