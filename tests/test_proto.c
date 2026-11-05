/**
 * test_proto.c —— 协议解析核心的 PC 侧单元测试
 *
 * 为什么要在 PC 上测：
 *   解析器是纯 C，不依赖 HAL/FreeRTOS/串口。先在 PC 上把状态机测到全绿，
 *   再搬进 CubeIDE。到板上就只剩一个变量："串口能不能收到字节"。
 *   把"协议对不对"和"串口通不通"分开调，是嵌入式少踩坑的核心方法。
 *
 * 编译运行（MinGW / gcc）：
 *   gcc -std=c99 -Wall -Wextra -O2 -o test_proto.exe test_proto.c proto_parser.c
 *   .\test_proto.exe
 *
 * 期望：最后一行 "ALL PASS (n checks)"
 */
#include <stdio.h>
#include <string.h>
#include "proto.h"

/* ------------------------------------------------------------------ 断言 */
static int g_pass = 0;
static int g_fail = 0;
static const char *g_case = "";

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (cond) {                                             \
            g_pass++;                                           \
        } else {                                                \
            g_fail++;                                           \
            printf("    FAIL [%s] line %d: ", g_case, __LINE__); \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

#define BEGIN(name)         \
    do {                    \
        g_case = (name);    \
        printf("  - %s\n", (name)); \
    } while (0)

/* ------------------------------------------------------------------ 工具 */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hex2bytes(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    while (*s) {
        int v, k;
        while (*s == ' ') s++;
        if (!*s) break;
        v = hexval(*s++);
        k = hexval(*s++);
        if (v < 0 || k < 0 || n >= cap) return -1;
        out[n++] = (uint8_t)((v << 4) | k);
    }
    return n;
}

/* 一个"被测对象 + 模拟时钟"的小夹具 */
typedef struct {
    proto_parser_t p;
    nav_state_t    s;
    uint32_t       ms;
    int            n_ok;
    int            n_drop;
} ctx_t;

static void ctx_reset(ctx_t *c)
{
    memset(c, 0, sizeof(*c));
    proto_init(&c->p);
}

static proto_ret_t ctx_feed(ctx_t *c, uint8_t b, uint32_t step_ms)
{
    proto_ret_t r;
    c->ms += step_ms;
    r = proto_feed(&c->p, b, c->ms, &c->s);
    if (r == PROTO_OK) {
        c->n_ok++;
    } else if (r != PROTO_WAIT) {
        c->n_drop++;
    }
    return r;
}

static void ctx_feed_buf(ctx_t *c, const uint8_t *buf, int n, uint32_t step_ms)
{
    int i;
    for (i = 0; i < n; i++) {
        (void)ctx_feed(c, buf[i], step_ms);
    }
}

/* -------------------------------------------------------- docs/protocol.md 的 8 条样例帧
 * 把它当成"可执行规格"：文档写的字节，解析出来必须是这些数。 */
typedef struct {
    const char *name;
    const char *hex;
    uint8_t  seq, turn, speed, flags;
    uint16_t dist, remain_dam, eta;
} sample_t;

static const sample_t k_samples[] = {
    { "右转350m",  "A5 5A 01 01 00 02 5E 01 2D 02 D8 04 12 00 AA AD", 0, 2, 45, 0x02,  350, 1240, 18 },
    { "右转120m",  "A5 5A 01 01 01 02 78 00 26 02 BA 04 11 00 77 A9", 1, 2, 38, 0x02,  120, 1210, 17 },
    { "左转800m",  "A5 5A 01 01 02 01 20 03 34 02 7E 04 10 00 9B 1E", 2, 1, 52, 0x02,  800, 1150, 16 },
    { "掉头40m",   "A5 5A 01 01 03 05 28 00 0F 02 4C 04 10 00 C0 8E", 3, 5, 15, 0x02,   40, 1100, 16 },
    { "直行9990m", "A5 5A 01 01 04 00 06 27 3C 02 D4 03 0E 00 08 50", 4, 0, 60, 0x02, 9990,  980, 14 },
    { "到达30m",   "A5 5A 01 01 05 06 1E 00 0C 02 14 00 01 00 E9 9D", 5, 6, 12, 0x02,   30,   20,  1 },
    { "偏航",      "A5 5A 01 01 06 02 58 02 2C 03 84 03 0D 00 6E 28", 6, 2, 44, 0x03,  600,  900, 13 },
    { "静音",      "A5 5A 01 01 07 01 FA 00 1E 06 20 03 0C 00 69 CE", 7, 1, 30, 0x06,  250,  800, 12 },
};

#define N_SAMPLES (int)(sizeof(k_samples) / sizeof(k_samples[0]))

/* ------------------------------------------------------------------ 用例 */

static void test_crc_vector(void)
{
    static const uint8_t v[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    BEGIN("CRC-16/MODBUS 标准向量");
    CHECK(proto_crc16(v, 9) == 0x4B37u, "crc=0x%04X, expect 0x4B37",
          proto_crc16(v, 9));
}

static void test_samples(void)
{
    int i;
    BEGIN("8 条样例帧逐个解析（文档即可执行规格）");
    for (i = 0; i < N_SAMPLES; i++) {
        uint8_t f[64];
        int n = hex2bytes(k_samples[i].hex, f, (int)sizeof(f));
        ctx_t c;
        CHECK(n == 16, "[%s] 帧长 %d != 16", k_samples[i].name, n);
        if (n != 16) continue;
        ctx_reset(&c);
        ctx_feed_buf(&c, f, n, 1);
        CHECK(c.n_ok == 1, "[%s] 应解析出 1 帧，实际 %d（丢弃 %d）",
              k_samples[i].name, c.n_ok, c.n_drop);
        CHECK(c.s.seq == k_samples[i].seq, "[%s] seq=%u", k_samples[i].name, c.s.seq);
        CHECK(c.s.turn == k_samples[i].turn, "[%s] turn=%u", k_samples[i].name, c.s.turn);
        CHECK(c.s.dist_m == k_samples[i].dist, "[%s] dist=%u", k_samples[i].name, c.s.dist_m);
        CHECK(c.s.speed_kmh == k_samples[i].speed, "[%s] speed=%u", k_samples[i].name, c.s.speed_kmh);
        CHECK(c.s.flags == k_samples[i].flags, "[%s] flags=0x%02X", k_samples[i].name, c.s.flags);
        CHECK(c.s.remain_dam == k_samples[i].remain_dam, "[%s] remain=%u (10m)",
              k_samples[i].name, c.s.remain_dam);
        CHECK(c.s.eta_min == k_samples[i].eta, "[%s] eta=%u", k_samples[i].name, c.s.eta_min);
    }
}

static void test_builder(void)
{
    uint8_t f[PROTO_FRAME_LEN], want[64];
    nav_state_t s;
    int n;
    BEGIN("proto_build 组帧与文档字节完全一致");
    memset(&s, 0, sizeof(s));
    s.seq = 0; s.turn = TURN_RIGHT; s.dist_m = 350; s.speed_kmh = 45;
    s.flags = FLAG_FIX_OK; s.remain_dam = 1240; s.eta_min = 18;
    proto_build(f, &s);
    n = hex2bytes(k_samples[0].hex, want, (int)sizeof(want));
    CHECK(n == 16, "样例帧解析失败");
    CHECK(memcmp(f, want, 16) == 0, "组帧结果与文档不符");
}

static void test_tcp_like_glue(void)
{
    uint8_t f1[64], f2[64], both[64];
    int n1, n2;
    ctx_t c;
    BEGIN("粘包：两帧连在一个缓冲里");
    n1 = hex2bytes(k_samples[0].hex, f1, (int)sizeof(f1));
    n2 = hex2bytes(k_samples[1].hex, f2, (int)sizeof(f2));
    memcpy(both, f1, (size_t)n1);
    memcpy(both + n1, f2, (size_t)n2);
    ctx_reset(&c);
    ctx_feed_buf(&c, both, n1 + n2, 1);
    CHECK(c.n_ok == 2, "应解析 2 帧，实际 %d", c.n_ok);
    CHECK(c.s.seq == 1, "最后一帧 seq 应为 1，实际 %u", c.s.seq);
}

static void test_noise_prefix(void)
{
    uint8_t f1[64], buf[80];
    int n1;
    ctx_t c;
    BEGIN("帧前有随机噪声字节");
    n1 = hex2bytes(k_samples[0].hex, f1, (int)sizeof(f1));
    buf[0] = 0xFF; buf[1] = 0x00; buf[2] = 0x13;
    memcpy(buf + 3, f1, (size_t)n1);
    ctx_reset(&c);
    ctx_feed_buf(&c, buf, n1 + 3, 1);
    CHECK(c.n_ok == 1, "应解析 1 帧，实际 %d", c.n_ok);
}

static void test_double_a5(void)
{
    uint8_t f1[64], buf[80];
    int n1;
    ctx_t c;
    BEGIN("帧头前面多一个 0xA5（A5 A5 5A ...）");
    n1 = hex2bytes(k_samples[0].hex, f1, (int)sizeof(f1));
    buf[0] = 0xA5;
    memcpy(buf + 1, f1, (size_t)n1);
    ctx_reset(&c);
    ctx_feed_buf(&c, buf, n1 + 1, 1);
    CHECK(c.n_ok == 1, "应解析 1 帧，实际 %d", c.n_ok);
    CHECK(c.s.dist_m == 350, "dist 应为 350，实际 %u", c.s.dist_m);
}

static void test_bad_crc_then_good(void)
{
    uint8_t f1[64], f2[64], both[64];
    int n1, n2;
    ctx_t c;
    BEGIN("错 CRC 的坏帧后面紧跟好帧（只丢 1 字节，不许连坐）");
    n1 = hex2bytes(k_samples[0].hex, f1, (int)sizeof(f1));
    n2 = hex2bytes(k_samples[1].hex, f2, (int)sizeof(f2));
    f1[6] ^= 0xFF;                    /* 改坏距离低字节，CRC 必然不匹配 */
    memcpy(both, f1, (size_t)n1);
    memcpy(both + n1, f2, (size_t)n2);
    ctx_reset(&c);
    ctx_feed_buf(&c, both, n1 + n2, 1);
    CHECK(c.n_ok == 1, "应只有 1 帧有效，实际 %d", c.n_ok);
    CHECK(c.s.seq == 1, "好帧 seq 应为 1，实际 %u", c.s.seq);
    CHECK(c.p.n_drop_crc == 1, "应记 1 次 CRC 错，实际 %u", c.p.n_drop_crc);
}

static void test_half_frame_timeout(void)
{
    uint8_t f1[64];
    int n1;
    ctx_t c;
    BEGIN("半帧后停顿 >50ms：丢弃，不许拼出假帧");
    n1 = hex2bytes(k_samples[0].hex, f1, (int)sizeof(f1));
    ctx_reset(&c);
    ctx_feed_buf(&c, f1, 10, 1);
    c.ms += 60;
    proto_tick(&c.p, c.ms);
    ctx_feed_buf(&c, f1 + 10, n1 - 10, 0);
    CHECK(c.n_ok == 0, "超时后不应解析出帧，实际 %d", c.n_ok);
    CHECK(c.p.n_timeout >= 1, "应记 1 次超时，实际 %u", c.p.n_timeout);
}

static void test_gap_below_timeout(void)
{
    uint8_t f1[64];
    int n1;
    ctx_t c;
    BEGIN("半帧后停顿 40ms（未超时）：仍应拼完");
    n1 = hex2bytes(k_samples[0].hex, f1, (int)sizeof(f1));
    ctx_reset(&c);
    ctx_feed_buf(&c, f1, 10, 1);
    c.ms += 40;
    ctx_feed_buf(&c, f1 + 10, n1 - 10, 0);
    CHECK(c.n_ok == 1, "应解析 1 帧，实际 %d", c.n_ok);
}

static void test_seq_wrap(void)
{
    int i;
    ctx_t c;
    BEGIN("seq 255 -> 0 回绕是正常的，不是丢包");
    ctx_reset(&c);
    for (i = 0; i < 4; i++) {
        static const uint8_t seqs[4] = { 254, 255, 0, 1 };
        uint8_t f[PROTO_FRAME_LEN];
        nav_state_t s;
        memset(&s, 0, sizeof(s));
        s.seq = seqs[i];
        s.turn = TURN_STRAIGHT;
        s.dist_m = 100;
        proto_build(f, &s);
        ctx_feed_buf(&c, f, PROTO_FRAME_LEN, 1);
    }
    CHECK(c.n_ok == 4, "应解析 4 帧，实际 %d", c.n_ok);
    CHECK(c.s.seq == 1, "最后一帧 seq 应为 1，实际 %u", c.s.seq);
}

static void test_garbage_stream(void)
{
    int i;
    ctx_t c;
    BEGIN("500 字节全 0x00：不崩、不出假帧");
    ctx_reset(&c);
    for (i = 0; i < 500; i++) (void)ctx_feed(&c, 0x00u, 1);
    CHECK(c.n_ok == 0, "不应解析出帧，实际 %d", c.n_ok);
    CHECK(c.p.st == P_IDLE, "应回到 IDLE，实际 %d", (int)c.p.st);
}

static void test_fake_header(void)
{
    uint8_t junk[16] = { 0xA5, 0x5A, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                         0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 };
    uint8_t f1[64];
    int n1;
    ctx_t c;
    BEGIN("假帧头（A5 5A 后面全是垃圾）后跟好帧：能恢复");
    n1 = hex2bytes(k_samples[0].hex, f1, (int)sizeof(f1));
    ctx_reset(&c);
    ctx_feed_buf(&c, junk, 16, 1);
    ctx_feed_buf(&c, f1, n1, 1);
    CHECK(c.n_ok == 1, "应解析 1 帧，实际 %d", c.n_ok);
    CHECK(c.n_drop >= 1, "假帧头应被丢弃，实际丢弃 %d", c.n_drop);
}

static void test_steady_stream(void)
{
    int i;
    ctx_t c;
    BEGIN("5Hz 连续 200 帧（模拟真实链路）");
    ctx_reset(&c);
    for (i = 0; i < 200; i++) {
        uint8_t f[PROTO_FRAME_LEN];
        nav_state_t s;
        memset(&s, 0, sizeof(s));
        s.seq = (uint8_t)(i & 0xFF);
        s.turn = TURN_SLIGHT_RIGHT;
        s.dist_m = (uint16_t)(1000 - i);
        s.speed_kmh = 47;
        s.flags = FLAG_FIX_OK;
        s.remain_dam = 1200;
        s.eta_min = 18;
        proto_build(f, &s);
        ctx_feed_buf(&c, f, PROTO_FRAME_LEN, 1);
        c.ms += 200;                 /* 帧间隔 200ms */
    }
    CHECK(c.n_ok == 200, "应解析 200 帧，实际 %d", c.n_ok);
    CHECK(c.s.seq == (uint8_t)199, "最后一帧 seq 应为 199，实际 %u", c.s.seq);
    CHECK(c.s.dist_m == 801, "最后一帧 dist 应为 801（1000-199），实际 %u", c.s.dist_m);
    CHECK(c.p.n_timeout == 0, "不应有超时，实际 %u", c.p.n_timeout);
}

/* -------------------------------------------------------- 练习（做完再进 CubeIDE）
 * 1) 把 proto_parser.c 里的 PROTO_TIMEOUT_MS 改成 5000，重跑：
 *    哪个用例变红？为什么这正好说明"超时"是必需的？
 * 2) 把 resync() 改成 parser_reset(p)（即"CRC 错就清空缓冲区"），重跑：
 *    观察「错 CRC 的坏帧后面紧跟好帧」这一条。把现象和原因写进 docs/log.md。
 * 3) 把 CRC 改成"各字节简单相加"，重跑：哪些用例变红？说明为什么弱校验会漏帧。
 * 4) 加一个用例：帧头被撕在两次 DMA 传输之间（先喂 A5，隔 10ms 再喂 5A）。
 */
int main(void)
{
    printf("proto parser unit tests\n");
    test_crc_vector();
    test_samples();
    test_builder();
    test_tcp_like_glue();
    test_noise_prefix();
    test_double_a5();
    test_bad_crc_then_good();
    test_half_frame_timeout();
    test_gap_below_timeout();
    test_seq_wrap();
    test_garbage_stream();
    test_fake_header();
    test_steady_stream();

    printf("\n  passed: %d, failed: %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("ALL PASS (%d checks)\n", g_pass);
        return 0;
    }
    printf("FAILED\n");
    return 1;
}
