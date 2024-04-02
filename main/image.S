	.section ".data", "aw"	
	.globl kernel_start, kernel_end, dtb_start, dtb_end
	.balign 16
kernel_start:
.incbin "Image"
kernel_end:
