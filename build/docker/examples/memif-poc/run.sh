#!/usr/bin/env bash
# One-click memif PoC: bring up two VPP containers, wire a memif link, ping across.
#
#   ./run.sh        # start + configure + ping
#   ./run.sh down   # tear everything down (containers + shared volume)
#
# Override the image:  VPP_IMAGE=ghcr.io/fivetime/vpp:latest ./run.sh
set -euo pipefail
export MSYS_NO_PATHCONV=1   # git-bash: don't mangle container-internal /run/... paths
cd "$(dirname "$0")"

COMPOSE="docker compose -f compose.yml"

if [ "${1:-up}" = "down" ]; then
  $COMPOSE down -v
  echo "torn down."
  exit 0
fi

echo "==> starting two VPP instances (vpp-a master, vpp-b slave)..."
$COMPOSE up -d

echo "==> waiting for VPP to be ready..."
for c in memif-poc-a memif-poc-b; do
  for _ in $(seq 1 30); do
    docker exec "$c" vppctl show version >/dev/null 2>&1 && break
    sleep 1
  done
done

echo "==> configuring memif (a=192.168.1.1 master, b=192.168.1.2 slave)..."
docker exec memif-poc-a vppctl create memif socket id 1 filename /run/memif/memif.sock
docker exec memif-poc-a vppctl create interface memif id 0 socket-id 1 master
docker exec memif-poc-a vppctl set interface state memif1/0 up
docker exec memif-poc-a vppctl set interface ip address memif1/0 192.168.1.1/24

docker exec memif-poc-b vppctl create memif socket id 1 filename /run/memif/memif.sock
docker exec memif-poc-b vppctl create interface memif id 0 socket-id 1 slave
docker exec memif-poc-b vppctl set interface state memif1/0 up
docker exec memif-poc-b vppctl set interface ip address memif1/0 192.168.1.2/24

sleep 2
echo "==> memif link state (slave side) — expect 'slave connected zero-copy':"
docker exec memif-poc-b vppctl show memif | grep -iE "remote-name|flags" || true

echo "==> ping a -> b over memif (first packet lost to ARP is normal):"
docker exec memif-poc-a vppctl ping 192.168.1.2 repeat 4

echo
echo "PoC up. Inspect with:  docker exec memif-poc-a vppctl show interface"
echo "Tear down with:        ./run.sh down"
