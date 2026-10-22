# Output plugin template (`udp_json`)

A complete, minimal output plugin for the Babble Frame Runtime. It sends the tracked values that changed, as one JSON
line per update, to a UDP port:

```json
{"seq":123,"values":{"jawOpen":0.25,"LeftEyeLid":0.97}}
```

Use it as the starting point for your own output: copy this directory, rename the target in `CMakeLists.txt` and the
plugin name in `plugin.cpp`, and replace `send_update()`.

## Build

On the headset (or any aarch64 Linux with the same glibc), from this directory:

```sh
cmake -S . -B build -DBABBLE_SDK_DIR=/path/to/Babble-Frame-Runtime/sdk
cmake --build build
```

`BABBLE_SDK_DIR` defaults to this repo's `sdk/`, so it can be left out when building in place. The only thing a plugin
needs from the runtime is `sdk/include/babble/output_plugin.h`.

## Install and enable

Copy `build/libbabble_udp_json.so` into the driver's `bin/linuxarm64/plugins/` directory, or pass its path, and add an
output to `~/.config/babble-frame-runtime/config.json`:

```json
{
  "sinks": {
    "json_out": {
      "type": "plugin",
      "library": "babble_udp_json",
      "config": { "host": "127.0.0.1", "port": 9500 }
    }
  }
}
```

A bare `library` name is looked up as `<driver>/bin/linuxarm64/plugins/lib<name>.so`; a value containing a `/` or
ending in `.so` is used as a path. Restart the runtime and its log shows
`sink json_out: plugin udp_json 1.0 loaded from ...`. Check the output with
`nc -ul 9500` (or any UDP listener).

To show the plugin on the Outputs page of the Babble tab, with an on/off switch and its own settings, give the entry
an `"output"` id and a `"module"` block, and turn it on in `"outputs"`
(see `docs/output-modules.md`, "Output modules and the Outputs page").

## The contract in short

See `sdk/include/babble/output_plugin.h` and `docs/output-modules.md` for the full reference.

- Export one function, `babble_output_plugin_v1()`, returning a `babble_output_v1` table with `abi = 1`.
- `create()` gets the host table, the instance name and the `config` object as JSON text.
- `start()` starts the plugin's own threads, and `stop()` must return promptly.
- Signals are named floats. Ids are `0 .. signal_count()-1`, and new ones may appear while the runtime starts up. Use
  `wait_update()` to wait for new values and `read()` to fetch the ones you need.
- Link `-static-libstdc++ -static-libgcc`: the plugin ships in a Steam depot next to the runtime.
