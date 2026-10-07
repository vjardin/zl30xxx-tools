/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Copyright 2026 Free Mobile - Vincent Jardin
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See <https://www.gnu.org/licenses/>.
 */

/**
 * @file zl3073x-dump.c
 * @brief Dump, decode and integrity-check a Microchip/Microsemi ZL3073x DPLL.
 *
 * Reads the ZL3073x (on nbxv3: ZL30733, chip-ID 0x0E95, SPI bus 0 CS 0) from
 * Linux to check the loaded flash configuration is intact and to understand it.
 *
 * Output: a decoded TEXT report (-o) and/or a structured JSON dump (-j); the
 * JSON carries the raw hex register_map for byte-level golden diffs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <err.h>
#include <signal.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#define ZL_CMD_READ	0x80u
#define ZL_PAGE_SEL	0x7Fu
#define ZL_LOCK_FILE	"/var/lock/zl3073x-spidev.lock"
#define ZL_LOCK_FILE2	"/tmp/zl3073x-spidev.lock"
static int	lock_fd		= -1;
static bool	force_lock	= false;
#define ZL_EXPECT_ID	0x0E95u
#define ZL_MB_SEM_RD	0x02u		/* BIT(1) - trigger window read */

#define NUM_REFS	10
#define DPLL_ASSIST	5	/* the NCO-hybrid assist DPLL, on every part */
#define NUM_DPLLS	(DPLL_ASSIST + 1)	/* DPLL mailbox slots: DPLL0..DPLL5 */
#define NUM_SYNTHS	5
#define NUM_OUTS	10
_Static_assert(NUM_DPLLS <= NUM_OUTS, "mbval[][NUM_OUTS] holds every DPLL slot");

static char	spidev_name[32]	= "spi0.0";
static char	dpll_driver[32]	= "zl3073x-spi";
static uint32_t	spi_hz		= 5000000;
static int	npages		= 15;
static int	stable_reads	= 8;
static int	reread_ms	= 300;
static bool	do_rebind	= true;
static bool	keep_spidev	= false;
static int	spi_fd		= -1;
static bool	unbound		= false;
static bool	spidev_bound = false;
static bool	override_set = false;

/** @brief Snapshot of the live register pages (offsets 0x00..0x7E per page).
 *  Filled by the CRC sampling in main(); reused by the emitters as the raw
 *  hex @c register_map so the dump can be byte-diffed against a golden one. */
static uint8_t	snap[15][0x80];

static int sysfs_write(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -errno;
	ssize_t n = write(fd, val, strlen(val));
	int e = (n < 0) ? -errno : 0;
	close(fd);
	return e;
}
static void restore_driver(void)
{
	char p[160];
	if (spi_fd >= 0) { close(spi_fd); spi_fd = -1; }
	if (keep_spidev) return;
	if (spidev_bound) {
		sysfs_write("/sys/bus/spi/drivers/spidev/unbind", spidev_name);
		spidev_bound = false;
	}
	if (override_set) {
		snprintf(p, sizeof(p), "/sys/bus/spi/devices/%s/driver_override", spidev_name);
		sysfs_write(p, "\n");
		override_set = false;
	}
	if (unbound) {
		snprintf(p, sizeof(p), "/sys/bus/spi/drivers/%s/bind", dpll_driver);
		sysfs_write(p, spidev_name);
		unbound = false;
	}
}
static void on_signal(int sig) { restore_driver(); _exit(128 + sig); }

static bool bound_to_spidev(void)
{
	char p[160], l[256];
	snprintf(p, sizeof(p), "/sys/bus/spi/devices/%s/driver", spidev_name);
	ssize_t r = readlink(p, l, sizeof(l) - 1);
	if (r <= 0) return false;
	l[r] = '\0';
	const char *s = strrchr(l, '/');
	return !strcmp(s ? s + 1 : l, "spidev");
}

static int grab_spidev(void)
{
	char p[160];
	int rc;
	if (bound_to_spidev()) return 0;	/* spidev already (device tree): nothing to swap */
	snprintf(p, sizeof(p), "/sys/bus/spi/drivers/%s/unbind", dpll_driver);
	rc = sysfs_write(p, spidev_name);
	if (rc && rc != -ENOENT) warnx("unbind %s: %s", dpll_driver, strerror(-rc));
	unbound = (rc == 0);
	snprintf(p, sizeof(p), "/sys/bus/spi/devices/%s/driver_override", spidev_name);
	rc = sysfs_write(p, "spidev");
	if (rc) { warnx("driver_override: %s", strerror(-rc)); return rc; }
	override_set = true;
	rc = sysfs_write("/sys/bus/spi/drivers/spidev/bind", spidev_name);
	if (rc && rc != -EEXIST && rc != -EBUSY) { warnx("bind spidev: %s", strerror(-rc)); return rc; }
	spidev_bound = (rc == 0);
	return 0;
}
static int take_lock(void)
{
	const char *path = ZL_LOCK_FILE;
	lock_fd = open(ZL_LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (lock_fd < 0) { path = ZL_LOCK_FILE2; lock_fd = open(ZL_LOCK_FILE2, O_RDWR | O_CREAT | O_CLOEXEC, 0644); }
	if (lock_fd < 0) { warn("lock file"); return force_lock ? 0 : -1; }
	if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0) return 0;
	if (!force_lock) {
		warnx("another process holds the ZL3073x (%s)", path);
		return -1;
	}
	warnx("lock held by another process: going on (-F), each access flock()ed");
	return 0;
}
static int open_spidev(void)
{
	unsigned bus, cs; char dev[40];
	if (sscanf(spidev_name, "spi%u.%u", &bus, &cs) != 2) { warnx("bad spi name '%s' (want spiB.C)", spidev_name); return -1; }
	snprintf(dev, sizeof(dev), "/dev/spidev%u.%u", bus, cs);
	for (int i = 0; i < 50 && spi_fd < 0; i++) { spi_fd = open(dev, O_RDWR); if (spi_fd < 0) usleep(20000); }
	if (spi_fd < 0) { warn("open %s", dev); return -1; }
	uint8_t mode = SPI_MODE_0, bits = 8;
	if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_hz) < 0) { warn("spidev setup %s", dev); return -1; }
	return 0;
}

static int spi_xfer(const uint8_t *tx, uint8_t *rx, int len)
{
	struct spi_ioc_transfer t = { .tx_buf = (uintptr_t)tx, .rx_buf = (uintptr_t)rx,
				      .len = len, .speed_hz = spi_hz, .bits_per_word = 8 };
	return ioctl(spi_fd, SPI_IOC_MESSAGE(1), &t) < 0 ? -errno : 0;
}
static int zl_read_off(uint8_t off, uint8_t *buf, int len)
{
	uint8_t tx[1 + 64] = {0}, rx[1 + 64] = {0};
	if (len > 64) return -EINVAL;
	tx[0] = ZL_CMD_READ | (off & 0x7F);
	int rc = spi_xfer(tx, rx, 1 + len);
	if (!rc) memcpy(buf, rx + 1, len);
	return rc;
}
static int zl_write_off(uint8_t off, const uint8_t *data, int len)
{
	uint8_t tx[1 + 8];
	if (len > 8) return -EINVAL;
	tx[0] = off & 0x7F;
	memcpy(tx + 1, data, len);
	return spi_xfer(tx, NULL, 1 + len);
}
static int zl_page(int page) { uint8_t p = page; return zl_write_off(ZL_PAGE_SEL, &p, 1); }

/* big-endian register read at raw 16-bit address (page<<7 | offset) */
static int zl_rd(uint16_t addr, uint8_t *buf, int len)
{
	(void)flock(spi_fd, LOCK_EX);
	int rc = zl_page(addr >> 7);
	if (!rc) rc = zl_read_off(addr & 0x7F, buf, len);
	(void)flock(spi_fd, LOCK_UN);
	return rc;
}
static int zl_wr(uint16_t addr, const uint8_t *data, int len)
{
	(void)flock(spi_fd, LOCK_EX);
	int rc = zl_page(addr >> 7);
	if (!rc) rc = zl_write_off(addr & 0x7F, data, len);
	(void)flock(spi_fd, LOCK_UN);
	return rc;
}
static uint64_t be(const uint8_t *b, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | b[i]; return v; }
static int rd_val(uint16_t addr, int size, uint64_t *out)
{
	uint8_t b[8];
	int rc = zl_rd(addr, b, size);
	if (rc) return rc;
	*out = be(b, size);
	return 0;
}
static int read_page(int pg, uint8_t *buf)
{
	for (int off = 0; off < 0x7F; off += 16) {
		int len = (off + 16 <= 0x7F) ? 16 : (0x7F - off);
		if (zl_rd((pg << 7) | off, buf + off, len)) return -1;
	}
	return 0;
}
static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, int n)
{
	crc = ~crc;
	while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1)); }
	return ~crc;
}

/* one config field inside a mailbox window: name, offset-in-page, byte size */
struct field { const char *name; uint8_t off; uint8_t size; };

struct mbtype {
	const char *name;
	int page, sem_off, mask_off;	/* MB_SEM/MASK offsets within the page */
	int count;
	const struct field *f;
	int nf;
	int cfg_last;			/* the window's last configuration byte */
};
static const struct field ref_f[] = {		/* page 10 window */
	{"freq_base",0x05,2}, {"freq_mult",0x07,2}, {"ratio_m",0x09,2}, {"ratio_n",0x0b,2},
	{"config",0x0d,1}, {"sync_ctrl",0x2e,1}, {"esync_div",0x30,4}, {"phase_off_comp",0x28,4},
};
static const struct field synth_f[] = {		/* page 13 window */
	{"freq_base",0x06,2}, {"freq_mult",0x08,4}, {"freq_m",0x0c,2}, {"freq_n",0x0e,2},
};
static const struct field out_f[] = {		/* page 14 window */
	{"mode",0x05,1}, {"div",0x0c,4}, {"width",0x10,4}, {"esync_period",0x14,4},
	{"esync_width",0x18,4}, {"phase_comp",0x20,4},
};
static const struct field dpll_f[] = {		/* page 12 window */
	{"ref_prio0",0x52,1}, {"ref_prio1",0x53,1}, {"ref_prio2",0x54,1},
	{"ref_prio3",0x55,1}, {"ref_prio4",0x56,1},
	{"bw_fixed",0x20,1}, {"bw_var",0x21,1},
};
#define DPLL_F_BW_FIXED	5	/* indexes in dpll_f[] */
#define DPLL_F_BW_VAR	6
static const struct mbtype MB[] = {
	{"ref",    10, 0x04, 0x02, NUM_REFS,   ref_f,   sizeof(ref_f)/sizeof(ref_f[0]),     0x3F},
	{"dpll",   12, 0x04, 0x02, NUM_DPLLS,  dpll_f,  sizeof(dpll_f)/sizeof(dpll_f[0]),   0x6C},
	{"synth",  13, 0x04, 0x02, NUM_SYNTHS, synth_f, sizeof(synth_f)/sizeof(synth_f[0]), 0x17},
	{"output", 14, 0x04, 0x02, NUM_OUTS,   out_f,   sizeof(out_f)/sizeof(out_f[0]),     0x2A},
};
#define NMB (int)(sizeof(MB)/sizeof(MB[0]))
#define MAXF 8

static uint64_t mbval[NMB][NUM_OUTS][MAXF];	/* [type][instance][field] */

#define MB_CFG_FIRST	0x05
static uint32_t mb_cfg_crc;
static int mb_cfg_instances, mb_cfg_bytes;

static uint32_t mb_win_crc(uint32_t crc, int t, int inst, const uint8_t *win)
{
	uint8_t key[2] = { (uint8_t)t, (uint8_t)inst };

	crc = crc32_upd(crc, key, 2);
	return crc32_upd(crc, win + MB_CFG_FIRST, MB[t].cfg_last - MB_CFG_FIRST + 1);
}

/* select instance `inst` of mailbox `t` and read its window into mbval */
static int mb_read(int t, int inst)
{
	const struct mbtype *m = &MB[t];
	uint16_t maskreg = (m->page << 7) | m->mask_off;
	uint16_t semreg  = (m->page << 7) | m->sem_off;
	uint8_t mb[2] = { (uint8_t)((1u << inst) >> 8), (uint8_t)((1u << inst) & 0xff) };
	uint8_t rd = ZL_MB_SEM_RD, s = ZL_MB_SEM_RD;

	if (zl_wr(maskreg, mb, 2) || zl_wr(semreg, &rd, 1)) return -1;
	for (int i = 0; i < 200 && (s & ZL_MB_SEM_RD); i++) {	/* poll up to ~2s */
		usleep(2000);
		if (zl_rd(semreg, &s, 1)) return -1;
	}
	if (s & ZL_MB_SEM_RD) { warnx("%s[%d] mailbox read timeout", m->name, inst); return -1; }
	for (int k = 0; k < m->nf; k++)
		if (rd_val((m->page << 7) | m->f[k].off, m->f[k].size, &mbval[t][inst][k]))
			return -1;

	uint8_t win[0x80];
	if (read_page(m->page, win)) return -1;
	mb_cfg_crc = mb_win_crc(mb_cfg_crc, t, inst, win);
	mb_cfg_instances++;
	mb_cfg_bytes += m->cfg_last - MB_CFG_FIRST + 1;
	return 0;
}

static unsigned dpll_present = (1u << NUM_DPLLS) - 1;

static unsigned dpll_mask(uint16_t id)
{
	unsigned part = id & 0x0FFFu;

	if (part < 0x0E93u || part > 0x0E97u)	/* not a ZL3073x chip ID: every slot */
		return (1u << NUM_DPLLS) - 1;
	return ((1u << (part - 0x0E92u)) - 1) | (1u << DPLL_ASSIST);
}
static bool mb_present(int t, int inst)
{
	return t != 1 || (dpll_present >> inst & 1);	/* MB[1]: dpll */
}
static int mb_count(int t)
{
	int n = 0;
	for (int i = 0; i < MB[t].count; i++) n += mb_present(t, i);
	return n;
}

struct id_blk {
	uint8_t info, rev, mcs;
	uint16_t id; uint8_t fw[6]; uint32_t cfgver; uint16_t gpio;
	bool id_ok, ready, cfg_invalid;
};
static int read_id(struct id_blk *b)
{
	uint8_t id2[2], gp[2], cv[4];
	if (zl_rd(0x00, &b->info, 1) || zl_rd(0x01, id2, 2) || zl_rd(0x03, &b->rev, 1) ||
	    zl_rd(0x05, b->fw, 6) || zl_rd(0x07, cv, 4) || zl_rd(0x19, gp, 2) || zl_rd(0x32, &b->mcs, 1))
		return -1;
	b->id = be(id2, 2); b->cfgver = be(cv, 4); b->gpio = be(gp, 2);
	b->id_ok = (b->id == ZL_EXPECT_ID); b->ready = b->info & 0x80; b->cfg_invalid = b->mcs & 0x04;
	return 0;
}

static const char *fld(int t, int k) { return MB[t].f[k].name; }

static double dpll_bw_hz(uint64_t bw_fixed, uint64_t bw_var)
{
	static const double fixed_hz[] = { 14, 29, 61, 141, 403, 470 };
	unsigned int code = bw_fixed & 0x7;

	if (code < 6)
		return fixed_hz[code];
	if (code == 7)
		return pow(10.0, (double)(bw_var & 0xff) / 32.0) * 1e-4;
	return -1.0;
}

static const char *dpll_bw_mode(uint64_t bw_fixed)
{
	switch (bw_fixed & 0x7) {
	case 6: return "reserved";
	case 7: return "variable";
	default: return "fixed";
	}
}

static void emit_text(FILE *o, const struct id_blk *b, uint32_t scrc, uint32_t wcrc,
		      int stable, int total, int nvol)
{
	fprintf(o, "ZL3073x DPLL config dump (%s)\n", spidev_name);
	fprintf(o, "  id 0x%04x %s  rev 0x%02x  fw %02x%02x  custom_config_ver 0x%08x%s\n",
		b->id, b->id_ok ? "(ZL30733)" : "(UNEXPECTED!)", b->rev, b->fw[0], b->fw[1],
		b->cfgver, b->cfgver == 0xffffffff ? " (BLANK)" : "");
	fprintf(o, "  ready=%d  cfg_invalid=%d  booted config slot AC=%u\n",
		b->ready, b->cfg_invalid, b->gpio & 0x7);
	fprintf(o, "  stable-cfg CRC32 0x%08x (%d/%d static)   whole-map CRC32 0x%08x (%d volatile)\n",
		scrc, stable, total, wcrc, nvol);
	fprintf(o, "  mailbox-cfg CRC32 0x%08x (%d instances, %d bytes)\n",
		mb_cfg_crc, mb_cfg_instances, mb_cfg_bytes);

	for (int t = 0; t < NMB; t++) {
		const struct mbtype *m = &MB[t];
		fprintf(o, "\n%s (%d instances):\n", m->name, mb_count(t));
		for (int i = 0; i < m->count; i++) {
			if (!mb_present(t, i)) continue;
			fprintf(o, "  [%2d]", i);
			for (int k = 0; k < m->nf; k++)
				fprintf(o, " %s=%llu", fld(t, k), (unsigned long long)mbval[t][i][k]);
			/* friendly frequency decode */
			if (t == 0) {	/* ref: base*mult*m/n Hz */
				uint64_t n = mbval[t][i][3] ? mbval[t][i][3] : 1;
				fprintf(o, "  => %llu Hz%s",
					(unsigned long long)(mbval[t][i][0] * mbval[t][i][1] * mbval[t][i][2] / n),
					(mbval[t][i][4] & 1) ? "" : " (disabled)");
			} else if (t == 1) {	/* dpll: loop bandwidth */
				uint64_t f = mbval[t][i][DPLL_F_BW_FIXED];
				double bw = dpll_bw_hz(f, mbval[t][i][DPLL_F_BW_VAR]);
				if (bw < 0)
					fprintf(o, "  => loop bandwidth: reserved code 6");
				else
					fprintf(o, "  => loop bandwidth %.3g Hz (%s)", bw, dpll_bw_mode(f));
			} else if (t == 2) {	/* synth: base*mult*m/n Hz */
				uint64_t n = mbval[t][i][3] ? mbval[t][i][3] : 1;
				fprintf(o, "  => %llu Hz",
					(unsigned long long)(mbval[t][i][0] * mbval[t][i][1] * mbval[t][i][2] / n));
			}
			fprintf(o, "\n");
		}
	}

	fprintf(o, "\nregister_map (raw hex, %d pages x 0x7F):\n", npages);
	for (int pg = 0; pg < npages; pg++)
		for (int off = 0; off < 0x7F; off += 16) {
			fprintf(o, "  %04x:", (pg << 7) | off);
			for (int i = 0; i < 16 && off + i < 0x7F; i++)
				fprintf(o, " %02x", snap[pg][off + i]);
			fprintf(o, "\n");
		}
}

static void emit_json(FILE *o, const struct id_blk *b, uint32_t scrc, uint32_t wcrc,
		      int stable, int total, int nvol)
{
	fprintf(o, "{\n");
	fprintf(o, "  \"device\": \"%s\",\n", spidev_name);
	fprintf(o, "  \"identity\": {\"id\":\"0x%04x\",\"part\":\"%s\",\"revision\":\"0x%02x\","
		"\"fw_ver\":\"0x%02x%02x\",\"custom_config_ver\":\"0x%08x\",\"config_blank\":%s},\n",
		b->id, b->id_ok ? "ZL30733" : "unknown", b->rev, b->fw[0], b->fw[1], b->cfgver,
		b->cfgver == 0xffffffff ? "true" : "false");
	fprintf(o, "  \"integrity\": {\"chip_id_ok\":%s,\"ready\":%s,\"cfg_invalid\":%s,"
		"\"config_slot\":%u,\"stable_cfg_crc32\":\"0x%08x\",\"whole_map_crc32\":\"0x%08x\","
		"\"static_bytes\":%d,\"total_bytes\":%d,\"volatile_bytes\":%d,"
		"\"mailbox_cfg_crc32\":\"0x%08x\",\"mailbox_instances\":%d,\"mailbox_bytes\":%d},\n",
		b->id_ok ? "true" : "false", b->ready ? "true" : "false",
		b->cfg_invalid ? "true" : "false", b->gpio & 0x7, scrc, wcrc, stable, total, nvol,
		mb_cfg_crc, mb_cfg_instances, mb_cfg_bytes);

	for (int t = 0; t < NMB; t++) {
		const struct mbtype *m = &MB[t];
		fprintf(o, "  \"%ss\": [\n", m->name);
		for (int i = 0; i < m->count; i++) {
			if (!mb_present(t, i)) continue;
			fprintf(o, "    {\"index\":%d", i);
			for (int k = 0; k < m->nf; k++)
				fprintf(o, ",\"%s\":%llu", fld(t, k), (unsigned long long)mbval[t][i][k]);
			if (t == 0 || t == 2) {
				uint64_t n = mbval[t][i][3] ? mbval[t][i][3] : 1;
				fprintf(o, ",\"freq_hz\":%llu",
					(unsigned long long)(mbval[t][i][0] * mbval[t][i][1] * mbval[t][i][2] / n));
			}
			if (t == 0) fprintf(o, ",\"enabled\":%s", (mbval[t][i][4] & 1) ? "true" : "false");
			if (t == 1) {
				uint64_t f = mbval[t][i][DPLL_F_BW_FIXED];
				double bw = dpll_bw_hz(f, mbval[t][i][DPLL_F_BW_VAR]);
				if (bw < 0)
					fprintf(o, ",\"loop_bw_hz\":null");
				else
					fprintf(o, ",\"loop_bw_hz\":%.6g", bw);
				fprintf(o, ",\"loop_bw_mode\":\"%s\"", dpll_bw_mode(f));
			}
			fprintf(o, "}%s\n", i + 1 < m->count ? "," : "");
		}
		fprintf(o, "  ],\n");
	}

	/* raw register map: one lowercase-hex string per page (offsets 0x00..0x7E) */
	fprintf(o, "  \"register_map\": {\n");
	fprintf(o, "    \"pages\": %d, \"page_bytes\": 127,\n", npages);
	fprintf(o, "    \"hex\": [\n");
	for (int pg = 0; pg < npages; pg++) {
		fprintf(o, "      \"");
		for (int off = 0; off < 0x7F; off++)
			fprintf(o, "%02x", snap[pg][off]);
		fprintf(o, "\"%s\n", pg + 1 < npages ? "," : "");
	}
	fprintf(o, "    ]\n  }\n");
	fprintf(o, "}\n");
}

static void usage(const char *a0)
{
	fprintf(stderr,
	"usage: %s [-o text.txt] [-j out.json] [opts]\n"
	"  -o file   decoded TEXT report   -j file   structured JSON dump\n"
	"            (both carry the raw hex register_map for byte-level diffs)\n"
	"  -d spiB.C device (default spi0.0)   -D drv   DPLL driver (zl3073x-spi)\n"
	"  -s hz     SPI clock (default 5M)    -p N     register pages to CRC (def 15)\n"
	"  -r passes / -w ms   volatile-detection sampling (default 8 / 300ms)\n"
	"  -n  assume spidev already bound    -k  leave spidev bound on exit\n"
	"  -F  run even while another process holds the chip's lock\n"
	"golden-CRC compare (avoid a driver re-probe in between):\n"
	"  %s -k -j golden.json ; %s -n -j now.json   # then diff stable_cfg_crc32\n",
	a0, a0, a0);
}

int main(int argc, char **argv)
{
	const char *textpath = NULL, *jsonpath = NULL;
	int c;
	while ((c = getopt(argc, argv, "o:j:d:D:s:p:r:w:nkFh")) != -1) switch (c) {
		case 'F': force_lock = true; break;
		case 'o': textpath = optarg; break;
		case 'j': jsonpath = optarg; break;
		case 'd': snprintf(spidev_name, sizeof(spidev_name), "%s", optarg); break;
		case 'D': snprintf(dpll_driver, sizeof(dpll_driver), "%s", optarg); break;
		case 's': spi_hz = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'p': npages = atoi(optarg); break;
		case 'r': stable_reads = atoi(optarg); break;
		case 'w': reread_ms = atoi(optarg); break;
		case 'n': do_rebind = false; break;
		case 'k': keep_spidev = true; break;
		default: usage(argv[0]); return 2;
	}
	if (npages < 1 || npages > 15) errx(2, "npages must be 1..15");
	if (!textpath && !jsonpath) textpath = "/dev/stdout";	/* default: text to stdout */

	if (take_lock()) return EXIT_FAILURE;	/* before the driver swap: a refused run changes nothing */
	if (do_rebind) {
		atexit(restore_driver);
		signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGHUP, on_signal);
		if (grab_spidev()) return EXIT_FAILURE;		/* atexit restores driver */
	}
	if (open_spidev()) return EXIT_FAILURE;

	struct id_blk idb;
	if (read_id(&idb)) errx(EXIT_FAILURE, "identity read failed");
	dpll_present = dpll_mask(idb.id);

	/* mailbox walk: every ref / dpll / synth / output, the DPLLs this part has */
	for (int t = 0; t < NMB; t++)
		for (int i = 0; i < MB[t].count; i++)
			if (mb_present(t, i) && mb_read(t, i))
				errx(EXIT_FAILURE, "mailbox %s[%d] read failed", MB[t].name, i);

	/* stable-config CRC over the register pages (excludes live-status bytes) */
	static uint8_t volm[15][0x80];
	memset(volm, 0, sizeof volm);
	for (int pg = 0; pg < npages; pg++)
		if (read_page(pg, snap[pg])) errx(EXIT_FAILURE, "page %d read failed", pg);
	for (int r = 1; r < stable_reads; r++) {
		usleep((useconds_t)reread_ms * 1000);
		for (int pg = 0; pg < npages; pg++) {
			uint8_t tmp[0x80];
			if (read_page(pg, tmp)) errx(EXIT_FAILURE, "page %d read failed", pg);
			for (int off = 0; off < 0x7F; off++) if (tmp[off] != snap[pg][off]) volm[pg][off] = 1;
		}
	}
	for (int off = 0x40; off <= 0x45; off++) volm[0][off] = 1;
	for (int pg = 0; pg < npages; pg++) volm[pg][0x7E] = 1;
	for (int pg = 2; pg <= 3 && pg < npages; pg++) memset(volm[pg], 1, 0x7F);
	for (int pg = 6; pg <= 7 && pg < npages; pg++) memset(volm[pg], 1, 0x7F);
	for (int pg = 10; pg < npages; pg++) if (pg != 11) memset(volm[pg], 1, 0x7F);
	if (npages > 9) for (int off = 0x1E; off <= 0x26; off++) volm[9][off] = 1;
	uint32_t wcrc = 0, scrc = 0;
	int stable = 0, total = 0, nvol = 0;
	for (int pg = 0; pg < npages; pg++) {
		wcrc = crc32_upd(wcrc, snap[pg], 0x7F);
		for (int off = 0; off < 0x7F; off++) {
			total++;
			if (volm[pg][off]) nvol++;
			else { scrc = crc32_upd(scrc, &snap[pg][off], 1); stable++; }
		}
	}

	if (textpath) {
		FILE *o = strcmp(textpath, "/dev/stdout") ? fopen(textpath, "w") : stdout;
		if (!o) err(EXIT_FAILURE, "open %s", textpath);
		emit_text(o, &idb, scrc, wcrc, stable, total, nvol);
		if (o != stdout) fclose(o);
	}
	if (jsonpath) {
		FILE *o = fopen(jsonpath, "w");
		if (!o) err(EXIT_FAILURE, "open %s", jsonpath);
		emit_json(o, &idb, scrc, wcrc, stable, total, nvol);
		fclose(o);
	}
	/* exit 0 only when the device passes the integrity verdict */
	return (idb.id_ok && idb.ready && !idb.cfg_invalid) ? EXIT_SUCCESS : EXIT_FAILURE;
}
