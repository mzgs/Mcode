#!/bin/sh
set -eu

cd -- "$(dirname -- "$0")"

REMOTE="${REMOTE:-origin}"
SIGNING_DIR="${MACOS_SIGNING_DIR:-$HOME/.macos-codesign}"
DRY_RUN=0
RAW_VERSION=""

github_repo_slug() {
    git remote get-url "$REMOTE" 2>/dev/null | sed -E \
        -e 's#^https?://([^@]+@)?github\.com/##' \
        -e 's#^ssh://git@github\.com(:[0-9]+)?/##' \
        -e 's#^git://github\.com/##' \
        -e 's#^git@github\.com:##' \
        -e 's#/*$##' \
        -e 's#\.git$##' \
        -e 's#/*$##'
}

sync_macos_signing_secrets() {
    if [ "${SKIP_SIGNING_SYNC:-0}" = "1" ]; then
        return 0
    fi

    cert_file="$SIGNING_DIR/macos-codesign.p12"
    pass_file="$SIGNING_DIR/password"
    if [ ! -f "$cert_file" ] || [ ! -f "$pass_file" ]; then
        return 0
    fi

    gh_repo="$(github_repo_slug)"
    if ! printf '%s\n' "$gh_repo" | grep -Eq '^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$'; then
        return 0
    fi

    if ! command -v gh >/dev/null 2>&1; then
        echo "Found $SIGNING_DIR, but 'gh' CLI is not installed." >&2
        echo "Install with 'brew install gh && gh auth login', or set SKIP_SIGNING_SYNC=1 to release unsigned." >&2
        exit 1
    fi

    if ! gh auth status >/dev/null 2>&1; then
        echo "Found $SIGNING_DIR, but 'gh' is not logged in." >&2
        echo "Run 'gh auth login', or set SKIP_SIGNING_SYNC=1 to release unsigned." >&2
        exit 1
    fi

    if ! secret_list="$(gh secret list -R "$gh_repo")"; then
        echo "Could not list GitHub Actions secrets for '$gh_repo'." >&2
        exit 1
    fi
    secret_names="$(printf '%s\n' "$secret_list" | awk '{ print $1 }')"

    need_cert=0
    need_pass=0
    printf '%s\n' "$secret_names" | grep -qx 'MACOS_CERT_P12' || need_cert=1
    printf '%s\n' "$secret_names" | grep -qx 'MACOS_CERT_PASSWORD' || need_pass=1

    if [ "$need_cert" -eq 0 ] && [ "$need_pass" -eq 0 ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "[dry-run] GitHub signing secrets already configured on $gh_repo"
        fi
        return 0
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        [ "$need_cert" -eq 1 ] && echo "[dry-run] Upload secret MACOS_CERT_P12 to $gh_repo from $cert_file"
        [ "$need_pass" -eq 1 ] && echo "[dry-run] Upload secret MACOS_CERT_PASSWORD to $gh_repo from $pass_file"
        return 0
    fi

    if [ "$need_cert" -eq 1 ]; then
        echo "Uploading MACOS_CERT_P12 to $gh_repo..."
        base64 < "$cert_file" | tr -d '\r\n' | gh secret set MACOS_CERT_P12 -R "$gh_repo"
    fi
    if [ "$need_pass" -eq 1 ]; then
        echo "Uploading MACOS_CERT_PASSWORD to $gh_repo..."
        tr -d '\r\n' < "$pass_file" | gh secret set MACOS_CERT_PASSWORD -R "$gh_repo"
    fi
}

current_version() {
    awk '
        /^[[:space:]]*project\(/ { in_proj = 1 }
        in_proj && /VERSION[[:space:]]+[0-9]+(\.[0-9]+)+/ {
            line = $0
            sub(/^.*VERSION[[:space:]]+/, "", line)
            sub(/[^0-9.].*$/, "", line)
            print line
            exit
        }
        in_proj && /\)/ { in_proj = 0 }
    ' CMakeLists.txt
}

bump_version() {
    awk -F. '
        NF == 3 && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ {
            printf "%d.%d.%d\n", $1, $2, $3 + 1
            exit
        }
    ' <<EOF
$1
EOF
}

usage() {
    cur="$(current_version || true)"
    next="$(bump_version "${cur:-}" || true)"
    cat <<EOF
Usage: ./release.sh [--dry-run] [version]

Auto-increments the project VERSION in CMakeLists.txt (or uses an explicit
MAJOR.MINOR.PATCH version), commits, tags v<version>, and pushes to trigger
the Release CI workflow.

Current version: ${cur:-unknown}
Next version:    ${next:-unknown}
EOF
}

for arg in "$@"; do
    case "$arg" in
        -h|--help)
            usage
            exit 0
            ;;
        -n|--dry-run)
            DRY_RUN=1
            ;;
        -*)
            echo "Unknown option: $arg" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [ -n "$RAW_VERSION" ]; then
                echo "Unexpected argument: $arg" >&2
                usage >&2
                exit 2
            fi
            RAW_VERSION="$arg"
            ;;
    esac
done

CUR_VERSION="$(current_version)"
if [ -z "$CUR_VERSION" ]; then
    echo "Could not find project(... VERSION ...) in CMakeLists.txt." >&2
    exit 1
fi

if [ -z "$RAW_VERSION" ]; then
    VERSION="$(bump_version "$CUR_VERSION")"
    if [ -z "$VERSION" ]; then
        echo "Cannot auto-increment version '$CUR_VERSION' in CMakeLists.txt. Expected MAJOR.MINOR.PATCH." >&2
        exit 1
    fi
else
    VERSION="${RAW_VERSION#v}"
    case "$VERSION" in
        *[!0-9.]*|""|*..*|.*|*.)
            echo "Invalid version '$RAW_VERSION'. Expected semantic version like 0.2.0." >&2
            exit 2
            ;;
    esac
    if ! printf '%s\n' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
        echo "Invalid version '$RAW_VERSION'. Expected MAJOR.MINOR.PATCH (e.g. 0.2.0)." >&2
        exit 2
    fi
fi

TAG="v$VERSION"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Not inside a git repository." >&2
    exit 1
fi

BRANCH="$(git symbolic-ref --quiet --short HEAD || true)"
if [ -z "$BRANCH" ]; then
    echo "Detached HEAD: check out a branch before releasing." >&2
    exit 1
fi

if ! git remote get-url "$REMOTE" >/dev/null 2>&1; then
    echo "Git remote '$REMOTE' is not configured." >&2
    exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
    echo "Working tree has uncommitted changes. Commit or stash them first." >&2
    exit 1
fi

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null 2>&1; then
    echo "Tag '$TAG' already exists locally." >&2
    exit 1
fi

rc=0
git ls-remote --exit-code --tags "$REMOTE" "refs/tags/$TAG" >/dev/null 2>&1 || rc=$?
if [ "$rc" -eq 0 ]; then
    echo "Tag '$TAG' already exists on '$REMOTE'." >&2
    exit 1
elif [ "$rc" -ne 2 ]; then
    echo "Could not query remote '$REMOTE' (git ls-remote exited $rc)." >&2
    exit 1
fi

sync_macos_signing_secrets

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[dry-run] Branch: $BRANCH -> remote: $REMOTE"
    if [ "$CUR_VERSION" != "$VERSION" ]; then
        echo "[dry-run] Update CMakeLists.txt version: $CUR_VERSION -> $VERSION"
        echo "[dry-run] Commit: Release $TAG"
    else
        echo "[dry-run] CMakeLists.txt already at $VERSION (no version bump commit needed)"
    fi
    echo "[dry-run] Create annotated tag: $TAG"
    echo "[dry-run] Push: git push --atomic $REMOTE $BRANCH refs/tags/$TAG"
    exit 0
fi

if [ "$CUR_VERSION" != "$VERSION" ]; then
    TMP_FILE="$(mktemp "${TMPDIR:-/tmp}/CMakeLists.XXXXXX")"
    trap 'rm -f "$TMP_FILE"' EXIT
    awk -v new_ver="$VERSION" '
        !done && /^[[:space:]]*project\(/ { in_proj = 1 }
        !done && in_proj && /VERSION[[:space:]]+[0-9]+(\.[0-9]+)+/ {
            sub(/VERSION[[:space:]]+[0-9]+(\.[0-9]+)+/, "VERSION " new_ver)
            done = 1
        }
        in_proj && /\)/ { in_proj = 0 }
        { print }
    ' CMakeLists.txt > "$TMP_FILE"
    mv "$TMP_FILE" CMakeLists.txt
    trap - EXIT

    UPDATED="$(current_version)"
    if [ "$UPDATED" != "$VERSION" ]; then
        echo "Failed to update version in CMakeLists.txt." >&2
        exit 1
    fi

    git add CMakeLists.txt
    git commit -m "Release $TAG"
fi

git tag -a "$TAG" -m "Release $TAG"
git push --atomic "$REMOTE" "$BRANCH" "refs/tags/$TAG"

echo "Released $TAG. GitHub Actions Release CI is now running."
