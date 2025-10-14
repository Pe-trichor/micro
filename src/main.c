#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>

#include <nrfx_pdm.h>
#include <hal/nrf_pdm.h>

/* —— 接线
 * CLK=P1.05, DIN=P1.07
 */
#define PDM_CLK_PIN  37
#define PDM_DIN_PIN  39

#define FRAME_SAMPLES 1600
static int16_t pdm_buf_a[FRAME_SAMPLES * 2];
static int16_t pdm_buf_b[FRAME_SAMPLES * 2];

static volatile bool buf_ready = false;
static int16_t *last_released = NULL;

/* —— 显式声明 NRFX 的 PDM IRQ 处理入口（通用函数名，各版本存在） —— */
void nrfx_pdm_irq_handler(void);

/* —— 强制把 pdm0 的中断连接到 nrfx —— */
static void connect_pdm_irq(void)
{
    /* 注意：IRQ_CONNECT 需要“编译期常量”，不能用中间变量 */
    IRQ_CONNECT(DT_IRQN(DT_NODELABEL(pdm0)),
                DT_IRQ(DT_NODELABEL(pdm0), priority),
                nrfx_pdm_irq_handler,
                0,
                0);
    irq_enable(DT_IRQN(DT_NODELABEL(pdm0)));
}

/* PDM 事件回调（在中断上下文被调用，但这里只设置标志位，不打印大串） */
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
    }
}

/* 打印左声道前 16 个样本（奇数索引 1,3,5,...） */
static void dump_right_16(int16_t *stereo, size_t count_stereo)
{
    printk("Right[0..15]: ");
    int printed = 0;
    for (size_t i = 1; i < count_stereo && printed < 16; i += 2) {
        printk("%hd ", stereo[i]);   // 左声道=偶数位
        printed++;
    }
    printk("\n");
}

/* 计算左声道 RMS/Peak（偶数索引） */
static void analyze_and_print_right(int16_t *stereo_interleaved, size_t count_stereo)
{
    int32_t sumsq = 0;
    int16_t peak = 0;
    size_t n = 0;

    for (size_t i = 0; i < count_stereo; i += 2) { // 左声道=0,2,4,...
        int16_t s = stereo_interleaved[i];
        int32_t a = s >= 0 ? s : -s;
        if (a > peak) peak = (int16_t)a;
        sumsq += (int32_t)s * (int32_t)s;
        n++;
    }
    uint32_t mean = (uint32_t)(sumsq / (n ? n : 1));

    // 简单整数开方
    uint32_t x = mean, r = 0;
    for (int i = 15; i >= 0; --i) {
        uint32_t t = (r << (i + 1)) + (1u << (2*i));
        if (t <= x) { x -= t; r |= 1u << i; }
    }
    printk("R: RMS=%u, Peak=%d\n", r, peak);
    dump_right_16(stereo_interleaved, count_stereo);
}


int main(void)
{
    printk("HELLO UART 115200 OK\n");
    printk("PDM capture start\n");

    nrfx_pdm_config_t cfg = NRFX_PDM_DEFAULT_CONFIG(PDM_CLK_PIN, PDM_DIN_PIN);
    cfg.mode       = NRF_PDM_MODE_STEREO;      /* 右时隙（SELECT=GND） */
    cfg.ratio      = NRF_PDM_RATIO_64X;        /* fs ≈ clock/ratio/2 */
    cfg.clock_freq = NRF_PDM_FREQ_1067K;       /* v2.7.0 可用：~8.34 kHz/声道 */
    cfg.gain_l     = 0x28;
    cfg.gain_r     = 0x28;

    nrfx_err_t err = nrfx_pdm_init(&cfg, pdm_handler);
    if (err != NRFX_SUCCESS) {
        printk("nrfx_pdm_init failed: %d\n", err);
        return 0;
    }

    /* ★ 关键：强制把 IRQ 38 挂到 nrfx 的处理函数 */
    connect_pdm_irq();

    /* 预投双缓冲 */
    nrfx_pdm_buffer_set(pdm_buf_a, ARRAY_SIZE(pdm_buf_a));
    nrfx_pdm_buffer_set(pdm_buf_b, ARRAY_SIZE(pdm_buf_b));

    nrfx_pdm_start();//启动采样

    while (1) {
        if (buf_ready) {
         buf_ready = false;
         analyze_and_print_right(last_released, FRAME_SAMPLES * 2);
    }

        k_sleep(K_MSEC(1));
    }
    return 0;
}
