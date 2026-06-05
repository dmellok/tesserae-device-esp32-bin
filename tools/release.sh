#!/usr/bin/env bash
#
# Cut a GitHub release for the current FW_VERSION in platformio.ini.
#
# What it does:
#   1. Reads FW_VERSION from platformio.ini.
#   2. Refuses to run if the working tree is dirty or if origin/main is ahead.
#   3. Builds the firmware fresh.
#   4. Copies the four .bin artifacts into release/<version>/ and writes
#      SHA256SUMS alongside them.
#   5. Tags vX.Y.Z (if not already tagged) and pushes the tag.
#   6. Creates / updates the GitHub release with the artifacts attached.
#
# Usage:
#   tools/release.sh                 # build, tag, release using FW_VERSION
#   tools/release.sh --notes-only    # just print suggested release notes
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

VERSION=$(awk -F'"' '/-DFW_VERSION/ {print $2; exit}' platformio.ini)
if [[ -z "${VERSION:-}" ]]; then
    echo "error: couldn't parse FW_VERSION from platformio.ini" >&2
    exit 1
fi
TAG="v${VERSION}"

if [[ "${1:-}" == "--notes-only" ]]; then
    echo "## tesserae-esp32-bin-client ${TAG}"
    echo
    git log --pretty=format:"- %s" "${TAG}~1..HEAD" 2>/dev/null \
        || git log --pretty=format:"- %s" -20
    exit 0
fi

# --- guardrails -------------------------------------------------------------
if [[ -n "$(git status --porcelain)" ]]; then
    echo "error: working tree is dirty; commit/stash first" >&2
    git status --short >&2
    exit 1
fi

git fetch origin --quiet
if ! git merge-base --is-ancestor origin/main HEAD; then
    echo "error: origin/main has commits this branch doesn't; pull/rebase first" >&2
    exit 1
fi

PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
ENV="${ENV:-tesserae-esp32-bin-client}"

# --- build ------------------------------------------------------------------
echo "==> building ${ENV} for FW_VERSION=${VERSION}"
"$PIO" run -e "$ENV" >/dev/null

BUILD_DIR=".pio/build/${ENV}"
for f in bootloader.bin partitions.bin firmware.bin firmware.factory.bin; do
    [[ -f "$BUILD_DIR/$f" ]] || { echo "error: missing $BUILD_DIR/$f" >&2; exit 1; }
done

OUT_DIR="release/${VERSION}"
mkdir -p "$OUT_DIR"
cp "$BUILD_DIR"/bootloader.bin       "$OUT_DIR/"
cp "$BUILD_DIR"/partitions.bin       "$OUT_DIR/"
cp "$BUILD_DIR"/firmware.bin         "$OUT_DIR/"
cp "$BUILD_DIR"/firmware.factory.bin "$OUT_DIR/"

(cd "$OUT_DIR" && shasum -a 256 *.bin > SHA256SUMS)
echo "==> artifacts:"
ls -la "$OUT_DIR/"
echo "==> SHA256SUMS:"
cat "$OUT_DIR/SHA256SUMS"

# --- tag --------------------------------------------------------------------
if git rev-parse "$TAG" >/dev/null 2>&1; then
    echo "==> tag ${TAG} already exists; skipping tag step"
else
    echo "==> tagging ${TAG}"
    git tag -a "$TAG" -m "$TAG"
    git push origin "$TAG"
fi

# --- release ----------------------------------------------------------------
if gh release view "$TAG" >/dev/null 2>&1; then
    echo "==> release ${TAG} already exists; uploading (clobbering) assets"
    gh release upload "$TAG" "$OUT_DIR"/*.bin "$OUT_DIR/SHA256SUMS" --clobber
else
    echo "==> creating GitHub release ${TAG}"
    NOTES=$(mktemp)
    {
        echo "## Flashing"
        echo
        echo '`firmware.factory.bin` is the combined image; flash it to offset 0 with esptool:'
        echo
        echo '```bash'
        echo 'esptool.py --chip esp32s3 --port /dev/cu.usbmodem... \\'
        echo '    write_flash 0x0 firmware.factory.bin'
        echo '```'
        echo
        echo 'Or flash the three pieces separately at their native offsets:'
        echo
        echo '```bash'
        echo 'esptool.py --chip esp32s3 --port /dev/cu.usbmodem... \\'
        echo '    write_flash 0x0     bootloader.bin \\'
        echo '                0x8000  partitions.bin \\'
        echo '                0x10000 firmware.bin'
        echo '```'
        echo
        echo "## Checksums"
        echo
        echo '```'
        cat "$OUT_DIR/SHA256SUMS"
        echo '```'
        echo
        echo "## Changes"
        echo
        if PREV_TAG=$(git describe --tags --abbrev=0 "${TAG}^" 2>/dev/null); then
            git log --pretty=format:"- %s" "${PREV_TAG}..${TAG}^"
        else
            git log --pretty=format:"- %s"
        fi
    } > "$NOTES"

    gh release create "$TAG" \
        --title "$TAG" \
        --notes-file "$NOTES" \
        "$OUT_DIR"/*.bin "$OUT_DIR/SHA256SUMS"
    rm -f "$NOTES"
fi

echo "==> done"
