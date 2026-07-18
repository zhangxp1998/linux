/* SPDX-License-Identifier: GPL-2.0 */

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
	mov	x0, #0
	b	.Lexit

.Lfail:
	mov	x0, #1
.Lexit:
	mov	x8, #93
	svc	#0

	.section .bss
	.balign 8
bss_probe:
	.skip	8192
