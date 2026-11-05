/**
 * proto_parser.c —— 状态机拆帧 + CRC 校验
 *
 * 这个文件里只有一件"聪明"的事：CRC 或版本校验失败时，**不清空缓冲区**，
 * 而是只丢 1 个字节，然后在剩下的字节里重新找帧头。
 * 原因：坏帧后面很可能紧跟着一个已经收进缓冲的好帧，清空会把它一起丢掉，
 * 表现就是"错一帧，连续丢好几帧"。
 */
#include "proto.h"
#include <string.h>

uint16_t proto_crc16(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFFu;
    while (n--) {
        crc ^= (uint16_t)(*p++);
        for (uint8_t i = 0; i < 8u; i++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u)
                             : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

void proto_init(proto_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->st = P_IDLE;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void parser_reset(proto_parser_t *p)
{
    p->len = 0;
    p->st  = P_IDLE;
}

/* 校验失败后重同步：从下标 1 起找下一个 A5 5A，找到就把剩余字节搬到开头。
 * 这等价于"丢掉最前面 1 个字节后重跑状态机"，只是一次做完。 */
static void resync(proto_parser_t *p)
{
    for (uint8_t i = 1u; (uint8_t)(i + 1u) < p->len; i++) {
        if (p->buf[i] == PROTO_H0 && p->buf[i + 1u] == PROTO_H1) {
            uint8_t n = (uint8_t)(p->len - i);
            memmove(p->buf, p->buf + i, n);
            p->len = n;
            p->st  = P_RECV;
            return;
        }
    }
    /* 尾部残留一个 0xA5 —— 可能是下一帧帧头的前半个，留给 P_WAIT_LSB */
    if (p->len > 0u && p->buf[p->len - 1u] == PROTO_H0) {
        p->buf[0] = PROTO_H0;
        p->len    = 1u;
        p->st     = P_WAIT_LSB;
        return;
    }
    parser_reset(p);
}

static void decode(const uint8_t *b, nav_state_t *s)
{
    s->ver        = b[2];
    s->type       = b[3];
    s->seq        = b[4];
    s->turn       = b[5];
    s->dist_m     = rd16(&b[6]);
    s->speed_kmh  = b[8];
    s->flags      = b[9];
    s->remain_dam = rd16(&b[10]);
    s->eta_min    = rd16(&b[12]);
    s->crc_rx     = rd16(&b[14]);
}

proto_ret_t proto_feed(proto_parser_t *p, uint8_t byte, uint32_t now_ms,
                       nav_state_t *out)
{
    /* 半帧卡住超过 50ms 就丢弃，防止波特率错/线路断在半路时状态机永久卡死 */
    if (p->st != P_IDLE && (uint32_t)(now_ms - p->t_byte) > PROTO_TIMEOUT_MS) {
        p->n_timeout++;
        parser_reset(p);
    }
    p->t_byte = now_ms;

    switch (p->st) {
    case P_IDLE:
        if (byte == PROTO_H0) {
            p->buf[0] = byte;
            p->len    = 1u;
            p->st     = P_WAIT_LSB;
        }
        return PROTO_WAIT;

    case P_WAIT_LSB:
        if (byte == PROTO_H1) {
            p->buf[1] = byte;
            p->len    = 2u;
            p->st     = P_RECV;
        } else if (byte == PROTO_H0) {
            p->len = 1u;          /* 又是 0xA5：留在本态重新等 0x5A */
        } else {
            parser_reset(p);
        }
        return PROTO_WAIT;

    case P_RECV:
    default:
        break;
    }

    p->buf[p->len++] = byte;
    if (p->len < PROTO_FRAME_LEN) {
        return PROTO_WAIT;
    }

    /* 收齐 16 字节，原地判断 */
    if (proto_crc16(p->buf, 14u) != rd16(&p->buf[14])) {
        p->n_drop_crc++;
        resync(p);
        return PROTO_DROP_CRC;
    }
    if (p->buf[2] != PROTO_VER) {
        p->n_drop_ver++;
        resync(p);
        return PROTO_DROP_VER;
    }
    if (p->buf[3] != PROTO_TYPE_STATE) {
        p->n_drop_type++;
        resync(p);
        return PROTO_DROP_TYPE;
    }

    decode(p->buf, out);
    p->n_ok++;
    parser_reset(p);
    return PROTO_OK;
}

void proto_tick(proto_parser_t *p, uint32_t now_ms)
{
    if (p->st != P_IDLE && (uint32_t)(now_ms - p->t_byte) > PROTO_TIMEOUT_MS) {
        p->n_timeout++;
        parser_reset(p);
    }
}

void proto_build(uint8_t out[PROTO_FRAME_LEN], const nav_state_t *s)
{
    out[0] = PROTO_H0;
    out[1] = PROTO_H1;
    out[2] = PROTO_VER;
    out[3] = PROTO_TYPE_STATE;
    out[4] = s->seq;
    out[5] = s->turn;
    wr16(&out[6],  s->dist_m);
    out[8] = s->speed_kmh;
    out[9] = s->flags;
    wr16(&out[10], s->remain_dam);
    wr16(&out[12], s->eta_min);
    wr16(&out[14], proto_crc16(out, 14u));
}
