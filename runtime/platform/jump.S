.text
.globl browser_setjmp
.p2align 4
browser_setjmp:
    movq %rbx, 0(%rcx)
    movq %rbp, 8(%rcx)
    movq %rsi, 16(%rcx)
    movq %rdi, 24(%rcx)
    movq %r12, 32(%rcx)
    movq %r13, 40(%rcx)
    movq %r14, 48(%rcx)
    movq %r15, 56(%rcx)
    leaq 8(%rsp), %rax
    movq %rax, 64(%rcx)
    movq (%rsp), %rax
    movq %rax, 72(%rcx)
    movdqu %xmm6, 80(%rcx)
    movdqu %xmm7, 96(%rcx)
    movdqu %xmm8, 112(%rcx)
    movdqu %xmm9, 128(%rcx)
    movdqu %xmm10, 144(%rcx)
    movdqu %xmm11, 160(%rcx)
    movdqu %xmm12, 176(%rcx)
    movdqu %xmm13, 192(%rcx)
    movdqu %xmm14, 208(%rcx)
    movdqu %xmm15, 224(%rcx)
    stmxcsr 240(%rcx)
    fnstcw 244(%rcx)
    xorl %eax, %eax
    ret

.globl browser_longjmp
.p2align 4
browser_longjmp:
    movl %edx, %eax
    testl %eax, %eax
    jnz 1f
    incl %eax
1:
    movq 0(%rcx), %rbx
    movq 8(%rcx), %rbp
    movq 16(%rcx), %rsi
    movq 24(%rcx), %rdi
    movq 32(%rcx), %r12
    movq 40(%rcx), %r13
    movq 48(%rcx), %r14
    movq 56(%rcx), %r15
    movdqu 80(%rcx), %xmm6
    movdqu 96(%rcx), %xmm7
    movdqu 112(%rcx), %xmm8
    movdqu 128(%rcx), %xmm9
    movdqu 144(%rcx), %xmm10
    movdqu 160(%rcx), %xmm11
    movdqu 176(%rcx), %xmm12
    movdqu 192(%rcx), %xmm13
    movdqu 208(%rcx), %xmm14
    movdqu 224(%rcx), %xmm15
    ldmxcsr 240(%rcx)
    fldcw 244(%rcx)
    movq 64(%rcx), %rsp
    jmp *72(%rcx)

.globl browser_call_on_stack
.p2align 4
browser_call_on_stack:
    pushq %r12
    movq %rsp, %r12
    movq %rcx, %rsp
    andq $-16, %rsp
    subq $32, %rsp
    movq %rdx, %rcx
    movq %r8, %rdx
    callq *%r9
    movq %r12, %rsp
    popq %r12
    retq
