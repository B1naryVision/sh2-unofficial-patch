#pragma once

// For naked x86 observation hooks. EBX anchors the saved GPR image; C callees
// preserve it. Original ESP is EBX+36, saved ECX is EBX+24, EBP is EBX+8.
// Isolate CRT/OS code from a full or exceptional game FPU stack, and restore
// x87, XMM, MXCSR, flags and every GPR before re-emitting stock instructions.
// The 16-byte outgoing area supports up to four cdecl arguments.
#define OBSERVER_ENTER \
    "pushfl\n\t" \
    "pushal\n\t" \
    "movl %esp, %ebx\n\t" \
    "subl $528, %esp\n\t" \
    "andl $-16, %esp\n\t" \
    "fxsave (%esp)\n\t" \
    "fninit\n\t" \
    "movl $0x1f80, 512(%esp)\n\t" \
    "ldmxcsr 512(%esp)\n\t" \
    "cld\n\t" \
    "subl $16, %esp\n\t"

#define OBSERVER_LEAVE \
    "addl $16, %esp\n\t" \
    "fxrstor (%esp)\n\t" \
    "movl %ebx, %esp\n\t" \
    "popal\n\t" \
    "popfl\n\t"

// Frame/reset callbacks already have ordinary C call frames. Their integer
// prologues are safe; isolate any formatting/OS calls inside the callback.
class ObserverFpScope {
    alignas(16) unsigned char state[512];
    unsigned defaultMxcsr = 0x1f80;
public:
    ObserverFpScope() {
        __asm__ volatile("fxsave %0\n\tfninit\n\tldmxcsr %1"
                         : "=m"(state) : "m"(defaultMxcsr) : "memory");
    }
    ~ObserverFpScope() {
        __asm__ volatile("fxrstor %0" : : "m"(state) : "memory");
    }
    ObserverFpScope(const ObserverFpScope &) = delete;
    ObserverFpScope &operator=(const ObserverFpScope &) = delete;
};
