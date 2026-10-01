	.file	"pcaa-report-layout.c"
	.option nopic
	.attribute arch, "rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0"
	.attribute unaligned_access, 0
	.attribute stack_align, 16
	.text
	.globl	pcaa_report_context_bytes
	.section	.sdata,"aw"
	.align	2
	.type	pcaa_report_context_bytes, @object
	.size	pcaa_report_context_bytes, 4
pcaa_report_context_bytes:
	.word	40032
	.ident	"GCC: (14.2.0+19) 14.2.0"
	.section	.note.GNU-stack,"",@progbits
