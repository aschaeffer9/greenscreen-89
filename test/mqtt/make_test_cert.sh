#!/bin/bash
# self-signed localhost certificate for the local test broker + a Java trust store for the JNI client tests
set -e
cd "$(dirname "$0")"
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 365 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" 2>/dev/null
rm -f trust.jks
keytool -importcert -noprompt -alias localhost -file cert.pem -keystore trust.jks -storepass changeit >/dev/null 2>&1
echo "cert.pem key.pem trust.jks ready"
