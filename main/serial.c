/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "serial.h"

#include <stdio.h>
#include <string.h>

static const char ALPHA[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

uint8_t df_luhn32(const uint8_t *v, uint8_t n)
{
    uint32_t sum = 0, factor = 2;
    for (uint8_t k = n; k > 0; --k) {
        uint32_t p = factor * v[k - 1];
        sum += p / 32 + p % 32;
        factor = (factor == 2) ? 1 : 2;
    }
    return (uint8_t)((32 - sum % 32) % 32);
}

void df_serial(const uint8_t mac[6], char out[DF_SERIAL_LEN])
{
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = (v << 8) | mac[i];
    uint8_t sym[10];
    memcpy(out, "KODE", 4);
    for (int i = 0; i < 10; i++) {
        sym[i] = (v >> (50 - (i + 1) * 5)) & 31;
        out[4 + i] = ALPHA[sym[i]];
    }
    out[14] = ALPHA[df_luhn32(sym, 10)];
    out[15] = 0;
}

void df_serial_pretty(const char serial[DF_SERIAL_LEN], char out[DF_PRETTY_LEN])
{
    snprintf(out, DF_PRETTY_LEN, "KODE-%.4s-%.4s-%.3s", serial + 4, serial + 8, serial + 12);
}
