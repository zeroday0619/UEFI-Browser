.text
.global process_enter
.type process_enter,@function
process_enter:
 push %rbp
 push %rbx
 push %r12
 push %r13
 push %r14
 push %r15
 mov %rsp,0(%rdi)
 mov 32(%rdi),%rax
 mov 40(%rdi),%rdx
 mov 48(%rdi),%rcx
 mov 56(%rdi),%rsi
 mov 64(%rdi),%r8
 mov 72(%rdi),%r9
 push %r9
 push %rdx
 pushq $2
 push %r8
 push %rax
 mov %rcx,%rdi
 iretq
.global process_interrupt
process_interrupt:
 # The supervisor stack has a context pointer above the hardware frame.
 mov 56(%rsp),%rax
 mov 0(%rsp),%rdx
 mov %rdx,8(%rax)
 mov 8(%rsp),%rdx
 mov %rdx,16(%rax)
 mov %cr2,%rdx
 mov %rdx,24(%rax)
 mov 24(%rsp),%rdx
 mov %rdx,80(%rax)
 mov 0(%rax),%rsp
 pop %r15
 pop %r14
 pop %r13
 pop %r12
 pop %rbx
 pop %rbp
 ret
.altmacro
.macro ISR number
.global process_isr_\number
process_isr_\number:
 .if (\number != 8) && (\number != 10) && (\number != 11) && (\number != 12) && (\number != 13) && (\number != 14) && (\number != 17) && (\number != 21) && (\number != 29) && (\number != 30)
 pushq $0
 .endif
 pushq $\number
 jmp process_interrupt
.endm
.set vector,0
.rept 256
 ISR %vector
 .set vector,vector+1
.endr
.section .rodata,"a"
.global process_interrupt_table
process_interrupt_table:
.macro POINTER number
 .quad process_isr_\number
.endm
.set vector,0
.rept 256
 POINTER %vector
 .set vector,vector+1
.endr

# This position-independent program is the entire unprivileged worker image.
# Its only mappings are its code, command, stack, source and destination buffers.
.section .rodata.process_program,"a"
.global process_program_start,process_program_end
process_program_start:
 mov 0(%rdi),%rax
 cmp $1,%rax
 je .Lfault
 cmp $2,%rax
 je .Lprivate
 cmp $3,%rax
 je .Lbarrier
 mov 8(%rdi),%rsi
 mov 16(%rdi),%rdx
 mov 24(%rdi),%r8
 mov 32(%rdi),%r9
 mov 40(%rdi),%r10
 mov 48(%rdi),%r11
 xor %rbx,%rbx
.Lrow:
 cmp %r9,%rbx
 jae .Ldone
 xor %rcx,%rcx
# Scalar prefixes align streaming stores without dropping odd-width tails.
.Lpixel:
 cmp %r8,%rcx
 jae .Lnext
 lea (%rdx,%rcx,4),%rax
 test $15,%rax
 jnz .Lscalar
 lea 4(%rcx),%rax
 cmp %r8,%rax
 ja .Lscalar
 movdqu (%rsi,%rcx,4),%xmm0
 test %r11,%r11
 jz .Lvector_store
# SSE2 converts four BGRA pixels to RGB with zero reserved bytes.
 movdqa %xmm0,%xmm1
 psrld $16,%xmm1
 pand .Llow_mask(%rip),%xmm1
 movdqa %xmm0,%xmm2
 pslld $16,%xmm2
 pand .Lhigh_mask(%rip),%xmm2
 pand .Lgreen_mask(%rip),%xmm0
 por %xmm1,%xmm0
 por %xmm2,%xmm0
.Lvector_store:
 movntdq %xmm0,(%rdx,%rcx,4)
 add $4,%rcx
 jmp .Lpixel
.Lscalar:
 mov (%rsi,%rcx,4),%eax
 test %r11,%r11
 jz .Lstore
 bswap %eax
 shr $8,%eax
.Lstore:
 mov %eax,(%rdx,%rcx,4)
 inc %rcx
 jmp .Lpixel
.Lnext:
 lea (%rsi,%r8,4),%rsi
 lea (%rdx,%r10,4),%rdx
 inc %rbx
 jmp .Lrow
.Lfault:
 mov 56(%rdi),%rax
 movq $0x1234,(%rax)
 jmp .Ldone
.Lprivate:
 mov 56(%rdi),%rax
 addq $1,(%rax)
 jmp .Ldone
.Lbarrier:
 mov 56(%rdi),%rax
 lock incq (%rax)
 mov 24(%rdi),%rcx
.Lbarrier_wait:
 pause
 cmp %rcx,(%rax)
 jb .Lbarrier_wait
.Ldone:
 sfence
 int $0x80
 ud2
.balign 16
.Llow_mask: .long 0xff,0xff,0xff,0xff
.Lhigh_mask: .long 0xff0000,0xff0000,0xff0000,0xff0000
.Lgreen_mask: .long 0xff00,0xff00,0xff00,0xff00
process_program_end:
.section .note.GNU-stack,"",@progbits
