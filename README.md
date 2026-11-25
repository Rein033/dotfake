# dotfake

A classic ESP32 devkit that advertises and answers like a Kode Dot's
Bluetooth console. Use it to see what the official phone app sends to a Dot,
without a Dot.

> **Not affiliated with Kode.** This is an independent test tool. "Kode",
> "Kode Dot" and "kodeOS" are Kode's trademarks. They appear here only to say
> what this tool talks to. The device names and product strings in the
> firmware copy what a real Dot sends, because the app expects them. Use it
> only with your own phone and your own account.

* **How the app and the Dot talk:** [docs/protocol.md](docs/protocol.md)
* **A real session, line by line:** [docs/session-2026-11-24.log](docs/session-2026-11-24.log)
* **Nederlands:** [docs/nl.md](docs/nl.md) · **Español:** [docs/es.md](docs/es.md)

## What it does

| Part        | What dotfake does                                                                |
|-------------|-----------------------------------------------------------------------------------|
| Advert      | Same flags, 128-bit console UUID `4b4f4445-0001-4f53-a2d1-7c0e9b5f3a61` and TX power as a Dot |
| Name        | A Dot-style serial made from the ESP32's own BT MAC, for example `KODE24QP6QH2XAQ` |
| GATT        | Console RX `…0002`, TX `…0003`, mirror `…0004`, Battery Service `0x180F`          |
| Pairing     | LE Secure Connections with a passkey. A Dot shows the passkey on its screen; dotfake uses **123456** |
| Console     | `ping`, `info` (reports kodeOS 0.1.5), `bat_info`, `help`, `btn_read`, `time`, `temp`, `pwd`, `ls`, `boot_info`, `gui_info`, `restart`, and a few empty lists |
| Refused     | `install`, `file_put`, `file_get`, `screenshot`, `write`, `cd`, `stat` answer `>err code=5`; `ident_*` answers `>err code=5 msg=cable`; any other verb answers `>err code=1` |
| UART log    | Every line from the phone as `<< line`, every reply as `>> line`                  |

It has no screen, no storage and no Wi-Fi. The app can find it, pair with it
and read its info and battery. Installing an app fails, on purpose.

## Build and flash

You need ESP-IDF v6.1 and a classic ESP32 (ESP32-D0WD) with 4 MB flash.

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build flash monitor
```

Or flash the prebuilt files in [dist/](dist/), for example with esptool:

```bash
esptool.py --chip esp32 write_flash 0x1000 dist/dotfake-0.1.5-boot.bin 0x8000 dist/dotfake-0.1.5-pt.bin 0x10000 dist/dotfake-0.1.5-app.bin
```

## Use it

1. Open a serial monitor at 115200 baud.
2. In the phone app, connect and choose the `KODE…` device.
3. Type **123456** when the phone asks for the code.
4. Use the app. Watch the `<<` lines in the monitor.

Type a line in the monitor to send it to the phone as a reply, for example
`>|pong` and then `>ok`. Lines that start with `!` stay local:

| Command          | What it does                                         |
|------------------|------------------------------------------------------|
| `!help`          | list these commands                                  |
| `!info`          | serial, pairing, code, battery, connection, MTU      |
| `!pair on\|off`  | require pairing or not (restarts)                    |
| `!code NNNNNN`   | set the passkey (kept in NVS)                        |
| `!soc N`         | set the battery percent the Dot reports              |
| `!forget`        | delete all bonds                                     |
| `!restart`       | restart                                              |

If pairing fails after a reflash, open iOS Settings → Bluetooth → ⓘ → Forget
This Device, then `!forget`, and connect again.

## Test codes

| What                 | Value                                         |
|----------------------|-----------------------------------------------|
| Pairing passkey      | `123456` (change with `!code`)                |
| Console service UUID | `4b4f4445-0001-4f53-a2d1-7c0e9b5f3a61`        |
| RX / TX / mirror     | `4b4f4445-0002-…`, `-0003-…`, `-0004-…`        |
| Serial test vectors  | `e8:f6:0a:e7:6c:1d` → `KODE78YR5EEV0XS`, `00:00:00:00:00:01` → `KODE0000000001Y`, `ff:ff:ff:ff:ff:ff` → `KODE7ZZZZZZZZZ2`, `44:bd:8d:78:8b:aa` → `KODE24QP6QH2XAQ` |
| Test lines           | `ping` → `>|pong`, `>ok`; `nosuchverb` → `>err code=1 msg=nosuchverb` |

The serial rule has a host test (no ESP-IDF needed):

```bash
make -C tests/host        # 263/263 pass
python3 tools/serial.py --selftest
python3 tools/serial.py 44:bd:8d:78:8b:aa
python3 tools/serial.py KODE-78YR-5EEV-0XS
```

The first four vectors are kode-sdk's own. The fifth is a serial the official
app read from a test unit.

## Files

```
main/main.c          BLE advert, GATT, pairing, console verbs, UART
main/serial.c/.h     serial from BT MAC (Crockford Base32 + Luhn mod 32)
tests/host/          host test of the serial rule
tools/serial.py      serial <-> MAC, with self-test
docs/protocol.md     the phone app <-> Dot protocol
docs/session-*.log   a real session with the iOS app
dist/                prebuilt firmware for a classic ESP32 (bootloader, partition table, app)
```

## Status

* Tested with the iOS app: it found dotfake, asked for the code, paired with
  123456 and showed it as a Dot. Installing failed, as expected.
* The session log in docs/ comes from a Kode-watch running the same console
  code with a few more verbs, because its UART log was easier to collect.
* Not tested: the Android app.
* Not emulated: Wi-Fi, the app list, installing, the screen mirror.

## Licence

GPL-3.0-or-later ([LICENSE](LICENSE)). The protocol details come from the
kodeOS source, which is GPL-3.0-or-later too. See [NOTICE.md](NOTICE.md).
