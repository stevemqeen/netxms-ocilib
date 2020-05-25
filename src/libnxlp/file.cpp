/*
** NetXMS - Network Management System
** Log Parsing Library
** Copyright (C) 2003-2017 Victor Kirhenshtein
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 3 of the License, or
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
** File: file.cpp
**
**/

#include "libnxlp.h"

#ifdef _WIN32
#include <share.h>
#endif


#if defined(_WIN32)
#define NX_STAT _tstati64
#define NX_STAT_STRUCT struct _stati64
#elif HAVE_STAT64 && HAVE_STRUCT_STAT64
#define NX_STAT stat64
#define NX_STAT_STRUCT struct stat64
#else
#define NX_STAT stat
#define NX_STAT_STRUCT struct stat
#endif

#if defined(_WIN32)
#define NX_FSTAT _fstati64
#elif HAVE_FSTAT64 && HAVE_STRUCT_STAT64
#define NX_FSTAT fstat64
#else
#define NX_FSTAT fstat
#endif

#if defined(UNICODE) && !defined(_WIN32)
inline int __call_stat(const WCHAR *f, NX_STAT_STRUCT *s)
{
	char *mbf = MBStringFromWideString(f);
	int rc = NX_STAT(mbf, s);
	free(mbf);
	return rc;
}
#define CALL_STAT(f, s) __call_stat(f, s)
#else
#define CALL_STAT(f, s) NX_STAT(f, s)
#endif


/**
 * Constants
 */
#define READ_BUFFER_SIZE      4096

/**
 * Find byte sequence in the stream
 */
static char *FindSequence(char *start, int length, const char *sequence, int seqLength)
{
	char *curr = start;
	int count = 0;
	while(length - count >= seqLength)
	{
		if (!memcmp(curr, sequence, seqLength))
			return curr;
		curr += seqLength;
		count += seqLength;
	}
	return NULL;
}

/**
 * Find end-of-line marker
 */
static char *FindEOL(char *start, int length, int encoding)
{
   char *eol = NULL;
	switch(encoding)
	{
		case LP_FCP_UCS2:
#if WORDS_BIGENDIAN
			eol = FindSequence(start, length, "\0\n", 2);
#else
			eol = FindSequence(start, length, "\n\0", 2);
#endif
			break;
		case LP_FCP_UCS2_LE:
			eol = FindSequence(start, length, "\n\0", 2);
			break;
		case LP_FCP_UCS2_BE:
			eol = FindSequence(start, length, "\0\n", 2);
			break;
		case LP_FCP_UCS4:
#if WORDS_BIGENDIAN
         eol = FindSequence(start, length, "\0\0\0\n", 4);
#else
         eol = FindSequence(start, length, "\n\0\0\0", 4);
#endif
		   break;
		case LP_FCP_UCS4_LE:
			eol = FindSequence(start, length, "\n\0\0\0", 4);
			break;
		case LP_FCP_UCS4_BE:
			eol = FindSequence(start, length, "\0\0\0\n", 4);
			break;
		default:
			eol = (char *)memchr(start, '\n', length);
			break;
	}

   if (eol == NULL)
   {
      // Try to find CR
	   switch(encoding)
	   {
		case LP_FCP_UCS2:
#if WORDS_BIGENDIAN
			eol = FindSequence(start, length, "\0\r", 2);
#else
			eol = FindSequence(start, length, "\r\0", 2);
#endif
			break;
		   case LP_FCP_UCS2_LE:
			   eol = FindSequence(start, length, "\r\0", 2);
				break;
		   case LP_FCP_UCS2_BE:
			   eol = FindSequence(start, length, "\0\r", 2);
				break;
		case LP_FCP_UCS4:
#if WORDS_BIGENDIAN
         eol = FindSequence(start, length, "\0\0\0\r", 4);
#else
         eol = FindSequence(start, length, "\r\0\0\0", 4);
#endif
		   break;
		   case LP_FCP_UCS4_LE:
			   eol = FindSequence(start, length, "\r\0\0\0", 4);
				break;
		   case LP_FCP_UCS4_BE:
			   eol = FindSequence(start, length, "\0\0\0\r", 4);
				break;
		   default:
			   eol = (char *)memchr(start, '\r', length);
				break;
	   }
   }

   return eol;
}

/**
 * Parse new log records
 */
static off_t ParseNewRecords(LogParser *parser, int fh)
{
   char *ptr, *eptr, buffer[READ_BUFFER_SIZE];
   int bytes, bufPos = 0;
   off_t resetPos;
	int encoding = parser->getFileEncoding();
	TCHAR text[READ_BUFFER_SIZE];

   do
   {
      resetPos = lseek(fh, 0, SEEK_CUR);
      if ((bytes = _read(fh, &buffer[bufPos], READ_BUFFER_SIZE - bufPos)) > 0)
      {
         bytes += bufPos;
         for(ptr = buffer;; ptr = eptr + 1)
         {
            bufPos = (int)(ptr - buffer);
				eptr = FindEOL(ptr, bytes - bufPos, encoding);
            if (eptr == NULL)
            {
					int remaining = bytes - bufPos;
               resetPos = lseek(fh, 0, SEEK_CUR) - remaining;
					if (remaining > 0)
					{
                  memmove(buffer, ptr, remaining);
                  if (parser->isFilePreallocated() && !memcmp(buffer, "\x00\x00\x00\x00", MIN(remaining, 4)))
                  {
                     // Found zeroes in preallocated file, next read should be after last known EOL
                     return resetPos;
                  }
					}
               break;
            }
				// remove possible CR character and put 0 to indicate end of line
				switch(encoding)
				{
					case LP_FCP_UCS2:
#if WORDS_BIGENDIAN
                  if ((eptr - ptr >= 2) && !memcmp(eptr - 2, "\0\r", 2))
							eptr -= 2;
						*eptr = 0;
						*(eptr + 1) = 0;
#else

						if ((eptr - ptr >= 2) && !memcmp(eptr - 2, "\r\0", 2))
							eptr -= 2;
						*eptr = 0;
						*(eptr + 1) = 0;
#endif
						break;
					case LP_FCP_UCS2_LE:
						if ((eptr - ptr >= 2) && !memcmp(eptr - 2, "\r\0", 2))
							eptr -= 2;
						*eptr = 0;
						*(eptr + 1) = 0;
						break;
					case LP_FCP_UCS2_BE:
						if ((eptr - ptr >= 2) && !memcmp(eptr - 2, "\0\r", 2))
							eptr -= 2;
						*eptr = 0;
						*(eptr + 1) = 0;
						break;
					case LP_FCP_UCS4:
#if WORDS_BIGENDIAN
                 if ((eptr - ptr >= 4) && !memcmp(eptr - 4, "\0\0\0\r", 4))
							eptr -= 4;
						memset(eptr, 0, 4);
#else

						if ((eptr - ptr >= 4) && !memcmp(eptr - 4, "\r\0\0\0", 4))
							eptr -= 4;
						memset(eptr, 0, 4);
#endif
						break;
					case LP_FCP_UCS4_LE:
						if ((eptr - ptr >= 4) && !memcmp(eptr - 4, "\r\0\0\0", 4))
							eptr -= 4;
						memset(eptr, 0, 4);
						break;
					case LP_FCP_UCS4_BE:
						if ((eptr - ptr >= 4) && !memcmp(eptr - 4, "\0\0\0\r", 4))
							eptr -= 4;
						memset(eptr, 0, 4);
						break;
					default:
						if (*(eptr - 1) == '\r')
							eptr--;
						*eptr = 0;
						break;
				}
				// Now ptr points to null-terminated string in original encoding
				// Do the conversion to platform encoding
#ifdef UNICODE
				switch(encoding)
				{
					case LP_FCP_ACP:
						MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, ptr, -1, text, READ_BUFFER_SIZE);
						break;
					case LP_FCP_UTF8:
						MultiByteToWideChar(CP_UTF8, 0, ptr, -1, text, READ_BUFFER_SIZE);
						break;
               case LP_FCP_UCS2_LE:
#if WORDS_BIGENDIAN
                  bswap_array_16((UINT16 *)ptr, -1);
#endif
#ifdef UNICODE_UCS2
						nx_strncpy(text, (TCHAR *)ptr, READ_BUFFER_SIZE);
#else
                  ucs2_to_ucs4((UCS2CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
#endif
                  break;
               case LP_FCP_UCS2_BE:
#if !WORDS_BIGENDIAN
                  bswap_array_16((UINT16 *)ptr, -1);
#endif
#ifdef UNICODE_UCS2
						nx_strncpy(text, (TCHAR *)ptr, READ_BUFFER_SIZE);
#else
                  ucs2_to_ucs4((UCS2CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
#endif
                  break;
               case LP_FCP_UCS2:
#ifdef UNICODE_UCS2
						nx_strncpy(text, (TCHAR *)ptr, READ_BUFFER_SIZE);
#else
                  ucs2_to_ucs4((UCS2CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
#endif
                  break;
               case LP_FCP_UCS4_LE:
#if WORDS_BIGENDIAN
                  bswap_array_32((UINT32 *)ptr, -1);
#endif
#ifdef UNICODE_UCS2
                  ucs4_to_ucs2((UCS4CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
#else
                  nx_strncpy(text, (TCHAR *)ptr, READ_BUFFER_SIZE);
#endif
                  break;
               case LP_FCP_UCS4_BE:
#if !WORDS_BIGENDIAN
                  bswap_array_32((UINT32 *)ptr, -1);
#endif
#ifdef UNICODE_UCS2
                  ucs4_to_ucs2((UCS4CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
#else
                  nx_strncpy(text, (TCHAR *)ptr, READ_BUFFER_SIZE);
#endif
                  break;
               case LP_FCP_UCS4:
#ifdef UNICODE_UCS2
                  ucs4_to_ucs2((UCS4CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
#else
                  nx_strncpy(text, (TCHAR *)ptr, READ_BUFFER_SIZE);
#endif
                  break;
					default:
						break;
				}
#else
				switch(encoding)
				{
					case LP_FCP_ACP:
						nx_strncpy(text, ptr, READ_BUFFER_SIZE);
						break;
               case LP_FCP_UTF8:
                  utf8_to_mb(ptr, -1, text, READ_BUFFER_SIZE);
                  break;
               case LP_FCP_UCS2_LE:
#if WORDS_BIGENDIAN
                  bswap_array_16((UINT16 *)ptr, -1);
#endif
                  ucs2_to_mb((UCS2CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
                  break;
               case LP_FCP_UCS2_BE:
#if !WORDS_BIGENDIAN
                  bswap_array_16((UINT16 *)ptr, -1);
#endif
                  ucs2_to_mb((UCS2CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
                  break;
               case LP_FCP_UCS2:
                  ucs2_to_mb((UCS2CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
                  break;
               case LP_FCP_UCS4_LE:
#if WORDS_BIGENDIAN
                  bswap_array_32((UINT32 *)ptr, -1);
#endif
                  ucs4_to_mb((UCS4CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
                  break;
               case LP_FCP_UCS4_BE:
#if !WORDS_BIGENDIAN
                  bswap_array_32((UINT32 *)ptr, -1);
#endif
                  ucs4_to_mb((UCS4CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
                  break;
               case LP_FCP_UCS4:
                  ucs4_to_mb((UCS4CHAR *)ptr, -1, text, READ_BUFFER_SIZE);
                  break;
					default:
						break;
				}
#endif
				parser->matchLine(text);
         }
      }
      else
      {
         bytes = 0;
      }
   } while(bytes == READ_BUFFER_SIZE);
   return resetPos;
}

/**
 * Scan first 10 bytes of a file to find its encoding
 */
static int ScanFileEncoding(int fh)
{
   char buffer[10];
   if (_read(fh, buffer, 4) > 3)
   {
      if (!memcmp(buffer, "\x00\x00\xFE\xFF", 4))
         return LP_FCP_UCS4_BE;
      if (!memcmp(buffer, "\xFF\xFE\x00\x00", 4))
         return LP_FCP_UCS4_LE;
      if (!memcmp(buffer, "\xEF\xBB\xBF", 3))
         return LP_FCP_UTF8;
      if (!memcmp(buffer, "\xFE\xFF", 2))
         return LP_FCP_UCS2_BE;
      if (!memcmp(buffer, "\xFF\xFE", 2))
         return LP_FCP_UCS2_LE;
   }

   return LP_FCP_ACP;
}

/**
 * Seek file to the beginning of zeroes block
 */
static void SeekToZero(int fh, int chsize)
{
   char buffer[4096];
   while(true)
   {
      int bytes = _read(fh, buffer, 4096);
      if (bytes <= 0)
         break;
      char *p = buffer;
      for(int i = 0; i < bytes - chsize + 1; i++, p++)
      {
         if ((*p == 0) && ((chsize == 1) || !memcmp(p, "\x00\x00\x00\x00", chsize)))
         {
            off_t pos = lseek(fh, i - bytes, SEEK_CUR);
            LogParserTrace(6, _T("LogParser: beginning of zero block found at %ld"), (long)pos);
            return;
         }
      }
      if (chsize > 1)
         lseek(fh, 1 - chsize, SEEK_CUR); // re-read potentially incomplete last character
   }
}

/**
 * File parser thread
 */
bool LogParser::monitorFile(CONDITION stopCondition, bool readFromCurrPos)
{
	TCHAR fname[MAX_PATH], temp[MAX_PATH];
	NX_STAT_STRUCT st, stn;
	size_t size;
	int fh;
	bool readFromStart = !readFromCurrPos;

	if (m_fileName == NULL)
	{
		LogParserTrace(0, _T("LogParser: parser thread will not start, file name not set"));
		return false;
	}

	LogParserTrace(0, _T("LogParser: parser thread for file \"%s\" started"), m_fileName);
	while(true)
	{
		ExpandFileName(getFileName(), fname, MAX_PATH, true);
		if (CALL_STAT(fname, &st) == 0)
		{
#ifdef _WIN32
			fh = _tsopen(fname, O_RDONLY, _SH_DENYNO);
#else
			fh = _topen(fname, O_RDONLY);
#endif
			if (fh != -1)
			{
				setStatus(LPS_RUNNING);
				LogParserTrace(3, _T("LogParser: file \"%s\" (pattern \"%s\") successfully opened"), fname, m_fileName);

            if (m_fileEncoding == -1)
            {
               m_fileEncoding = ScanFileEncoding(fh);
               lseek(fh, 0, SEEK_SET);
            }

				size = (size_t)st.st_size;
				if (readFromStart)
				{
					LogParserTrace(5, _T("LogParser: parsing existing records in file \"%s\""), fname);
					off_t resetPos = ParseNewRecords(this, fh);
               lseek(fh, resetPos, SEEK_SET);
				}
				else if (m_preallocatedFile)
				{
				   SeekToZero(fh, getCharSize());
				}
				else
				{
					lseek(fh, 0, SEEK_END);
				}

				while(true)
				{
					if (ConditionWait(stopCondition, 5000))
						goto stop_parser;

					// Check if file name was changed
					ExpandFileName(getFileName(), temp, MAX_PATH, true);
					if (_tcscmp(temp, fname))
					{
						LogParserTrace(5, _T("LogParser: file name change for \"%s\" (\"%s\" -> \"%s\")"), m_fileName, fname, temp);
						readFromStart = true;
						break;
					}

#ifdef _NETWARE
					if (fgetstat(fh, &st, ST_SIZE_BIT | ST_NAME_BIT) < 0)
					{
						LogParserTrace(1, _T("LogParser: fgetstat(%d) failed, errno=%d"), fh, errno);
						readFromStart = true;
						break;
					}
#else
					if (NX_FSTAT(fh, &st) < 0)
					{
						LogParserTrace(1, _T("LogParser: fstat(%d) failed, errno=%d"), fh, errno);
						readFromStart = true;
						break;
					}
#endif

					if (CALL_STAT(fname, &stn) < 0)
					{
						LogParserTrace(1, _T("LogParser: stat(%s) failed, errno=%d"), fname, errno);
						readFromStart = true;
						break;
					}

#ifdef _WIN32
					if (st.st_ctime != stn.st_ctime)
					{
						LogParserTrace(3, _T("LogParser: creation time for fstat(%d) is not equal to creation time for stat(%s), assume file rename"), fh, fname);
						readFromStart = true;
						break;
					}
#else
					if ((st.st_ino != stn.st_ino) || (st.st_dev != stn.st_dev))
					{
						LogParserTrace(3, _T("LogParser: file device or inode differs for stat(%d) and fstat(%s), assume file rename"), fh, fname);
						readFromStart = true;
						break;
					}
#endif

					if ((size_t)st.st_size != size)
					{
						if ((size_t)st.st_size < size)
						{
							// File was cleared, start from the beginning
							lseek(fh, 0, SEEK_SET);
							LogParserTrace(3, _T("LogParser: file \"%s\" st_size != size"), fname);
						}
						size = (size_t)st.st_size;
						LogParserTrace(6, _T("LogParser: new data available in file \"%s\""), fname);
						off_t resetPos = ParseNewRecords(this, fh);
						lseek(fh, resetPos, SEEK_SET);
					}
					else if (m_preallocatedFile)
					{
					   char buffer[4];
					   int bytes = _read(fh, buffer, 4);
					   if ((bytes == 4) && memcmp(buffer, "\x00\x00\x00\x00", 4))
					   {
                     lseek(fh, -4, SEEK_CUR);
	                  LogParserTrace(6, _T("LogParser: new data available in file \"%s\""), fname);
	                  off_t resetPos = ParseNewRecords(this, fh);
	                  lseek(fh, resetPos, SEEK_SET);
					   }
					   else
					   {
                     off_t pos = lseek(fh, -bytes, SEEK_CUR);
                     if (pos > 0)
                     {
                        int readSize = MIN(pos, 4);
                        lseek(fh, -readSize, SEEK_CUR);
                        int bytes = _read(fh, buffer, readSize);
                        if (!memcmp(buffer, "\x00\x00\x00\x00", readSize))
                        {
                           LogParserTrace(6, _T("LogParser: detected reset of preallocated file \"%s\""), fname);
                           lseek(fh, 0, SEEK_SET);
                           off_t resetPos = ParseNewRecords(this, fh);
                           lseek(fh, resetPos, SEEK_SET);
                        }
                     }
					   }
					}
				}
				_close(fh);
			}
			else
			{
				setStatus(LPS_OPEN_ERROR);
			}
		}
		else
		{
			setStatus(LPS_NO_FILE);
			if (ConditionWait(stopCondition, 10000))
				break;
		}
	}

stop_parser:
	LogParserTrace(0, _T("LogParser: parser thread for file \"%s\" stopped"), m_fileName);
	return true;
}
