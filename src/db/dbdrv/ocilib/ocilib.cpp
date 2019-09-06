/* 
** OCILib Database Driver
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
** File: ocilib.cpp
**
**/
#include "ocilibdrv.h"

DECLARE_DRIVER_HEADER("OCILIB")

static DWORD DrvQueryInternal(ORACLE_CONN *pConn, const WCHAR *pwszQuery, WCHAR *errorText);

void err_handler(OCI_Error *err)
{
	nxlog_debug(0, _T("[ORA-%05i][%s][%s]"), OCI_ErrorGetOCICode(err), OCI_ErrorGetString(err), OCI_GetSql(OCI_ErrorGetStatement(err)));
}

/**
 * Global environment handle
 */
static OCIEnv *s_handleEnv = NULL;

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
	if(OCI_Initialize(NULL, NULL, OCI_THREADED | OCI_NCHAR_LITERAL_REPLACE_OFF | OCI_ENV_CONTEXT) != true)
		return false;
	else
		s_handleEnv = (OCIEnv*)OCI_HandleGetEnvironment();

	nxlog_debug(0, _T("OCILIB: version %d.%d.%d, charset : %s"), OCILIB_MAJOR_VERSION, OCILIB_MINOR_VERSION, OCILIB_REVISION_VERSION, (OCI_GetCharset() == OCI_CHAR_ANSI) ? _T("ANSI") : _T("UNICODE"));

	if(s_handleEnv == NULL)
		return false;

	OCI_EnableWarnings(TRUE);

	return true;
}

/**
 * Unload handler
 */
extern "C" void EXPORT DrvUnload()
{
	if(s_handleEnv != NULL)
		OCI_Cleanup();
}

/**
 * Get error text from error handle
 */
static void GetErrorFromHandle(sb4 *errorCode, WCHAR *errorText)
{
	OCI_Error *handle = OCI_GetLastError();

#if UNICODE_UCS2
	errorCode = OCI_ErrorGetOCICode(handle);
	errorText = OCI_ErrorGetString(handle);

	ucs4_to_ucs2(OCI_ErrorGetString(handle) , ucs4_strlen(OCI_ErrorGetString(handle)) + 1, errorText, DBDRV_MAX_ERROR_TEXT);
	errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
#else
	if(handle != NULL)
		wcsncpy(errorText, OCI_ErrorGetString(handle), DBDRV_MAX_ERROR_TEXT);
#endif
	RemoveTrailingCRLFW(errorText);
}

/**
 * Set last error text
 */
static void SetLastError(ORACLE_CONN *pConn)
{
	GetErrorFromHandle(&pConn->lastErrorCode, pConn->lastErrorText);
}

/**
 * Check if last error was cause by lost connection to server
 */
static DWORD IsConnectionError(ORACLE_CONN *pConn)
{
	bool nStatus = OCI_IsConnected(pConn->handleConnection);
	return (nStatus == true) ? DBERR_OTHER_ERROR : DBERR_CONNECTION_LOST;
}

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
	OCI_Connection *OCIConn;
	UCS4CHAR *szHost, *szLogin, *szPassword;

	pConn = (ORACLE_CONN *)calloc(1, sizeof(ORACLE_CONN));

	if(pConn != NULL)
	{
		szHost = UCS4StringFromMBString(host);
		szLogin = UCS4StringFromMBString(login);
		szPassword = UCS4StringFromMBString(password);

		OCIConn = OCI_ConnectionCreate(szHost, szLogin, szPassword, OCI_SESSION_DEFAULT);

		free(szHost);
		free(szLogin);
		free(szPassword);

		if(OCIConn && OCI_IsConnected(OCIConn))
		{
		    OCI_SetAutoCommit(OCIConn, true);

		    pConn->handleConnection = OCIConn;
		    pConn->handleServer = (OCIServer*)OCI_HandleGetServer(OCIConn);
		    pConn->handleSession = (OCISession*)OCI_HandleGetSession(OCIConn);
		    pConn->handleService = (OCISvcCtx*)OCI_HandleGetContext(OCIConn);
		    pConn->handleError = (OCIError*)OCI_HandleGetError(OCIConn);
		    pConn->mutexQueryLock = MutexCreate();
		    pConn->nTransLevel = 0;
		    pConn->lastErrorCode = 0;
		    pConn->lastErrorText[0] = 0;
		    pConn->prefetchLimit = 0;

		    OCI_SetStatementCacheSize(pConn->handleConnection, pConn->prefetchLimit);

 		    DrvQueryInternal(pConn, _T("ALTER SESSION SET NLS_LANGUAGE='AMERICAN' NLS_NUMERIC_CHARACTERS='.,'"), NULL);
 		    nxlog_debug(5, _T("ORACLE: connected to %i.%i.%i"), OCI_GetServerMajorVersion(OCIConn), OCI_GetServerMinorVersion(OCIConn), OCI_GetServerRevisionVersion(OCIConn));
		}
		else
		{
			GetErrorFromHandle(&pConn->lastErrorCode, errorText);
			pConn = NULL;
		}
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
	if(pConn == NULL)
		return;

	OCI_ConnectionFree(pConn->handleConnection);
	MutexDestroy(pConn->mutexQueryLock);
	free(pConn);
}


/**
 * Set prefetch limit
 */
extern "C" void EXPORT DrvSetPrefetchLimit(ORACLE_CONN *pConn, int limit)
{
	if(pConn != NULL)
		pConn->prefetchLimit = limit;
}

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
	OCI_Statement *handleStmt = OCI_StatementCreate(pConn->handleConnection);

	UCS2CHAR *ucs2Query = ConvertQuery(pwszQuery);
	WCHAR *wcs2Query = UCS4StringFromUCS2String(ucs2Query);

	MutexLock(pConn->mutexQueryLock);
	OCI_AllowRebinding(handleStmt, true);
	if(OCI_Prepare(handleStmt, (otext*)wcs2Query) == true)
	{	
		stmt = (ORACLE_STATEMENT*)malloc(sizeof(ORACLE_STATEMENT));
		stmt->connection = pConn;
		stmt->handleStmt = handleStmt;
		stmt->bindings = new Array(8, 8, false);
		stmt->lobBinds = new Array(8, 8, false);
		stmt->batchBindings = NULL;
		stmt->buffers = new Array(8, 8, true);
		stmt->batchMode = false;
		stmt->batchSize = 0;
		*pdwError = DBERR_SUCCESS;
	}
	else
	{
		SetLastError(pConn);
		*pdwError = IsConnectionError(pConn);
	}
	free(wcs2Query);
	free(ucs2Query);

	if(errorText != NULL)
	{
		wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
		errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	}
	MutexUnlock(pConn->mutexQueryLock);

	return stmt;
}

/**
 * Open batch
 */
extern "C" bool EXPORT DrvOpenBatch(ORACLE_STATEMENT *stmt)
{
	stmt->buffers->clear();
	if(stmt->batchBindings != NULL)
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
	if(!stmt->batchMode)
		return;

	for(int i = 0; i < stmt->batchBindings->size(); i++)
	{
		OracleBatchBind *bind = stmt->batchBindings->get(i);

		if(bind != NULL)
			bind->addRow();
	}
	stmt->batchSize++;
}

/**
 * Buffer sizes for different C types
 */
static DWORD s_bufferSize[] = { 0, sizeof(LONG), sizeof(DWORD), sizeof(INT64), sizeof(QWORD), sizeof(double)};

/**
 * Corresponding Oracle types for C types
 */
static ub2 s_oracleType[] = { SQLT_STR, SQLT_INT, SQLT_UIN, SQLT_INT, SQLT_UIN, SQLT_FLT };

/**
 * Bind parameter to statement - normal mode (non-batch)
 */
static void BindNormal(ORACLE_STATEMENT *stmt, int pos, int sqlType, int cType, void *buffer, int allocType)
{
	void *sqlBuffer;
	WCHAR bindPos[8]={0};
	swprintf(bindPos, 8, L":%d", pos);

	switch(cType)
	{
		case DB_CTYPE_STRING:
		{
#if UNICODE_UCS4
		sqlBuffer = wcsdup((WCHAR *)buffer);
		stmt->buffers->set(pos - 1, sqlBuffer);

		if(allocType == DB_BIND_DYNAMIC)
			free(buffer);
#else
		if(allocType == DB_BIND_TRANSIENT)
		{
			sqlBuffer = wcsdup((WCHAR *)buffer);
			stmt->buffers->set(pos - 1, sqlBuffer);
		}
		else
		{
			sqlBuffer = buffer;
			if(allocType == DB_BIND_DYNAMIC)
				stmt->buffers->set(pos - 1, sqlBuffer);
		}
#endif
		if(sqlType == DB_SQLTYPE_CLOB)
		{	
			OCI_Lob *lob = OCI_LobCreate(stmt->connection->handleConnection, OCI_CLOB);
			OCI_LobWrite(lob, (otext *)sqlBuffer, _tcslen((WCHAR *)sqlBuffer));
			OCI_BindLob(stmt->handleStmt, bindPos, lob);
			stmt->lobBinds->set(pos - 1, lob);
		}
		else
			OCI_BindString(stmt->handleStmt, bindPos, (otext *)sqlBuffer, 0);
		}
		break;
		case DB_CTYPE_UTF8_STRING:
#if UNICODE_UCS4
			sqlBuffer = UCS2StringFromUTF8String((char *)buffer);
#else
			sqlBuffer = WideStringFromUTF8String((char *)buffer);
#endif
			stmt->buffers->set(pos - 1, sqlBuffer);

			if(allocType == DB_BIND_DYNAMIC)
				free(buffer);

			OCI_BindString(stmt->handleStmt, bindPos, (otext *)sqlBuffer, 0);
			break;
		case DB_CTYPE_INT64:
			sqlBuffer = nx_memdup(buffer, s_bufferSize[cType]);

			stmt->buffers->set(pos - 1, sqlBuffer);
			OCI_BindBigInt(stmt->handleStmt, bindPos, (INT64*)sqlBuffer);

			if(allocType == DB_BIND_DYNAMIC)
				free(buffer);
			break;
		case DB_CTYPE_UINT64:
			sqlBuffer = nx_memdup(buffer, s_bufferSize[cType]);

			stmt->buffers->set(pos - 1, sqlBuffer);
			OCI_BindUnsignedBigInt(stmt->handleStmt, bindPos, (UINT64*)sqlBuffer);

			if(allocType == DB_BIND_DYNAMIC)
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
					return; // invalid call
			}

			switch(cType)
			{
				case DB_CTYPE_INT32:
					OCI_BindInt(stmt->handleStmt, bindPos, (int*)sqlBuffer);
					break;
				case DB_CTYPE_UINT32:
					OCI_BindUnsignedInt(stmt->handleStmt, bindPos, (unsigned int*)sqlBuffer);
					break;
				case DB_CTYPE_INT64:
					OCI_BindBigInt(stmt->handleStmt, bindPos, (long long int*)sqlBuffer);
					break;
				case DB_CTYPE_UINT64:
					OCI_BindUnsignedBigInt(stmt->handleStmt, bindPos, (long long unsigned int*)sqlBuffer);
					break;
				case DB_CTYPE_DOUBLE:
					OCI_BindDouble(stmt->handleStmt, bindPos, (double*)sqlBuffer);
					break;
			}
			break;
	}

	stmt->bindings->set(pos - 1, OCI_GetBind(stmt->handleStmt, pos));
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
      m_elementSize = sizeof(WCHAR);
      m_string = true;
      m_oraType = (sqlType == DB_SQLTYPE_TEXT) ? SQLT_LNG : SQLT_STR;
      m_data = NULL;
      m_strings = (WCHAR **)calloc(m_allocated + 1, sizeof(WCHAR *));
   }
   else
   {
      m_elementSize = s_bufferSize[cType];
      m_string = false;
      m_oraType = s_oracleType[cType];
      m_data = calloc(m_allocated + 1, m_elementSize);
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
         m_strings = (WCHAR **)realloc(m_strings, (m_allocated + 1) * sizeof(WCHAR *));
         memset(m_strings + m_size, 0, ((m_allocated + 1) - m_size) * sizeof(WCHAR *));
      }
      else
      {
         m_data = realloc(m_data, (m_allocated + 1) * m_elementSize);
         memset((char *)m_data + m_size * m_elementSize, 0, ((m_allocated + 1) - m_size) * m_elementSize);
      }
   }
   if (m_size > 0)
   {
      // clone last element
      if (m_string)
      {
         WCHAR *p = m_strings[m_size - 1];
         m_strings[m_size] = (p != NULL) ? wcsdup(p) : NULL;
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
      m_strings[m_size - 1] = (WCHAR*)value;
      if (value != NULL)
      {
         int l = (int)(wcslen((WCHAR*)value) + 1) * sizeof(WCHAR);
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
   m_data = calloc(m_size, m_elementSize + 1);
   char *p = (char *)m_data;
   for(int i = 0; i < m_size; i++)
   {
      if (m_strings[i] == NULL)
         continue;
      memcpy(p, m_strings[i], wcslen(m_strings[i]) * sizeof(WCHAR));
      p += m_elementSize;
   }
   return m_data;
}

/**
 * Bind parameter to statement - batch mode
 */
static void BindBatch(ORACLE_STATEMENT *stmt, int pos, int sqlType, int cType, void *buffer, int allocType)
{
	if(stmt->batchSize == 0)
		return; // no batch rows added yet

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

    void *sqlBuffer = NULL;
	switch(bind->getCType())
	{
		case DB_CTYPE_STRING:
			{			
#if UNICODE_UCS4
			if(_tcslen((WCHAR *)buffer) == 0)
				sqlBuffer = wcsdup(_T("\r\n")); // set the end of line symbols
			else
				sqlBuffer = wcsdup((WCHAR *)buffer);

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
         	}
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
				WCHAR temp[64];
				swprintf(temp, 64, INT64_FMT, *((INT64 *)buffer));
				sqlBuffer = wcsdup(temp);
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
				WCHAR temp[64];
				swprintf(temp, 64, UINT64_FMT, *((QWORD *)buffer));
				sqlBuffer = wcsdup(temp);
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
	if(stmt->batchMode)
		BindBatch(stmt, pos, sqlType, cType, buffer, allocType);
	else
		BindNormal(stmt, pos, sqlType, cType, buffer, allocType);
}

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

		OCI_BindArraySetSize(stmt->handleStmt, stmt->batchSize);

		for(int i = 0; i < stmt->batchBindings->size(); i++)
		{
			OracleBatchBind *b = stmt->batchBindings->get(i);
			if (b == NULL)
				continue;

			WCHAR bindPos[8]={0};
			swprintf(bindPos, 8, _T(":%d"), i+1);

			switch(b->getOraType())
			{
				case SQLT_LNG:
				case SQLT_STR:
					{
						unsigned int m_dataLen = (b->getElementSize() / sizeof(WCHAR)); // maximum length of single string element
						OCI_BindArrayOfStrings(stmt->handleStmt, bindPos, (WCHAR*)b->getData(), m_dataLen-1, 0);
					}
					break;
				case SQLT_UIN:
				case SQLT_INT:
					{	
						switch(b->getCType())
						{
							case DB_CTYPE_INT32:
								OCI_BindArrayOfInts(stmt->handleStmt, bindPos, (int*)b->getData(), 0);
								break;
 							case DB_CTYPE_UINT32:
 								OCI_BindArrayOfUnsignedInts(stmt->handleStmt, bindPos, (unsigned int*)b->getData(), 0);
 								break;
						}
					}
					break;
				case SQLT_FLT:
					{
						switch(b->getCType())
						{
							case DB_CTYPE_DOUBLE:
								OCI_BindArrayOfDoubles(stmt->handleStmt, bindPos, (double*)b->getData(), 0);
								break;
						}
					}
					break;
				default:
					nxlog_debug(6, _T("Error : Unknown type of bind variable, %d"), b->getOraType());
					break;
			}
		}
	}

	MutexLock(pConn->mutexQueryLock);
	if(OCI_Execute(stmt->handleStmt) == true)
	{
		dwResult = DBERR_SUCCESS;
	}
	else
	{
		SetLastError(pConn);
		dwResult = IsConnectionError(pConn);
	}

	if(errorText != NULL)
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
	if(stmt == NULL)
		return;

	MutexLock(stmt->connection->mutexQueryLock);
	stmt->connection->lastErrorText[0] = 0;
	stmt->connection->lastErrorCode = 0;
	OCI_StatementFree(stmt->handleStmt);
	MutexUnlock(stmt->connection->mutexQueryLock);

	for(int i = 0; i < stmt->lobBinds->size(); i++)
	{
		OCI_Lob *lob = (OCI_Lob *)stmt->lobBinds->get(i);

		if(lob != NULL)
			OCI_LobFree(lob);
	}

	delete stmt->bindings;
	delete stmt->lobBinds;
	delete stmt->batchBindings;
	delete stmt->buffers;
	free(stmt);
}

/**
 * Perform non-SELECT query
 */
static DWORD DrvQueryInternal(ORACLE_CONN *pConn, const WCHAR *pwszQuery, WCHAR *errorText)
{
	DWORD dwResult = -1;

#if UNICODE_UCS4
	UCS2CHAR *ucs2Query = UCS2StringFromUCS4String(pwszQuery);
#else
	const UCS2CHAR *ucs2Query = pwszQuery;
#endif

	MutexLock(pConn->mutexQueryLock);
	OCI_Statement *handleStmt = OCI_StatementCreate(pConn->handleConnection);
	if(OCI_ExecuteStmt(handleStmt, pwszQuery) == true)
	{
		dwResult = DBERR_SUCCESS;
	}
	else
	{
		SetLastError(pConn);
		dwResult = IsConnectionError(pConn);
	}

	if(errorText != NULL)
	{
		wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
		errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	}

	OCI_Commit(pConn->handleConnection);
	OCI_StatementFree(handleStmt);
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
 * Process SELECT results
 */
static ORACLE_RESULT *ProcessQueryResults(ORACLE_CONN *pConn, OCI_Statement *handleStmt, DWORD *pdwError)
{
	bool nStatus;
	unsigned int nWidth;
	ORACLE_RESULT *pResult = (ORACLE_RESULT *)malloc(sizeof(ORACLE_RESULT));
	OCI_Resultset *resultSet;
	resultSet = OCI_GetResultset(handleStmt);

	if(resultSet != NULL)
	{
		pResult->nCols = OCI_GetColumnCount(resultSet);
		pResult->nRows = 0;
		pResult->pData = NULL;
		pResult->columnNames = NULL;
		
		if(pResult->nCols > 0)
		{
			// Get table column names
			pResult->columnNames = (char **)calloc(pResult->nCols, sizeof(char *));
			//pBuffers = (ORACLE_FETCH_BUFFER *)calloc(pResult->nCols, sizeof(ORACLE_FETCH_BUFFER));

			for(int i = 0; i < pResult->nCols; i++)
			{
				OCI_Column *col;
				if (col = OCI_GetColumn(resultSet, i + 1))
				{
					nStatus = OCI_SUCCESS;
					// Columns name
					pResult->columnNames[i] = MBStringFromWideString(OCI_ColumnGetName(col));
				}
				else
				{
					nStatus = OCI_NO_DATA;
					SetLastError(pConn);
					*pdwError = IsConnectionError(pConn);
				}
			}
		}

		// Fetch data
		if(nStatus == OCI_SUCCESS)
		{
			int nPos = 0;

			while(1)
			{
				if(OCI_FetchNext(resultSet) == false)
				{
					*pdwError = DBERR_SUCCESS;
					break;
				}

				// New row
				pResult->nRows++;
				pResult->pData = (WCHAR **)realloc(pResult->pData, sizeof(WCHAR *) * pResult->nCols * pResult->nRows);

				for(int i = 0; i < pResult->nCols; i++)
				{
					OCI_Column *col = OCI_GetColumn(resultSet, i + 1);

					if(col == NULL)
					{
						nStatus = OCI_NO_DATA;
						SetLastError(pConn);
						*pdwError = IsConnectionError(pConn);
						continue;
					}

					unsigned int type = OCI_ColumnGetType(col);

					if(OCI_IsNull(resultSet, i + 1) == true)
					{
						pResult->pData[nPos] = (WCHAR *)nx_memdup("\0\0\0", sizeof(WCHAR));
					}
					else if(type == OCI_CDT_LOB)
					{
						OCI_Lob *lob = OCI_GetLob(resultSet, i + 1);

						if(lob != NULL)
						{
							int length = OCI_LobGetLength(lob);

							if(length > 0)
							{
								int max_chars = length, max_bytes = 0;
								WCHAR *result = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));
								pResult->pData[nPos] = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));

								if(OCI_LobRead2(lob, (WCHAR*)result, (unsigned int*)&max_chars, (unsigned int*)&max_bytes))
								{
									wcsncpy(pResult->pData[nPos], result, length);
									pResult->pData[nPos][length] = 0;
								}
								free(result);							
							}
							else
							{
								pResult->pData[nPos] = (WCHAR *)nx_memdup("\0\0\0", sizeof(WCHAR));
							}
						}
						else
						{
							nStatus = OCI_NO_DATA;
							SetLastError(pConn);
							*pdwError = IsConnectionError(pConn);
							continue;
						}
						// ----
					}
					else
					{
						UCS2CHAR *result = UCS2StringFromUCS4String(OCI_GetString(resultSet, i + 1));
						int length = ucs2_strlen(result);
						bool emptyFlag = false;
						
						// If there is only end of string symbols, the result should be empty
						if(length == 2 && _tcsicmp(OCI_GetString(resultSet, i + 1), _T("\r\n")) == 0)
							pResult->pData[nPos] = (WCHAR *)nx_memdup("\0\0\0", sizeof(WCHAR));
						else
						{
							pResult->pData[nPos] = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));
#if UNICODE_UCS4
							ucs2_to_ucs4(result, length, pResult->pData[nPos], length + 1);
#else
							memcpy(pResult->pData[nPos], result, length);
#endif
							pResult->pData[nPos][length] = 0;
						}
						
						if(NULL != result)
						{
							free(result);
						}
					}
					nPos++;
				}
			}
		}

		if(*pdwError != DBERR_SUCCESS)
		{
			DestroyQueryResult(pResult);
			pResult = NULL;
		}
	}
	else
	{
		SetLastError(pConn);
		*pdwError = IsConnectionError(pConn);
	}

	return pResult;
}

/**
 * Perform SELECT query
 */
extern "C" DBDRV_RESULT EXPORT DrvSelect(ORACLE_CONN *pConn, WCHAR *pwszQuery, DWORD *pdwError, WCHAR *errorText)
{
	ORACLE_RESULT *pResult = NULL;

	OCI_SetStatementCacheSize(pConn->handleConnection, pConn->prefetchLimit);
	OCI_Statement *handleStmt = OCI_StatementCreate(pConn->handleConnection);

#if UNICODE_UCS4
	char *ucs2Query = MBStringFromWideString(pwszQuery);
#else
	const UCS2CHAR *ucs2Query = pwszQuery;
#endif
	OCI_SetPrefetchMemory(handleStmt, pConn->prefetchLimit);
	OCI_SetPrefetchSize(handleStmt, pConn->prefetchLimit);

	MutexLock(pConn->mutexQueryLock);
	if(OCI_ExecuteStmt(handleStmt, pwszQuery) == true)
	{
		pResult = ProcessQueryResults(pConn, handleStmt, pdwError);
		*pdwError = DBERR_SUCCESS;
	}
	else
	{
		SetLastError(pConn);
		*pdwError = IsConnectionError(pConn);
	}
	OCI_StatementFree(handleStmt);

	if(errorText != NULL)
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

	OCI_SetPrefetchMemory(stmt->handleStmt, pConn->prefetchLimit);
	OCI_SetPrefetchSize(stmt->handleStmt, pConn->prefetchLimit);
	OCI_SetStatementCacheSize(pConn->handleConnection, pConn->prefetchLimit);

	MutexLock(pConn->mutexQueryLock);
	if(OCI_Execute(stmt->handleStmt) == true)
	{
		pResult = ProcessQueryResults(pConn, stmt->handleStmt, pdwError);
		*pdwError = DBERR_SUCCESS;
	}
	else
	{
		SetLastError(pConn);
		*pdwError = IsConnectionError(pConn);
	}

	if(errorText != NULL)
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
	if(pResult == NULL)
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

	if(pResult != NULL)
	{
		if((nRow < pResult->nRows) && (nRow >= 0) &&
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
	if(pResult != NULL)
		DestroyQueryResult(pResult);
}

/**
 * Destroy unbuffered query result
 */
static void DestroyUnbufferedQueryResult(ORACLE_UNBUFFERED_RESULT *result, bool freeStatement)
{
	int i;

	if(freeStatement)
		OCI_StatementFree(result->handleStmt);

	free(result->pBuffers);

	for(i = 0; i < result->nCols; i++)
		free(result->columnNames[i]);
	free(result->columnNames);
	free(result);
}


/**
 * Free Fetch allocated memory result
 */

extern "C" void EXPORT DrvFetchFreeResult(ORACLE_UNBUFFERED_RESULT *result)
{
	if(result == NULL)
		return;

	for(int i = 0; i < result->nCols; i++)
	{
		safe_free(result->pBuffers[i].pData);
		if (result->pBuffers[i].lobLocator != NULL) // this maybe not needed anymore
		{
			free(result->pBuffers[i].lobLocator);
		}
	}
}

/**
 * Process results of unbuffered query execution (prepare for fetching results)
 */
static ORACLE_UNBUFFERED_RESULT *ProcessUnbufferedQueryResults(ORACLE_CONN *pConn, OCI_Statement *handleStmt, DWORD *pdwError)
{
	// #FIX - rework the code similar to ProcessQueryResults function.
	ORACLE_UNBUFFERED_RESULT *result = (ORACLE_UNBUFFERED_RESULT *)malloc(sizeof(ORACLE_UNBUFFERED_RESULT));
	result->handleStmt = handleStmt;
	result->connection = pConn;

	OCI_Resultset *resultSet;
	resultSet = OCI_GetResultset(handleStmt);
	result->nCols = OCI_GetColumnCount(resultSet);

	*pdwError = DBERR_SUCCESS;

	if(resultSet != NULL)
	{
		if(result->nCols > 0)
		{
			result->columnNames = (char **)calloc(result->nCols, sizeof(char *));
			result->pBuffers = (ORACLE_FETCH_BUFFER *)calloc(result->nCols, sizeof(ORACLE_FETCH_BUFFER));

			for(int i = 0; i < result->nCols; i++)
			{
				OCI_Column *col = OCI_GetColumn(resultSet, i + 1);
				result->columnNames[i] = MBStringFromWideString(OCI_ColumnGetName(col));
			}
		}
		else
		{
			free(result);
			result = NULL;
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

	MutexLock(pConn->mutexQueryLock);

	OCI_SetStatementCacheSize(pConn->handleConnection, 0);
	OCI_Statement *handleStmt = OCI_StatementCreate(pConn->handleConnection);

	if (OCI_SFM_SCROLLABLE == mode)
	{
		if (!OCI_SetFetchMode(handleStmt, mode))
		{
			SetLastError(pConn);
			nxlog_debug(2, _T("DrvSelectUnbuffered failed[%d]: %s"), pConn->lastErrorCode, pConn->lastErrorText);
			*pdwError = IsConnectionError(pConn);
		}
	}

	if(OCI_Prepare(handleStmt, (otext *)pwszQuery) == true)
	{	
		OCI_SetPrefetchMemory(handleStmt, 0);
		OCI_SetPrefetchSize(handleStmt, 0);

		if(OCI_Execute(handleStmt) == true)
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

	if((*pdwError == DBERR_SUCCESS) && (result != NULL))
		return result;

	OCI_StatementFree(handleStmt);
	if(errorText != NULL)
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

	OCI_SetPrefetchMemory(stmt->handleStmt, 0);
	OCI_SetPrefetchSize(stmt->handleStmt, 0);
	OCI_SetStatementCacheSize(pConn->handleConnection, 0);

	if(OCI_Execute(stmt->handleStmt) == true)
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

	if(errorText != NULL)
	{
		wcsncpy(errorText, pConn->lastErrorText, DBDRV_MAX_ERROR_TEXT);
		errorText[DBDRV_MAX_ERROR_TEXT - 1] = 0;
	}
	MutexUnlock(pConn->mutexQueryLock);
	return NULL;
}

/**
 * Custom Fetch for result set
 * Modes :
 *  OCI_SFD_ABSOLUTE
 *  OCI_SFD_RELATIVE
 */
extern "C" bool EXPORT DrvFetchSeek(ORACLE_UNBUFFERED_RESULT *result, UINT32 mode, int offset)
{
	bool success = false;

	if (result == NULL)
		return success;

	if (OCI_GetFetchMode(result->handleStmt) == OCI_SFM_SCROLLABLE)
	{
		success = OCI_SetFetchSize(result->handleStmt, 1); // Note: You must set the fetching size to 1

		if (!success)
		{
			SetLastError(result->connection);
			nxlog_debug(2, _T("[0]DrvFetchSeek failed[%d]: %s"), result->connection->lastErrorCode, result->connection->lastErrorText);
			return success;
		}
	}
	
	OCI_Resultset *resultSet = OCI_GetResultset(result->handleStmt);

	if (OCI_FetchSeek(resultSet, mode, offset))
	{
		success = true;
	}
	else
	{
		SetLastError(result->connection);
		nxlog_debug(2, _T("[1]DrvFetchSeek failed[%d]: %s"), result->connection->lastErrorCode, result->connection->lastErrorText);
	}

	return success;
}

/**
 * Fetch next result line from unbuffered SELECT results
 */
extern "C" bool EXPORT DrvFetch(ORACLE_UNBUFFERED_RESULT *result)
{
	bool success;

	if(result == NULL)
		return false;

	OCI_Resultset *resultSet = OCI_GetResultset(result->handleStmt);

	if(OCI_FetchNext(resultSet) == true)
	{
		success = true;

		for(int i = 0; i < result->nCols; i++)
		{
			OCI_Column *col = OCI_GetColumn(resultSet, i + 1);

			if(OCI_IsNull(resultSet, i + 1) == true)
			{
				result->pBuffers[i].pData = (WCHAR *)nx_memdup("\0\0\0", sizeof(WCHAR));
				result->pBuffers[i].isNull = 1;
				result->pBuffers[i].nLength = 0;
			}
			else if(OCI_ColumnGetType(col) != OCI_CDT_LOB)
			{
				UCS2CHAR *ucs2string = UCS2StringFromUCS4String(OCI_GetString(resultSet, i + 1));
				int len = ucs2_strlen(ucs2string);
				
				// If there is only end of string symbols, the result should be empty
				if(len == 2 && _tcsicmp(OCI_GetString(resultSet, i + 1), _T("\r\n")) == 0)
					result->pBuffers[i].pData = (WCHAR *)nx_memdup("\0\0\0", sizeof(WCHAR));
				else
				{	
					result->pBuffers[i].pData = (WCHAR*)malloc(sizeof(WCHAR) * (len + 1));
#if UNICODE_UCS4
					ucs2_to_ucs4(ucs2string, len, result->pBuffers[i].pData, len + 1);
#else
					memcpy(result->pBuffers[i].pData, ucs2string, len);
#endif
					result->pBuffers[i].pData[len] = 0;
					result->pBuffers[i].isNull = 0;
					result->pBuffers[i].nLength = len * sizeof(WCHAR);
				}
				
				if (NULL != ucs2string)
				{
					free(ucs2string);
				}	
			}
			else
			{
				OCI_Lob *lob = OCI_GetLob(resultSet, i + 1);
				if(lob != NULL)
				{
					ub2 nWidth = OCI_LobGetLength(lob);

					if(nWidth > 0)
					{
						unsigned int max_len = nWidth, max_byte = 0;
						result->pBuffers[i].pData = (WCHAR *)malloc((nWidth + 1) * sizeof(WCHAR));
						WCHAR *pLob = (WCHAR*)malloc((nWidth + 1) * sizeof(WCHAR));

						if(OCI_LobRead2(lob, (WCHAR*)pLob, (unsigned int*)&max_len, (unsigned int*)&max_byte))
						{
							wcsncpy(result->pBuffers[i].pData, pLob, nWidth);
							result->pBuffers[i].pData[nWidth] = 0;
							result->pBuffers[i].isNull = 0;
							result->pBuffers[i].nLength = max_len * sizeof(WCHAR);
						}
						else
						{
							SetLastError(result->connection);
							success = false;
						}

						free(pLob);
					}
					else
					{
						result->pBuffers[i].pData = (WCHAR *)nx_memdup("\0\0\0", sizeof(WCHAR));
						result->pBuffers[i].isNull = 1;
						result->pBuffers[i].nLength = 0;
					}
				}
				else
				{
					SetLastError(result->connection);
					success = false;
				}
			}
		}
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
	if(result == NULL)
		return 0;

	if((nColumn < 0) || (nColumn >= result->nCols))
		return 0;

	if(result->pBuffers[nColumn].isNull)
		return 0;

	return (LONG)(result->pBuffers[nColumn].nLength / sizeof(WCHAR));
}

/**
 * Get field from current row in unbuffered query result
 */
extern "C" WCHAR EXPORT *DrvGetFieldUnbuffered(ORACLE_UNBUFFERED_RESULT *result, int nColumn, WCHAR *pBuffer, int nBufSize)
{
	int nLen;

	if(result == NULL)
		return NULL;

	if((nColumn < 0) || (nColumn >= result->nCols))
		return NULL;

	if(result->pBuffers[nColumn].isNull)
	{
		*pBuffer = 0;
	}
	else if(result->pBuffers[nColumn].lobLocator != NULL)
	{
		ub4 length = 0;
	}
	else
	{
		nLen = min(nBufSize - 1, ((int)(result->pBuffers[nColumn].nLength / sizeof(WCHAR))));
#if _WIN32
		wcsncpy_s(pBuffer, nBufSize, result->pBuffers[nColumn]->pData, _TRUNCATE);
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
	if(result == NULL)
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
	if(pConn == NULL)
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

	if(pConn == NULL)
		return DBERR_INVALID_HANDLE;

	MutexLock(pConn->mutexQueryLock);
	if(pConn->nTransLevel > 0)
	{
		if(OCI_Commit(pConn->handleConnection) == true)
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

	if(pConn == NULL)
		return DBERR_INVALID_HANDLE;

	MutexLock(pConn->mutexQueryLock);
	if(pConn->nTransLevel > 0)
	{
		if(OCI_Rollback(pConn->handleConnection) == true)
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

	if(hResult != NULL)
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
