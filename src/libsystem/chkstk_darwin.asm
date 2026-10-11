; ___chkstk_darwin - the stack probe clang emits (with the allocation size in RAX) before it grows the stack by more than a page:
;
;     mov  eax, <frame size>
;     call ___chkstk_darwin
;     sub  rsp, rax
;
; Apple's routine touches every page of the new area so that the guard page is hit in order and returns with RAX and every other
; register unchanged. A guest thread's stack here is committed in full with one inaccessible guard page below it, so there is nothing
; to commit: the routine only has to preserve all registers (a positional thunk would clobber RAX). A frame larger than the stack
; faults on its first use of the guard page, exactly like a failed probe would.
OPTION CASEMAP:NONE

.code
PUBLIC orvyn_chkstk_darwin
orvyn_chkstk_darwin PROC
    ret
orvyn_chkstk_darwin ENDP

END
