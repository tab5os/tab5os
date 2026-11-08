/* SPDX-License-Identifier: Apache-2.0
 * Pure protocol helpers for t5_usb, host-tested: USB-MIDI event packets and
 * HID keyboard usages.
 */
#include <string.h>
#include "usb_proto.h"

/* Bytes of MIDI data per code index number (USB MIDI 1.0, table 4-1). */
static const uint8_t s_cin_len[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };

size_t usb_midi_parse(const uint8_t pkt[4], uint8_t out[3])
{
    uint8_t cin = pkt[0] & 0x0F;
    size_t n = s_cin_len[cin];
    if (!n) return 0;
    memcpy(out, &pkt[1], n);
    return n;
}

bool usb_midi_pack(const uint8_t *msg, size_t len, uint8_t pkt[4])
{
    if (!msg || !len || len > 3) return false;
    uint8_t st = msg[0];
    uint8_t cin;
    if (st >= 0x80 && st < 0xF0) {
        cin = st >> 4;                           /* channel voice: CIN = status high nibble */
        if (len != (size_t)s_cin_len[cin]) return false;
    } else if (st == 0xF2) {
        cin = 0x3;
    } else if (st == 0xF1 || st == 0xF3) {
        cin = 0x2;
    } else if (st >= 0xF8 || st == 0xF6) {
        cin = 0xF;                               /* single byte */
        if (len != 1) return false;
    } else {
        return false;                            /* SysEx is not supported here */
    }
    memset(pkt, 0, 4);
    pkt[0] = cin;
    memcpy(&pkt[1], msg, len);
    return true;
}

/* ---- HID usage to LVGL key ---- */
uint32_t usb_hid_to_lv_key(uint8_t u, bool shift)
{
    static const char num[] = "1234567890";
    static const char num_sh[] = "!@#$%^&*()";
    static const struct { uint8_t u; char c, s; } sym[] = {
        { 0x2C, ' ', ' ' }, { 0x2D, '-', '_' }, { 0x2E, '=', '+' }, { 0x2F, '[', '{' }, { 0x30, ']', '}' },
        { 0x31, '\\', '|' }, { 0x33, ';', ':' }, { 0x34, '\'', '"' }, { 0x35, '`', '~' }, { 0x36, ',', '<' },
        { 0x37, '.', '>' }, { 0x38, '/', '?' },
    };
    if (u >= 0x04 && u <= 0x1D) return (uint32_t)((shift ? 'A' : 'a') + (u - 0x04));
    if (u >= 0x1E && u <= 0x27) return (uint32_t)(shift ? num_sh[u - 0x1E] : num[u - 0x1E]);
    for (size_t i = 0; i < sizeof sym / sizeof sym[0]; i++) {
        if (sym[i].u == u) return (uint32_t)(shift ? sym[i].s : sym[i].c);
    }
    switch (u) {
    case 0x28: case 0x58: return T5_KEY_ENTER;
    case 0x29: return T5_KEY_ESC;
    case 0x2A: return T5_KEY_BACKSPACE;
    case 0x2B: return shift ? T5_KEY_PREV : T5_KEY_NEXT;
    case 0x4C: return T5_KEY_DEL;
    case 0x4A: return T5_KEY_HOME;
    case 0x4D: return T5_KEY_END;
    case 0x4F: return T5_KEY_RIGHT;
    case 0x50: return T5_KEY_LEFT;
    case 0x51: return T5_KEY_DOWN;
    case 0x52: return T5_KEY_UP;
    default: return 0;
    }
}

