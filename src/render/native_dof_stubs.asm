; Exact guarded sites:35AF95 after flags loaded intoR13D,355B7F afterR12D.
; Only bit3 of that local value changes. Original instruction(s) execute in the
; MinHook resume trampoline. No calls, game-data writes, XMM operations or
; persistent stack changes. TLS reads use the same TLS-index/SECREL addressing
; emitted by MSVC for the plain __declspec(thread) certificate.
; These stubs are JMP-entered inside an existing native frame. They deliberately
; do not claim ordinary PROC FRAME unwind metadata: that would describe a CALL
; frame which does not exist. Normal execution preserves stack/register state;
; asynchronous stack unwinding with a PC inside the stub remains unsupported.
option casemap:none
EXTERN _tls_index:DWORD
EXTERN bone_eater_dof_setup_resume:QWORD
EXTERN bone_eater_dof_combine_resume:QWORD
_TLS SEGMENT
EXTERN bone_eater_dof_decision:BYTE
_TLS ENDS
.code
PUBLIC bone_eater_dof_setup_stub
bone_eater_dof_setup_stub PROC
    pushfq
    push r10
    push r11
    mov r10d, DWORD PTR _tls_index
    mov r11, QWORD PTR gs:[58h]
    mov r10, QWORD PTR [r11+r10*8]
    mov r11d, SECTIONREL bone_eater_dof_decision
    add r11, r10
    cmp QWORD PTR [r11], 1
    jne setup_done
    cmp QWORD PTR [r11+8], rax
    jne setup_done
    cmp QWORD PTR [r11+16], r12
    jne setup_done
    cmp QWORD PTR [r11+24], rdi
    jne setup_done
    inc QWORD PTR [r11+40]
    movzx r10d, r13b
    mov QWORD PTR [r11+72], r10
    test r13b, 8
    jz setup_done
    and r13b, 0f7h
    inc QWORD PTR [r11+56]
setup_done:
    pop r11
    pop r10
    popfq
    jmp QWORD PTR bone_eater_dof_setup_resume
bone_eater_dof_setup_stub ENDP

PUBLIC bone_eater_dof_combine_stub
bone_eater_dof_combine_stub PROC
    pushfq
    push r10
    push r11
    mov r10d, DWORD PTR _tls_index
    mov r11, QWORD PTR gs:[58h]
    mov r10, QWORD PTR [r11+r10*8]
    mov r11d, SECTIONREL bone_eater_dof_decision
    add r11, r10
    cmp QWORD PTR [r11], 1
    jne combine_done
    cmp QWORD PTR [r11+8], rcx
    jne combine_done
    cmp QWORD PTR [r11+16], rax
    jne combine_done
    cmp QWORD PTR [r11+32], rsi
    jne combine_done
    inc QWORD PTR [r11+48]
    movzx r10d, r12b
    mov QWORD PTR [r11+80], r10
    test r12b, 8
    jz combine_done
    and r12b, 0f7h
    inc QWORD PTR [r11+64]
combine_done:
    pop r11
    pop r10
    popfq
    jmp QWORD PTR bone_eater_dof_combine_resume
bone_eater_dof_combine_stub ENDP
END
