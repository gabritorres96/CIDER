; Objective-C message dispatch stubs (System V AMD64, x86_64 Darwin ABI).
;
; objc_msgSend(id self, SEL _cmd, ...) must behave as if the method implementation had been called directly: every argument register
; (rdi rsi rdx rcx r8 r9, xmm0-xmm7), AL (variadic methods) and the stack arguments reach the IMP untouched, and the IMP returns to the
; ORIGINAL caller. So the stub saves the argument registers, asks the host (a Windows-ABI function reached through the usual thunk) for
; the IMP, restores everything and JUMPS to it. The host function and the thunk run on the host stack; this code runs on the guest
; stack, which keeps a 16-byte alignment at the call (rsp % 16 == 0 after the frame is built).
OPTION CASEMAP:NONE

.data
PUBLIC orvyn_objc_lookup_thunk
PUBLIC orvyn_objc_lookup_super_thunk
orvyn_objc_lookup_thunk QWORD 0           ; thunk of Runtime::lookup_for_message(self, sel)
orvyn_objc_lookup_super_thunk QWORD 0     ; thunk of Runtime::lookup_for_super(super*, sel, mode)

.code

; ---- shared macros -----------------------------------------------------------------------------------------------------
SAVE_ARGS MACRO
    push rbp
    mov rbp, rsp
    sub rsp, 192
    mov qword ptr [rsp+0],  rdi
    mov qword ptr [rsp+8],  rsi
    mov qword ptr [rsp+16], rdx
    mov qword ptr [rsp+24], rcx
    mov qword ptr [rsp+32], r8
    mov qword ptr [rsp+40], r9
    mov qword ptr [rsp+48], rax
    movups xmmword ptr [rsp+64],  xmm0
    movups xmmword ptr [rsp+80],  xmm1
    movups xmmword ptr [rsp+96],  xmm2
    movups xmmword ptr [rsp+112], xmm3
    movups xmmword ptr [rsp+128], xmm4
    movups xmmword ptr [rsp+144], xmm5
    movups xmmword ptr [rsp+160], xmm6
    movups xmmword ptr [rsp+176], xmm7
ENDM

RESTORE_ARGS MACRO
    mov rdi, qword ptr [rsp+0]
    mov rsi, qword ptr [rsp+8]
    mov rdx, qword ptr [rsp+16]
    mov rcx, qword ptr [rsp+24]
    mov r8,  qword ptr [rsp+32]
    mov r9,  qword ptr [rsp+40]
    mov rax, qword ptr [rsp+48]
    movups xmm0, xmmword ptr [rsp+64]
    movups xmm1, xmmword ptr [rsp+80]
    movups xmm2, xmmword ptr [rsp+96]
    movups xmm3, xmmword ptr [rsp+112]
    movups xmm4, xmmword ptr [rsp+128]
    movups xmm5, xmmword ptr [rsp+144]
    movups xmm6, xmmword ptr [rsp+160]
    movups xmm7, xmmword ptr [rsp+176]
ENDM

; ---- IMP returned for a nil receiver: returns zero in every return register ------------------------------------------------
orvyn_objc_nil_return PROC
    xor eax, eax
    xor edx, edx
    xorps xmm0, xmm0
    xorps xmm1, xmm1
    ret
orvyn_objc_nil_return ENDP

; id objc_msgSend(id self, SEL op, ...)
orvyn_objc_msgSend PROC
    test rdi, rdi
    jz orvyn_objc_nil_return             ; messages to nil return 0 / 0.0 / zeroed registers
    SAVE_ARGS
    mov rax, orvyn_objc_lookup_thunk     ; rdi = self, rsi = op are already the two arguments
    call rax
    mov r11, rax                         ; IMP
    RESTORE_ARGS
    leave
    jmp r11
orvyn_objc_msgSend ENDP

; void objc_msgSend_stret(void* ret, id self, SEL op, ...)   -- methods returning a large struct: hidden pointer in rdi
orvyn_objc_msgSend_stret PROC
    test rsi, rsi
    jz nil_stret
    SAVE_ARGS
    mov rdi, rsi                         ; lookup(self, op)
    mov rsi, rdx
    mov rax, orvyn_objc_lookup_thunk
    call rax
    mov r11, rax
    RESTORE_ARGS
    leave
    jmp r11
nil_stret:
    mov rax, rdi
    ret
orvyn_objc_msgSend_stret ENDP

; id objc_msgSendSuper(struct objc_super* super, SEL op, ...)        super = { receiver, class whose METHOD LIST search starts }
orvyn_objc_msgSendSuper PROC
    SAVE_ARGS
    mov edx, 1
    mov rax, orvyn_objc_lookup_super_thunk
    call rax
    mov r11, rax
    RESTORE_ARGS
    mov rdi, qword ptr [rdi]             ; the receiver becomes `self`
    leave
    jmp r11
orvyn_objc_msgSendSuper ENDP

; id objc_msgSendSuper2(struct objc_super* super, SEL op, ...)       super = { receiver, class containing the calling method }
orvyn_objc_msgSendSuper2 PROC
    SAVE_ARGS
    mov edx, 2
    mov rax, orvyn_objc_lookup_super_thunk
    call rax
    mov r11, rax
    RESTORE_ARGS
    mov rdi, qword ptr [rdi]
    leave
    jmp r11
orvyn_objc_msgSendSuper2 ENDP

END
