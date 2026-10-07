#!/bin/bash
# Extract the entry for the given version from CHANGELOG.md.
#
# Usage: extract-changelog.sh <version>
#   <version> is a tag name like "v1.2.3" or "v1.2.3beta1"; the leading "v"
#   is stripped before matching. The entry runs from its "## <version>" heading
#   up to (but excluding) the next "## " heading. Exit status is 1 when no
#   matching entry is found.

set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <version>" >&2
  exit 2
fi

changelog="$(dirname "$0")/../CHANGELOG.md"
version="${1#v}"

entry="$(awk -v version="$version" '
  $0 ~ "^## " version "([ \t]|$)" { found = 1; next }
  found && /^## / { exit }
  found { print }
' "$changelog")"

if [ -z "$entry" ]; then
  echo "error: no changelog entry for version ${version}" >&2
  exit 1
fi

printf '%s\n' "$entry"
