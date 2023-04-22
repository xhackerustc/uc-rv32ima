/*
 * Copyright (c) 2023, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * Use some code of mini-rv32ima.c from https://github.com/cnlohr/mini-rv32ima
 * Copyright 2022 Charles Lohr
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "hal/usb_serial_jtag_ll.h"

#include "cache.h"
#include "psram.h"

const char *TAG = "uc-rv32";
static uint32_t ram_amt = 8 * 1024 * 1024;

static uint64_t GetTimeMicroseconds();
static uint32_t HandleException(uint32_t ir, uint32_t retval);
static uint32_t HandleControlStore(uint32_t addy, uint32_t val);
static uint32_t HandleControlLoad(uint32_t addy );
static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value);
static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno);
static void MiniSleep();
static int IsKBHit();
static int ReadKBByte();

// This is the functionality we want to override in the emulator.
//  think of this as the way the emulator's processor is connected to the outside world.
#define MINIRV32WARN(x...) ESP_LOGE(TAG, x);
#define MINI_RV32_RAM_SIZE ram_amt
#define MINIRV32_IMPLEMENTATION
#define MINIRV32_POSTEXEC(pc, ir, retval) { if (retval > 0) {  retval = HandleException(ir, retval); } }
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) if (HandleControlStore(addy, val)) return val;
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval) rval = HandleControlLoad(addy);
#define MINIRV32_OTHERCSR_WRITE(csrno, value) HandleOtherCSRWrite(image, csrno, value);
#define MINIRV32_OTHERCSR_READ(csrno, value) value = HandleOtherCSRRead(image, csrno);

#define MINIRV32_CUSTOM_MEMORY_BUS
static void MINIRV32_STORE4(uint32_t ofs, uint32_t val)
{
	cache_write(ofs, &val, 4);
}

static void MINIRV32_STORE2(uint32_t ofs, uint16_t val)
{
	cache_write(ofs, &val, 2);
}

static void MINIRV32_STORE1(uint32_t ofs, uint8_t val)
{
	cache_write(ofs, &val, 1);
}

static uint32_t MINIRV32_LOAD4(uint32_t ofs)
{
	uint32_t val;
	cache_read(ofs, &val, 4);
	return val;
}

static uint16_t MINIRV32_LOAD2(uint32_t ofs)
{
	uint16_t val;
	cache_read(ofs, &val, 2);
	return val;
}

static uint8_t MINIRV32_LOAD1(uint32_t ofs)
{
	uint8_t val;
	cache_read(ofs, &val, 1);
	return val;
}

#include "mini-rv32ima.h"

static void DumpState(struct MiniRV32IMAState *core)
{
	unsigned int pc = core->pc;
	unsigned int *regs = (unsigned int *)core->regs;
	uint64_t thit, taccessed;

	cache_get_stat(&thit, &taccessed);
	ESP_LOGI(TAG, "hit: %llu accessed: %llu\n", thit, taccessed);
	ESP_LOGI(TAG, "PC: %08x ", pc);
	ESP_LOGI(TAG, "Z:%08x ra:%08x sp:%08x gp:%08x tp:%08x t0:%08x t1:%08x t2:%08x s0:%08x s1:%08x a0:%08x a1:%08x a2:%08x a3:%08x a4:%08x a5:%08x ",
		regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7],
		regs[8], regs[9], regs[10], regs[11], regs[12], regs[13], regs[14], regs[15] );
	ESP_LOGI(TAG, "a6:%08x a7:%08x s2:%08x s3:%08x s4:%08x s5:%08x s6:%08x s7:%08x s8:%08x s9:%08x s10:%08x s11:%08x t3:%08x t4:%08x t5:%08x t6:%08x\n",
		regs[16], regs[17], regs[18], regs[19], regs[20], regs[21], regs[22], regs[23],
		regs[24], regs[25], regs[26], regs[27], regs[28], regs[29], regs[30], regs[31] );
}

static struct MiniRV32IMAState core;

#define dtb_start	0x3ff000
#define dtb_end		0x3ff5c0
#define kernel_start	0x200000
#define kernel_end	0x3c922c

static char dmabuf[64];
void app_main(void)
{
	int dtb_ptr;
	long flen;
	uint32_t addr, flashaddr;

	usleep(5000000);
	ESP_LOGI(TAG, "psram init\n");

	if (psram_init() < 0) {
		ESP_LOGI(TAG, "failed to init psram\n");
		return;
	}

restart:
	flen = kernel_end - kernel_start;
	if (flen > ram_amt) {
		ESP_LOGE(TAG, "Error: Could not fit RAM image (%ld bytes) into %"PRIu32"\n", flen, ram_amt);
		return;
	}

	addr = 0;
	flashaddr = kernel_start;
	ESP_LOGI(TAG, "loading kernel Image (%ld bytes) from flash:%lx into psram:%lx\n", flen, flashaddr, addr);
	while (flen >= 64) {
		esp_flash_read(NULL, dmabuf, flashaddr, 64);
		psram_write(handle, addr, dmabuf, 64);
		addr += 64;
		flashaddr += 64;
		flen -= 64;
	}
	if (flen) {
		esp_flash_read(NULL, dmabuf, flashaddr, flen);
		psram_write(handle, addr, dmabuf, flen);
	}

	flen = dtb_end - dtb_start;
	dtb_ptr = ram_amt - flen;

	addr = dtb_ptr;
	flashaddr = dtb_start;
	ESP_LOGI(TAG, "loading dtb (%ld bytes) from flash:%lx into psram:%lx\n", flen, flashaddr, addr);
	while (flen >= 64) {
		esp_flash_read(NULL, dmabuf, flashaddr, 64);
		psram_write(handle, addr, dmabuf, 64);
		addr += 64;
		flashaddr += 64;
		flen -= 64;
	}
	if (flen) {
		esp_flash_read(NULL, dmabuf, flashaddr, flen);
		psram_write(handle, addr, dmabuf, flen);
	}

	core.pc = MINIRV32_RAM_IMAGE_OFFSET;
	core.regs[10] = 0x00; //hart ID
	 //dtb_pa must be valid pointer
	core.regs[11] = dtb_ptr + MINIRV32_RAM_IMAGE_OFFSET;
	core.extraflags |= 3; // Machine-mode.

	// Image is loaded.
	uint64_t lastTime = GetTimeMicroseconds();
	int instrs_per_flip = 1024;
	ESP_LOGI(TAG, "RV32IMA starting\n");
	while (1) {
		int ret;
		uint64_t *this_ccount = ((uint64_t*)&core.cyclel);
		uint32_t elapsedUs = GetTimeMicroseconds() / 6 - lastTime;

		lastTime += elapsedUs;
		 // Execute upto 1024 cycles before breaking out.
		ret = MiniRV32IMAStep(&core, NULL, 0, elapsedUs, instrs_per_flip);
		switch (ret) {
		case 0:
			break;
		case 1:
			MiniSleep();
			*this_ccount += instrs_per_flip;
			break;
		case 3:
			break;

		//syscon code for restart
		case 0x7777:
			goto restart;

		//syscon code for power-off
		case 0x5555:
			ESP_LOGI(TAG, "POWEROFF@0x%"PRIu32"%"PRIu32"\n", core.cycleh, core.cyclel);
			DumpState(&core);
			return;
		default:
			ESP_LOGI(TAG, "Unknown failure\n");
			break;
		}
	}

	DumpState(&core);
}

//////////////////////////////////////////////////////////////////////////
// Platform-specific functionality
//////////////////////////////////////////////////////////////////////////
static void MiniSleep(void)
{
	vTaskDelay(pdMS_TO_TICKS(10));
}

static uint64_t GetTimeMicroseconds()
{
	return esp_timer_get_time();
}

static int ReadKBByte(void)
{
	uint8_t rxchar;
	int rread;

	rread = usb_serial_jtag_ll_read_rxfifo(&rxchar, 1);

	if (rread > 0)
		return rxchar;
	else
		return -1;
}

static int IsKBHit(void)
{
	return usb_serial_jtag_ll_rxfifo_data_available();
}

static uint32_t HandleException(uint32_t ir, uint32_t code)
{
	// Weird opcode emitted by duktape on exit.
	if (code == 3) {
		// Could handle other opcodes here.
	}
	return code;
}

static uint32_t HandleControlStore(uint32_t addy, uint32_t val)
{
	//UART 8250 / 16550 Data Buffer
	if (addy == 0x10000000) {
		while (usb_serial_jtag_ll_write_txfifo((uint8_t *)&val, 1) < 1) ;
		usb_serial_jtag_ll_txfifo_flush();
	}
	return 0;
}

static uint32_t HandleControlLoad(uint32_t addy)
{
	// Emulating a 8250 / 16550 UART
	if (addy == 0x10000005)
		return 0x60 | IsKBHit();
	else if (addy == 0x10000000 && IsKBHit())
		return ReadKBByte();
	return 0;
}

static void HandleOtherCSRWrite(uint8_t *image, uint16_t csrno, uint32_t value)
{
	uint32_t ptrstart, ptrend;

	switch (csrno) {
	case 0x136:
		ESP_LOGI(TAG, "%d", (int)value);
		break;
	case 0x137:
		ESP_LOGI(TAG, "%08x", (int)value);
		break;
	case 0x138:
		//Print "string"
		ptrstart = value - MINIRV32_RAM_IMAGE_OFFSET;
		ptrend = ptrstart;
		if (ptrstart >= ram_amt)
			ESP_LOGE(TAG, "DEBUG PASSED INVALID PTR (%"PRIu32")\n", value);
		while (ptrend < ram_amt) {
			uint8_t c = MINIRV32_LOAD1(ptrend);
			if (c == 0)
				break;
			while (usb_serial_jtag_ll_write_txfifo((uint8_t *)&value, 1) < 1) ;
			usb_serial_jtag_ll_txfifo_flush();
			ptrend++;
		}
		break;
	case 0x139:
		while (usb_serial_jtag_ll_write_txfifo((uint8_t *)&value, 1) < 1) ;
		usb_serial_jtag_ll_txfifo_flush();
		break;
	default:
		break;
	}
}

static int32_t HandleOtherCSRRead(uint8_t *image, uint16_t csrno)
{
	if (csrno == 0x140) {
		if (!IsKBHit())
			return -1;
		return ReadKBByte();
	}
	return 0;
}
