#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <nrfx_pdm.h>
#include <hal/nrf_pdm.h>

#include "SEGGER_RTT.h"    // ★ RTT 头文件
#include <string.h>
#include <stdint.h>

/* —— 接线：CLK=P1.05(37), DIN=P1.07(39) —— */
#define PDM_CLK_PIN   37
#define PDM_DIN_PIN   39

/* —— PDM 双缓冲：每帧“每通道”样本数 —— */
#define FRAME_SAMPLES 1600
static int16_t pdm_buf_a[FRAME_SAMPLES * 2];
static int16_t pdm_buf_b[FRAME_SAMPLES * 2];

/* —— 目标采样（右通道单声道）——
 * PDM: clock=~2.128 MHz, ratio=64 → 每通道 ≈ 2128000/(64*2)=16625 Hz
 * 为了“听感”不用非得精准，fs 写 16625 更严谨
 */
#define TARGET_FS_HZ     16625
#define TARGET_SECONDS   3
#define TARGET_SAMPLES   (TARGET_FS_HZ * TARGET_SECONDS)

/* —— 采集状态 —— */
static int16_t *mono_buf = NULL;
static volatile size_t mono_write = 0;
static volatile bool   capture_done = false;

static volatile bool   buf_ready = false;
static int16_t        *last_released = NULL;

/* —— NRFX IRQ 声明 —— */
void nrfx_pdm_irq_handler(void);

/* —— 把 pdm0 中断连接到 nrfx —— */
static void connect_pdm_irq(void)
{
    IRQ_CONNECT(DT_IRQN(DT_NODELABEL(pdm0)),
                DT_IRQ(DT_NODELABEL(pdm0), priority),
                nrfx_pdm_irq_handler,
                0, 0);
    irq_enable(DT_IRQN(DT_NODELABEL(pdm0)));
}

/* =========  RTT（二进制）相关 ========= */
#define RTT_BIN_CH  1                   // 使用上行通道 #1 发送二进制
static uint8_t rtt_bin_buf[8192];       // RTT 通道 1 的环形缓冲

/* 简单 CRC32（LE，多项式 0xEDB88320） */
static uint32_t crc32_le(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t m = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & m);
        }
    }
    return ~crc;
}

/* 阻塞发送（不丢数） */
static void rtt_send_block(const void *p, size_t n)
{
    const uint8_t *q = (const uint8_t*)p;
    while (n) {
        unsigned wr = SEGGER_RTT_Write(RTT_BIN_CH, q, n); // 阻塞直到写入
        q += wr;
        n -= wr;
        if (wr == 0) {
            /* 极端情况下给一点点喘息 */
            k_sleep(K_MSEC(1));
        }
    }
}

/* =========  PDM 事件回调 ========= */
//麦克风通过PDM接口采集数据后，，硬件会周期性触发中断，告诉硬件下一帧数据放到哪里，收到上一帧数据采完，把右声道的PCM样本提取，拼到大缓冲里
static void pdm_handler(nrfx_pdm_evt_t const *evt)
{
    if (evt->buffer_requested) {
        static bool toggle = false;
        (void)nrfx_pdm_buffer_set(toggle ? pdm_buf_a : pdm_buf_b, FRAME_SAMPLES * 2);
        toggle = !toggle;
    }
    if (evt->buffer_released) {
        last_released = evt->buffer_released;
        buf_ready = true;

        // 解交织右通道（奇数位）
        int16_t *st = evt->buffer_released;
        size_t   N  = FRAME_SAMPLES;
        for (size_t i = 0; i < N && mono_write < TARGET_SAMPLES; i++) {
            mono_buf[mono_write++] = st[i*2 + 1];
        }
        if (mono_write >= TARGET_SAMPLES) {
            capture_done = true;
        }
    }
}

int main(void)
{
    /* 1) 配置 RTT 通道 #1：阻塞模式（保证不丢） */
    SEGGER_RTT_ConfigUpBuffer(
        RTT_BIN_CH,
        "pcm-bin",
        rtt_bin_buf,
        sizeof(rtt_bin_buf),
        SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL
    );

    /* 2) 分配单声道缓冲 */
    mono_buf = k_malloc(TARGET_SAMPLES * sizeof(int16_t));
    if (!mono_buf) {
        /* 为避免干扰通道 1，尽量少打字；必要时你也可以完全禁用这句 */
        SEGGER_RTT_WriteString(0, "ERR: k_malloc mono_buf failed\n");
        return 0;
    }

    /* 3) PDM 初始化 */
    nrfx_pdm_config_t cfg = NRFX_PDM_DEFAULT_CONFIG(PDM_CLK_PIN, PDM_DIN_PIN);
    cfg.mode       = NRF_PDM_MODE_STEREO;       // 仍用双通道，只取右通道
    cfg.ratio      = NRF_PDM_RATIO_64X;
    cfg.clock_freq = NRF_PDM_FREQ_1280K;       // 约 2.128MHz → 每通道 ~16625 Hz
    cfg.gain_l     = 0x28;
    cfg.gain_r     = 0x28;

    if (nrfx_pdm_init(&cfg, pdm_handler) != NRFX_SUCCESS) {
        SEGGER_RTT_WriteString(0, "ERR: nrfx_pdm_init failed\n");
        return 0;
    }

    connect_pdm_irq();
    nrfx_pdm_buffer_set(pdm_buf_a, ARRAY_SIZE(pdm_buf_a));
    nrfx_pdm_buffer_set(pdm_buf_b, ARRAY_SIZE(pdm_buf_b));
    nrfx_pdm_start();

    /* 4) 等待采满（不再大量 printk，避免干扰） */
    while (!capture_done) {
        k_sleep(K_MSEC(1));
    }
    nrfx_pdm_stop();
    k_sleep(K_MSEC(20));

    /* 5) 发送头 + PCM + CRC （走 RTT 通道 1） */
    struct __attribute__((packed)) {
        uint8_t  magic[4];   // 'P''C''M''1'
        uint32_t fs_hz;      // 16625
        uint32_t n_samp;     // 单声道样本数
        uint32_t reserved;   // 0
    } hdr;

    hdr.magic[0]='P'; hdr.magic[1]='C'; hdr.magic[2]='M'; hdr.magic[3]='1';
    hdr.fs_hz    = TARGET_FS_HZ;
    hdr.n_samp   = (uint32_t)mono_write;
    hdr.reserved = 0;

    /* 为计算 CRC，把头+数据拼在一起 */
    uint32_t total = sizeof(hdr) + mono_write * sizeof(int16_t);
    uint8_t *tmp = k_malloc(total);
    if (!tmp) {
        SEGGER_RTT_WriteString(0, "ERR: k_malloc tmp failed\n");
        return 0;
    }
    memcpy(tmp, &hdr, sizeof(hdr));
    memcpy(tmp + sizeof(hdr), (uint8_t*)mono_buf, mono_write * sizeof(int16_t));
    uint32_t crc = crc32_le(tmp, total);

    /* —— 发送（仅走 BIN 通道 #1，不混文字）—— */
    rtt_send_block(tmp, total);
    rtt_send_block(&crc, sizeof(crc));

    k_free(tmp);
    k_free(mono_buf);

    /* 可选：在 console 0 发一句提示（不会写进 BIN 文件，因为我们抓的是通道 1） */
    SEGGER_RTT_WriteString(0, "DONE: PCM sent on RTT up-channel #1\n");
    return 0;
}
