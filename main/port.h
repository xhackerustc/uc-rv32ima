/*
 * Copyright (c) 2023, Jisheng Zhang <jszhang@kernel.org>. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PORT_H
#define PORT_H

uint64_t GetTimeMicroseconds();
int IsKBHit();
int ReadKBByte();
int load_images(int ram_size, int *kern_len);

#endif /* PORT_H */
