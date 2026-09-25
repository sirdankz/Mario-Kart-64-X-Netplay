# Mario-Kart-64-X-Netplay

Original Xbox netplay and Xbox 360 crossplay additions for
[Team Resurgent's Mario-Kart-64-X](https://github.com/Team-Resurgent/Mario-Kart-64-X).

The base port is a native Original Xbox port of the
[Mario Kart 64 decompilation](https://github.com/n64decomp/mk64), built on
[jnmartin84's Dreamcast port](https://github.com/jnmartin84/mario-kart-64-dc).

## Netplay

This fork adds online multiplayer support to the Original Xbox port and a shared
deterministic netplay protocol for Xbox 360 crossplay.

For cross-console play, the currently supported setup is:

```text
Xbox 360 = host
Original Xbox = join
```

The netplay work includes deterministic gameplay synchronization, local-player
handling, per-player rendering/camera behavior, online pause/lifecycle sync,
and per-player presentation/audio fixes needed for multiplayer.

Normal offline play remains available.

## No ROM or generated game assets are included

You must supply your own Mario Kart 64 ROM.

Use the USA big-endian `.z64` ROM named exactly:

```text
baserom.us.z64
```

Expected MD5:

```text
3a67d9986f54eb282924fca4cd5f6dff
```

`.n64` and `.v64` dumps are byte-swapped and are not accepted by the build.

## Requirements

- Python 3
- git
- [RXDK](https://github.com/Team-Resurgent/RXDK-VS20XX)
- CMake, or allow `setup.py` to obtain the portable build it expects

`setup.py` fetches and builds its pinned Torch dependency as needed.

## Build from a fresh clone

```text
git clone https://github.com/sirdankz/Mario-Kart-64-X-Netplay.git
cd Mario-Kart-64-X-Netplay
```

Copy your own `baserom.us.z64` into the repository root, then run:

```text
python setup.py
```

A successful Release build ends with:

```text
out\Release\XISO\mk64x.iso
```

The repository has been tested from a fresh GitHub clone with only the user's
own ROM added locally.

### setup.py options

```text
python setup.py
python setup.py --check
python setup.py --force
python setup.py --skip-build
python setup.py --config Debug
python setup.py --native-torch
```

Run `python setup.py` at least once before opening the project for normal
Visual Studio build/deploy work because the setup process generates the
ROM-derived files that are intentionally not stored in Git.

## Before publishing changes

Run:

```text
python tools/audit_repo_clean.py
```

Exit code `0` is required before publishing. The audit checks the publishable
tree for ROM-derived payloads, build output, and other generated game data.

Do not commit or distribute `baserom.us.z64`, generated ROM assets, an XBE, or
an XISO/ISO.

## Netplay license

The original netplay/crossplay code authored by sirdankz is licensed
`GPL-3.0-only`.

See:

- `NETPLAY-LICENSE.md` for the exact scope
- `COPYING.NETPLAY` for the GNU GPL version 3 text

This GPL notice does **not** relicense the entire upstream repository. Existing
Mario-Kart-64-X, MK64 decompilation, Dreamcast-port, RXDK, Nintendo, and other
third-party material remains subject to the rights and terms of its respective
authors.

## Credits

- [Team Resurgent / Mario-Kart-64-X](https://github.com/Team-Resurgent/Mario-Kart-64-X) — Original Xbox base port
- [n64decomp/mk64](https://github.com/n64decomp/mk64) — Mario Kart 64 decompilation
- [jnmartin84](https://github.com/jnmartin84) — Dreamcast port
- [HarbourMasters/torch](https://github.com/HarbourMasters/torch) — asset extraction
- [RXDK](https://github.com/Team-Resurgent/RXDK-VS20XX) — Original Xbox toolchain

Mario Kart 64 and related game content are Nintendo's. This repository does
not include a Mario Kart 64 ROM or generated ROM-derived asset payloads.
