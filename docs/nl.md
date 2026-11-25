# dotfake (Nederlands)

Een gewone ESP32-devkit die zich via Bluetooth voordoet als een Kode Dot. Zo
zie je wat de officiële telefoon-app naar een Dot stuurt, zonder Dot. Dit
project is niet van Kode.

## Hoe het werkt

1. **Gevonden worden.** De ESP32 zendt dezelfde advertentie als een Dot: de
   console-UUID `4b4f4445-0001-4f53-a2d1-7c0e9b5f3a61`. Zijn naam is een
   serienummer in Dot-vorm, gemaakt uit zijn eigen Bluetooth-adres:
   `KODE`, tien Crockford-Base32-tekens van het MAC-adres, en één
   controleteken (Luhn mod 32).
2. **Koppelen.** De Dot toont een code van 6 cijfers op zijn scherm en de
   telefoon vraagt erom. De ESP32 heeft geen scherm, dus de code is vast:
   **123456**. Hij staat ook in de seriële monitor.
3. **Console.** De app schrijft tekstregels naar kanaal RX (`…0002`). De Dot
   antwoordt op TX (`…0003`) met regels `>|sleutel=waarde`. Een antwoord
   eindigt altijd op `>ok` of `>err code=N msg=woord`.
4. **Wat de app vraagt.** Na het verbinden stuurt de app `info`,
   `wifi_info`, `wifi_scan`, `bat_info` en `app_list`. Op de pagina van de Dot
   stuurt hij `config key="bright"`. De knoppen op het scherm sturen
   `btn id=top state=press`. Download stuurt
   `install src="terminal" dst=app size=… crc32=… run=on`. Daarna gaat het
   bestand in blokken van 4096 bytes. Na elk blok stuurt de Dot `>|ack`.

Alles staat uitgebreid in [protocol.md](protocol.md). Een echte sessie staat in
[session-2026-11-24.log](session-2026-11-24.log).

## Testen

1. Flash de bestanden uit `dist/` (zie de README).
2. Open een seriële monitor op 115200 baud.
3. Verbind in de app en kies het `KODE…`-apparaat.
4. Typ **123456** als de telefoon om de code vraagt.
5. Kijk naar de `<<`-regels: dat is wat de app stuurt.

Lukt koppelen niet na opnieuw flashen? Vergeet het apparaat in iOS
(Instellingen → Bluetooth → ⓘ → Vergeet dit apparaat), typ `!forget` in de
monitor en verbind opnieuw.

## Testcodes

* Koppelcode: `123456` (wijzigen met `!code NNNNNN`).
* Serienummer-vectoren: `make -C tests/host` en
  `python3 tools/serial.py --selftest`.
* Probeer in de monitor: typ `>|pong` en dan `>ok` om zelf iets naar de
  telefoon te sturen.

## Wat niet werkt

Wi-Fi, de lijst met apps, apps installeren en het scherm spiegelen. Een
installatie vanuit de app mislukt dus. Dat is de bedoeling.
