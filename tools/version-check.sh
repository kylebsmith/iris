#!/bin/sh
# tools/version-check.sh -- one version everywhere, and a release date on a
# release tag.
#
#   sh tools/version-check.sh v0.2.0
#
# The release workflow runs this when a tag is pushed; tests/version_check.sh
# (part of sh build.sh docs) runs it against altered copies of the files. It
# is the one check in the repository that compares text with text, and a
# version only has to agree at the moment it is released. Five places must
# name the same version:
#   the tag                       v0.2.0 (the leading v is dropped)
#   iris.h                        #define IRIS_VERSION_STRING "0.2.0"
#   library.properties            version=0.2.0
#   CITATION.cff                  version: 0.2.0
#   CHANGELOG.md                  the first heading of the form "## 0.2.0 ..."
# That heading ends in "unreleased" or in the release date, YYYY-MM-DD. Off a
# tag either will do. On a tag -- GITHUB_REF_TYPE is tag, as the release
# workflow sets it, or HEAD carries the tag v<version> -- it must be the date,
# and CITATION.cff must give the same date as date-released.
# It prints what it read and exits 1 unless every rule holds.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
tag=${1:-${GITHUB_REF_NAME:-}}
if [ -z "$tag" ]; then
  echo "usage: sh tools/version-check.sh v<version>   (or set GITHUB_REF_NAME)"
  exit 2
fi
tag=${tag#v}
header=$(sed -n 's/^#define IRIS_VERSION_STRING "\(.*\)"$/\1/p' iris.h)
props=$(sed -n 's/^version=//p' library.properties)
cff=$(sed -n 's/^version: *"\{0,1\}\([^"]*\)"\{0,1\} *$/\1/p' CITATION.cff)
released=$(sed -n 's/^date-released: *"\{0,1\}\([^"]*\)"\{0,1\} *$/\1/p' CITATION.cff)
line=$(grep -m1 '^## [0-9]' CHANGELOG.md || true)
changelog=$(printf '%s\n' "$line" | awk '{ print $2 }')
when=$(printf '%s\n' "$line" | awk 'NF == 4 { print $4 }')
on_tag=no
if [ "${GITHUB_REF_TYPE:-}" = tag ]; then on_tag=yes
elif [ -z "${GITHUB_REF_TYPE:-}" ] \
     && [ "$(git describe --exact-match --tags HEAD 2> /dev/null || true)" = "v$tag" ]; then on_tag=yes
fi
printf '  tag                   %s (on a tag: %s)\n' "$tag" "$on_tag"
printf '  IRIS_VERSION_STRING   %s\n' "$header"
printf '  library.properties    %s\n' "$props"
printf '  CITATION.cff          %s, date-released %s\n' "$cff" "${released:-none}"
printf '  CHANGELOG.md          %s, %s\n' "$changelog" "${when:-no date or unreleased}"
for v in "$header" "$props" "$cff" "$changelog"; do
  if [ "$v" != "$tag" ]; then
    echo "FAIL  the versions disagree"
    exit 1
  fi
done
case $when in
  [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) dated=yes ;;
  unreleased) dated=no ;;
  *) echo "FAIL  the CHANGELOG.md heading must end in unreleased or a date, YYYY-MM-DD"; exit 1 ;;
esac
if [ "$on_tag" = yes ]; then
  if [ "$dated" = no ]; then
    echo "FAIL  on a release tag the CHANGELOG.md heading must give the release date"
    exit 1
  fi
  if [ "$released" != "$when" ]; then
    echo "FAIL  on a release tag CITATION.cff's date-released must be the CHANGELOG.md date, $when"
    exit 1
  fi
fi
echo "PASS  one version everywhere: $tag${when:+, $when}"
