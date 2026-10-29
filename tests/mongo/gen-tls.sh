#!/bin/bash
# Throwaway CA and server certificate for the mongo-tls service, generated inside a container
# (openssl of the mongo image itself: nothing is installed on the host).
#
#   tests/mongo/gen-tls.sh            creates tests/mongo/tls/{ca.pem,server.pem} if missing
#   tests/mongo/gen-tls.sh --force    regenerates them
#
# Output (git-ignored): ca.pem (CA certificate), server.pem (server certificate + key, as mongod's
# --tlsCertificateKeyFile wants). SAN: localhost, 127.0.0.1, mongo-tls. Valid for 10 years.
# Files are 0644: a toy key, readable by whatever user mongod runs as in the container.
set -euo pipefail

image=mongo:8.0.34@sha256:4a4ab5d4834207389e0d7270fac988e21ffedccecba77991a552f755df229907
here=$(cd "$(dirname "$0")" && pwd)
out=$here/tls

if [ "${1:-}" != "--force" ] && [ -s "$out/ca.pem" ] && [ -s "$out/server.pem" ]; then
    echo "gen-tls: $out already has ca.pem and server.pem (use --force to regenerate)"
    exit 0
fi

mkdir -p "$out"
rm -f "$out/ca.pem" "$out/server.pem"

docker run --rm --network none --user "$(id -u):$(id -g)" -v "$out:/out" --entrypoint bash "$image" -c '
set -euo pipefail
work=$(mktemp -d)
cd "$work"
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
    -keyout ca.key -out ca.pem -subj "/CN=askmdb test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
openssl req -new -newkey rsa:2048 -nodes -sha256 \
    -keyout server.key -out server.csr -subj "/CN=localhost" 2>/dev/null
printf "%s\n" "basicConstraints=CA:FALSE" "keyUsage=critical,digitalSignature,keyEncipherment" \
    "extendedKeyUsage=serverAuth" "subjectAltName=DNS:localhost,IP:127.0.0.1,DNS:mongo-tls" > server.ext
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -sha256 -days 3650 \
    -extfile server.ext -out server.crt 2>/dev/null
openssl verify -CAfile ca.pem server.crt >/dev/null
cat server.crt server.key > /out/server.pem
cp ca.pem /out/ca.pem
chmod 0644 /out/ca.pem /out/server.pem
rm -rf "$work"
'

echo "gen-tls: wrote $out/ca.pem and $out/server.pem"
