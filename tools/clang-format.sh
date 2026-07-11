#!/usr/bin/env bash

set -euo pipefail

usage()
{
    printf 'usage: tools/clang-format.sh format|check\n' >&2
}

if (( $# != 1 )); then
    usage
    exit 2
fi

case "$1" in
    format|check) mode="$1" ;;
    *)
        usage
        exit 2
        ;;
esac

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
version_file="$repo_root/.clang-format-version"

if [[ ! -r "$version_file" ]]; then
    printf 'clang-format version file not found: %s\n' "$version_file" >&2
    exit 1
fi

expected_version="$(<"$version_file")"
if [[ -z "$expected_version" ]]; then
    printf 'clang-format version file is empty: %s\n' "$version_file" >&2
    exit 1
fi

default_formatter='/opt/homebrew/opt/llvm/bin/clang-format'
if [[ -n "${CLANG_FORMAT:-}" ]]; then
    formatter_candidate="$CLANG_FORMAT"
elif [[ -x "$default_formatter" ]]; then
    formatter_candidate="$default_formatter"
else
    formatter_candidate='clang-format'
fi

if ! formatter="$(command -v -- "$formatter_candidate" 2>/dev/null)"; then
    printf 'clang-format executable not found: %s\n' "$formatter_candidate" >&2
    exit 1
fi

actual_version="$("$formatter" --version 2>&1)"
if [[ "$actual_version" != *"$expected_version"* ]]; then
    printf 'clang-format version mismatch: expected %s, got %s\n' "$expected_version" "$actual_version" >&2
    exit 1
fi

patterns=('*.c' '*.cc' '*.cpp' '*.cppm' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx')
files=()
while IFS= read -r -d '' file; do
    files+=("$file")
done < <(git -C "$repo_root" ls-files -z --cached --others --exclude-standard -- "${patterns[@]}")

if (( ${#files[@]} == 0 )); then
    exit 0
fi

cd -- "$repo_root"
if [[ "$mode" == 'format' ]]; then
    "$formatter" --style=file -i -- "${files[@]}"
else
    "$formatter" --style=file --dry-run --Werror -- "${files[@]}"
fi
