# scripts/prepare.sh
set -eu
cd -- "$(dirname -- "$0")/.."
umask 077
if [ ! -f .env ]; then
  task_token=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
  task_host=$(hostname -f 2>/dev/null || hostname)
  printf 'MONITOR_API_TOKEN=%s\nMONITOR_HOST_NAME=%s\nMONITOR_BIND_ADDRESS=127.0.0.1\nMONITOR_PORT=8080\nMONITOR_WARN_HOST_DIR=./warn\n' "$task_token" "$task_host" > .env
fi
mkdir -p warn
if [ "$(id -u)" -eq 0 ]; then
  chown 10001:10001 warn
  chmod 0750 warn
else
  sudo chown 10001:10001 warn
  sudo chmod 0750 warn
fi
printf 'Prepared .env and project/warn. Run docker compose up -d --build.\n'
