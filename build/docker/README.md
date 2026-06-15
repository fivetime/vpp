# VPP container image

This image builds **FD.io VPP from this repository's source** on Debian and
ships a slim runtime image containing only the VPP runtime packages.

Why Debian, not Alpine: VPP depends on glibc, DPDK and hugepages and does not
build against musl, so both stages use `debian:trixie-slim` (Debian 13, which
VPP's `Makefile` explicitly supports). The `.deb` packages are linked against
the build environment's libraries, so **the builder and runtime stages must use
the same distro/version** — don't mix (e.g. Debian builder + Ubuntu runtime).

The build is a two-stage Dockerfile:

1. **builder** — runs the canonical VPP build (`make install-dep`,
   `make install-ext-deps`, `make pkg-deb`) and produces `.deb` packages under
   `build-root/`.
2. **runtime** — installs only the runtime packages (`vpp`, `libvppinfra`,
   `vpp-plugin-core`, `vpp-plugin-dpdk`, `vpp-drivers`, `vpp-crypto-engines`)
   and runs `vpp -c /etc/vpp/startup.conf`.

## Building

The `Dockerfile` uses `COPY .`, so the **build context must be the repo root**:

```bash
# from the top of the vpp repo
docker build -f build/docker/Dockerfile -t vpp:dev .

# debug build instead of release
docker build -f build/docker/Dockerfile \
  --build-arg BUILD_TYPE=debug \
  -t vpp:dev-debug .
```

> **Tip:** add a `.dockerignore` at the repo root that ignores `build-root/`
> (and other large/generated dirs) so the build context stays small.

## Running

VPP needs elevated privileges and hugepages. The simplest way is Compose:

```bash
docker compose -f build/docker/compose.yml up -d --build
```

Or directly with `docker run`:

```bash
docker run -d --name vpp \
  --privileged \
  -v /dev/hugepages:/dev/hugepages \
  -v "$PWD/startup.conf:/etc/vpp/startup.conf:ro" \
  --ulimit memlock=-1 \
  --shm-size=1g \
  vpp:dev
```

The image ships VPP's default `/etc/vpp/startup.conf`; mount your own to
override it. Make sure the host has hugepages reserved, e.g.:

```bash
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

### Talking to the daemon

VPP is controlled with `vppctl` over the CLI socket
(`/run/vpp/cli.sock` inside the container):

```bash
docker exec -it vpp vppctl show version
docker exec -it vpp vppctl show interface
docker exec -it vpp vppctl show runtime
```

## Notes

- This is a development/convenience image built from local source. For
  released binaries, FD.io also publishes packages via packagecloud
  (`https://packagecloud.io/fdio`) — see `extras/docker/run/` for image
  examples that install those instead of building from source.
- The runtime stage deliberately omits `vpp-dev` / `vpp-dbg` (development and
  debug-symbol packages); they are not needed to run VPP.
