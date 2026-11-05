/**
 * proto.h —— 导航状态帧协议核心（V1）
 *
 * 设计约束（记住这三条，面试会问）：
 *   1. 定长 16 字节。没有长度字段，没有转义，状态机退化成"计数到 16 就校验"。
 *   2. 不依赖 HAL / FreeRTOS / 串口。传进来的只是一个字节 + 当前毫秒时间。
 *      所以这个文件能在 PC 上用 gcc 跑单元测试，也能原样放进 CubeIDE。
 *   3. 解析器只负责"字节流 -> 结构体"，不负责显示、不负责重连判定。
 *      上层拿 PROTO_OK 才算一帧有效数据。
 *
 * 字段定义见 docs/protocol.md。所有多字节字段小端序。
 */
#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

#define PROTO_FRAME_LEN   16u   /* 状态帧固定长度 */
#define PROTO_TIMEOUT_MS  50u   /* 半帧收不齐的超时 */

#define PROTO_H0          0xA5u
#define PROTO_H1          0x5Au
#define PROTO_VER         0x01u
#define PROTO_TYPE_STATE  0x01u

/* 转向枚举：必须与 tools/sim_sender.py、手机端严格一致，改动同步升 ver */
typedef enum {
    TURN_STRAIGHT      = 0,
    TURN_LEFT          = 1,
    TURN_RIGHT         = 2,
    TURN_SLIGHT_LEFT   = 3,
    TURN_SLIGHT_RIGHT  = 4,
    TURN_UTURN         = 5,
    TURN_ARRIVE        = 6
} turn_t;

/* flags 位定义 */
#define FLAG_OFF_ROUTE    0x01u
#define FLAG_FIX_OK       0x02u
#define FLAG_MUTE         0x04u

/* 一帧解析出来的导航状态。UI 任务只读这个结构体，不碰字节。 */
typedef struct {
    uint8_t  ver;
    uint8_t  type;
    uint8_t  seq;          /* 0~255 循环，用于统计丢包，不用于重传 */
    uint8_t  turn;         /* turn_t */
    uint16_t dist_m;       /* 距下一动作距离，米 */
    uint8_t  speed_kmh;
    uint8_t  flags;
    uint16_t remain_dam;   /* 剩余总里程，单位 10 米 */
    uint16_t eta_min;
    uint16_t crc_rx;       /* 收到的 CRC，调试用 */
} nav_state_t;

/* 喂字节的返回值。只有 PROTO_OK 表示拿到一帧有效数据。 */
typedef enum {
    PROTO_OK = 0,       /* 收齐并通过校验，*out 有效 */
    PROTO_WAIT,         /* 还没收齐，继续喂 */
    PROTO_DROP_CRC,     /* CRC 错，已重同步 */
    PROTO_DROP_VER,     /* 版本不认识，已重同步 */
    PROTO_DROP_TYPE     /* 类型不支持，已重同步 */
} proto_ret_t;

typedef enum {
    P_IDLE = 0,     /* 丢字节，直到读到 0xA5 */
    P_WAIT_LSB,     /* 等 0x5A */
    P_RECV          /* 收剩余字节 */
} proto_state_t;

typedef struct {
    uint8_t      buf[PROTO_FRAME_LEN];
    uint8_t      len;
    proto_state_t st;
    uint32_t     t_byte;    /* 最近一次收到字节的时刻（ms） */
    /* 统计，打日志/做指标用 */
    uint32_t     n_ok;
    uint32_t     n_drop_crc;
    uint32_t     n_drop_ver;
    uint32_t     n_drop_type;
    uint32_t     n_timeout;
} proto_parser_t;

uint16_t    proto_crc16(const uint8_t *p, uint16_t n);

void        proto_init(proto_parser_t *p);

/* 每收到一个字节调一次。now_ms 由调用者提供（STM32 上用 HAL_GetTick()）。
 * 注意：timeout 检查在这里也做一遍，所以即使没人调 proto_tick 也不会卡死。 */
proto_ret_t proto_feed(proto_parser_t *p, uint8_t byte, uint32_t now_ms,
                       nav_state_t *out);

/* 可选：在 10ms 的任务/定时器里调一次，半帧超时后主动回到 IDLE。 */
void        proto_tick(proto_parser_t *p, uint32_t now_ms);

/* 组帧，只用于测试和回环自测（相当于 C 版的 sim_sender）。 */
void        proto_build(uint8_t out[PROTO_FRAME_LEN], const nav_state_t *s);

#endif /* PROTO_H */
