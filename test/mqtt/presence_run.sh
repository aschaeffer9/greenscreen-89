#!/bin/bash
# presence / invitation scenarios against a local TLS broker (no internet needed)
cd "$(dirname "$0")"
[ -f trust.jks ] || ./make_test_cert.sh >/dev/null
J=${JAVA_HOME:-$(dirname $(dirname $(readlink -f $(which java))))}
gcc -O1 -w -DNET_DESKTOP_TEST -I$J/include -I$J/include/linux -I../../android presence_test.c ../../android/net.c \
  -L$J/lib/server -ljvm -Wl,-rpath,$J/lib/server -lpthread -o ptest || exit 1
python3 broker.py 18884 cert.pem key.pem > broker.log 2>&1 & BP=$!; trap "kill $BP 2>/dev/null" EXIT; sleep 1
P() { timeout 60 ./ptest localhost:18884 trust.jks "$@" 2>&1 | grep --line-buffered '^\['; }
W() { for p in $BG; do wait $p; done; BG=""; }      # wait for the background players only (not the broker)
B() { "$@" & BG="$BG $!"; }
echo "== 1: who is free (one free, one busy, one that crashes after 3 s)"
B eval "P free-decline 12 > p1a.log"; B eval "P busy 12 > p1b.log"; B eval "P crash 12 > p1c.log"; sleep 0.5; P watch 10; W; cat p1c.log
echo "== 2: asking a free player who says no -> waits, then gives up"
B eval "P free-decline 16 > p2.log"; sleep 0.5; SEARCH_TIMEOUT=10 P search 16 | grep -v "WAITING 0:0[1-8]"; W; cat p2.log
echo "== 3: a busy player is never asked"
B eval "P busy 12 > p3.log"; sleep 0.5; SEARCH_TIMEOUT=6 P search 12 | grep -v "WAITING 0:0[1-4]"; W; cat p3.log
echo "== 4: asking a free player who accepts -> linked"
B eval "P free-accept 25 > p4.log"; sleep 0.5; P search 25; W; cat p4.log
echo "== 5: two players searching find each other"
B eval "P search 25 > p5.log"; sleep 0.3; P search 25 | grep "LINKED\|exchanged\|ended"; W; grep "LINKED\|exchanged\|ended" p5.log
rm -f p?.log p1?.log
