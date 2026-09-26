/* ============================================================
 *  horno-fractal  —  ESP32-C3 SuperMini + OLED SSD1306 128x32
 * ------------------------------------------------------------
 *  Renderiza un Mandelbrot con zoom infinito usando aritmetica
 *  de punto fijo de 64 bits: eso mantiene el unico nucleo del
 *  C3 clavado al 100% a 160 MHz (calor maximo para probar
 *  disipadores) y ademas dibuja algo bonito.
 *
 *  Arriba, una barra de estado muestra:
 *    T<actual>C  ^<maxima>C  <fps>f  <spinner>
 *  La temperatura sale del sensor interno del propio chip, asi
 *  que ves en vivo cuanto baja al poner cada disipador.
 *
 *  Cableado (I2C):  SDA = GPIO8   SCL = GPIO9   addr 0x3C
 * ============================================================ */

#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"

/* ---------------- Configuracion de hardware ---------------- */
#define I2C_SDA      8
#define I2C_SCL      9
#define OLED_ADDR    0x3C
#define OLED_HZ      400000
#define OLED_W       128
#define OLED_H       32

/* ---------------- Parametros del fractal ------------------- */
#define FRAC     28                 /* punto fijo Q4.28            */
#define ONE      (1LL << FRAC)
#define MAXITER  160                /* mas iteraciones = mas calor */
#define FRACT_H  24                 /* alto del area de fractal    */
#define FRACT_Y  8                  /* empieza bajo la barra       */

static const char *TAG = "horno";

/* ---------------- Framebuffer 128x32 = 512 bytes ----------- */
static uint8_t fb[OLED_W * OLED_H / 8];

static inline void px(int x, int y)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    fb[(y >> 3) * OLED_W + x] |= (uint8_t)(1u << (y & 7));
}

/* ---------------- Fuente 5x7 (solo lo que uso) ------------- */
/* Cada glifo son 7 filas; los 5 bits bajos son las columnas,
 * bit4 = columna izquierda. Asi es facil revisarlo a ojo.     */
typedef struct { char c; uint8_t r[7]; } glyph_t;

static const glyph_t FONT[] = {
    {' ', {0,0,0,0,0,0,0}},
    {'0', {0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110}},
    {'1', {0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}},
    {'2', {0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111}},
    {'3', {0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110}},
    {'4', {0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}},
    {'5', {0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110}},
    {'6', {0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110}},
    {'7', {0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}},
    {'8', {0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}},
    {'9', {0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100}},
    {'.', {0,0,0,0,0,0b01100,0b01100}},
    {'-', {0,0,0,0b11111,0,0,0}},
    {'C', {0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110}},
    {'T', {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}},
    {'f', {0b00110,0b01000,0b11110,0b01000,0b01000,0b01000,0b01000}},
    {'^', {0b00100,0b01010,0b10001,0,0,0,0}},
    {'|', {0b00100,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}},
    {'/', {0b00001,0b00010,0b00100,0b00100,0b00100,0b01000,0b10000}},
    {'\\',{0b10000,0b01000,0b00100,0b00100,0b00100,0b00010,0b00001}},
};
#define NGLYPH (sizeof(FONT)/sizeof(FONT[0]))

static void draw_char(int x0, char c)
{
    const glyph_t *g = NULL;
    for (unsigned i = 0; i < NGLYPH; i++) if (FONT[i].c == c) { g = &FONT[i]; break; }
    if (!g) return;
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g->r[row] & (1u << (4 - col))) px(x0 + col, row);
}

static void draw_str(int x0, const char *s)
{
    int x = x0;
    while (*s) { draw_char(x, *s++); x += 6; }
}

/* Escribe un float con 1 decimal, termina en '\0', sin printf. */
static char *fmt1(char *p, float v)
{
    if (v < 0) { *p++ = '-'; v = -v; }
    int iv = (int)(v * 10.0f + 0.5f);
    int whole = iv / 10, frac = iv % 10;
    char t[8]; int n = 0;
    if (whole == 0) t[n++] = '0';
    while (whole) { t[n++] = (char)('0' + whole % 10); whole /= 10; }
    while (n) *p++ = t[--n];
    *p++ = '.';
    *p++ = (char)('0' + frac);
    *p = 0;
    return p;
}

/* ---------------- Driver minimo SSD1306 ------------------- */
static i2c_master_dev_handle_t dev;

static void oled_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[40];
    buf[0] = 0x00;                       /* Co=0, D/C=0 -> comandos */
    memcpy(buf + 1, cmds, n);
    i2c_master_transmit(dev, buf, n + 1, 200);
}

static void oled_flush(void)
{
    static const uint8_t win[] = {
        0x21, 0x00, 0x7F,                /* columnas 0..127 */
        0x22, 0x00, 0x03,                /* paginas  0..3   */
    };
    oled_cmds(win, sizeof(win));

    static uint8_t out[OLED_W * OLED_H / 8 + 1];
    out[0] = 0x40;                        /* Co=0, D/C=1 -> datos */
    memcpy(out + 1, fb, sizeof(fb));
    i2c_master_transmit(dev, out, sizeof(out), 200);
}

static void oled_init(void)
{
    static const uint8_t seq[] = {
        0xAE,             /* display off            */
        0xD5, 0x80,       /* clock                  */
        0xA8, 0x1F,       /* multiplex = 32-1       */
        0xD3, 0x00,       /* offset                 */
        0x40,             /* start line 0           */
        0x8D, 0x14,       /* charge pump ON         */
        0x20, 0x00,       /* addressing horizontal  */
        0xA1,             /* segment remap          */
        0xC8,             /* com scan desc          */
        0xDA, 0x02,       /* com pins (128x32)      */
        0x81, 0x8F,       /* contraste              */
        0xD9, 0xF1,       /* precharge              */
        0xDB, 0x40,       /* vcom                   */
        0xA4,             /* mostrar RAM            */
        0xA6,             /* normal (no invertido)  */
        0x2E,             /* scroll off             */
        0xAF,             /* display ON             */
    };
    oled_cmds(seq, sizeof(seq));
}

/* ---------------- Estado del zoom del fractal ------------- */
static int64_t cx   = (int64_t)(-0.743643887037151 * (double)ONE);
static int64_t cy   = (int64_t)( 0.131825904205330 * (double)ONE);
static int64_t span = (int64_t)( 3.0               * (double)ONE);

static const uint8_t bayer[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5},
};

/* Renderiza el fractal en las paginas 1..3 (y = 8..31). */
static void mandel(void)
{
    int64_t step = span / OLED_W;
    int64_t x0 = cx - (OLED_W / 2) * step;
    int64_t y0 = cy - (FRACT_H / 2) * step;

    for (int py = 0; py < FRACT_H; py++) {
        int64_t ci = y0 + (int64_t)py * step;
        for (int sx = 0; sx < OLED_W; sx++) {
            int64_t cr = x0 + (int64_t)sx * step;
            int64_t zr = 0, zi = 0;
            int it = 0;
            while (it < MAXITER) {
                int64_t zr2 = (zr * zr) >> FRAC;
                int64_t zi2 = (zi * zi) >> FRAC;
                if (zr2 + zi2 > (4LL << FRAC)) break;
                int64_t m = (zr * zi) >> FRAC;
                zi = (m << 1) + ci;
                zr = zr2 - zi2 + cr;
                it++;
            }
            if (it < MAXITER) {                       /* fuera del set */
                int inten = (it * 16) / MAXITER;      /* 0..15         */
                if (inten > bayer[py & 3][sx & 3])
                    px(sx, py + FRACT_Y);
            }
        }
    }
}

/* ---------------- Programa principal ---------------------- */
void app_main(void)
{
    /* Bus I2C */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = I2C_SCL,
        .sda_io_num = I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = OLED_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

    oled_init();

    /* Sensor de temperatura interno del C3 */
    temperature_sensor_handle_t ts = NULL;
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    ESP_ERROR_CHECK(temperature_sensor_install(&tcfg, &ts));
    ESP_ERROR_CHECK(temperature_sensor_enable(ts));

    ESP_LOGI(TAG, "Horno encendido: Mandelbrot @160MHz. Calienta y disfruta.");

    const char spin[4] = { '|', '/', '-', '\\' };
    float tmax = -100.0f;
    int fps = 0;
    uint32_t frame = 0;
    int64_t last = esp_timer_get_time();

    for (;;) {
        memset(fb, 0, sizeof(fb));
        mandel();

        /* Zoom lento (~2.8%/frame) y reinicio al perder precision */
        span = (span * 243) / 250;
        if (span < 300) span = (int64_t)(3.0 * (double)ONE);

        /* Temperatura */
        float tc = 0;
        temperature_sensor_get_celsius(ts, &tc);
        if (tc > tmax) tmax = tc;

        /* Barra de estado:  T<act>C ^<max>C <fps>f */
        char line[32];
        char *p = line;
        *p++ = 'T'; p = fmt1(p, tc);   *p++ = 'C';
        *p++ = ' ';
        *p++ = '^'; p = fmt1(p, tmax); *p++ = 'C';
        *p++ = ' ';
        {   int f = fps; char t[6]; int n = 0;
            if (f == 0) t[n++] = '0';
            while (f) { t[n++] = (char)('0' + f % 10); f /= 10; }
            while (n) *p++ = t[--n];
        }
        *p++ = 'f'; *p = 0;
        draw_str(0, line);

        /* Spinner: prueba visual de que sigue crunchando */
        char sp[2] = { spin[frame & 3], 0 };
        draw_str(123, sp);

        oled_flush();

        /* FPS */
        int64_t now = esp_timer_get_time();
        int64_t dt = now - last; last = now;
        if (dt > 0) fps = (int)(1000000LL / dt);
        frame++;

        /* 1 ms para alimentar el watchdog; el nucleo igual va ~99% */
        vTaskDelay(1);
    }
}
