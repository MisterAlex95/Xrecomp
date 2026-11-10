#!/bin/sh
# One heavy CI job at a time on the shared Docker host (XBA-53).
#   tools/ci/heavy_lock.sh acquire   # waits until no other heavy job holds the lock, then takes it
#   tools/ci/heavy_lock.sh release   # gives it back (only if this job holds it)
#
# Why not `concurrency:`: Gitea follows GitHub's rule of one running and ONE pending job per group; a third job
# entering the group cancels the one already waiting (getConcurrencyEntriesToReplace in Gitea's models/actions).
# With a main push chaining two heavy jobs while a PR waits, that would cancel PR jobs routinely. This lock queues
# instead: nobody is cancelled, jobs wait (not FIFO, polling).
#
# The lock is a created, never started container named $XBA_LOCK_NAME on the host's Docker: names are unique, so
# `docker create --name` is an atomic test-and-set, and it costs nothing. Its labels say who holds it. A job that
# died without releasing (cancelled run, runner restart) is detected when its job container is gone; when the holder
# cannot be identified, the lock expires after $XBA_LOCK_MAX_AGE seconds.
set -eu

NAME=${XBA_LOCK_NAME:-xba-ci-heavy}
IMAGE=${XBA_LOCK_IMAGE:-docker:29.8.1-cli}   # the job containers' image: always on the host
MAX_AGE=${XBA_LOCK_MAX_AGE:-14400}
POLL=${XBA_LOCK_POLL:-30}
TOKEN="${GITHUB_REPOSITORY:-local}#${GITHUB_RUN_ID:-0}#${GITHUB_JOB:-job}#${GITHUB_RUN_ATTEMPT:-1}"

# This job's own container on the host (the job runs in a container of the host's Docker; its hostname is the
# container id), so that waiters can tell whether the holder is still alive. Empty if it cannot be found.
self=$(hostname 2>/dev/null || true)
if [ -z "$self" ] || ! docker inspect --type container "$self" >/dev/null 2>&1; then self=""; fi

label() { docker inspect --type container -f "{{ index .Config.Labels \"$2\" }}" "$1" 2>/dev/null || true; }

stale() {   # $1: lock container id; true when its holder is gone or it is too old
    holder=$(label "$1" xba.holder)
    if [ -n "$holder" ]; then
        [ "$(docker inspect --type container -f '{{.State.Running}}' "$holder" 2>/dev/null || echo false)" != true ]
        return
    fi
    since=$(label "$1" xba.since)
    [ -n "$since" ] && [ $(( $(date +%s) - since )) -gt "$MAX_AGE" ]
}

case "${1:-}" in
acquire)
    start=$(date +%s); last=0; errors=0
    while ! err=$(docker create --name "$NAME" --label "xba.token=$TOKEN" --label "xba.holder=$self" \
              --label "xba.since=$(date +%s)" "$IMAGE" true 2>&1 >/dev/null); do
        id=$(docker inspect --type container -f '{{.Id}}' "$NAME" 2>/dev/null || true)
        if [ -z "$id" ]; then                        # released in between, or Docker itself failed
            errors=$((errors + 1))
            [ "$errors" -lt 5 ] || { echo "heavy lock: cannot create $NAME: $err" >&2; exit 1; }
            sleep 2; continue
        fi
        if stale "$id"; then
            echo "heavy lock: $(label "$id" xba.token) is gone or too old, removing its lock"
            docker rm "$id" >/dev/null 2>&1 || true  # by id: never removes a lock someone just took
            continue
        fi
        now=$(date +%s)
        if [ $(( now - last )) -ge 300 ]; then
            echo "heavy lock: held by $(label "$id" xba.token), waiting ($(( (now - start) / 60 )) min so far)"
            last=$now
        fi
        sleep "$POLL"
    done
    echo "heavy lock: taken by $TOKEN after $(( ($(date +%s) - start) / 60 )) min${self:+ (holder container $self)}"
    ;;
release)
    id=$(docker inspect --type container -f '{{.Id}}' "$NAME" 2>/dev/null || true)
    if [ -n "$id" ] && [ "$(label "$id" xba.token)" = "$TOKEN" ]; then
        docker rm "$id" >/dev/null && echo "heavy lock: released by $TOKEN"
    else
        echo "heavy lock: not held by $TOKEN, nothing to release"
    fi
    ;;
*)
    echo "usage: $0 acquire|release" >&2; exit 2 ;;
esac
