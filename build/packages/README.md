# Bare-metal VPP packages (apt / yum)

This builds **FD.io VPP from this repo's source** into native Linux packages and
publishes them as **GPG-signed apt and yum repositories on GitHub Pages**, so you
can `apt install vpp` / `dnf install vpp` on a bare-metal host (no container).

It is intentionally **separate from the container pipeline** — the
`build-img-from-*.yml` / `sync-upstream.yml` workflows handle the Docker images.
Package builds are handled by `build-pkg-from-main.yml`,
`build-pkg-from-tags.yml`, and `release-pkg-assets.yml`, plus the files here.

Each distro is built on its **own base image** (a package is linked against its
build distro's libraries) and published under its own apt **suite** (by codename)
or yum **el\<N\>** tree; apt/dnf then pick the one matching the user's distro. Both
Dockerfiles take the base as `--build-arg BASE_IMAGE=…`.

| Output | Built on (`BASE_IMAGE`) | Suite / repo | Covers |
|--------|-------------------------|--------------|--------|
| `.deb` | `debian:trixie-slim` | `apt … trixie` | Debian 13, Ubuntu 24.04+ (glibc ≥ 2.38) |
| `.deb` | `debian:bookworm-slim` | `apt … bookworm` | Debian 12 |
| `.deb` | `ubuntu:22.04` | `apt … jammy` | Ubuntu 22.04 |
| `.rpm` | `quay.io/rockylinux/rockylinux:9` | `rpm/el9` | RHEL/Rocky/Alma/CentOS-Stream/Oracle 9 — and RHEL 10 (glibc 2.34 forward-compat) |

Built for **amd64 + arm64** on native runners (no QEMU — VPP source builds are too
heavy to emulate). `.deb` via `build/packages/Dockerfile.deb`, `.rpm` via
`build/packages/Dockerfile.rpm`.

> Rocky (not AlmaLinux) is the RPM base because VPP's `make install-dep` only has a
> `rocky` branch in its RHEL-family subtree — AlmaLinux installs no deps. The rpm
> still installs across the whole el\<N\> family.
>
> **Evaluated and dropped** (only `el9` is shipped on the RPM side; its glibc 2.34 is
> forward-compatible so the el9 rpm also installs on RHEL 10):
> - **el8 / openSUSE-Leap-15** — Python 3.6, too old for VPP 26.02's build tooling
>   (split-interpreter `ply` / `setuptools>=61`).
> - **Fedora / el10** — too NEW. el10 builds through the entire main VPP compile but
>   fails there: VPP 26.02's `tlsopenssl` plugin uses OpenSSL's ENGINE API, which
>   RHEL 10's OpenSSL 3 removed (el9's still ships it, deprecated). That's a
>   source-level port — upstream VPP v26.06+ territory. Fedora's rpm-4.20/dnf5/clang
>   stack is the same class of moving target.
>
> See the `Dockerfile.rpm` header and the per-distro fix comments for the full diagnosis.

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

`build-pkg-from-tags.yml` publishes packages from release tags:

1. **prepare** — resolve the requested or newest stable `vYY.MM[.P]` tag, build a
   `deb×{trixie,bookworm,jammy} + rpm×{el9}` × arch matrix, and skip when all
   per-target package asset Releases already exist unless `force=true`.
2. **build** — each `(kind, suite/el, arch)` compiles on a native runner from the
   release tag's source, with package build infra overlaid from `master`, using
   `Dockerfile.<kind>` + `BASE_IMAGE`. It exports `.deb` / `.rpm` files as
   workflow artifacts. No QEMU — VPP source builds are too heavy to emulate.
3. **publish** — import the key, accumulate the new packages onto the existing
   `gh-pages` package pool, regenerate signed apt (`dpkg-scanpackages` +
   `apt-ftparchive`) and yum (`createrepo_c`) metadata, and push `gh-pages`.
4. **trigger-release-assets** — dispatch `release-pkg-assets.yml` with the build
   run id so raw package files can be attached to GitHub Releases without
   recompiling if the asset publishing logic needs a rerun.

`build-pkg-from-main.yml` follows the same package/publish flow for `master`.
Its package version is `0.<short_sha>`, so main branch packages do not collide
with release-tag packages while still clearly showing the source commit.

`release-pkg-assets.yml` downloads package artifacts from an existing package
build run and uploads raw `.deb` / `.rpm` files to per-target Releases:

- `pkg-v<ver>-bookworm`
- `pkg-v<ver>-jammy`
- `pkg-v<ver>-trixie`
- `pkg-v<ver>-el9`
- `pkg-0.<short_sha>-<target>` for main builds

Triggers:

- `build-pkg-from-tags.yml`: automatically after **Sync from Upstream** succeeds,
  weekly on a schedule, or manually via **Run workflow** with optional `version`,
  `platforms`, and `force`.
- `build-pkg-from-main.yml`: on pushes to `master`, or manually with `platforms`
  and `force`.
- `release-pkg-assets.yml`: manually, or automatically from a successful package
  build workflow.

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

`el9` covers RHEL/Rocky/Alma/CentOS-Stream/Oracle 9, and (glibc 2.34 forward-compat)
installs on RHEL 10 too.

```bash
sudo rpm --import https://<owner>.github.io/<repo>/RPM-GPG-KEY-vpp
sudo tee /etc/yum.repos.d/vpp.repo >/dev/null <<'EOF'
[vpp]
name=VPP packages
baseurl=https://<owner>.github.io/<repo>/rpm/el9/$basearch
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
