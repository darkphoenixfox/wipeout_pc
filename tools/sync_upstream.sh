#!/usr/bin/env bash
# Merges upstream's main (AbdelmoumenDeghbouche/wipeout_pc) into this fork's main and runs the
# Python tests (docs/fork.md). Run from the repository; then build, play, and push:
#   git push fork HEAD:main
# Remotes: "origin" (upstream) and "fork" (darkphoenixfox/wipeout_pc), added when missing.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
upstream_url=https://github.com/AbdelmoumenDeghbouche/wipeout_pc.git
fork_url=https://github.com/darkphoenixfox/wipeout_pc.git
remote_of() { git remote -v | awk -v url="$1" '$2 == url && $3 == "(fetch)" {print $1; exit}'; }
upstream=$(remote_of "$upstream_url"); upstream=${upstream:-upstream}
fork=$(remote_of "$fork_url"); fork=${fork:-fork}
git remote get-url "$upstream" > /dev/null 2>&1 || git remote add "$upstream" "$upstream_url"
git remote get-url "$fork" > /dev/null 2>&1 || git remote add "$fork" "$fork_url"
# Conflict resolutions are remembered and replayed in later merges.
git config rerere.enabled true

if [[ -n $(git status --porcelain --untracked-files=no | grep -v ' gpu/third_party/') ]]; then
    echo 'Commit or stash your changes first.' >&2
    exit 1
fi
git fetch "$upstream" main
git fetch "$fork" main
if [[ $(git rev-parse HEAD) != $(git rev-parse "$fork/main") ]]; then
    echo "Check out this fork's main first, e.g.: git switch -c fork-main --track $fork/main" >&2
    exit 1
fi
count=$(git rev-list --count "HEAD..$upstream/main")
if [[ $count == 0 ]]; then
    echo 'main has every upstream commit.'
    exit 0
fi
echo "Merging $count upstream commits:"
git log --oneline "HEAD..$upstream/main"
if ! git merge --no-ff "$upstream/main" -m "Merge upstream main ($(git rev-parse --short "$upstream/main"))"; then
    echo
    echo 'Resolve the conflicts above, then: git add <files> && git commit'
    echo 'Fork values (version, releases) live in launcher/fork_info.py: keep upstream lines as upstream has them.'
    exit 1
fi
python -m unittest discover -s tests -p 'test_*.py' 2>&1 | grep -v '^Patches:' | tail -5
echo
echo "Merged. Build (bash build.sh), play, then push: git push $fork HEAD:main"
