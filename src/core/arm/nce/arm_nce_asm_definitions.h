/* SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project */
/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#define __ASSEMBLY__

#include <asm-generic/unistd.h>

// <asm-generic/signal.h> is deliberately not included: it typedefs sigset_t unconditionally, so it
// fails to compile ("typedef redefinition") in any translation unit where glibc's sigset_t was
// already pulled in by an earlier include (k_process.cpp). These are the generic Linux signal
// numbers; arm_nce.cpp checks them against <signal.h> at compile time.
#define ReturnToRunCodeByExceptionLevelChangeSignal 12 /* SIGUSR2 */
#define BreakFromRunCodeSignal 23                      /* SIGURG */
#define GuestAccessFaultSignal 11                      /* SIGSEGV */
#define GuestAlignmentFaultSignal 7                    /* SIGBUS */

#define GuestContextSp 0xF8
#define GuestContextHostContext 0x320

#define HostContextSpTpidrEl0 0xE0
#define HostContextTpidrEl0 0xE8
#define HostContextRegs 0x0
#define HostContextVregs 0x60

#define TpidrEl0NativeContext 0x10
#define TpidrEl0Lock 0x18
#define TpidrEl0TlsMagic 0x20
#define TlsMagic 0x555a5559

#define SpinLockLocked 0
#define SpinLockUnlocked 1
