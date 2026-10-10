; ABI self-test helper: proves that cider_enter_guest preserves every register Windows x64 requires.
;
; uint64_t cider_abi_selftest(GuestCall* rcx, RegSnapshot* rdx)
;   RegSnapshot { uint64 gpr[8] (rbx rbp rdi rsi r12 r13 r14 r15); uint8 xmm[10][16] (xmm6..xmm15) }
; Loads the snapshot into the callee-saved registers, calls cider_enter_guest(rcx), then writes the
; register values found AFTER the call back into the snapshot so the caller can compare them.
OPTION CASEMAP:NONE

EXTERN cider_enter_guest:PROC

.code
cider_abi_selftest PROC
    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 40                           ; 8 pushes (rsp%16==8) + 40 => 16-byte aligned, 32 bytes shadow space
    mov qword ptr [rsp+32], rdx           ; keep the snapshot pointer across the call

    mov rbx, qword ptr [rdx+0]
    mov rbp, qword ptr [rdx+8]
    mov rdi, qword ptr [rdx+16]
    mov rsi, qword ptr [rdx+24]
    mov r12, qword ptr [rdx+32]
    mov r13, qword ptr [rdx+40]
    mov r14, qword ptr [rdx+48]
    mov r15, qword ptr [rdx+56]
    movups xmm6,  xmmword ptr [rdx+64]
    movups xmm7,  xmmword ptr [rdx+80]
    movups xmm8,  xmmword ptr [rdx+96]
    movups xmm9,  xmmword ptr [rdx+112]
    movups xmm10, xmmword ptr [rdx+128]
    movups xmm11, xmmword ptr [rdx+144]
    movups xmm12, xmmword ptr [rdx+160]
    movups xmm13, xmmword ptr [rdx+176]
    movups xmm14, xmmword ptr [rdx+192]
    movups xmm15, xmmword ptr [rdx+208]

    call cider_enter_guest                ; rcx = GuestCall* is still intact

    mov rdx, qword ptr [rsp+32]
    mov qword ptr [rdx+0], rbx
    mov qword ptr [rdx+8], rbp
    mov qword ptr [rdx+16], rdi
    mov qword ptr [rdx+24], rsi
    mov qword ptr [rdx+32], r12
    mov qword ptr [rdx+40], r13
    mov qword ptr [rdx+48], r14
    mov qword ptr [rdx+56], r15
    movups xmmword ptr [rdx+64],  xmm6
    movups xmmword ptr [rdx+80],  xmm7
    movups xmmword ptr [rdx+96],  xmm8
    movups xmmword ptr [rdx+112], xmm9
    movups xmmword ptr [rdx+128], xmm10
    movups xmmword ptr [rdx+144], xmm11
    movups xmmword ptr [rdx+160], xmm12
    movups xmmword ptr [rdx+176], xmm13
    movups xmmword ptr [rdx+192], xmm14
    movups xmmword ptr [rdx+208], xmm15

    add rsp, 40
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    ret
cider_abi_selftest ENDP

END
