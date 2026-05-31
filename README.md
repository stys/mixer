# mixer

Low-latency Core Audio routing, mixing, and recording CLI for macOS. Builds a
private aggregate device from the referenced hardware, sums sources through
named buses, and routes them to device outputs. Optionally records per-source
and per-bus WAV stems.

For more information and motivation see this video 
https://www.youtube.com/watch?v=xq3tZH-dLCM

## Build

Requires CMake (3.20+) and a C++20 toolchain (Apple Clang). nlohmann/json is
fetched at configure time.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary is `build/mixer`.

## Run

```
mixer <config.json>          route/mix audio per config (Ctrl-C to stop)
    [--seconds <n>]          auto-stop after n seconds
mixer --check <config.json>  parse + validate a config, print the plan, exit
mixer --plan <config.json>   print the computed channel-lane matrix plan, exit
mixer --list                 list available devices, exit
mixer --help                 show usage
```

## Configuration

JSON with the following sections:

- `sample_rate`, `buffer_frames` — audio format.
- `devices` — map of config key to Core Audio device name.
- `buses` — named summing nodes, each with a channel count.
- `sends` — `from` (device key) → `to` (bus), with a `gain`.
- `outputs` — `bus` → `device`, with an optional `device_channel` offset.
- `recording` — `enabled`, optional `directory`, and `record` (`"sources"`, `"buses"`).

## Working with Claude

Claude can build and install this tool, run `--list` to discover the connected
devices, and write configuration files from plain-language instructions about
the desired routing. Configurations are not meant to be written by hand — ask
Claude to produce them.

## License

Personal educational use only. See [LICENSE](LICENSE).
