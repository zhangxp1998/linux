/* SPDX-License-Identifier: GPL-2.0 */

#include <asm/unistd.h>

	.text
	.global _start
	.type _start, %function
_start:
	/* Not reached: exec starts the interpreter, which exits 0. */
	mov	x0, #1
	mov	x8, #__NR_exit
	svc	#0
	.size _start, .-_start
