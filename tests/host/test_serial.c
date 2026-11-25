/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of main/serial.c: cc -std=c11 -I../../main test_serial.c ../../main/serial.c */
#include <stdio.h>
#include <string.h>

#include "serial.h"

static int fails, runs;

static void check(const char *what, const char *got, const char *want)
{
    runs++;
    if (strcmp(got, want) != 0) {
        fails++;
        printf("FAIL %s: got %s, want %s\n", what, got, want);
    }
}

int main(void)
{
    /* The first four are kode-sdk's own vectors (tests/host/cases/test_khal_identity.c,
     * kaos/autotest/src/autotest_ao.c). The last is the serial the official app
     * read from a test unit in the 2026-11-24 session (docs/session-2026-11-24.log). */
    static const struct { uint8_t mac[6]; const char *want; } V[] = {
        { { 0xE8, 0xF6, 0x0A, 0xE7, 0x6C, 0x1D }, "KODE78YR5EEV0XS" },
        { { 0xE8, 0xF6, 0x0A, 0xE7, 0x6C, 0x1E }, "KODE78YR5EEV0YQ" },
        { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }, "KODE0000000001Y" },
        { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, "KODE7ZZZZZZZZZ2" },
        { { 0x44, 0xBD, 0x8D, 0x78, 0x8B, 0xAA }, "KODE24QP6QH2XAQ" },
    };
    char s[DF_SERIAL_LEN], p[DF_PRETTY_LEN];

    for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
        df_serial(V[i].mac, s);
        check("serial", s, V[i].want);
    }

    df_serial(V[0].mac, s);
    df_serial_pretty(s, p);
    check("pretty", p, "KODE-78YR-5EEV-0XS");

    /* Every symbol is in the Crockford alphabet, for any MAC. */
    uint8_t mac[6] = { 0xE8, 0xF6, 0x0A, 0xE7, 0, 0 };
    for (unsigned k = 0; k < 65536; k += 257) {
        mac[4] = (uint8_t)(k >> 8);
        mac[5] = (uint8_t)k;
        df_serial(mac, s);
        runs++;
        if (strlen(s) != 15 || strspn(s + 4, "0123456789ABCDEFGHJKMNPQRSTVWXYZ") != 11) {
            fails++;
            printf("FAIL alphabet: %s\n", s);
        }
    }

    /* A changed symbol changes the check symbol (single-error detection). */
    uint8_t sym[10] = { 7, 8, 30, 23, 5, 14, 14, 27, 0, 23 };
    uint8_t c0 = df_luhn32(sym, 10);
    int caught = 0, tried = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t keep = sym[i];
        for (uint8_t d = 0; d < 32; d++) {
            if (d == keep) continue;
            sym[i] = d;
            tried++;
            caught += df_luhn32(sym, 10) != c0;
        }
        sym[i] = keep;
    }
    runs++;
    if (caught != tried) {
        fails++;
        printf("FAIL luhn: caught %d of %d single errors\n", caught, tried);
    }

    printf("%d/%d pass\n", runs - fails, runs);
    return fails ? 1 : 0;
}
