; Test-only ABI-preserving harness. Executes the production stubs.
option casemap:none
EXTERN bone_eater_dof_setup_stub:PROC
EXTERN bone_eater_dof_combine_stub:PROC
.code
PUBLIC dof_test_resume
dof_test_resume PROC
    ret
dof_test_resume ENDP
PUBLIC dof_test_setup
dof_test_setup PROC
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
    movdqu XMMWORD PTR [rsp+0], xmm6
    movdqu XMMWORD PTR [rsp+16], xmm7
    movdqu XMMWORD PTR [rsp+32], xmm8
    movdqu XMMWORD PTR [rsp+48], xmm9
    movdqu XMMWORD PTR [rsp+64], xmm10
    movdqu XMMWORD PTR [rsp+80], xmm11
    movdqu XMMWORD PTR [rsp+96], xmm12
    movdqu XMMWORD PTR [rsp+112], xmm13
    movdqu XMMWORD PTR [rsp+128], xmm14
    movdqu XMMWORD PTR [rsp+144], xmm15
    mov [rsp+160], rcx
    mov [rsp+168], rdx
    stmxcsr DWORD PTR [rsp+184]
    mov r11, rcx
    ldmxcsr DWORD PTR [r11+400]
    movdqu xmm0, XMMWORD PTR [r11+128]
    movdqu xmm1, XMMWORD PTR [r11+144]
    movdqu xmm2, XMMWORD PTR [r11+160]
    movdqu xmm3, XMMWORD PTR [r11+176]
    movdqu xmm4, XMMWORD PTR [r11+192]
    movdqu xmm5, XMMWORD PTR [r11+208]
    movdqu xmm6, XMMWORD PTR [r11+224]
    movdqu xmm7, XMMWORD PTR [r11+240]
    movdqu xmm8, XMMWORD PTR [r11+256]
    movdqu xmm9, XMMWORD PTR [r11+272]
    movdqu xmm10, XMMWORD PTR [r11+288]
    movdqu xmm11, XMMWORD PTR [r11+304]
    movdqu xmm12, XMMWORD PTR [r11+320]
    movdqu xmm13, XMMWORD PTR [r11+336]
    movdqu xmm14, XMMWORD PTR [r11+352]
    movdqu xmm15, XMMWORD PTR [r11+368]
    push QWORD PTR [r11+120]
    mov rax, QWORD PTR [r11+0]
    mov rcx, QWORD PTR [r11+8]
    mov rdx, QWORD PTR [r11+16]
    mov rbx, QWORD PTR [r11+24]
    mov rbp, QWORD PTR [r11+32]
    mov rsi, QWORD PTR [r11+40]
    mov rdi, QWORD PTR [r11+48]
    mov r8, QWORD PTR [r11+56]
    mov r9, QWORD PTR [r11+64]
    mov r10, QWORD PTR [r11+72]
    mov r12, QWORD PTR [r11+88]
    mov r13, QWORD PTR [r11+96]
    mov r14, QWORD PTR [r11+104]
    mov r15, QWORD PTR [r11+112]
    mov r11, QWORD PTR [r11+80]
    popfq
    mov QWORD PTR [rsp+176], rsp
    call bone_eater_dof_setup_stub
    pushfq
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov r10, QWORD PTR [rsp+296]
    mov rax, QWORD PTR [rsp+112]
    mov QWORD PTR [r10+0], rax
    mov rax, QWORD PTR [rsp+104]
    mov QWORD PTR [r10+8], rax
    mov rax, QWORD PTR [rsp+96]
    mov QWORD PTR [r10+16], rax
    mov rax, QWORD PTR [rsp+88]
    mov QWORD PTR [r10+24], rax
    mov rax, QWORD PTR [rsp+80]
    mov QWORD PTR [r10+32], rax
    mov rax, QWORD PTR [rsp+72]
    mov QWORD PTR [r10+40], rax
    mov rax, QWORD PTR [rsp+64]
    mov QWORD PTR [r10+48], rax
    mov rax, QWORD PTR [rsp+56]
    mov QWORD PTR [r10+56], rax
    mov rax, QWORD PTR [rsp+48]
    mov QWORD PTR [r10+64], rax
    mov rax, QWORD PTR [rsp+40]
    mov QWORD PTR [r10+72], rax
    mov rax, QWORD PTR [rsp+32]
    mov QWORD PTR [r10+80], rax
    mov rax, QWORD PTR [rsp+24]
    mov QWORD PTR [r10+88], rax
    mov rax, QWORD PTR [rsp+16]
    mov QWORD PTR [r10+96], rax
    mov rax, QWORD PTR [rsp+8]
    mov QWORD PTR [r10+104], rax
    mov rax, QWORD PTR [rsp+0]
    mov QWORD PTR [r10+112], rax
    mov rax, QWORD PTR [rsp+120]
    mov QWORD PTR [r10+120], rax
    movdqu XMMWORD PTR [r10+128], xmm0
    movdqu XMMWORD PTR [r10+144], xmm1
    movdqu XMMWORD PTR [r10+160], xmm2
    movdqu XMMWORD PTR [r10+176], xmm3
    movdqu XMMWORD PTR [r10+192], xmm4
    movdqu XMMWORD PTR [r10+208], xmm5
    movdqu XMMWORD PTR [r10+224], xmm6
    movdqu XMMWORD PTR [r10+240], xmm7
    movdqu XMMWORD PTR [r10+256], xmm8
    movdqu XMMWORD PTR [r10+272], xmm9
    movdqu XMMWORD PTR [r10+288], xmm10
    movdqu XMMWORD PTR [r10+304], xmm11
    movdqu XMMWORD PTR [r10+320], xmm12
    movdqu XMMWORD PTR [r10+336], xmm13
    movdqu XMMWORD PTR [r10+352], xmm14
    movdqu XMMWORD PTR [r10+368], xmm15
    mov rax, QWORD PTR [rsp+304]
    mov QWORD PTR [r10+384], rax
    lea rax, [rsp+128]
    mov QWORD PTR [r10+392], rax
    stmxcsr DWORD PTR [r10+400]
    cld
    add rsp, 128
    movdqu xmm6, XMMWORD PTR [rsp+0]
    movdqu xmm7, XMMWORD PTR [rsp+16]
    movdqu xmm8, XMMWORD PTR [rsp+32]
    movdqu xmm9, XMMWORD PTR [rsp+48]
    movdqu xmm10, XMMWORD PTR [rsp+64]
    movdqu xmm11, XMMWORD PTR [rsp+80]
    movdqu xmm12, XMMWORD PTR [rsp+96]
    movdqu xmm13, XMMWORD PTR [rsp+112]
    movdqu xmm14, XMMWORD PTR [rsp+128]
    movdqu xmm15, XMMWORD PTR [rsp+144]
    ldmxcsr DWORD PTR [rsp+184]
    add rsp, 200
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret
dof_test_setup ENDP
PUBLIC dof_test_combine
dof_test_combine PROC
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
    movdqu XMMWORD PTR [rsp+0], xmm6
    movdqu XMMWORD PTR [rsp+16], xmm7
    movdqu XMMWORD PTR [rsp+32], xmm8
    movdqu XMMWORD PTR [rsp+48], xmm9
    movdqu XMMWORD PTR [rsp+64], xmm10
    movdqu XMMWORD PTR [rsp+80], xmm11
    movdqu XMMWORD PTR [rsp+96], xmm12
    movdqu XMMWORD PTR [rsp+112], xmm13
    movdqu XMMWORD PTR [rsp+128], xmm14
    movdqu XMMWORD PTR [rsp+144], xmm15
    mov [rsp+160], rcx
    mov [rsp+168], rdx
    stmxcsr DWORD PTR [rsp+184]
    mov r11, rcx
    ldmxcsr DWORD PTR [r11+400]
    movdqu xmm0, XMMWORD PTR [r11+128]
    movdqu xmm1, XMMWORD PTR [r11+144]
    movdqu xmm2, XMMWORD PTR [r11+160]
    movdqu xmm3, XMMWORD PTR [r11+176]
    movdqu xmm4, XMMWORD PTR [r11+192]
    movdqu xmm5, XMMWORD PTR [r11+208]
    movdqu xmm6, XMMWORD PTR [r11+224]
    movdqu xmm7, XMMWORD PTR [r11+240]
    movdqu xmm8, XMMWORD PTR [r11+256]
    movdqu xmm9, XMMWORD PTR [r11+272]
    movdqu xmm10, XMMWORD PTR [r11+288]
    movdqu xmm11, XMMWORD PTR [r11+304]
    movdqu xmm12, XMMWORD PTR [r11+320]
    movdqu xmm13, XMMWORD PTR [r11+336]
    movdqu xmm14, XMMWORD PTR [r11+352]
    movdqu xmm15, XMMWORD PTR [r11+368]
    push QWORD PTR [r11+120]
    mov rax, QWORD PTR [r11+0]
    mov rcx, QWORD PTR [r11+8]
    mov rdx, QWORD PTR [r11+16]
    mov rbx, QWORD PTR [r11+24]
    mov rbp, QWORD PTR [r11+32]
    mov rsi, QWORD PTR [r11+40]
    mov rdi, QWORD PTR [r11+48]
    mov r8, QWORD PTR [r11+56]
    mov r9, QWORD PTR [r11+64]
    mov r10, QWORD PTR [r11+72]
    mov r12, QWORD PTR [r11+88]
    mov r13, QWORD PTR [r11+96]
    mov r14, QWORD PTR [r11+104]
    mov r15, QWORD PTR [r11+112]
    mov r11, QWORD PTR [r11+80]
    popfq
    mov QWORD PTR [rsp+176], rsp
    call bone_eater_dof_combine_stub
    pushfq
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov r10, QWORD PTR [rsp+296]
    mov rax, QWORD PTR [rsp+112]
    mov QWORD PTR [r10+0], rax
    mov rax, QWORD PTR [rsp+104]
    mov QWORD PTR [r10+8], rax
    mov rax, QWORD PTR [rsp+96]
    mov QWORD PTR [r10+16], rax
    mov rax, QWORD PTR [rsp+88]
    mov QWORD PTR [r10+24], rax
    mov rax, QWORD PTR [rsp+80]
    mov QWORD PTR [r10+32], rax
    mov rax, QWORD PTR [rsp+72]
    mov QWORD PTR [r10+40], rax
    mov rax, QWORD PTR [rsp+64]
    mov QWORD PTR [r10+48], rax
    mov rax, QWORD PTR [rsp+56]
    mov QWORD PTR [r10+56], rax
    mov rax, QWORD PTR [rsp+48]
    mov QWORD PTR [r10+64], rax
    mov rax, QWORD PTR [rsp+40]
    mov QWORD PTR [r10+72], rax
    mov rax, QWORD PTR [rsp+32]
    mov QWORD PTR [r10+80], rax
    mov rax, QWORD PTR [rsp+24]
    mov QWORD PTR [r10+88], rax
    mov rax, QWORD PTR [rsp+16]
    mov QWORD PTR [r10+96], rax
    mov rax, QWORD PTR [rsp+8]
    mov QWORD PTR [r10+104], rax
    mov rax, QWORD PTR [rsp+0]
    mov QWORD PTR [r10+112], rax
    mov rax, QWORD PTR [rsp+120]
    mov QWORD PTR [r10+120], rax
    movdqu XMMWORD PTR [r10+128], xmm0
    movdqu XMMWORD PTR [r10+144], xmm1
    movdqu XMMWORD PTR [r10+160], xmm2
    movdqu XMMWORD PTR [r10+176], xmm3
    movdqu XMMWORD PTR [r10+192], xmm4
    movdqu XMMWORD PTR [r10+208], xmm5
    movdqu XMMWORD PTR [r10+224], xmm6
    movdqu XMMWORD PTR [r10+240], xmm7
    movdqu XMMWORD PTR [r10+256], xmm8
    movdqu XMMWORD PTR [r10+272], xmm9
    movdqu XMMWORD PTR [r10+288], xmm10
    movdqu XMMWORD PTR [r10+304], xmm11
    movdqu XMMWORD PTR [r10+320], xmm12
    movdqu XMMWORD PTR [r10+336], xmm13
    movdqu XMMWORD PTR [r10+352], xmm14
    movdqu XMMWORD PTR [r10+368], xmm15
    mov rax, QWORD PTR [rsp+304]
    mov QWORD PTR [r10+384], rax
    lea rax, [rsp+128]
    mov QWORD PTR [r10+392], rax
    stmxcsr DWORD PTR [r10+400]
    cld
    add rsp, 128
    movdqu xmm6, XMMWORD PTR [rsp+0]
    movdqu xmm7, XMMWORD PTR [rsp+16]
    movdqu xmm8, XMMWORD PTR [rsp+32]
    movdqu xmm9, XMMWORD PTR [rsp+48]
    movdqu xmm10, XMMWORD PTR [rsp+64]
    movdqu xmm11, XMMWORD PTR [rsp+80]
    movdqu xmm12, XMMWORD PTR [rsp+96]
    movdqu xmm13, XMMWORD PTR [rsp+112]
    movdqu xmm14, XMMWORD PTR [rsp+128]
    movdqu xmm15, XMMWORD PTR [rsp+144]
    ldmxcsr DWORD PTR [rsp+184]
    add rsp, 200
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret
dof_test_combine ENDP
END
