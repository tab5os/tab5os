/* SPDX-License-Identifier: Apache-2.0
 * HID report descriptors built from parts, and report packing (t5_hid/hid.h).
 * Each part is its application collection split where the report ID goes.
 */
#include <string.h>
#include "t5_hid/hid.h"

typedef struct {
    uint32_t part;
    uint8_t id;
    const uint8_t *head;      /* up to and including the collection that takes the ID */
    uint8_t head_len;
    const uint8_t *body;
    uint8_t body_len;
} part_t;

static const uint8_t KB_HEAD[] = { 0x05, 0x01, 0x09, 0x06, 0xa1, 0x01 };
static const uint8_t KB_BODY[] = {
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,                                   /* 8 modifier bits */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,           /* reserved byte */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02,                                   /* 5 LEDs out */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,           /* LED padding */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00,
    0x81, 0x00,                                   /* 6 key array */
    0xc0,
};

static const uint8_t MOUSE_HEAD[] = { 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01 };
static const uint8_t MOUSE_BODY[] = {
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x81, 0x02,                                   /* 5 buttons */
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,           /* padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02,
    0x81, 0x06,                                   /* x, y: 16-bit relative */
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x06,                                   /* wheel */
    0x05, 0x0c, 0x0a, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x06,                                   /* AC Pan */
    0xc0, 0xc0,
};

static const uint8_t CC_HEAD[] = { 0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01 };
static const uint8_t CC_BODY[] = {
    0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03, 0x75, 0x10, 0x95, 0x01,
    0x81, 0x00,                                   /* one 16-bit usage */
    0xc0,
};

static const uint8_t PAD_HEAD[] = { 0x05, 0x01, 0x09, 0x05, 0xa1, 0x01 };
static const uint8_t PAD_BODY[] = {
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10,
    0x81, 0x02,                                   /* 16 buttons */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x04,
    0x81, 0x02,                                   /* sticks: x, y left; z, rz right */
    0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02,
    0x81, 0x02,                                   /* triggers: rx, ry */
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01,
    0x81, 0x42,                                   /* hat, null state */
    0x65, 0x00, 0x45, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01,   /* padding */
    0xc0,
};

static const uint8_t ABS_HEAD[] = { 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01 };
static const uint8_t ABS_BODY[] = {
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02,                                   /* 3 buttons */
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,           /* padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02,
    0x81, 0x02,                                   /* x, y: absolute */
    0xc0, 0xc0,
};

#define PART(p, i, h, b) { p, i, h, sizeof h, b, sizeof b }
static const part_t PARTS[] = {
    PART(T5_HID_PART_KEYBOARD, T5_HID_ID_KEYBOARD, KB_HEAD, KB_BODY),
    PART(T5_HID_PART_MOUSE, T5_HID_ID_MOUSE, MOUSE_HEAD, MOUSE_BODY),
    PART(T5_HID_PART_CONSUMER, T5_HID_ID_CONSUMER, CC_HEAD, CC_BODY),
    PART(T5_HID_PART_GAMEPAD, T5_HID_ID_GAMEPAD, PAD_HEAD, PAD_BODY),
    PART(T5_HID_PART_ABSOLUTE, T5_HID_ID_ABSOLUTE, ABS_HEAD, ABS_BODY),
};
#define NPARTS (sizeof PARTS / sizeof PARTS[0])

static bool parts_valid(uint32_t parts)
{
    uint32_t p = parts & ~(uint32_t)T5_HID_NO_IDS;
    if (!p || (p & ~(uint32_t)T5_HID_PART_ALL)) return false;
    if ((parts & T5_HID_NO_IDS) && (p & (p - 1))) return false;   /* NO_IDS: one part only */
    return true;
}

int t5_hid_descriptor(uint32_t parts, uint8_t *out, size_t cap)
{
    if (!parts_valid(parts) || (!out && cap)) return T5_ERR_INVALID_ARG;
    bool ids = !(parts & T5_HID_NO_IDS);
    size_t n = 0;
    for (size_t i = 0; i < NPARTS; i++) {
        const part_t *p = &PARTS[i];
        if (!(parts & p->part)) continue;
        size_t need = p->head_len + (ids ? 2u : 0u) + p->body_len;
        if (n + need > cap) return T5_ERR_NO_MEM;
        memcpy(out + n, p->head, p->head_len);
        n += p->head_len;
        if (ids) {
            out[n++] = 0x85;
            out[n++] = p->id;
        }
        memcpy(out + n, p->body, p->body_len);
        n += p->body_len;
    }
    return (int)n;
}

int t5_hid_report_id(uint32_t parts, uint32_t part)
{
    if (!parts_valid(parts) || !(parts & part) || (part & (part - 1))) return -1;
    if (parts & T5_HID_NO_IDS) return 0;
    for (size_t i = 0; i < NPARTS; i++) {
        if (PARTS[i].part == part) return PARTS[i].id;
    }
    return -1;
}

/* ---- packing -------------------------------------------------------------- */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int16_t clamp16(int16_t v)
{
    return v < -32767 ? -32767 : v;
}

void t5_hid_pack_keyboard(const t5_hid_keyboard_t *k, uint8_t out[T5_HID_KEYBOARD_LEN])
{
    out[0] = k->mods;
    out[1] = 0;
    memcpy(out + 2, k->keys, 6);
}

void t5_hid_pack_mouse(const t5_hid_mouse_t *m, uint8_t out[T5_HID_MOUSE_LEN])
{
    out[0] = m->buttons & 0x1f;
    put16(out + 1, (uint16_t)clamp16(m->dx));
    put16(out + 3, (uint16_t)clamp16(m->dy));
    out[5] = (uint8_t)(m->wheel < -127 ? -127 : m->wheel);
    out[6] = (uint8_t)(m->pan < -127 ? -127 : m->pan);
}

void t5_hid_pack_consumer(uint16_t usage, uint8_t out[T5_HID_CONSUMER_LEN])
{
    put16(out, usage > 0x3ff ? 0 : usage);
}

static uint8_t axis8(int8_t v)
{
    return (uint8_t)(v < -127 ? -127 : v);
}

void t5_hid_pack_gamepad(const t5_hid_gamepad_t *g, uint8_t out[T5_HID_GAMEPAD_LEN])
{
    put16(out, g->buttons);
    out[2] = axis8(g->lx);
    out[3] = axis8(g->ly);
    out[4] = axis8(g->rx);
    out[5] = axis8(g->ry);
    out[6] = g->lt;
    out[7] = g->rt;
    out[8] = g->hat > 7 ? T5_HID_HAT_NONE : g->hat;
}

void t5_hid_pack_absolute(const t5_hid_absolute_t *a, uint8_t out[T5_HID_ABSOLUTE_LEN])
{
    out[0] = a->buttons & 0x07;
    put16(out + 1, a->x > 32767 ? 32767 : a->x);
    put16(out + 3, a->y > 32767 ? 32767 : a->y);
}
