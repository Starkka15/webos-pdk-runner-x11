# webos-pdk-runner-x11

Runs ARM webOS PDK game binaries on x86_64 Linux via `qemu-arm` with native X11/OpenGL display.

## How it works

Uses QEMU user-mode emulation to run ARM webOS PDK game binaries, intercepting GLES calls and forwarding them to native OpenGL on the host X11 display. Audio is relayed through a custom PipeWire-based relay.

## Usage

```bash
./run.sh /path/to/game
```

## Components

- `gl_relay.c` — GLES-to-OpenGL relay that forwards rendering calls from emulated ARM to host X11
- `audio_relay.c` — Audio forwarding via PipeWire
- `font_redirect.c` — Redirects font paths from webOS to host fonts
- `pdk_input.h` — Input event definitions
- `pdk_gl_cmd.h` — GL command definitions shared with the relay

## Dependencies

- `qemu-arm` (user-mode)
- Mesa/OpenGL development libraries
- PipeWire (for audio relay)
- ARM sysroot with webOS PDK libraries

## License

MIT
