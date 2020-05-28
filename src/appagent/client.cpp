/* 
** NetXMS - Network Management System
** Copyright (C) 2003-2013 Victor Kirhenshtein
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
** File: client.cpp
**
**/

#include "appagent-internal.h"

/**
 * Connect to application agent
 */
bool APPAGENT_EXPORTABLE AppAgentConnect(const TCHAR *name, HPIPE *hPipe, const TCHAR *path)
{
#ifdef _WIN32
	TCHAR pipeName[MAX_PATH];
	_sntprintf(pipeName, MAX_PATH, _T("\\\\.\\pipe\\appagent.%s"), name);

reconnect:
	*hPipe = CreateFile(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (*hPipe == INVALID_HANDLE_VALUE)
	{
		if (GetLastError() == ERROR_PIPE_BUSY)
		{
			if (WaitNamedPipe(pipeName, 5000))
				goto reconnect;
		}
		return false;
	}

	DWORD pipeMode = PIPE_READMODE_MESSAGE;
	SetNamedPipeHandleState(*hPipe, &pipeMode, NULL, NULL);
#else
	*hPipe = socket(AF_UNIX, SOCK_STREAM, 0);
	if (*hPipe == -1)
		return false;

	struct sockaddr_un remote;
	remote.sun_family = AF_UNIX;
#ifdef UNICODE
   sprintf(remote.sun_path, "%S/.appagent.%S", path, name);
#else
	sprintf(remote.sun_path, "%s/.appagent.%s", path, name);
#endif
	if (connect(*hPipe, (struct sockaddr *)&remote, SUN_LEN(&remote)) == -1)
	{
		close(*hPipe);
		return false;
	}
#endif

	return true;
}

/**
 * Disconnect from application agent
 */
void APPAGENT_EXPORTABLE AppAgentDisconnect(HPIPE hPipe)
{
#ifdef _WIN32
	CloseHandle(hPipe);
#else
	close(hPipe);
#endif
}

/**
 * Get metric from application agent
 */
int APPAGENT_EXPORTABLE AppAgentGetMetric(HPIPE hPipe, const TCHAR *name, TCHAR *value, int bufferSize)
{
	int rcc = APPAGENT_RCC_COMM_FAILURE;

	APPAGENT_MSG *request = NewMessage(APPAGENT_CMD_GET_METRIC, 0, ((int)_tcslen(name) + 1) * sizeof(WCHAR));
#ifdef UNICODE
	wcscpy((WCHAR *)request->payload, name);
#else
	MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, name, -1, (WCHAR *)request->payload, (int)strlen(name) + 1);
#endif

	if (SendMessageToPipe(hPipe, request))
	{
		AppAgentMessageBuffer *mb = new AppAgentMessageBuffer;
		APPAGENT_MSG *response = ReadMessageFromPipe(hPipe, mb);
		if (response != NULL)
		{
			if (response->command == APPAGENT_CMD_REQUEST_COMPLETED)
			{
				rcc = response->rcc;
				if (rcc == APPAGENT_RCC_SUCCESS)
				{
					int valueLen = (response->length - APPAGENT_MSG_HEADER_LEN) / sizeof(TCHAR);
#ifdef UNICODE
					nx_strncpy(value, (WCHAR *)response->payload, MIN(valueLen, bufferSize));
#else
					WideCharToMultiByte(CP_ACP, WC_COMPOSITECHECK | WC_DEFAULTCHAR, (WCHAR *)response->payload, valueLen, value, bufferSize, NULL, NULL);
					value[MIN(valueLen, bufferSize - 1)] = 0;
#endif
				}
			}
			free(response);
		}
		delete mb;
	}

	free(request);
	return rcc;
}

/**
 * Get all supported metrics from agent
 */
int APPAGENT_EXPORTABLE AppAgentListMetrics(HPIPE hPipe, APPAGENT_METRIC **metrics, UINT32 *size)
{
   return APPAGENT_RCC_BAD_REQUEST;
}
