# dotfake (Español)

Un ESP32 clásico que se anuncia y responde por Bluetooth como la consola de
un Kode Dot. Sirve para ver qué envía la app oficial del teléfono a un Dot,
sin tener un Dot. Este proyecto no es de Kode.

## Cómo funciona

1. **Ser encontrado.** El ESP32 envía el mismo anuncio que un Dot: el UUID
   de la consola `4b4f4445-0001-4f53-a2d1-7c0e9b5f3a61`. Su nombre es un
   número de serie con forma de Dot, hecho con su propia dirección
   Bluetooth: `KODE`, diez símbolos Crockford Base32 de la MAC y un símbolo
   de control (Luhn mod 32).
2. **Emparejar.** El Dot muestra un código de 6 cifras en su pantalla y el
   teléfono lo pide. El ESP32 no tiene pantalla, así que el código es fijo:
   **123456**. También aparece en el monitor serie.
3. **Consola.** La app escribe líneas de texto en el canal RX (`…0002`). El
   Dot responde en TX (`…0003`) con líneas `>|clave=valor`. Una respuesta
   siempre termina con `>ok` o `>err code=N msg=palabra`.
4. **Qué pide la app.** Después de conectar, la app envía `info`,
   `wifi_info`, `wifi_scan`, `bat_info` y `app_list`. En la página del Dot
   envía `config key="bright"`. Los botones de la pantalla envían
   `btn id=top state=press`. Descargar envía
   `install src="terminal" dst=app size=… crc32=… run=on`. Después el archivo
   va en bloques de 4096 bytes. Tras cada bloque el Dot envía `>|ack`.

Todo el detalle está en [protocol.md](protocol.md) (en inglés). Una sesión
real está en [session-2026-11-24.log](session-2026-11-24.log).

## Probar

1. Graba los archivos de `dist/` (ver el README).
2. Abre un monitor serie a 115200 baudios.
3. Conecta en la app y elige el dispositivo `KODE…`.
4. Escribe **123456** cuando el teléfono pida el código.
5. Mira las líneas `<<`: es lo que envía la app.

¿Falla el emparejamiento después de volver a grabar? Olvida el dispositivo en
iOS (Ajustes → Bluetooth → ⓘ → Omitir este dispositivo), escribe `!forget`
en el monitor y conecta de nuevo.

## Códigos de prueba

* Código de emparejamiento: `123456` (cámbialo con `!code NNNNNN`).
* Vectores del número de serie: `make -C tests/host` y
  `python3 tools/serial.py --selftest`.
* En el monitor, escribe `>|pong` y luego `>ok` para enviar tú mismo una
  respuesta al teléfono.

## Qué no funciona

Wi-Fi, la lista de apps, instalar apps y la réplica de pantalla. Una
instalación desde la app falla. Es a propósito.
