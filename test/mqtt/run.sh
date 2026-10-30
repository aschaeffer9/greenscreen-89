#!/bin/bash
# online matchmaking tests against a local TLS MQTT broker (no internet needed):
# direct P2P, broker relay only, and a direct path that dies mid-game
set -e
cd "$(dirname "$0")"
[ -f trust.jks ] || ./make_test_cert.sh
J=${JAVA_HOME:-$(dirname $(dirname $(readlink -f $(which java))))}
CF="-O1 -w -DNET_DESKTOP_TEST -I$J/include -I$J/include/linux -I../../android"
LF="-L$J/lib/server -ljvm -Wl,-rpath,$J/lib/server -lpthread"
gcc $CF mqclient_test.c ../../android/net.c $LF -o mqtest
gcc $CF fallback_test.c ../../android/net.c $LF -o fbtest
python3 broker.py 18883 cert.pem key.pem > broker.log 2>&1 & BP=$!; trap "kill $BP 2>/dev/null" EXIT; sleep 1
for mode in direct relay; do
  E=""; [ $mode = relay ] && E="NO_DIRECT=1"
  (env $E ./mqtest localhost:18883 a trust.jks 2>&1 | grep '^\[' > a.log &); env $E ./mqtest localhost:18883 b trust.jks 2>&1 | grep '^\['; sleep 3; cat a.log
done
(./fbtest localhost:18883 a trust.jks 2>&1 | grep '^\[' > a.log &); ./fbtest localhost:18883 b trust.jks 2>&1 | grep '^\['; sleep 2; cat a.log
