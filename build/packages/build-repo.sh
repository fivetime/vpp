#!/usr/bin/env bash
###############################################################################
# build-repo.sh — assemble GPG-signed apt + yum repositories under $SITE_DIR
# from the packages in $INCOMING_DIR, accumulating on top of whatever is already
# published (the caller checks out the existing gh-pages tree into $SITE_DIR).
#
# Multi-suite / multi-distro: a .deb/.rpm is linked against its build distro's
# libraries, so each distro is published under its own apt SUITE (by codename) or
# yum el<N> tree, and apt/dnf then pick the one matching the user's distro.
#
#   apt: per-suite pool + dists/<suite> via dpkg-scanpackages + apt-ftparchive
#        (signed InRelease/Release.gpg)
#   yum: per-el<N>/<arch> via createrepo_c + detached-signed repomd.xml
#
# Everything is rebuilt from the package pool — no stateful DB — so re-running over
# the accumulated pool always yields a correct, signed repo.
#
# Required env:
#   SITE_DIR      gh-pages working tree (output root, served by GitHub Pages)
#   INCOMING_DIR  has deb/<suite>/*.deb and rpm/<elver>/<rpm_arch>/*.rpm
#   GPG_KEY_ID    fingerprint/key id of the imported signing key
#   REPO_URL      public base URL, e.g. https://owner.github.io/repo
# Optional env:
#   GPG_PASSPHRASE,
#   SUITES(="trixie bookworm jammy"), EL_VERS(="el9 el10"), COMPONENT(=main),
#   ARCHES_DEB(="amd64 arm64"), ARCHES_RPM(="x86_64 aarch64"), ORIGIN, LABEL
###############################################################################
set -euo pipefail

SITE="${SITE_DIR:?set SITE_DIR}"
INCOMING="${INCOMING_DIR:?set INCOMING_DIR}"
KEYID="${GPG_KEY_ID:?set GPG_KEY_ID}"
REPO_URL="${REPO_URL:?set REPO_URL, e.g. https://owner.github.io/repo}"
REPO_URL="${REPO_URL%/}"

SUITES="${SUITES:-trixie bookworm jammy}"
EL_VERS="${EL_VERS:-el9}"
COMPONENT="${COMPONENT:-main}"
ARCHES_DEB="${ARCHES_DEB:-amd64 arm64}"
ARCHES_RPM="${ARCHES_RPM:-x86_64 aarch64}"
ORIGIN="${ORIGIN:-VPP community packages}"
LABEL="${LABEL:-VPP}"
GPG_PASSPHRASE="${GPG_PASSPHRASE:-}"

# Non-interactive signing helper (loopback only when a passphrase is provided).
gpg_pass=()
[ -n "$GPG_PASSPHRASE" ] && gpg_pass=(--pinentry-mode loopback --passphrase "$GPG_PASSPHRASE")
gpg_sign() { gpg --batch --yes "${gpg_pass[@]}" -u "$KEYID" "$@"; }

# Copy a package into the hosted pool, but NOT debug-symbol packages (vpp-dbg is
# ~112MB) and nothing over GitHub's 100MB per-file git limit — a single oversized
# file makes the whole gh-pages push fail (pre-receive hook). Debug packages stay
# available via the GitHub Release (2GB limit), just not in the apt/yum repo.
MAX_MB="${MAX_PKG_MB:-99}"
copy_pkg() {  # copy_pkg <dest_dir> <file>
  local dest="$1" f="$2" base sz
  base="$(basename "$f")"
  case "$base" in
    *-dbg_*.deb|*-dbgsym_*.deb|*-debuginfo-*.rpm|*-debugsource-*.rpm)
      echo "   skip debug pkg (kept off the repo): $base"; return 0 ;;
  esac
  sz=$(( $(stat -c%s "$f") / 1048576 ))
  if [ "$sz" -gt "$MAX_MB" ]; then
    echo "::warning::skip $base (${sz}MB > ${MAX_MB}MB GitHub per-file limit) — NOT in repo"
    return 0
  fi
  cp -f "$f" "$dest"/
}

###############################################################################
# APT:  <site>/apt/{pool/<suite>/main, dists/<suite>/main/binary-<arch>}
# One independent pool + dists per suite (codename), since the same package
# name/version is a DIFFERENT binary per distro.
###############################################################################
apt_root="$SITE/apt"
for suite in $SUITES; do
  pool="$apt_root/pool/$suite/$COMPONENT"
  mkdir -p "$pool"
  if compgen -G "$INCOMING/deb/$suite/*.deb" >/dev/null; then
    echo "==> apt[$suite]: adding $(ls "$INCOMING/deb/$suite"/*.deb | wc -l) incoming .deb"
    for f in "$INCOMING/deb/$suite"/*.deb; do copy_pkg "$pool" "$f"; done
  fi
  compgen -G "$pool/*.deb" >/dev/null || { echo "   (apt[$suite]: empty, skipping)"; continue; }

  ( cd "$apt_root"
    for arch in $ARCHES_DEB; do
      bindir="dists/$suite/$COMPONENT/binary-$arch"
      mkdir -p "$bindir"
      # -a <arch> keeps this arch + Architecture:all; paths stay relative to apt_root.
      dpkg-scanpackages -a "$arch" "pool/$suite/$COMPONENT" > "$bindir/Packages" 2>/dev/null
      gzip -9kf "$bindir/Packages"
      cat > "$bindir/Release" <<EOF
Archive: $suite
Suite: $suite
Component: $COMPONENT
Origin: $ORIGIN
Label: $LABEL
Architecture: $arch
EOF
    done

    relconf="$(mktemp)"
    cat > "$relconf" <<EOF
APT::FTPArchive::Release::Origin "$ORIGIN";
APT::FTPArchive::Release::Label "$LABEL";
APT::FTPArchive::Release::Suite "$suite";
APT::FTPArchive::Release::Codename "$suite";
APT::FTPArchive::Release::Architectures "$ARCHES_DEB";
APT::FTPArchive::Release::Components "$COMPONENT";
APT::FTPArchive::Release::Description "$ORIGIN ($suite)";
EOF
    apt-ftparchive -c="$relconf" release "dists/$suite" > "dists/$suite/Release"
    rm -f "$relconf"

    # InRelease (inline sig) + Release.gpg (detached) — apt accepts either.
    rm -f "dists/$suite/InRelease" "dists/$suite/Release.gpg"
    gpg_sign --clearsign  -o "dists/$suite/InRelease"   "dists/$suite/Release"
    gpg_sign --detach-sign -a -o "dists/$suite/Release.gpg" "dists/$suite/Release"
  )
  echo "   apt[$suite]: signed"
done

###############################################################################
# YUM:  <site>/rpm/<elver>/<basearch>/{*.rpm, repodata/}
###############################################################################
# rpm --addsign drives gpg through these macros; loopback passes the passphrase.
{
  echo "%_signature gpg"
  echo "%_gpg_name $KEYID"
  if [ -n "$GPG_PASSPHRASE" ]; then
    echo "%__gpg_sign_cmd %{__gpg} gpg --batch --no-verbose --no-armor --pinentry-mode loopback --passphrase $GPG_PASSPHRASE --digest-algo sha256 -u \"%{_gpg_name}\" -sbo %{__signature_filename} %{__plaintext_filename}"
  else
    echo "%__gpg_sign_cmd %{__gpg} gpg --batch --no-verbose --no-armor --digest-algo sha256 -u \"%{_gpg_name}\" -sbo %{__signature_filename} %{__plaintext_filename}"
  fi
} > "$HOME/.rpmmacros"

for elver in $EL_VERS; do
  for arch in $ARCHES_RPM; do
    d="$SITE/rpm/$elver/$arch"
    mkdir -p "$d"
    if compgen -G "$INCOMING/rpm/$elver/$arch/*.rpm" >/dev/null; then
      echo "==> yum[$elver/$arch]: adding $(ls "$INCOMING/rpm/$elver/$arch"/*.rpm | wc -l) incoming .rpm"
      for f in "$INCOMING/rpm/$elver/$arch"/*.rpm; do copy_pkg "$d" "$f"; done
    fi
    if compgen -G "$d/*.rpm" >/dev/null; then
      rpm --addsign "$d"/*.rpm            # re-signing is idempotent
      createrepo_c --update "$d"
      rm -f "$d/repodata/repomd.xml.asc"
      gpg_sign --detach-sign -a "$d/repodata/repomd.xml"   # repo_gpgcheck=1 gate
      echo "   yum[$elver/$arch]: signed"
    else
      echo "   (yum[$elver/$arch]: empty, skipping)"
    fi
  done
done

###############################################################################
# Public key, landing page, Pages housekeeping
###############################################################################
echo "==> publishing public key + index"
gpg --batch --export --armor "$KEYID" > "$SITE/vpp-archive-keyring.asc"
cp "$SITE/vpp-archive-keyring.asc" "$SITE/RPM-GPG-KEY-vpp"   # conventional rpm name
touch "$SITE/.nojekyll"                                       # serve _/dotted paths verbatim

# Distinct VPP versions currently in any apt pool (for display only).
versions="$(ls "$apt_root"/pool/*/"$COMPONENT"/vpp_*.deb 2>/dev/null | sed -E 's#.*/vpp_([^_]+)_.*#\1#' | sort -uV || true)"
ver_items=""
for v in $versions; do ver_items="${ver_items}<li><code>${v}</code></li>"; done
[ -z "$ver_items" ] && ver_items="<li>(none yet)</li>"

# Per-suite apt snippet (first suite shown inline; the rest listed as alternatives).
apt_suite_list=""
for s in $SUITES; do apt_suite_list="${apt_suite_list}<code>${s}</code> "; done
default_suite="$(echo $SUITES | awk '{print $1}')"
el_list=""
for e in $EL_VERS; do el_list="${el_list}<code>${e}</code> "; done

cat > "$SITE/index.html" <<EOF
<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>VPP packages (apt / yum)</title>
<style>
 body{font:15px/1.5 system-ui,sans-serif;max-width:860px;margin:2.5rem auto;padding:0 1rem;color:#1a1a1a}
 h1{margin-bottom:.2rem} h2{margin-top:2rem;border-bottom:1px solid #eee;padding-bottom:.3rem}
 code,pre{font-family:ui-monospace,Menlo,Consolas,monospace}
 pre{background:#0d1117;color:#e6edf3;padding:1rem;border-radius:8px;overflow:auto}
 .muted{color:#666}
</style></head><body>
<h1>VPP packages</h1>
<p class="muted">Community builds of <a href="https://fd.io/">FD.io VPP</a> from source —
signed apt &amp; yum repositories. Built for <code>amd64</code> and <code>arm64</code>.</p>

<h2>Debian / Ubuntu (apt)</h2>
<p>Pick the <b>suite</b> matching your release — available: ${apt_suite_list}<br>
<span class="muted">${default_suite}=Debian&nbsp;13, bookworm=Debian&nbsp;12, jammy=Ubuntu&nbsp;22.04.
Ubuntu&nbsp;24.04+ works with <code>${default_suite}</code>.</span></p>
<pre>curl -fsSL $REPO_URL/vpp-archive-keyring.asc \\
  | sudo gpg --dearmor -o /usr/share/keyrings/vpp-archive-keyring.gpg

# replace ${default_suite} with your suite from the list above
echo "deb [signed-by=/usr/share/keyrings/vpp-archive-keyring.gpg] $REPO_URL/apt ${default_suite} $COMPONENT" \\
  | sudo tee /etc/apt/sources.list.d/vpp.list

sudo apt-get update
sudo apt-get install vpp vpp-plugin-core</pre>

<h2>RHEL / Rocky / AlmaLinux (yum/dnf)</h2>
<p>Available: ${el_list}<span class="muted">(el9 = RHEL/Rocky/Alma/CentOS-Stream/Oracle 9;
its glibc 2.34 is forward-compatible, so it also installs on RHEL 10).</span></p>
<pre>sudo rpm --import $REPO_URL/RPM-GPG-KEY-vpp

# set EL to your major version: el9 or el10
EL=el9
sudo tee /etc/yum.repos.d/vpp.repo >/dev/null <<REPO
[vpp]
name=VPP packages
baseurl=$REPO_URL/rpm/\$EL/\\\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=$REPO_URL/RPM-GPG-KEY-vpp
REPO

sudo dnf install vpp vpp-plugins</pre>

<h2>Available versions</h2>
<ul>$ver_items</ul>

<p class="muted">Signing key: <a href="$REPO_URL/vpp-archive-keyring.asc">vpp-archive-keyring.asc</a>
&middot; key id <code>$KEYID</code>. Per-version package files (incl. debug) are also attached to the
<a href="https://github.com/${GITHUB_REPOSITORY:-fivetime/vpp}/releases">GitHub Releases</a>.</p>
</body></html>
EOF

echo "==> done. Site tree:"
find "$SITE" -maxdepth 3 -type d | sort
