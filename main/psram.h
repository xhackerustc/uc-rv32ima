/*
 * Copyright (c) 2023, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PSRAM_H
#define PSRAM_H

#include "driver/spi_master.h"

extern spi_device_handle_t handle;

int psram_init(void);
int psram_read(spi_device_handle_t h, uint32_t addr, void *buf, int len);
int psram_write(spi_device_handle_t h, uint32_t addr, void *buf, int len);

#endif /* PSRAM_H */
