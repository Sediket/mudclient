#!/usr/bin/env python3
"""Orchestrates the replay ctest: starts tests/fake_mud replaying a
recorded session on an ephemeral port, then runs the built mudclient
binary in --test mode against it, and propagates its exit code.

Usage:
    run_replay_test.py --fake-mud <path> --client <path> --record <path>
                        --script <path> [--speed N] [--timeout N]
"""
import argparse
import subprocess
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fake-mud", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--record", required=True)
    parser.add_argument("--script", required=True)
    parser.add_argument("--speed", default="10")
    parser.add_argument("--timeout", default="60")
    args = parser.parse_args()

    server = subprocess.Popen(
        [args.fake_mud, "--record", args.record, "--speed", args.speed],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    assert server.stdout is not None
    port_line = server.stdout.readline().strip()
    if not port_line.startswith("PORT "):
        print(f"run_replay_test: fake_mud did not report a port (got: {port_line!r})", file=sys.stderr)
        server.kill()
        return 1
    port = port_line.split()[1]

    env_overrides = {"MUD_HOST": "127.0.0.1", "MUD_PORT": port}
    import os

    env = dict(os.environ)
    env.update(env_overrides)

    client = subprocess.run(
        [args.client, "--test", args.script, "--timeout", args.timeout],
        env=env,
    )

    # Drain and print the server's remaining output for diagnostics, then
    # ensure it's terminated regardless of how far the replay got.
    try:
        remaining, _ = server.communicate(timeout=5)
        if remaining:
            print(remaining, file=sys.stderr)
    except subprocess.TimeoutExpired:
        server.kill()

    return client.returncode


if __name__ == "__main__":
    sys.exit(main())
