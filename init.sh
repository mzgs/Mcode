#!/bin/sh
set -eu

# Bootstrap a new project cloned or forked from C-GUI-Template.
# Updates CMakeLists.txt, install.sh, README.md, and AGENTS.md in one step.
# Safe to re-run later to adjust project identity or metadata.

cd -- "$(dirname -- "$0")"

REMOTE="${REMOTE:-origin}"
DRY_RUN=0
ASSUME_YES=0

OPT_NAME=""
OPT_NAME_SET=0
OPT_DESCRIPTION=""
OPT_DESCRIPTION_SET=0
OPT_AUTHOR=""
OPT_AUTHOR_SET=0
OPT_COPYRIGHT=""
OPT_COPYRIGHT_SET=0
OPT_BUNDLE_ID=""
OPT_BUNDLE_ID_SET=0
OPT_REPO=""
OPT_REPO_SET=0
OPT_TARGET=""
OPT_TARGET_SET=0
OPT_PROJECT=""
OPT_PROJECT_SET=0
OPT_VERSION=""
OPT_VERSION_SET=0

usage() {
    cat <<'EOF'
Usage: ./init.sh [options]

Configure app identity and metadata for a new project based on this template.
Without --yes, prompts interactively for any unspecified values using defaults
derived from your git remote, git config, and CMakeLists.txt.

Options:
  --name <name>              Display name (window title, .app name, settings dir)
  --description <text>       Short description (About dialog, OS metadata)
  --author <name>            Author / publisher name (Windows version info)
  --copyright <text>         Optional copyright notice (macOS & Windows metadata)
  --bundle-id <id>           Reverse-DNS app ID (e.g. com.alice.my-app)
  --repo <owner/repo>        GitHub repo for updates & install.sh ("" to disable)
  --target <name>            Executable / CMake target name (e.g. my-app)
  --project <name>           CMake project() name (default: target with _ )
  --version <X.Y.Z>          Starting semantic version (default: 0.1.0)
  -y, --yes                  Accept derived defaults without prompting
  -n, --dry-run              Preview changes without modifying any files
  -h, --help                 Show this help message and exit

Example:
  ./init.sh --name "My Notes" --repo "alice/my-notes" --yes
EOF
}

need_arg() {
    if [ "$#" -lt 2 ]; then
        echo "Missing value for $1" >&2
        exit 2
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        -n|--dry-run)
            DRY_RUN=1
            shift
            ;;
        -y|--yes)
            ASSUME_YES=1
            shift
            ;;
        --name)
            need_arg "$@"
            OPT_NAME="$2"
            OPT_NAME_SET=1
            shift 2
            ;;
        --name=*)
            OPT_NAME="${1#*=}"
            OPT_NAME_SET=1
            shift
            ;;
        --description)
            need_arg "$@"
            OPT_DESCRIPTION="$2"
            OPT_DESCRIPTION_SET=1
            shift 2
            ;;
        --description=*)
            OPT_DESCRIPTION="${1#*=}"
            OPT_DESCRIPTION_SET=1
            shift
            ;;
        --author)
            need_arg "$@"
            OPT_AUTHOR="$2"
            OPT_AUTHOR_SET=1
            shift 2
            ;;
        --author=*)
            OPT_AUTHOR="${1#*=}"
            OPT_AUTHOR_SET=1
            shift
            ;;
        --copyright)
            need_arg "$@"
            OPT_COPYRIGHT="$2"
            OPT_COPYRIGHT_SET=1
            shift 2
            ;;
        --copyright=*)
            OPT_COPYRIGHT="${1#*=}"
            OPT_COPYRIGHT_SET=1
            shift
            ;;
        --bundle-id)
            need_arg "$@"
            OPT_BUNDLE_ID="$2"
            OPT_BUNDLE_ID_SET=1
            shift 2
            ;;
        --bundle-id=*)
            OPT_BUNDLE_ID="${1#*=}"
            OPT_BUNDLE_ID_SET=1
            shift
            ;;
        --repo)
            need_arg "$@"
            OPT_REPO="$2"
            OPT_REPO_SET=1
            shift 2
            ;;
        --repo=*)
            OPT_REPO="${1#*=}"
            OPT_REPO_SET=1
            shift
            ;;
        --target)
            need_arg "$@"
            OPT_TARGET="$2"
            OPT_TARGET_SET=1
            shift 2
            ;;
        --target=*)
            OPT_TARGET="${1#*=}"
            OPT_TARGET_SET=1
            shift
            ;;
        --project)
            need_arg "$@"
            OPT_PROJECT="$2"
            OPT_PROJECT_SET=1
            shift 2
            ;;
        --project=*)
            OPT_PROJECT="${1#*=}"
            OPT_PROJECT_SET=1
            shift
            ;;
        --version)
            need_arg "$@"
            OPT_VERSION="$2"
            OPT_VERSION_SET=1
            shift 2
            ;;
        --version=*)
            OPT_VERSION="${1#*=}"
            OPT_VERSION_SET=1
            shift
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Try './init.sh --help' for usage." >&2
            exit 2
            ;;
    esac
done

if [ ! -f CMakeLists.txt ]; then
    echo "CMakeLists.txt not found in $(pwd)." >&2
    exit 1
fi

github_repo_slug() {
    git remote get-url "$REMOTE" 2>/dev/null | sed -E \
        -e 's#^https?://([^@]+@)?github\.com/##' \
        -e 's#^ssh://git@github\.com(:[0-9]+)?/##' \
        -e 's#^git://github\.com/##' \
        -e 's#^git@github\.com:##' \
        -e 's#/*$##' \
        -e 's#\.git$##' \
        -e 's#/*$##' || true
}

read_cmake_project_name() {
    awk '
        /^[[:space:]]*project\(/ {
            line = $0
            sub(/^[[:space:]]*project\([[:space:]]*/, "", line)
            sub(/[[:space:]].*$/, "", line)
            print line
            exit
        }
    ' CMakeLists.txt
}

read_cmake_project_version() {
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

read_cmake_cache_var() {
    VAR_NAME="$1" awk '
        BEGIN {
            var = ENVIRON["VAR_NAME"]
            pat = "^[[:space:]]*set\\(" var "[[:space:]]+\""
        }
        $0 ~ pat {
            line = $0
            sub(pat, "", line)
            out = ""
            esc = 0
            for (i = 1; i <= length(line); i++) {
                ch = substr(line, i, 1)
                if (esc) {
                    out = out ch
                    esc = 0
                } else if (ch == "\\") {
                    esc = 1
                } else if (ch == "\"") {
                    break
                } else {
                    out = out ch
                }
            }
            print out
            exit
        }
    ' CMakeLists.txt
}

read_cmake_exe_target() {
    awk '
        /^[[:space:]]*add_executable\(/ && /MACOSX_BUNDLE/ {
            line = $0
            sub(/^[[:space:]]*add_executable\([[:space:]]*/, "", line)
            sub(/[[:space:]].*$/, "", line)
            print line
            exit
        }
    ' CMakeLists.txt
}

read_install_repo() {
    if [ ! -f install.sh ]; then
        return 0
    fi
    awk '
        /^REPO="\$\{REPO:-/ {
            line = $0
            sub(/^REPO="\$\{REPO:-/, "", line)
            sub(/\}"$/, "", line)
            print line
            exit
        }
    ' install.sh
}

slugify_kebab() {
    printf '%s' "$1" | LC_ALL=C tr '[:upper:]' '[:lower:]' | LC_ALL=C sed -E \
        -e 's/[^a-z0-9]+/-/g' \
        -e 's/^-+//' \
        -e 's/-+$//'
}

slugify_snake() {
    printf '%s' "$1" | LC_ALL=C tr '[:upper:]' '[:lower:]' | LC_ALL=C sed -E \
        -e 's/[^a-z0-9]+/_/g' \
        -e 's/^_+//' \
        -e 's/_+$//'
}

is_reserved_target() {
    t_lower="$(printf '%s' "$1" | LC_ALL=C tr '[:upper:]' '[:lower:]')"
    case "$t_lower" in
        app_common|app_log|app_platform|app_settings|app_chrome|\
        log_tests|updater_tests|actions_tests|cui|yyjson|\
        run|all|all_build|zero_check|install|test|run_tests|clean|help|\
        edit_cache|rebuild_cache|package|package_source|\
        list_install_components|install_local|install_strip)
            return 0
            ;;
    esac
    return 1
}

title_case_slug() {
    printf '%s' "$1" | tr '_-' '  ' | awk '
        {
            for (i = 1; i <= NF; i++) {
                w = $i
                first = toupper(substr(w, 1, 1))
                rest = substr(w, 2)
                printf "%s%s", (i > 1 ? " " : ""), first rest
            }
        }
    '
}

NL="$(printf '\nx')"
NL="${NL%x}"
CR="$(printf '\r')"

validate_no_newlines() {
    case "$2" in
        *"$NL"*|*"$CR"*)
            echo "Invalid $1: must be a single line." >&2
            exit 2
            ;;
    esac
}

validate_inputs() {
    if [ -z "$NEW_NAME" ]; then
        echo "App name cannot be empty." >&2
        exit 2
    fi
    validate_no_newlines "app name" "$NEW_NAME"
    case "$NEW_NAME" in
        "."|".."|*"/"*|*"\\"*|*":"*|*"*"*|*"?"*|*'"'*|*"<"*|*">"*|*"|"*)
            echo "Invalid app name '$NEW_NAME': cannot be '.'/'..' or contain path/filename characters (/ \\ : * ? \" < > |)." >&2
            exit 2
            ;;
        " "*|*" "|*".")
            echo "Invalid app name '$NEW_NAME': cannot start/end with a space or end with '.'." >&2
            exit 2
            ;;
    esac
    validate_no_newlines "description" "$NEW_DESCRIPTION"
    validate_no_newlines "author" "$NEW_AUTHOR"
    validate_no_newlines "copyright" "$NEW_COPYRIGHT"

    if ! printf '%s\n' "$NEW_TARGET" | grep -Eq '^[A-Za-z][A-Za-z0-9_-]*$'; then
        echo "Invalid target name '$NEW_TARGET'. Use letters, digits, '-' or '_' starting with a letter." >&2
        exit 2
    fi
    if is_reserved_target "$NEW_TARGET"; then
        echo "Target name '$NEW_TARGET' conflicts with a reserved CMake target." >&2
        exit 2
    fi

    if ! printf '%s\n' "$NEW_PROJECT" | grep -Eq '^[A-Za-z][A-Za-z0-9_-]*$'; then
        echo "Invalid CMake project name '$NEW_PROJECT'. Use letters, digits, '-' or '_' starting with a letter." >&2
        exit 2
    fi

    if ! printf '%s\n' "$NEW_BUNDLE_ID" | grep -Eq '^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$'; then
        echo "Invalid bundle ID '$NEW_BUNDLE_ID'. Expected reverse-DNS like 'com.yourname.my-app'." >&2
        exit 2
    fi

    if [ -n "$NEW_REPO" ] && ! printf '%s\n' "$NEW_REPO" | grep -Eq '^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$'; then
        echo "Invalid GitHub repo '$NEW_REPO'. Expected 'owner/repo' or empty string." >&2
        exit 2
    fi

    if ! printf '%s\n' "$NEW_VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
        echo "Invalid version '$NEW_VERSION'. Expected MAJOR.MINOR.PATCH (e.g. 0.1.0)." >&2
        exit 2
    fi
}

CUR_PROJECT="$(read_cmake_project_name)"
CUR_VERSION="$(read_cmake_project_version)"
CUR_NAME="$(read_cmake_cache_var APP_NAME)"
CUR_DESCRIPTION="$(read_cmake_cache_var APP_DESCRIPTION)"
CUR_AUTHOR="$(read_cmake_cache_var APP_AUTHOR)"
CUR_COPYRIGHT="$(read_cmake_cache_var APP_COPYRIGHT)"
CUR_BUNDLE_ID="$(read_cmake_cache_var APP_BUNDLE_ID)"
CUR_REPO="$(read_cmake_cache_var APP_GITHUB_REPO)"
CUR_TARGET="$(read_cmake_exe_target)"
CUR_INSTALL_REPO="$(read_install_repo)"

if [ -z "$CUR_PROJECT" ] || [ -z "$CUR_VERSION" ] || [ -z "$CUR_NAME" ] || [ -z "$CUR_TARGET" ]; then
    echo "Could not parse project settings from CMakeLists.txt." >&2
    exit 1
fi

IS_TEMPLATE_DEFAULTS=0
if [ "$CUR_PROJECT" = "c_gui_template" ] || [ "$CUR_NAME" = "C GUI Template" ]; then
    IS_TEMPLATE_DEFAULTS=1
fi

DETECTED_REPO="$(github_repo_slug)"
if ! printf '%s\n' "$DETECTED_REPO" | grep -Eq '^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$'; then
    DETECTED_REPO=""
fi
if [ "$DETECTED_REPO" = "mzgs/C-GUI-Template" ]; then
    DETECTED_REPO=""
fi

GIT_USER_NAME="$(git config user.name 2>/dev/null || true)"
DIR_BASENAME="$(basename "$(pwd)")"

# Compute smart default app name
DEFAULT_NAME="$CUR_NAME"
if [ "$IS_TEMPLATE_DEFAULTS" -eq 1 ]; then
    if [ -n "$DETECTED_REPO" ]; then
        DEFAULT_NAME="$(title_case_slug "${DETECTED_REPO#*/}")"
    elif [ "$DIR_BASENAME" != "C-GUI-Template" ] && [ -n "$DIR_BASENAME" ]; then
        DEFAULT_NAME="$(title_case_slug "$DIR_BASENAME")"
    fi
fi

# Compute smart default author
DEFAULT_AUTHOR="$CUR_AUTHOR"
if [ "$CUR_AUTHOR" = "Example" ] || [ -z "$CUR_AUTHOR" ]; then
    if [ -n "$GIT_USER_NAME" ]; then
        DEFAULT_AUTHOR="$GIT_USER_NAME"
    elif [ -n "$DETECTED_REPO" ]; then
        DEFAULT_AUTHOR="${DETECTED_REPO%%/*}"
    else
        DEFAULT_AUTHOR="Example"
    fi
fi

# Compute smart default repo
DEFAULT_REPO="$CUR_REPO"
if [ -n "$DETECTED_REPO" ]; then
    DEFAULT_REPO="$DETECTED_REPO"
elif [ "$CUR_REPO" = "mzgs/C-GUI-Template" ]; then
    DEFAULT_REPO=""
fi

# Compute smart default version
if [ "$IS_TEMPLATE_DEFAULTS" -eq 1 ]; then
    DEFAULT_VERSION="0.1.0"
else
    DEFAULT_VERSION="$CUR_VERSION"
fi

prompt_value() {
    label="$1"
    default_val="$2"
    allow_clear="${3:-0}"
    if [ "$allow_clear" -eq 1 ] && [ -n "$default_val" ]; then
        printf '%s [%s] ("-" to clear): ' "$label" "$default_val" >&2
    elif [ -n "$default_val" ]; then
        printf '%s [%s]: ' "$label" "$default_val" >&2
    else
        printf '%s (optional): ' "$label" >&2
    fi
    if ! IFS= read -r reply && [ -z "$reply" ]; then
        printf '\nAborted.\n' >&2
        return 1
    fi
    if [ "$allow_clear" -eq 1 ] && [ "$reply" = "-" ]; then
        printf ''
    elif [ -n "$reply" ]; then
        printf '%s' "$reply"
    else
        printf '%s' "$default_val"
    fi
}

# Resolve fields in dependency order so derived defaults follow earlier answers
if [ "$OPT_NAME_SET" -eq 1 ]; then
    NEW_NAME="$OPT_NAME"
elif [ "$ASSUME_YES" -eq 1 ]; then
    NEW_NAME="$DEFAULT_NAME"
else
    NEW_NAME="$(prompt_value "App display name" "$DEFAULT_NAME" 0)"
fi

if [ "$OPT_DESCRIPTION_SET" -eq 1 ]; then
    NEW_DESCRIPTION="$OPT_DESCRIPTION"
elif [ "$ASSUME_YES" -eq 1 ]; then
    NEW_DESCRIPTION="$CUR_DESCRIPTION"
else
    NEW_DESCRIPTION="$(prompt_value "Short description" "$CUR_DESCRIPTION" 1)"
fi

if [ "$OPT_AUTHOR_SET" -eq 1 ]; then
    NEW_AUTHOR="$OPT_AUTHOR"
elif [ "$ASSUME_YES" -eq 1 ]; then
    NEW_AUTHOR="$DEFAULT_AUTHOR"
else
    NEW_AUTHOR="$(prompt_value "Author / publisher" "$DEFAULT_AUTHOR" 1)"
fi

if [ "$OPT_COPYRIGHT_SET" -eq 1 ]; then
    NEW_COPYRIGHT="$OPT_COPYRIGHT"
elif [ "$ASSUME_YES" -eq 1 ]; then
    NEW_COPYRIGHT="$CUR_COPYRIGHT"
else
    NEW_COPYRIGHT="$(prompt_value "Copyright notice" "$CUR_COPYRIGHT" 1)"
fi

if [ "$OPT_REPO_SET" -eq 1 ]; then
    NEW_REPO="$OPT_REPO"
elif [ "$ASSUME_YES" -eq 1 ]; then
    NEW_REPO="$DEFAULT_REPO"
else
    NEW_REPO="$(prompt_value "GitHub repo (owner/repo)" "$DEFAULT_REPO" 1)"
fi

# Derive target default from NEW_NAME
DERIVED_TARGET="$(slugify_kebab "$NEW_NAME")"
case "$DERIVED_TARGET" in
    ""|[0-9]*) DERIVED_TARGET="app-${DERIVED_TARGET:-gui}" ;;
esac
if is_reserved_target "$DERIVED_TARGET"; then
    DERIVED_TARGET="${DERIVED_TARGET}-app"
fi
if [ "$IS_TEMPLATE_DEFAULTS" -eq 0 ] && [ "$NEW_NAME" = "$CUR_NAME" ]; then
    DEFAULT_TARGET="$CUR_TARGET"
else
    DEFAULT_TARGET="$DERIVED_TARGET"
fi

if [ "$OPT_TARGET_SET" -eq 1 ]; then
    NEW_TARGET="$OPT_TARGET"
elif [ "$ASSUME_YES" -eq 1 ]; then
    NEW_TARGET="$DEFAULT_TARGET"
else
    NEW_TARGET="$(prompt_value "Executable / CMake target" "$DEFAULT_TARGET" 0)"
fi

# Derive CMake project() name from NEW_TARGET
DERIVED_PROJECT="$(slugify_snake "$NEW_TARGET")"
case "$DERIVED_PROJECT" in
    ""|[0-9]*) DERIVED_PROJECT="app_${DERIVED_PROJECT:-gui}" ;;
esac
if [ "$IS_TEMPLATE_DEFAULTS" -eq 0 ] && [ "$NEW_TARGET" = "$CUR_TARGET" ]; then
    DEFAULT_PROJECT="$CUR_PROJECT"
else
    DEFAULT_PROJECT="$DERIVED_PROJECT"
fi
if [ "$OPT_PROJECT_SET" -eq 1 ]; then
    NEW_PROJECT="$OPT_PROJECT"
elif [ "$ASSUME_YES" -eq 1 ]; then
    NEW_PROJECT="$DEFAULT_PROJECT"
else
    NEW_PROJECT="$(prompt_value "CMake project name" "$DEFAULT_PROJECT" 0)"
fi

# Derive reverse-DNS bundle ID from repo owner / author and target
OWNER_SLUG=""
if [ -n "$NEW_REPO" ]; then
    OWNER_SLUG="$(slugify_kebab "${NEW_REPO%%/*}")"
fi
if [ -z "$OWNER_SLUG" ] && [ -n "$NEW_AUTHOR" ]; then
    OWNER_SLUG="$(slugify_kebab "$NEW_AUTHOR")"
fi
if [ -z "$OWNER_SLUG" ]; then
    OWNER_SLUG="example"
fi
APP_SLUG="$(slugify_kebab "$NEW_TARGET")"
if [ -z "$APP_SLUG" ]; then
    APP_SLUG="app"
fi
DERIVED_BUNDLE_ID="com.${OWNER_SLUG}.${APP_SLUG}"
if [ "$IS_TEMPLATE_DEFAULTS" -eq 0 ] && [ "$NEW_TARGET" = "$CUR_TARGET" ] && [ "$NEW_REPO" = "$CUR_REPO" ] && { [ -n "$NEW_REPO" ] || [ "$NEW_AUTHOR" = "$CUR_AUTHOR" ]; }; then
    DEFAULT_BUNDLE_ID="$CUR_BUNDLE_ID"
else
    DEFAULT_BUNDLE_ID="$DERIVED_BUNDLE_ID"
fi

if [ "$OPT_BUNDLE_ID_SET" -eq 1 ]; then
    NEW_BUNDLE_ID="$OPT_BUNDLE_ID"
elif [ "$ASSUME_YES" -eq 1 ]; then
    NEW_BUNDLE_ID="$DEFAULT_BUNDLE_ID"
else
    NEW_BUNDLE_ID="$(prompt_value "Bundle ID (reverse-DNS)" "$DEFAULT_BUNDLE_ID" 0)"
fi

if [ "$OPT_VERSION_SET" -eq 1 ]; then
    NEW_VERSION="$OPT_VERSION"
elif [ "$ASSUME_YES" -eq 1 ]; then
    NEW_VERSION="$DEFAULT_VERSION"
else
    NEW_VERSION="$(prompt_value "Starting version" "$DEFAULT_VERSION" 0)"
fi

validate_inputs

cat <<EOF

Project configuration:
  App name:      $NEW_NAME
  Description:   ${NEW_DESCRIPTION:-<empty>}
  Author:        ${NEW_AUTHOR:-<empty>}
  Copyright:     ${NEW_COPYRIGHT:-<empty>}
  Bundle ID:     $NEW_BUNDLE_ID
  GitHub repo:   ${NEW_REPO:-<disabled>}
  Target:        $NEW_TARGET
  CMake project: $NEW_PROJECT
  Version:       $NEW_VERSION
EOF

if [ "$DRY_RUN" -eq 1 ]; then
    echo ""
    echo "[dry-run] Would update CMakeLists.txt"
    if [ -f install.sh ]; then
        if [ -n "$NEW_REPO" ]; then
            echo "[dry-run] Would update install.sh (REPO -> $NEW_REPO)"
        elif [ -n "$CUR_INSTALL_REPO" ]; then
            echo "[dry-run] Would update install.sh (clear default REPO)"
        fi
    fi
    if [ -f README.md ]; then
        echo "[dry-run] Would update README.md"
    fi
    if [ -f AGENTS.md ]; then
        echo "[dry-run] Would update AGENTS.md"
    fi
    if [ -f build/CMakeCache.txt ]; then
        echo "[dry-run] Would remove stale build/CMakeCache.txt"
    fi
    exit 0
fi

if [ "$ASSUME_YES" -eq 0 ]; then
    printf '\nApply these changes? [Y/n]: ' >&2
    if ! IFS= read -r confirm && [ -z "$confirm" ]; then
        printf '\nAborted.\n' >&2
        exit 1
    fi
    case "$confirm" in
        ""|[Yy]|[Yy][Ee][Ss]) ;;
        *)
            echo "Aborted."
            exit 0
            ;;
    esac
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/init-app.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

# 1. Update CMakeLists.txt
NEW_PROJECT="$NEW_PROJECT" \
NEW_VERSION="$NEW_VERSION" \
NEW_NAME="$NEW_NAME" \
NEW_DESCRIPTION="$NEW_DESCRIPTION" \
NEW_AUTHOR="$NEW_AUTHOR" \
NEW_COPYRIGHT="$NEW_COPYRIGHT" \
NEW_BUNDLE_ID="$NEW_BUNDLE_ID" \
NEW_REPO="$NEW_REPO" \
OLD_TARGET="$CUR_TARGET" \
NEW_TARGET="$NEW_TARGET" \
awk '
    function cmake_escape(s,    i, ch, out) {
        out = ""
        for (i = 1; i <= length(s); i++) {
            ch = substr(s, i, 1)
            if (ch == "\\" || ch == "\"" || ch == "$") out = out "\\" ch
            else out = out ch
        }
        return out
    }
    function replace_cache_var(line, var, val,    lead, rest, prefix, i, ch, esc, after) {
        lead = ""
        rest = line
        if (match(rest, /^[[:space:]]+/)) {
            lead = substr(rest, 1, RLENGTH)
            rest = substr(rest, RLENGTH + 1)
        }
        prefix = "set(" var " \""
        if (index(rest, prefix) != 1) return line
        rest = substr(rest, length(prefix) + 1)
        esc = 0
        after = ""
        for (i = 1; i <= length(rest); i++) {
            ch = substr(rest, i, 1)
            if (esc) {
                esc = 0
            } else if (ch == "\\") {
                esc = 1
            } else if (ch == "\"") {
                after = substr(rest, i + 1)
                break
            }
        }
        return lead prefix cmake_escape(val) "\"" after
    }
    function replace_literal(s, old_s, new_s,    out, pos, ol, rest) {
        if (old_s == "" || old_s == new_s) return s
        ol = length(old_s)
        out = ""
        rest = s
        while ((pos = index(rest, old_s)) > 0) {
            out = out substr(rest, 1, pos - 1) new_s
            rest = substr(rest, pos + ol)
        }
        return out rest
    }
    function replace_after_prefix(s, regex, old_t, new_t,    pre, rest, ol, after) {
        if (!match(s, regex)) return s
        pre = substr(s, 1, RSTART + RLENGTH - 1)
        rest = substr(s, RSTART + RLENGTH)
        ol = length(old_t)
        if (substr(rest, 1, ol) != old_t) return s
        after = substr(rest, ol + 1, 1)
        if (after ~ /[A-Za-z0-9_.\/-]/) return s
        return pre new_t substr(rest, ol + 1)
    }
    function replace_target_line(line, old_t, new_t) {
        if (old_t == "" || old_t == new_t) return line
        line = replace_literal(line, "$<TARGET_FILE:" old_t ">", "$<TARGET_FILE:" new_t ">")
        line = replace_after_prefix(line, "^[[:space:]]*(add_executable|target_link_libraries|cui_app_target|target_sources|set_target_properties)\\([[:space:]]*", old_t, new_t)
        line = replace_after_prefix(line, "^[[:space:]]*install\\([[:space:]]*TARGETS[[:space:]]+", old_t, new_t)
        line = replace_after_prefix(line, "^[[:space:]]*get_target_property\\([[:space:]]*APP_(WIN32|LINUX)_OUTPUT_NAME[[:space:]]+", old_t, new_t)
        line = replace_after_prefix(line, "^[[:space:]]*set\\([[:space:]]*APP_(WIN32|LINUX)_OUTPUT_NAME[[:space:]]+\"", old_t, new_t)
        if (line ~ /^[[:space:]]*add_custom_target\([[:space:]]*run[[:space:]]/) {
            line = replace_after_prefix(line, "[[:space:]]DEPENDS[[:space:]]+", old_t, new_t)
        }
        return line
    }
    {
        line = $0
        if (in_proj_skip) {
            if (line ~ /\)/) in_proj_skip = 0
            next
        }
        if (!proj_done && line ~ /^[[:space:]]*project\(/) {
            lead = ""
            if (match(line, /^[[:space:]]+/)) lead = substr(line, 1, RLENGTH)
            print lead "project(" ENVIRON["NEW_PROJECT"] " VERSION " ENVIRON["NEW_VERSION"] " LANGUAGES C)"
            proj_done = 1
            if (line !~ /\)/) in_proj_skip = 1
            next
        }
        if (line ~ /^[[:space:]]*set\((APP_NAME|APP_DESCRIPTION|APP_AUTHOR|APP_COPYRIGHT|APP_BUNDLE_ID|APP_GITHUB_REPO)[[:space:]]+"/) {
            line = replace_cache_var(line, "APP_NAME", ENVIRON["NEW_NAME"])
            line = replace_cache_var(line, "APP_DESCRIPTION", ENVIRON["NEW_DESCRIPTION"])
            line = replace_cache_var(line, "APP_AUTHOR", ENVIRON["NEW_AUTHOR"])
            line = replace_cache_var(line, "APP_COPYRIGHT", ENVIRON["NEW_COPYRIGHT"])
            line = replace_cache_var(line, "APP_BUNDLE_ID", ENVIRON["NEW_BUNDLE_ID"])
            line = replace_cache_var(line, "APP_GITHUB_REPO", ENVIRON["NEW_REPO"])
            print line
            next
        }
        line = replace_target_line(line, ENVIRON["OLD_TARGET"], ENVIRON["NEW_TARGET"])
        print line
    }
' CMakeLists.txt > "$TMP_DIR/CMakeLists.txt"
mv "$TMP_DIR/CMakeLists.txt" CMakeLists.txt

# 2. Update install.sh
if [ -f install.sh ]; then
    OLD_REPO="$CUR_INSTALL_REPO" \
    NEW_REPO="$NEW_REPO" \
    awk '
        function replace_all(s, old_s, new_s,    out, pos, ol, rest) {
            if (old_s == "" || old_s == new_s) return s
            ol = length(old_s)
            out = ""
            rest = s
            while ((pos = index(rest, old_s)) > 0) {
                out = out substr(rest, 1, pos - 1) new_s
                rest = substr(rest, pos + ol)
            }
            return out rest
        }
        {
            line = $0
            new_repo = ENVIRON["NEW_REPO"]
            old_repo = ENVIRON["OLD_REPO"]
            if (line ~ /^REPO="\$\{REPO:-.*\}\"$/) {
                print "REPO=\"${REPO:-" new_repo "}\""
                next
            }
            if (new_repo != "") {
                if (old_repo != "") {
                    line = replace_all(line, old_repo, new_repo)
                }
                line = replace_all(line, "<owner>/<repo>", new_repo)
                line = replace_all(line, "(default: none; set REPO=owner/repo)", "(default: " new_repo ")")
            } else if (old_repo != "") {
                line = replace_all(line, "(default: " old_repo ")", "(default: none; set REPO=owner/repo)")
                line = replace_all(line, old_repo, "<owner>/<repo>")
            }
            print line
        }
    ' install.sh > "$TMP_DIR/install.sh"
    chmod 755 "$TMP_DIR/install.sh"
    mv "$TMP_DIR/install.sh" install.sh
fi

# 3. Update README.md
if [ -f README.md ]; then
    OLD_NAME="$CUR_NAME" \
    NEW_NAME="$NEW_NAME" \
    OLD_TARGET="$CUR_TARGET" \
    NEW_TARGET="$NEW_TARGET" \
    OLD_REPO="${CUR_REPO:-$CUR_INSTALL_REPO}" \
    NEW_REPO="$NEW_REPO" \
    awk '
        function replace_all(s, old_s, new_s,    out, pos, ol, rest) {
            if (old_s == "" || old_s == new_s) return s
            ol = length(old_s)
            out = ""
            rest = s
            while ((pos = index(rest, old_s)) > 0) {
                out = out substr(rest, 1, pos - 1) new_s
                rest = substr(rest, pos + ol)
            }
            return out rest
        }
        NR == 1 && /^# / {
            print "# " ENVIRON["NEW_NAME"]
            next
        }
        {
            line = $0
            line = replace_all(line, "build/" ENVIRON["OLD_NAME"] ".app", "build/" ENVIRON["NEW_NAME"] ".app")
            line = replace_all(line, "./build/" ENVIRON["OLD_TARGET"], "./build/" ENVIRON["NEW_TARGET"])
            line = replace_all(line, "build\\Release\\" ENVIRON["OLD_TARGET"] ".exe", "build\\Release\\" ENVIRON["NEW_TARGET"] ".exe")
            line = replace_all(line, "build\\" ENVIRON["OLD_TARGET"] ".exe", "build\\" ENVIRON["NEW_TARGET"] ".exe")
            line = replace_all(line, "`" ENVIRON["OLD_TARGET"] "`", "`" ENVIRON["NEW_TARGET"] "`")
            if (ENVIRON["NEW_REPO"] != "") {
                if (ENVIRON["OLD_REPO"] != "") {
                    line = replace_all(line, "raw.githubusercontent.com/" ENVIRON["OLD_REPO"] "/main/install.sh", "raw.githubusercontent.com/" ENVIRON["NEW_REPO"] "/main/install.sh")
                    line = replace_all(line, "REPO:-" ENVIRON["OLD_REPO"], "REPO:-" ENVIRON["NEW_REPO"])
                }
                line = replace_all(line, "raw.githubusercontent.com/<owner>/<repo>/main/install.sh", "raw.githubusercontent.com/" ENVIRON["NEW_REPO"] "/main/install.sh")
            } else if (ENVIRON["OLD_REPO"] != "") {
                line = replace_all(line, "raw.githubusercontent.com/" ENVIRON["OLD_REPO"] "/main/install.sh", "raw.githubusercontent.com/<owner>/<repo>/main/install.sh")
                line = replace_all(line, "REPO:-" ENVIRON["OLD_REPO"], "REPO:-")
            }
            print line
        }
    ' README.md > "$TMP_DIR/README.md"
    mv "$TMP_DIR/README.md" README.md
fi

# 4. Update AGENTS.md
if [ -f AGENTS.md ]; then
    OLD_TARGET="$CUR_TARGET" \
    NEW_TARGET="$NEW_TARGET" \
    awk '
        function replace_all(s, old_s, new_s,    out, pos, ol, rest) {
            if (old_s == "" || old_s == new_s) return s
            ol = length(old_s)
            out = ""
            rest = s
            while ((pos = index(rest, old_s)) > 0) {
                out = out substr(rest, 1, pos - 1) new_s
                rest = substr(rest, pos + ol)
            }
            return out rest
        }
        {
            print replace_all($0, "cui_app_target(" ENVIRON["OLD_TARGET"] ")", "cui_app_target(" ENVIRON["NEW_TARGET"] ")")
        }
    ' AGENTS.md > "$TMP_DIR/AGENTS.md"
    mv "$TMP_DIR/AGENTS.md" AGENTS.md
fi

# 5. Clear stale CMakeCache.txt so next build picks up updated APP_* defaults
if [ -f build/CMakeCache.txt ]; then
    rm -f build/CMakeCache.txt
    echo ""
    echo "Removed stale build/CMakeCache.txt (build/_deps/ preserved)."
fi

cat <<EOF

Initialized project '$NEW_NAME'.
Next steps:
  1. Replace resources/app.png (1024x1024) and run ./resources/generate_icons.sh on macOS.
  2. Build and run: ./run.sh (macOS) or cmake -S . -B build && cmake --build build
  3. Run tests: ctest --test-dir build --output-on-failure
EOF
