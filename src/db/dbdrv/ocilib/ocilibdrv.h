/* $Id$ */
/** OCILib Database Driver
** Copyright (C) 2007-2015 Victor Kirhenshtein
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
** File: ocilibdrv.h
**
**/

#ifndef _ocilibdrv_h_
#define _ocilibdrv_h_

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0502
#endif

#include <winsock2.h>
#include <windows.h>
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif   /* _WIN32 */

#define OCI_CHARSET_UNICODE

#include <nms_common.h>
#include <nms_util.h>
#include <dbdrv.h>
#include <oci.h>
#include <ocilib.h>

/**
 * Fetch buffer
 */
struct ORACLE_FETCH_BUFFER
{
	WCHAR *pData;
   OCILobLocator *lobLocator;
	ub2 nLength;
	ub2 nCode;
	sb2 isNull;
};

/**
 * Database connection
 */
struct ORACLE_CONN
{
   OCI_Connection *handleConnection;
	OCIServer *handleServer;
	OCISvcCtx *handleService;
	OCISession *handleSession;
	OCIError *handleError;
	MUTEX mutexQueryLock;
	int nTransLevel;
	sb4 lastErrorCode;
	WCHAR lastErrorText[DBDRV_MAX_ERROR_TEXT];
   ub4 prefetchLimit;
};

/**
 * Batch bind data for single column
 */
class OracleBatchBind
{
private:
   int m_cType;
   ub2 m_oraType;
   int m_size;
   int m_allocated;
   int m_elementSize;
   bool m_string;
   WCHAR **m_strings;
   void *m_data;

public:
   OracleBatchBind(int cType, int sqlType);
   ~OracleBatchBind();

   void addRow();
   void set(void *value);
   void *getData();
   int getElementSize() { return m_elementSize; }
   int getCType() { return m_cType; }
   ub2 getOraType() { return m_oraType; }
};

/**
 * Statement
 */
struct ORACLE_STATEMENT
{
	ORACLE_CONN *connection;
	OCI_Statement *handleStmt;
	OCIError *handleError;
	Array *bindings;
   Array *lobBinds;
   ObjectArray<OracleBatchBind> *batchBindings;
	Array *buffers;
   bool batchMode;
   int batchSize;
};

/**
 * Result set
 */
struct ORACLE_RESULT
{
	int nRows;
	int nCols;
	WCHAR **pData;
	char **columnNames;
};

struct ORACLE_UNBUFFERED_RESULT
{
   ORACLE_CONN *connection;
   OCI_Statement *handleStmt;
   ORACLE_FETCH_BUFFER *pBuffers;
   int nCols;
   char **columnNames;
};

/**
 * Undocumented internal structure for column parameter handler
 */
struct OCI_PARAM_STRUCT_COLUMN_INFO
{
   unsigned char unknownFields[6 * sizeof(int) + 2 * sizeof(void *)];
   unsigned char attributes[sizeof(void *)];
   char *name;
};

/**
 * Undocumented internal structure for column parameter handler
 */
struct OCI_PARAM_STRUCT
{
   unsigned char unknownFields[3 * sizeof(void *)];
   OCI_PARAM_STRUCT_COLUMN_INFO *columnInfo;
};

#endif   /* _ocilibdrv_h_ */
