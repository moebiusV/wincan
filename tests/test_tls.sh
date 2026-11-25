#! /bin/sh
# test_tls.sh -- candled with TLS and client keys, with curl and openssl as the clients.
# Everything is on the loopback address, on a port picked at random, in a temporary directory.
#   CANDLED=path/to/candled sh tests/test_tls.sh
set -u
CANDLED=${CANDLED:-./candled}
command -v curl >/dev/null 2>&1 && command -v openssl >/dev/null 2>&1 || { echo "test_tls: curl and openssl are needed; skipped"; exit 77; }
T=$(mktemp -d)
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
fails=0; n=0
ok()   { n=$((n + 1)); echo "ok $n - $1"; }
fail() { n=$((n + 1)); fails=$((fails + 1)); echo "not ok $n - $1"; }
check() { if eval "$2"; then ok "$1"; else fail "$1"; fi; }

PORT=$(( 20000 + $(od -An -N2 -tu2 /dev/urandom | tr -d ' ') % 30000 ))
mkdir -p "$T/candles"
echo "# no settings: the defaults and the options" > "$T/empty.conf"
OPTS="--config $T/empty.conf --bind 127.0.0.1 --port $PORT --dir $T/candles --certificate $T/s.crt --key $T/s.key --authorized $T/authorized"

# the server's key and certificate
OUT=$($CANDLED --gen-cert --certificate "$T/s.crt" --key "$T/s.key" 2>&1)
check "--gen-cert makes a certificate and a key" '[ -s "$T/s.crt" ] && [ -s "$T/s.key" ]'
check "the key is private (mode 600)" '[ "$(stat -c %a "$T/s.key")" = 600 ]'
SFP=$($CANDLED --fingerprint --certificate "$T/s.crt")
check "--fingerprint is SHA256: and 64 hex digits" 'echo "$SFP" | grep -Eq "^SHA256:[0-9a-f]{64}$"'
OSFP=$(openssl x509 -in "$T/s.crt" -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr 'A-F' 'a-f')
check "the fingerprint is the SHA-256 of the certificate as openssl sees it" '[ "$SFP" = "SHA256:$OSFP" ]'
check "--gen-cert does not replace existing files" '$CANDLED --gen-cert --certificate "$T/s.crt" --key "$T/s.key" 2>&1 | grep -q "exist"'

# two clients, each with a key of its own
for c in c1 c2; do
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$T/$c.key" -out "$T/$c.crt" \
        -subj "/CN=$c" -days 2 -addext extendedKeyUsage=clientAuth >/dev/null 2>&1
done
FP1=$(openssl x509 -in "$T/c1.crt" -noout -fingerprint -sha256)      # sha256 Fingerprint=AB:CD:...
FP2=$(openssl x509 -in "$T/c2.crt" -noout -fingerprint -sha256)
check "--authorize takes openssl's fingerprint format" '$CANDLED --config "$T/empty.conf" --authorize "$FP1" --comment "client one" --authorized "$T/authorized" | grep -q "authorized: added"'
check "the authorized file names the fingerprint and the note" 'grep -q "^SHA256:.*client one" "$T/authorized"'
check "authorizing the same key twice is said, not repeated" '$CANDLED --config "$T/empty.conf" --authorize "$FP1" --authorized "$T/authorized" | grep -q "already"; [ "$(grep -c ^SHA256 "$T/authorized")" = 1 ]'
check "--authorize refuses what is not a fingerprint" '! $CANDLED --config "$T/empty.conf" --authorize "hello" --authorized "$T/authorized" 2>/dev/null'

# the server
$CANDLED $OPTS >"$T/log" 2>&1 &
PID=$!
i=0; while [ $i -lt 50 ]; do curl -sk --max-time 1 --cert "$T/c1.crt" --key "$T/c1.key" "https://127.0.0.1:$PORT/pairs" >/dev/null 2>&1 && break; sleep 0.1; i=$((i + 1)); done
GOT=$(curl -sk --max-time 5 --cert "$T/c1.crt" --key "$T/c1.key" "https://127.0.0.1:$PORT/pairs")
check "an authorized client is served over TLS" '[ "$GOT" = "[]" ]'
check "an unauthorized client key is refused" '! curl -sk --max-time 5 --cert "$T/c2.crt" --key "$T/c2.key" "https://127.0.0.1:$PORT/pairs" | grep -q "\["'
check "a client without a certificate is refused" '! curl -sk --max-time 5 "https://127.0.0.1:$PORT/pairs" | grep -q "\["'
check "plain HTTP to the TLS port is refused" '! curl -s --max-time 5 "http://127.0.0.1:$PORT/pairs" | grep -q "\["'
sleep 0.3
check "a refused client is logged with its fingerprint" 'grep -q "not authorized: SHA256:" "$T/log"'
$CANDLED --config "$T/empty.conf" --authorize "$FP2" --authorized "$T/authorized" >/dev/null
GOT=$(curl -sk --max-time 5 --cert "$T/c2.crt" --key "$T/c2.key" "https://127.0.0.1:$PORT/pairs")
check "a newly authorized client gets in without a restart" '[ "$GOT" = "[]" ]'
SEEN=$(openssl s_client -connect "127.0.0.1:$PORT" -cert "$T/c1.crt" -key "$T/c1.key" </dev/null 2>/dev/null | openssl x509 -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr 'A-F' 'a-f')
check "the certificate the server presents is the one fingerprinted" '[ "SHA256:$SEEN" = "$SFP" ]'
check "the old protocols are refused (TLS 1.1)" '! openssl s_client -connect "127.0.0.1:$PORT" -tls1_1 -cert "$T/c1.crt" -key "$T/c1.key" </dev/null 2>&1 | grep -q "BEGIN CERTIFICATE"'
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""

# the safety rules
$CANDLED --config "$T/empty.conf" --no-tls --bind 0.0.0.0 --port "$PORT" --dir "$T/candles" >"$T/log2" 2>&1; RC=$?
check "plain HTTP on a non-loopback address is refused (exit 2)" '[ $RC = 2 ] && grep -q "refusing" "$T/log2"'
$CANDLED --config "$T/empty.conf" --bind 127.0.0.1 --port "$PORT" --dir "$T/candles" --certificate "$T/none.crt" --key "$T/none.key" >"$T/log3" 2>&1; RC=$?
check "TLS without a certificate says how to make one (exit 2)" '[ $RC = 2 ] && grep -q "gen-cert" "$T/log3"'
$CANDLED --config "$T/empty.conf" --no-tls --bind 127.0.0.1 --port "$PORT" --dir "$T/candles" >"$T/log4" 2>&1 &
PID=$!
i=0; while [ $i -lt 50 ]; do curl -s --max-time 1 "http://127.0.0.1:$PORT/pairs" >/dev/null 2>&1 && break; sleep 0.1; i=$((i + 1)); done
check "plain HTTP works on the loopback address" '[ "$(curl -s --max-time 5 "http://127.0.0.1:$PORT/pairs")" = "[]" ]'
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""
check "--check-config accepts a good configuration" '$CANDLED --config "$T/empty.conf" --check-config --no-tls | grep -q "configuration is good"'
printf 'port 99999\n' > "$T/bad.conf"
check "--check-config refuses a bad one (exit 2)" '$CANDLED --config "$T/bad.conf" --check-config >/dev/null 2>&1; [ $? = 2 ]'

[ $fails = 0 ] && echo "all $n TLS checks passed" || echo "$fails of $n TLS checks failed"
[ $fails = 0 ]
