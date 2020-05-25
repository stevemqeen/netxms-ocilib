/*
** NetXMS - Network Management System
** Copyright (C) 2003-2017 Victor Kirhenshtein
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published
** by the Free Software Foundation; either version 3 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** File: tools.cpp
**
**/

#include "libnetxms.h"
#include <stdarg.h>
#include <nms_agent.h>
#include <nms_threads.h>
#include <netxms-regex.h>
#include <nxstat.h>

#ifdef _WIN32
#include <psapi.h>
#endif

#if !defined(_WIN32) && !defined(UNDER_CE)
#include <sys/time.h>
#include <signal.h>
#endif

#if HAVE_SYS_UTSNAME_H
#include <sys/utsname.h>
#endif

#if HAVE_POLL_H
#include <poll.h>
#endif

#if HAVE_MALLOC_H && !WITH_JEMALLOC
#include <malloc.h>
#endif

#if HAVE_LOCALE_H
#include <locale.h>
#endif

#ifdef _WIN32
# ifndef __GNUC__
#  define EPOCHFILETIME (116444736000000000i64)
# else
#  define EPOCHFILETIME (116444736000000000LL)
# endif
#endif

/**
 * Common initialization for any NetXMS process
 *
 * @param commandLineTool set to true for command line tool initialization
 */
void LIBNETXMS_EXPORTABLE InitNetXMSProcess(bool commandLineTool)
{
   InitThreadLibrary();

   // Set locale to C. It shouldn't be needed, according to
   // documentation, but I've seen the cases when agent formats
   // floating point numbers by sprintf inserting comma in place
   // of a dot, as set by system's regional settings.
#if HAVE_SETLOCALE
   setlocale(LC_NUMERIC, "C");
#if defined(UNICODE) && !defined(_WIN32)
   const char *locale = getenv("LC_CTYPE");
   if (locale == NULL)
      locale = getenv("LC_ALL");
   if (locale == NULL)
      locale = getenv("LANG");
   if (locale != NULL)
      setlocale(LC_CTYPE, locale);
#endif
#endif

#ifdef NETXMS_MEMORY_DEBUG
   InitMemoryDebugger();
#endif

#ifdef _WIN32
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif

#if defined(__sun) || defined(_AIX) || defined(__hpux)
   signal(SIGPIPE, SIG_IGN);
   signal(SIGHUP, SIG_IGN);
   signal(SIGQUIT, SIG_IGN);
   signal(SIGUSR1, SIG_IGN);
   signal(SIGUSR2, SIG_IGN);
#endif

#ifndef _WIN32
   BlockAllSignals(true, commandLineTool);
#endif
}

/**
 * Calculate number of bits in netmask (in host byte order)
 */
int LIBNETXMS_EXPORTABLE BitsInMask(UINT32 dwMask)
{
   int bits;
   UINT32 dwTemp;

   for(bits = 0, dwTemp = dwMask; dwTemp != 0; bits++, dwTemp <<= 1);
   return bits;
}

/**
 * Convert IP address from binary form (host bytes order) to string
 */
TCHAR LIBNETXMS_EXPORTABLE *IpToStr(UINT32 dwAddr, TCHAR *szBuffer)
{
   static TCHAR szInternalBuffer[32];
   TCHAR *szBufPtr;

   szBufPtr = (szBuffer == NULL) ? szInternalBuffer : szBuffer;
   _sntprintf(szBufPtr, 32, _T("%d.%d.%d.%d"), (int)(dwAddr >> 24), (int)((dwAddr >> 16) & 255),
              (int)((dwAddr >> 8) & 255), (int)(dwAddr & 255));
   return szBufPtr;
}

#ifdef UNICODE

char LIBNETXMS_EXPORTABLE *IpToStrA(UINT32 dwAddr, char *szBuffer)
{
   static char szInternalBuffer[32];
   char *szBufPtr;

   szBufPtr = (szBuffer == NULL) ? szInternalBuffer : szBuffer;
   snprintf(szBufPtr, 32, "%d.%d.%d.%d", (int)(dwAddr >> 24), (int)((dwAddr >> 16) & 255),
            (int)((dwAddr >> 8) & 255), (int)(dwAddr & 255));
   return szBufPtr;
}

#endif

/**
 * Universal IPv4/IPv6 to string converter
 */
TCHAR LIBNETXMS_EXPORTABLE *SockaddrToStr(struct sockaddr *addr, TCHAR *buffer)
{
	switch(addr->sa_family)
	{
		case AF_INET:
			return IpToStr(ntohl(((struct sockaddr_in *)addr)->sin_addr.s_addr), buffer);
		case AF_INET6:
			return Ip6ToStr(((struct sockaddr_in6 *)addr)->sin6_addr.s6_addr, buffer);
		default:
			buffer[0] = 0;
			return buffer;
	}
}

/**
 * Convert IPv6 address from binary form to string
 */
TCHAR LIBNETXMS_EXPORTABLE *Ip6ToStr(const BYTE *addr, TCHAR *buffer)
{
   static TCHAR internalBuffer[64];
   TCHAR *bufPtr = (buffer == NULL) ? internalBuffer : buffer;

	if (!memcmp(addr, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 16))
	{
		_tcscpy(bufPtr, _T("::"));
		return bufPtr;
	}

	TCHAR *out = bufPtr;
	WORD *curr = (WORD *)addr;
	bool hasNulls = false;
	for(int i = 0; i < 8; i++)
	{
		WORD value = ntohs(*curr);
		if ((value != 0) || hasNulls)
		{
			if (out != bufPtr)
				*out++ = _T(':');
			_sntprintf(out, 5, _T("%x"), value);
			out = bufPtr + _tcslen(bufPtr);
			curr++;
		}
		else
		{
			*out++ = _T(':');
			do
			{
				i++;
				curr++;
			}
			while((*curr == 0) && (i < 8));
         if (i == 8)
         {
   			*out++ = _T(':');
            break;
         }
			i--;
			hasNulls = true;
		}
	}
	*out = 0;
   return bufPtr;
}

#ifdef UNICODE

/**
 * Convert IPv6 address from binary form to string
 */
char LIBNETXMS_EXPORTABLE *Ip6ToStrA(const BYTE *addr, char *buffer)
{
   static char internalBuffer[64];
   char *bufPtr = (buffer == NULL) ? internalBuffer : buffer;

   if (!memcmp(addr, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 16))
   {
      strcpy(bufPtr, "::");
      return bufPtr;
   }

   char *out = bufPtr;
   WORD *curr = (WORD *)addr;
   bool hasNulls = false;
   for(int i = 0; i < 8; i++)
   {
      WORD value = ntohs(*curr);
      if ((value != 0) || hasNulls)
      {
         if (out != bufPtr)
            *out++ = ':';
         snprintf(out, 5, "%x", value);
         out = bufPtr + strlen(bufPtr);
         curr++;
      }
      else
      {
         *out++ = ':';
         do
         {
            i++;
            curr++;
         }
         while((*curr == 0) && (i < 8));
         if (i == 8)
         {
            *out++ = ':';
            break;
         }
         i--;
         hasNulls = true;
      }
   }
   *out = 0;
   return bufPtr;
}

#endif

/**
 * Duplicate memory block
 */
void LIBNETXMS_EXPORTABLE *nx_memdup(const void *data, size_t size)
{
   void *newData;

   newData = malloc(size);
   memcpy(newData, data, size);
   return newData;
}

/**
 * Swap two memory blocks
 */
void LIBNETXMS_EXPORTABLE nx_memswap(void *block1, void *block2, size_t size)
{
   void *temp;

   temp = malloc(size);
   memcpy(temp, block1, size);
   memcpy(block1, block2, size);
   memcpy(block2, temp, size);
   free(temp);
}

#if defined(_WIN32) && defined(USE_WIN32_HEAP)

/**
 * Copy string
 */
char LIBNETXMS_EXPORTABLE *nx_strdup(const char *src)
{
	return (char *)nx_memdup(src, strlen(src) + 1);
}

/**
 * Copy string
 */
WCHAR LIBNETXMS_EXPORTABLE *nx_wcsdup(const WCHAR *src)
{
	return (WCHAR *)nx_memdup(src, (wcslen(src) + 1) * sizeof(WCHAR));
}

#endif

#if !HAVE_WCSDUP && !defined(_WIN32)

/**
 * Copy string
 */
WCHAR LIBNETXMS_EXPORTABLE *wcsdup(const WCHAR *src)
{
	return (WCHAR *)nx_memdup(src, (wcslen(src) + 1) * sizeof(WCHAR));
}

#elif defined(_AIX)

/**
 * Copy string
 */
WCHAR LIBNETXMS_EXPORTABLE *nx_wcsdup(const WCHAR *src)
{
	return (WCHAR *)nx_memdup(src, (wcslen(src) + 1) * sizeof(WCHAR));
}

#endif

/**
 * Compare pattern with possible ? characters with given text
 */
static bool CompareTextBlocks(const TCHAR *pattern, const TCHAR *text, size_t size)
{
	const TCHAR *p = pattern;
	const TCHAR *t = text;
	for(size_t i = size; i > 0; i--, p++, t++)
	{
		if (*p == _T('?'))
			continue;
		if (*p != *t)
			return false;
	}
	return true;
}

/**
 * Match string against pattern with * and ? metasymbols - implementation
 */
static bool MatchStringEngine(const TCHAR *pattern, const TCHAR *string)
{
   const TCHAR *SPtr, *MPtr, *BPtr, *EPtr;
	size_t bsize;
   bool finishScan;

   SPtr = string;
   MPtr = pattern;

   while(*MPtr != 0)
   {
      switch(*MPtr)
      {
         case _T('?'):
            if (*SPtr != 0)
            {
               SPtr++;
               MPtr++;
            }
            else
				{
               return false;
				}
            break;
         case _T('*'):
            while(*MPtr == _T('*'))
               MPtr++;
            if (*MPtr == 0)
	            return true;
            while(*MPtr == _T('?'))      // Handle "*?" case
            {
               if (*SPtr != 0)
                  SPtr++;
               else
                  return false;
					MPtr++;
            }
            BPtr = MPtr;           // Text block begins here
            while((*MPtr != 0) && (*MPtr != _T('*')))
               MPtr++;     // Find the end of text block
            // Try to find rightmost matching block
				bsize = (size_t)(MPtr - BPtr);
            EPtr = NULL;
            finishScan = false;
            do
            {
               while(1)
               {
                  while((*SPtr != 0) && (*SPtr != *BPtr))
                     SPtr++;
                  if (_tcslen(SPtr) < bsize)
                  {
                     if (EPtr == NULL)
                     {
                        return false;  // Length of remained text less than remaining pattern
                     }
                     else
                     {
                        SPtr = EPtr;   // Revert back to point after last match
                        finishScan = true;
                        break;
                     }
                  }
                  if (CompareTextBlocks(BPtr, SPtr, bsize))
                     break;
                  SPtr++;
               }
               if (!finishScan)
               {
                  EPtr = SPtr + bsize;   // Remember point after last match
                  SPtr++;    // continue scan at next character
               }
            }
            while(!finishScan);
            break;
         default:
            if (*SPtr == 0)
               return false;
            if (*MPtr == *SPtr)
            {
               SPtr++;
               MPtr++;
            }
            else
				{
               return false;
				}
            break;
      }
   }

   return *SPtr == 0;
}

/**
 * Match string against pattern with * and ? metasymbols
 *
 * @param pattern pattern to match against
 * @param str string to match
 * @param matchCase set to true for case-sensetive match
 * @return true if string matches given pattern
 */
bool LIBNETXMS_EXPORTABLE MatchString(const TCHAR *pattern, const TCHAR *str, bool matchCase)
{
   if (matchCase)
      return MatchStringEngine(pattern, str);

   TCHAR *tp, *ts;
   bool bResult;

   tp = _tcsdup(pattern);
   ts = _tcsdup(str);
   _tcsupr(tp);
   _tcsupr(ts);
   bResult = MatchStringEngine(tp, ts);
   free(tp);
   free(ts);
   return bResult;
}

/**
 * Strip whitespaces and tabs off the string
 */
void LIBNETXMS_EXPORTABLE StrStripA(char *str)
{
   int i;

   for(i = 0; (str[i]!=0) && ((str[i] == ' ') || (str[i] == '\t')); i++);
   if (i > 0)
      memmove(str, &str[i], strlen(&str[i]) + 1);
   for(i = (int)strlen(str) - 1; (i >= 0) && ((str[i] == ' ') || (str[i] == '\t')); i--);
   str[i + 1] = 0;
}

/**
 * Strip whitespaces and tabs off the string
 */
void LIBNETXMS_EXPORTABLE StrStripW(WCHAR *str)
{
   int i;

   for(i = 0; (str[i]!=0) && ((str[i] == L' ') || (str[i] == L'\t')); i++);
   if (i > 0)
      memmove(str, &str[i], (wcslen(&str[i]) + 1) * sizeof(WCHAR));
   for(i = (int)wcslen(str) - 1; (i >= 0) && ((str[i] == L' ') || (str[i] == L'\t')); i--);
   str[i + 1] = 0;
}

/**
 * Strip whitespaces and tabs off the string.
 *
 * @param str string to trim
 * @return str for convenience
 */
TCHAR LIBNETXMS_EXPORTABLE *Trim(TCHAR *str)
{
   if (str == NULL)
      return NULL;

   int i;
   for(i = 0; (str[i] != 0) && _istspace(str[i]); i++);
   if (i > 0)
      memmove(str, &str[i], (_tcslen(&str[i]) + 1) * sizeof(TCHAR));
   for(i = (int)_tcslen(str) - 1; (i >= 0) && _istspace(str[i]); i--);
   str[i + 1] = 0;
   return str;
}

/**
 * Remove trailing CR/LF or LF from string (multibyte version)
 */
void LIBNETXMS_EXPORTABLE RemoveTrailingCRLFA(char *str)
{
	if (*str == 0)
		return;

	char *p = str + strlen(str) - 1;
	if (*p == '\n')
		p--;
	if (*p == '\r')
		p--;
	*(p + 1) = 0;
}

/**
 * Remove trailing CR/LF or LF from string (UNICODE version)
 */
void LIBNETXMS_EXPORTABLE RemoveTrailingCRLFW(WCHAR *str)
{
	if (*str == 0)
		return;

	WCHAR *p = str + wcslen(str) - 1;
	if (*p == L'\n')
		p--;
	if (*p == L'\r')
		p--;
	*(p + 1) = 0;
}

/**
 * Expand file name. Source and destiation may point to same location.
 * Can be used strftime placeholders and external commands enclosed in ``
 */
const TCHAR LIBNETXMS_EXPORTABLE *ExpandFileName(const TCHAR *name, TCHAR *buffer, size_t bufSize, bool allowShellCommands)
{
	time_t t;
	struct tm *ltm;
#if HAVE_LOCALTIME_R
	struct tm tmBuff;
#endif
	TCHAR temp[8192], command[1024];
	size_t outpos = 0;

	t = time(NULL);
#if HAVE_LOCALTIME_R
	ltm = localtime_r(&t, &tmBuff);
#else
	ltm = localtime(&t);
#endif
	if (_tcsftime(temp, 8192, name, ltm) <= 0)
		return NULL;

	for(int i = 0; (temp[i] != 0) && (outpos < bufSize - 1); i++)
	{
		if (temp[i] == _T('`') && allowShellCommands)
		{
			int j = ++i;
			while((temp[j] != _T('`')) && (temp[j] != 0))
				j++;
			int len = MIN(j - i, 1023);
			memcpy(command, &temp[i], len * sizeof(TCHAR));
			command[len] = 0;

			FILE *p = _tpopen(command, _T("r"));
			if (p != NULL)
			{
				char result[1024];

				int rc = (int)fread(result, 1, 1023, p);
				pclose(p);

				if (rc > 0)
				{
					result[rc] = 0;
					char *lf = strchr(result, '\n');
					if (lf != NULL)
						*lf = 0;

					len = (int)MIN(strlen(result), bufSize - outpos - 1);
#ifdef UNICODE
					MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, result, len, &buffer[outpos], len);
#else
					memcpy(&buffer[outpos], result, len);
#endif
					outpos += len;
				}
			}

			i = j;
		}
      else if (temp[i] == _T('$') && temp[i + 1] == _T('{'))
      {
         i += 2;
			int j = i;
			while((temp[j] != _T('}')) && (temp[j] != 0))
				j++;

			int len = MIN(j - i, 1023);

         TCHAR varName[256];
         memcpy(varName, &temp[i], len * sizeof(TCHAR));
         varName[len] = 0;

         TCHAR *result = _tgetenv(varName);
         if (result != NULL)
         {
            len = (int)MIN(_tcslen(result), bufSize - outpos - 1);
            memcpy(&buffer[outpos], result, len * sizeof(TCHAR));
         }
         else
         {
            len = 0;
         }

         outpos += len;
         i = j;
      }
      else
		{
			buffer[outpos++] = temp[i];
		}
	}

	buffer[outpos] = 0;
	return buffer;
}

/**
 * Create folder
 */
BOOL LIBNETXMS_EXPORTABLE CreateFolder(const TCHAR *directory)
{
   NX_STAT_STRUCT st;
   TCHAR *previous = _tcsdup(directory);
   TCHAR *ptr = _tcsrchr(previous, FS_PATH_SEPARATOR_CHAR);
   BOOL result = FALSE;
   if (ptr != NULL)
   {
      *ptr = 0;
      if (CALL_STAT(previous, &st) != 0)
      {
         result = CreateFolder(previous);
         if (result)
         {
            result = (CALL_STAT(previous, &st) == 0);
         }
      }
      else
      {
         if (S_ISDIR(st.st_mode))
         {
            result = TRUE;
         }
      }
   }
   else
   {
      result = true;
      st.st_mode = 0700;
   }
   safe_free(previous);

   if (result)
   {
#ifdef _WIN32
      result = CreateDirectory(directory, NULL);
#else
      result = (_tmkdir(directory, st.st_mode) == 0);
#endif /* _WIN32 */
   }

   return result;
}

/**
 * Set last modification time to file
 */
bool SetLastModificationTime(TCHAR *fileName, time_t lastModDate)
{
   bool success = false;
#ifdef _WIN32
   struct _utimbuf ut;
#else
   struct utimbuf ut;
#endif // _WIN32
   ut.actime = lastModDate;
   ut.modtime = lastModDate;
   success = _tutime(fileName, &ut) == 0;
   return success;
}

/**
 * Get current time in milliseconds
 * Based on timeval.h by Wu Yongwei
 */
INT64 LIBNETXMS_EXPORTABLE GetCurrentTimeMs()
{
#ifdef _WIN32
   FILETIME ft;
   LARGE_INTEGER li;
   __int64 t;

   GetSystemTimeAsFileTime(&ft);
   li.LowPart  = ft.dwLowDateTime;
   li.HighPart = ft.dwHighDateTime;
   t = li.QuadPart;       // In 100-nanosecond intervals
   t -= EPOCHFILETIME;    // Offset to the Epoch time
   t /= 10000;            // Convert to milliseconds
#else
   struct timeval tv;
   INT64 t;

   gettimeofday(&tv, NULL);
   t = (INT64)tv.tv_sec * 1000 + (INT64)(tv.tv_usec / 1000);
#endif

   return t;
}

/**
 * Extract word from line (UNICODE version). Extracted word will be placed in buffer.
 * Returns pointer to the next word or to the null character if end
 * of line reached.
 */
const WCHAR LIBNETXMS_EXPORTABLE *ExtractWordW(const WCHAR *line, WCHAR *buffer)
{
   const WCHAR *ptr;
	WCHAR *bptr;

   for(ptr = line; (*ptr == L' ') || (*ptr == L'\t'); ptr++);  // Skip initial spaces
   // Copy word to buffer
   for(bptr = buffer; (*ptr != L' ') && (*ptr != L'\t') && (*ptr != 0); ptr++, bptr++)
      *bptr = *ptr;
   *bptr = 0;
   return ptr;
}

/**
 * Extract word from line (multibyte version). Extracted word will be placed in buffer.
 * Returns pointer to the next word or to the null character if end
 * of line reached.
 */
const char LIBNETXMS_EXPORTABLE *ExtractWordA(const char *line, char *buffer)
{
   const char *ptr;
	char *bptr;

   for(ptr = line; (*ptr == ' ') || (*ptr == '\t'); ptr++);  // Skip initial spaces
   // Copy word to buffer
   for(bptr = buffer; (*ptr != ' ') && (*ptr != '\t') && (*ptr != 0); ptr++, bptr++)
      *bptr = *ptr;
   *bptr = 0;
   return ptr;
}

#if defined(_WIN32)

/**
 * Get system error string by call to FormatMessage
 * (Windows only)
 */
TCHAR LIBNETXMS_EXPORTABLE *GetSystemErrorText(UINT32 dwError, TCHAR *pszBuffer, size_t iBufSize)
{
   TCHAR *msgBuf;

   if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                     FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                     NULL, dwError,
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
                     (LPTSTR)&msgBuf, 0, NULL) > 0)
   {
      msgBuf[_tcscspn(msgBuf, _T("\r\n"))] = 0;
      nx_strncpy(pszBuffer, msgBuf, iBufSize);
      LocalFree(msgBuf);
   }
   else
   {
      _sntprintf(pszBuffer, iBufSize, _T("MSG 0x%08X - Unable to find message text"), dwError);
   }
   return pszBuffer;
}

#endif

/**
 * Get last socket error as text
 */
TCHAR LIBNETXMS_EXPORTABLE *GetLastSocketErrorText(TCHAR *buffer, size_t size)
{
#ifdef _WIN32
   return GetSystemErrorText(WSAGetLastError(), buffer, size);
#else
   return nx_strncpy(buffer, _tcserror(errno), size);
#endif
}

#if (!HAVE_DAEMON || !HAVE_DECL_DAEMON) && !defined(_NETWARE) && !defined(_WIN32)

/**
 * daemon() implementation for systems which doesn't have one
 */
int LIBNETXMS_EXPORTABLE __daemon(int nochdir, int noclose)
{
   int pid;

   if ((pid = fork()) < 0)
      return -1;
   if (pid != 0)
      exit(0);                // Terminate parent

   setsid();

   if (!nochdir)
      chdir("/");

   if (!noclose)
   {
      fclose(stdin);          // don't need stdin, stdout, stderr
      fclose(stdout);
      fclose(stderr);
   }

   return 0;
}

#endif

/**
 * Check if given name is a valid object name
 */
BOOL LIBNETXMS_EXPORTABLE IsValidObjectName(const TCHAR *pszName, BOOL bExtendedChars)
{
   static TCHAR szValidCharacters[] = _T("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_- @()./");
   static TCHAR szInvalidCharacters[] = _T("\x01\x02\x03\x04\x05\x06\x07")
	                                     _T("\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F")
													 _T("\x10\x11\x12\x13\x14\x15\x16\x17")
	                                     _T("\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F")
	                                     _T("|\"'*%#\\`;?<>=");

   return (pszName[0] != 0) && (bExtendedChars ? (_tcscspn(pszName, szInvalidCharacters) == _tcslen(pszName)) : (_tcsspn(pszName, szValidCharacters) == _tcslen(pszName)));
}

/**
 * Check if given name is a valid script name
 */
BOOL LIBNETXMS_EXPORTABLE IsValidScriptName(const TCHAR *pszName)
{
   static TCHAR szValidCharacters[] = _T("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_:");
   return (pszName[0] != 0) && (!isdigit(pszName[0])) && (pszName[0] != ':') &&
          (_tcsspn(pszName, szValidCharacters) == _tcslen(pszName));
}

/**
 * Convert 6-byte MAC address to text representation
 */
TCHAR LIBNETXMS_EXPORTABLE *MACToStr(const BYTE *pData, TCHAR *pStr)
{
   UINT32 i;
   TCHAR *pCurr;

   for(i = 0, pCurr = pStr; i < 6; i++)
   {
      *pCurr++ = bin2hex(pData[i] >> 4);
      *pCurr++ = bin2hex(pData[i] & 15);
      *pCurr++ = _T(':');
   }
   *(pCurr - 1) = 0;
	return pStr;
}

/**
 * Convert byte array to text representation (wide character version)
 */
WCHAR LIBNETXMS_EXPORTABLE *BinToStrW(const void *data, size_t size, WCHAR *str)
{
   const BYTE *in = (const BYTE *)data;
   WCHAR *out = str;
   for(size_t i = 0; i < size; i++, in++)
   {
      *out++ = bin2hex(*in >> 4);
      *out++ = bin2hex(*in & 15);
   }
   *out = 0;
   return str;
}

/**
 * Convert byte array to text representation (multibyte character version)
 */
char LIBNETXMS_EXPORTABLE *BinToStrA(const void *data, size_t size, char *str)
{
   const BYTE *in = (const BYTE *)data;
   char *out = str;
   for(size_t i = 0; i < size; i++, in++)
   {
      *out++ = bin2hex(*in >> 4);
      *out++ = bin2hex(*in & 15);
   }
   *out = 0;
   return str;
}

/**
 * Convert string of hexadecimal digits to byte array (wide character version)
 * Returns number of bytes written to destination
 */
size_t LIBNETXMS_EXPORTABLE StrToBinW(const WCHAR *pStr, BYTE *pData, size_t size)
{
   size_t i;
   const WCHAR *pCurr;

   memset(pData, 0, size);
   for(i = 0, pCurr = pStr; (i < size) && (*pCurr != 0); i++)
   {
      pData[i] = hex2bin(*pCurr) << 4;
      pCurr++;
		if (*pCurr != 0)
		{
			pData[i] |= hex2bin(*pCurr);
			pCurr++;
		}
   }
   return i;
}

/**
 * Convert string of hexadecimal digits to byte array (multibyte character version)
 * Returns number of bytes written to destination
 */
size_t LIBNETXMS_EXPORTABLE StrToBinA(const char *pStr, BYTE *pData, size_t size)
{
   size_t i;
   const char *pCurr;

   memset(pData, 0, size);
   for(i = 0, pCurr = pStr; (i < size) && (*pCurr != 0); i++)
   {
      pData[i] = hex2bin(*pCurr) << 4;
      pCurr++;
		if (*pCurr != 0)
		{
			pData[i] |= hex2bin(*pCurr);
			pCurr++;
		}
   }
   return i;
}

/**
 * Translate string
 * NOTE: replacement string shouldn't be longer than original
 */
void LIBNETXMS_EXPORTABLE TranslateStr(TCHAR *pszString, const TCHAR *pszSubStr, const TCHAR *pszReplace)
{
   TCHAR *pszSrc, *pszDst;
   int iSrcLen, iRepLen;

   iSrcLen = (int)_tcslen(pszSubStr);
   iRepLen = (int)_tcslen(pszReplace);
   for(pszSrc = pszString, pszDst = pszString; *pszSrc != 0;)
   {
      if (!_tcsncmp(pszSrc, pszSubStr, iSrcLen))
      {
         memcpy(pszDst, pszReplace, sizeof(TCHAR) * iRepLen);
         pszSrc += iSrcLen;
         pszDst += iRepLen;
      }
      else
      {
         *pszDst++ = *pszSrc++;
      }
   }
   *pszDst = 0;
}

/**
 * Get size of file in bytes
 */
QWORD LIBNETXMS_EXPORTABLE FileSizeW(const WCHAR *pszFileName)
{
#ifdef _WIN32
   HANDLE hFind;
   WIN32_FIND_DATAW fd;
#else
   struct stat fileInfo;
#endif

#ifdef _WIN32
   hFind = FindFirstFileW(pszFileName, &fd);
   if (hFind == INVALID_HANDLE_VALUE)
      return 0;
   FindClose(hFind);

   return (unsigned __int64)fd.nFileSizeLow + ((unsigned __int64)fd.nFileSizeHigh << 32);
#else
   if (wstat(pszFileName, &fileInfo) == -1)
      return 0;

   return (QWORD)fileInfo.st_size;
#endif
}

/**
 * Get size of file in bytes
 */
QWORD LIBNETXMS_EXPORTABLE FileSizeA(const char *pszFileName)
{
#ifdef _WIN32
   HANDLE hFind;
   WIN32_FIND_DATAA fd;
#else
   struct stat fileInfo;
#endif

#ifdef _WIN32
   hFind = FindFirstFileA(pszFileName, &fd);
   if (hFind == INVALID_HANDLE_VALUE)
      return 0;
   FindClose(hFind);

   return (unsigned __int64)fd.nFileSizeLow + ((unsigned __int64)fd.nFileSizeHigh << 32);
#else
   if (stat(pszFileName, &fileInfo) == -1)
      return 0;

   return (QWORD)fileInfo.st_size;
#endif
}

/**
 * Get pointer to clean file name (without path specification)
 */
const TCHAR LIBNETXMS_EXPORTABLE *GetCleanFileName(const TCHAR *pszFileName)
{
   const TCHAR *ptr;

   ptr = pszFileName + _tcslen(pszFileName);
   while((ptr >= pszFileName) && (*ptr != _T('/')) && (*ptr != _T('\\')) && (*ptr != _T(':')))
      ptr--;
   return (ptr + 1);
}

/**
 * Translate DCI data type from text form to code
 */
int LIBNETXMS_EXPORTABLE NxDCIDataTypeFromText(const TCHAR *pszText)
{
   static const TCHAR *m_pszValidTypes[] = { _T("INT"), _T("UINT"), _T("INT64"),
                                             _T("UINT64"), _T("STRING"),
                                             _T("FLOAT"), NULL };
   int i;

   for(i = 0; m_pszValidTypes[i] != NULL; i++)
      if (!_tcsicmp(pszText, m_pszValidTypes[i]))
         return i;
   return -1;     // Invalid data type
}

/**
 * Extended send() - send all data even if single call to send()
 * cannot handle them all
 */
int LIBNETXMS_EXPORTABLE SendEx(SOCKET hSocket, const void *data, size_t len, int flags, MUTEX mutex)
{
	int nLeft = (int)len;
	int nRet;

	if (mutex != INVALID_MUTEX_HANDLE)
		MutexLock(mutex);

	do
	{
retry:
#ifdef MSG_NOSIGNAL
		nRet = send(hSocket, ((char *)data) + (len - nLeft), nLeft, flags | MSG_NOSIGNAL);
#else
		nRet = send(hSocket, ((char *)data) + (len - nLeft), nLeft, flags);
#endif
		if (nRet <= 0)
		{
			if ((WSAGetLastError() == WSAEWOULDBLOCK)
#ifndef _WIN32
			    || (errno == EAGAIN)
#endif
			   )
			{
				// Wait until socket becomes available for writing
			   SocketPoller p(true);
			   p.add(hSocket);
				nRet = p.poll(60000);
#ifdef _WIN32
				if (nRet > 0)
#else
				if ((nRet > 0) || ((nRet == -1) && (errno == EINTR)))
#endif
					goto retry;
			}
			break;
		}
		nLeft -= nRet;
	} while (nLeft > 0);

	if (mutex != INVALID_MUTEX_HANDLE)
		MutexUnlock(mutex);

	return nLeft == 0 ? (int)len : nRet;
}

/**
 * Extended recv() - receive data with timeout
 *
 * @param hSocket socket handle
 * @param data data buffer
 * @param len buffer length in bytes
 * @param flags flags to be passed to recv() call
 * @param timeout waiting timeout in milliseconds
 * @return number of bytes read on success, 0 if socket was closed, -1 on error, -2 on timeout
 */
int LIBNETXMS_EXPORTABLE RecvEx(SOCKET hSocket, void *data, size_t len, int flags, UINT32 timeout)
{
	if (hSocket == INVALID_SOCKET)
		return -1;

	int rc;
   if (timeout != INFINITE)
   {
      SocketPoller sp;
      sp.add(hSocket);
      rc = sp.poll(timeout);
      if (rc > 0)
      {
#ifdef _WIN32
         rc = recv(hSocket, (char *)data, (int)len, flags);
#else
         do
         {
            rc = recv(hSocket, (char *)data, len, flags);
         } while((rc == -1) && (errno == EINTR));
#endif
      }
      else
      {
         rc = -2;
      }
   }
   else
   {
#ifdef _WIN32
      rc = recv(hSocket, (char *)data, (int)len, flags);
#else
      do
      {
         rc = recv(hSocket, (char *)data, (int)len, flags);
      } while((rc == -1) && (errno == EINTR));
#endif
   }
   return rc;
}

/**
 * Read exact number of bytes from socket
 */
bool RecvAll(SOCKET s, void *buffer, size_t size, UINT32 timeout)
{
   size_t bytes = 0;
   char *pos = (char *)buffer;
   while(bytes < size)
   {
      int b = RecvEx(s, pos, size - bytes, 0, timeout);
      if (b <= 0)
         return false;
      bytes += b;
      pos += b;
   }
   return true;
}

/**
 * Connect with given timeout
 * Sets socket to non-blocking mode
 */
int LIBNETXMS_EXPORTABLE ConnectEx(SOCKET s, struct sockaddr *addr, int len, UINT32 timeout)
{
	SetSocketNonBlocking(s);

	int rc = connect(s, addr, len);
	if (rc == -1)
	{
		if ((WSAGetLastError() == WSAEWOULDBLOCK) || (WSAGetLastError() == WSAEINPROGRESS))
		{
#if HAVE_POLL
		   struct pollfd fds;
			fds.fd = s;
			fds.events = POLLOUT;
			fds.revents = POLLOUT;
			do
			{
				INT64 startTime = GetCurrentTimeMs();
				rc = poll(&fds, 1, timeout);
				if ((rc != -1) || (errno != EINTR))
					break;
				UINT32 elapsed = (UINT32)(GetCurrentTimeMs() - startTime);
				timeout -= MIN(timeout, elapsed);
			} while(timeout > 0);

			if (rc > 0)
			{
            if (fds.revents == POLLOUT)
            {
               rc = 0;
            }
            else
            {
               rc = -1;
            }
			}
			else if (rc == 0)	// timeout, return error
			{
				rc = -1;
			}
#else
			struct timeval tv;
			fd_set wrfs, exfs;

			FD_ZERO(&wrfs);
			FD_SET(s, &wrfs);

			FD_ZERO(&exfs);
			FD_SET(s, &exfs);

#ifdef _WIN32
			tv.tv_sec = timeout / 1000;
			tv.tv_usec = (timeout % 1000) * 1000;
			rc = select(SELECT_NFDS(s + 1), NULL, &wrfs, &exfs, &tv);
#else
			do
			{
				tv.tv_sec = timeout / 1000;
				tv.tv_usec = (timeout % 1000) * 1000;
				INT64 startTime = GetCurrentTimeMs();
				rc = select(SELECT_NFDS(s + 1), NULL, &wrfs, &exfs, &tv);
				if ((rc != -1) || (errno != EINTR))
					break;
				UINT32 elapsed = (UINT32)(GetCurrentTimeMs() - startTime);
				timeout -= MIN(timeout, elapsed);
			} while(timeout > 0);
#endif
			if (rc > 0)
			{
				if (FD_ISSET(s, &exfs))
				{
#ifdef _WIN32
					int err, len = sizeof(int);
					if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0)
						WSASetLastError(err);
#endif
					rc = -1;
				}
				else
				{
					rc = 0;
				}
			}
			else if (rc == 0)	// timeout, return error
			{
				rc = -1;
#ifdef _WIN32
				WSASetLastError(WSAETIMEDOUT);
#endif
			}
#endif
		}
	}
	return rc;
}

/**
 * Connect to given host/port
 *
 * @return connected socket on success or INVALID_SOCKET on error
 */
SOCKET LIBNETXMS_EXPORTABLE ConnectToHost(const InetAddress& addr, UINT16 port, UINT32 timeout)
{
   SOCKET s = socket(addr.getFamily(), SOCK_STREAM, 0);
   if (s == INVALID_SOCKET)
      return INVALID_SOCKET;

   SockAddrBuffer saBuffer;
   struct sockaddr *sa = addr.fillSockAddr(&saBuffer, port);
   if (ConnectEx(s, sa, SA_LEN(sa), timeout) == -1)
   {
      closesocket(s);
      s = INVALID_SOCKET;
   }
   return s;
}

#ifndef VER_PLATFORM_WIN32_WINDOWS
#define VER_PLATFORM_WIN32_WINDOWS 1
#endif
#ifndef VER_PLATFORM_WIN32_CE
#define VER_PLATFORM_WIN32_CE 3
#endif
#ifndef SM_SERVERR2
#define SM_SERVERR2             89
#endif

/**
 * Get OS name and version
 */
void LIBNETXMS_EXPORTABLE GetOSVersionString(TCHAR *pszBuffer, int nBufSize)
{
   int nSize = nBufSize - 1;

   memset(pszBuffer, 0, nBufSize * sizeof(TCHAR));

#if defined(_WIN32)
   OSVERSIONINFO ver;

   ver.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
   GetVersionEx(&ver);
   switch(ver.dwPlatformId)
   {
      case VER_PLATFORM_WIN32_WINDOWS:
         _sntprintf(pszBuffer, nSize, _T("Win%s"), ver.dwMinorVersion == 0 ? _T("95") :
                    (ver.dwMinorVersion == 10 ? _T("98") :
                    (ver.dwMinorVersion == 90 ? _T("Me") : _T("9x"))));
         break;
      case VER_PLATFORM_WIN32_NT:
         _sntprintf(pszBuffer, nSize, _T("WinNT %d.%d"), ver.dwMajorVersion, ver.dwMinorVersion);
         break;
#ifdef VER_PLATFORM_WIN32_CE
      case VER_PLATFORM_WIN32_CE:
         _sntprintf(pszBuffer, nSize, _T("WinCE %d.%d"), ver.dwMajorVersion, ver.dwMinorVersion);
         break;
#endif
      default:
         _sntprintf(pszBuffer, nSize, _T("WinX %d.%d"), ver.dwMajorVersion, ver.dwMinorVersion);
         break;
   }
#elif defined(_NETWARE)
   struct utsname un;

   uname(&un);
   _sntprintf(pszBuffer, nSize, _T("NetWare %d.%d"), un.netware_major, un.netware_minor);
   if (un.servicepack > 0)
   {
      int nLen = (int)_tcslen(pszBuffer);
      nSize -= nLen;
      if (nSize > 0)
         _sntprintf(&pszBuffer[nLen], nSize, _T(" sp%d"), un.servicepack);
   }
#else
   struct utsname un;

   uname(&un);
#ifdef UNICODE
   char buf[1024];
   snprintf(buf, 1024, "%s %s", un.sysname, un.release);
   MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, buf, -1, pszBuffer, nSize);
#else
   snprintf(pszBuffer, nSize, "%s %s", un.sysname, un.release);
#endif
#endif
}

#ifdef _WIN32

/**
 * Get more specific Windows version string
 */
BOOL LIBNETXMS_EXPORTABLE GetWindowsVersionString(TCHAR *versionString, int strSize)
{
	OSVERSIONINFOEX ver;
	TCHAR buffer[256];

	ver.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	if (!GetVersionEx((OSVERSIONINFO *)&ver))
		return FALSE;

	switch(ver.dwMajorVersion)
	{
		case 5:
			switch(ver.dwMinorVersion)
			{
				case 0:
					_tcscpy(buffer, _T("2000"));
					break;
				case 1:
					_tcscpy(buffer, _T("XP"));
					break;
				case 2:
					_tcscpy(buffer, (GetSystemMetrics(SM_SERVERR2) != 0) ? _T("Server 2003 R2") : _T("Server 2003"));
					break;
				default:
					_sntprintf(buffer, 256, _T("NT %d.%d"), ver.dwMajorVersion, ver.dwMinorVersion);
					break;
			}
			break;
		case 6:
			switch(ver.dwMinorVersion)
			{
				case 0:
					_tcscpy(buffer, (ver.wProductType == VER_NT_WORKSTATION) ? _T("Vista") : _T("Server 2008"));
					break;
				case 1:
					_tcscpy(buffer, (ver.wProductType == VER_NT_WORKSTATION) ? _T("7") : _T("Server 2008 R2"));
					break;
				case 2:
					_tcscpy(buffer, (ver.wProductType == VER_NT_WORKSTATION) ? _T("8") : _T("Server 2012"));
					break;
				case 3:
					_tcscpy(buffer, (ver.wProductType == VER_NT_WORKSTATION) ? _T("8.1") : _T("Server 2012 R2"));
					break;
				default:
					_sntprintf(buffer, 256, _T("NT %d.%d"), ver.dwMajorVersion, ver.dwMinorVersion);
					break;
			}
			break;
		case 10:
			switch(ver.dwMinorVersion)
			{
				case 0:
					_tcscpy(buffer, (ver.wProductType == VER_NT_WORKSTATION) ? _T("10") : _T("Server"));
					break;
				default:
					_sntprintf(buffer, 256, _T("NT %d.%d"), ver.dwMajorVersion, ver.dwMinorVersion);
					break;
			}
			break;
		default:
			_sntprintf(buffer, 256, _T("NT %d.%d"), ver.dwMajorVersion, ver.dwMinorVersion);
			break;
	}

	_sntprintf(versionString, strSize, _T("Windows %s Build %d %s"), buffer, ver.dwBuildNumber, ver.szCSDVersion);
	StrStrip(versionString);
	return TRUE;
}

#endif


//
// Count number of characters in string
//

int LIBNETXMS_EXPORTABLE NumCharsW(const WCHAR *pszStr, WCHAR ch)
{
   const WCHAR *p;
   int nCount;

   for(p = pszStr, nCount = 0; *p != 0; p++)
      if (*p == ch)
         nCount++;
   return nCount;
}

int LIBNETXMS_EXPORTABLE NumCharsA(const char *pszStr, char ch)
{
   const char *p;
   int nCount;

   for(p = pszStr, nCount = 0; *p != 0; p++)
      if (*p == ch)
         nCount++;
   return nCount;
}

/**
 * Match string against regexp (UNICODE version)
 */
bool LIBNETXMS_EXPORTABLE RegexpMatchW(const WCHAR *str, const WCHAR *expr, bool matchCase)
{
   regex_t preg;
   bool result = false;

	if (tre_regwcomp(&preg, expr, matchCase ? REG_EXTENDED | REG_NOSUB : REG_EXTENDED | REG_NOSUB | REG_ICASE) == 0)
	{
		if (tre_regwexec(&preg, str, 0, NULL, 0) == 0) // MATCH
			result = true;
		regfree(&preg);
	}

   return result;
}

/**
 * Match string against regexp (multibyte version)
 */
bool LIBNETXMS_EXPORTABLE RegexpMatchA(const char *str, const char *expr, bool matchCase)
{
   regex_t preg;
   bool result = false;

	if (tre_regcomp(&preg, expr, matchCase ? REG_EXTENDED | REG_NOSUB : REG_EXTENDED | REG_NOSUB | REG_ICASE) == 0)
	{
		if (tre_regexec(&preg, str, 0, NULL, 0) == 0) // MATCH
			result = true;
		regfree(&preg);
	}

   return result;
}

/**
 * Translate given code to text
 */
const TCHAR LIBNETXMS_EXPORTABLE *CodeToText(int iCode, CODE_TO_TEXT *pTranslator, const TCHAR *pszDefaultText)
{
   int i;

   for(i = 0; pTranslator[i].text != NULL; i++)
      if (pTranslator[i].code == iCode)
         return pTranslator[i].text;
   return pszDefaultText;
}

/**
 * Extract option value from string of form option=value;option=value;... (UNICODE version)
 */
bool LIBNETXMS_EXPORTABLE ExtractNamedOptionValueW(const WCHAR *optString, const WCHAR *option, WCHAR *buffer, int bufSize)
{
	int state, pos;
	const WCHAR *curr, *start;
	WCHAR temp[256];

	for(curr = start = optString, pos = 0, state = 0; *curr != 0; curr++)
	{
		switch(*curr)
		{
			case L';':		// Next option
				if (state == 1)
				{
					buffer[pos] = 0;
					StrStripW(buffer);
					return true;
				}
				state = 0;
				start = curr + 1;
				break;
			case L'=':
				if (state == 0)
				{
					wcsncpy(temp, start, curr - start);
					temp[curr - start] = 0;
					StrStripW(temp);
					if (!wcsicmp(option, temp))
						state = 1;
					else
						state = 2;
				}
				else if ((state == 1) && (pos < bufSize - 1))
				{
					buffer[pos++] = L'=';
				}
				break;
			default:
				if ((state == 1) && (pos < bufSize - 1))
					buffer[pos++] = *curr;
				break;
		}
	}

	if (state == 1)
	{
		buffer[pos] = 0;
		StrStripW(buffer);
		return true;
	}

	return false;
}

/**
 * Extract option value from string of form option=value;option=value;... (multibyte version)
 */
bool LIBNETXMS_EXPORTABLE ExtractNamedOptionValueA(const char *optString, const char *option, char *buffer, int bufSize)
{
	int state, pos;
	const char *curr, *start;
	char temp[256];

	for(curr = start = optString, pos = 0, state = 0; *curr != 0; curr++)
	{
		switch(*curr)
		{
			case ';':		// Next option
				if (state == 1)
				{
					buffer[pos] = 0;
					StrStripA(buffer);
					return true;
				}
				state = 0;
				start = curr + 1;
				break;
			case '=':
				if (state == 0)
				{
					strncpy(temp, start, curr - start);
					temp[curr - start] = 0;
					StrStripA(temp);
					if (!stricmp(option, temp))
						state = 1;
					else
						state = 2;
				}
				else if ((state == 1) && (pos < bufSize - 1))
				{
					buffer[pos++] = '=';
				}
				break;
			default:
				if ((state == 1) && (pos < bufSize - 1))
					buffer[pos++] = *curr;
				break;
		}
	}

	if (state == 1)
	{
		buffer[pos] = 0;
		StrStripA(buffer);
		return true;
	}

	return false;
}

/**
 * Extract named option value as boolean (UNICODE version)
 */
bool LIBNETXMS_EXPORTABLE ExtractNamedOptionValueAsBoolW(const WCHAR *optString, const WCHAR *option, bool defVal)
{
	WCHAR buffer[256];
	if (ExtractNamedOptionValueW(optString, option, buffer, 256))
		return !wcsicmp(buffer, L"yes") || !wcsicmp(buffer, L"true");
	return defVal;
}

/**
 * Extract named option value as boolean (multibyte version)
 */
bool LIBNETXMS_EXPORTABLE ExtractNamedOptionValueAsBoolA(const char *optString, const char *option, bool defVal)
{
	char buffer[256];
	if (ExtractNamedOptionValueA(optString, option, buffer, 256))
		return !stricmp(buffer, "yes") || !stricmp(buffer, "true");
	return defVal;
}

/**
 * Extract named option value as integer (UNICODE version)
 */
long LIBNETXMS_EXPORTABLE ExtractNamedOptionValueAsIntW(const WCHAR *optString, const WCHAR *option, long defVal)
{
	WCHAR buffer[256], *eptr;
	long val;

	if (ExtractNamedOptionValueW(optString, option, buffer, 256))
	{
		val = wcstol(buffer, &eptr, 0);
		if (*eptr == 0)
			return val;
	}
	return defVal;
}

/**
 * Extract named option value as integer (multibyte version)
 */
long LIBNETXMS_EXPORTABLE ExtractNamedOptionValueAsIntA(const char *optString, const char *option, long defVal)
{
	char buffer[256], *eptr;
	long val;

	if (ExtractNamedOptionValueA(optString, option, buffer, 256))
	{
		val = strtol(buffer, &eptr, 0);
		if (*eptr == 0)
			return val;
	}
	return defVal;
}

/**
 * Split string
 */
TCHAR LIBNETXMS_EXPORTABLE **SplitString(const TCHAR *source, TCHAR sep, int *numStrings)
{
	TCHAR **strings;

	*numStrings = NumChars(source, sep) + 1;
	strings = (TCHAR **)malloc(sizeof(TCHAR *) * (*numStrings));
	for(int n = 0, i = 0; n < *numStrings; n++, i++)
	{
		int start = i;
		while((source[i] != sep) && (source[i] != 0))
			i++;
		int len = i - start;
		strings[n] = (TCHAR *)malloc(sizeof(TCHAR) * (len + 1));
		memcpy(strings[n], &source[start], len * sizeof(TCHAR));
		strings[n][len] = 0;
	}
	return strings;
}

/**
 * Get step size for "%" and "/" crontab cases
 */
static int GetStepSize(TCHAR *str)
{
  int step = 0;
  if (str != NULL)
  {
    *str = 0;
    str++;
    step = *str == _T('\0') ? 1 : _tcstol(str, NULL, 10);
  }

  if (step <= 0)
  {
    step = 1;
  }

  return step;
}

/**
 * Get last day of current month
 */
int LIBNETXMS_EXPORTABLE GetLastMonthDay(struct tm *currTime)
{
   switch(currTime->tm_mon)
   {
      case 1:  // February
         if (((currTime->tm_year % 4) == 0) && (((currTime->tm_year % 100) != 0) || (((currTime->tm_year + 1900) % 400) == 0)))
            return 29;
         return 28;
      case 0:  // January
      case 2:  // March
      case 4:  // May
      case 6:  // July
      case 7:  // August
      case 9:  // October
      case 11: // December
         return 31;
      default:
         return 30;
   }
}

/**
 * Match schedule element
 * NOTE: We assume that pattern can be modified during processing
 */
bool LIBNETXMS_EXPORTABLE MatchScheduleElement(TCHAR *pszPattern, int nValue, int maxValue, struct tm *localTime, time_t currTime)
{
   TCHAR *ptr, *curr;
   int nStep, nCurr, nPrev;
   bool bRun = true, bRange = false;

   // Check for "last" pattern
   if (*pszPattern == _T('L'))
      return nValue == maxValue;

	// Check if time() step was specified (% - special syntax)
	ptr = _tcschr(pszPattern, _T('%'));
	if (ptr != NULL)
		return (currTime % GetStepSize(ptr)) != 0;

   // Check if step was specified
   ptr = _tcschr(pszPattern, _T('/'));
   nStep = GetStepSize(ptr);

   if (*pszPattern == _T('*'))
      goto check_step;

   for(curr = pszPattern; bRun; curr = ptr + 1)
   {
      for(ptr = curr; (*ptr != 0) && (*ptr != '-') && (*ptr != ','); ptr++);
      switch(*ptr)
      {
         case '-':
            if (bRange)
               return false;  // Form like 1-2-3 is invalid
            bRange = true;
            *ptr = 0;
            nPrev = _tcstol(curr, NULL, 10);
            break;
         case 'L':  // special case for last day ow week in a month (like 5L - last Friday)
            if (bRange || (localTime == NULL))
               return false;  // Range with L is not supported; nL form supported only for day of week
            *ptr = 0;
            nCurr = _tcstol(curr, NULL, 10);
            if ((nValue == nCurr) && (localTime->tm_mday + 7 > GetLastMonthDay(localTime)))
               return true;
            ptr++;
            if (*ptr != ',')
               bRun = false;
            break;
         case 0:
            bRun = false;
            /* no break */
         case ',':
            *ptr = 0;
            nCurr = _tcstol(curr, NULL, 10);
            if (bRange)
            {
               if ((nValue >= nPrev) && (nValue <= nCurr))
                  goto check_step;
               bRange = false;
            }
            else
            {
               if (nValue == nCurr)
                  return true;
            }
            break;
      }
   }

   return false;

check_step:
   return (nValue % nStep) == 0;
}

/**
 * Failure handler for DecryptPasswordW
 */
inline bool DecryptPasswordFailW(const WCHAR *encryptedPasswd, WCHAR *decryptedPasswd, size_t bufferLenght)
{
   if (decryptedPasswd != encryptedPasswd)
      wcsncpy(decryptedPasswd, encryptedPasswd, bufferLenght);
   return false;
}

/**
 * Decrypt password encrypted with nxencpassw.
 * In case when it was not possible to decrypt password as the decrypted password will be set the original one.
 * The buffer length for encryptedPasswd and decryptedPasswd should be the same.
 */
bool LIBNETXMS_EXPORTABLE DecryptPasswordW(const WCHAR *login, const WCHAR *encryptedPasswd, WCHAR *decryptedPasswd, size_t bufferLenght)
{
   //check that lenght is correct
	if (wcslen(encryptedPasswd) != 44)
      return DecryptPasswordFailW(encryptedPasswd, decryptedPasswd, bufferLenght);

   // check that password contains only allowed symbols
   int invalidSymbolIndex = (int)wcsspn(encryptedPasswd, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
   if ((invalidSymbolIndex < 42) || ((invalidSymbolIndex != 44) && ((encryptedPasswd[invalidSymbolIndex] != L'=') || ((invalidSymbolIndex == 42) && (encryptedPasswd[43] != L'=')))))
      return DecryptPasswordFailW(encryptedPasswd, decryptedPasswd, bufferLenght);

	char *mbencrypted = MBStringFromWideString(encryptedPasswd);
	char *mblogin = MBStringFromWideString(login);

	BYTE encrypted[32], decrypted[32], key[16];
	size_t encSize = 32;
	base64_decode(mbencrypted, strlen(mbencrypted), (char *)encrypted, &encSize);
	if (encSize != 32)
      return DecryptPasswordFailW(encryptedPasswd, decryptedPasswd, bufferLenght);

	CalculateMD5Hash((BYTE *)mblogin, strlen(mblogin), key);
	ICEDecryptData(encrypted, 32, decrypted, key);
	decrypted[31] = 0;

	MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, (char *)decrypted, -1, decryptedPasswd, (int)bufferLenght);
	decryptedPasswd[bufferLenght - 1] = 0;
	free(mbencrypted);
	free(mblogin);

	return true;
}

/**
 * Failure handler for DecryptPasswordA
 */
inline bool DecryptPasswordFailA(const char *encryptedPasswd, char *decryptedPasswd, size_t bufferLenght)
{
   if (decryptedPasswd != encryptedPasswd)
      strncpy(decryptedPasswd, encryptedPasswd, bufferLenght);
   return false;
}

/**
 * Decrypt password encrypted with nxencpassw.
 * In case when it was not possible to decrypt password as the decrypted password will be set the original one.
 * The buffer length for encryptedPasswd and decryptedPasswd should be the same.
 */
bool LIBNETXMS_EXPORTABLE DecryptPasswordA(const char *login, const char *encryptedPasswd, char *decryptedPasswd, size_t bufferLenght)
{
   //check that lenght is correct
   if (strlen(encryptedPasswd) != 44)
      return DecryptPasswordFailA(encryptedPasswd, decryptedPasswd, bufferLenght);

   // check that password contains only allowed symbols
   int invalidSymbolIndex = (int)strspn(encryptedPasswd, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
   if ((invalidSymbolIndex < 42) || ((invalidSymbolIndex != 44) && ((encryptedPasswd[invalidSymbolIndex] != '=') || ((invalidSymbolIndex == 42) && (encryptedPasswd[43] != '=')))))
      return DecryptPasswordFailA(encryptedPasswd, decryptedPasswd, bufferLenght);

   BYTE encrypted[32], decrypted[32], key[16];
   size_t encSize = 32;
   base64_decode(encryptedPasswd, strlen(encryptedPasswd), (char *)encrypted, &encSize);
   if (encSize != 32)
      return DecryptPasswordFailA(encryptedPasswd, decryptedPasswd, bufferLenght);

   CalculateMD5Hash((BYTE *)login, strlen(login), key);
   ICEDecryptData(encrypted, 32, decrypted, key);
   decrypted[31] = 0;

   strncpy(decryptedPasswd, (char *)decrypted, bufferLenght);
   return true;
}

#ifndef UNDER_CE

/**
 * Load file content into memory
 */
static BYTE *LoadFileContent(int fd, UINT32 *pdwFileSize)
{
   int iBufPos, iNumBytes, iBytesRead;
   BYTE *pBuffer = NULL;
   NX_STAT_STRUCT fs;

   if (NX_FSTAT(fd, &fs) != -1)
   {
      pBuffer = (BYTE *)malloc((size_t)fs.st_size + 1);
      if (pBuffer != NULL)
      {
         *pdwFileSize = (UINT32)fs.st_size;
         for(iBufPos = 0; iBufPos < fs.st_size; iBufPos += iBytesRead)
         {
            iNumBytes = MIN(16384, (int)fs.st_size - iBufPos);
            if ((iBytesRead = _read(fd, &pBuffer[iBufPos], iNumBytes)) < 0)
            {
               free(pBuffer);
               pBuffer = NULL;
               break;
            }
         }
         if (pBuffer != NULL)
            pBuffer[fs.st_size] = 0;
      }
   }
   _close(fd);
   return pBuffer;
}

BYTE LIBNETXMS_EXPORTABLE *LoadFile(const TCHAR *pszFileName, UINT32 *pdwFileSize)
{
   int fd;
   BYTE *pBuffer = NULL;

   fd = _topen(pszFileName, O_RDONLY | O_BINARY);
   if (fd != -1)
   {
		pBuffer = LoadFileContent(fd, pdwFileSize);
   }
   return pBuffer;
}

#ifdef UNICODE

BYTE LIBNETXMS_EXPORTABLE *LoadFileA(const char *pszFileName, UINT32 *pdwFileSize)
{
   int fd;
   BYTE *pBuffer = NULL;

#ifdef _WIN32
   fd = _open(pszFileName, O_RDONLY | O_BINARY);
#else
   fd = open(pszFileName, O_RDONLY | O_BINARY);
#endif
   if (fd != -1)
   {
      pBuffer = LoadFileContent(fd, pdwFileSize);
   }
   return pBuffer;
}

#endif

#endif

#ifdef _WIN32

/**
 * Get memory consumed by current process
 */
INT64 LIBNETXMS_EXPORTABLE GetProcessRSS()
{
	PROCESS_MEMORY_COUNTERS pmc;

	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		return pmc.WorkingSetSize;
	return 0;
}

#define BG_MASK 0xF0
#define FG_MASK 0x0F

/**
 * Apply terminal attributes to console - Win32 API specific
 */
static WORD ApplyTerminalAttribute(HANDLE out, WORD currAttr, long code)
{
	WORD attr = currAttr;
	switch(code)
	{
		case 0:	// reset attribute
			attr = 0x07;
			break;
		case 1:	// bold/bright
			attr = currAttr | FOREGROUND_INTENSITY;
			break;
		case 22:	// normal color
			attr = currAttr & ~FOREGROUND_INTENSITY;
			break;
		case 30:	// black foreground
			attr = (currAttr & BG_MASK);
			break;
		case 31:	// red foreground
			attr = (currAttr & BG_MASK) | 0x04;
			break;
		case 32:	// green foreground
			attr = (currAttr & BG_MASK) | 0x02;
			break;
		case 33:	// yellow foreground
			attr = (currAttr & BG_MASK) | 0x06;
			break;
		case 34:	// blue foreground
			attr = (currAttr & BG_MASK) | 0x01;
			break;
		case 35:	// magenta foreground
			attr = (currAttr & BG_MASK) | 0x05;
			break;
		case 36:	// cyan foreground
			attr = (currAttr & BG_MASK) | 0x03;
			break;
		case 37:	// white (gray) foreground
			attr = (currAttr & BG_MASK) | 0x07;
			break;
		case 40:	// black background
			attr = (currAttr & FG_MASK);
			break;
		case 41:	// red background
			attr = (currAttr & FG_MASK) | 0x40;
			break;
		case 42:	// green background
			attr = (currAttr & FG_MASK) | 0x20;
			break;
		case 43:	// yellow background
			attr = (currAttr & FG_MASK) | 0x60;
			break;
		case 44:	// blue background
			attr = (currAttr & FG_MASK) | 0x10;
			break;
		case 45:	// magenta background
			attr = (currAttr & FG_MASK) | 0x50;
			break;
		case 46:	// cyan background
			attr = (currAttr & FG_MASK) | 0x30;
			break;
		case 47:	// white (gray) background
			attr = (currAttr & FG_MASK) | 0x70;
			break;
		default:
			break;
	}
	SetConsoleTextAttribute(out, attr);
	return attr;
}

#endif

/**
 * Write to terminal with support for ANSI color codes
 */
void LIBNETXMS_EXPORTABLE WriteToTerminal(const TCHAR *text)
{
#ifdef _WIN32
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

   DWORD mode;
   if (!GetConsoleMode(out, &mode))
   {
      // Assume output is redirected
#ifdef UNICODE
      char *mbText = MBStringFromWideString(text);
      WriteFile(out, mbText, (UINT32)strlen(mbText), &mode, NULL);
      free(mbText);
#else
      WriteFile(out, text, (UINT32)strlen(text), &mode, NULL);
#endif
      return;
   }

	CONSOLE_SCREEN_BUFFER_INFO csbi;
	GetConsoleScreenBufferInfo(out, &csbi);

	const TCHAR *curr = text;
	while(*curr != 0)
	{
		const TCHAR *esc = _tcschr(curr, 27);	// Find ESC
		if (esc != NULL)
		{
			esc++;
			if (*esc == _T('['))
			{
				// write everything up to ESC char
				DWORD chars;
				WriteConsole(out, curr, (UINT32)(esc - curr - 1), &chars, NULL);

				esc++;

				TCHAR code[64];
				int pos = 0;
				while((*esc != 0) && (*esc != _T('m')))
				{
					if (*esc == _T(';'))
					{
						code[pos] = 0;
						csbi.wAttributes = ApplyTerminalAttribute(out, csbi.wAttributes, _tcstol(code, NULL, 10));
						pos = 0;
					}
					else
					{
						if (pos < 63)
							code[pos++] = *esc;
					}
					esc++;
				}
				if (pos > 0)
				{
					code[pos] = 0;
					csbi.wAttributes = ApplyTerminalAttribute(out, csbi.wAttributes, _tcstol(code, NULL, 10));
				}
				esc++;
			}
			else
			{
				DWORD chars;
				WriteConsole(out, curr, (UINT32)(esc - curr), &chars, NULL);
			}
			curr = esc;
		}
		else
		{
			DWORD chars;
			WriteConsole(out, curr, (UINT32)_tcslen(curr), &chars, NULL);
			break;
		}
	}
#else
#ifdef UNICODE
#if HAVE_FPUTWS
	fputws(text, stdout);
#else
	char *mbtext = MBStringFromWideStringSysLocale(text);
	fputs(mbtext, stdout);
	free(mbtext);
#endif
#else
	fputs(text, stdout);
#endif
#endif
}

/**
 * Write to terminal with support for ANSI color codes
 */
void LIBNETXMS_EXPORTABLE WriteToTerminalEx(const TCHAR *format, ...)
{
	TCHAR buffer[8192];
	va_list args;

	va_start(args, format);
	_vsntprintf(buffer, 8192, format, args);
	va_end(args);
	WriteToTerminal(buffer);
}

/**
 * mkstemp() implementation for Windows
 */
#ifdef _WIN32

int LIBNETXMS_EXPORTABLE mkstemp(char *tmpl)
{
	char *name = _mktemp(tmpl);
	if (name == NULL)
		return -1;
	return _open(name, O_RDWR | O_BINARY | O_CREAT | O_EXCL| _O_SHORT_LIVED, _S_IREAD | _S_IWRITE);
}

int LIBNETXMS_EXPORTABLE wmkstemp(WCHAR *tmpl)
{
	WCHAR *name = _wmktemp(tmpl);
	if (name == NULL)
		return -1;
	return _wopen(name, O_RDWR | O_BINARY | O_CREAT | O_EXCL| _O_SHORT_LIVED, _S_IREAD | _S_IWRITE);
}

#endif

#ifndef _WIN32

/**
 * strcat_s implementation
 */
int LIBNETXMS_EXPORTABLE strcat_s(char *dst, size_t dstSize, const char *src)
{
	if (strlen(dst) + strlen(src) + 1 >= dstSize)
		return EINVAL;
	strcat(dst, src);
	return 0;
}

/**
 * wcscat_s implementation
 */
int LIBNETXMS_EXPORTABLE wcscat_s(WCHAR *dst, size_t dstSize, const WCHAR *src)
{
	if (wcslen(dst) + wcslen(src) + 1 >= dstSize)
		return EINVAL;
	wcscat(dst, src);
	return 0;
}

#endif

/**
 * Destructor for RefCountObject
 */
RefCountObject::~RefCountObject()
{
}

/**
 * Safe _fgetts implementation which will work
 * with handles opened by popen
 */
TCHAR LIBNETXMS_EXPORTABLE *safe_fgetts(TCHAR *buffer, int len, FILE *f)
{
#ifdef UNICODE
#if SAFE_FGETWS_WITH_POPEN
	return fgetws(buffer, len, f);
#else
	char *mbBuffer = (char *)alloca(len);
	char *s = fgets(mbBuffer, len, f);
	if (s == NULL)
		return NULL;
	mbBuffer[len - 1] = 0;
	MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, mbBuffer, -1, buffer, len);
	return buffer;
#endif
#else
	return fgets(buffer, len, f);
#endif
}

#if !HAVE_STRLWR && !defined(_WIN32)

/**
 * Convert UNICODE string to lowercase
 */
char LIBNETXMS_EXPORTABLE *strlwr(char *str)
{
   for(char *p = str; *p != 0; p++)
   {
#if HAVE_TOLOWER
      *p = tolower(*p);
#else
      if ((*p >= 'a') && (*p <= 'z'))
         *p = *p - ('a' - 'A');
#endif
   }
   return str;
}

#endif

#if !HAVE_WCSLWR && !defined(_WIN32)

/**
 * Convert UNICODE string to lowercase
 */
WCHAR LIBNETXMS_EXPORTABLE *wcslwr(WCHAR *str)
{
   for(WCHAR *p = str; *p != 0; p++)
   {
#if HAVE_TOWLOWER
      *p = towlower(*p);
#else
      if ((*p >= 'a') && (*p <= 'z'))
         *p = *p - ('a' - 'A');
#endif
   }
   return str;
}

#endif

#if !defined(_WIN32) && (!HAVE_WCSFTIME || !WORKING_WCSFTIME)

/**
 * wide char version of strftime
 */
size_t LIBNETXMS_EXPORTABLE nx_wcsftime(WCHAR *buffer, size_t bufsize, const WCHAR *format, const struct tm *t)
{
#if HAVE_ALLOCA
   char *mbuf = (char *)alloca(bufsize);
   size_t flen = wcslen(format) + 1;
   char *mfmt = (char *)alloca(flen);
   WideCharToMultiByte(CP_ACP, WC_DEFAULTCHAR | WC_COMPOSITECHECK, format, -1, mfmt, flen, NULL, NULL);
#else
   char *mbuf = (char *)malloc(bufsize);
   char *mfmt = MBStringFromWideString(format);
#endif
   size_t rc = strftime(mbuf, bufsize, mfmt, t);
   if (rc > 0)
   {
      MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, mbuf, -1, buffer, (int)bufsize);
      buffer[bufsize - 1] = 0;
   }
   else
   {
      buffer[0] = 0;
   }
#if !HAVE_ALLOCA
   free(mbuf);
   free(mfmt);
#endif
   return rc;
}

#endif

#if !HAVE__ITOA && !defined(_WIN32)

/**
 * _itoa() implementation
 */
char LIBNETXMS_EXPORTABLE *_itoa(int value, char *str, int base)
{
   char *p = str;
   if (value < 0)
   {
      *p++ = '-';
      value = -value;
   }

   char buffer[64];
   char *t = buffer;
   do
   {
      int rem = value % base;
      *t++ = (rem < 10) ? (rem + '0') : (rem - 10 + 'a');
      value = value / base;
   } while(value > 0);

   t--;
   while(t >= buffer)
      *p++ = *t--;
   *p = 0;
   return str;
}

#endif

#if !HAVE__ITOA && !defined(_WIN32)

/**
 * _itow() implementation
 */
WCHAR LIBNETXMS_EXPORTABLE *_itow(int value, WCHAR *str, int base)
{
   WCHAR *p = str;
   if (value < 0)
   {
      *p++ = '-';
      value = -value;
   }

   WCHAR buffer[64];
   WCHAR *t = buffer;
   do
   {
      int rem = value % base;
      *t++ = (rem < 10) ? (rem + '0') : (rem - 10 + 'a');
      value = value / base;
   } while(value > 0);

   t--;
   while(t >= buffer)
      *p++ = *t--;
   *p = 0;
   return str;
}

#endif

/**
 * Get sleep time until specific time
 */
int LIBNETXMS_EXPORTABLE GetSleepTime(int hour, int minute, int second)
{
   time_t now = time(NULL);

   struct tm localTime;
#if HAVE_LOCALTIME_R
   localtime_r(&now, &localTime);
#else
   memcpy(&localTime, localtime(&now), sizeof(struct tm));
#endif

   int target = hour * 3600 + minute * 60 + second;
   int curr = localTime.tm_hour * 3600 + localTime.tm_min * 60 + localTime.tm_sec;
   return (target >= curr) ? target - curr : 86400 - (curr - target);
}

/**
 * Parse timestamp (should be in form YYMMDDhhmmss or YYYYMMDDhhmmss), local time
 * If timestamp string is invalid returns default value
 */
time_t LIBNETXMS_EXPORTABLE ParseDateTimeA(const char *text, time_t defaultValue)
{
	int len = (int)strlen(text);
	if ((len != 12) && (len != 14))
		return defaultValue;

	struct tm t;
	char buffer[16], *curr;

	strncpy(buffer, text, 16);
	curr = &buffer[len - 2];

	memset(&t, 0, sizeof(struct tm));
	t.tm_isdst = -1;

	t.tm_sec = strtol(curr, NULL, 10);
	*curr = 0;
	curr -= 2;

	t.tm_min = strtol(curr, NULL, 10);
	*curr = 0;
	curr -= 2;

	t.tm_hour = strtol(curr, NULL, 10);
	*curr = 0;
	curr -= 2;

	t.tm_mday = strtol(curr, NULL, 10);
	*curr = 0;
	curr -= 2;

	t.tm_mon = strtol(curr, NULL, 10) - 1;
	*curr = 0;

	if (len == 12)
	{
		curr -= 2;
		t.tm_year = strtol(curr, NULL, 10) + 100;	// Assuming XXI century
	}
	else
	{
		curr -= 4;
		t.tm_year = strtol(curr, NULL, 10) - 1900;
	}

	return mktime(&t);
}

/**
 * Parse timestamp (should be in form YYMMDDhhmmss or YYYYMMDDhhmmss), local time
 * If timestamp string is invalid returns default value
 * (UNICODE version)
 */
time_t LIBNETXMS_EXPORTABLE ParseDateTimeW(const WCHAR *text, time_t defaultValue)
{
   char buffer[16];
   WideCharToMultiByte(CP_ACP, WC_COMPOSITECHECK | WC_DEFAULTCHAR, text, -1, buffer, 16, NULL, NULL);
   buffer[15] = 0;
   return ParseDateTimeA(buffer, defaultValue);
}

/**
 * Get NetXMS directory
 */
void LIBNETXMS_EXPORTABLE GetNetXMSDirectory(nxDirectoryType type, TCHAR *dir)
{
   *dir = 0;

   const TCHAR *homeDir = _tgetenv(_T("NETXMS_HOME"));
   if (homeDir != NULL)
   {
#ifdef _WIN32
      switch(type)
      {
         case nxDirBin:
            _sntprintf(dir, MAX_PATH, _T("%s\\bin"), homeDir);
            break;
         case nxDirData:
            _sntprintf(dir, MAX_PATH, _T("%s\\var"), homeDir);
            break;
         case nxDirEtc:
            _sntprintf(dir, MAX_PATH, _T("%s\\etc"), homeDir);
            break;
         case nxDirLib:
            _sntprintf(dir, MAX_PATH, _T("%s\\lib"), homeDir);
            break;
         case nxDirShare:
            _sntprintf(dir, MAX_PATH, _T("%s\\share"), homeDir);
            break;
         default:
            nx_strncpy(dir, homeDir, MAX_PATH);
            break;
      }
#else
      switch(type)
      {
         case nxDirBin:
            _sntprintf(dir, MAX_PATH, _T("%s/bin"), homeDir);
            break;
         case nxDirData:
            _sntprintf(dir, MAX_PATH, _T("%s/var/lib/netxms"), homeDir);
            break;
         case nxDirEtc:
            _sntprintf(dir, MAX_PATH, _T("%s/etc"), homeDir);
            break;
         case nxDirLib:
            _sntprintf(dir, MAX_PATH, _T("%s/lib/netxms"), homeDir);
            break;
         case nxDirShare:
            _sntprintf(dir, MAX_PATH, _T("%s/share/netxms"), homeDir);
            break;
         default:
            nx_strncpy(dir, homeDir, MAX_PATH);
            break;
      }
#endif
      return;
   }

#ifdef _WIN32
   TCHAR installPath[MAX_PATH] = _T("");
   HKEY hKey;
   bool found = false;
   if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, _T("Software\\NetXMS\\Server"), 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
   {
      DWORD size = MAX_PATH * sizeof(TCHAR);
      found = (RegQueryValueEx(hKey, _T("InstallPath"), NULL, NULL, (BYTE *)installPath, &size) == ERROR_SUCCESS);
      RegCloseKey(hKey);
   }

   if (!found)
   {
      if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, _T("Software\\NetXMS\\Agent"), 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
      {
         DWORD size = MAX_PATH * sizeof(TCHAR);
         found = (RegQueryValueEx(hKey, _T("InstallPath"), NULL, NULL, (BYTE *)installPath, &size) == ERROR_SUCCESS);
         RegCloseKey(hKey);
      }
   }

   if (!found && (GetModuleFileName(NULL, installPath, MAX_PATH) > 0))
   {
      TCHAR *p = _tcsrchr(installPath, _T('\\'));
      if (p != NULL)
      {
         *p = 0;
         p = _tcsrchr(installPath, _T('\\'));
         if (p != NULL)
         {
            *p = 0;
            found = true;
         }
      }
   }

   if (!found)
   {
      _tcscpy(installPath, _T("C:\\NetXMS"));
   }

   switch(type)
   {
      case nxDirBin:
         _sntprintf(dir, MAX_PATH, _T("%s\\bin"), installPath);
         break;
      case nxDirData:
         _sntprintf(dir, MAX_PATH, _T("%s\\var"), installPath);
         break;
      case nxDirEtc:
         _sntprintf(dir, MAX_PATH, _T("%s\\etc"), installPath);
         break;
      case nxDirLib:
         _sntprintf(dir, MAX_PATH, _T("%s\\lib"), installPath);
         break;
      case nxDirShare:
         _sntprintf(dir, MAX_PATH, _T("%s\\share"), installPath);
         break;
      default:
         nx_strncpy(dir, installPath, MAX_PATH);
         break;
   }
#else
   switch(type)
   {
      case nxDirBin:
#ifdef PREFIX
         _tcscpy(dir, PREFIX _T("/bin"));
#else
         _tcscpy(dir, _T("/usr/bin"));
#endif
         break;
      case nxDirData:
#ifdef STATEDIR
         _tcscpy(dir, STATEDIR);
#else
         _tcscpy(dir, _T("/var/lib/netxms"));
#endif
         break;
      case nxDirEtc:
#ifdef PREFIX
         _tcscpy(dir, PREFIX _T("/etc"));
#else
         _tcscpy(dir, _T("/etc"));
#endif
         break;
      case nxDirLib:
#ifdef PKGLIBDIR
         _tcscpy(dir, PKGLIBDIR);
#else
         _tcscpy(dir, _T("/usr/lib/netxms"));
#endif
         break;
      case nxDirShare:
#ifdef DATADIR
         _tcscpy(dir, DATADIR);
#else
         _tcscpy(dir, _T("/usr/share/netxms"));
#endif
         break;
      default:
         _tcscpy(dir, _T("/usr"));
         break;
   }
#endif
}

#ifndef _WIN32

/**
 * Check potential JVM path
 */
static bool CheckJvmPath(const char *base, const char *libdir, const char *arch, char *jvm, const TCHAR *description)
{
   snprintf(jvm, MAX_PATH, "%s%s/lib/%s/server/libjvm.so", base, libdir, arch);
   nxlog_debug(7, _T("FindJavaRuntime: checking %hs (%s)"), jvm, description);
   if (_access(jvm, 0) == 0)
      return true;

   snprintf(jvm, MAX_PATH, "%s%s/jre/lib/%s/server/libjvm.so", base, libdir, arch);
   nxlog_debug(7, _T("FindJavaRuntime: checking %hs (%s)"), jvm, description);
   if (_access(jvm, 0) == 0)
      return true;

   if (!strcmp(arch, "x86_64"))
      return CheckJvmPath(base, libdir, "amd64", jvm, description);

   if (!strcmp(arch, "i686"))
      return CheckJvmPath(base, libdir, "i386", jvm, description);

   return false;
}

#endif

/**
 * Find Java runtime module. Search algorithm is following:
 * 1. Windows only - check for bundled JRE in bin\jre
 * 2. Check for bundled JRE in $NETXMS_HOME/bin/jre (Windows) or $NETXMS_HOME/lib/jre (non-Windows)
 * 3. Windows only - check JRE location in registry
 * 3. Check $JAVA_HOME
 * 4. Check $JAVA_HOME/jre
 * 5. Check JDK location specified at compile time
 *
 * @param buffer buffer for result
 * @param size buffer size in characters
 * @return buffer on success or NULL on failure
 */
TCHAR LIBNETXMS_EXPORTABLE *FindJavaRuntime(TCHAR *buffer, size_t size)
{
#ifdef _WIN32
   TCHAR path[MAX_PATH];
   GetModuleFileName(NULL, path, MAX_PATH);
   TCHAR *s = _tcsrchr(path, _T('\\'));
   if (s != NULL)
   {
      s++;
      _tcscpy(s, _T("jre\\bin\\server\\jvm.dll"));
      nxlog_debug(7, _T("FindJavaRuntime: checking %s (executable path)"), path);
      if (_taccess(path, 0) == 0)
      {
         nx_strncpy(buffer, path, size);
         return buffer;
      }
   }
#endif

   char jvm[MAX_PATH] = "";

#ifndef _WIN32
   struct utsname un;
   uname(&un);
#endif

   // Use NETXMS_HOME
   const char *netxmsHome = getenv("NETXMS_HOME");
   if ((netxmsHome != NULL) && (*netxmsHome != 0))
   {
#ifdef _WIN32
      snprintf(jvm, MAX_PATH, "%s\\bin\\jre\\bin\\server\\jvm.dll", netxmsHome);
      nxlog_debug(7, _T("FindJavaRuntime: checking %hs (NetXMS home)"), jvm);
#else
      CheckJvmPath(netxmsHome, "/lib", un.machine, jvm, _T("NetXMS home"));
#endif
   }

#ifdef _WIN32
   if ((jvm[0] == 0) || (_access(jvm, 0) != 0))
   {
      HKEY hKey;
      if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, _T("SOFTWARE\\JavaSoft\\Java Runtime Environment"), 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
      {
         TCHAR currVersion[64];
         DWORD size = sizeof(currVersion);
         if (RegQueryValueEx(hKey, _T("CurrentVersion"), NULL, NULL, (BYTE *)currVersion, &size) == ERROR_SUCCESS)
         {
            HKEY hSubKey;
            if (RegOpenKeyEx(hKey, currVersion, 0, KEY_QUERY_VALUE, &hSubKey) == ERROR_SUCCESS)
            {
               size = MAX_PATH - 20;
               if (RegQueryValueExA(hSubKey, "JavaHome", NULL, NULL, (BYTE *)jvm, &size) == ERROR_SUCCESS)
               {
                  strcat(jvm, "\\bin\\server\\jvm.dll");
                  nxlog_debug(7, _T("FindJavaRuntime: checking %hs (registry)"), jvm);
               }
               RegCloseKey(hSubKey);
            }
         }
         RegCloseKey(hKey);
      }
   }
#endif

   // Check JAVA_HOME
   if ((jvm[0] == 0) || (_access(jvm, 0) != 0))
   {
      const char *javaHome = getenv("JAVA_HOME");
      if ((javaHome != NULL) && (*javaHome != 0))
      {
#ifdef _WIN32
         snprintf(jvm, MAX_PATH, "%s\\bin\\server\\jvm.dll", javaHome);
         nxlog_debug(7, _T("FindJavaRuntime: checking %hs (Java home)"), jvm);
         if (_access(jvm, 0) != 0)
         {
            snprintf(jvm, MAX_PATH, "%s\\jre\\bin\\server\\jvm.dll", javaHome);
            nxlog_debug(7, _T("FindJavaRuntime: checking %hs (Java home)"), jvm);
         }
#else
         CheckJvmPath(javaHome, "", un.machine, jvm, _T("Java home"));
#endif
      }
   }

#ifdef JDK_LOCATION
   if ((jvm[0] == 0) || (_access(jvm, 0) != 0))
   {
#ifdef _WIN32
      snprintf(jvm, MAX_PATH, JDK_LOCATION "\\jre\\bin\\server\\jvm.dll");
      nxlog_debug(7, _T("FindJavaRuntime: checking %hs (JDK defined at compile time)"), jvm);
#else
      CheckJvmPath(JDK_LOCATION, "", un.machine, jvm, _T("JDK defined at compile time"));
#endif
   }
#endif

   if ((jvm[0] == 0) || (_access(jvm, 0) != 0))
      return NULL;

#ifdef UNICODE
   MultiByteToWideChar(CP_UTF8, 0, jvm, -1, buffer, (int)size);
   buffer[size - 1] = 0;
#else
   nx_strncpy(buffer, jvm, size);
#endif
   return buffer;
}

#if WITH_JEMALLOC

/**
 * Callback for jemalloc's malloc_stats_print
 */
static void jemalloc_stats_cb(void *arg, const char *text)
{
   fwrite(text, 1, strlen(text), (FILE *)arg);
}

#endif

/**
 * Get heap information using system-specific functions (if available)
 */
TCHAR LIBNETXMS_EXPORTABLE *GetHeapInfo()
{
#if WITH_JEMALLOC || HAVE_MALLOC_INFO
   char *buffer = NULL;
   size_t size = 0;
   FILE *f = open_memstream(&buffer, &size);
   if (f == NULL)
      return NULL;
#if WITH_JEMALLOC
   malloc_stats_print(jemalloc_stats_cb, f, NULL);
#else
   malloc_info(0, f);
#endif
   fclose(f);
#ifdef UNICODE
   WCHAR *wtext = WideStringFromMBString(buffer);
   free(buffer);
   return wtext;
#else
   return buffer;
#endif
#else
   return _tcsdup(_T("No heap information API available"));
#endif
}

/**
 * Destructor for abstract iterator
 */
AbstractIterator::~AbstractIterator()
{
}

/**
 * Escape string for JSON
 */
String LIBNETXMS_EXPORTABLE EscapeStringForJSON(const TCHAR *s)
{
   String js;
   if (s == NULL)
      return js;
   for(const TCHAR *p = s; *p != 0; p++)
   {
      if (*p == _T('"') || *p == _T('\\'))
         js.append(_T('\\'));
      js.append(*p);
   }
   return js;
}

/**
 * Escape string for agent parameter
 */
String LIBNETXMS_EXPORTABLE EscapeStringForAgent(const TCHAR *s)
{
   String out;
   if (s == NULL)
      return out;
   for(const TCHAR *p = s; *p != 0; p++)
   {
      if (*p == _T('"'))
         out.append(_T('"'));
      out.append(*p);
   }
   return out;
}

/**
 * Parse command line into argumen list considering single and double quotes
 */
StringList LIBNETXMS_EXPORTABLE *ParseCommandLine(const TCHAR *cmdline)
{
   StringList *args = new StringList();

   TCHAR *temp = _tcsdup(cmdline);
   int state = 0;

   TCHAR *curr = temp;
   while(*curr == ' ')
      curr++;

   if (*curr != 0)
   {
      int len = (int)_tcslen(temp);
      for(int i = (int)(curr - temp); i < len; i++)
      {
         switch(temp[i])
         {
            case ' ':
               if (state == 0)
               {
                  temp[i] = 0;
                  args->add(curr);
                  while(temp[i + 1] == ' ')
                     i++;
                  curr = &temp[i + 1];
               }
               break;
            case '"':
               if (state == 2)
                  break;   // within single quoted string
               if (state == 0)
               {
                  state = 1;
               }
               else
               {
                  state = 0;
               }
               memmove(&temp[i], &temp[i + 1], (len - i) * sizeof(TCHAR));
               i--;
               break;
            case '\'':
               if (state == 1)
                  break;   // within double quoted string
               if (state == 0)
               {
                  state = 2;
               }
               else
               {
                  state = 0;
               }
               memmove(&temp[i], &temp[i + 1], (len - i) * sizeof(TCHAR));
               i--;
               break;
            default:
               break;
         }
      }

      if (*curr != 0)
         args->add(curr);
   }
   free(temp);
   return args;
}
