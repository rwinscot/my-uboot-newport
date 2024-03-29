/*
 * (C) Copyright 2014-2016, Cavium Networks, Inc
 *
 * SPDX-License-Identifier:	GPL-2.0+
 *
 * Extracted from armv8/start.S
 */

#include <config.h>
#include <linux/linkage.h>
#include <asm/arch/thunderx_svc.h>

#include <asm/macro.h>

.align 8
.global fdt_base_addr
fdt_base_addr:
	.dword 0x0

.global save_boot_params
save_boot_params:
	/* Read FDT base from x1 register passed by ATF */
	adr	x21, fdt_base_addr
	str	x1, [x21]

	/* Returns */
	b	save_boot_params_ret

ENTRY(lowlevel_init)
	mov	x29, lr			/* Save LR */

	/* any lowlevel init should go here */

	mov	lr, x29			/* Restore LR */
	ret
ENDPROC(lowlevel_init)

