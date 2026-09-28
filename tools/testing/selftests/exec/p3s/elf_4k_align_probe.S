/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/unistd.h>

	.section .text
	.global _start
_start:
	adrp	x9, bss_probe
	add	x9, x9, :lo12:bss_probe
	mov	w10, #0x5a
	strb	w10, [x9]
	ldrb	w11, [x9]
	cmp	w10, w11
	b.ne	.Lfail

	/* The first 4K page past the BSS must still be unmapped. */
	adrp	x0, bss_probe_end
	add	x0, x0, :lo12:bss_probe_end
	add	x0, x0, #0xfff
	and	x0, x0, #~0xfff
	mov	x9, x0
	tst	x9, #0x3fff
	b.eq	.Lfail
	mov	x1, #4096
	mov	x2, #3			/* PROT_READ | PROT_WRITE */
	mov	x3, #0x22		/* MAP_PRIVATE | MAP_ANONYMOUS */
	movk	x3, #0x10, lsl #16	/* MAP_FIXED_NOREPLACE */
	mov	x4, #-1
	mov	x5, #0
	mov	x8, #__NR_mmap
	svc	#0
	cmp	x0, x9
	b.ne	.Lfail
	mov	x1, #4096
	mov	x8, #__NR_munmap
	svc	#0
	mov	x0, #0
	b	.Lexit

.Lfail:
	mov	x0, #1
.Lexit:
	mov	x8, #__NR_exit
	svc	#0

	.section .bss
	.balign 16384
bss_probe:
	.skip	4096
bss_probe_end:
