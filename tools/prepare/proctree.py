"""
A child process and everything it starts, killed together.

`cmake --build` runs ninja (or make), which runs the compilers: killing cmake
alone leaves them running, reparented, until the build is done. A child
started with popen_kw() heads a process group of its own (POSIX: a new
session, so Linux, macOS and Android alike; Windows: a new process group),
and kill_tree() kills that whole group (POSIX: killpg; Windows: taskkill /T,
which follows the parent links).
"""

import os
import signal
import subprocess


def popen_kw():
    """The Popen keywords that put the child at the head of its own group."""
    if os.name == "nt":
        return {"creationflags": getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0x200)}
    return {"start_new_session": True}


def kill_tree(p):
    """Kill Popen p, started with popen_kw(), and every process it started
    that is still in its group. Never raises; does not wait for p."""
    if os.name == "nt":
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        except (OSError, subprocess.SubprocessError):
            pass
    else:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:             # the group is gone already (or was never p's own)
            pass
    try:
        p.kill()
    except OSError:
        pass
