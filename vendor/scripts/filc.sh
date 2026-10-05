#!/usr/bin/env bash
# F00 toolchain script: pinned Fil-C 0.685 (dependencies.json "filc" entry).
# Installs the two Linux/x86_64 distributions outside the repo at
# /home/msaraiva/.local/fil-c/0.685 (override with FILC_ROOT) and verifies them.
#
# Assets (https://github.com/pizlonator/fil-c/releases/tag/v0.685); size and
# sha256 are the values published in the GitHub release API asset metadata:
#   filc-0.685-linux-x86_64.tar.xz   77045600 bytes  (pizfix/musl distro)
#   optfil-0.685-linux-x86_64.tar.xz 274092224 bytes (/opt/fil glibc distro)
#
# Idempotent and non-interactive: verified downloads and extracted trees are
# reused; nothing existing is deleted. Requires curl, tar (xz), patchelf
# (pizfix setup.sh) and root only for the /opt/fil bind mount described below.
#
# /opt/fil: the optfil binaries hardcode the absolute prefix /opt/fil (their ELF
# interpreter is /opt/fil/lib/ld-fil1-x86_64.so and the driver selects
# /opt/fil/include, /opt/fil/lib only when its own executable path is under
# /opt/fil). The physical tree stays at the recorded local path; a bind mount at
# /opt/fil presents it at the canonical prefix so the unmodified binaries work.
# The mount is runtime state and must be re-established after a reboot by
# re-running this script.
set -euo pipefail

VERSION=0.685
ARCH=x86_64
ROOT="${FILC_ROOT:-/home/msaraiva/.local/fil-c/$VERSION}"
DL_DIR="${FILC_DL_DIR:-/tmp}"
RELEASE_BASE="https://github.com/pizlonator/fil-c/releases/download/v$VERSION"

FILC_NAME="filc-$VERSION-linux-$ARCH.tar.xz"
OPTFIL_NAME="optfil-$VERSION-linux-$ARCH.tar.xz"
FILC_URL="$RELEASE_BASE/$FILC_NAME"
OPTFIL_URL="$RELEASE_BASE/$OPTFIL_NAME"
FILC_SHA256="d12bd30c33f18179a9355b32ea44ba61dcc0342c7d77d1ac2548852e64994727"
OPTFIL_SHA256="f7d2d73b17ee9bfff0859ca15b3ca9c10e8501f8d891b0dfa961e81f0390e788"
FILC_SIZE=77045600
OPTFIL_SIZE=274092224
MIN_FREE_KIB=$((4 * 1024 * 1024)) # 4 GiB

PIZFIX_DIR="$ROOT/filc-$VERSION-linux-$ARCH"       # unpacked pizfix distribution
OPTFIL_DIR="$ROOT/optfil-$VERSION-linux-$ARCH"     # unpacked /opt/fil installer
OPTFIL_TREE="$OPTFIL_DIR/opt/fil"                  # extracted /opt/fil payload
PIZFIX_CC="$PIZFIX_DIR/build/bin/clang"
OPTFIL_CC="/opt/fil/bin/filcc"

log() { echo "filc: $*"; }
die() { echo "filc: ERROR: $*" >&2; exit 1; }

sha256_of() { sha256sum "$1" | awk '{print $1}'; }

fetch() { # fetch <url> <file> <sha256> <size>
  local url=$1 file=$2 sha=$3 size=$4
  if [ -f "$file" ] && [ "$(stat -c %s "$file")" = "$size" ] && [ "$(sha256_of "$file")" = "$sha" ]; then
    log "reusing verified $(basename "$file")"
    return 0
  fi
  log "downloading $url"
  curl -fSL --retry 3 --retry-delay 2 -o "$file.part" "$url"
  if [ "$(stat -c %s "$file.part")" != "$size" ]; then
    rm -f "$file.part"
    die "size mismatch for $(basename "$file") (expected $size)"
  fi
  if [ "$(sha256_of "$file.part")" != "$sha" ]; then
    rm -f "$file.part"
    die "sha256 mismatch for $(basename "$file")"
  fi
  mv "$file.part" "$file"
  log "verified sha256 $sha"
}

# 1. Disk space guard (task F00: stop if less than 4 GiB free).
mkdir -p "$ROOT"
free_kib="$(df -Pk "$ROOT" | awk 'NR==2 {print $4}')"
if [ "$free_kib" -lt "$MIN_FREE_KIB" ]; then
  df -h "$ROOT"
  die "less than 4 GiB free on the filesystem holding $ROOT"
fi
log "free space ok ($((free_kib / 1024 / 1024)) GiB) on $ROOT"

# 2. Downloads.
fetch "$FILC_URL" "$DL_DIR/$FILC_NAME" "$FILC_SHA256" "$FILC_SIZE"
fetch "$OPTFIL_URL" "$DL_DIR/$OPTFIL_NAME" "$OPTFIL_SHA256" "$OPTFIL_SIZE"

# 3. Pizfix distribution: extract and run its relocatable setup.sh once.
if [ ! -x "$PIZFIX_CC" ]; then
  [ -f "$DL_DIR/$FILC_NAME" ] || die "missing $DL_DIR/$FILC_NAME"
  log "extracting $FILC_NAME to $ROOT"
  tar -xJf "$DL_DIR/$FILC_NAME" -C "$ROOT"
fi
if [ ! -d "$PIZFIX_DIR/pizfix/os-include/linux" ]; then
  command -v patchelf >/dev/null || die "patchelf is required by the pizfix setup.sh"
  log "running pizfix setup.sh (patchelf rpaths + os-include symlinks)"
  ( cd "$PIZFIX_DIR" && ./setup.sh >/dev/null )
fi
[ -x "$PIZFIX_CC" ] || die "pizfix compiler missing at $PIZFIX_CC"

# 4. /opt/fil distribution: unpack packaging tarball, then the fil.tar.xz payload
#    (setup.sh would extract it straight to /opt/fil as root; we keep the payload
#    under the recorded local path and mount it, see the header comment).
if [ ! -f "$OPTFIL_DIR/fil.tar.xz" ]; then
  log "extracting $OPTFIL_NAME to $ROOT"
  tar -xJf "$DL_DIR/$OPTFIL_NAME" -C "$ROOT"
fi
if [ ! -x "$OPTFIL_TREE/bin/filcc" ]; then
  log "extracting optfil payload fil.tar.xz to $OPTFIL_DIR/opt"
  mkdir -p "$OPTFIL_DIR/opt"
  tar -xJf "$OPTFIL_DIR/fil.tar.xz" -C "$OPTFIL_DIR/opt"
fi
[ -x "$OPTFIL_TREE/bin/filcc" ] || die "optfil compiler missing at $OPTFIL_TREE/bin/filcc"

# 5. Present the tree at the canonical /opt/fil prefix (bind mount; root needed).
same_tree() { [ "$(stat -c '%d:%i' /opt/fil)" = "$(stat -c '%d:%i' "$OPTFIL_TREE")" ]; }
if [ -L /opt/fil ]; then
  if [ "$(readlink -f /opt/fil)" = "$(readlink -f "$OPTFIL_TREE")" ]; then
    log "replacing our /opt/fil symlink with a bind mount (driver needs real prefix)"
    sudo rm /opt/fil || die "cannot remove /opt/fil symlink (root required)"
    sudo mkdir -p /opt/fil || die "cannot create /opt/fil (root required)"
    sudo mount --bind "$OPTFIL_TREE" /opt/fil || die "bind mount of $OPTFIL_TREE at /opt/fil failed (root required)"
  else
    die "/opt/fil is a symlink to $(readlink -f /opt/fil); refusing to modify it"
  fi
elif [ -d /opt/fil ]; then
  if same_tree; then
    log "/opt/fil already is a bind mount of $OPTFIL_TREE"
  elif [ -x /opt/fil/bin/filcc ]; then
    log "using existing /opt/fil installation (not touching it)"
  elif [ -z "$(ls -A /opt/fil)" ]; then
    sudo mount --bind "$OPTFIL_TREE" /opt/fil || die "bind mount of $OPTFIL_TREE at /opt/fil failed (root required)"
  else
    die "/opt/fil exists, is not a mount of $OPTFIL_TREE and has no bin/filcc; refusing to modify it"
  fi
elif [ -e /opt/fil ]; then
  die "/opt/fil exists and is not a directory; refusing to modify it"
else
  sudo mkdir -p /opt/fil || die "cannot create /opt/fil (root required)"
  sudo mount --bind "$OPTFIL_TREE" /opt/fil || die "bind mount of $OPTFIL_TREE at /opt/fil failed (root required)"
fi

# 6. Verify both compilers end to end with the release's recipe.
check_cc() { # check_cc <label> <compiler> <version_needle>
  local label=$1 cc=$2 needle=$3
  local out
  out="$("$cc" --version 2>&1 | head -1)"
  case "$out" in
    *"$needle"*) log "$label --version ok: $out" ;;
    *) die "$label --version unexpected: $out" ;;
  esac
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
printf '#include <stdio.h>\n\nint main(void) {\n    printf("Hello from Fil-C!\\n");\n    return 0;\n}\n' > "$TMP/hello.c"

run_hello() { # run_hello <label> <compiler>
  local label=$1 cc=$2 out
  ( cd "$TMP" && "$cc" -O2 -g -o "hello-$label" hello.c ) || die "$label compile failed"
  out="$(cd "$TMP" && "./hello-$label")" || die "$label run failed"
  [ "$out" = "Hello from Fil-C!" ] || die "$label output unexpected: $out"
  log "$label hello: compile=0 run=0 output='$out'"
}

check_cc "pizfix" "$PIZFIX_CC" "Fil-C 0.685"
check_cc "optfil" "$OPTFIL_CC" "Fil-C 0.685"
run_hello pizfix "$PIZFIX_CC"
run_hello optfil "$OPTFIL_CC"

cat <<EOF
filc: READY
  install root      $ROOT
  pizfix compiler   $PIZFIX_CC   (build/bin/filcc, build/bin/clang++ symlink to clang-20)
  optfil compiler   $OPTFIL_CC   (fil++ symlink; presented via bind mount $OPTFIL_TREE)
  env (optional)    export PATH=/opt/fil/bin:\$PATH
  app compile flags -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread
EOF
