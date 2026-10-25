#!/bin/bash
# Optional live protocol check. All server data disappears with this container.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
image=${KOSYNC_TEST_IMAGE:-koreader/kosync@sha256:bb3f13615365703315a43b9059f65e71e876440f867e23a42bf27f2fa18264e1}
if [[ $# -gt 1 || ($# -eq 1 && $1 != --pull) ]]; then
    echo "Usage: bash tests/support/run-kosync-server.sh [--pull]" >&2
    exit 2
fi
if [[ ${1:-} == --pull ]]; then docker pull "$image"; fi
docker image inspect "$image" >/dev/null
mkdir -p "$root/build/desktop-mac/test-tmp"
scratch=$(mktemp -d "$root/build/kosync-server.XXXXXX")
container=
cleanup() {
    if [[ -n "$container" ]]; then
        docker stop --timeout 3 "$container" >/dev/null 2>&1 || true
        for attempt in {1..30}; do
            if ! docker inspect "$container" >/dev/null 2>&1; then break; fi
            sleep 0.1
        done
        if docker inspect "$container" >/dev/null 2>&1; then
            echo "Local KOsync container was not removed: $container" >&2
            return 1
        fi
    fi
    rm -rf "$scratch"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj '/CN=Kobalt local test CA' \
    -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign' \
    -keyout "$scratch/ca.key" -out "$scratch/ca.crt" >"$scratch/certificate.log" 2>&1
openssl req -new -newkey rsa:2048 -nodes -subj /CN=localhost \
    -keyout "$scratch/server.key" -out "$scratch/server.csr" >>"$scratch/certificate.log" 2>&1
cat >"$scratch/server.extensions" <<'CERTIFICATE'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
CERTIFICATE
openssl x509 -req -in "$scratch/server.csr" -CA "$scratch/ca.crt" -CAkey "$scratch/ca.key" \
    -CAcreateserial -days 1 -extfile "$scratch/server.extensions" -out "$scratch/server.crt" \
    >>"$scratch/certificate.log" 2>&1
container=$(docker run --rm --detach --name "kobalt-${scratch##*/}" \
    --publish 127.0.0.1::7200 \
    --tmpfs /var/lib/redis --tmpfs /var/log/redis \
    --mount "type=bind,source=$scratch/server.crt,target=/etc/nginx/ssl/nginx.crt,readonly" \
    --mount "type=bind,source=$scratch/server.key,target=/etc/nginx/ssl/nginx.key,readonly" \
    "$image")
port=$(docker port "$container" 7200/tcp | sed -n 's/^127\.0\.0\.1://p')
[[ -n "$port" ]] || { echo 'Local KOsync port was not assigned' >&2; exit 1; }
ready=false
for attempt in {1..40}; do
    if curl --silent --fail --max-time 2 --cacert "$scratch/ca.crt" \
        -H 'Accept: application/vnd.koreader.v1+json' "https://localhost:$port/healthcheck" >"$scratch/health.json"; then
        ready=true
        break
    fi
    sleep 0.5
done
[[ "$ready" == true ]] || { docker logs "$container"; echo 'Local KOsync did not become healthy' >&2; exit 1; }
cmake "-DTEST_EXECUTABLE=$root/build/desktop-mac/kobalt-kosync-tests" \
    "-DTEST_ARGUMENTS=--server;https://localhost:$port;$scratch/ca.crt" \
    "-DTEST_TMP_ROOT=$root/build/desktop-mac/test-tmp" -DTEST_TIMEOUT=60 \
    -P "$root/platforms/desktop/run-test.cmake"
username="reader-fixture-$$"
printf '{"username":"%s","password":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}' "$username" >"$scratch/reader-account.json"
curl --silent --show-error --fail --max-time 10 --cacert "$scratch/ca.crt" \
    -H 'Accept: application/vnd.koreader.v1+json' -H 'Content-Type: application/json' \
    --data-binary "@$scratch/reader-account.json" "https://localhost:$port/users/create" >"$scratch/registered.json"
KOBALT_KOSYNC_TEST_SERVER="https://localhost:$port" KOBALT_KOSYNC_TEST_CA="$scratch/ca.crt" \
    KOBALT_KOSYNC_TEST_USER="$username" \
    cmake "-DTEST_EXECUTABLE=$root/build/desktop-mac/kobalt-reader-lifecycle-tests" \
    "-DTEST_ARGUMENTS=kosyncFinal:https-close;kosyncFinal:https-sleep;kosyncFinal:https-filename-close;kosyncFinal:https-filename-sleep;kosyncFinal:https-switch-close;kosyncFinal:https-choice-keep;kosyncFinal:https-choice-resume;kosyncFinal:https-choice-dismiss;kosyncFinal:https-choice-invalid" \
    "-DTEST_TMP_ROOT=$root/build/desktop-mac/test-tmp" -DTEST_TIMEOUT=120 \
    -P "$root/platforms/desktop/run-test.cmake"
echo 'Local KOsync HTTPS integration passed.'
