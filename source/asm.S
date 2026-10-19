    .syntax unified
    .arm
    .text

@ Unimplemented import handler: thunk leaves its index in r12. Logs name and returns 0
@ (r0 = r1 = 0, which acts as "nil" for ObjC and 0 for 64-bit integer returns).
    .global ml_unimpl_entry
    .type ml_unimpl_entry, %function
ml_unimpl_entry:
    push    {r4, lr}
    mov     r4, sp
    bic     sp, sp, #7          @ ml_unimpl_log is native PS Vita code (AAPCS 8-byte aligned)
    mov     r0, r12
    blx     ml_unimpl_log
    mov     sp, r4
    pop     {r4, lr}
    mov     r0, #0
    mov     r1, #0
    bx      lr

@ uint32_t call_guest(void *fn, a0, a1, a2, a3): invokes guest function while preserving r9,
@ which iOS treats as a scratch register.
    .global call_guest
    .type call_guest, %function
call_guest:
    push    {r4, r9, r10, lr}
    mov     r12, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    ldr     r3, [sp, #16]
    blx     r12
    pop     {r4, r9, r10, pc}

@ uint32_t call_guest6(void *fn, a0, a1, a2, a3, a4, a5): like call_guest, with 2 extra
@ stack arguments (AudioUnit render callback / 6-argument guest functions).
    .global call_guest6
    .type call_guest6, %function
call_guest6:
    push    {r4, r5, r9, r10, r11, lr}
    mov     r12, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    ldr     r3, [sp, #24]
    ldr     r4, [sp, #28]
    ldr     r5, [sp, #32]
    sub     sp, sp, #8
    str     r4, [sp]
    str     r5, [sp, #4]
    blx     r12
    add     sp, sp, #8
    pop     {r4, r5, r9, r10, r11, pc}

@ Bridge entry for native functions called by guest code (imports and hooks), with ip = target.
@ The iOS ARMv6 ABI only guarantees 4-byte stack alignment; Vita code (newlib, vitaGL)
@ expects 8-byte alignment (AAPCS) and fails on unaligned stacks (aligned NEON, 64-bit va_arg).
@ If sp is misaligned, an aligned frame is created by copying 16 words of stack arguments.
    .global ml_align_call
    .type ml_align_call, %function
ml_align_call:
    tst     sp, #4
    bxeq    ip
    push    {r4-r8, r10, r11, lr}
    mov     r4, sp
    sub     sp, sp, #64
    bic     sp, sp, #7
    add     r5, r4, #32
    mov     lr, sp
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    ldm     r5!, {r6-r8, r10}
    stm     lr!, {r6-r8, r10}
    blx     ip
    mov     sp, r4
    pop     {r4-r8, r10, r11, pc}

@ Objective-C dispatch. objc_lookup*(self, sel) returns r0 = IMP, r1 = 1 if the IMP is ours
@ (native Vita code: enter through ml_align_call) or 0 if it is guest code (plain tail call).
@ Arguments in r0-r3 and on the stack reach the IMP untouched, as with Apple's objc_msgSend.
@ pop/ldr do not modify the flags set by `cmp r1, #0`.
    .global objc_msgSend_entry
    .type objc_msgSend_entry, %function
objc_msgSend_entry:
    cmp     r0, #0
    moveq   r1, #0
    bxeq    lr                  @ message to nil: returns 0 (r0 = r1 = 0)
    push    {r0-r3, r4, lr}
    mov     r4, sp
    bic     sp, sp, #7
    blx     objc_lookup
    mov     sp, r4
    mov     ip, r0
    cmp     r1, #0
    pop     {r0-r3, r4, lr}
    bne     ml_align_call
    bx      ip

@ objc_msgSend_stret(ret, self, sel, ...): structure return pointer in r0.
    .global objc_msgSend_stret_entry
    .type objc_msgSend_stret_entry, %function
objc_msgSend_stret_entry:
    cmp     r1, #0
    bxeq    lr
    push    {r0-r3, r4, lr}
    mov     r4, sp
    bic     sp, sp, #7
    mov     r0, r1
    mov     r1, r2
    blx     objc_lookup_stret
    mov     sp, r4
    mov     ip, r0
    cmp     r1, #0
    pop     {r0-r3, r4, lr}
    bne     ml_align_call
    bx      ip

@ objc_msgSendSuper2(struct objc_super *{receiver, class}, sel, ...): lookup starts at
@ class->superclass and the IMP receives the receiver as self.
    .global objc_msgSendSuper2_entry
    .type objc_msgSendSuper2_entry, %function
objc_msgSendSuper2_entry:
    push    {r0-r3, r4, lr}
    mov     r4, sp
    bic     sp, sp, #7
    blx     objc_lookup_super2
    mov     sp, r4
    mov     ip, r0
    cmp     r1, #0
    pop     {r0-r3, r4, lr}
    ldr     r0, [r0]
    bne     ml_align_call
    bx      ip
