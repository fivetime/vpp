# memif PoC — two VPP instances over shared memory

A minimal proof-of-concept showing VPP's **memif** (shared-memory packet
interface) connecting two independent containers — **no Kubernetes, no shared
network namespace, no NIC**, just a shared unix socket. This is the building
block behind the "one VPP per node, pods as memif clients" model (the VPP analog
of ovs-dpdk + ovs-cni).

## What it proves

- Two separate VPP containers exchange packets over a **zero-copy shared-memory**
  link — fully userspace, the kernel is not on the data path.
- The only thing shared is the memif **control socket** (a Docker named volume);
  the shared-memory regions are passed as file descriptors over that socket, so
  no shared IPC namespace or hugepage file is needed.

## Prerequisites

- The VPP image. Either build it (`docker build -f build/docker/Dockerfile -t
  vpp:dev .` from the repo root) or set `VPP_IMAGE=ghcr.io/fivetime/vpp:latest`.
- Hugepages on the host/VM (Docker Desktop's WSL2 VM ships some by default).
  DPDK is disabled here, but VPP still uses hugepages for its buffer pool.

## Run

```bash
cd build/docker/examples/memif-poc
./run.sh            # start, wire the memif link, ping across
./run.sh down       # tear down (containers + shared volume)
```

Expected output ends with a working ping and a link state of
`slave connected zero-copy`:

```
==> memif link state (slave side) — expect 'slave connected zero-copy':
  remote-name "VPP 26.02-release"
      flags admin-up slave connected zero-copy
==> ping a -> b over memif ...
116 bytes from 192.168.1.2: icmp_seq=2 ttl=64 time=...
```

## How it maps to Kubernetes

| This PoC | Production (e.g. Calico/VPP or Multus + userspace-cni) |
| --- | --- |
| `vpp-a` (master) | the per-node VPP dataplane (DaemonSet) |
| `vpp-b` (slave)  | a pod running a memif-aware app (libmemif/gomemif/VPP) |
| shared `memif-sock` volume | the memif socket dir the CNI mounts into the pod |
| the `vppctl create interface memif …` calls | what the CNI automates per pod |

So the userspace datapath can be validated locally before involving K8s — the
CNI layer just automates the socket mount and the memif interface setup.

## Notes

- `main-heap-size` must not be too small — 128M makes plugin init segfault; this
  example uses 512M. `buffers-per-numa` is kept low (4096) so two VPP instances
  fit in the limited Docker Desktop hugepages.
- A pod app only gets userspace networking if it actually speaks memif (links
  `libmemif`/`gomemif`, runs DPDK, or runs VPP). Plain kernel-socket apps would
  instead use the VPP host stack via VCL/`LD_PRELOAD` — a different mechanism.
