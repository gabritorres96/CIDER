; CIDER 0.2 - System V AMD64 (macOS) <-> Windows x64 bridge (MASM, x64).
; Layout of GuestCall (include/cider/abi.hpp, static_assert-ed there):
;   +0 fn  +8 stack_top  +16..+40 arg[4]  +48 host_rsp  +56 guest_rsp  +64 result  +72 exited  +80 exit_code

OPTION CASEMAP:NONE

.data
PUBLIC cider_current_call
cider_current_call QWORD 0

.code

; uint64_t cider_enter_guest(GuestCall* rcx)      Windows x64 ABI on the outside
cider_enter_guest PROC
    ; Windows requires these to survive the call: rbx rbp rdi rsi r12-r15 xmm6-xmm15.
    ; System V guest code may clobber rdi, rsi and xmm6-xmm15, so save everything.
    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 168                         ; 160 bytes of xmm + 8 padding: keeps rsp 16-byte aligned
    movups xmmword ptr [rsp+0],   xmm6
    movups xmmword ptr [rsp+16],  xmm7
    movups xmmword ptr [rsp+32],  xmm8
    movups xmmword ptr [rsp+48],  xmm9
    movups xmmword ptr [rsp+64],  xmm10
    movups xmmword ptr [rsp+80],  xmm11
    movups xmmword ptr [rsp+96],  xmm12
    movups xmmword ptr [rsp+112], xmm13
    movups xmmword ptr [rsp+128], xmm14
    movups xmmword ptr [rsp+144], xmm15

    mov rbx, rcx
    mov qword ptr [rbx+48], rsp          ; host_rsp (16-byte aligned)
    mov cider_current_call, rbx
    mov rax, qword ptr [rbx+0]           ; guest function
    mov rdi, qword ptr [rbx+16]          ; System V integer argument registers
    mov rsi, qword ptr [rbx+24]
    mov rdx, qword ptr [rbx+32]
    mov rcx, qword ptr [rbx+40]
    mov rsp, qword ptr [rbx+8]           ; switch to the guest stack (16-byte aligned: after CALL, rsp%16 == 8)
    cld                                  ; System V requires DF=0 at calls
    call rax                             ; <-- guest code runs here
    mov r11, cider_current_call
    mov qword ptr [r11+64], rax
    jmp cider_host_epilogue
cider_enter_guest ENDP

; Restores the Windows registers saved by cider_enter_guest and returns to its caller.
; In:  r11 = GuestCall* (result already stored at +64).
cider_host_epilogue PROC
    mov rsp, qword ptr [r11+48]
    mov rax, qword ptr [r11+64]
    movups xmm6,  xmmword ptr [rsp+0]
    movups xmm7,  xmmword ptr [rsp+16]
    movups xmm8,  xmmword ptr [rsp+32]
    movups xmm9,  xmmword ptr [rsp+48]
    movups xmm10, xmmword ptr [rsp+64]
    movups xmm11, xmmword ptr [rsp+80]
    movups xmm12, xmmword ptr [rsp+96]
    movups xmm13, xmmword ptr [rsp+112]
    movups xmm14, xmmword ptr [rsp+128]
    movups xmm15, xmmword ptr [rsp+144]
    add rsp, 168
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    ret
cider_host_epilogue ENDP

; Common target of every per-symbol stub. Entered by the guest with a System V call:
;   [rsp] = guest return address, rdi rsi rdx rcx r8 r9 = integer arguments, r10 = Windows function.
cider_guest_thunk PROC
    mov r11, cider_current_call
    mov qword ptr [r11+56], rsp          ; remember the guest stack
    mov rsp, qword ptr [r11+48]          ; back to the HOST stack (below the saved registers)
    sub rsp, 48                          ; 32 bytes shadow space + 2 stack argument slots + alignment
    mov qword ptr [rsp+32], r8           ; 5th integer argument  -> Windows stack slot
    mov qword ptr [rsp+40], r9           ; 6th integer argument  -> Windows stack slot
    mov r9, rcx                          ; 4th
    mov r8, rdx                          ; 3rd
    mov rdx, rsi                         ; 2nd
    mov rcx, rdi                         ; 1st
    call r10                             ; Windows x64 function; rax / xmm0 hold the result
    mov r11, cider_current_call
    cmp qword ptr [r11+72], 0            ; did the function request exit()?
    jne guest_exited
    mov rsp, qword ptr [r11+56]          ; back to the guest stack, return into guest code
    ret
guest_exited:
    mov rax, qword ptr [r11+80]
    mov qword ptr [r11+64], rax
    jmp cider_host_epilogue
cider_guest_thunk ENDP

END
