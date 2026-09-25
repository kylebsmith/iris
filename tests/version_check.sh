#!/bin/sh
# tests/version_check.sh -- tools/version-check.sh against copies of the
# files it reads, each changed one way.
#
# RUN (from anywhere; it writes only to a temporary directory):
#     sh tests/version_check.sh            or, with the other checks of the
#                                          files that describe the code,
#                                          sh build.sh docs
# The exit status is non-zero if any case gives the wrong answer.
#
# THE RULE BEING CHECKED. Off a release tag the first CHANGELOG.md heading
# may say "unreleased" or give a date; on a tag (GITHUB_REF_TYPE=tag, as the
# release workflow sets it) it must give the release date, and CITATION.cff
# must carry the same date as date-released. Everywhere the tag, iris.h,
# library.properties, CITATION.cff and CHANGELOG.md name one version.
# Every case sets GITHUB_REF_TYPE itself, so a run inside a workflow started
# by a tag sees the same cases as any other.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d 2>/dev/null || mktemp -d -t iris-version)
trap 'rm -rf "$T"' EXIT
V=$(sed -n 's/^#define IRIS_VERSION_STRING "\(.*\)"$/\1/p' "$ROOT/iris.h")
fail=0

# a fresh copy of the five files, as committed
fresh() {
  rm -rf "$T/r"
  mkdir -p "$T/r/tools"
  cp "$ROOT/tools/version-check.sh" "$T/r/tools/"
  for f in iris.h library.properties CITATION.cff CHANGELOG.md; do cp "$ROOT/$f" "$T/r/"; done
}
# heading <text>: the first version heading of CHANGELOG.md becomes
# "## <version> — <text>"
heading() {
  awk -v v="$V" -v t="$1" '!done && /^## [0-9]/ { print "## " v " \342\200\224 " t; done = 1; next } { print }' \
    "$T/r/CHANGELOG.md" > "$T/c" && mv "$T/c" "$T/r/CHANGELOG.md"
}
# released <date>: CITATION.cff gains date-released
released() { grep -v '^date-released:' "$T/r/CITATION.cff" > "$T/c"; echo "date-released: $1" >> "$T/c"; mv "$T/c" "$T/r/CITATION.cff"; }
# expect PASS|FAIL <ref type> <label>
expect() {
  if GITHUB_REF_TYPE=$2 sh "$T/r/tools/version-check.sh" "v$V" > "$T/out" 2>&1; then got=PASS; else got=FAIL; fi
  if [ "$got" = "$1" ]; then
    printf '  PASS  %-58s %s, as it must\n' "$3" "$got"
  else
    printf '  FAIL  %-58s %s, where it must %s\n' "$3" "$got" "$1"
    sed 's/^/        /' "$T/out"
    fail=1
  fi
}

echo "tools/version-check.sh, version $V"
fresh;                                    expect PASS branch "the files as committed, off a tag"
fresh; heading unreleased;                expect PASS branch "an unreleased heading, off a tag"
fresh; heading unreleased;                expect FAIL tag    "an unreleased heading, on a tag"
fresh; heading 2026-10-01;                expect PASS branch "a dated heading, off a tag"
fresh; heading 2026-10-01; released 2026-10-01
                                          expect PASS tag    "a dated heading and the same date-released, on a tag"
fresh; heading 2026-10-01;                expect FAIL tag    "a dated heading and no date-released, on a tag"
fresh; heading 2026-10-01; released 2026-10-02
                                          expect FAIL tag    "a date-released that is not the heading's, on a tag"
fresh; heading soon;                      expect FAIL branch "a heading that is neither a date nor unreleased"
fresh; sed "s/^version=.*/version=$V.1/" "$T/r/library.properties" > "$T/c" && mv "$T/c" "$T/r/library.properties"
                                          expect FAIL branch "library.properties naming another version"
if [ "$fail" = 0 ]; then echo "PASS  every case"; else echo "FAIL  a case gave the wrong answer"; fi
exit "$fail"
