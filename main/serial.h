/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The serial a Kode Dot advertises as its BLE name: "KODE", then the 48-bit
 * Bluetooth MAC as ten Crockford Base32 symbols (left-padded to 50 bits), then
 * one Luhn mod 32 check symbol. Same rule as kode-sdk khal_identity.c.
 * Pure C, no ESP-IDF, so the host test can build it.
 */
#pragma once
#include <stdint.h>

#define DF_SERIAL_LEN 16   /* 15 characters and a NUL */
#define DF_PRETTY_LEN 19   /* KODE-XXXX-XXXX-XXX and a NUL */

/* mac: most significant byte first, as esp_read_mac() gives it. */
void df_serial(const uint8_t mac[6], char out[DF_SERIAL_LEN]);
/* The printed form, grouped 4-4-3 after the prefix. */
void df_serial_pretty(const char serial[DF_SERIAL_LEN], char out[DF_PRETTY_LEN]);
/* Luhn mod 32 over n symbols (each 0..31). */
uint8_t df_luhn32(const uint8_t *v, uint8_t n);
