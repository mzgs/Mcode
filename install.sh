#!/bin/sh
set -eu

# Install or update the latest GitHub Release on macOS (or Linux).
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/mzgs/Mcode/main/install.sh | sh
#   curl -fsSL https://raw.githubusercontent.com/mzgs/Mcode/main/install.sh | sh -s -- v0.1.3
#
# Environment overrides:
#   REPO         GitHub "owner/repo" (default: mzgs/Mcode)
#   VERSION      Release tag to install (default: latest)
#   INSTALL_DIR  Destination directory (default: /Applications on macOS, ~/.local/bin on Linux)

REPO="${REPO:-mzgs/Mcode}"
VERSION="${VERSION:-${1:-}}"

case "$REPO" in
    */*)
        REPO_NAME="${REPO#*/}"
        ;;
    *)
        echo "Invalid REPO '$REPO'. Expected 'owner/repo'." >&2
        exit 1
        ;;
esac

if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required to install." >&2
    exit 1
fi

OS_RAW="$(uname -s)"
ARCH_RAW="$(uname -m)"

case "$OS_RAW" in
    Darwin) OS="macos" ;;
    Linux)  OS="linux" ;;
    *)
        echo "Unsupported OS: $OS_RAW" >&2
        exit 1
        ;;
esac

case "$ARCH_RAW" in
    arm64|aarch64) ARCH="arm64" ;;
    x86_64|amd64)  ARCH="x64" ;;
    *)
        echo "Unsupported architecture: $ARCH_RAW" >&2
        exit 1
        ;;
esac

if [ -n "$VERSION" ]; then
    case "$VERSION" in
        v*) TAG="$VERSION" ;;
        *)  TAG="v$VERSION" ;;
    esac
else
    LATEST_URL="$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest")"
    case "$LATEST_URL" in
        */releases/tag/*)
            TAG="${LATEST_URL##*/releases/tag/}"
            ;;
        *)
            echo "Could not find a published GitHub Release for $REPO." >&2
            exit 1
            ;;
    esac
fi

case "$TAG" in
    ""|*/*|*" "*)
        echo "Invalid release tag: '$TAG'" >&2
        exit 1
        ;;
esac

if [ "$OS" = "macos" ]; then
    ASSET="${REPO_NAME}-${TAG}-macos-${ARCH}.zip"
else
    ASSET="${REPO_NAME}-${TAG}-linux-${ARCH}.tar.gz"
fi

URL="https://github.com/$REPO/releases/download/$TAG/$ASSET"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/${REPO_NAME}-install.XXXXXX")"
STAGED_DEST=""

cleanup() {
    rm -rf "$TMP_DIR"
    if [ -n "$STAGED_DEST" ] && [ -e "$STAGED_DEST" ]; then
        rm -rf "$STAGED_DEST"
    fi
}
trap cleanup EXIT INT TERM

ARCHIVE="$TMP_DIR/$ASSET"
echo "Downloading $REPO $TAG ($OS/$ARCH)..."
if ! curl -fsSL -o "$ARCHIVE" "$URL"; then
    echo "Failed to download $URL" >&2
    echo "Check that release $TAG has an asset for $OS/$ARCH." >&2
    exit 1
fi

EXTRACT_DIR="$TMP_DIR/extract"
mkdir -p "$EXTRACT_DIR"

if [ "$OS" = "macos" ]; then
    ditto -x -k "$ARCHIVE" "$EXTRACT_DIR"

    APP_SRC=""
    for candidate in "$EXTRACT_DIR"/*.app; do
        if [ -d "$candidate" ]; then
            APP_SRC="$candidate"
            break
        fi
    done

    if [ -z "$APP_SRC" ]; then
        echo "No .app bundle found inside $ASSET." >&2
        exit 1
    fi

    APP_BASENAME="$(basename "$APP_SRC")"

    if [ -n "${INSTALL_DIR:-}" ]; then
        TARGET_DIR="$INSTALL_DIR"
    elif [ -w /Applications ]; then
        TARGET_DIR="/Applications"
    else
        TARGET_DIR="$HOME/Applications"
    fi
    mkdir -p "$TARGET_DIR"

    DEST="$TARGET_DIR/$APP_BASENAME"
    STAGED_DEST="$TARGET_DIR/.${APP_BASENAME}.installing.$$"
    BACKUP_DEST="$TARGET_DIR/.${APP_BASENAME}.backup.$$"

    ditto "$APP_SRC" "$STAGED_DEST"
    xattr -dr com.apple.quarantine "$STAGED_DEST" 2>/dev/null || true

    if [ -e "$DEST" ]; then
        mv "$DEST" "$BACKUP_DEST"
    fi
    if mv "$STAGED_DEST" "$DEST"; then
        STAGED_DEST=""
        rm -rf "$BACKUP_DEST"
    else
        if [ -e "$BACKUP_DEST" ]; then
            mv "$BACKUP_DEST" "$DEST"
        fi
        exit 1
    fi

    echo "Installed $APP_BASENAME ($TAG) to $DEST"
    echo "Open with: open \"$DEST\""
else
    tar -xzf "$ARCHIVE" -C "$EXTRACT_DIR"

    TARGET_DIR="${INSTALL_DIR:-$HOME/.local/bin}"
    mkdir -p "$TARGET_DIR"

    INSTALLED=0
    INSTALLED_BIN=""
    for candidate in "$EXTRACT_DIR"/*; do
        if [ -f "$candidate" ] && [ -x "$candidate" ]; then
            BIN_NAME="$(basename "$candidate")"
            install -m 755 "$candidate" "$TARGET_DIR/$BIN_NAME"
            echo "Installed $BIN_NAME ($TAG) to $TARGET_DIR/$BIN_NAME"
            if [ -z "$INSTALLED_BIN" ]; then
                INSTALLED_BIN="$TARGET_DIR/$BIN_NAME"
            fi
            INSTALLED=1
        fi
    done

    if [ "$INSTALLED" -eq 0 ]; then
        echo "No executable found inside $ASSET." >&2
        exit 1
    fi

    DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}"
    if [ -d "$EXTRACT_DIR/share/icons/hicolor/512x512/apps" ]; then
        ICON_DIR="$DATA_DIR/icons/hicolor/512x512/apps"
        mkdir -p "$ICON_DIR"
        for icon in "$EXTRACT_DIR"/share/icons/hicolor/512x512/apps/*.png; do
            if [ -f "$icon" ]; then
                install -m 644 "$icon" "$ICON_DIR/$(basename "$icon")"
            fi
        done
        if command -v gtk-update-icon-cache >/dev/null 2>&1; then
            gtk-update-icon-cache -f -t "$DATA_DIR/icons/hicolor" >/dev/null 2>&1 || true
        fi
    fi

    if [ -d "$EXTRACT_DIR/share/applications" ]; then
        APP_DIR="$DATA_DIR/applications"
        mkdir -p "$APP_DIR"
        for desktop in "$EXTRACT_DIR"/share/applications/*.desktop; do
            if [ -f "$desktop" ]; then
                DEST_DESKTOP="$APP_DIR/$(basename "$desktop")"
                INSTALLED_BIN="$INSTALLED_BIN" awk '
                    /^Exec=/ {
                        raw = ENVIRON["INSTALLED_BIN"]
                        need_quote = (raw ~ /[[:space:]"\047\\><~|&;$*?()#`]/)
                        out = ""
                        for (i = 1; i <= length(raw); i++) {
                            ch = substr(raw, i, 1)
                            if (ch == "%") {
                                out = out "%%"
                            } else if (need_quote && ch == "\\") {
                                out = out "\\\\\\\\"
                            } else if (need_quote && (ch == "\"" || ch == "$" || ch == "`")) {
                                out = out "\\\\" ch
                            } else if (ch == "\\") {
                                out = out "\\\\"
                            } else {
                                out = out ch
                            }
                        }
                        if (need_quote) out = "\"" out "\""
                        print "Exec=" out
                        next
                    }
                    { print }
                ' "$desktop" > "$DEST_DESKTOP"
                chmod 644 "$DEST_DESKTOP"
            fi
        done
        if command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
        fi
    fi
fi
