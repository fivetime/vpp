# Bare-metal VPP packages (apt / yum)

This builds **FD.io VPP from this repo's source** into native Linux packages and
publishes them as **GPG-signed apt and yum repositories on GitHub Pages**, so you
can `apt install vpp` / `dnf install vpp` on a bare-metal host (no container).

It is intentionally **separate from the container pipeline** — the
`build-from-*.yml` / `sync-upstream.yml` workflows handle the Docker images and
are untouched. This pipeline is the `release-packages.yml` workflow plus the two
files here.

| Output | Built on | Arches | Layout on Pages |
|--------|----------|--------|-----------------|
| `.deb` | Debian 13 (trixie), via `build/docker/Dockerfile --target packages` | amd64, arm64 | `/apt/{pool,dists}` |
| `.rpm` | Rocky Linux 9 (el9), via `build/packages/Dockerfile.rpm` | x86_64, aarch64 | `/rpm/el9/$basearch` |

> Rocky (not AlmaLinux) is the RPM base because VPP's `make install-dep` only
> has a `rocky` branch in its RHEL-family subtree — AlmaLinux installs no deps.

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
   container build), build a `deb+rpm × arch` matrix, and **skip** if a
   `pkg-v<ver>` GitHub Release already exists (idempotent).
2. **build** — each `(kind, arch)` compiles on a native runner from the release
   tag's source (with `build/docker`, `build/packages`, `.dockerignore` overlaid
   from `master`), exporting packages as artifacts. No QEMU — VPP source builds
   are too heavy to emulate.
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

```bash
curl -fsSL https://<owner>.github.io/<repo>/vpp-archive-keyring.asc \
  | sudo gpg --dearmor -o /usr/share/keyrings/vpp-archive-keyring.gpg

echo "deb [signed-by=/usr/share/keyrings/vpp-archive-keyring.gpg] https://<owner>.github.io/<repo>/apt stable main" \
  | sudo tee /etc/apt/sources.list.d/vpp.list

sudo apt-get update && sudo apt-get install vpp vpp-plugin-core
```

### RHEL / Rocky / AlmaLinux 9

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
# .deb (reuses the container image's builder stage)
docker build -f build/docker/Dockerfile   --target packages \
  --output type=local,dest=./debs --build-arg VERSION=26.02 .

# .rpm
docker build -f build/packages/Dockerfile.rpm --target packages \
  --output type=local,dest=./rpms --build-arg VERSION=26.02 .
```
