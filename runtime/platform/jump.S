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

/* The interrupt return clears NMI blocking before the supervisor abandons the payload stack. */
.globl renderer_trap_breakpoint,renderer_trap_nmi,renderer_trap_invalid,renderer_trap_page,renderer_trap_general,renderer_trap_divide
renderer_trap_nmi:
    pushq $0
    pushq $2
    jmp renderer_trap_common
renderer_trap_breakpoint:
    pushq $0
    pushq $3
    jmp renderer_trap_common
renderer_trap_invalid:
    pushq $0
    pushq $6
    jmp renderer_trap_common
renderer_trap_divide:
    pushq $0
    pushq $0
    jmp renderer_trap_common
renderer_trap_general:
    pushq $13
    jmp renderer_trap_common
renderer_trap_page:
    pushq $14
renderer_trap_common:
    pushq %rax
    pushq %r11
    cmpq $2,16(%rsp)
    jne 1f
    movl $1,renderer_nmi_seen(%rip)
1:
    xorl %eax,%eax
    xchgl %eax,renderer_trap_armed(%rip)
    testl %eax,%eax
    jz 2f
    movq 16(%rsp),%rax
    movq %rax,renderer_fault(%rip)
    movq 24(%rsp),%rax
    movq %rax,renderer_fault+8(%rip)
    movq 32(%rsp),%rax
    movq %rax,renderer_fault+16(%rip)
    movq 40(%rsp),%rax
    movq %rax,renderer_fault+24(%rip)
    movq %cr2,%rax
    movq %rax,renderer_fault+32(%rip)
    testb $3,40(%rsp)
    jz 3f
    movq renderer_supervisor_cs(%rip),%rax
    movq %rax,40(%rsp)
    movq renderer_supervisor_ss(%rip),%rax
    movq %rax,64(%rsp)
    movq $2,48(%rsp)
3:
    leaq renderer_escape(%rip),%rax
    movq %rax,32(%rsp)
    movq renderer_escape_stack(%rip),%rax
    movq %rax,56(%rsp)
2:
    popq %r11
    popq %rax
    addq $16,%rsp
    iretq
.globl renderer_escape
renderer_escape:
    leaq renderer_escape_context(%rip),%rcx
    movl $1,%edx
    jmp browser_longjmp

/* The first four arguments use the loader ABI; the payload entry uses SysV. */
.globl renderer_enter_user
renderer_enter_user:
    movq 40(%rsp),%r10
    movq 48(%rsp),%r11
    pushq %r9
    pushq %rdx
    pushq $2
    pushq %r8
    pushq %rcx
    xor %edi,%edi
    xor %esi,%esi
    movq %r10,%rdx
    movq %r11,%rcx
    xor %ebp,%ebp
    iretq

.globl renderer_system_interrupt
renderer_system_interrupt:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    cld
    mov %rsp,%rcx
    mov %rsp,%rbx
    and $-16,%rsp
    sub $32,%rsp
    call renderer_system_dispatch
    mov %rbx,%rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    iretq

.altmacro
.macro RENDERER_EXCEPTION number
renderer_exception_\number:
 .if (\number != 8) && (\number != 10) && (\number != 11) && (\number != 12) && (\number != 13) && (\number != 14) && (\number != 17) && (\number != 21) && (\number != 29) && (\number != 30)
 pushq $0
 .endif
 pushq $\number
 jmp renderer_trap_common
.endm
.set renderer_vector,0
.rept 32
 RENDERER_EXCEPTION %renderer_vector
 .set renderer_vector,renderer_vector+1
.endr
.section .rdata,"dr"
.globl renderer_exception_table
renderer_exception_table:
.macro RENDERER_EXCEPTION_POINTER number
 .quad renderer_exception_\number
.endm
.set renderer_vector,0
.rept 32
 RENDERER_EXCEPTION_POINTER %renderer_vector
 .set renderer_vector,renderer_vector+1
.endr
