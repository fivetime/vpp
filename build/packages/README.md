# Bare-metal VPP packages (apt / yum)

This builds **FD.io VPP from this repo's source** into native Linux packages and
publishes them as **GPG-signed apt and yum repositories on GitHub Pages**, so you
can `apt install vpp` / `dnf install vpp` on a bare-metal host (no container).

It is intentionally **separate from the container pipeline** — the
`build-from-*.yml` / `sync-upstream.yml` workflows handle the Docker images and
are untouched. This pipeline is the `release-packages.yml` workflow plus the two
files here.

Each distro is built on its **own base image** (a package is linked against its
build distro's libraries) and published under its own apt **suite** (by codename)
or yum **el\<N\>** tree; apt/dnf then pick the one matching the user's distro. Both
Dockerfiles take the base as `--build-arg BASE_IMAGE=…`.

| Output | Built on (`BASE_IMAGE`) | Suite / repo | Covers |
|--------|-------------------------|--------------|--------|
| `.deb` | `debian:trixie-slim` | `apt … trixie` | Debian 13, Ubuntu 24.04+ (glibc ≥ 2.38) |
| `.deb` | `debian:bookworm-slim` | `apt … bookworm` | Debian 12 |
| `.deb` | `ubuntu:22.04` | `apt … jammy` | Ubuntu 22.04 |
| `.rpm` | `quay.io/rockylinux/rockylinux:9` | `rpm/el9` | RHEL/Rocky/Alma/CentOS-Stream/Oracle 9 |
| `.rpm` | `quay.io/rockylinux/rockylinux:10` | `rpm/el10` | RHEL/Rocky 10 |

Built for **amd64 + arm64** on native runners (no QEMU — VPP source builds are too
heavy to emulate). `.deb` via `build/packages/Dockerfile.deb`, `.rpm` via
`build/packages/Dockerfile.rpm`.

> Rocky (not AlmaLinux) is the RPM base because VPP's `make install-dep` only has a
> `rocky` branch in its RHEL-family subtree — AlmaLinux installs no deps. The rpm
> still installs across the whole el\<N\> family.
>
> **Evaluated and dropped:** el8 and openSUSE-Leap-15 ship Python 3.6, too old for
> VPP 26.02's build tooling (split-interpreter `ply` / `setuptools>=61`); Fedora's
> bleeding-edge rpm-4.20/dnf5/clang stack needs a porting-level effort that re-breaks
> each release. el9 + el10 cover current and next RHEL. See `Dockerfile.rpm` header
> and the per-distro fix comments for the full diagnosis.

## One-time setup (required before the first run)

The publish job **fails fast** without a signing key. Two steps:

### 1. Create a signing key and add it as a secret

```bash
# generate (no passphrase shown; add one if you prefer and store it too)
gpg --batch --gen-key <<EOF
%no-protection
Key-Type: eddsa
Key-Curve: ed25519
Subkey-Type: ecdh
Subkey-Curve: cv25519
Name-Real: VPP packages (community build)
Name-Email: you@example.com
Expire-Date: 0
%commit
EOF

# export the ASCII-armored PRIVATE key and copy it into the secret
gpg --armor --export-secret-keys you@example.com | clip   # Windows; or | pbcopy / xclip
```

Add repository secrets (Settings → Secrets and variables → Actions):
- **`GPG_PRIVATE_KEY`** — the ASCII-armored private key (the `-----BEGIN PGP PRIVATE KEY BLOCK-----` block).
- **`GPG_PASSPHRASE`** — *only if* your key has a passphrase (omit for `%no-protection`).

The matching **public** key is exported and published automatically at
`…/vpp-archive-keyring.asc` and `…/RPM-GPG-KEY-vpp` on each run — users import that.

### 2. Enable GitHub Pages

Settings → Pages → **Source: Deploy from a branch → `gh-pages` / `(root)`**.
The branch is created on the first successful publish; set this once the branch
exists (or set it now — Pages will start serving when the branch appears).

Site URL: `https://<owner>.github.io/<repo>/`.

## How it runs

`release-packages.yml`:

1. **prepare** — resolve the newest stable `vYY.MM[.P]` tag (same regex as the
   container build), build a `deb×{trixie,bookworm,jammy} + rpm×{el9,el10}` × arch
   matrix, and **skip** if a `pkg-v<ver>` GitHub Release already exists (idempotent).
2. **build** — each `(kind, suite/el, arch)` compiles on a native runner from the
   release tag's source (with `build/docker`, `build/packages`, `.dockerignore`
   overlaid from `master`) using `Dockerfile.<kind>` + `BASE_IMAGE`, exporting
   packages as artifacts. No QEMU — VPP source builds are too heavy to emulate.
3. **publish** — import the key, **accumulate** the new packages onto whatever is
   already on `gh-pages`, regenerate signed apt (`dpkg-scanpackages` +
   `apt-ftparchive`) and yum (`createrepo_c`) metadata, push `gh-pages`, and
   attach the raw `.deb`/`.rpm` to a `pkg-v<ver>` GitHub Release.

Triggers: automatically after **Sync from Upstream** succeeds, weekly on a
schedule (skips if unchanged), or manually via **Run workflow** (with optional
`version`, `platforms`, and `force` rebuild).

> The repo is rebuilt from the **package pool** each run (no stateful DB), so old
> versions on `gh-pages` are preserved and the metadata is always reproducible.

## Consuming the repo

### Debian / Ubuntu

`SUITE` = `trixie` (Debian 13 / Ubuntu 24.04+), `bookworm` (Debian 12), or `jammy` (Ubuntu 22.04).

```bash
SUITE=trixie
curl -fsSL https://<owner>.github.io/<repo>/vpp-archive-keyring.asc \
  | sudo gpg --dearmor -o /usr/share/keyrings/vpp-archive-keyring.gpg

echo "deb [signed-by=/usr/share/keyrings/vpp-archive-keyring.gpg] https://<owner>.github.io/<repo>/apt $SUITE main" \
  | sudo tee /etc/apt/sources.list.d/vpp.list

sudo apt-get update && sudo apt-get install vpp vpp-plugin-core
```

### RHEL / Rocky / AlmaLinux

`EL` = `el9` (RHEL/Rocky/Alma 9) or `el10` (RHEL/Rocky 10).

```bash
EL=el9
sudo rpm --import https://<owner>.github.io/<repo>/RPM-GPG-KEY-vpp
sudo tee /etc/yum.repos.d/vpp.repo >/dev/null <<EOF
[vpp]
name=VPP packages
baseurl=https://<owner>.github.io/<repo>/rpm/$EL/\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=https://<owner>.github.io/<repo>/RPM-GPG-KEY-vpp
EOF
sudo dnf install vpp vpp-plugins
```

The published `index.html` (the site root) shows these snippets pre-filled with
your actual URL and the list of available versions.

## Building packages locally

```bash
# .deb — pass the target distro as BASE_IMAGE (debian:trixie-slim / debian:bookworm-slim / ubuntu:22.04)
docker build -f build/packages/Dockerfile.deb --target packages \
  --build-arg BASE_IMAGE=debian:trixie-slim --build-arg VERSION=26.02 \
  --output type=local,dest=./debs .

# .rpm — BASE_IMAGE = quay.io/rockylinux/rockylinux:9 or :10
docker build -f build/packages/Dockerfile.rpm --target packages \
  --build-arg BASE_IMAGE=quay.io/rockylinux/rockylinux:9 --build-arg VERSION=26.02 \
  --output type=local,dest=./rpms .
```
