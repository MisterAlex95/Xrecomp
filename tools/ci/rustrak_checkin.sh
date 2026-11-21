#!/bin/sh
# Sends one Sentry cron check-in to the infra project of the maintainers' Rustrak (XBA-343): the server then shows the
# job in Crons and opens an issue when a run is missed or reports an error.
#
#   tools/ci/rustrak_checkin.sh <monitor-slug> <ok|error|in_progress> [crontab] [margin-minutes]
#
# The DSN comes from XBA_INFRA_DSN (a CI secret / the host's environment, never committed: the app's DSN is public,
# this one must not be, or anyone could post check-ins). Without it the script does nothing and exits 0, so forks
# and local runs are unaffected. The crontab (UTC) creates or updates the monitor on the server. A failed post is
# reported on stderr and never fails the caller.
set -u
slug=${1:?monitor slug}
status=${2:?ok, error or in_progress}
schedule=${3:-}
margin=${4:-30}
dsn=${XBA_INFRA_DSN:-}
[ -n "$dsn" ] || exit 0

# https://KEY@HOST/PROJECT -> the envelope endpoint and the key
key=$(printf '%s' "$dsn" | sed -n 's#^https\{0,1\}://\([0-9a-fA-F]*\)@.*#\1#p')
endpoint=$(printf '%s' "$dsn" | sed -n 's#^\(https\{0,1\}\)://[^@]*@\([^/]*\)\(.*\)/\([0-9][0-9]*\)/\{0,1\}$#\1://\2\3/api/\4/envelope/#p')
if [ -z "$key" ] || [ -z "$endpoint" ]; then
    echo "rustrak_checkin: XBA_INFRA_DSN is not a DSN" >&2
    exit 0
fi

id=$(cat /proc/sys/kernel/random/uuid 2>/dev/null || date +%s%N)
id=$(printf '%s' "$id" | tr -d '-')
now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
config=""
if [ -n "$schedule" ]; then
    config=",\"monitor_config\":{\"schedule\":{\"type\":\"crontab\",\"value\":\"$schedule\"},\"checkin_margin\":$margin,\"timezone\":\"UTC\"}"
fi
payload="{\"check_in_id\":\"$id\",\"monitor_slug\":\"$slug\",\"status\":\"$status\",\"environment\":\"production\"$config}"
len=$(printf '%s' "$payload" | wc -c | tr -d ' ')

code=$(printf '{"sent_at":"%s"}\n{"type":"check_in","length":%s}\n%s\n' "$now" "$len" "$payload" |
    curl -s -o /dev/null -w '%{http_code}' --max-time 15 \
        -H 'Content-Type: application/x-sentry-envelope' \
        -H "X-Sentry-Auth: Sentry sentry_version=7, sentry_key=$key, sentry_client=xrecomp-ci/1" \
        --data-binary @- "$endpoint") || code=000
case $code in
2??) ;;
*) echo "rustrak_checkin: $slug $status: HTTP $code" >&2 ;;
esac
exit 0
