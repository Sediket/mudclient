#!/usr/bin/env python3
"""Standalone, dependency-free capture tool for a real zombiemud.org
session, for use from a location that actually has network access to it
(this development environment's outbound network policy blocks port 3000
to that host, which is exactly why this script exists as something to
run elsewhere and bring the results back from).

Connects once, logs every byte received (with millisecond timestamps,
preserving chunk boundaries) and every line sent, and drives a
conservative login -> move -> quit sequence by watching for quiet gaps in
the server's output rather than trying to pattern-match its exact prompt
text (which isn't known in advance). Produces two files:

  - a JSONL recording in the *exact* format mudclient's own --record
    flag produces ({"data_b64":...,"dir":"in","t_ms":...} /
    {"dir":"out","line":...,"t_ms":...}), so it can be dropped in
    directly as tests/replay/zombiemud_session.jsonl.
  - a plain-text transcript (raw bytes, errors replaced) for easy
    reading and for quoting real lines into NOTES.md.

Usage:
    python3 capture_zombiemud_session.py
    python3 capture_zombiemud_session.py --commands v,east,east,south,quit
    python3 capture_zombiemud_session.py --host zombiemud.org --port 3000

This script makes exactly ONE connection attempt per run. The project
this is for follows a "max 5 connections per milestone, at least 60
seconds apart" budget for the live server -- please don't run this back
to back in a tight loop.
"""
import argparse
import base64
import json
import select
import socket
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="zombiemud.org")
    parser.add_argument("--port", type=int, default=3000)
    parser.add_argument(
        "--commands",
        default="v,east,east,east,east,south,south,south,north,quit",
        help="Comma-separated commands to send, one per quiet gap (default: %(default)s)",
    )
    parser.add_argument(
        "--quiet-ms",
        type=int,
        default=1500,
        help="Milliseconds of no incoming data before sending the next command (default: %(default)s)",
    )
    parser.add_argument(
        "--max-seconds",
        type=float,
        default=90.0,
        help="Hard cap on total session time (default: %(default)s)",
    )
    parser.add_argument("--connect-timeout", type=float, default=10.0)
    parser.add_argument("--out-record", default="zombiemud_session.jsonl")
    parser.add_argument("--out-transcript", default="zombiemud_transcript.txt")
    args = parser.parse_args()

    commands = [c for c in args.commands.split(",") if c != ""]

    print(f"Connecting to {args.host}:{args.port} ...", file=sys.stderr)
    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.connect_timeout)
    except OSError as exc:
        print(f"Connection failed: {exc}", file=sys.stderr)
        return 1
    sock.setblocking(False)
    print("Connected.", file=sys.stderr)

    start = time.monotonic()
    last_data_time = start
    command_index = 0
    quit_sent_at = None
    record_entries = []
    transcript_parts = []

    def elapsed_ms() -> int:
        return int((time.monotonic() - start) * 1000)

    def send_command(cmd: str) -> None:
        nonlocal quit_sent_at
        line = (cmd + "\r\n").encode("utf-8")
        try:
            sock.sendall(line)
        except OSError as exc:
            print(f"Send failed ({cmd!r}): {exc}", file=sys.stderr)
            return
        record_entries.append({"dir": "out", "line": cmd, "t_ms": elapsed_ms()})
        transcript_parts.append(f"\n>>> {cmd}\n")
        print(f"[{elapsed_ms():>7} ms] SENT: {cmd}", file=sys.stderr)
        if cmd.strip().lower() == "quit":
            quit_sent_at = time.monotonic()

    try:
        while True:
            now = time.monotonic()
            if now - start > args.max_seconds:
                print("Max session time reached, stopping.", file=sys.stderr)
                break
            if quit_sent_at is not None and now - quit_sent_at > 5.0:
                print("Sent quit and waited 5s, stopping.", file=sys.stderr)
                break

            readable, _, _ = select.select([sock], [], [], 0.2)
            if readable:
                try:
                    chunk = sock.recv(4096)
                except OSError as exc:
                    print(f"Read error: {exc}", file=sys.stderr)
                    break
                if not chunk:
                    print("Server closed the connection.", file=sys.stderr)
                    break
                last_data_time = time.monotonic()
                record_entries.append(
                    {"data_b64": base64.b64encode(chunk).decode("ascii"), "dir": "in", "t_ms": elapsed_ms()}
                )
                transcript_parts.append(chunk.decode("utf-8", errors="replace"))
                sys.stderr.buffer.write(chunk)
                sys.stderr.flush()
                continue

            # No data this poll. If we've been quiet long enough and there's
            # a command left to send, send the next one.
            if command_index < len(commands) and (now - last_data_time) * 1000 >= args.quiet_ms:
                send_command(commands[command_index])
                command_index += 1
                last_data_time = time.monotonic()  # require another quiet gap before the *next* one
    except KeyboardInterrupt:
        print("\nInterrupted, saving what was captured so far.", file=sys.stderr)
    finally:
        try:
            sock.close()
        except OSError:
            pass

    with open(args.out_record, "w", encoding="utf-8") as f:
        for entry in record_entries:
            f.write(json.dumps(entry) + "\n")
    with open(args.out_transcript, "w", encoding="utf-8") as f:
        f.write("".join(transcript_parts))

    print(f"\nWrote {len(record_entries)} entries to {args.out_record}", file=sys.stderr)
    print(f"Wrote transcript to {args.out_transcript}", file=sys.stderr)
    print(
        f"Commands sent: {command_index}/{len(commands)}"
        + (" (all sent)" if command_index == len(commands) else " (stopped early -- see transcript)"),
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
