# mudclient

A high-performance, modular MUD client core in C++20 with embedded Lua
scripting. See [`docs/SPEC.md`](docs/SPEC.md) for the full specification.

This README covers building and running what exists at each milestone; it
will be filled out with usage, scripting, release, and live-test sections as
those parts of the client are built (see `docs/SPEC.md` §8 for the target
scope).

## Building (Linux/Windows, GCC 13 / Clang 17 / MSVC 2022)

Requires CMake ≥ 3.25 and Ninja. All dependencies are fetched automatically
via `FetchContent`.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

Debug and sanitizer builds:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

## Status

Milestone 1 (parser, network client, event queue, CI) is in progress. See
`docs/SPEC.md` for the milestone plan and `NOTES.md` for the running log of
decisions and deviations.
