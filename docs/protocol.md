# How the phone app talks to a Dot

This is what the official phone app ("Kode DIY" on the App Store, "Kode" on
Google Play) does over Bluetooth Low Energy, and what the Dot answers. It
comes from two sources:

* the kodeOS 0.1.5 source (GPL-3.0): `kdrv_ble.c`, `kdrv_ble_adv.c`,
  `console_ao.c`, `khal_identity.c`, `radio_verbs.c`, `appmgr_verbs.c`;
* logs of the app talking to a test unit
  ([session-2026-11-24.log](session-2026-11-24.log)).

The app itself is closed source. Anything it does that was not seen in a log
is not described here.

## 1. Finding the Dot

The Dot advertises connectable, undirected, every 100 to 150 ms.

| Packet        | Field                     | Value                                         |
|---------------|---------------------------|-----------------------------------------------|
| advert        | flags                     | `0x06` (general discoverable, no BR/EDR)      |
| advert        | complete 128-bit UUIDs    | `4b4f4445-0001-4f53-a2d1-7c0e9b5f3a61`        |
| advert        | TX power                  | auto                                          |
| scan response | complete local name       | the serial, for example `KODE24QP6QH2XAQ`     |

The first four bytes of the UUID spell `KODE` in ASCII. A unit that
advertises this UUID and a serial as its name was found by the app.

### The serial

```
KODE  +  ten Crockford Base32 symbols of the 48-bit BT MAC  +  one Luhn mod 32 check symbol
```

* The alphabet is `0123456789ABCDEFGHJKMNPQRSTVWXYZ`: no I, L, O or U.
* The MAC is left-padded from 48 to 50 bits and cut into ten 5-bit symbols,
  most significant first.
* The check symbol is Luhn mod 32 over the ten symbols, so any one wrong
  symbol is caught.
* Printed, it is grouped 4-4-3: `KODE-24QP-6QH2-XAQ`.

Test vectors: [tests/host/test_serial.c](../tests/host/test_serial.c) and
`python3 tools/serial.py --selftest`.

## 2. GATT

| Service / characteristic                      | Properties               | Use                                   |
|-----------------------------------------------|--------------------------|---------------------------------------|
| `4b4f4445-0001-4f53-a2d1-7c0e9b5f3a61`        | primary service          | console                               |
| `4b4f4445-0002-…` RX                          | write, write without rsp | phone to Dot: command lines and raw bytes |
| `4b4f4445-0003-…` TX                          | notify                   | Dot to phone: reply lines             |
| `4b4f4445-0004-…` mirror                      | notify                   | screen mirror (not emulated here)     |
| `0x180F` Battery / `0x2A19` level             | read, notify             | percent                               |

With pairing on, RX needs an encrypted, authenticated link, and so do the TX
and mirror subscriptions. A reply longer than MTU − 3 is cut into several
notifications. The phone joins them at the newline.

## 3. Pairing

* LE Secure Connections, bonding, MITM.
* The Dot is **DisplayOnly**: it shows a six-digit passkey, and the phone
  types it in. On the iPhone this showed as a prompt for an access code.
* The Dot asks for security as soon as the phone connects.
* If the phone forgot the bond but the Dot did not, the Dot deletes its old
  bond and pairs again.

dotfake has no screen, so it uses a fixed passkey: **123456**. It prints the
passkey on its UART as `[ble] *** PAIRING CODE: 123456 ***`. Change it with
`!code NNNNNN`.

## 4. The console

The console reads text lines and writes text replies.

* A line ends at CR or LF. Empty lines are ignored. A line of 240 bytes or
  longer is dropped whole.
* The form is `verb key=value key="quoted value" …`.
* A reply is any number of records `>|k=v k=v …`, closed by exactly one end
  line:
  * `>ok` for success;
  * `>err code=N msg=word` for failure.
* One command runs at a time.

| code | name    | meaning                                                 |
|------|---------|---------------------------------------------------------|
| 1    | VERB    | unknown verb (`msg` is the verb)                        |
| 2    | MISSING | a required key is missing                               |
| 3    | VALUE   | bad value: wrong type, out of range                     |
| 4    | BUSY    | another command is still running                        |
| 5    | REJECT  | refused or failed by the part of kodeOS that owns it    |
| 6    | TIMEOUT | that part never answered                                |

## 5. What the app sends

These are the verbs from the logs, in the order the app sent them.

| When                       | Line                                                        | kodeOS answer (from the source)                                          |
|----------------------------|-------------------------------------------------------------|--------------------------------------------------------------------------|
| after connecting           | `info`                                                      | `>|serial=… product="KODE-DOT" board=…`, `>|mac_p4=… mac_c5=…`, one `>|slot=…` per partition, `>|coredump=…`, `>|log=…`, `>ok` |
|                            | `wifi_info`                                                 | `>|wifi=<state> ssid=… ip=… rssi=… en=0|1`, `>ok`                         |
|                            | `wifi_scan`                                                 | one `>|ssid="…" rssi=… ch=… auth=… bssid=…` per network, `>ok`           |
|                            | `bat_info`                                                  | `>|vbat_mv=… vbus_mv=… vbus=on|off boost=… current_ma=… soc=… charging=0|1 fault=…`, `>ok` |
|                            | `app_list`                                                  | one `>|name="…" version=… size_kb=… category="…" display="…"` per installed app, `>ok` |
| the Dot's page             | `config key="bright"`                                       | `>|config key=bright value=… stored=…`, `>ok`                             |
| remote buttons             | `btn id=up|down|left|right|top|bot state=press` (kodeOS also takes `release`, `hold`, `double`) | `>ok`                         |
| Wi-Fi settings             | `wifi_join ssid="…" pass="…" remember=on` (kodeOS verb; not seen in a log) | `>ok` after the join                                      |
| Download                   | `install src="terminal" dst=app size=N crc32=C run=on`      | see 6                                                                    |
| delete an app              | `app_erase name="…"` (kodeOS verb; not seen in a log)       | `>|erased …`, `>ok`                                                      |

## 6. Installing an app

`install src="terminal"` streams the image over the same BLE link:

```
phone: install src="terminal" dst=app size=3740736 crc32=3803774522 run=on
Dot:   >|wait                        (once a second, while an app it must evict is copied to the card)
Dot:   >|evicted name="y" kept=off   (one per app it had to remove)
Dot:   >|ready chunk=4096            (RX now takes raw bytes, not lines)
phone: <4096 raw bytes>
Dot:   >|ack
phone: <4096 raw bytes>
Dot:   >|ack
…
phone: <the last, partial chunk>     (no ack)
Dot:   >ok                           (written, CRC-32 right, installed; with run=on it reboots into the app)
       or >err code=5 / code=6
```

* The stop-and-wait `>|ack` is the only flow control.
* `>ok` is a result, not an accept. It means the transfer, the CRC check and
  the install all happened.
* `dst=c5` (the radio chip's firmware) is cable only: installing it reboots
  the C5, which is the BLE radio, so the `>ok` could never arrive. The app
  only sent `dst=app` in the logs.

dotfake does not store apps. It answers `install` with `>err code=5`, so the
app shows a failed install. The Kode-watch "Dot pose" does take the stream and
acks every chunk, then drops it. That is how the install log above was made.

## 7. What is not known

* Whether the app checks the Dot's identity certificate (`id_cert`,
  `id_sign`). It did not ask for it in any log. Those verbs need the Dot's
  factory key, which a clone does not have.
* What the app sends for the screen mirror (characteristic 0004).
* Android: all logs are from the iOS app.
