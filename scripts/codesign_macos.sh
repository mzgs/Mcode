#!/bin/sh
set -eu

APP_BUNDLE="${1:-}"
if [ -z "$APP_BUNDLE" ] || [ ! -d "$APP_BUNDLE" ]; then
    echo "codesign_macos.sh: missing app bundle path" >&2
    exit 1
fi

P12_PATH="${MCODE_CODESIGN_P12:-$HOME/.macos-codesign/macos-codesign.p12}"
PASS_PATH="${MCODE_CODESIGN_PASSWORD:-$HOME/.macos-codesign/password}"
KEYCHAIN="${MCODE_CODESIGN_KEYCHAIN:-$HOME/.macos-codesign/mcode-signing.keychain-db}"

if [ ! -f "$P12_PATH" ] || [ ! -f "$PASS_PATH" ]; then
    exit 0
fi

PASS="$(tr -d '\r\n' < "$PASS_PATH")"
if [ -z "$PASS" ]; then
    echo "codesign_macos.sh: password file is empty" >&2
    exit 1
fi

# Extract leaf certificate SHA-1 fingerprint (hex, no colons)
HASH="$(openssl pkcs12 -in "$P12_PATH" -passin "file:$PASS_PATH" -nokeys -clcerts 2>/dev/null \
    | openssl x509 -noout -fingerprint -sha1 2>/dev/null \
    | sed 's/^.*=//; s/://g')"

if [ -z "$HASH" ]; then
    echo "codesign_macos.sh: failed to read certificate from $P12_PATH" >&2
    exit 1
fi

import_p12() {
    if security import "$P12_PATH" -k "$KEYCHAIN" -P "$PASS" -T /usr/bin/codesign >/dev/null 2>&1; then
        return 0
    fi
    # macOS SecKeychainItemImport rejects OpenSSL 3+ modern PKCS#12 encryption; re-pack into a legacy temp .p12.
    TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mcode-sign.XXXXXX")"
    openssl pkcs12 -in "$P12_PATH" -passin "file:$PASS_PATH" -nodes -out "$TMP_DIR/bundle.pem" 2>/dev/null
    openssl pkcs12 -export -legacy -in "$TMP_DIR/bundle.pem" -out "$TMP_DIR/legacy.p12" -passout "file:$PASS_PATH" 2>/dev/null
    security import "$TMP_DIR/legacy.p12" -k "$KEYCHAIN" -P "$PASS" -T /usr/bin/codesign >/dev/null
    rm -rf "$TMP_DIR"
}

if [ ! -f "$KEYCHAIN" ]; then
    security create-keychain -p "$PASS" "$KEYCHAIN" >/dev/null
    security set-keychain-settings "$KEYCHAIN" >/dev/null
    security unlock-keychain -p "$PASS" "$KEYCHAIN" >/dev/null
    import_p12
    security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$PASS" "$KEYCHAIN" >/dev/null
else
    security unlock-keychain -p "$PASS" "$KEYCHAIN" >/dev/null
    if ! security find-identity -p codesigning "$KEYCHAIN" 2>/dev/null | grep -qi "$HASH"; then
        import_p12
        security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$PASS" "$KEYCHAIN" >/dev/null
    fi
fi

# codesign only resolves private keys from keychains in the user search list.
# Temporarily add our signing keychain and restore the original list on exit.
ORIG_KEYCHAINS="$(security list-keychains -d user | sed 's/^[[:space:]]*"//; s/"[[:space:]]*$//')"
restore_keychains() {
    # shellcheck disable=SC2086
    IFS='
'
    set -- $ORIG_KEYCHAINS
    if [ "$#" -gt 0 ]; then
        security list-keychains -d user -s "$@" >/dev/null 2>&1 || true
    fi
}
trap restore_keychains EXIT

IFS='
'
# shellcheck disable=SC2086
set -- $ORIG_KEYCHAINS "$KEYCHAIN"
security list-keychains -d user -s "$@" >/dev/null

codesign --force --deep --sign "$HASH" --keychain "$KEYCHAIN" "$APP_BUNDLE"
