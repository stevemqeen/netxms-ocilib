/* 
** NetXMS subagent for FreeBSD
** Copyright (C) 2004 Alex Kirhenshtein
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
**/

#ifndef __SYSTEM_H__
#define __SYSTEM_H__

enum
{
	PHYSICAL_FREE,
	PHYSICAL_FREE_PCT,
	PHYSICAL_USED,
	PHYSICAL_USED_PCT,
	PHYSICAL_TOTAL,
	SWAP_FREE,
	SWAP_FREE_PCT,
	SWAP_USED,
	SWAP_USED_PCT,
	SWAP_TOTAL,
	VIRTUAL_FREE,
	VIRTUAL_FREE_PCT,
	VIRTUAL_USED,
	VIRTUAL_USED_PCT,
	VIRTUAL_TOTAL,
};

enum
{
	PROCINFO_CPUTIME,
	PROCINFO_KTIME,
	PROCINFO_PAGEFAULTS,
	PROCINFO_THREADS,
	PROCINFO_UTIME,
	PROCINFO_VMSIZE,
	PROCINFO_WKSET
};

#define INFOTYPE_MIN             0
#define INFOTYPE_MAX             1
#define INFOTYPE_AVG             2
#define INFOTYPE_SUM             3


LONG H_ProcessList(const TCHAR *, const TCHAR *, StringList *, AbstractCommSession *);
LONG H_Uptime(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_Uname(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_Hostname(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_Hostname(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_CpuInfo(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_CpuLoad(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_CpuUsage(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_ProcessCount(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_ProcessInfo(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_MemoryInfo(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);
LONG H_SourcePkgSupport(const TCHAR *, const TCHAR *, TCHAR *, AbstractCommSession *);

#endif // __SYSTEM_H__
