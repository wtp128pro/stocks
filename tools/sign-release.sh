#!/bin/sh
# Sign the Stocks release artifacts and build the signed dnf repository.
#
#   tools/sign-release.sh IN_DIR OUT_DIR
#
# IN_DIR   holds the unsigned release files: stocks-<v>-<r>.<dist>.<arch>.rpm,
#          stocks-<v>-<r>.<dist>.src.rpm, stocks-<v>.tar.gz
# OUT_DIR  must not exist; it receives
#            release/  signed RPMs, tarball + .asc, SHA256SUMS + .asc, public key
#                      (the GitHub release assets)
#            site/     the dnf repository tree for GitHub Pages
#                      (fedora/<N>/<arch>/, fedora/<N>/SRPMS/, stocks.repo, key)
#
# Environment:
#   SIGN_KEY   fingerprint of the signing key (required, 40 hex digits)
#   GNUPGHOME  GnuPG home that holds the secret key (required, absolute)
#   SIGN_PASSPHRASE_CMD  optional command that prints the key's passphrase; it
#              is loaded into gpg-agent's cache (gpg-preset-passphrase) so
#              signing runs unattended. Without it gpg-agent asks via pinentry.
#
# Everything is verified before it is declared done: every RPM must pass
# `rpm -K` against a scratch rpmdb that trusts only SIGN_KEY, and every detached
# signature must pass `gpg --verify`. The key is never written to OUT_DIR.
set -eu

die() { printf 'sign-release: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing command: $1 (package: $2)"; }

[ $# -eq 2 ] || die "usage: $0 IN_DIR OUT_DIR"
in=$1 out=$2
[ -d "$in" ] || die "IN_DIR '$in' is not a directory"
[ ! -e "$out" ] || die "OUT_DIR '$out' exists; refusing to overwrite"
[ -n "${SIGN_KEY:-}" ] || die "SIGN_KEY (fingerprint) is required"
case $SIGN_KEY in *[!0-9A-F]*|'') die "SIGN_KEY must be an uppercase hex fingerprint";; esac
[ "${#SIGN_KEY}" -eq 40 ] || die "SIGN_KEY must be a full 40-digit fingerprint, not a short key id"
case ${GNUPGHOME:-} in /*) ;; *) die "GNUPGHOME must be an absolute path";; esac
export GNUPGHOME

need gpg gnupg2
need rpm rpm
need rpmsign rpm-sign
need createrepo_c createrepo_c
need sha256sum coreutils

gpg --batch --list-secret-keys "$SIGN_KEY" >/dev/null 2>&1 || die "no secret key $SIGN_KEY in $GNUPGHOME"

if [ -n "${SIGN_PASSPHRASE_CMD:-}" ]; then
  preset=/usr/libexec/gpg-preset-passphrase
  [ -x "$preset" ] || die "$preset not found (package: gnupg2)"
  grips=$(gpg --batch --with-colons --with-keygrip --list-secret-keys "$SIGN_KEY" | sed -n 's/^grp:::::::::\([0-9A-F]*\):$/\1/p')
  [ -n "$grips" ] || die "no keygrip for $SIGN_KEY"
  # A key and its subkeys share one passphrase; preset every keygrip.
  pass=$(sh -c "$SIGN_PASSPHRASE_CMD") || die "SIGN_PASSPHRASE_CMD failed"
  [ -n "$pass" ] || die "SIGN_PASSPHRASE_CMD printed an empty passphrase"
  for g in $grips; do printf %s "$pass" | "$preset" --preset "$g"; done
  unset pass
fi

mkdir -p "$out/release" "$out/site"
work=$(mktemp -d "${TMPDIR:-/tmp}/sign-release.XXXXXX")
trap 'gpgconf --kill gpg-agent >/dev/null 2>&1 || true; rm -rf "$work"' EXIT

pub="$out/release/RPM-GPG-KEY-stocks"
gpg --batch --armor --export "$SIGN_KEY" > "$pub"
[ -s "$pub" ] || die "exporting the public key failed"
here=$(cd "$(dirname "$0")/.." && pwd -P)
cmp -s "$pub" "$here/packaging/rpm/RPM-GPG-KEY-stocks" || die "packaging/rpm/RPM-GPG-KEY-stocks differs from the exported public key of $SIGN_KEY"

# Collect the inputs.
rpms=$(find "$in" -maxdepth 1 -type f -name '*.rpm' | sort)
tarballs=$(find "$in" -maxdepth 1 -type f -name '*.tar.gz' | sort)
[ -n "$rpms" ] || die "no .rpm files in $in"
for f in $rpms $tarballs; do cp -- "$f" "$out/release/"; done

# 1. Sign the RPMs (header+payload signature, RSA/SHA-256, in place).
for f in "$out"/release/*.rpm; do
  rpmsign --addsign \
    --define "_gpg_name $SIGN_KEY" \
    --define "_gpg_path $GNUPGHOME" \
    "$f"
done

# 2. Verify them against a scratch rpmdb that trusts exactly this key.
db="$work/rpmdb"; mkdir "$db"
rpm --dbpath "$db" --import "$pub"
for f in "$out"/release/*.rpm; do
  # `rpm -K` exits 0 for an unsigned package ("digests OK"), so insist on the
  # words "signatures OK" in the one-line form.
  res=$(rpm --dbpath "$db" -K "$f") || { printf '%s\n' "$res" >&2; die "signature check failed for $f"; }
  case $res in *"digests signatures OK") ;; *) printf '%s\n' "$res" >&2; die "$f is not validly signed by $SIGN_KEY";; esac
done

# 3. Checksums and detached signatures for the non-RPM files.
(
  cd "$out/release"
  sha256sum -- *.rpm *.tar.gz > SHA256SUMS
  for f in SHA256SUMS *.tar.gz; do
    gpg --batch --yes --armor --detach-sign --local-user "$SIGN_KEY" --output "$f.asc" -- "$f"
    gpg --batch --verify "$f.asc" "$f" 2>/dev/null || die "detached signature for $f does not verify"
  done
)

# 4. dnf repository tree. Layout: fedora/<N>/<arch>/ and fedora/<N>/SRPMS/.
site="$out/site"
for f in "$out"/release/*.rpm; do
  rel=$(rpm -qp --qf '%{RELEASE}' "$f")
  arch=$(rpm -qp --qf '%{ARCH}' "$f")
  fc=$(printf '%s' "$rel" | sed -n 's/.*\.fc\([0-9][0-9]*\)$/\1/p')
  [ -n "$fc" ] || die "cannot read the Fedora release from '$rel' in $f"
  case $f in *.src.rpm) sub=SRPMS;; *) sub=$arch;; esac
  mkdir -p "$site/fedora/$fc/$sub"
  cp -- "$f" "$site/fedora/$fc/$sub/"
done
for d in "$site"/fedora/*/*/; do
  createrepo_c --quiet --general-compress-type=gz "$d"
  gpg --batch --yes --armor --detach-sign --local-user "$SIGN_KEY" --output "$d/repodata/repomd.xml.asc" -- "$d/repodata/repomd.xml"
  gpg --batch --verify "$d/repodata/repomd.xml.asc" "$d/repodata/repomd.xml" 2>/dev/null || die "repomd.xml signature in $d does not verify"
done
cp -- "$pub" "$site/RPM-GPG-KEY-stocks"
cp -- "$here/packaging/rpm/stocks.repo" "$here/packaging/rpm/index.html" "$site/"
: > "$site/.nojekyll"

printf 'Signed with %s\n' "$SIGN_KEY"
printf 'Release assets: %s/release\nPages tree:     %s/site\n' "$out" "$out"
