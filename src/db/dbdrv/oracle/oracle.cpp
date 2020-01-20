/* 
** Oracle Database Driver
** Copyright (C) 2007-2016 Victor Kirhenshtein
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
** File: oracle.cpp
**
**/

#include "oracledrv.h"

DECLARE_DRIVER_HEADER("ORACLE")

static DWORD DrvQueryInternal(ORACLE_CONN *pConn, const WCHAR *pwszQuery, WCHAR *errorText);

/**
 * Global environment handle
 */
static OCIEnv *s_handleEnv = NULL;

/**
 * Major OCI version
 */
static int s_ociVersionMajor = 0;

/**
 * Prepare string for using in SQL query - enclose in quotes and escape as needed
 */
extern "C" WCHAR EXPORT *DrvPrepareStringW(const WCHAR *str)
{
	int len = (int)wcslen(str) + 3;   // + two quotes and \0 at the end
	int bufferSize = len + 128;
	WCHAR *out = (WCHAR *)malloc(bufferSize * sizeof(WCHAR));
	out[0] = L'\'';

	const WCHAR *src = str;
	int outPos;
	for(outPos = 1; *src != 0; src++)
	{
		if (*src == L'\'')
		{
			len++;
			if (len >= bufferSize)
			{
				bufferSize += 128;
				out = (WCHAR *)realloc(out, bufferSize * sizeof(WCHAR));
			}
			out[outPos++] = L'\'';
			out[outPos++] = L'\'';
		}
		else
		{
			out[outPos++] = *src;
		}
	}
	out[outPos++] = L'\'';
	out[outPos++] = 0;

	return out;
}

/**
 * Prepare string for using in SQL query - enclose in quotes and escape as needed
 */
extern "C" char EXPORT *DrvPrepareStringA(const char *str)
{
	int len = (int)strlen(str) + 3;   // + two quotes and \0 at the end
	int bufferSize = len + 128;
	char *out = (char *)malloc(bufferSize);
	out[0] = '\'';

	const char *src = str;
	int outPos;
	for(outPos = 1; *src != 0; src++)
	{
		if (*src == '\'')
		{
			len++;
			if (len >= bufferSize)
			{
				bufferSize += 128;
				out = (char *)realloc(out, bufferSize);
			}
			out[outPos++] = '\'';
			out[outPos++] = '\'';
		}
		else
		{
			out[outPos++] = *src;
		}
	}
	out[outPos++] = '\'';
	out[outPos++] = 0;

	return out;
}

/**
 * Initialize driver
 */
extern "C" bool EXPORT DrvInit(const char *cmdLine)
{
   sword major, minor, update, patch, pupdate;
   OCIClientVersion(&major, &minor, &update, &patch, &pupdate);
   nxlog_debug(1, _T("ORACLE: OCI version %d.%d.%d.%d.%d"), (int)major, (int)minor, (int)update, (int)patch, (int)pupdate);
   s_ociVersionMajor = (int)major;

   if (OCIEnvNlsCreate(&s_handleEnv, OCI_THREADED | OCI_NCHAR_LITERAL_REPLACE_OFF,
                       NULL, NULL, NULL, NULL, 0, NULL, OCI_UTF16ID, OCI_UTF16ID) != OCI_SUCCESS)
   {
      nxlog_debug(1, _T("ORACLE: cannot allocate environment handle"));
      return false;
   }
	return true;
}

/**
 * Unload handler
 */
extern "C" void EXPORT DrvUnload()
{
   if (s_handleEnv != NULL)
      OCIHandleFree(s_handleEnv, OCI_HTYPE_ENV);
	OCITerminate(OCI_DEFAULT);
}

/**
 * Get error text from error handle
 */
static void GetErrorFromHandle(OCIError *handle, sb4 *errorCode, WCHAR *errorText)
{
#if UNICODE_UCS2
	OCIErrorGet(handle, 1, NULL, errorCode, (text *)errorText, DBDRV_MAX_ERROR_TEXT, OCI_HTYPE_ERROR);
	errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
#else
	UCS2CHAR buffer[DBDRV_MAX_ERROR_TEXT];

	OCIErrorGet(handle, 1, NULL, errorCode, (text *)buffer, DBDRV_MAX_ERROR_TEXT, OCI_HTYPE_ERROR);
	buffer[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	ucs2_to_ucs4(buffer, ucs2_strlen(buffer) + 1, errorText, DBDRV_MAX_ERROR_TEXT);
	errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
#endif
	RemoveTrailingCRLFW(errorText);
}

/**
 * Set last error text
 */
static void SetLastError(ORACLE_CONN *pConn)
{
	GetErrorFromHandle(pConn->handleError, &pConn->lastErrorCode, pConn->lastErrorText);
}

/**
 * Check if last error was caused by lost connection to server
 */
static DWORD IsConnectionError(ORACLE_CONN *conn)
{
	ub4 nStatus = 0;
	OCIAttrGet(conn->handleServer, OCI_HTYPE_SERVER, &nStatus, NULL, OCI_ATTR_SERVER_STATUS, conn->handleError);
	return (nStatus == OCI_SERVER_NOT_CONNECTED) ? DBERR_CONNECTION_LOST : DBERR_OTHER_ERROR;
}

/**
 * Destroy query result
 */
static void DestroyQueryResult(ORACLE_RESULT *pResult)
{
	int i, nCount;

	nCount = pResult->nCols * pResult->nRows;
	for(i = 0; i < nCount; i++)
		free(pResult->pData[i]);
	free(pResult->pData);

	for(i = 0; i < pResult->nCols; i++)
		free(pResult->columnNames[i]);
	free(pResult->columnNames);

	free(pResult);
}

/**
 * Connect to database
 */
extern "C" DBDRV_CONNECTION EXPORT DrvConnect(const char *host, const char *login, const char *password, 
                                              const char *database, const char *schema, WCHAR *errorText)
{
	ORACLE_CONN *pConn;
	UCS2CHAR *pwszStr;

	pConn = (ORACLE_CONN *)calloc(1, sizeof(ORACLE_CONN));
	if (pConn != NULL)
	{
      OCIHandleAlloc(s_handleEnv, (void **)&pConn->handleError, OCI_HTYPE_ERROR, 0, NULL);
		OCIHandleAlloc(s_handleEnv, (void **)&pConn->handleServer, OCI_HTYPE_SERVER, 0, NULL);
		pwszStr = UCS2StringFromMBString(host);
		if (OCIServerAttach(pConn->handleServer, pConn->handleError,
		                    (text *)pwszStr, (sb4)ucs2_strlen(pwszStr) * sizeof(UCS2CHAR), OCI_DEFAULT) == OCI_SUCCESS)
		{
			free(pwszStr);

			// Initialize service handle
			OCIHandleAlloc(s_handleEnv, (void **)&pConn->handleService, OCI_HTYPE_SVCCTX, 0, NULL);
			OCIAttrSet(pConn->handleService, OCI_HTYPE_SVCCTX, pConn->handleServer, 0, OCI_ATTR_SERVER, pConn->handleError);
			
			// Initialize session handle
			OCIHandleAlloc(s_handleEnv, (void **)&pConn->handleSession, OCI_HTYPE_SESSION, 0, NULL);
			pwszStr = UCS2StringFromMBString(login);
			OCIAttrSet(pConn->handleSession, OCI_HTYPE_SESSION, pwszStr,
			           (ub4)ucs2_strlen(pwszStr) * sizeof(UCS2CHAR), OCI_ATTR_USERNAME, pConn->handleError);
			free(pwszStr);
			pwszStr = UCS2StringFromMBString(password);
			OCIAttrSet(pConn->handleSession, OCI_HTYPE_SESSION, pwszStr,
			           (ub4)ucs2_strlen(pwszStr) * sizeof(UCS2CHAR), OCI_ATTR_PASSWORD, pConn->handleError);

			// Authenticate
			if (OCISessionBegin(pConn->handleService, pConn->handleError,
			                    pConn->handleSession, OCI_CRED_RDBMS, OCI_STMT_CACHE) == OCI_SUCCESS)
			{
				OCIAttrSet(pConn->handleService, OCI_HTYPE_SVCCTX, pConn->handleSession, 0, OCI_ATTR_SESSION, pConn->handleError);
				pConn->mutexQueryLock = MutexCreate();
				pConn->nTransLevel = 0;
				pConn->lastErrorCode = 0;
				pConn->lastErrorText[0] = 0;
            pConn->prefetchLimit = 10;

				if ((schema != NULL) && (schema[0] != 0))
				{
					free(pwszStr);
					pwszStr = UCS2StringFromMBString(schema);
					OCIAttrSet(pConn->handleSession, OCI_HTYPE_SESSION, pwszStr,
								  (ub4)ucs2_strlen(pwszStr) * sizeof(UCS2CHAR), OCI_ATTR_CURRENT_SCHEMA, pConn->handleError);
				}

            // Setup session
            DrvQueryInternal(pConn, L"ALTER SESSION SET NLS_LANGUAGE='AMERICAN' NLS_NUMERIC_CHARACTERS='.,'", NULL);

            UCS2CHAR version[1024];
            if (OCIServerVersion(pConn->handleService, pConn->handleError, (OraText *)version, sizeof(version), OCI_HTYPE_SVCCTX) == OCI_SUCCESS)
            {
#ifdef UNICODE
#if UNICODE_UCS4
               WCHAR *wver = UCS4StringFromUCS2String(version);
               nxlog_debug(5, _T("ORACLE: connected to %s"), wver);
               free(wver);
#else
               nxlog_debug(5, _T("ORACLE: connected to %s"), version);
#endif
#else
               char *mbver = MBStringFromUCS2String(version);
               nxlog_debug(5, _T("ORACLE: connected to %s"), mbver);
               free(mbver);
#endif
            }
         }
			else
			{
				GetErrorFromHandle(pConn->handleError, &pConn->lastErrorCode, errorText);
		      OCIServerDetach(pConn->handleServer, pConn->handleError, OCI_DEFAULT);
			   OCIHandleFree(pConn->handleService, OCI_HTYPE_SVCCTX);
				OCIHandleFree(pConn->handleServer, OCI_HTYPE_SERVER);
			   OCIHandleFree(pConn->handleError, OCI_HTYPE_ERROR);
				free(pConn);
				pConn = NULL;
			}
		}
		else
		{
			GetErrorFromHandle(pConn->handleError, &pConn->lastErrorCode, errorText);
			OCIHandleFree(pConn->handleServer, OCI_HTYPE_SERVER);
			OCIHandleFree(pConn->handleError, OCI_HTYPE_ERROR);
			free(pConn);
			pConn = NULL;
		}
		free(pwszStr);
	}
	else
	{
		wcscpy(errorText, L"Memory allocation error");
	}

   return (DBDRV_CONNECTION)pConn;
}

/**
 * Disconnect from database
 */
extern "C" void EXPORT DrvDisconnect(ORACLE_CONN *pConn)
{
	if (pConn == NULL)
	   return;

   OCISessionEnd(pConn->handleService, pConn->handleError, NULL, OCI_DEFAULT);
   OCIServerDetach(pConn->handleServer, pConn->handleError, OCI_DEFAULT);
   OCIHandleFree(pConn->handleSession, OCI_HTYPE_SESSION);
   OCIHandleFree(pConn->handleService, OCI_HTYPE_SVCCTX);
   OCIHandleFree(pConn->handleServer, OCI_HTYPE_SERVER);
   OCIHandleFree(pConn->handleError, OCI_HTYPE_ERROR);
   MutexDestroy(pConn->mutexQueryLock);
   free(pConn);
}

/**
 * Set prefetch limit
 */
extern "C" void EXPORT DrvSetPrefetchLimit(ORACLE_CONN *pConn, int limit)
{
	if (pConn != NULL)
      pConn->prefetchLimit = limit;
}

/**
 * Convert query from NetXMS portable format to native Oracle format
 */
static UCS2CHAR *ConvertQuery(WCHAR *query)
{
#if UNICODE_UCS4
	UCS2CHAR *srcQuery = UCS2StringFromUCS4String(query);
#else
	UCS2CHAR *srcQuery = query;
#endif
	int count = NumCharsW(query, L'?');
	if (count == 0)
	{
#if UNICODE_UCS4
		return srcQuery;
#else
		return wcsdup(query);
#endif
	}

	UCS2CHAR *dstQuery = (UCS2CHAR *)malloc((ucs2_strlen(srcQuery) + count * 3 + 1) * sizeof(UCS2CHAR));
	bool inString = false;
	int pos = 1;
	UCS2CHAR *src, *dst;
	for(src = srcQuery, dst = dstQuery; *src != 0; src++)
	{
		switch(*src)
		{
			case '\'':
				*dst++ = *src;
				inString = !inString;
				break;
			case '\\':
				*dst++ = *src++;
				*dst++ = *src;
				break;
			case '?':
				if (inString)
				{
					*dst++ = '?';
				}
				else
				{
					*dst++ = ':';
					if (pos < 10)
					{
						*dst++ = pos + '0';
					}
					else if (pos < 100)
					{
						*dst++ = pos / 10 + '0';
						*dst++ = pos % 10 + '0';
					}
					else
					{
						*dst++ = pos / 100 + '0';
						*dst++ = (pos % 100) / 10 + '0';
						*dst++ = pos % 10 + '0';
					}
					pos++;
				}
				break;
			default:
				*dst++ = *src;
				break;
		}
	}
	*dst = 0;
#if UNICODE_UCS4
	free(srcQuery);
#endif
	return dstQuery;
}

/**
 * Prepare statement
 */
extern "C" ORACLE_STATEMENT EXPORT *DrvPrepare(ORACLE_CONN *pConn, WCHAR *pwszQuery, DWORD *pdwError, WCHAR *errorText)
{
	ORACLE_STATEMENT *stmt = NULL;
	OCIStmt *handleStmt;

	UCS2CHAR *ucs2Query = ConvertQuery(pwszQuery);

	MutexLock(pConn->mutexQueryLock);
	if (OCIStmtPrepare2(pConn->handleService, &handleStmt, pConn->handleError, (text *)ucs2Query,
	                    (ub4)ucs2_strlen(ucs2Query) * sizeof(UCS2CHAR), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
	{
		stmt = (ORACLE_STATEMENT *)malloc(sizeof(ORACLE_STATEMENT));
		stmt->connection = pConn;
		stmt->handleStmt = handleStmt;
		stmt->bindings = new Array(8, 8, false);
      stmt->batchBindings = NULL;
		stmt->buffers = new Array(8, 8, true);
      stmt->batchMode = false;
      stmt->batchSize = 0;
		OCIHandleAlloc(s_handleEnv, (void **)&stmt->handleError, OCI_HTYPE_ERROR, 0, NULL);
		*pdwError = DBERR_SUCCESS;
	}
	else
	{
		SetLastError(pConn);
		*pdwError = IsConnectionError(pConn);
	}

	if (errorText != NULL)
	{
		wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
		errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	}
	MutexUnlock(pConn->mutexQueryLock);

	free(ucs2Query);
	return stmt;
}

/**
 * Open batch
 */
extern "C" bool EXPORT DrvOpenBatch(ORACLE_STATEMENT *stmt)
{
   stmt->buffers->clear();
   if (stmt->batchBindings != NULL)
      stmt->batchBindings->clear();
   else
      stmt->batchBindings = new ObjectArray<OracleBatchBind>(16, 16, true);
   stmt->batchMode = true;
   stmt->batchSize = 0;
   return true;
}

/**
 * Start next batch row
 */
extern "C" void EXPORT DrvNextBatchRow(ORACLE_STATEMENT *stmt)
{
   if (!stmt->batchMode)
      return;
   
   for(int i = 0; i < stmt->batchBindings->size(); i++)
   {
	   OracleBatchBind *bind = stmt->batchBindings->get(i);
      if (bind != NULL)
         bind->addRow();
   }
   stmt->batchSize++;
}

/**
 * Buffer sizes for different C types
 */
static DWORD s_bufferSize[] = { 0, sizeof(LONG), sizeof(DWORD), sizeof(INT64), sizeof(QWORD), sizeof(double) };

/**
 * Corresponding Oracle types for C types
 */
static ub2 s_oracleType[] = { SQLT_STR, SQLT_INT, SQLT_UIN, SQLT_INT, SQLT_UIN, SQLT_FLT };

/**
 * Bind parameter to statement - normal mode (non-batch)
 */
static void BindNormal(ORACLE_STATEMENT *stmt, int pos, int sqlType, int cType, void *buffer, int allocType)
{
	OCIBind *handleBind = (OCIBind *)stmt->bindings->get(pos - 1);
	void *sqlBuffer;
	switch(cType)
	{
		case DB_CTYPE_STRING:
#if UNICODE_UCS4
			sqlBuffer = UCS2StringFromUCS4String((WCHAR *)buffer);
		   stmt->buffers->set(pos - 1, sqlBuffer);
			if (allocType == DB_BIND_DYNAMIC)
				free(buffer);
#else
         if (allocType == DB_BIND_TRANSIENT)
			{
				sqlBuffer = wcsdup((WCHAR *)buffer);
   		   stmt->buffers->set(pos - 1, sqlBuffer);
			}
			else
			{
				sqlBuffer = buffer;
				if (allocType == DB_BIND_DYNAMIC)
					stmt->buffers->set(pos - 1, sqlBuffer);
			}
#endif
		   OCIBindByPos(stmt->handleStmt, &handleBind, stmt->handleError, pos, sqlBuffer,
						    ((sb4)ucs2_strlen((UCS2CHAR *)sqlBuffer) + 1) * sizeof(UCS2CHAR), 
						    (sqlType == DB_SQLTYPE_TEXT) ? SQLT_LNG : SQLT_STR,
						    NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
			break;
      case DB_CTYPE_UTF8_STRING:
#if UNICODE_UCS4
         sqlBuffer = UCS2StringFromUTF8String((char *)buffer);
#else
         sqlBuffer = WideStringFromUTF8String((char *)buffer);
#endif
         stmt->buffers->set(pos - 1, sqlBuffer);
         if (allocType == DB_BIND_DYNAMIC)
            free(buffer);
         OCIBindByPos(stmt->handleStmt, &handleBind, stmt->handleError, pos, sqlBuffer,
                      ((sb4)ucs2_strlen((UCS2CHAR *)sqlBuffer) + 1) * sizeof(UCS2CHAR),
                      (sqlType == DB_SQLTYPE_TEXT) ? SQLT_LNG : SQLT_STR,
                      NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
         break;
		case DB_CTYPE_INT64:	// OCI prior to 11.2 cannot bind 64 bit integers
		   sqlBuffer = malloc(sizeof(OCINumber));
         stmt->buffers->set(pos - 1, sqlBuffer);
		   OCINumberFromInt(stmt->handleError, buffer, sizeof(INT64), OCI_NUMBER_SIGNED, (OCINumber *)sqlBuffer);
         OCIBindByPos(stmt->handleStmt, &handleBind, stmt->handleError, pos, sqlBuffer, sizeof(OCINumber),
                      SQLT_VNU, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
			if (allocType == DB_BIND_DYNAMIC)
				free(buffer);
			break;
		case DB_CTYPE_UINT64:	// OCI prior to 11.2 cannot bind 64 bit integers
         sqlBuffer = malloc(sizeof(OCINumber));
         stmt->buffers->set(pos - 1, sqlBuffer);
         OCINumberFromInt(stmt->handleError, buffer, sizeof(INT64), OCI_NUMBER_UNSIGNED, (OCINumber *)sqlBuffer);
         OCIBindByPos(stmt->handleStmt, &handleBind, stmt->handleError, pos, sqlBuffer, sizeof(OCINumber),
                      SQLT_VNU, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
         if (allocType == DB_BIND_DYNAMIC)
            free(buffer);
			break;
		default:
		   switch(allocType)
		   {
			   case DB_BIND_STATIC:
				   sqlBuffer = buffer;
				   break;
			   case DB_BIND_DYNAMIC:
				   sqlBuffer = buffer;
				   stmt->buffers->set(pos - 1, buffer);
				   break;
			   case DB_BIND_TRANSIENT:
				   sqlBuffer = nx_memdup(buffer, s_bufferSize[cType]);
				   stmt->buffers->set(pos - 1, sqlBuffer);
				   break;
			   default:
				   return;	// Invalid call
		   }
		   OCIBindByPos(stmt->handleStmt, &handleBind, stmt->handleError, pos, sqlBuffer, s_bufferSize[cType],
						    s_oracleType[cType], NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
			break;
	}

   stmt->bindings->set(pos - 1, handleBind);
}

/**
 * Batch bind - constructor
 */
OracleBatchBind::OracleBatchBind(int cType, int sqlType)
{
   m_cType = cType;
   m_size = 0;
   m_allocated = 256;
   if ((cType == DB_CTYPE_STRING) || (cType == DB_CTYPE_INT64) || (cType == DB_CTYPE_UINT64))
   {
      m_elementSize = sizeof(UCS2CHAR);
      m_string = true;
      m_oraType = (sqlType == DB_SQLTYPE_TEXT) ? SQLT_LNG : SQLT_STR;
      m_data = NULL;
      m_strings = (UCS2CHAR **)calloc(m_allocated, sizeof(UCS2CHAR *));
   }
   else
   {
      m_elementSize = s_bufferSize[cType];
      m_string = false;
      m_oraType = s_oracleType[cType];
      m_data = calloc(m_allocated, m_elementSize);
      m_strings = NULL;
   }
}

/**
 * Batch bind - destructor
 */
OracleBatchBind::~OracleBatchBind()
{
   if (m_strings != NULL)
   {
      for(int i = 0; i < m_size; i++)
         safe_free(m_strings[i]);
      free(m_strings);
   }
   safe_free(m_data);
}

/**
 * Batch bind - add row
 */
void OracleBatchBind::addRow()
{
   if (m_size == m_allocated)
   {
      m_allocated += 256;
      if (m_string)
      {
         m_strings = (UCS2CHAR **)realloc(m_strings, m_allocated * sizeof(UCS2CHAR *));
         memset(m_strings + m_size, 0, (m_allocated - m_size) * sizeof(UCS2CHAR *));
      }
      else
      {
         m_data = realloc(m_data, m_allocated * m_elementSize);
         memset((char *)m_data + m_size * m_elementSize, 0, (m_allocated - m_size) * m_elementSize);
      }
   }
   if (m_size > 0)
   {
      // clone last element
      if (m_string)
      {
         UCS2CHAR *p = m_strings[m_size - 1];
         m_strings[m_size] = (p != NULL) ? ucs2_strdup(p) : NULL;
      }
      else
      {
         memcpy((char *)m_data + m_size * m_elementSize, (char *)m_data + (m_size - 1) * m_elementSize, m_elementSize);
      }
   }
   m_size++;
}

/**
 * Batch bind - set value
 */
void OracleBatchBind::set(void *value)
{
   if (m_string)
   {
      safe_free(m_strings[m_size - 1]);
      m_strings[m_size - 1] = (UCS2CHAR *)value;
      if (value != NULL)
      {
         int l = (int)(ucs2_strlen((UCS2CHAR *)value) + 1) * sizeof(UCS2CHAR);
         if (l > m_elementSize)
            m_elementSize = l;
      }
   }
   else
   {
      memcpy((char *)m_data + (m_size - 1) * m_elementSize, value, m_elementSize);
   }
}

/**
 * Get data for OCI bind
 */
void *OracleBatchBind::getData()
{
   if (!m_string)
      return m_data;

   safe_free(m_data);
   m_data = calloc(m_size, m_elementSize);
   char *p = (char *)m_data;
   for(int i = 0; i < m_size; i++)
   {
      if (m_strings[i] == NULL)
         continue;
      memcpy(p, m_strings[i], ucs2_strlen(m_strings[i]) * sizeof(UCS2CHAR));
      p += m_elementSize;
   }
   return m_data;
}

/**
 * Bind parameter to statement - batch mode
 */
static void BindBatch(ORACLE_STATEMENT *stmt, int pos, int sqlType, int cType, void *buffer, int allocType)
{
   if (stmt->batchSize == 0)
      return;  // no batch rows added yet

	OracleBatchBind *bind = stmt->batchBindings->get(pos - 1);
   if (bind == NULL)
   {
      bind = new OracleBatchBind(cType, sqlType);
      stmt->batchBindings->set(pos - 1, bind);
      for(int i = 0; i < stmt->batchSize; i++)
         bind->addRow();
   }

   if (bind->getCType() != cType)
      return;

	void *sqlBuffer;
   switch(bind->getCType())
	{
		case DB_CTYPE_STRING:
#if UNICODE_UCS4
			sqlBuffer = UCS2StringFromUCS4String((WCHAR *)buffer);
         if (allocType == DB_BIND_DYNAMIC)
				free(buffer);
#else
         if (allocType == DB_BIND_DYNAMIC)
         {
				sqlBuffer = buffer;
         }
         else
			{
				sqlBuffer = wcsdup((WCHAR *)buffer);
			}
#endif
         bind->set(sqlBuffer);
			break;
      case DB_CTYPE_UTF8_STRING:
#if UNICODE_UCS4
         sqlBuffer = UCS2StringFromUTF8String((char *)buffer);
#else
         sqlBuffer = WideStringFromUTF8String((char *)buffer);
#endif
         if (allocType == DB_BIND_DYNAMIC)
            free(buffer);
         bind->set(sqlBuffer);
         break;
		case DB_CTYPE_INT64:	// OCI prior to 11.2 cannot bind 64 bit integers
#ifdef UNICODE_UCS2
			sqlBuffer = malloc(64 * sizeof(WCHAR));
			swprintf((WCHAR *)sqlBuffer, 64, INT64_FMTW, *((INT64 *)buffer));
#else
			{
				char temp[64];
				snprintf(temp, 64, INT64_FMTA, *((INT64 *)buffer));
				sqlBuffer = UCS2StringFromMBString(temp);
			}
#endif
         bind->set(sqlBuffer);
			if (allocType == DB_BIND_DYNAMIC)
				free(buffer);
			break;
		case DB_CTYPE_UINT64:	// OCI prior to 11.2 cannot bind 64 bit integers
#ifdef UNICODE_UCS2
			sqlBuffer = malloc(64 * sizeof(WCHAR));
			swprintf((WCHAR *)sqlBuffer, 64, UINT64_FMTW, *((QWORD *)buffer));
#else
			{
				char temp[64];
				snprintf(temp, 64, UINT64_FMTA, *((QWORD *)buffer));
				sqlBuffer = UCS2StringFromMBString(temp);
			}
#endif
         bind->set(sqlBuffer);
			if (allocType == DB_BIND_DYNAMIC)
				free(buffer);
			break;
		default:
         bind->set(buffer);
			if (allocType == DB_BIND_DYNAMIC)
				free(buffer);
			break;
	}
}

/**
 * Bind parameter to statement
 */
extern "C" void EXPORT DrvBind(ORACLE_STATEMENT *stmt, int pos, int sqlType, int cType, void *buffer, int allocType)
{
   if (stmt->batchMode)
      BindBatch(stmt, pos, sqlType, cType, buffer, allocType);
   else
      BindNormal(stmt, pos, sqlType, cType, buffer, allocType);
}

/**
 * Execute prepared non-select statement
 */
extern "C" DWORD EXPORT DrvExecute(ORACLE_CONN *pConn, ORACLE_STATEMENT *stmt, WCHAR *errorText)
{
	DWORD dwResult;

   if (stmt->batchMode)
   {
      if (stmt->batchSize == 0)
      {
         stmt->batchMode = false;
         stmt->batchBindings->clear();
         return DBERR_SUCCESS;   // empty batch
      }

      for(int i = 0; i < stmt->batchBindings->size(); i++)
      {
         OracleBatchBind *b = stmt->batchBindings->get(i);
         if (b == NULL)
            continue;

         OCIBind *handleBind = NULL;
         OCIBindByPos(stmt->handleStmt, &handleBind, stmt->handleError, i + 1, b->getData(), 
                      b->getElementSize(), b->getOraType(), NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
      }
   }

	MutexLock(pConn->mutexQueryLock);
   if (OCIStmtExecute(pConn->handleService, stmt->handleStmt, pConn->handleError, 
                      stmt->batchMode ? stmt->batchSize : 1, 0, NULL, NULL,
	                   (pConn->nTransLevel == 0) ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT) == OCI_SUCCESS)
	{
		dwResult = DBERR_SUCCESS;
	}
	else
	{
		SetLastError(pConn);
		dwResult = IsConnectionError(pConn);
	}

	if (errorText != NULL)
	{
		wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
		errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	}
	MutexUnlock(pConn->mutexQueryLock);

   if (stmt->batchMode)
   {
      stmt->batchMode = false;
      stmt->batchBindings->clear();
   }

	return dwResult;
}

/**
 * Destroy prepared statement
 */
extern "C" void EXPORT DrvFreeStatement(ORACLE_STATEMENT *stmt)
{
	if (stmt == NULL)
		return;

	MutexLock(stmt->connection->mutexQueryLock);
	OCIStmtRelease(stmt->handleStmt, stmt->handleError, NULL, 0, OCI_DEFAULT);
	OCIHandleFree(stmt->handleError, OCI_HTYPE_ERROR);
	MutexUnlock(stmt->connection->mutexQueryLock);
	delete stmt->bindings;
   delete stmt->batchBindings;
	delete stmt->buffers;
	free(stmt);
}

/**
 * Perform non-SELECT query
 */
static DWORD DrvQueryInternal(ORACLE_CONN *pConn, const WCHAR *pwszQuery, WCHAR *errorText)
{
	OCIStmt *handleStmt;
	DWORD dwResult;

#if UNICODE_UCS4
	UCS2CHAR *ucs2Query = UCS2StringFromUCS4String(pwszQuery);
#else
	const UCS2CHAR *ucs2Query = pwszQuery;
#endif

	MutexLock(pConn->mutexQueryLock);
	if (OCIStmtPrepare2(pConn->handleService, &handleStmt, pConn->handleError, (text *)ucs2Query,
	                    (ub4)ucs2_strlen(ucs2Query) * sizeof(UCS2CHAR), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
	{
		if (OCIStmtExecute(pConn->handleService, handleStmt, pConn->handleError, 1, 0, NULL, NULL,
		                   (pConn->nTransLevel == 0) ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT) == OCI_SUCCESS)
		{
			dwResult = DBERR_SUCCESS;
		}
		else
		{
			SetLastError(pConn);
			dwResult = IsConnectionError(pConn);
		}
		OCIStmtRelease(handleStmt, pConn->handleError, NULL, 0, OCI_DEFAULT);
	}
	else
	{
		SetLastError(pConn);
		dwResult = IsConnectionError(pConn);
	}
	if (errorText != NULL)
	{
		wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
		errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	}
	MutexUnlock(pConn->mutexQueryLock);

#if UNICODE_UCS4
	free(ucs2Query);
#endif
	return dwResult;
}

/**
 * Perform non-SELECT query - entry point
 */
extern "C" DWORD EXPORT DrvQuery(ORACLE_CONN *conn, const WCHAR *query, WCHAR *errorText)
{
   return DrvQueryInternal(conn, query, errorText);
}

/**
 * Get column name from parameter handle
 *
 * OCI library has memory leak when retrieving column name in UNICODE mode
 * Driver attempts to use workaround described in https://github.com/vrogier/ocilib/issues/31
 * (accessing internal OCI structure directly to avoid conversion to UTF-16 by OCI library)
 */
static char *GetColumnName(OCIParam *handleParam, OCIError *handleError)
{
   if ((s_ociVersionMajor == 11) || (s_ociVersionMajor == 12))
   {
      OCI_PARAM_STRUCT *p = (OCI_PARAM_STRUCT *)handleParam;
      if ((p->columnInfo != NULL) && (p->columnInfo->name != NULL) && (p->columnInfo->attributes[1] != 0))
      {
         size_t len = p->columnInfo->attributes[1];
         char *n = (char *)malloc(len + 1);
         memcpy(n, p->columnInfo->name, len);
         n[len] = 0;
         return n;
      }
   }

   // Use standard method as fallback
   text *colName;
   ub4 size;
   if (OCIAttrGet(handleParam, OCI_DTYPE_PARAM, &colName, &size, OCI_ATTR_NAME, handleError) == OCI_SUCCESS)
   {
      // We are in UTF-16 mode, so OCIAttrGet will return UTF-16 strings
      return MBStringFromUCS2String((UCS2CHAR *)colName);
   }
   else
   {
      return strdup("");
   }
}

/**
 * Process SELECT results
 */
static ORACLE_RESULT *ProcessQueryResults(ORACLE_CONN *pConn, OCIStmt *handleStmt, DWORD *pdwError)
{
	OCIParam *handleParam;
	OCIDefine *handleDefine;
	ub4 nCount;
	ub2 nWidth;
	sword nStatus;
	ORACLE_FETCH_BUFFER *pBuffers;

	ORACLE_RESULT *pResult = (ORACLE_RESULT *)malloc(sizeof(ORACLE_RESULT));
	pResult->nRows = 0;
	pResult->pData = NULL;
	pResult->columnNames = NULL;
	OCIAttrGet(handleStmt, OCI_HTYPE_STMT, &nCount, NULL, OCI_ATTR_PARAM_COUNT, pConn->handleError);
	pResult->nCols = nCount;
	if (pResult->nCols > 0)
	{
		// Prepare receive buffers and fetch column names
		pResult->columnNames = (char **)calloc(pResult->nCols, sizeof(char *));
		pBuffers = (ORACLE_FETCH_BUFFER *)calloc(pResult->nCols, sizeof(ORACLE_FETCH_BUFFER));
		for(int i = 0; i < pResult->nCols; i++)
		{
			if ((nStatus = OCIParamGet(handleStmt, OCI_HTYPE_STMT, pConn->handleError,
			                           (void **)&handleParam, (ub4)(i + 1))) == OCI_SUCCESS)
			{
				// Column name
            pResult->columnNames[i] = GetColumnName(handleParam, pConn->handleError);

				// Data size
            ub2 type = 0;
				OCIAttrGet(handleParam, OCI_DTYPE_PARAM, &type, NULL, OCI_ATTR_DATA_TYPE, pConn->handleError);
            if (type == OCI_TYPECODE_CLOB)
            {
               pBuffers[i].pData = NULL;
               OCIDescriptorAlloc(s_handleEnv, (void **)&pBuffers[i].lobLocator, OCI_DTYPE_LOB, 0, NULL);
				   handleDefine = NULL;
				   nStatus = OCIDefineByPos(handleStmt, &handleDefine, pConn->handleError, i + 1,
                                        &pBuffers[i].lobLocator, 0, SQLT_CLOB, &pBuffers[i].isNull, 
                                        NULL, NULL, OCI_DEFAULT);
            }
            else
            {
               pBuffers[i].lobLocator = NULL;
				   OCIAttrGet(handleParam, OCI_DTYPE_PARAM, &nWidth, NULL, OCI_ATTR_DATA_SIZE, pConn->handleError);
				   pBuffers[i].pData = (UCS2CHAR *)malloc((nWidth + 31) * sizeof(UCS2CHAR));
				   handleDefine = NULL;
				   nStatus = OCIDefineByPos(handleStmt, &handleDefine, pConn->handleError, i + 1,
                                        pBuffers[i].pData, (nWidth + 31) * sizeof(UCS2CHAR),
				                            SQLT_CHR, &pBuffers[i].isNull, &pBuffers[i].nLength,
				                            &pBuffers[i].nCode, OCI_DEFAULT);
            }
            if (nStatus != OCI_SUCCESS)
			   {
				   SetLastError(pConn);
				   *pdwError = IsConnectionError(pConn);
			   }
				OCIDescriptorFree(handleParam, OCI_DTYPE_PARAM);
			}
			else
			{
				SetLastError(pConn);
				*pdwError = IsConnectionError(pConn);
			}
		}

		// Fetch data
		if (nStatus == OCI_SUCCESS)
		{
			int nPos = 0;
			while(1)
			{
				nStatus = OCIStmtFetch2(handleStmt, pConn->handleError, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
				if (nStatus == OCI_NO_DATA)
				{
					*pdwError = DBERR_SUCCESS;	// EOF
					break;
				}
				if ((nStatus != OCI_SUCCESS) && (nStatus != OCI_SUCCESS_WITH_INFO))
				{
					SetLastError(pConn);
					*pdwError = IsConnectionError(pConn);
					break;
				}

				// New row
				pResult->nRows++;
				pResult->pData = (WCHAR **)realloc(pResult->pData, sizeof(WCHAR *) * pResult->nCols * pResult->nRows);
				for(int i = 0; i < pResult->nCols; i++)
				{
					if (pBuffers[i].isNull)
					{
						pResult->pData[nPos] = (WCHAR *)nx_memdup("\0\0\0", sizeof(WCHAR));
					}
               else if (pBuffers[i].lobLocator != NULL)
               {
                  ub4 length = 0;
                  ub4 amount = length;
                  OCILobGetLength(pConn->handleService, pConn->handleError, pBuffers[i].lobLocator, &length);
						pResult->pData[nPos] = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));
#if UNICODE_UCS4
                  UCS2CHAR *ucs2buffer = (UCS2CHAR *)malloc(sizeof(UCS2CHAR) * length);
                  OCILobRead(pConn->handleService, pConn->handleError, pBuffers[i].lobLocator, &amount, 1, 
                             ucs2buffer, length * sizeof(UCS2CHAR), NULL, NULL, OCI_UCS2ID, SQLCS_IMPLICIT);
						ucs2_to_ucs4(ucs2buffer, length, pResult->pData[nPos], length + 1);
                  free(ucs2buffer);
#else
                  OCILobRead(pConn->handleService, pConn->handleError, pBuffers[i].lobLocator, &amount, 1, 
                             pResult->pData[nPos], (length + 1) * sizeof(WCHAR), NULL, NULL, OCI_UCS2ID, SQLCS_IMPLICIT);
#endif
						pResult->pData[nPos][length] = 0;
               }
					else
					{
						int length = pBuffers[i].nLength / sizeof(UCS2CHAR);
						pResult->pData[nPos] = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));
#if UNICODE_UCS4
						ucs2_to_ucs4(pBuffers[i].pData, length, pResult->pData[nPos], length + 1);
#else
						memcpy(pResult->pData[nPos], pBuffers[i].pData, pBuffers[i].nLength);
#endif
						pResult->pData[nPos][length] = 0;
					}
					nPos++;
				}
			}
		}

		// Cleanup
		for(int i = 0; i < pResult->nCols; i++)
      {
			free(pBuffers[i].pData);
         if (pBuffers[i].lobLocator != NULL)
         {
            OCIDescriptorFree(pBuffers[i].lobLocator, OCI_DTYPE_LOB);
         }
      }
		free(pBuffers);

		// Destroy results in case of error
		if (*pdwError != DBERR_SUCCESS)
		{
			DestroyQueryResult(pResult);
			pResult = NULL;
		}
	}

	return pResult;
}

/**
 * Perform SELECT query
 */
extern "C" DBDRV_RESULT EXPORT DrvSelect(ORACLE_CONN *pConn, WCHAR *pwszQuery, DWORD *pdwError, WCHAR *errorText)
{
	ORACLE_RESULT *pResult = NULL;
	OCIStmt *handleStmt;

#if UNICODE_UCS4
	UCS2CHAR *ucs2Query = UCS2StringFromUCS4String(pwszQuery);
#else
	UCS2CHAR *ucs2Query = pwszQuery;
#endif

	MutexLock(pConn->mutexQueryLock);
	if (OCIStmtPrepare2(pConn->handleService, &handleStmt, pConn->handleError, (text *)ucs2Query,
	                    (ub4)ucs2_strlen(ucs2Query) * sizeof(UCS2CHAR), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
	{
      OCIAttrSet(handleStmt, OCI_HTYPE_STMT, &pConn->prefetchLimit, 0, OCI_ATTR_PREFETCH_ROWS, pConn->handleError);
		if (OCIStmtExecute(pConn->handleService, handleStmt, pConn->handleError,
		                   0, 0, NULL, NULL, (pConn->nTransLevel == 0) ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT) == OCI_SUCCESS)
		{
			pResult = ProcessQueryResults(pConn, handleStmt, pdwError);
		}
		else
		{
			SetLastError(pConn);
			*pdwError = IsConnectionError(pConn);
		}
		OCIStmtRelease(handleStmt, pConn->handleError, NULL, 0, OCI_DEFAULT);
	}
	else
	{
		SetLastError(pConn);
		*pdwError = IsConnectionError(pConn);
	}
	if (errorText != NULL)
	{
		wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
		errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	}
	MutexUnlock(pConn->mutexQueryLock);

#if UNICODE_UCS4
	free(ucs2Query);
#endif
	return pResult;
}

/**
 * Perform SELECT query using prepared statement
 */
extern "C" DBDRV_RESULT EXPORT DrvSelectPrepared(ORACLE_CONN *pConn, ORACLE_STATEMENT *stmt, DWORD *pdwError, WCHAR *errorText)
{
	ORACLE_RESULT *pResult = NULL;

	MutexLock(pConn->mutexQueryLock);
   OCIAttrSet(stmt->handleStmt, OCI_HTYPE_STMT, &pConn->prefetchLimit, 0, OCI_ATTR_PREFETCH_ROWS, pConn->handleError);
	if (OCIStmtExecute(pConn->handleService, stmt->handleStmt, pConn->handleError,
	                   0, 0, NULL, NULL, (pConn->nTransLevel == 0) ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT) == OCI_SUCCESS)
	{
		pResult = ProcessQueryResults(pConn, stmt->handleStmt, pdwError);
	}
	else
	{
		SetLastError(pConn);
		*pdwError = IsConnectionError(pConn);
	}

	if (errorText != NULL)
	{
		wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
		errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	}
	MutexUnlock(pConn->mutexQueryLock);

	return pResult;
}

/**
 * Get field length from result
 */
extern "C" LONG EXPORT DrvGetFieldLength(ORACLE_RESULT *pResult, int nRow, int nColumn)
{
	if (pResult == NULL)
		return -1;

	if ((nRow >= 0) && (nRow < pResult->nRows) &&
		 (nColumn >= 0) && (nColumn < pResult->nCols))
		return (LONG)wcslen(pResult->pData[pResult->nCols * nRow + nColumn]);
	
	return -1;
}

/**
 * Get field value from result
 */
extern "C" WCHAR EXPORT *DrvGetField(ORACLE_RESULT *pResult, int nRow, int nColumn,
                                     WCHAR *pBuffer, int nBufLen)
{
   WCHAR *pValue = NULL;

   if (pResult != NULL)
   {
      if ((nRow < pResult->nRows) && (nRow >= 0) &&
          (nColumn < pResult->nCols) && (nColumn >= 0))
      {
#ifdef _WIN32
         wcsncpy_s(pBuffer, nBufLen, pResult->pData[nRow * pResult->nCols + nColumn], _TRUNCATE);
#else
         wcsncpy(pBuffer, pResult->pData[nRow * pResult->nCols + nColumn], nBufLen);
         pBuffer[nBufLen - 1] = 0;
#endif
         pValue = pBuffer;
      }
   }
   return pValue;
}

/**
 * Get number of rows in result
 */
extern "C" int EXPORT DrvGetNumRows(ORACLE_RESULT *pResult)
{
	return (pResult != NULL) ? pResult->nRows : 0;
}

/**
 * Get column count in query result
 */
extern "C" int EXPORT DrvGetColumnCount(ORACLE_RESULT *pResult)
{
	return (pResult != NULL) ? pResult->nCols : 0;
}

/**
 * Get column name in query result
 */
extern "C" const char EXPORT *DrvGetColumnName(ORACLE_RESULT *pResult, int column)
{
	return ((pResult != NULL) && (column >= 0) && (column < pResult->nCols)) ? pResult->columnNames[column] : NULL;
}

/**
 * Free SELECT results
 */
extern "C" void EXPORT DrvFreeResult(ORACLE_RESULT *pResult)
{
	if (pResult != NULL)
		DestroyQueryResult(pResult);
}

/**
 * Destroy unbuffered query result
 */
static void DestroyUnbufferedQueryResult(ORACLE_UNBUFFERED_RESULT *result, bool freeStatement)
{
   int i;

   if (freeStatement)
      OCIStmtRelease(result->handleStmt, result->connection->handleError, NULL, 0, OCI_DEFAULT);

   for(i = 0; i < result->nCols; i++)
   {
      free(result->pBuffers[i].pData);
      if (result->pBuffers[i].lobLocator != NULL)
      {
         OCIDescriptorFree(result->pBuffers[i].lobLocator, OCI_DTYPE_LOB);
      }
   }
   free(result->pBuffers);

   for(i = 0; i < result->nCols; i++)
      free(result->columnNames[i]);
   free(result->columnNames);

   free(result);
}

/**
 * Process results of unbuffered query execution (prepare for fetching results)
 */
static ORACLE_UNBUFFERED_RESULT *ProcessUnbufferedQueryResults(ORACLE_CONN *pConn, OCIStmt *handleStmt, DWORD *pdwError)
{
   ORACLE_UNBUFFERED_RESULT *result = (ORACLE_UNBUFFERED_RESULT *)malloc(sizeof(ORACLE_UNBUFFERED_RESULT));
   result->handleStmt = handleStmt;
   result->connection = pConn;

   ub4 nCount;
   OCIAttrGet(result->handleStmt, OCI_HTYPE_STMT, &nCount, NULL, OCI_ATTR_PARAM_COUNT, pConn->handleError);
   result->nCols = nCount;
   if (result->nCols > 0)
   {
      // Prepare receive buffers and fetch column names
      result->columnNames = (char **)calloc(result->nCols, sizeof(char *));
      result->pBuffers = (ORACLE_FETCH_BUFFER *)calloc(result->nCols, sizeof(ORACLE_FETCH_BUFFER));
      for(int i = 0; i < result->nCols; i++)
      {
         OCIParam *handleParam;

         result->pBuffers[i].isNull = 1;   // Mark all columns as NULL initially
         if (OCIParamGet(result->handleStmt, OCI_HTYPE_STMT, pConn->handleError,
                         (void **)&handleParam, (ub4)(i + 1)) == OCI_SUCCESS)
         {
            // Column name
            result->columnNames[i] = GetColumnName(handleParam, pConn->handleError);

            // Data size
            sword nStatus;
            ub2 type = 0;
            OCIAttrGet(handleParam, OCI_DTYPE_PARAM, &type, NULL, OCI_ATTR_DATA_TYPE, pConn->handleError);
            OCIDefine *handleDefine;
            if (type == OCI_TYPECODE_CLOB)
            {
               result->pBuffers[i].pData = NULL;
               OCIDescriptorAlloc(s_handleEnv, (void **)&result->pBuffers[i].lobLocator, OCI_DTYPE_LOB, 0, NULL);
               handleDefine = NULL;
               nStatus = OCIDefineByPos(result->handleStmt, &handleDefine, pConn->handleError, i + 1,
                                        &result->pBuffers[i].lobLocator, 0, SQLT_CLOB, &result->pBuffers[i].isNull,
                                        NULL, NULL, OCI_DEFAULT);
            }
            else
            {
               ub2 nWidth;
               result->pBuffers[i].lobLocator = NULL;
               OCIAttrGet(handleParam, OCI_DTYPE_PARAM, &nWidth, NULL, OCI_ATTR_DATA_SIZE, pConn->handleError);
               result->pBuffers[i].pData = (UCS2CHAR *)malloc((nWidth + 31) * sizeof(UCS2CHAR));
               handleDefine = NULL;
               nStatus = OCIDefineByPos(result->handleStmt, &handleDefine, pConn->handleError, i + 1,
                                        result->pBuffers[i].pData, (nWidth + 31) * sizeof(UCS2CHAR),
                                        SQLT_CHR, &result->pBuffers[i].isNull, &result->pBuffers[i].nLength,
                                        &result->pBuffers[i].nCode, OCI_DEFAULT);
            }
            OCIDescriptorFree(handleParam, OCI_DTYPE_PARAM);
            if (nStatus == OCI_SUCCESS)
            {
               *pdwError = DBERR_SUCCESS;
            }
            else
            {
               SetLastError(pConn);
               *pdwError = IsConnectionError(pConn);
               DestroyUnbufferedQueryResult(result, false);
               result = NULL;
               break;
            }
         }
         else
         {
            SetLastError(pConn);
            *pdwError = IsConnectionError(pConn);
            DestroyUnbufferedQueryResult(result, false);
            result = NULL;
            break;
         }
      }
   }
   else
   {
      free(result);
      result = NULL;
   }

   return result;
}

/**
 * Perform unbuffered SELECT query
 */
extern "C" DBDRV_UNBUFFERED_RESULT EXPORT DrvSelectUnbuffered(ORACLE_CONN *pConn, WCHAR *pwszQuery, DWORD *pdwError, WCHAR *errorText, UINT32 mode)
{
   ORACLE_UNBUFFERED_RESULT *result = NULL;

#if UNICODE_UCS4
	UCS2CHAR *ucs2Query = UCS2StringFromUCS4String(pwszQuery);
#else
	UCS2CHAR *ucs2Query = pwszQuery;
#endif

	MutexLock(pConn->mutexQueryLock);

	OCIStmt *handleStmt;
	if (OCIStmtPrepare2(pConn->handleService, &handleStmt, pConn->handleError, (text *)ucs2Query,
	                    (ub4)ucs2_strlen(ucs2Query) * sizeof(UCS2CHAR), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
	{
      OCIAttrSet(handleStmt, OCI_HTYPE_STMT, &pConn->prefetchLimit, 0, OCI_ATTR_PREFETCH_ROWS, pConn->handleError);
		if (OCIStmtExecute(pConn->handleService, handleStmt, pConn->handleError,
		                   0, 0, NULL, NULL, (pConn->nTransLevel == 0) ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT) == OCI_SUCCESS)
		{
		   result = ProcessUnbufferedQueryResults(pConn, handleStmt, pdwError);
		}
		else
		{
			SetLastError(pConn);
			*pdwError = IsConnectionError(pConn);
		}
	}
	else
	{
		SetLastError(pConn);
		*pdwError = IsConnectionError(pConn);
	}

#if UNICODE_UCS4
	free(ucs2Query);
#endif

	if ((*pdwError == DBERR_SUCCESS) && (result != NULL))
		return result;

	// On failure, unlock query mutex and do cleanup
	OCIStmtRelease(handleStmt, pConn->handleError, NULL, 0, OCI_DEFAULT);
	if (errorText != NULL)
	{
		wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
		errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	}
	MutexUnlock(pConn->mutexQueryLock);
	return NULL;
}

/**
 * Perform SELECT query using prepared statement
 */
extern "C" DBDRV_UNBUFFERED_RESULT EXPORT DrvSelectPreparedUnbuffered(ORACLE_CONN *pConn, ORACLE_STATEMENT *stmt, DWORD *pdwError, WCHAR *errorText)
{
   ORACLE_UNBUFFERED_RESULT *result = NULL;

   MutexLock(pConn->mutexQueryLock);

   OCIAttrSet(stmt->handleStmt, OCI_HTYPE_STMT, &pConn->prefetchLimit, 0, OCI_ATTR_PREFETCH_ROWS, pConn->handleError);
   if (OCIStmtExecute(pConn->handleService, stmt->handleStmt, pConn->handleError,
                      0, 0, NULL, NULL, (pConn->nTransLevel == 0) ? OCI_COMMIT_ON_SUCCESS : OCI_DEFAULT) == OCI_SUCCESS)
   {
      result = ProcessUnbufferedQueryResults(pConn, stmt->handleStmt, pdwError);
   }
   else
   {
      SetLastError(pConn);
      *pdwError = IsConnectionError(pConn);
   }

   if ((*pdwError == DBERR_SUCCESS) && (result != NULL))
      return result;

   // On failure, unlock query mutex and do cleanup
   if (errorText != NULL)
   {
      wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
      errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
   }
   MutexUnlock(pConn->mutexQueryLock);
   return NULL;
}

/**
 * Fetch next result line from unbuffered SELECT results
 */
extern "C" bool EXPORT DrvFetch(ORACLE_UNBUFFERED_RESULT *result)
{
	bool success;

	if (result == NULL)
		return false;

	sword rc = OCIStmtFetch2(result->handleStmt, result->connection->handleError, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
	if ((rc == OCI_SUCCESS) || (rc == OCI_SUCCESS_WITH_INFO))
	{
		success = true;
	}
	else
	{
		SetLastError(result->connection);
		success = false;
	}
	return success;
}

/**
 * Get field length from current row in unbuffered query result
 */
extern "C" LONG EXPORT DrvGetFieldLengthUnbuffered(ORACLE_UNBUFFERED_RESULT *result, int nColumn)
{
	if (result == NULL)
		return 0;

	if ((nColumn < 0) || (nColumn >= result->nCols))
		return 0;

	if (result->pBuffers[nColumn].isNull)
		return 0;

   if (result->pBuffers[nColumn].lobLocator != NULL)
   {
      ub4 length = 0;
      OCILobGetLength(result->connection->handleService, result->connection->handleError, result->pBuffers[nColumn].lobLocator, &length);
      return (LONG)length;
   }

	return (LONG)(result->pBuffers[nColumn].nLength / sizeof(UCS2CHAR));
}

/**
 * Get field from current row in unbuffered query result
 */
extern "C" WCHAR EXPORT *DrvGetFieldUnbuffered(ORACLE_UNBUFFERED_RESULT *result, int nColumn, WCHAR *pBuffer, int nBufSize)
{
	int nLen;

	if (result == NULL)
		return NULL;

	if ((nColumn < 0) || (nColumn >= result->nCols))
		return NULL;

	if (result->pBuffers[nColumn].isNull)
	{
		*pBuffer = 0;
	}
   else if (result->pBuffers[nColumn].lobLocator != NULL)
   {
      ub4 length = 0;
      OCILobGetLength(result->connection->handleService, result->connection->handleError, result->pBuffers[nColumn].lobLocator, &length);

		nLen = min(nBufSize - 1, (int)length);
      ub4 amount = nLen;
#if UNICODE_UCS4
      UCS2CHAR *ucs2buffer = (UCS2CHAR *)malloc(nLen * sizeof(UCS2CHAR));
      OCILobRead(result->connection->handleService, result->connection->handleError, result->pBuffers[nColumn].lobLocator, &amount, 1,
                 ucs2buffer, nLen * sizeof(UCS2CHAR), NULL, NULL, OCI_UCS2ID, SQLCS_IMPLICIT);
		ucs2_to_ucs4(ucs2buffer, nLen, pBuffer, nLen);
      free(ucs2buffer);
#else
      OCILobRead(result->connection->handleService, result->connection->handleError, result->pBuffers[nColumn].lobLocator, &amount, 1,
                 pBuffer, nBufSize * sizeof(WCHAR), NULL, NULL, OCI_UCS2ID, SQLCS_IMPLICIT);
#endif
		pBuffer[nLen] = 0;
   }
	else
	{
		nLen = min(nBufSize - 1, ((int)(result->pBuffers[nColumn].nLength / sizeof(UCS2CHAR))));
#if UNICODE_UCS4
		ucs2_to_ucs4(result->pBuffers[nColumn].pData, nLen, pBuffer, nLen + 1);
#else
		memcpy(pBuffer, result->pBuffers[nColumn].pData, nLen * sizeof(WCHAR));
#endif
		pBuffer[nLen] = 0;
	}

	return pBuffer;
}

/**
 * Get column count in unbuffered query result
 */
extern "C" int EXPORT DrvGetColumnCountUnbuffered(ORACLE_UNBUFFERED_RESULT *result)
{
	return (result != NULL) ? result->nCols : 0;
}

/**
 * Get column name in unbuffered query result
 */
extern "C" const char EXPORT *DrvGetColumnNameUnbuffered(ORACLE_UNBUFFERED_RESULT *result, int column)
{
	return ((result != NULL) && (column >= 0) && (column < result->nCols)) ? result->columnNames[column] : NULL;
}

/**
 * Destroy result of unbuffered query
 */
extern "C" void EXPORT DrvFreeUnbufferedResult(ORACLE_UNBUFFERED_RESULT *result)
{
	if (result == NULL)
		return;

	MUTEX mutex = result->connection->mutexQueryLock;
	DestroyUnbufferedQueryResult(result, true);
	MutexUnlock(mutex);
}

/**
 * Begin transaction
 */
extern "C" DWORD EXPORT DrvBegin(ORACLE_CONN *pConn)
{
	if (pConn == NULL)
		return DBERR_INVALID_HANDLE;

	MutexLock(pConn->mutexQueryLock);
	pConn->nTransLevel++;
	MutexUnlock(pConn->mutexQueryLock);
	return DBERR_SUCCESS;
}

/**
 * Commit transaction
 */
extern "C" DWORD EXPORT DrvCommit(ORACLE_CONN *pConn)
{
	DWORD dwResult;

	if (pConn == NULL)
		return DBERR_INVALID_HANDLE;

	MutexLock(pConn->mutexQueryLock);
	if (pConn->nTransLevel > 0)
	{
		if (OCITransCommit(pConn->handleService, pConn->handleError, OCI_DEFAULT) == OCI_SUCCESS)
		{
			dwResult = DBERR_SUCCESS;
			pConn->nTransLevel = 0;
		}
		else
		{
			SetLastError(pConn);
			dwResult = IsConnectionError(pConn);
		}
	}
	else
	{
		dwResult = DBERR_SUCCESS;
	}
	MutexUnlock(pConn->mutexQueryLock);
	return dwResult;
}

/**
 * Rollback transaction
 */
extern "C" DWORD EXPORT DrvRollback(ORACLE_CONN *pConn)
{
	DWORD dwResult;

	if (pConn == NULL)
		return DBERR_INVALID_HANDLE;

	MutexLock(pConn->mutexQueryLock);
	if (pConn->nTransLevel > 0)
	{
		if (OCITransRollback(pConn->handleService, pConn->handleError, OCI_DEFAULT) == OCI_SUCCESS)
		{
			dwResult = DBERR_SUCCESS;
			pConn->nTransLevel = 0;
		}
		else
		{
			SetLastError(pConn);
			dwResult = IsConnectionError(pConn);
		}
	}
	else
	{
		dwResult = DBERR_SUCCESS;
	}
	MutexUnlock(pConn->mutexQueryLock);
	return dwResult;
}

/**
 * Check if table exist
 */
extern "C" int EXPORT DrvIsTableExist(ORACLE_CONN *pConn, const WCHAR *name)
{
   WCHAR query[256];
   swprintf(query, 256, L"SELECT count(*) FROM user_tables WHERE table_name=upper('%ls')", name);
   DWORD error;
   WCHAR errorText[DBDRV_MAX_ERROR_TEXT];
   int rc = DBIsTableExist_Failure;
   ORACLE_RESULT *hResult = (ORACLE_RESULT *)DrvSelect(pConn, query, &error, errorText);
   if (hResult != NULL)
   {
      WCHAR buffer[64] = L"";
      DrvGetField(hResult, 0, 0, buffer, 64);
      rc = (wcstol(buffer, NULL, 10) > 0) ? DBIsTableExist_Found : DBIsTableExist_NotFound;
      DrvFreeResult(hResult);
   }
   return rc;
}

#ifdef _WIN32

/**
 * DLL Entry point
 */
bool WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
	if (dwReason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(hInstance);
	return true;
}

#endif
