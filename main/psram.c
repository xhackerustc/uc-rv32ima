/*
 * Copyright (c) 2023, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "psram.h"

#define GPIO_MOSI	7
#define GPIO_MISO	2
#define GPIO_CS		10
#define GPIO_SCLK	6
#define SPI_HOST_ID	1
#define SPI_FREQ	80000000; // 80MHz

#define CMD_WRITE	0x02
#define CMD_READ	0x03
#define CMD_FAST_READ	0x0b
#define CMD_RESET_EN	0x66
#define CMD_RESET	0x99
#define CMD_READ_ID	0x9f

extern const char *TAG;
spi_device_handle_t handle;

static esp_err_t psram_send_cmd(spi_device_handle_t h, const uint8_t cmd)
{
	spi_transaction_ext_t t = { };
	t.base.flags = SPI_TRANS_VARIABLE_ADDR;
	t.base.cmd = cmd;
	t.base.length = 0;
        t.command_bits = 8U;
        t.address_bits = 0;

	return spi_device_polling_transmit(h, (spi_transaction_t*)&t);
}

static esp_err_t psram_read_id(spi_device_handle_t h, uint8_t *rxdata)
{
	spi_transaction_t t = { };
	t.cmd = CMD_READ_ID;
	t.addr = 0;
	t.rx_buffer = rxdata;
	t.length = 6 * 8;
	return spi_device_polling_transmit(h, &t);
}

int psram_init(void)
{
	esp_err_t ret;
	uint8_t id[6];

	gpio_reset_pin(GPIO_CS);
	gpio_set_direction(GPIO_CS, GPIO_MODE_OUTPUT);
	gpio_set_level(GPIO_CS, 1);

	spi_bus_config_t spi_bus_config = {
		.mosi_io_num = GPIO_MOSI,
		.miso_io_num = GPIO_MISO,
		.sclk_io_num = GPIO_SCLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 0,
		.flags = 0,
	};

	ESP_LOGI(TAG, "SPI_HOST_ID = %d\n", SPI_HOST_ID);
	ret = spi_bus_initialize(SPI_HOST_ID, &spi_bus_config, SPI_DMA_CH_AUTO);
	ESP_LOGI(TAG, "spi_bus_initialize = %d\n", ret);
	if (ret != ESP_OK)
		return -1;

	spi_device_interface_config_t devcfg;
	memset(&devcfg, 0, sizeof(spi_device_interface_config_t));
	devcfg.clock_speed_hz = SPI_FREQ;
	devcfg.spics_io_num = -1;
	devcfg.queue_size = 1;
	devcfg.command_bits = 8;
	devcfg.address_bits = 24;

	ret = spi_bus_add_device(SPI_HOST_ID, &devcfg, &handle);
	ESP_LOGI(TAG, "spi_bus_add_device = %d\n", ret);
	if (ret != ESP_OK)
		return -1;

	gpio_set_level(GPIO_CS, 1);
	usleep(200);

	gpio_set_level(GPIO_CS, 0);
	psram_send_cmd(handle, CMD_RESET_EN);
	psram_send_cmd(handle, CMD_RESET);
	gpio_set_level(GPIO_CS, 1);
	usleep(200);

	gpio_set_level(GPIO_CS, 0);
	ret = psram_read_id(handle, id);
	gpio_set_level(GPIO_CS, 1);
	if (ret != ESP_OK)
		return -1;

	ESP_LOGI(TAG, "PSRAM ID: %02x%02x%02x%02x%02x%02x\n", id[0], id[1], id[2], id[3], id[4], id[5]);
	return 0;
}

int psram_read(spi_device_handle_t h, uint32_t addr, void *buf, int len)
{
	esp_err_t ret;
	spi_transaction_ext_t t = { };

	t.base.cmd = CMD_FAST_READ;
	t.base.addr = addr;
	t.base.rx_buffer = buf;
	t.base.length = len * 8;
	t.base.flags = SPI_TRANS_VARIABLE_DUMMY;
	t.dummy_bits = 8;

	gpio_set_level(GPIO_CS, 0);
	ret = spi_device_polling_transmit(h, (spi_transaction_t*)&t);
	gpio_set_level(GPIO_CS, 1);

	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "psram_read failed %lx %d\n", addr, len);
		return -1;
	}

	return len;
}

int psram_write(spi_device_handle_t h, uint32_t addr, void *buf, int len)
{
	esp_err_t ret;
	spi_transaction_t t = {};

	t.cmd = CMD_WRITE;
	t.addr = addr;
	t.tx_buffer = buf;
	t.length = len * 8;

	gpio_set_level(GPIO_CS, 0);
	ret = spi_device_polling_transmit(h, &t);
	gpio_set_level(GPIO_CS, 1);

	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "psram_write failed %lx %d\n", addr, len);
		return -1;
	}

	return len;
}

#if 0
static void psram_test(void)
{
	int i;
	uint32_t addr;
	uint64_t t1;
	uint8_t testbuf[64];

	printf("Writing PSRAM...\n");
	fflush(stdout);
	for (addr = 0; addr < 8 * 1024 * 1024; ++addr) {
		uint8_t data = addr & 0xff;
		psram_write(handle, addr, &data, 1);
	}

	printf("Reading PSRAM...\n");
	fflush(stdout);
	for (addr = 0; addr < 8 * 1024 * 1024; ++addr) {
		uint8_t data, expected;

		expected = addr & 0xff;
		psram_read(handle, addr, &data, 1);
		if (data != expected)
			printf("PSRAM 8bit read failed at %lx (%x != %x)\n", addr, data, expected);
	}
	printf("PSRAM 8bit read pass.\n");
	fflush(stdout);

	for (addr = 0; addr < 8 * 1024 * 1024; addr += 2) {
		uint16_t data, expected;
		expected = (((addr + 1) & 0xff) << 8) |
			   (addr & 0xff);
		psram_read(handle, addr, &data, 2);
		if (data != expected)
			printf("PSRAM 16bit read failed at %lx (%x != %x)\n", addr, data, expected);
	}
	printf("PSRAM 16bit read pass.\n");
	fflush(stdout);

	for (addr = 0; addr < 8 * 1024 * 1024; addr += 4) {
		uint32_t data, expected;
		expected = (((addr + 3) & 0xff) << 24) |
			   (((addr + 2) & 0xff) << 16) |
			   (((addr + 1) & 0xff) << 8) |
			   (addr & 0xff);
		psram_read(handle, addr, &data, 4);
		if (data != expected)
			printf("PSRAM 32bit read failed at %lx (%lx != %lx)\n", addr, data, expected);
	}
	printf("PSRAM 32bit read pass.\n");
	fflush(stdout);

	memset(testbuf, 0x5a, sizeof(testbuf));
	t1 = esp_timer_get_time();
	for (i = 0; i < 10000; ++i) {
		psram_write(handle, i * 64, &testbuf, 64);
	}
	t1 = esp_timer_get_time() - t1;
	t1 /= 1000;
	printf("PSRAM write speed: %lld B/s.\n", 64 * 10000 * 1000 / t1);
	fflush(stdout);

	t1 = esp_timer_get_time();
	for (i = 0; i < 10000; ++i) {
		psram_read(handle, i * 64, &testbuf, 64);
	}
	t1 = esp_timer_get_time() - t1;
	t1 /= 1000;
	printf("PSRAM read speed: %lld B/s.\n", 64 * 10000 * 1000 / t1);
	fflush(stdout);

	for (;;) {
		printf("PSRAM test done\n");
		usleep(1000000);
	}
}
#endif
