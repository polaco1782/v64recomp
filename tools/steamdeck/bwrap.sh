#!/usr/bin/env bash
# Rootless Debian bootstrap; the host needs no package-manager changes.
set -euo pipefail
root=$1
game=$2
jobs=$3
for program in bwrap unshare newuidmap newgidmap curl sha256sum tar flock; do
    if ! command -v "$program" >/dev/null; then
        printf 'Bubblewrap builds require %s on the host. See docs/steamdeck.md.\n' "$program" >&2
        exit 1
    fi
done

# Official Debian Docker image artifacts. Pin both the revision and layer digest.
revision=a3751996111ce2c1c904050c84440184535e23b1
digest=7fd1ebe418b5079d4aa36f416af9b143d127bc75b0155d71cb5fd559b5e04d39
url="https://raw.githubusercontent.com/debuerreotype/docker-debian-artifacts/$revision/bookworm/slim/oci/blobs/rootfs.tar.gz"
cache="$root/build/steamdeck/bwrap/$digest"
filesystem="$cache/rootfs"
mkdir -p "$cache"
# Serialize setup and builds that share this filesystem and the Python cache.
exec 9>"$root/build/steamdeck/bwrap/build.lock"
flock 9
archive="$cache/debian-bookworm.tar.gz"
verify() {
    local actual
    actual=$(sha256sum -- "$1")
    if [[ ${actual%% *} != "$digest" ]]; then
        printf 'Debian image checksum mismatch: %s\n' "$1" >&2
        return 1
    fi
}
if [[ ! -f $archive ]]; then
    printf 'Downloading pinned Debian 12 root filesystem (28 MB)\n'
    curl --fail --location --retry 3 --output "$archive.part" "$url"
    verify "$archive.part"
    mv -- "$archive.part" "$archive"
else
    verify "$archive"
fi
if [[ ! -d $filesystem ]]; then
    unpack=$(mktemp -d "$cache/unpack.XXXXXX")
    trap 'rm -rf -- "$unpack"' EXIT
    tar --extract --gzip --file "$archive" --directory "$unpack" --no-same-owner
    mkdir -p "$unpack/src" "$unpack/tmp"
    mv -- "$unpack" "$filesystem"
    trap - EXIT
fi

# A full subordinate UID/GID mapping lets Debian configure package ownerships.
# Mapping only the current UID to root makes chown to system groups fail.
namespace=(unshare --map-auto --map-root-user)
options=(--die-with-parent --unshare-pid --unshare-ipc --unshare-uts
    --proc /proc --dev /dev --perms 1777 --tmpfs /tmp
    --ro-bind /etc/resolv.conf /etc/resolv.conf
    --clearenv --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
    --setenv HOME /tmp)
setup_key=$(sha256sum -- "$root/tools/steamdeck/setup.sh")
setup_key=${setup_key%% *}
if [[ ! -f $cache/setup.key || $(<"$cache/setup.key") != "$setup_key" ]]; then
    printf 'Installing build dependencies inside the Debian filesystem\n'
    "${namespace[@]}" bwrap --bind "$filesystem" / "${options[@]}" \
        --ro-bind "$root/tools/steamdeck/setup.sh" /tmp/steamdeck-setup.sh \
        -- /bin/bash /tmp/steamdeck-setup.sh
    printf '%s\n' "$setup_key" > "$cache/setup.key"
fi

# During compilation even the Debian base is read-only. Only build caches and
# the private /tmp are writable; the host environment and tools are excluded.
"${namespace[@]}" bwrap --ro-bind "$filesystem" / "${options[@]}" \
    --ro-bind "$root" /src \
    --bind "$root/build/steamdeck" /src/build/steamdeck \
    --ro-bind "$root/build/steamdeck/bwrap" /src/build/steamdeck/bwrap \
    --bind "$root/build/shared/rt64/steamdeck" /src/build/shared/rt64/steamdeck \
    --chdir /src -- /bin/bash tools/steamdeck/build.sh "$game" "$jobs"
