Vector Packet Processing
========================

## Introduction

The VPP platform is an extensible framework that provides out-of-the-box
production quality switch/router functionality. It is the open source version
of Cisco's Vector Packet Processing (VPP) technology: a high performance,
packet-processing stack that can run on commodity CPUs.

The benefits of this implementation of VPP are its high performance, proven
technology, its modularity and flexibility, and rich feature set.

For more information on VPP and its features please visit the
[FD.io website](http://fd.io/) and
[What is VPP?](https://wiki.fd.io/view/VPP/What_is_VPP%3F) pages.


## Prebuilt packages & container image (this fork)

This fork publishes **community builds of VPP from source** — no need to compile
it yourself. Built for `amd64` and `arm64`.

### Bare metal — signed apt / yum repositories

Hosted on GitHub Pages at **<https://fivetime.github.io/vpp/>** (GPG-signed).

**Debian / Ubuntu** — pick the `SUITE` matching your release: `trixie` (Debian 13,
also Ubuntu 24.04+), `bookworm` (Debian 12), `jammy` (Ubuntu 22.04).

```bash
SUITE=trixie   # or bookworm / jammy
curl -fsSL https://fivetime.github.io/vpp/vpp-archive-keyring.asc \
  | sudo gpg --dearmor -o /usr/share/keyrings/vpp-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/vpp-archive-keyring.gpg] https://fivetime.github.io/vpp/apt $SUITE main" \
  | sudo tee /etc/apt/sources.list.d/vpp.list
sudo apt-get update && sudo apt-get install vpp vpp-plugin-core
```

**RHEL / Rocky / AlmaLinux** — `el9` (RHEL/Rocky/Alma 9) or `el10` (RHEL/Rocky 10).

```bash
EL=el9   # or el10
sudo rpm --import https://fivetime.github.io/vpp/RPM-GPG-KEY-vpp
sudo tee /etc/yum.repos.d/vpp.repo >/dev/null <<EOF
[vpp]
name=VPP packages
baseurl=https://fivetime.github.io/vpp/rpm/$EL/\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=https://fivetime.github.io/vpp/RPM-GPG-KEY-vpp
EOF
sudo dnf install vpp vpp-plugins
```

Individual `.deb`/`.rpm` files (including debug packages) are also attached to each
[GitHub Release](https://github.com/fivetime/vpp/releases). See
[`build/packages/README.md`](build/packages/README.md) for details.

### Container image

```bash
docker pull ghcr.io/fivetime/vpp:latest    # newest release (also :26.02)
docker pull ghcr.io/fivetime/vpp:master    # latest master build
```

See [`build/docker/README.md`](build/docker/README.md) for running, configuration,
and a memif example.


## Changes

Details of the changes leading up to this version of VPP can be found under
doc/releasenotes.


## Directory layout

| Directory name         | Description                                 |
| ---------------------- | ------------------------------------------- |
| build-data             | Build metadata                              |
| build-root             | Build output directory                      |
| docs                   | Sphinx Documentation                        |
| dpdk                   | DPDK patches and build infrastructure       |
| extras/libmemif        | Client library for memif                    |
| src/examples           | VPP example code                            |
| src/plugins            | VPP bundled plugins directory               |
| src/svm                | Shared virtual memory allocation library    |
| src/tests              | Standalone tests (not part of test harness) |
| src/vat                | VPP API test program                        |
| src/vlib               | VPP application library                     |
| src/vlibapi            | VPP API library                             |
| src/vlibmemory         | VPP Memory management                       |
| src/vnet               | VPP networking                              |
| src/vpp                | VPP application                             |
| src/vpp-api            | VPP application API bindings                |
| src/vppinfra           | VPP core library                            |
| src/vpp/api            | Not-yet-relocated API bindings              |
| test                   | Unit tests and Python test harness          |

## Getting started

In general anyone interested in building, developing or running VPP should
consult the [VPP wiki](https://wiki.fd.io/view/VPP) for more complete
documentation.

In particular, readers are recommended to take a look at [Pulling, Building,
Running, Hacking, Pushing](https://wiki.fd.io/view/VPP/Pulling,_Building,_Run
ning,_Hacking_and_Pushing_VPP_Code) which provides extensive step-by-step
coverage of the topic.

For the impatient, some salient information is distilled below.


### Quick-start: On an existing Linux host

To install system dependencies, build VPP and then install it, simply run the
build script. This should be performed a non-privileged user with `sudo`
access from the project base directory:

    ./extras/vagrant/build.sh

If you want a more fine-grained approach because you intend to do some
development work, the `Makefile` in the root directory of the source tree
provides several convenience shortcuts as `make` targets that may be of
interest. To see the available targets run:

    make


### Quick-start: Vagrant

The directory `extras/vagrant` contains a `VagrantFile` and supporting
scripts to bootstrap a working VPP inside a Vagrant-managed Virtual Machine.
This VM can then be used to test concepts with VPP or as a development
platform to extend VPP. Some obvious caveats apply when using a VM for VPP
since its performance will never match that of bare metal; if your work is
timing or performance sensitive, consider using bare metal in addition or
instead of the VM.

For this to work you will need a working installation of Vagrant. Instructions
for this can be found [on the Setting up Vagrant wiki page]
(https://wiki.fd.io/view/DEV/Setting_Up_Vagrant).

### Quick-start: FreeBSD

VPP is packaged in the FreeBSD ports system. Binary packages are available and
can be installed with the following command:

    # pkg install vpp


## More information

Several modules provide documentation, see @subpage user_doc for more
end-user-oriented information. Also see @subpage dev_doc for developer notes.

Visit the [VPP wiki](https://wiki.fd.io/view/VPP) for details on more
advanced building strategies and other development notes.

