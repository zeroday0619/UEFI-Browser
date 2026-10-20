.text
.globl firmware_thread_switch
.type firmware_thread_switch,@function
firmware_thread_switch:
  mov %rsp, 0(%rdi)
  mov %rbx, 8(%rdi)
  mov %rbp, 16(%rdi)
  mov %r12, 24(%rdi)
  mov %r13, 32(%rdi)
  mov %r14, 40(%rdi)
  mov %r15, 48(%rdi)
  fxsave64 64(%rdi)
  mov %rsi, %r8
  mov %cs,%ax
  test $3,%al
  jnz .Luser_thread
  mov $0xc0000100, %ecx
  rdmsr
  mov %eax, 576(%rdi)
  mov %edx, 580(%rdi)
  mov 576(%r8), %eax
  mov 580(%r8), %edx
  wrmsr
  jmp .Lrestore_thread
.Luser_thread:
  mov %fs:0,%rax
  mov %rax,576(%rdi)
  mov 576(%r8),%rdi
  mov $0x1001,%eax
  int $0x80
  cmp $-14,%rax
  jne .Lrestore_thread
  ud2
.Lrestore_thread:
  mov 0(%r8), %rsp
  mov 8(%r8), %rbx
  mov 16(%r8), %rbp
  mov 24(%r8), %r12
  mov 32(%r8), %r13
  mov 40(%r8), %r14
  mov 48(%r8), %r15
  fxrstor64 64(%r8)
  ret
.size firmware_thread_switch,.-firmware_thread_switch
.section .note.GNU-stack,"",@progbits
