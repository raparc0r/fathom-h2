/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * slcd-poke: bring-up probe for the X1000 smart-LCD controller on the H2.
 * Drives the controller's MDATA register directly through /dev/mem, so the
 * panel path can be tested without the kernel driver. Debug tool only.
 *
 *   slcd-poke rect RRGGBB X Y W H   fill a rectangle (RGB565 on the wire)
 *   slcd-poke cmd HEX [HEX...]       send a DCS command with parameters
 *   slcd-poke regs                   print the controller and pin registers
 *   slcd-poke pins rockbox|ctrl      CS/RD as GPIOs (Rockbox) or controller function
 *   slcd-poke reset                  pulse the panel reset line (PB13)
 *   slcd-poke mcfg HEX               write the MCFG register
 *   slcd-poke readid                 bit-bang DCS 0x04 / 0xD3 ID reads (as Rockbox)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LCD_BASE  0x13050000u
#define GPIO_BASE 0x10010000u
#define CTRL      0x30
#define STATE     0x34
#define MCFG      0xa0
#define MCTRL     0xa4
#define MSTATE    0xa8
#define MDATA     0xac
#define MCFG_NEW  0xb8
#define CMD       (1u << 30)

static volatile uint32_t *lcd, *gpio;

static uint32_t rd(volatile uint32_t *b, unsigned off) { return b[off / 4]; }
static void wr(volatile uint32_t *b, unsigned off, uint32_t v) { b[off / 4] = v; }

static int idle(void)
{
    for (long i = 0; i < 1000000; i++)
        if (!(rd(lcd, MSTATE) & 1)) return 0;
    fprintf(stderr, "timeout: MSTATE=%08x\n", rd(lcd, MSTATE));
    return -1;
}

static void send(uint32_t w) { idle(); wr(lcd, MDATA, w); }

/* Stop the frame DMA the bootloader may have left running (as Rockbox's lcd_dma_stop). */
static void stop_dma(void)
{
    wr(lcd, CTRL, rd(lcd, CTRL) & ~(1u << 3));         /* ENABLE = 0 */
    wr(lcd, MCTRL, rd(lcd, MCTRL) & ~1u);              /* DMA_TX_EN = 0 */
    for (long i = 0; i < 1000000 && !(rd(lcd, STATE) & (1u << 7)); i++) ;  /* wait QD */
    wr(lcd, STATE, 0);
}

static void setup(void)
{
    stop_dma();
    wr(lcd, MCFG, (rd(lcd, MCFG) & ~(3u << 8)) | (1u << 8));   /* 8-bit commands */
    wr(lcd, MCFG_NEW, 0);       /* 8-bit parallel data, 1 transfer per write, no format conversion */
    wr(lcd, MCTRL, (1u << 8) | (1u << 4) | (1u << 2));        /* no TE, fast mode, DMA mode (as Rockbox) */
    idle();
}

/* GPIO port B: INT 0x10, MSK 0x20, PAT1 0x30, PAT0 0x40; +4 set, +8 clear */
#define PB(off) (0x100 + (off))
static void pin_gpio_out(int pin, int level)
{
    uint32_t m = 1u << pin;
    wr(gpio, PB(0x18), m);                      /* INT clear */
    wr(gpio, PB(0x24), m);                      /* MSK set: GPIO */
    wr(gpio, PB(0x38), m);                      /* PAT1 clear: output */
    wr(gpio, PB(level ? 0x44 : 0x48), m);       /* PAT0 */
}
static void pin_func1(int pin)
{
    uint32_t m = 1u << pin;
    wr(gpio, PB(0x18), m);                      /* INT clear */
    wr(gpio, PB(0x28), m);                      /* MSK clear: device function */
    wr(gpio, PB(0x38), m);                      /* PAT1 clear */
    wr(gpio, PB(0x44), m);                      /* PAT0 set: function 1 */
}

/* ---- bit-bang ID read, after Rockbox lcd-erosqnative.c ---- */
#define PA(off) (off)
static void pa_input(void)
{
    wr(gpio, PA(0x18), 0xff);     /* INT clear */
    wr(gpio, PA(0x24), 0xff);     /* MSK set: GPIO */
    wr(gpio, PA(0x34), 0xff);     /* PAT1 set: input */
}
static void pa_func1(void)
{
    wr(gpio, PA(0x18), 0xff);
    wr(gpio, PA(0x28), 0xff);     /* MSK clear: device */
    wr(gpio, PA(0x38), 0xff);     /* PAT1 clear */
    wr(gpio, PA(0x44), 0xff);     /* PAT0 set: function 1 */
}
static void pa_out(uint8_t d)
{
    wr(gpio, PA(0x44), d);
    wr(gpio, PA(0x48), (uint8_t)~d);
    wr(gpio, PA(0x38), 0xff);     /* PAT1 clear: drive */
}
static void lvl(int pin, int v) { wr(gpio, PB(v ? 0x44 : 0x48), 1u << pin); }
static void bb_cmd(uint8_t c)
{
    lvl(20, 0); lvl(17, 0); pa_out(c); usleep(1); lvl(17, 1); lvl(20, 1);
    wr(gpio, PA(0x34), 0xff);     /* back to input */
}
static uint8_t bb_read(void)
{
    lvl(16, 0); usleep(1);
    uint8_t v = (uint8_t)rd(gpio, PA(0x00));
    lvl(16, 1); usleep(1);
    return v;
}
static uint32_t bb_id(uint8_t cmd, uint8_t raw[4])
{
    lvl(18, 0); usleep(1);
    bb_cmd(cmd);
    for (int i = 0; i < 4; i++) raw[i] = bb_read();
    lvl(18, 1); usleep(1);
    return (uint32_t)raw[1] << 16 | (uint32_t)raw[2] << 8 | raw[3];
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "see source for usage\n"); return 2; }
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    lcd = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, LCD_BASE);
    gpio = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);
    if (lcd == MAP_FAILED || gpio == MAP_FAILED) { perror("mmap"); return 1; }

    if (!strcmp(argv[1], "regs")) {
        printf("CTRL=%08x STATE=%08x ", rd(lcd, CTRL), rd(lcd, STATE));
        printf("MCFG=%08x MCTRL=%08x MSTATE=%08x MCFG_NEW=%08x\n",
               rd(lcd, MCFG), rd(lcd, MCTRL), rd(lcd, MSTATE), rd(lcd, MCFG_NEW));
        printf("PB INT=%08x MSK=%08x PAT1=%08x PAT0=%08x PIN=%08x\n",
               rd(gpio, PB(0x10)), rd(gpio, PB(0x20)), rd(gpio, PB(0x30)), rd(gpio, PB(0x40)), rd(gpio, PB(0x00)));
    } else if (!strcmp(argv[1], "pins") && argc > 2) {
        if (!strcmp(argv[2], "rockbox")) { pin_gpio_out(16, 1); pin_gpio_out(18, 1); usleep(5000); pin_gpio_out(18, 0); }
        else { pin_func1(16); pin_func1(18); }
    } else if (!strcmp(argv[1], "mcfg") && argc > 2) {
        wr(lcd, MCFG, (uint32_t)strtoul(argv[2], NULL, 16));
    } else if (!strcmp(argv[1], "readid")) {
        uint8_t r[4];
        stop_dma();
        pa_input();
        pin_gpio_out(16, 1); pin_gpio_out(17, 1); pin_gpio_out(20, 1); pin_gpio_out(18, 1);
        usleep(1000);
        uint32_t id = bb_id(0x04, r);
        printf("RDDID 0x04: %06x  (raw %02x %02x %02x %02x)\n", id, r[0], r[1], r[2], r[3]);
        id = bb_id(0xd3, r);
        printf("ID4   0xD3: %06x  (raw %02x %02x %02x %02x)\n", id, r[0], r[1], r[2], r[3]);
        id = bb_id(0x0a, r);
        printf("RDDPM 0x0A: power mode %02x  (raw %02x %02x)\n", r[1], r[0], r[1]);
        pa_func1(); pin_func1(17); pin_func1(20); lvl(18, 0);   /* CS stays low, as Rockbox */
    } else if (!strcmp(argv[1], "reset")) {
        pin_gpio_out(13, 0); usleep(20000); pin_gpio_out(13, 1); usleep(120000);
    } else if (!strcmp(argv[1], "cmd") && argc > 2) {
        setup();
        send(CMD | (uint32_t)strtoul(argv[2], NULL, 16));
        for (int i = 3; i < argc; i++) send((uint32_t)strtoul(argv[i], NULL, 16) & 0xff);
        idle();
    } else if (!strcmp(argv[1], "rect") && argc > 6) {
        uint32_t rgb = strtoul(argv[2], NULL, 16);
        int x = atoi(argv[3]), y = atoi(argv[4]), w = atoi(argv[5]), h = atoi(argv[6]);
        uint16_t c = (uint16_t)(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x1f));
        setup();
        send(CMD | 0x2a); send((x >> 8) & 0xff); send(x & 0xff); send(((x + w - 1) >> 8) & 0xff); send((x + w - 1) & 0xff);
        send(CMD | 0x2b); send((y >> 8) & 0xff); send(y & 0xff); send(((y + h - 1) >> 8) & 0xff); send((y + h - 1) & 0xff);
        send(CMD | 0x2c);
        for (int pad = getenv("PAD") ? atoi(getenv("PAD")) : 0; pad > 0; pad--) send(0);  /* debug: byte-phase test */
        for (long i = 0; i < (long)w * h; i++) { send(c >> 8); send(c & 0xff); }
        idle();
    } else {
        fprintf(stderr, "unknown command\n"); return 2;
    }
    return 0;
}
