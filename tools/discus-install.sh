#!/bin/bash
# Install DiscusSlicer from a GitHub Actions build into /Applications/DiscusSlicer.app.
#
#   tools/discus-install.sh                  # latest successful build of the discus branch
#   tools/discus-install.sh --run 123456789  # the build from one workflow run
#   tools/discus-install.sh --version 1.0.2  # the zip attached to the discus-1.0.2 release
#
# The previous app is kept at ~/Vibes/DiscusSlicer-backups/DiscusSlicer-prev.app.
# Never touches /Applications/OrcaSlicer.app, ~/Library/Application Support/DiscusSlicer or the
# Keychain. After the ad-hoc re-sign macOS may ask once more for access to the
# "DiscusSlicer/Spoolside" Keychain item; that is expected.
set -euo pipefail

REPO=shelbyklein/DiscusSlicer
WORKFLOW=discus_macos.yml
APP=/Applications/DiscusSlicer.app
BACKUP_DIR="$HOME/Vibes/DiscusSlicer-backups"
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

run_id="" version=""
while [ $# -gt 0 ]; do
    case "$1" in
        --run)     run_id="$2"; shift 2 ;;
        --version) version="$2"; shift 2 ;;
        -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
done

if pgrep -f "DiscusSlicer.app/Contents/MacOS" >/dev/null; then
    echo "DiscusSlicer is running. Quit it (Cmd-Q) and run this again." >&2
    exit 1
fi

tmp=$(mktemp -d -t discus-install)
trap 'rm -rf "$tmp"' EXIT

if [ -n "$version" ]; then
    echo "Downloading release discus-$version..."
    gh release download "discus-$version" -R "$REPO" -p 'DiscusSlicer-*.zip' -D "$tmp"
else
    if [ -z "$run_id" ]; then
        run_id=$(gh run list -R "$REPO" -w "$WORKFLOW" -b discus -s success -L 1 --json databaseId -q '.[0].databaseId')
        [ -n "$run_id" ] || { echo "No successful $WORKFLOW run found." >&2; exit 1; }
    fi
    echo "Downloading the build from run $run_id..."
    gh run download "$run_id" -R "$REPO" -p 'DiscusSlicer-*' -D "$tmp"
fi

zip=$(find "$tmp" -name 'DiscusSlicer-*.zip' -print -quit)
[ -n "$zip" ] || { echo "No DiscusSlicer zip in the download." >&2; exit 1; }
echo "Unpacking $(basename "$zip")..."
mkdir "$tmp/app"
ditto -x -k "$zip" "$tmp/app"
new="$tmp/app/DiscusSlicer.app"
[ -d "$new/Contents/MacOS" ] || { echo "The zip has no DiscusSlicer.app." >&2; exit 1; }

if [ -d "$APP" ]; then
    mkdir -p "$BACKUP_DIR"
    rm -rf "$BACKUP_DIR/DiscusSlicer-prev.app"
    mv "$APP" "$BACKUP_DIR/DiscusSlicer-prev.app"
    echo "Previous app moved to $BACKUP_DIR/DiscusSlicer-prev.app"
fi

ditto "$new" "$APP"
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
codesign --force --deep --sign - "$APP"
"$LSREGISTER" -f "$APP"
touch "$APP"
killall Dock 2>/dev/null || true

echo "Installed DiscusSlicer $(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString) at $APP"
