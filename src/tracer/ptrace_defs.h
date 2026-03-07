#ifndef BDTRACE_PTRACE_DEFS_H
#define BDTRACE_PTRACE_DEFS_H

#include <sys/ptrace.h>
#include <sys/user.h>

// Syscall numbers per architecture
#ifdef __x86_64__

#define SYS_OPEN_NR        2
#define SYS_CLOSE_NR       3
#define SYS_STAT_NR        4
#define SYS_LSTAT_NR       6
#define SYS_ACCESS_NR      21
#define SYS_FORK_NR        57
#define SYS_VFORK_NR       58
#define SYS_EXECVE_NR      59
#define SYS_CLONE_NR       56
#define SYS_EXIT_GROUP_NR  231
#define SYS_CHDIR_NR       80
#define SYS_FCHDIR_NR      81
#define SYS_RENAME_NR      82
#define SYS_MKDIR_NR       83
#define SYS_LINK_NR        86
#define SYS_UNLINK_NR      87
#define SYS_SYMLINK_NR     88
#define SYS_READLINK_NR    89
#define SYS_CHMOD_NR       90
#define SYS_CHOWN_NR       92
#define SYS_MKNOD_NR       133
#define SYS_TRUNCATE_NR    76
#define SYS_CREAT_NR       85
#define SYS_OPENAT_NR      257
#define SYS_MKDIRAT_NR     258
#define SYS_MKNODAT_NR     259
#define SYS_FCHOWNAT_NR    260
#define SYS_UNLINKAT_NR    263
#define SYS_RENAMEAT_NR    264
#define SYS_LINKAT_NR      265
#define SYS_SYMLINKAT_NR   266
#define SYS_READLINKAT_NR  267
#define SYS_FCHMODAT_NR    268
#define SYS_FACCESSAT_NR   269
#define SYS_NEWFSTATAT_NR  262
#define SYS_UTIMENSAT_NR   280
#define SYS_RENAMEAT2_NR   316
#define SYS_EXECVEAT_NR    322
#define SYS_STATX_NR       332
#define SYS_FACCESSAT2_NR  439
#define SYS_OPENAT2_NR     437

// Register accessors for x86_64
#define REG_SYSCALL(regs) ((regs).orig_rax)
#define REG_ARG0(regs)    ((regs).rdi)
#define REG_ARG1(regs)    ((regs).rsi)
#define REG_ARG2(regs)    ((regs).rdx)
#define REG_ARG3(regs)    ((regs).r10)
#define REG_RETVAL(regs)  ((regs).rax)

#elif defined(__i386__)

#define SYS_OPEN_NR        5
#define SYS_CLOSE_NR       6
#define SYS_STAT_NR        106
#define SYS_LSTAT_NR       107
#define SYS_ACCESS_NR      33
#define SYS_FORK_NR        2
#define SYS_VFORK_NR       190
#define SYS_EXECVE_NR      11
#define SYS_CLONE_NR       120
#define SYS_EXIT_GROUP_NR  252
#define SYS_CHDIR_NR       12
#define SYS_FCHDIR_NR      133
#define SYS_RENAME_NR      38
#define SYS_MKDIR_NR       39
#define SYS_LINK_NR        9
#define SYS_UNLINK_NR      10
#define SYS_SYMLINK_NR     83
#define SYS_READLINK_NR    85
#define SYS_CHMOD_NR       15
#define SYS_CHOWN_NR       182
#define SYS_MKNOD_NR       14
#define SYS_TRUNCATE_NR    92
#define SYS_CREAT_NR       8
#define SYS_OPENAT_NR      295
#define SYS_MKDIRAT_NR     296
#define SYS_MKNODAT_NR     297
#define SYS_FCHOWNAT_NR    298
#define SYS_UNLINKAT_NR    301
#define SYS_RENAMEAT_NR    302
#define SYS_LINKAT_NR      303
#define SYS_SYMLINKAT_NR   304
#define SYS_READLINKAT_NR  305
#define SYS_FCHMODAT_NR    306
#define SYS_FACCESSAT_NR   307
#define SYS_NEWFSTATAT_NR  300
#define SYS_UTIMENSAT_NR   320
#define SYS_RENAMEAT2_NR   353
#define SYS_EXECVEAT_NR    358
#define SYS_STATX_NR       383
#define SYS_FACCESSAT2_NR  439
#define SYS_OPENAT2_NR     437

// Register accessors for i386
#define REG_SYSCALL(regs) ((regs).orig_eax)
#define REG_ARG0(regs)    ((regs).ebx)
#define REG_ARG1(regs)    ((regs).ecx)
#define REG_ARG2(regs)    ((regs).edx)
#define REG_ARG3(regs)    ((regs).esi)
#define REG_RETVAL(regs)  ((regs).eax)

#else
#error "Unsupported architecture"
#endif

#endif // BDTRACE_PTRACE_DEFS_H
