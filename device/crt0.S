/* crt0.S — OABI C runtime entry for the Hi3515 DVR.
 * Linux ABI: at _start, sp -> [argc][argv0][argv1]...[0][envp...].
 * We pass argc/argv to main(), then call OABI exit with its return value.
 */
    .text
    .global _start
    .type _start, %function
_start:
    ldr   r0, [sp]            @ argc
    add   r1, sp, #4          @ argv
    bl    main                @ r0 = main(argc, argv)
    mov   r7, #0             @ (unused in OABI)
    swi   #0x900001           @ __NR_exit with r0 = return code
    .size _start, .-_start
