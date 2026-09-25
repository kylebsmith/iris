#!/bin/sh
# tools/version-check.sh -- one version everywhere, checked on a release tag.
#
#   sh tools/version-check.sh v0.2.0
#
# The release workflow runs this when a tag is pushed, and nothing else runs
# it: it is the one check in the repository that compares text with text,
# and a version only has to agree at the moment it is released. Five places
# must name the same version:
#   the tag                       v0.2.0 (the leading v is dropped)
#   iris.h                        #define IRIS_VERSION_STRING "0.2.0"
#   library.properties            version=0.2.0
#   CITATION.cff                  version: 0.2.0
#   CHANGELOG.md                  the first heading of the form "## 0.2.0 ..."
# It prints all five and exits 1 unless they agree.
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
changelog=$(sed -n 's/^## \([0-9][0-9.]*[0-9A-Za-z.-]*\).*/\1/p' CHANGELOG.md | head -1)
printf '  tag                   %s\n' "$tag"
printf '  IRIS_VERSION_STRING   %s\n' "$header"
printf '  library.properties    %s\n' "$props"
printf '  CITATION.cff          %s\n' "$cff"
printf '  CHANGELOG.md          %s\n' "$changelog"
for v in "$header" "$props" "$cff" "$changelog"; do
  if [ "$v" != "$tag" ]; then
    echo "FAIL  the versions disagree"
    exit 1
  fi
done
echo "PASS  one version everywhere: $tag"
