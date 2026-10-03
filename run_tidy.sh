#!/usr/bin/env bash

set -euo pipefail

# Lint with clang-tidy using the repo .clang-tidy config. Mirrors format_code.sh
# conventions: version probe highest-first, changed-files-by-default, -a for
# everything, third-party excludes. Requires a build tree with
# compile_commands.json (every tree configured since CMAKE_EXPORT_COMPILE_COMMANDS
# has one; plain `cmake -S . -B build` provides it).
#
# Only translation units (.cpp/.cc/.cxx) are passed to clang-tidy — with -p a
# header argument cannot be resolved to a compile command. Headers are checked
# via the TUs that include them; changed headers are mapped to including TUs by
# basename. Positional args allow scoped runs (e.g. `./run_tidy.sh src/http`).

CLANG_TIDY=""
RUNNER=""
APPLY=""
CHECK_ALL=0
FORMAT_AND_FIX=0
BUILD_DIR="build"
JOBS="$(nproc)"
if (( JOBS > 12 )); then
    JOBS=12
fi

TU_PATHS=(
    "*.cpp"
    "*.cc"
    "*.cxx"
    ":(glob,exclude)**/third_party/**"
    ":(glob,exclude)**/3rdparty/**"
    ":(glob,exclude)**/vendor/**"
    ":(glob,exclude)**/external/**"
    ":(glob,exclude)**/deps/**"
)
HDR_PATHS=(
    "*.h"
    "*.hh"
    "*.hpp"
    "*.hxx"
    ":(glob,exclude)**/third_party/**"
    ":(glob,exclude)**/3rdparty/**"
    ":(glob,exclude)**/vendor/**"
    ":(glob,exclude)**/external/**"
    ":(glob,exclude)**/deps/**"
)

usage() {
    cat <<'EOF'
Usage: ./run_tidy.sh [-a] [-f] [-p build-dir] [-j jobs] [file or dir ...]

Checks changed project translation units with clang-tidy (mirrors format_code.sh).
Headers cannot be checked directly; changed headers fan out to the .cpp files
that include them. Positional files/dirs replace the changed-file selection.

Options:
  -a         Check all tracked project translation units (src, apps, tests,
             example, fuzz).
  -f         Apply fixes, run ./format_code.sh over the result, then re-check;
             exits 1 if unfixable findings remain.
  -p DIR     Build directory containing compile_commands.json (default: build).
  -j N       Parallel jobs (default: min(nproc, 12)).

Requires clang-tidy >= 17 and a build tree configured with Clang
(SelectToolchain.cmake picks clang automatically).

Exit codes: 0 = clean, 1 = findings remain, 2 = environment/config error.
EOF
}

while getopts ":afp:j:h" opt; do
    case "${opt}" in
        a)
            CHECK_ALL=1
            ;;
        f)
            FORMAT_AND_FIX=1
            ;;
        p)
            BUILD_DIR="${OPTARG}"
            ;;
        j)
            JOBS="${OPTARG}"
            ;;
        h)
            usage
            exit 0
            ;;
        \?)
            echo "run_tidy.sh: unknown option: -${OPTARG}" >&2
            usage >&2
            exit 2
            ;;
    esac
done
shift $((OPTIND - 1))

for version in 20 19 18 17; do
    if command -v "clang-tidy-${version}" >/dev/null 2>&1; then
        CLANG_TIDY="clang-tidy-${version}"
        break
    fi
done

if [[ -z "${CLANG_TIDY}" ]]; then
    if command -v clang-tidy >/dev/null 2>&1; then
        CLANG_TIDY="clang-tidy"
    else
        echo "run_tidy.sh: no clang-tidy binary found on PATH (need >= 17)" >&2
        exit 2
    fi
fi

# A bare clang-tidy may resolve to an LLVM too old to parse the C++23 compile
# db (this repo's toolchain floor is Clang 17); enforce it for every probe.
TIDY_MAJOR="$( ("${CLANG_TIDY}" --version 2>/dev/null || true) | grep -oE '[0-9]+\.[0-9]+' | head -1 | cut -d. -f1 || true)"
if [[ -z "${TIDY_MAJOR}" ]] || (( TIDY_MAJOR < 17 )); then
    echo "run_tidy.sh: ${CLANG_TIDY} is older than 17 and cannot analyze the C++23 compile db" >&2
    exit 2
fi

for version in 20 19 18 17; do
    if command -v "run-clang-tidy-${version}" >/dev/null 2>&1; then
        RUNNER="run-clang-tidy-${version}"
        break
    fi
done
if [[ -z "${RUNNER}" ]] && command -v run-clang-tidy >/dev/null 2>&1; then
    RUNNER="run-clang-tidy"
fi
# Preferred: run-clang-tidy dedups conflicting fixes to the same header across
# TUs (src shares headers heavily). Without it, -f falls back to sequential
# per-file fixing, which is safe but cannot merge header fixes.
if (( FORMAT_AND_FIX == 1 )); then
    for version in 20 19 18 17; do
        if command -v "clang-apply-replacements-${version}" >/dev/null 2>&1; then
            APPLY="clang-apply-replacements-${version}"
            break
        fi
    done
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
ORIG_DIR="$(pwd)"
cd "${REPO_ROOT}"

declare -A seen=()
files=()

is_tu() {
    case "$1" in
        *.cpp|*.cc|*.cxx) return 0 ;;
        *) return 1 ;;
    esac
}

is_third_party() {
    case "$1" in
        */third_party/*|*/3rdparty/*|*/vendor/*|*/external/*|*/deps/*) return 0 ;;
        *) return 1 ;;
    esac
}

add_tu() {
    local path="$1"
    if [[ -f "${path}" ]] && is_tu "${path}" && ! is_third_party "${path}" && [[ -z "${seen[${path}]+x}" ]]; then
        seen["${path}"]=1
        files+=("${path}")
    fi
}

# A changed header is checked via every TU that includes it. Includes are
# relative paths like "../event/EventLoop.h", so basename matching is the
# reliable key. Warn (but proceed) when one header fans out very widely.
add_header_fanout() {
    local header="$1"
    local base count tu
    base="$(basename "${header}")"
    count=0
    while IFS= read -r -d '' tu; do
        add_tu "${tu}"
        count=$((count + 1))
    done < <(grep -rlFZ --include='*.cpp' --include='*.cc' --include='*.cxx' -- "${base}" src apps tests example fuzz 2>/dev/null || true)
    if (( count > 50 )); then
        echo "run_tidy.sh: note: ${header} fans out to ${count} TU(s)" >&2
    fi
}

if (( $# > 0 )); then
    # Explicit files/dirs replace the changed-file selection.
    for arg in "$@"; do
        local_path="${arg}"
        if [[ "${local_path}" != /* ]]; then
            local_path="${ORIG_DIR}/${local_path}"
        fi
        local_path="$(realpath -m "${local_path}")"
        if [[ -d "${local_path}" ]]; then
            while IFS= read -r -d '' tu; do
                add_tu "${tu}"
            done < <(find "${local_path}" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \) -print0)
        else
            add_tu "${local_path}"
        fi
    done
elif (( CHECK_ALL == 1 )); then
    while IFS= read -r -d '' path; do
        add_tu "${REPO_ROOT}/${path}"
    done < <(git ls-files -z -- "${TU_PATHS[@]}")
else
    while IFS= read -r -d '' path; do
        add_tu "${REPO_ROOT}/${path}"
    done < <(git diff --name-only -z --diff-filter=ACMRT HEAD -- "${TU_PATHS[@]}")
    while IFS= read -r -d '' path; do
        add_tu "${REPO_ROOT}/${path}"
    done < <(git ls-files --others --exclude-standard -z -- "${TU_PATHS[@]}")
fi

# Untracked files need the same treatment as diffed ones in the default mode.
if (( $# == 0 && CHECK_ALL == 0 )); then
    while IFS= read -r -d '' path; do
        add_header_fanout "${REPO_ROOT}/${path}"
    done < <( { git diff --name-only -z --diff-filter=ACMRT HEAD -- "${HDR_PATHS[@]}"; git ls-files --others --exclude-standard -z -- "${HDR_PATHS[@]}"; } )
fi

if (( ${#files[@]} == 0 )); then
    if (( CHECK_ALL == 1 )); then
        echo "run_tidy.sh: no project translation units to check"
    else
        echo "run_tidy.sh: no changed project translation units to check"
    fi
    exit 0
fi

# Locate the compile database. A relative -p is resolved against the caller's
# cwd first, then the repo root.
DB=""
for candidate in "${ORIG_DIR}/${BUILD_DIR}" "${REPO_ROOT}/${BUILD_DIR}"; do
    if [[ -f "${candidate}/compile_commands.json" ]]; then
        DB="${candidate}/compile_commands.json"
        break
    fi
done
if [[ -z "${DB}" ]]; then
    echo "run_tidy.sh: no compile_commands.json under '${BUILD_DIR}' — configure a build tree first (cmake -S . -B ${BUILD_DIR})" >&2
    exit 2
fi
BUILD_DIR_ABS="$(dirname "${DB}")"

# Guard: a GCC-configured db can carry driver flags clang-tidy cannot parse;
# SelectToolchain.cmake prefers Clang, so reconfiguring fixes it.
db_compiler="$( (grep -m1 -o '"command": *\[\? *"[^"]*"' "${DB}" || true) | head -1 | sed 's/.*"\([^"]*\)"$/\1/' | awk '{print $1}')"
db_compiler_base="$(basename "${db_compiler:-unknown}")"
case "${db_compiler_base}" in
    g++*|gcc*|*-g++*|*-gcc*)
        echo "run_tidy.sh: compile db in ${BUILD_DIR_ABS} was configured with ${db_compiler_base};" \
             "reconfigure with Clang (default via cmake -S . -B ${BUILD_DIR})" >&2
        exit 2
        ;;
esac
db_major="$(printf '%s' "${db_compiler_base}" | grep -oE '[0-9]+' | tail -1 || true)"
if [[ -n "${db_major}" && "${db_major}" != "${TIDY_MAJOR}" ]]; then
    echo "run_tidy.sh: warning: db compiler ${db_compiler_base} (major ${db_major}) differs from ${CLANG_TIDY} (major ${TIDY_MAJOR}); mismatched versions may misparse" >&2
fi

# Membership pre-filter: TUs missing from the db (brand-new files before a
# reconfigure, stale GLOB) would otherwise be silently skipped.
declare -A db_set=()
while IFS= read -r db_file; do
    db_set["${db_file}"]=1
done < <(grep -oE '"file": *"[^"]*"' "${DB}" | sed 's/^"file": *"//; s/"$//')

present=()
dropped=0
for f in "${files[@]}"; do
    if [[ -n "${db_set[${f}]+x}" ]]; then
        present+=("${f}")
    else
        dropped=$((dropped + 1))
    fi
done
if (( dropped > 0 )); then
    echo "run_tidy.sh: ${dropped} file(s) not in compile db (configure/build the tree first?) — skipping them" >&2
fi
if (( ${#present[@]} == 0 )); then
    echo "run_tidy.sh: no requested translation units found in the compile db" >&2
    exit 2
fi
files=("${present[@]}")

printf 'run_tidy.sh: using %s\n' "$("${CLANG_TIDY}" --version | head -1)"
printf 'run_tidy.sh: checking %d translation unit(s) at -j%s (db: %s)\n' "${#files[@]}" "${JOBS}" "${BUILD_DIR_ABS}"

LOG="${BUILD_DIR_ABS}/tidy.log"

run_check() {
    if [[ -n "${RUNNER}" ]]; then
        "${RUNNER}" -p "${BUILD_DIR_ABS}" \
            -clang-tidy-binary "$(command -v "${CLANG_TIDY}")" \
            -j "${JOBS}" -quiet -- "${files[@]}"
    else
        printf '%s\n' "${files[@]}" | xargs -P "${JOBS}" -n 1 \
            "${CLANG_TIDY}" -p="${BUILD_DIR_ABS}" -quiet
    fi
}

summarize() {
    local log="$1"
    local warnings errors
    warnings="$( (grep -cE ': warning: ' "${log}" || true) )"
    errors="$( (grep -cE ': error: ' "${log}" || true) )"
    echo "==== clang-tidy summary: ${warnings} warning(s), ${errors} error(s) ===="
    if (( warnings > 0 )); then
        echo "-- per check --"
        grep -E ': warning: .* \[[^]]+\]$' "${log}" | grep -oE '\[[^]]+\]$' | tr -d '[]' | sort | uniq -c | sort -rn | head -25 || true
        echo "-- top files --"
        grep -E ': warning: ' "${log}" | cut -d: -f1 | sort | uniq -c | sort -rn | head -10 || true
    fi
    echo "full log: ${log}"
}

if (( FORMAT_AND_FIX == 1 )); then
    FIX_LOG="${BUILD_DIR_ABS}/tidy-fix.log"
    echo "run_tidy.sh: fixing (log: ${FIX_LOG})"
    if [[ -n "${RUNNER}" ]]; then
        runner_fix_args=(-p "${BUILD_DIR_ABS}" -clang-tidy-binary "$(command -v "${CLANG_TIDY}")" -j "${JOBS}" -quiet -fix)
        if [[ -n "${APPLY}" ]]; then
            runner_fix_args+=(-clang-apply-replacements-binary "$(command -v "${APPLY}")")
        fi
        runner_fix_args+=(-- "${files[@]}")
        set +e
        "${RUNNER}" "${runner_fix_args[@]}" 2>&1 | tee "${FIX_LOG}"
        fix_rc=${PIPESTATUS[0]}
        set -e
    else
        echo "run_tidy.sh: no run-clang-tidy found — fixing sequentially per file (no cross-TU header merge)" >&2
        set +e
        : > "${FIX_LOG}"
        for f in "${files[@]}"; do
            "${CLANG_TIDY}" -p="${BUILD_DIR_ABS}" -quiet -fix -- "$f" 2>&1 | tee -a "${FIX_LOG}"
        done
        fix_rc=0
        set -e
    fi
    if (( fix_rc != 0 )); then
        summarize "${FIX_LOG}"
        echo "run_tidy.sh: fix pass failed (environment/config error)" >&2
        exit 2
    fi
    "${REPO_ROOT}/format_code.sh"
fi

set +e
run_check 2>&1 | tee "${LOG}"
check_rc=${PIPESTATUS[0]}
set -e

summarize "${LOG}"

if (( check_rc != 0 )); then
    echo "run_tidy.sh: clang-tidy invocation failed (environment/config error)" >&2
    exit 2
fi
final_errors="$( (grep -cE ': error: ' "${LOG}" || true) )"
final_warnings="$( (grep -cE ': warning: ' "${LOG}" || true) )"
if (( final_errors > 0 )); then
    echo "run_tidy.sh: frontend errors present — fix the environment/config before trusting the counts" >&2
    exit 2
fi
if (( final_warnings > 0 )); then
    exit 1
fi
exit 0
