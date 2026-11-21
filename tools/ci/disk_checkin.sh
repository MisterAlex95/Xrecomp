#!/bin/sh
# The CI host's disk, watched from Rustrak (XBA-343). Run from the host's cron every 30 minutes:
#
#   */30 * * * * root XBA_INFRA_DSN=... /path/to/tools/ci/disk_checkin.sh / /var/lib/docker
#
# Each run sends one check-in to monitor « ci-host-disk »: ok while every given mount is below the threshold
# (XBA_DISK_MAX_PERCENT, default 85), error past it, so the issue opens while there is still room for Rustrak (which
# lives on that disk) to record it. A missed check-in also opens one (the host or its cron is down).
set -u
here=$(cd "$(dirname "$0")" && pwd)
max=${XBA_DISK_MAX_PERCENT:-85}
[ $# -gt 0 ] || set -- /
status=ok
for m in "$@"; do
    used=$(df -P "$m" 2>/dev/null | awk 'NR == 2 { sub("%", "", $5); print $5 }')
    [ -n "$used" ] || continue
    if [ "$used" -ge "$max" ]; then
        status=error
        echo "disk_checkin: $m at $used % (max $max %)" >&2
    fi
done
exec "$here/rustrak_checkin.sh" ci-host-disk "$status" '*/30 * * * *' 20
