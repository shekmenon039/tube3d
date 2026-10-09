#!/bin/sh
# Checks that commit subjects follow Conventional Commits (type(scope)!: summary, at most 100
# characters). Merge commits are skipped.
#   sh tools/check_commits.sh [RANGE]      (default: the last commit, e.g. origin/main..HEAD)
RANGE=${1:-HEAD~1..HEAD}
TYPES='feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert'
shas=$(git rev-list --no-merges "$RANGE") || exit 1
status=0
for sha in $shas; do
  subject=$(git log -1 --format=%s "$sha")
  if ! printf '%s\n' "$subject" | grep -Eq "^($TYPES)(\([a-z0-9._/-]+\))?!?: [^ ].*"; then
    echo "$(git log -1 --format=%h "$sha"): not a conventional commit: $subject"
    status=1
  elif [ "${#subject}" -gt 100 ]; then
    echo "$(git log -1 --format=%h "$sha"): subject longer than 100 characters"
    status=1
  fi
done
[ $status = 0 ] && echo "commit messages: ok"
exit $status
