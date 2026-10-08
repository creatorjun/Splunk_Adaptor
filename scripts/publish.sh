# scripts/publish.sh
set -eu
cd -- "$(dirname -- "$0")/.."
task_repository=https://github.com/creatorjun/Splunk_Adaptor.git
task_message=${1:-"Update resource monitor"}
if [ ! -d .git ]; then
  git init -b main
fi
if git remote get-url origin >/dev/null 2>&1; then
  if [ "$(git remote get-url origin)" != "$task_repository" ]; then
    printf 'The origin remote does not match this project.\n' >&2
    exit 1
  fi
else
  git remote add origin "$task_repository"
fi
git add --all
if ! git diff --cached --quiet; then
  git commit -m "$task_message"
fi
task_branch=$(git branch --show-current)
if [ -z "$task_branch" ]; then
  printf 'A named branch is required.\n' >&2
  exit 1
fi
git push -u origin "$task_branch"
