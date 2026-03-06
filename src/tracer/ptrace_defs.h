#ifndef BDTRACE_PTRACE_DEFS_H
#define BDTRACE_PTRACE_DEFS_H

#include <sys/ptrace.h>
#include <sys/user.h>

// Syscall numbers per architecture
#ifdef __x86_64__

#define SYS_OPEN_NR      2
#define SYS_OPENAT_NR    257
#define SYS_EXECVE_NR    59
#define SYS_FORK_NR      57
#define SYS_CLONE_NR     56
#define SYS_VFORK_NR     58
#define SYS_EXIT_GROUP_NR 231
#define SYS_CLOSE_NR     3

// Register accessors for x86_64
#define REG_SYSCALL(regs) ((regs).orig_rax)
#define REG_ARG0(regs)    ((regs).rdi)
#define REG_ARG1(regs)    ((regs).rsi)
#define REG_ARG2(regs)    ((regs).rdx)
#define REG_RETVAL(regs)  ((regs).rax)

#elif defined(__i386__)

#define SYS_OPEN_NR      5
#define SYS_OPENAT_NR    295
#define SYS_EXECVE_NR    11
#define SYS_FORK_NR      2
#define SYS_CLONE_NR     120
#define SYS_VFORK_NR     190
#define SYS_EXIT_GROUP_NR 252
#define SYS_CLOSE_NR     6

// Register accessors for i386
#define REG_SYSCALL(regs) ((regs).orig_eax)
#define REG_ARG0(regs)    ((regs).ebx)
#define REG_ARG1(regs)    ((regs).ecx)
#define REG_ARG2(regs)    ((regs).edx)
#define REG_RETVAL(regs)  ((regs).eax)

#else
#error "Unsupported architecture"
#endif

#endif // BDTRACE_PTRACE_DEFS_H
