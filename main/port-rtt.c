/*
 * Copyright (c) 2024, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "rtthread.h"
#include "drv_spi.h"
#include "drv_pin.h"
#include "termios.h"

#include "psram.h"

extern struct MiniRV32IMAState core;
extern void DumpState(struct MiniRV32IMAState *core);
extern void app_main(void);
//extern char kernel_start[], kernel_end[];
#define kernel_start 0x60000
#define kernel_end 0x1e3b94

static int is_eofd;

static void ResetKeyboardInput(void)
{
	// Re-enable echo, etc. on keyboard.
	struct termios term;
	tcgetattr(0, &term);
	term.c_lflag |= ICANON | ECHO;
	tcsetattr(0, TCSANOW, &term);
}

static void CtrlC(int sig)
{
	DumpState(&core);
	ResetKeyboardInput();
	exit(0);
}

// Override keyboard, so we can capture all keyboard input for the VM.
static void CaptureKeyboardInput(void)
{
	struct termios term;

	// Hook exit, because we want to re-enable keyboard.
	signal(SIGINT, CtrlC);

	tcgetattr(0, &term);
	term.c_lflag &= ~(ICANON | ECHO); // Disable echo as well
	tcsetattr(0, TCSANOW, &term);
}

uint64_t GetTimeMicroseconds()
{
	return rt_tick_get_millisecond();
}

int ReadKBByte(void)
{
	char rxchar;
	int rread;

	if (is_eofd)
		return 0xffffffff;

	rread = read(0, &rxchar, 1);

	if (rread > 0) // Tricky: getchar can't be used with arrow keys.
		return rxchar;
	else
		return -1;
}

int IsKBHit(void)
{
	int byteswaiting;

	if (is_eofd)
		return -1;

	ioctl(0, FIONREAD, &byteswaiting);
	// Is end-of-file for
	if (!byteswaiting && write(0, 0, 0 ) != 0) {
		is_eofd = 1;
		return -1;
	}
	return !!byteswaiting;
}

#define SPI_HOST	"spi6"
#define SPI_NAME	"spi60"
#define GPIO_CS		(3*32+23)
#define SPI_FREQ	48000000; // 48MHz

#define CMD_WRITE	0x02
#define CMD_READ	0x03
#define CMD_FAST_READ	0x0b
#define CMD_RESET_EN	0x66
#define CMD_RESET	0x99
#define CMD_READ_ID	0x9f

static struct rt_spi_device *spi_dev;

static void psram_send_cmd(struct rt_spi_device *h, const uint8_t cmd)
{
	struct rt_spi_message msg = { };
	msg.send_buf = &cmd;
	msg.length = sizeof(cmd);

	rt_spi_transfer_message(h, &msg);
}

static void psram_read_id(struct rt_spi_device *h, uint8_t *rxdata)
{
	struct rt_spi_message msg = { };
	uint8_t cmd[4];

	/* CMD_READ_ID with 24bit dummy addr */
	cmd[0] = CMD_READ_ID;
	msg.send_buf = &cmd;
	msg.length = sizeof(cmd);
	rt_spi_transfer_message(h, &msg);

	msg.send_buf = RT_NULL;
	msg.recv_buf = rxdata;
	msg.length = 6;
	rt_spi_transfer_message(h, &msg);
}

int psram_init(void)
{
	uint8_t id[6];
	struct rt_spi_configuration cfg;

	memset(id, 0, sizeof(id));
	rt_pin_mode(GPIO_CS, PIN_MODE_OUTPUT);
	rt_pin_write(GPIO_CS, PIN_HIGH);

	rt_hw_spi_device_attach(SPI_HOST, SPI_NAME, GPIO_CS);
	spi_dev = (struct rt_spi_device *)rt_device_find(SPI_NAME);
	if (RT_NULL == spi_dev) {
		printf("can't find %s device!\n", SPI_NAME);
		return -RT_ERROR;
	}

	cfg.data_width = 8;
	cfg.mode = RT_SPI_MASTER | RT_SPI_MODE_0 | RT_SPI_MSB | RT_SPI_NO_CS;
	cfg.max_hz = SPI_FREQ;
	rt_spi_configure(spi_dev, &cfg);

	rt_pin_write(GPIO_CS, PIN_HIGH);
	rt_thread_mdelay(1);

	rt_pin_write(GPIO_CS, PIN_LOW);
	psram_send_cmd(spi_dev, CMD_RESET_EN);
	psram_send_cmd(spi_dev, CMD_RESET);
	rt_pin_write(GPIO_CS, PIN_HIGH);
	rt_thread_mdelay(1);

	rt_pin_write(GPIO_CS, PIN_LOW);
	psram_read_id(spi_dev, id);
	rt_pin_write(GPIO_CS, PIN_HIGH);

	printf("PSRAM ID: %02x%02x%02x%02x%02x%02x\n", id[0], id[1], id[2], id[3], id[4], id[5]);
	return 0;
}

int psram_read(uint32_t addr, void *buf, int len)
{
	struct rt_spi_message msg = { };
	/* cmdaddr[4] is dummy cycle */
	uint8_t cmdaddr[5];

	cmdaddr[0] = CMD_FAST_READ;
	cmdaddr[1] = (addr >> 16) & 0xff;
	cmdaddr[2] = (addr >> 8) & 0xff;
	cmdaddr[3] = (addr >> 0) & 0xff;
	msg.send_buf = &cmdaddr;
	msg.recv_buf = RT_NULL;
	msg.length = sizeof(cmdaddr);

	rt_pin_write(GPIO_CS, PIN_LOW);

	rt_spi_transfer_message(spi_dev, &msg);

	msg.send_buf = RT_NULL;
	msg.recv_buf = buf;
	msg.length = len;
	rt_spi_transfer_message(spi_dev, &msg);

	rt_pin_write(GPIO_CS, PIN_HIGH);

	return len;
}

int psram_write(uint32_t addr, void *buf, int len)
{
	struct rt_spi_message msg = { };
	uint8_t cmdaddr[4];

	cmdaddr[0] = CMD_WRITE;
	cmdaddr[1] = (addr >> 16) & 0xff;
	cmdaddr[2] = (addr >> 8) & 0xff;
	cmdaddr[3] = (addr >> 0) & 0xff;
	msg.send_buf = &cmdaddr;
	msg.recv_buf = RT_NULL;
	msg.length = sizeof(cmdaddr);

	rt_pin_write(GPIO_CS, PIN_LOW);

	rt_spi_transfer_message(spi_dev, &msg);

	msg.send_buf = buf;
	msg.length = len;
	rt_spi_transfer_message(spi_dev, &msg);

	rt_pin_write(GPIO_CS, PIN_HIGH);

	return len;
}

int load_images(int ram_size, int *kern_len)
{
	int flen;
	char dmabuf[64];
	uint32_t addr;
	void *flashaddr;

	printf("kernel_start: %x kernel_end: %x\n", kernel_start, kernel_end);
	flen = kernel_end - kernel_start;
	if (flen > ram_size) {
		printf("Error: Could not fit RAM image (%d bytes) into %d\n", flen, ram_size);
		return -1;
	}
	if (kern_len)
		*kern_len = flen;

	addr = 0;
	flashaddr = kernel_start;
	printf("loading kernel Image (%d bytes) from flash:%lx into psram:%lx\n", flen, (uint32_t)flashaddr, addr);
	while (flen >= 64) {
		memcpy(dmabuf, flashaddr, 64);
		psram_write(addr, dmabuf, 64);
		addr += 64;
		flashaddr += 64;
		flen -= 64;
	}
	if (flen) {
		memcpy(dmabuf, flashaddr, flen);
		psram_write(addr, dmabuf, flen);
	}

	return 0;
}

#if 0
static void psram_test(void)
{
	int i;
	uint32_t addr;
	uint32_t t1;
	uint8_t testbuf[64];

	printf("Writing PSRAM...\n");
	fflush(stdout);
	for (addr = 0; addr < 1 * 1024 * 1024; ++addr) {
		uint8_t data = addr & 0xff;
		psram_write(addr, &data, 1);
	}

	printf("Reading PSRAM...\n");
	fflush(stdout);
	for (addr = 0; addr < 1 * 1024 * 1024; ++addr) {
		uint8_t data, expected;

		expected = addr & 0xff;
		psram_read(addr, &data, 1);
		if (data != expected)
			printf("PSRAM 8bit read failed at %lx (%x != %x)\n", addr, data, expected);
	}
	printf("PSRAM 8bit read pass.\n");
	fflush(stdout);

	for (addr = 0; addr < 1 * 1024 * 1024; addr += 2) {
		uint16_t data, expected;
		expected = (((addr + 1) & 0xff) << 8) |
			   (addr & 0xff);
		psram_read(addr, &data, 2);
		if (data != expected)
			printf("PSRAM 16bit read failed at %lx (%x != %x)\n", addr, data, expected);
	}
	printf("PSRAM 16bit read pass.\n");
	fflush(stdout);

	for (addr = 0; addr < 1 * 1024 * 1024; addr += 4) {
		uint32_t data, expected;
		expected = (((addr + 3) & 0xff) << 24) |
			   (((addr + 2) & 0xff) << 16) |
			   (((addr + 1) & 0xff) << 8) |
			   (addr & 0xff);
		psram_read(addr, &data, 4);
		if (data != expected)
			printf("PSRAM 32bit read failed at %lx (%lx != %lx)\n", addr, data, expected);
	}
	printf("PSRAM 32bit read pass.\n");
	fflush(stdout);

	memset(testbuf, 0x5a, sizeof(testbuf));
	t1 = rt_tick_get_millisecond();
	for (i = 0; i < 10000; ++i) {
		psram_write(i * 64, &testbuf, 64);
	}
	t1 = rt_tick_get_millisecond() - t1;
	t1 /= 1000;
	printf("PSRAM write speed: %ld B/s.\n", 64 * 10000 / t1);
	fflush(stdout);

	t1 = rt_tick_get_millisecond();
	for (i = 0; i < 10000; ++i) {
		psram_read(i * 64, &testbuf, 64);
	}
	t1 = rt_tick_get_millisecond() - t1;
	t1 /= 1000;
	printf("PSRAM read speed: %ld B/s.\n", 64 * 10000 / t1);
	fflush(stdout);

	printf("PSRAM test done\n");
}
#endif

int linux(void)
{
	CaptureKeyboardInput();
	app_main();
	//psram_init();
	//psram_test();

	return 0;
}
MSH_CMD_EXPORT(linux, run linux);
