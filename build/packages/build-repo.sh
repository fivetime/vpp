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
<title>FD.io VPP — prebuilt apt / yum packages</title>
<style>
 *{box-sizing:border-box}
 body{margin:0;font:15px/1.65 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;color:#1f2328;background:#f6f8fa}
 a{color:#0969da;text-decoration:none} a:hover{text-decoration:underline}
 .wrap{max-width:900px;margin:0 auto;padding:0 1.1rem}
 header{background:linear-gradient(135deg,#0b1f3a 0%,#10325c 55%,#1f6feb 100%);color:#eaf1fb}
 header .wrap{padding:2.6rem 1.1rem 2.2rem}
 header h1{margin:0;font-size:2rem;letter-spacing:-.5px;font-weight:700}
 header h1 span{color:#7ab7ff}
 header p{margin:.55rem 0 0;color:#bcd2f2;max-width:40rem}
 .badges{margin-top:1.1rem;display:flex;gap:.45rem;flex-wrap:wrap}
 .badge{background:rgba(255,255,255,.12);border:1px solid rgba(255,255,255,.2);padding:.18rem .65rem;border-radius:999px;font-size:.78rem;letter-spacing:.2px}
 main{padding:1.6rem 0 .5rem}
 .card{background:#fff;border:1px solid #d0d7de;border-radius:12px;padding:1.3rem 1.45rem;margin:1.15rem 0;box-shadow:0 1px 3px rgba(27,31,36,.06)}
 .card h2{margin:0 0 .25rem;font-size:1.22rem;display:flex;align-items:center;gap:.5rem}
 .hint{color:#57606a;font-size:.92rem;margin:.15rem 0 .85rem}
 .pills{display:flex;gap:.4rem;flex-wrap:wrap;margin:.1rem 0 .95rem}
 .pills code,.vers code{background:#ddf4ff;color:#0550ae;border:1px solid #b6e3ff;padding:.12rem .6rem;border-radius:999px;font:600 .82rem ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
 .codewrap{position:relative}
 pre{background:#0d1117;color:#e6edf3;padding:1rem 1.1rem;border-radius:9px;overflow:auto;margin:0;font:12.5px/1.6 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
 .copy{position:absolute;top:.55rem;right:.55rem;background:#21262d;color:#c9d1d9;border:1px solid #444c56;border-radius:6px;padding:.22rem .6rem;font-size:.75rem;cursor:pointer;transition:.15s}
 .copy:hover{background:#30363d;color:#fff}
 .copy.ok{background:#238636;border-color:#238636;color:#fff}
 code{background:#eff1f3;border-radius:5px;padding:.05rem .35rem;font:.88em ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
 pre code{background:none;padding:0;border:0}
 .vers{display:flex;gap:.45rem;flex-wrap:wrap;list-style:none;padding:0;margin:.3rem 0}
 footer{border-top:1px solid #d0d7de;color:#57606a;font-size:.86rem}
 footer .wrap{padding:1.4rem 1.1rem 2.6rem}
 @media(max-width:560px){header h1{font-size:1.55rem}}
</style></head><body>
<header><div class="wrap">
 <h1>FD.io <span>VPP</span> — prebuilt packages</h1>
 <p>Community builds of <a href="https://fd.io/" style="color:#cfe2ff">Vector Packet Processing</a>
 compiled from source, served as GPG-signed <b>apt</b> &amp; <b>yum</b> repositories. Just add the repo and install.</p>
 <div class="badges"><span class="badge">amd64</span><span class="badge">arm64</span><span class="badge">GPG-signed</span><span class="badge">auto-updated</span></div>
</div></header>
<main class="wrap">

 <section class="card">
  <h2>🐧 Debian / Ubuntu · apt</h2>
  <p class="hint">Pick the suite matching your release —
  <code>${default_suite}</code> = Debian&nbsp;13 (also Ubuntu&nbsp;24.04+),
  <code>bookworm</code> = Debian&nbsp;12, <code>jammy</code> = Ubuntu&nbsp;22.04.</p>
  <div class="pills">${apt_suite_list}</div>
  <div class="codewrap"><button class="copy" onclick="cp(this)">Copy</button><pre>curl -fsSL $REPO_URL/vpp-archive-keyring.asc \\
  | sudo gpg --dearmor -o /usr/share/keyrings/vpp-archive-keyring.gpg

# replace ${default_suite} with your suite from the list above
echo "deb [signed-by=/usr/share/keyrings/vpp-archive-keyring.gpg] $REPO_URL/apt ${default_suite} $COMPONENT" \\
  | sudo tee /etc/apt/sources.list.d/vpp.list

sudo apt-get update
sudo apt-get install vpp vpp-plugin-core</pre></div>
 </section>

 <section class="card">
  <h2>🎩 RHEL / Rocky / AlmaLinux · yum/dnf</h2>
  <p class="hint"><code>el9</code> covers RHEL / Rocky / Alma / CentOS&nbsp;Stream / Oracle&nbsp;9 — and,
  glibc&nbsp;2.34 being forward-compatible, it installs on RHEL&nbsp;10 too.</p>
  <div class="pills">${el_list}</div>
  <div class="codewrap"><button class="copy" onclick="cp(this)">Copy</button><pre>sudo rpm --import $REPO_URL/RPM-GPG-KEY-vpp

sudo tee /etc/yum.repos.d/vpp.repo >/dev/null <<'REPO'
[vpp]
name=VPP packages
baseurl=$REPO_URL/rpm/el9/\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=$REPO_URL/RPM-GPG-KEY-vpp
REPO

sudo dnf install vpp vpp-plugins</pre></div>
 </section>

 <section class="card">
  <h2>📦 Available versions</h2>
  <ul class="vers">$ver_items</ul>
  <p class="hint">Individual <code>.deb</code> / <code>.rpm</code> files (incl. debug packages) are also
  attached to each <a href="https://github.com/${GITHUB_REPOSITORY:-fivetime/vpp}/releases">GitHub&nbsp;Release</a>.</p>
 </section>

</main>
<footer><div class="wrap">
 Signing key <a href="$REPO_URL/vpp-archive-keyring.asc">vpp-archive-keyring.asc</a>
 · id <code>$KEYID</code> &nbsp;·&nbsp; built from source by
 <a href="https://github.com/${GITHUB_REPOSITORY:-fivetime/vpp}">${GITHUB_REPOSITORY:-fivetime/vpp}</a>.
 Community packages — not an official FD.io distribution.
</div></footer>
<script>
function cp(b){var p=b.parentElement.querySelector('pre');navigator.clipboard.writeText(p.innerText).then(function(){b.textContent='Copied';b.classList.add('ok');setTimeout(function(){b.textContent='Copy';b.classList.remove('ok')},1400)})}
</script>
</body></html>
EOF

echo "==> done. Site tree:"
find "$SITE" -maxdepth 3 -type d | sort
