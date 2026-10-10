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
    mov r10, qword ptr [rbx+0]           ; guest function
    movq xmm0, qword ptr [rbx+104]       ; System V vector argument registers (doubles / floats)
    movq xmm1, qword ptr [rbx+112]
    movq xmm2, qword ptr [rbx+120]
    movq xmm3, qword ptr [rbx+128]
    movq xmm4, qword ptr [rbx+136]
    movq xmm5, qword ptr [rbx+144]
    movq xmm6, qword ptr [rbx+152]
    movq xmm7, qword ptr [rbx+160]
    mov rdi, qword ptr [rbx+16]          ; System V integer argument registers
    mov rsi, qword ptr [rbx+24]
    mov rdx, qword ptr [rbx+32]
    mov rcx, qword ptr [rbx+40]
    mov r8,  qword ptr [rbx+88]
    mov r9,  qword ptr [rbx+96]
    mov eax, dword ptr [rbx+192]         ; AL = vector registers used (variadic callees)
    mov rsp, qword ptr [rbx+8]           ; guest stack: 16-byte aligned, stack arguments already stored at [rsp]
    cld                                  ; System V requires DF=0 at calls
    call r10                             ; <-- guest code runs here
    mov r11, cider_current_call
    mov qword ptr [r11+64], rax
    mov qword ptr [r11+168], rdx         ; second integer result
    movq qword ptr [r11+176], xmm0       ; vector results
    movq qword ptr [r11+184], xmm1
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

; Raw variant: the host function receives a pointer to a GuestRegs record (include/cider/abi.hpp) describing every
; register and the stack-argument area of the System V call, and leaves its results in the same record.
;   GuestRegs: +0 rdi +8 rsi +16 rdx +24 rcx +32 r8 +40 r9 +48 rax (AL = vector registers used by variadic calls)
;              +56 stack_args +64..+120 xmm0..xmm7 (low qwords) +128 ret_rax +136 ret_rdx +144 ret_xmm0 +152 ret_xmm1
; Entered like cider_guest_thunk: [rsp] = guest return address, r10 = host function  void fn(GuestRegs*).
cider_guest_thunk_raw PROC
    mov r11, cider_current_call
    mov qword ptr [r11+56], rsp          ; remember the guest stack
    mov rsp, qword ptr [r11+48]          ; HOST stack
    sub rsp, 208                         ; 32 shadow + 168 GuestRegs + 8 padding: rsp is 16-byte aligned at the call
    mov qword ptr [rsp+32+48], rax       ; AL of a variadic call (rax is untouched so far)
    mov qword ptr [rsp+32+0],  rdi
    mov qword ptr [rsp+32+8],  rsi
    mov qword ptr [rsp+32+16], rdx
    mov qword ptr [rsp+32+24], rcx
    mov qword ptr [rsp+32+32], r8
    mov qword ptr [rsp+32+40], r9
    mov rax, qword ptr [r11+56]          ; guest rsp: [rsp] = return address, stack arguments start at rsp+8
    add rax, 8
    mov qword ptr [rsp+32+56], rax
    movq qword ptr [rsp+32+64],  xmm0
    movq qword ptr [rsp+32+72],  xmm1
    movq qword ptr [rsp+32+80],  xmm2
    movq qword ptr [rsp+32+88],  xmm3
    movq qword ptr [rsp+32+96],  xmm4
    movq qword ptr [rsp+32+104], xmm5
    movq qword ptr [rsp+32+112], xmm6
    movq qword ptr [rsp+32+120], xmm7
    xor eax, eax
    mov qword ptr [rsp+32+128], rax      ; results start as zero
    mov qword ptr [rsp+32+136], rax
    mov qword ptr [rsp+32+144], rax
    mov qword ptr [rsp+32+152], rax
    lea rcx, qword ptr [rsp+32]          ; Windows ABI: first argument = GuestRegs*
    call r10
    mov r11, cider_current_call
    cmp qword ptr [r11+72], 0            ; did the function request exit()?
    jne raw_exited
    mov rax, qword ptr [rsp+32+128]      ; rax
    mov rdx, qword ptr [rsp+32+136]      ; rdx (second integer eightbyte)
    movq xmm0, qword ptr [rsp+32+144]
    movq xmm1, qword ptr [rsp+32+152]
    mov rsp, qword ptr [r11+56]          ; back to the guest stack, return into guest code
    ret
raw_exited:
    mov rax, qword ptr [r11+80]
    mov qword ptr [r11+64], rax
    jmp cider_host_epilogue
cider_guest_thunk_raw ENDP

; Darwin thread-local-variable convention: `leaq _x(%rip), %rdi ; callq *(%rdi)`. The callee takes the descriptor in rdi,
; returns the address in rax and PRESERVES EVERY OTHER REGISTER (clobbering only rax, r11 and the flags): compilers keep
; live values in rcx/rdx/rsi/r8-r10 and xmm registers across the call. A Windows function would trash all of them, so this
; thunk saves and restores the guest's volatile registers around it.
; Entered like cider_guest_thunk but with the host function in r11 (the stub does not touch r10, which belongs to the guest).
cider_guest_thunk_tlv PROC
    mov rax, cider_current_call
    mov qword ptr [rax+56], rsp          ; remember the guest stack
    mov rsp, qword ptr [rax+48]          ; HOST stack
    sub rsp, 192                         ; 32 shadow | +32 xmm0-5 (96) | +128 rcx rdx rsi rdi r8 r9 r10 (56) | padding
    mov qword ptr [rsp+128], rcx
    mov qword ptr [rsp+136], rdx
    mov qword ptr [rsp+144], rsi
    mov qword ptr [rsp+152], rdi
    mov qword ptr [rsp+160], r8
    mov qword ptr [rsp+168], r9
    mov qword ptr [rsp+176], r10
    movups xmmword ptr [rsp+32],  xmm0
    movups xmmword ptr [rsp+48],  xmm1
    movups xmmword ptr [rsp+64],  xmm2
    movups xmmword ptr [rsp+80],  xmm3
    movups xmmword ptr [rsp+96],  xmm4
    movups xmmword ptr [rsp+112], xmm5
    mov rcx, rdi                         ; Windows ABI: descriptor pointer
    call r11
    mov r11, cider_current_call
    cmp qword ptr [r11+72], 0            ; did the function request exit() (a trap)?
    jne tlv_exited
    mov rcx, qword ptr [rsp+128]
    mov rdx, qword ptr [rsp+136]
    mov rsi, qword ptr [rsp+144]
    mov rdi, qword ptr [rsp+152]
    mov r8,  qword ptr [rsp+160]
    mov r9,  qword ptr [rsp+168]
    mov r10, qword ptr [rsp+176]
    movups xmm0, xmmword ptr [rsp+32]
    movups xmm1, xmmword ptr [rsp+48]
    movups xmm2, xmmword ptr [rsp+64]
    movups xmm3, xmmword ptr [rsp+80]
    movups xmm4, xmmword ptr [rsp+96]
    movups xmm5, xmmword ptr [rsp+112]
    mov rsp, qword ptr [r11+56]          ; back to the guest stack; rax = result
    ret
tlv_exited:
    mov rax, qword ptr [r11+80]
    mov qword ptr [r11+64], rax
    jmp cider_host_epilogue
cider_guest_thunk_tlv ENDP

END
