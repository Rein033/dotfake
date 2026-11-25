/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dotfake: a classic ESP32 that advertises and answers like a Kode Dot's
 * BLE console, for testing the official phone app with your own hardware.
 *
 * What it copies from kode-sdk (kdrv_ble.c, kdrv_ble_adv.c, console_ao.c):
 *  - advert: flags (GEN|BREDR_UNSUP), 128-bit console UUID, TX power
 *  - scan response: complete local name = canonical serial "KODE" + 11 symbols
 *  - GATT: console service 4b4f4445-0001-..., RX 0002 (write/wnr), TX 0003 (notify),
 *    mirror 0004 (notify), plus Battery Service 0x180F
 *  - pairing: LE Secure Connections, DisplayOnly, MITM, bonding; the passkey
 *    the Dot would show on its glass is fixed here (123456) and printed on UART
 *  - console: text lines, records ">|k=v", end ">ok" / ">err code=N msg=word"
 *
 * UART (115200): everything the phone sends is logged as "<< line", every
 * reply as ">> line". A typed line is sent to the phone as-is (manual reply).
 * Typed lines starting with '!' are local: !help.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/uart.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "serial.h"

#define TAG "dotfake"

extern void ble_store_config_init(void);

/* ---- identity ---------------------------------------------------------- */
static char s_serial[DF_SERIAL_LEN];   /* KODE + 10 + check */
static char s_pretty[DF_PRETTY_LEN];
static uint8_t s_mac[6];         /* BT MAC, most significant first */

static void make_serial(void)
{
    esp_read_mac(s_mac, ESP_MAC_BT);
    df_serial(s_mac, s_serial);
    df_serial_pretty(s_serial, s_pretty);
}

/* ---- settings (NVS "df") ----------------------------------------------- */
static bool s_pair = true;       /* require pairing like a real kodeOS */
static uint32_t s_passkey = 123456;
static uint8_t s_soc = 87;

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("df", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8;
    uint32_t u32;
    if (nvs_get_u8(h, "pair", &u8) == ESP_OK) s_pair = u8;
    if (nvs_get_u32(h, "pk", &u32) == ESP_OK) s_passkey = u32;
    if (nvs_get_u8(h, "soc", &u8) == ESP_OK) s_soc = u8;
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("df", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "pair", s_pair);
    nvs_set_u32(h, "pk", s_passkey);
    nvs_set_u8(h, "soc", s_soc);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- GATT -------------------------------------------------------------- */
/* 4b4f4445-000N-4f53-a2d1-7c0e9b5f3a61, wire order */
#define KODE_UUID(n) BLE_UUID128_INIT(0x61, 0x3a, 0x5f, 0x9b, 0x0e, 0x7c, 0xd1, 0xa2, \
                                      0x53, 0x4f, (n), 0x00, 0x45, 0x44, 0x4f, 0x4b)
static const ble_uuid128_t UUID_SVC = KODE_UUID(0x01);
static const ble_uuid128_t UUID_RX = KODE_UUID(0x02);
static const ble_uuid128_t UUID_TX = KODE_UUID(0x03);
static const ble_uuid128_t UUID_MIRROR = KODE_UUID(0x04);
static const ble_uuid16_t UUID_BATT_SVC = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t UUID_BATT_LVL = BLE_UUID16_INIT(0x2A19);

static uint16_t s_tx_handle, s_mirror_handle, s_batt_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static bool s_tx_sub;
static uint8_t s_own_addr_type;

static QueueHandle_t s_lines;    /* char[256] lines from the phone */
#define DF_LINE 256

/* RX line assembler: CR or LF ends a line, empty lines ignored, >=240 dropped */
static char s_rx_buf[DF_LINE];
static size_t s_rx_len;
static bool s_rx_drop;

static void rx_bytes(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)d[i];
        if (c == '\r' || c == '\n') {
            if (!s_rx_drop && s_rx_len > 0) {
                s_rx_buf[s_rx_len] = 0;
                xQueueSend(s_lines, s_rx_buf, 0);
            }
            s_rx_len = 0;
            s_rx_drop = false;
        } else if (s_rx_len >= 239) {
            s_rx_drop = true;
        } else {
            s_rx_buf[s_rx_len++] = c;
        }
    }
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(ctxt->chr->uuid, &UUID_RX.u) == 0) {
        uint8_t buf[512];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_UNLIKELY;
        if (len == 0) {
            printf("(empty write)\n");
        }
        rx_bytes(buf, len);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(ctxt->chr->uuid, &UUID_BATT_LVL.u) == 0) {
        return os_mbuf_append(ctxt->om, &s_soc, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

/* Flags are filled at start from s_pair, like kdrv_ble.c's CHR_*_ENC masks. */
static struct ble_gatt_chr_def s_chrs[4];
static struct ble_gatt_chr_def s_batt_chrs[2];
static struct ble_gatt_svc_def s_svcs[3];

static void build_table(void)
{
    uint32_t wenc = s_pair ? (BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN) : 0;
    uint32_t nenc = s_pair ? (BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN) : 0;
    s_chrs[0] = (struct ble_gatt_chr_def){ .uuid = &UUID_RX.u, .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | wenc };
    s_chrs[1] = (struct ble_gatt_chr_def){ .uuid = &UUID_TX.u, .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_NOTIFY | nenc, .val_handle = &s_tx_handle };
    s_chrs[2] = (struct ble_gatt_chr_def){ .uuid = &UUID_MIRROR.u, .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_NOTIFY | nenc, .val_handle = &s_mirror_handle };
    s_chrs[3] = (struct ble_gatt_chr_def){ 0 };
    s_batt_chrs[0] = (struct ble_gatt_chr_def){ .uuid = &UUID_BATT_LVL.u, .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_batt_handle };
    s_batt_chrs[1] = (struct ble_gatt_chr_def){ 0 };
    s_svcs[0] = (struct ble_gatt_svc_def){ .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &UUID_SVC.u, .characteristics = s_chrs };
    s_svcs[1] = (struct ble_gatt_svc_def){ .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &UUID_BATT_SVC.u, .characteristics = s_batt_chrs };
    s_svcs[2] = (struct ble_gatt_svc_def){ 0 };
}

/* ---- sending ----------------------------------------------------------- */
static void send_raw(const char *s, size_t n)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_tx_sub) return;
    size_t chunk = (s_mtu > 3) ? (size_t)(s_mtu - 3) : 20;
    while (n > 0) {
        size_t k = n < chunk ? n : chunk;
        for (int tries = 0; tries < 50; tries++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(s, k);
            if (om == NULL) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
            if (rc == 0) break;
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) return;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s += k;
        n -= k;
    }
}

static void reply(const char *fmt, ...)
{
    char line[DF_LINE + 2];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, DF_LINE, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > DF_LINE - 1) n = DF_LINE - 1;
    printf(">> %s\n", line);
    line[n++] = '\n';
    send_raw(line, (size_t)n);
}

/* ---- console verbs ----------------------------------------------------- */
static const char *VERBS[] = {
    "info", "help", "ping", "ls", "pwd", "cd", "stat", "bat_info", "btn_read", "boot_info",
    "time", "temp", "gui_info", "appdata_ls", "variable_list", "function_list", "nvs_list",
    "restart",
};

static void cmd_info(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    reply(">|serial=%s product=\"KODE-DOT\" board=\"DVT\"", s_serial);
    reply(">|mac_p4=%02x:%02x:%02x:%02x:%02x:%02x mac_c5=%02x:%02x:%02x:%02x:%02x:%02x",
          s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], (uint8_t)(s_mac[5] - 2),
          s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    reply(">|slot=factory addr=0x020000 size_kb=4096 state=running project=\"kodeos\" version=\"%s\" idf=\"%s\"",
          d->version, d->idf_ver);
    reply(">|slot=ota_0 addr=0x420000 size_kb=4096 state=empty project=- version=- idf=-");
    reply(">|slot=ota_1 addr=0x820000 size_kb=4096 state=empty project=- version=- idf=-");
    reply(">|coredump=none");
    reply(">|log=warn source=default");
    reply(">|temp=p4 c=34.0");
    reply(">ok");
}

static void dispatch(char *line)
{
    printf("<< %s\n", line);
    char verb[32];
    size_t i = 0;
    while (line[i] && !isspace((unsigned char)line[i]) && i < sizeof(verb) - 1) { verb[i] = line[i]; i++; }
    verb[i] = 0;

    if (!strcmp(verb, "ping")) { reply(">|pong"); reply(">ok"); }
    else if (!strcmp(verb, "info")) cmd_info();
    else if (!strcmp(verb, "help")) {
        for (size_t k = 0; k < sizeof(VERBS) / sizeof(VERBS[0]); k++) reply(">|verb=%s", VERBS[k]);
        reply(">ok");
    }
    else if (!strcmp(verb, "bat_info")) {
        unsigned mv = 3500 + (unsigned)s_soc * 7;
        reply(">|vbat_mv=%u vbus_mv=0 vbus=off boost=off current_ma=-120 soc=%u charging=0 fault=0", mv, s_soc);
        reply(">ok");
    }
    else if (!strcmp(verb, "btn_read")) {
        static const char *b[] = { "up", "down", "left", "right", "top", "bot" };
        for (int k = 0; k < 6; k++) reply(">|btn=%s pressed=off", b[k]);
        reply(">ok");
    }
    else if (!strcmp(verb, "time")) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec < 1700000000) reply(">|time state=unset");
        else {
            char buf[32];
            struct tm tm;
            gmtime_r(&tv.tv_sec, &tm);
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            reply(">|time state=set utc=\"%s\" source=phone", buf);
        }
        reply(">ok");
    }
    else if (!strcmp(verb, "temp")) { reply(">|temp=p4 c=34.0"); reply(">ok"); }
    else if (!strcmp(verb, "pwd")) { reply(">|path=\"/storage\""); reply(">ok"); }
    else if (!strcmp(verb, "ls")) { reply(">|name=\"kode\" type=dir size=0 medium=flash"); reply(">ok"); }
    else if (!strcmp(verb, "cd") || !strcmp(verb, "stat")) { reply(">err code=5 msg=%s", verb); }
    else if (!strcmp(verb, "boot_info")) {
        reply(">|reason=poweron uptime_ms=%llu", (unsigned long long)(esp_timer_get_time() / 1000));
        reply(">ok");
    }
    else if (!strcmp(verb, "gui_info")) {
        reply(">|gui state=home sel=- level=0 group=- sheet=-");
        reply(">ok");
    }
    else if (!strcmp(verb, "appdata_ls") || !strcmp(verb, "variable_list") ||
             !strcmp(verb, "function_list") || !strcmp(verb, "nvs_list")) {
        reply(">ok");
    }
    else if (!strcmp(verb, "restart")) { reply(">ok"); vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); }
    else if (!strcmp(verb, "install") || !strcmp(verb, "file_put") || !strcmp(verb, "file_get") ||
             !strcmp(verb, "screenshot") || !strcmp(verb, "write")) {
        /* Not emulated: no storage and no screen. */
        reply(">err code=5 msg=%s", verb);
    }
    else if (!strncmp(verb, "ident_", 6)) { reply(">err code=5 msg=cable"); }
    else { reply(">err code=1 msg=%s", verb); }
}

static void console_task(void *arg)
{
    char line[DF_LINE];
    for (;;) {
        if (xQueueReceive(s_lines, line, portMAX_DELAY) == pdTRUE) dispatch(line);
    }
}

/* ---- GAP --------------------------------------------------------------- */
static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_mtu = 23;
            s_tx_sub = false;
            s_rx_len = 0;
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(s_conn, &d) == 0) {
                printf("[ble] connected %02x:%02x:%02x:%02x:%02x:%02x\n",
                       d.peer_id_addr.val[5], d.peer_id_addr.val[4], d.peer_id_addr.val[3],
                       d.peer_id_addr.val[2], d.peer_id_addr.val[1], d.peer_id_addr.val[0]);
            }
            if (s_pair) ble_gap_security_initiate(s_conn);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        printf("[ble] disconnected reason=0x%x\n", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_tx_sub = false;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) {
            s_tx_sub = ev->subscribe.cur_notify;
            printf("[ble] TX notify %s\n", s_tx_sub ? "on" : "off");
        } else if (ev->subscribe.attr_handle == s_mirror_handle) {
            printf("[ble] mirror notify %s (not emulated)\n", ev->subscribe.cur_notify ? "on" : "off");
        } else if (ev->subscribe.attr_handle == s_batt_handle) {
            printf("[ble] battery notify %s\n", ev->subscribe.cur_notify ? "on" : "off");
        }
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        printf("[ble] mtu %u\n", s_mtu);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            printf("[ble] encryption status=%d enc=%d authen=%d bonded=%d\n", ev->enc_change.status,
                   d.sec_state.encrypted, d.sec_state.authenticated, d.sec_state.bonded);
        }
        break;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pk = { .action = ev->passkey.params.action };
        if (pk.action == BLE_SM_IOACT_DISP) {
            pk.passkey = s_passkey;
            printf("\n[ble] *** PAIRING CODE: %06lu ***\n\n", (unsigned long)s_passkey);
            ble_sm_inject_io(ev->passkey.conn_handle, &pk);
        } else {
            printf("[ble] passkey action %d not supported\n", pk.action);
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The phone forgot us: forget it too and pair again. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        printf("[ble] repeat pairing: old bond removed\n");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.tx_pwr_lvl_is_present = 1;
    f.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    f.uuids128 = &UUID_SVC;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { printf("[ble] adv fields rc=%d\n", rc); return; }

    struct ble_hs_adv_fields r;
    memset(&r, 0, sizeof(r));
    r.name = (uint8_t *)s_serial;
    r.name_len = strlen(s_serial);
    r.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc) { printf("[ble] scan rsp rc=%d\n", rc); return; }

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = 160;  /* 100 ms */
    p.itvl_max = 240;  /* 150 ms */
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) printf("[ble] adv start rc=%d\n", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    printf("[ble] advertising as %s (%s), pairing %s\n", s_serial, s_pretty,
           s_pair ? "on (passkey)" : "off");
    advertise();
}

static void on_reset(int reason)
{
    printf("[ble] host reset %d\n", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- UART -------------------------------------------------------------- */
static void local_cmd(char *s)
{
    if (!strcmp(s, "!help")) {
        printf("!info | !pair on|off | !code NNNNNN | !soc N | !forget | !restart\n"
               "other lines are sent to the phone as-is\n");
    } else if (!strcmp(s, "!info")) {
        printf("serial=%s pretty=%s pair=%d code=%06lu soc=%u conn=%d tx_sub=%d mtu=%u\n", s_serial, s_pretty,
               s_pair, (unsigned long)s_passkey, s_soc, s_conn != BLE_HS_CONN_HANDLE_NONE, s_tx_sub, s_mtu);
    } else if (!strncmp(s, "!pair ", 6)) {
        s_pair = !strcmp(s + 6, "on");
        settings_save();
        printf("pair=%d, restarting\n", s_pair);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (!strncmp(s, "!code ", 6)) {
        s_passkey = (uint32_t)strtoul(s + 6, NULL, 10) % 1000000;
        settings_save();
        printf("code=%06lu\n", (unsigned long)s_passkey);
    } else if (!strncmp(s, "!soc ", 5)) {
        int v = atoi(s + 5);
        s_soc = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
        settings_save();
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gatts_chr_updated(s_batt_handle);
        printf("soc=%u\n", s_soc);
    } else if (!strcmp(s, "!forget")) {
        ble_store_clear();
        printf("all bonds removed\n");
    } else if (!strcmp(s, "!restart")) {
        esp_restart();
    } else {
        printf("unknown; try !help\n");
    }
}

static void uart_task(void *arg)
{
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    char line[DF_LINE];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            if (n == 0) continue;
            line[n] = 0;
            n = 0;
            if (line[0] == '!') local_cmd(line);
            else reply("%s", line);
        } else if (n < DF_LINE - 1) {
            line[n++] = (char)c;
        }
    }
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    make_serial();
    s_lines = xQueueCreate(8, DF_LINE);

    printf("\n=== dotfake %s: ESP32 posing as a Kode Dot BLE console (private test) ===\n",
           esp_app_get_description()->version);

    if (nimble_port_init() != ESP_OK) { printf("nimble init failed\n"); return; }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = s_pair ? 1 : 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    build_table();
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);
    ble_svc_gap_device_name_set(s_serial);
    ble_store_config_init();

    xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
    xTaskCreate(uart_task, "uart", 3072, NULL, 4, NULL);
    nimble_port_freertos_init(host_task);
}
