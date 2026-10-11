// Minimal declarations for the PRO CFW SystemCtrl kernel API.
// Stubs live in SystemCtrlForKernel.S.
#ifndef PSPCAP_SYSTEMCTRL_H
#define PSPCAP_SYSTEMCTRL_H

#include <psptypes.h>

int sctrlHENGetVersion(void);
int sctrlHENGetMinorVersion(void);

// Returns the address of an exported function, or 0 if not found.
u32 sctrlHENFindFunction(const char *modname, const char *libname, u32 nid);

// Points every syscall that targets `addr` at `newaddr` instead.
void sctrlHENPatchSyscall(void *addr, void *newaddr);

#endif
