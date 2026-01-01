#!/bin/bash
set -euo pipefail

PROXY_BIN=./build/proxy-http
PROXY_LOG=/tmp/proxy_test.log

echo "[test] starting proxy: $PROXY_BIN"
# Запускаем прокси в фоне, лог пишем в файл
$PROXY_BIN >"$PROXY_LOG" 2>&1 &
PROXY_PID=$!

cleanup() {
    echo "[test] stopping proxy (pid=$PROXY_PID)"
    kill "$PROXY_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Дадим прокси подняться
sleep 1

echo "[test] 1) basic GET via proxy to example.com"
BODY=$(curl -s -x 127.0.0.1:80 http://example.com/)
echo "$BODY" | grep -q "Example Domain"
echo "[test]    body contains 'Example Domain' — OK"

echo "[test] 2) check HTTP status code 200"
STATUS=$(curl -s -o /dev/null -w "%{http_code}" -x 127.0.0.1:80 http://example.com/)
if [ "$STATUS" != "200" ]; then
    echo "[test]    expected status 200, got $STATUS"
    exit 1
fi
echo "[test]    status 200 — OK"

echo "[test] 3) multiple parallel requests through proxy (3 clients, timeout 10s)"
PIDS=()
for i in 1 2 3; do
    # -m 10: максимум 10 секунд на запрос, чтобы тест не завис навсегда
    curl -s -m 10 -x 127.0.0.1:80 http://example.com/ >/dev/null &
    PIDS+=($!)
done

# Ждём все три curl
for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then
        echo "[test]    one of parallel curl requests failed or timed out"
        exit 1
    fi
done
echo "[test]    3 parallel requests completed — OK"

echo "[test] ALL TESTS PASSED"