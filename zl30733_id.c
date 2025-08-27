/* Copyright Free Mobile 2025 */

/*
 * ZL30733 / ZL3073x identity reader over Linux spidev
 * - Verifies Chip ID against a small known list
 * - Prints a friendly device name when recognized
 * - Dumps Revision, FW version, Custom Config version
 *
 * Notes:
 * * Page size = 0x80; page select register = 0x7F (low nibble)
 * * Multi-byte fields are big-endian
 * * Start with MODE0, 1 MHz if unsure and tune up
 */

#define _GNU_SOURCE
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/spi/spidev.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <arpa/inet.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif

/* ZL3073x register map basics */
#define ZL_PAGE_SIZE  0x80
#define ZL_PAGE_SEL   0x7F

/* Identity block (page 0) */
#define ZL_REG_ID                 0x0001  // u16, big-endian: Chip ID / family
#define ZL_REG_REVISION           0x0003  // u16, big-endian
#define ZL_REG_FW_VER             0x0005  // u16, big-endian
#define ZL_REG_CUSTOM_CONFIG_VER  0x0007  // u32, big-endian

/* Known Chip IDs (best-effort; exact mapping can vary by OTP/package) */
static struct id_map {
    uint16_t id;
    const char *name;
} const known_ids[] = {
    /* Common ZL3073x family IDs */
    { .id = 0x0E95, .name = "ZL3073x (A)" },
    { .id = 0x1E95, .name = "ZL3073x (B)" },
    { .id = 0x2E95, .name = "ZL3073x (C)" },
};

static const char *devnode = "/dev/spidev0.0";
static uint32_t speed_hz = 1000000; /* 1 MHz default */
static uint8_t mode = SPI_MODE_0; /* default MODE0 */
static uint8_t bits_per_word = 8;
static int debug = 0;

static void
hexdump(const char *prefix, const uint8_t *buf, size_t len)
{
    fprintf(stderr, "%s", prefix);
    for (size_t i = 0; i < len; i++)
        fprintf(stderr, "%s%02X", i ? " " : "", buf[i]);
    fprintf(stderr, "\n");
}

static const
char *lookup_name(uint16_t id)
{
    for (size_t i = 0; i < ARRAY_SIZE(known_ids); i++) {
        if (known_ids[i].id == id)
            return known_ids[i].name;
    }
    return "Unknown";
}

static int
spi_write_u8(int fd, uint8_t reg_off, uint8_t val)
{
    uint8_t tx[2] = { [0] = reg_off, [1] = val };
    if (debug > 0) {
        char pfx[64];
	snprintf(pfx, sizeof(pfx), "SPI_W: off=0x%02X,  data=", reg_off);
        hexdump(pfx, &tx[1], 1);
    }
    struct spi_ioc_transfer xfer = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = 0,
        .len    = sizeof(tx),
        .speed_hz = speed_hz,
        .bits_per_word = bits_per_word,
        .cs_change = 0
    };

    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);

    return (ret < 1) ? -1 : 0;
}

static int
spi_read(int fd, uint8_t reg_off, uint8_t *buf, size_t len)
{
    int r = 0;

    if (len == 0 || len > 255)
        return -EINVAL;

    uint8_t *tx = calloc(1, len + 1);
    uint8_t *rx = calloc(1, len + 1);
    if (!tx || !rx) {
        r = -ENOMEM;
	goto fini;
    }

    tx[0] = reg_off;
    //memset(tx + 1, 0x00, len);

    struct spi_ioc_transfer xfer = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len    = (uint32_t)(len + 1),
        .speed_hz = speed_hz,
        .bits_per_word = bits_per_word,
        .cs_change = 0
    };

    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);

    if (ret < 1) {
        r = -1;
	goto fini;
    }

    if (debug > 0) {
      char pfx[64];
      snprintf(pfx, sizeof(pfx), "SPI_R: off=0x%02X rx=", reg_off);
      hexdump(pfx, rx + 1, len); /* skip echoed address byte */
    }

    memcpy(buf, rx + 1, len); /* skip echoed address byte */

fini:
    free(tx);
    free(rx);
    return r;
}

static int
zl_set_page(int fd, uint8_t page)
{
    if (page > 0x0F)
        return -EINVAL; /* 4-bit page field */

    if (debug > 0)
        fprintf(stderr, "PAGE -> 0x%X (write 0x%02X to 0x%02X)\n",
                        page & 0xf, page & 0xf, ZL_PAGE_SEL);

    return spi_write_u8(fd, ZL_PAGE_SEL, page & 0x0F);
}

static int
zl_read_reg(int fd, uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t page = (reg >> 7) & 0x0F;
    uint8_t off  = reg & 0x7F;

    int rc = zl_set_page(fd, page);

    if (rc)
        return rc;

    return spi_read(fd, off, buf, len);
}

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-d /dev/spidevX.Y] [-s speed_hz] [-m 0..3] [-D debug_level]\n"
        "  -d  spidev device (default %s)\n"
        "  -s  SPI speed in Hz (default %u)\n"
        "  -m  SPI mode 0..3 (default %d)\n"
        "  -D  debug of SPI transfers (default %d)\n"
        ,prog
        ,devnode
        ,speed_hz
        ,mode
        ,debug
    );
}

int
main(int argc, char **argv)
{
    static const struct option long_opts[] = {
        {"device", required_argument, 0, 'd'},
        {"speed", required_argument, 0, 's'},
        {"mode", required_argument, 0, 'm'},
        {"debug", required_argument, 0, 'D'},
        {"help", no_argument, 0, 'h'},
        {}
    };

    int opt, optidx;

    while ((opt = getopt_long(argc, argv, "d:s:m:D:h", long_opts, &optidx)) != -1) {
        switch (opt) {
        case 'd':
            devnode = optarg;
            break;
        case 's':
            speed_hz = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'm': {
            int m = atoi(optarg);
            if (m < 0 || m > 3)
                errx(EXIT_FAILURE, "Invalid SPI mode %d, expected 0..3", m);
            switch (m) {
                case 0: mode = SPI_MODE_0; break;
                case 1: mode = SPI_MODE_1; break;
                case 2: mode = SPI_MODE_2; break;
                case 3: mode = SPI_MODE_3; break;
            }
            break;
        }
        case 'D':
            debug = strtoul(optarg, NULL, 0);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    int fd = open(devnode, O_RDWR);

    if (fd < 0)
        err(EXIT_FAILURE, "open %s failed", devnode); 

    /* set spi bus settings */
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) == -1)
        err(EXIT_FAILURE, "SPI_IOC_WR_MODE(%d)", mode);
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) == -1)
        err(EXIT_FAILURE, "SPI_IOC_WR_BITS_PER_WORD(%d)", bits_per_word);
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) == -1)
        err(EXIT_FAILURE, "SPI_IOC_WR_MAX_SPEED_HZ(%u)", speed_hz);

    uint16_t chip_id, revision, fw_ver;
    uint32_t cfg_ver;

#define ZL_READ_REG(FD, REG, OUTVAR, LEN) do { \
    size_t _len = (size_t)(LEN);                                                \
    uint8_t _b[8];                                                              \
    if (_len > sizeof(_b))                                                      \
        errx(EXIT_FAILURE, #REG " read len %zu > %zu", _len, sizeof(_b));       \
    if (_len > sizeof(OUTVAR))                                                  \
        warnx(#REG " read len %zu > %zu for " #OUTVAR, _len, sizeof(OUTVAR));   \
    if (zl_read_reg((FD), (REG), _b, _len) < 0)                                 \
        errx(EXIT_FAILURE, "read %s failed", #REG);                             \
    if (_len == 1) {                                                            \
        (OUTVAR) = (typeof(OUTVAR))_b[0];                                       \
    } else if (_len == 2) {                                                     \
        uint16_t _t; memcpy(&_t, _b, 2);                                        \
        (OUTVAR) = (typeof(OUTVAR))ntohs(_t);                                   \
    } else if (_len == 4) {                                                     \
        uint32_t _t; memcpy(&_t, _b, 4);                                        \
        (OUTVAR) = (typeof(OUTVAR))ntohl(_t);                                   \
    } else {                                                                    \
        errx(EXIT_FAILURE, #REG " unsupported LEN=%zu (only 8,16,32 bits)", _len);\
    }                                                                           \
} while (0)

    ZL_READ_REG(fd, ZL_REG_ID, chip_id, 2);
    ZL_READ_REG(fd, ZL_REG_REVISION, revision, 2);
    ZL_READ_REG(fd, ZL_REG_FW_VER, fw_ver, 2);
    ZL_READ_REG(fd, ZL_REG_CUSTOM_CONFIG_VER, cfg_ver, 4);

    /* done, print it */
    printf("ZL3073x identity via %s\n", devnode);
    printf("  Chip ID              : 0x%04X  (%s)\n", chip_id, lookup_name(chip_id));
    printf("  Revision             : 0x%04X  (major=%u minor=%u)\n",
           revision, (revision >> 4) & 0xF, revision & 0xF);
    printf("  Firmware Version     : 0x%04X\n", fw_ver);
    printf("  Custom Config Version: 0x%08X\n", cfg_ver);

    close(fd);

    return EXIT_SUCCESS;
}
