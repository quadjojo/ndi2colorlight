# ndi2colorlight

Real-time NDI to Colorlight LED wall — Linux / Raspberry Pi CLI.

Receive an [NDI®](https://www.ndi.video/) video stream over the network and drive a [Colorlight](https://www.lightsbycolorlight.com/) E-series receiver card directly via raw Ethernet (`AF_PACKET`).

> **Status:** Working alpha, version 0.1. Tested on Raspberry Pi with Colorlight E120 and a 6×4 wall of 64×32 pixel panels (384×128 px output).

> **Heads up — this is vibecoded.** Most of this code was pair-written with an LLM during quick iteration sessions, not carefully engineered. It works for the author's setup but is not hardened, audited, or production-tested. Constants are reverse-engineered from packet captures, error handling is "good enough", and edge cases are mostly unexplored. Treat it as a starting point, not a finished product. Bug reports and PRs are very welcome.

---

## Features

- **NDI 6 receiver** — auto-discovery, source selection by name (substring match)
- **Colorlight protocol** — raw Layer-2 Ethernet, max 497 px/packet, BGR pixel order, sync + brightness control packets
- **Scaling and gamma** — output is sized to the configured panel grid; per-channel gamma applied before transmission
- **Brightness** sent as 0x0A control packets, no client-side scaling
- **systemd installer** — interactive setup that auto-detects interfaces and discovers NDI sources
- **`--list` mode** for quick NDI source discovery from the terminal

---

## Architecture

```
┌─────────────┐      ┌──────────────┐      ┌────────────────┐      ┌───────────┐
│ NDI source  │ ───▶ │ ndi_receiver │ ───▶ │ frame_convert  │ ───▶ │ colorlight│ ───▶ Wall
│ (network)   │ BGRX │  (libndi)    │ BGRX │ scale + BGR    │  BGR │ AF_PACKET │
└─────────────┘      └──────────────┘      └────────────────┘      └───────────┘
```

| Source file | Role |
|---|---|
| `main.c` | CLI parsing, NDI discovery loop, frame pump, signal handling |
| `ndi_receiver.{c,h}` | NDI SDK integration: discovery, connect, frame callbacks |
| `frame_convert.{c,h}` | RGB→BGR, scaling, gamma correction |
| `colorlight_output.{c,h}` | Layer-2 packet builder + `AF_PACKET` socket |
| `config.{c,h}` | `wall.conf` key/value parser |

---

## Hardware

- **Colorlight E-series** receiver card. E120 verified — other E-series models using the same Layer-2 protocol should work; please open an issue with results.
- LED panels driven by the receiver card. Panel pixel size and grid layout are configurable.
- Dedicated wired Ethernet path between host and receiver. The Colorlight protocol floods raw frames at line rate; **do not share** the link with normal LAN traffic. A USB Ethernet adapter on a separate interface is the typical Raspberry Pi setup.

---

## Prerequisites

- Linux with `AF_PACKET` raw sockets (any modern kernel; tested on Raspberry Pi OS, Debian, Ubuntu)
- `gcc`, `make`
- **NDI SDK 6 for Linux** — headers at `/usr/local/include`, `libndi.so.6` reachable by the dynamic linker. Adjust `NDI_INCLUDE` in the `Makefile` if installed elsewhere. SDK download: [ndi.video/sdk](https://www.ndi.video/sdk/).

---

## Build

```bash
git clone https://github.com/quadjojo/ndi2colorlight.git
cd ndi2colorlight
make
```

Produces `./ndi-led-cli`. `make clean` removes object files and the binary.

---

## Configure

Copy the template and edit it:

```bash
cp wall.conf.example wall.conf
```

| Key | Meaning | Default |
|---|---|---|
| `panel_width` | pixels per panel, horizontal | 64 |
| `panel_height` | pixels per panel, vertical | 32 |
| `panels_x` | panel count, horizontal | 6 |
| `panels_y` | panel count, vertical | 4 |
| `brightness` | 0–100 | 100 |
| `gamma` | 1.0 = linear, 2.2 = typical display | 1.0 |

Output resolution is `panels_x × panel_width` × `panels_y × panel_height`.

---

## Run manually

```bash
sudo ./ndi-led-cli --source "MY-NDI-SOURCE" \
                   --interface eth1 \
                   --config wall.conf
```

`sudo` is required because `AF_PACKET` raw sockets need `CAP_NET_RAW`. To run without `sudo`, set the capability on the binary:

```bash
sudo setcap cap_net_raw+ep ./ndi-led-cli
```

### CLI flags

| Flag | Long form | Purpose |
|---|---|---|
| `-s NAME` | `--source NAME` | NDI source name (substring match) |
| `-i IF` | `--interface IF` | Output interface (e.g. `eth1`) |
| `-c FILE` | `--config FILE` | Config file path (default `./wall.conf`) |
| `-l` | `--list` | Discover NDI sources for 5 s and exit |
| `-b N` | `--brightness N` | Override brightness from config |
| `-h` | `--help` | Show usage |

`--list` is the easiest way to find the exact source name before wiring it into a service.

---

## Install as systemd service

```bash
sudo ./install.sh
```

The installer:

1. Lists available network interfaces and prompts for the LED output one (skip the prompt by passing it as `$1`).
2. Discovers NDI sources for 5 s and lets you pick one (skip with `$2`).
3. Copies `ndi-led-cli` to `/usr/local/bin/`.
4. Copies `wall.conf.example` to `/etc/ndi-led-wall/wall.conf` (only if not already present — safe to re-run).
5. Writes a `ndi-led-wall.service` unit and enables it.

Non-interactive install:

```bash
sudo ./install.sh eth1 "MY-NDI-SOURCE"
```

After install:

```bash
systemctl status ndi-led-wall          # state
journalctl -u ndi-led-wall -f          # live logs
systemctl restart ndi-led-wall         # reload after config edits
systemctl stop ndi-led-wall            # stop output
```

### Uninstall

```bash
sudo systemctl disable --now ndi-led-wall
sudo rm /etc/systemd/system/ndi-led-wall.service
sudo rm /usr/local/bin/ndi-led-cli
sudo rm -rf /etc/ndi-led-wall
sudo systemctl daemon-reload
```

---

## Network setup notes

- **Layer 2, no IP needed.** The Colorlight protocol uses raw Ethernet frames addressed to MAC `11:22:33:44:55:66`. No DHCP, no static IP, no switch configuration on a direct link.
- **Direct cable** Pi → receiver card is the simplest setup. Switches work but must not filter unknown unicast or rate-limit broadcast.
- **Dedicated NIC.** A 384×128 wall at 60 fps already pushes ~50 Mbit/s of pure pixel data — keep regular LAN traffic off this link.
- **MTU** stays at 1500. The protocol caps payload at 497 px × 3 B + headers ≈ 1500 B per packet by design.

---

## Protocol summary

Raw Ethernet frames, no EtherType. Header layout:

| Bytes | Field |
|---|---|
| 0–5 | Destination MAC (`11:22:33:44:55:66`) |
| 6–11 | Source MAC (host NIC) |
| 12 | Packet type (`0x55` pixel, `0x01` sync, `0x0A` brightness) |
| 13+ | Payload |

A frame is one or more `0x55` pixel packets followed by a `0x01` sync packet (with a small inter-row delay; ~5 ms after the last row in our experience). Brightness changes go out as `0x0A` packets and are persisted by the receiver card until power-cycled.

This is reverse-engineered, not vendor-documented. Treat constants as best-effort.

---

## Known limitations

- Only Colorlight **E-series** has been verified — Z-series uses a different protocol and is not supported.
- No HDR / 10-bit color path — internal pipeline is 8-bit BGR.
- No audio (NDI audio frames are dropped).
- No remote control / OSC / MIDI yet.

---

## Contributing

Issues and PRs welcome. If you test against a Colorlight model not listed here, please open an issue with: receiver model, panel scan rate, panel pixel dimensions, and whether discovery + pixel output worked.

---

## License

[MIT](LICENSE) — © 2026 Tobias Wessely.

## Credits and trademarks

- **NDI®** is a registered trademark of [Vizrt Group](https://www.vizrt.com/). This project is not affiliated with or endorsed by Vizrt. The NDI SDK is distributed by Vizrt under its own license — install it separately.
- **Colorlight** receiver cards are products of [Colorlight Cloud Tech Ltd](https://www.lightsbycolorlight.com/). This project is not affiliated with or endorsed by Colorlight.
