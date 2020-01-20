/* 
** NetXMS - Network Management System
** Copyright (C) 2003-2015 Victor Kirhenshtein
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
** File: comm.cpp
**
**/

#include "libnxcc.h"
#include <nxdbapi.h>

/**
 * Externals
 */
void ClusterNodeJoin(void *arg);
void ProcessClusterJoinRequest(ClusterNodeInfo *node, NXCPMessage *msg);
bool ClusterGetNodeStateFromDB(ClusterNodeInfo *node);
static THREAD_RESULT THREAD_CALL DatabaseUpdateTimestampThread(void *arg);

/**
 * Keepalive interval
 */
#define KEEPALIVE_INTERVAL    200

/**
 * Thread handles
 */
static THREAD s_listenerThread = INVALID_THREAD_HANDLE;
static THREAD s_connectorThread = INVALID_THREAD_HANDLE;
static THREAD s_keepaliveThread = INVALID_THREAD_HANDLE;
static THREAD s_databaseTimeThread = INVALID_THREAD_HANDLE;

/**
 * Command ID
 */
static VolatileCounter s_commandId = 1;

/**
 * Join condition
 */
static CONDITION s_joinCondition = ConditionCreate(TRUE);

/**
 * Mark as joined
 */
void SetJoinCondition()
{
   ConditionSet(s_joinCondition);
}

/**
 * Process cluster notification
 */
static void ProcessClusterNotification(ClusterNodeInfo *node, int code)
{
   ClusterDebug(4, _T("ProcessClusterNotification: code %d from node %d [%s]"), code, node->m_id, (const TCHAR *)node->m_addr->toString());
   switch(code)
   {
      case CN_NEW_MASTER:
         ClusterDebug(3, _T("Node %d [%s] is new master"), node->m_id, (const TCHAR *)node->m_addr->toString());
         MutexLock(node->m_mutex);
         node->m_master = true;
         ChangeClusterNodeState(node, CLUSTER_NODE_UP);
         MutexUnlock(node->m_mutex);
         ConditionSet(s_joinCondition);
         break;
      case CN_NODE_RUNNING:
         MutexLock(node->m_mutex);
         ChangeClusterNodeState(node, CLUSTER_NODE_UP);
         MutexUnlock(node->m_mutex);
         break;
      default:
         if (code >= CN_CUSTOM)
         {
            g_nxccEventHandler->onNotification(code, node->m_id);
         }
         break;
   }
}

/**
 * Receiver thread stop data
 */
struct ReceiverThreadStopData
{
   ClusterNodeInfo *node;
   SOCKET s;
};

/**
 * Shutdown callback for receiver thread
 */
static void ReceiverThreadShutdownCB(void *arg)
{
   ReceiverThreadStopData *data = (ReceiverThreadStopData *)arg;

   MutexLock(data->node->m_mutex);
   if (data->node->m_socket == data->s)
   {
      shutdown(data->s, SHUT_RDWR);
      data->node->m_socket = INVALID_SOCKET;
      ChangeClusterNodeState(data->node, CLUSTER_NODE_DOWN);
   }
   MutexUnlock(data->node->m_mutex);
   free(data);
   ClusterDebug(6, _T("Cluster receiver thread shutdown callback completed"));
}

/**
 * Node receiver thread
 */
static THREAD_RESULT THREAD_CALL ClusterReceiverThread(void *arg)
{
   ClusterNodeInfo *node = (ClusterNodeInfo *)arg;
   SOCKET s = node->m_socket;
   ClusterDebug(5, _T("Receiver thread started for cluster node %d [%s] on socket %d"), node->m_id, (const TCHAR *)node->m_addr->toString(), (int)s);

   SocketMessageReceiver receiver(s, 8192, 4194304);

   while(!g_nxccShutdown)
   {
      MutexLock(node->m_mutex);
      SOCKET cs = node->m_socket;
      MutexUnlock(node->m_mutex);

      if (cs != s)
         break;   // socket was changed

      MessageReceiverResult result;
      NXCPMessage *msg = receiver.readMessage(KEEPALIVE_INTERVAL * 3, &result);
      if (msg != NULL)
      {
         if (msg->getCode() != CMD_KEEPALIVE)
         {
            TCHAR buffer[128];
            ClusterDebug(7, _T("ClusterReceiverThread: message %s (%d) from node %d [%s]"),
               NXCPMessageCodeName(msg->getCode(), buffer), msg->getId(), node->m_id, (const TCHAR *)node->m_addr->toString());
         }

         switch(msg->getCode())
         {
            case CMD_CLUSTER_NOTIFY:
               ProcessClusterNotification(node, msg->getFieldAsInt16(VID_NOTIFICATION_CODE));
               delete msg;
               break;
            case CMD_JOIN_CLUSTER:
               ProcessClusterJoinRequest(node, msg);
               delete msg;
               break;
            case CMD_KEEPALIVE:
               delete msg;
               break;
            case CMD_REQUEST_COMPLETED:
               node->m_msgWaitQueue->put(msg);
               break;
            default:
               ClusterMessageProcessingResult r = g_nxccEventHandler->onMessage(msg, node->m_id);
               if (r == CLUSTER_MSG_IGNORED)
                  node->m_msgWaitQueue->put(msg);
               else if (r == CLUSTER_MSG_PROCESSED)
                  delete msg;
               break;
         }
      }
      else
      {
         ClusterDebug(5, _T("Receiver error for cluster node %d [%s] on socket %d: %s"),
            node->m_id, (const TCHAR *)node->m_addr->toString(), (int)s, AbstractMessageReceiver::resultToText(result));
         ReceiverThreadStopData *data = (ReceiverThreadStopData *)malloc(sizeof(ReceiverThreadStopData));
         data->node = node;
         data->s = s;
         ThreadPoolExecute(g_nxccThreadPool, ReceiverThreadShutdownCB, data);
         break;
      }
   }

   closesocket(s);
   ClusterDebug(5, _T("Receiver thread stopped for cluster node %d [%s] on socket %d"), node->m_id, (const TCHAR *)node->m_addr->toString(), (int)s);
   return THREAD_OK;
}

/**
 * Find cluster node by IP
 */
static int FindClusterNode(const InetAddress& addr)
{
   for(int i = 0; i < CLUSTER_MAX_NODE_ID; i++)
      if ((g_nxccNodes[i].m_id != 0) && g_nxccNodes[i].m_addr->equals(addr))
         return i;
   return -1;
}

/**
 * Find cluster node by ID
 */
static int FindClusterNode(UINT32 id)
{
   for(int i = 0; i < CLUSTER_MAX_NODE_ID; i++)
      if (g_nxccNodes[i].m_id == id)
         return i;
   return -1;
}

/**
 * Change cluster node state
 * Mutex on ClusterNodeInfo structure should be locked by caller
 */
void ChangeClusterNodeState(ClusterNodeInfo *node, ClusterNodeState state)
{
   static const TCHAR *stateNames[] = { _T("DOWN"), _T("CONNECTED"), _T("SYNC"), _T("UP") };

   if (node->m_state == state)
      return;

   node->m_state = state;
   ClusterDebug(1, _T("Cluster node %d [%s] changed state to %s"), node->m_id, (const TCHAR *)node->m_addr->toString(), stateNames[state]);
   THREAD receiverThread;
   switch(state)
   {
      case CLUSTER_NODE_CONNECTED:
         node->m_receiverThread = ThreadCreateEx(ClusterReceiverThread, 0, node);
         if (node->m_id < g_nxccNodeId)
            ThreadPoolExecute(g_nxccThreadPool, ClusterNodeJoin, node);
         break;
      case CLUSTER_NODE_DOWN:
         receiverThread = node->m_receiverThread;
         node->m_receiverThread = INVALID_THREAD_HANDLE;
         MutexUnlock(node->m_mutex);
         // Join receiver thread with node mutex unlocked to avoid deadlock
         ThreadJoin(receiverThread);
         MutexLock(node->m_mutex);
         g_nxccEventHandler->onNodeDisconnect(node->m_id);
         if (node->m_master)
         {
            bool node_status = ClusterGetNodeStateFromDB(node);
            ClusterDebug(5, _T("Cluster node %d [%s] state from database is %s"), node->m_id, (const TCHAR *)node->m_addr->toString(), node_status ? _T("UP") : _T("DOWN"));

            if(!node_status)
            {
               node->m_master = false;
               PromoteClusterNode();
            }
         }
         break;
      case CLUSTER_NODE_SYNC:
         g_nxccEventHandler->onNodeJoin(node->m_id);
         break;
      case CLUSTER_NODE_UP:
         g_nxccEventHandler->onNodeUp(node->m_id);
         break;
   }
}

/**
 * Listener thread
 */
static THREAD_RESULT THREAD_CALL ClusterListenerThread(void *arg)
{
   SOCKET s = CAST_FROM_POINTER(arg, SOCKET);
   SocketPoller p;
   while(!g_nxccShutdown)
   {
      p.reset();
      p.add(s);
      int rc = p.poll(1000);
      if ((rc > 0) && !g_nxccShutdown)
      {
         char clientAddr[128];
         socklen_t size = 128;
         SOCKET in = accept(s, (struct sockaddr *)clientAddr, &size);
         if (in == INVALID_SOCKET)
         {
            ClusterDebug(5, _T("ClusterListenerThread: accept() failure"));
            continue;
         }

#ifndef _WIN32
         fcntl(in, F_SETFD, fcntl(in, F_GETFD) | FD_CLOEXEC);
#endif

         InetAddress addr = InetAddress::createFromSockaddr((struct sockaddr *)clientAddr);
         ClusterDebug(5, _T("ClusterListenerThread: incoming connection from %s"), (const TCHAR *)addr.toString());

         int idx = FindClusterNode(addr);
         if (idx == -1)
         {
            ClusterDebug(5, _T("ClusterListenerThread: incoming connection rejected (unknown IP address)"));
            closesocket(in);
            continue;
         }

         MutexLock(g_nxccNodes[idx].m_mutex);
         if (g_nxccNodes[idx].m_socket == INVALID_SOCKET)
         {
            g_nxccNodes[idx].m_socket = in;
            ClusterDebug(5, _T("Cluster peer node %d [%s] connected"),
               g_nxccNodes[idx].m_id, (const TCHAR *)g_nxccNodes[idx].m_addr->toString());
            ChangeClusterNodeState(&g_nxccNodes[idx], CLUSTER_NODE_CONNECTED);
         }
         else
         {
            ClusterDebug(5, _T("Cluster connection from peer %d [%s] discarded because connection already present"),
               g_nxccNodes[idx].m_id, (const TCHAR *)g_nxccNodes[idx].m_addr->toString());
            closesocket(in);
         }
         MutexUnlock(g_nxccNodes[idx].m_mutex);
      }
   }

   closesocket(s);
   ClusterDebug(1, _T("Cluster listener thread stopped"));
   return THREAD_OK;
}

/**
 * Connector thread
 */
static THREAD_RESULT THREAD_CALL ClusterConnectorThread(void *arg)
{
   ClusterDebug(1, _T("Cluster connector thread started"));

   while(!g_nxccShutdown)
   {
      ThreadSleepMs(500);
      if (g_nxccShutdown)
         break;

      for(int i = 0; i < CLUSTER_MAX_NODE_ID; i++)
      {
         MutexLock(g_nxccNodes[i].m_mutex);
         if ((g_nxccNodes[i].m_id != 0) && (g_nxccNodes[i].m_socket == INVALID_SOCKET))
         {
            MutexUnlock(g_nxccNodes[i].m_mutex);
            SOCKET s = ConnectToHost(*g_nxccNodes[i].m_addr, g_nxccNodes[i].m_port, 500);
            MutexLock(g_nxccNodes[i].m_mutex);
            if (s != INVALID_SOCKET)
            {
               if (g_nxccNodes[i].m_socket == INVALID_SOCKET)
               {
                  g_nxccNodes[i].m_socket = s;
                  ClusterDebug(5, _T("Cluster peer node %d [%s] connected"),
                     g_nxccNodes[i].m_id, (const TCHAR *)g_nxccNodes[i].m_addr->toString());
                  ChangeClusterNodeState(&g_nxccNodes[i], CLUSTER_NODE_CONNECTED);
               }
               else
               {
                  ClusterDebug(5, _T("Cluster connection established with peer %d [%s] but discarded because connection already present"),
                     g_nxccNodes[i].m_id, (const TCHAR *)g_nxccNodes[i].m_addr->toString());
                  closesocket(s);
               }
            }
         }
         MutexUnlock(g_nxccNodes[i].m_mutex);
      }
   }

   ClusterDebug(1, _T("Cluster connector thread stopped"));
   return THREAD_OK;
}

/**
 * Cluster keepalive thread
 */
static THREAD_RESULT THREAD_CALL ClusterKeepaliveThread(void *arg)
{
   ClusterDebug(1, _T("Cluster keepalive thread started"));

   NXCPMessage msg;
   msg.setCode(CMD_KEEPALIVE);
   msg.setField(VID_NODE_ID, g_nxccNodeId);
   NXCP_MESSAGE *rawMsg = msg.createMessage();

   while(!g_nxccShutdown)
   {
      ThreadSleepMs(KEEPALIVE_INTERVAL);
      if (g_nxccShutdown)
         break;

      for(int i = 0; i < CLUSTER_MAX_NODE_ID; i++)
      {
         if (g_nxccNodes[i].m_id == 0)
            continue;   // empty slot

         MutexLock(g_nxccNodes[i].m_mutex);
         if (g_nxccNodes[i].m_socket != INVALID_SOCKET)
         {
            if (SendEx(g_nxccNodes[i].m_socket, rawMsg, ntohl(rawMsg->size), 0, NULL) <= 0)
            {
               ClusterDebug(5, _T("ClusterKeepaliveThread: send failed for peer %d [%s]"),
                  g_nxccNodes[i].m_id, (const TCHAR *)g_nxccNodes[i].m_addr->toString());
               shutdown(g_nxccNodes[i].m_socket, SHUT_RDWR);
               g_nxccNodes[i].m_socket = INVALID_SOCKET; // current socket will be closed by receiver
               ChangeClusterNodeState(&g_nxccNodes[i], CLUSTER_NODE_DOWN);
            }
         }
         MutexUnlock(g_nxccNodes[i].m_mutex);
      }
   }

   free(rawMsg);
   ClusterDebug(1, _T("Cluster keepalive thread stopped"));
   return THREAD_OK;
}

/**
 * Send message to cluster node
 */
void ClusterSendMessage(ClusterNodeInfo *node, NXCPMessage *msg)
{
   TCHAR buffer[64];

   if (node->m_addr != NULL)
   {
      ClusterDebug(7, _T("ClusterSendMessage: sending message %s (%d) to peer %d [%s]"),
         NXCPMessageCodeName(msg->getCode(), buffer), msg->getId(),
         node->m_id, (const TCHAR *)node->m_addr->toString());
   }
   else
   {
      ClusterDebug(7, _T("ClusterSendMessage: sending message %s (%d) to peer %d"),
         NXCPMessageCodeName(msg->getCode(), buffer), msg->getId(),
         node->m_id);
   }

   NXCP_MESSAGE *rawMsg = msg->createMessage();
   MutexLock(node->m_mutex);
   if (node->m_socket != INVALID_SOCKET)
   {
      if (SendEx(node->m_socket, rawMsg, ntohl(rawMsg->size), 0, NULL) <= 0)
      {
         ClusterDebug(5, _T("ClusterSendMessage: send failed for peer %d [%s]"), node->m_id, (const TCHAR *)node->m_addr->toString());
         shutdown(node->m_socket, SHUT_RDWR);
         node->m_socket = INVALID_SOCKET; // current socket will be closed by receiver
         ChangeClusterNodeState(node, CLUSTER_NODE_DOWN);
      }
   }
   else
   {
      if (node->m_addr != NULL)
      {
         ClusterDebug(5, _T("ClusterSendMessage: send failed for peer %d [%s]"), node->m_id, (const TCHAR *)node->m_addr->toString());
      }
      else
      {
         ClusterDebug(5, _T("ClusterSendMessage: send failed to peer %d [<NULL>]"), node->m_id);
      }
   }
   MutexUnlock(node->m_mutex);
   free(rawMsg);
}

/**
 * Send notification to all connected nodes
 */
void LIBNXCC_EXPORTABLE ClusterNotify(INT16 code)
{
   NXCPMessage msg;
   msg.setCode(CMD_CLUSTER_NOTIFY);
   msg.setId((UINT32)InterlockedIncrement(&s_commandId));
   msg.setField(VID_NOTIFICATION_CODE, code);
   msg.setField(VID_NODE_ID, g_nxccNodeId);
   msg.setField(VID_IS_MASTER, (INT16)(g_nxccMasterNode ? 1 : 0));
   ClusterNotify(&msg);
}

/**
 * Send notification to all connected nodes
 */
void LIBNXCC_EXPORTABLE ClusterNotify(NXCPMessage *msg)
{
   NXCP_MESSAGE *rawMsg = msg->createMessage();

   for(int i = 0; i < CLUSTER_MAX_NODE_ID; i++)
   {
      if (g_nxccNodes[i].m_id == 0)
         continue;   // empty slot

      MutexLock(g_nxccNodes[i].m_mutex);
      if (g_nxccNodes[i].m_socket != INVALID_SOCKET)
      {
         if (SendEx(g_nxccNodes[i].m_socket, rawMsg, ntohl(rawMsg->size), 0, NULL) <= 0)
         {
            ClusterDebug(5, _T("ClusterNotify: send failed for peer %d [%s]"),
               g_nxccNodes[i].m_id, (const TCHAR *)g_nxccNodes[i].m_addr->toString());
            shutdown(g_nxccNodes[i].m_socket, SHUT_RDWR);
            g_nxccNodes[i].m_socket = INVALID_SOCKET; // current socket will be closed by receiver
            ChangeClusterNodeState(&g_nxccNodes[i], CLUSTER_NODE_DOWN);
         }
      }
      MutexUnlock(g_nxccNodes[i].m_mutex);
   }

   free(rawMsg);
}

/**
 * Direct notify with just notification code
 */
void LIBNXCC_EXPORTABLE ClusterDirectNotify(UINT32 nodeId, INT16 code)
{
   int index = FindClusterNode(nodeId);
   if (index != -1)
   {
      NXCPMessage msg;
      msg.setCode(CMD_CLUSTER_NOTIFY);
      msg.setField(VID_NOTIFICATION_CODE, code);
      ClusterDirectNotify(&g_nxccNodes[index], &msg);
   }
}

/**
 * Direct notify
 */
void LIBNXCC_EXPORTABLE ClusterDirectNotify(UINT32 nodeId, NXCPMessage *msg)
{
   int index = FindClusterNode(nodeId);
   if (index != -1)
      ClusterDirectNotify(&g_nxccNodes[index], msg);
}

/**
 * Direct notify with just notification code
 */
void ClusterDirectNotify(ClusterNodeInfo *node, NXCPMessage *msg)
{
   msg->setId((UINT32)InterlockedIncrement(&s_commandId));
   msg->setField(VID_NODE_ID, g_nxccNodeId);
   msg->setField(VID_IS_MASTER, (INT16)(g_nxccMasterNode ? 1 : 0));
   ClusterSendMessage(node, msg);
}

/**
 * Send command to all connected nodes and wait for response
 *
 * @return number of execution errors
 */
int LIBNXCC_EXPORTABLE ClusterSendCommand(NXCPMessage *msg)
{
   INT64 startTime = GetCurrentTimeMs();

   UINT32 requestId = (UINT32)InterlockedIncrement(&s_commandId);
   msg->setId(requestId);
   NXCP_MESSAGE *rawMsg = msg->createMessage();

   bool waitFlags[CLUSTER_MAX_NODE_ID];
   memset(waitFlags, 0, sizeof(waitFlags));

   int errors = 0;
   for(int i = 0; i < CLUSTER_MAX_NODE_ID; i++)
   {
      if (g_nxccNodes[i].m_id == 0)
         continue;   // empty slot

      MutexLock(g_nxccNodes[i].m_mutex);
      if (g_nxccNodes[i].m_socket != INVALID_SOCKET)
      {
         if (SendEx(g_nxccNodes[i].m_socket, rawMsg, ntohl(rawMsg->size), 0, NULL) > 0)
         {
            waitFlags[i] = true;
         }
         else
         {
            nxlog_debug(5, _T("ClusterCommand: send failed for peer %d [%s]"),
               g_nxccNodes[i].m_id, (const TCHAR *)g_nxccNodes[i].m_addr->toString());
            shutdown(g_nxccNodes[i].m_socket, SHUT_RDWR);
            g_nxccNodes[i].m_socket = INVALID_SOCKET; // current socket will be closed by receiver
            ChangeClusterNodeState(&g_nxccNodes[i], CLUSTER_NODE_DOWN);
            errors++;
         }
      }
      MutexUnlock(g_nxccNodes[i].m_mutex);
   }

   free(rawMsg);

   // Collect responses
   for(int i = 0; i < CLUSTER_MAX_NODE_ID; i++)
   {
      if (!waitFlags[i])
         continue;
      NXCPMessage *response = g_nxccNodes[i].m_msgWaitQueue->waitForMessage(CMD_REQUEST_COMPLETED, requestId, g_nxccCommandTimeout);
      if (response != NULL)
      {
         UINT32 rcc = response->getFieldAsInt32(VID_RCC);
         if (rcc != NXCC_RCC_SUCCESS)
         {
            nxlog_debug(5, _T("ClusterCommand: failed request to peer %d [%s] RCC=%d"),
               g_nxccNodes[i].m_id, (const TCHAR *)g_nxccNodes[i].m_addr->toString(), rcc);
            errors++;
         }
         delete response;
      }
      else
      {
         nxlog_debug(5, _T("ClusterCommand: timed out request to peer %d [%s]"),
            g_nxccNodes[i].m_id, (const TCHAR *)g_nxccNodes[i].m_addr->toString());
         errors++;
      }
   }

   UINT32 elapsed = (UINT32)(GetCurrentTimeMs() - startTime);
   TCHAR buffer[64];
   nxlog_debug(6, _T("ClusterCommand: command %s [%u] processed in %u ms (errors=%d)"),
               NXCPMessageCodeName(msg->getCode(), buffer), msg->getId(), elapsed, errors);
   return errors;
}

/**
 * Send command to specific node and wait for response
 *
 * @return request completion code
 */
UINT32 LIBNXCC_EXPORTABLE ClusterSendDirectCommand(UINT32 nodeId, NXCPMessage *msg)
{
   NXCPMessage *response = ClusterSendDirectCommandEx(nodeId, msg);
   if (response == NULL)
   {
      nxlog_debug(5, _T("ClusterDirectCommand: request timeout to peer %d (requestId=%d)"), nodeId, msg->getId());
      return NXCC_RCC_TIMEOUT;
   }

   UINT32 rcc = response->getFieldAsUInt32(VID_RCC);
   if (rcc != NXCC_RCC_SUCCESS)
   {
      nxlog_debug(5, _T("ClusterDirectCommand: failed request to peer %d (rcc=%d, requestId=%d)"), nodeId, rcc, msg->getId());
   }
   delete response;
   return rcc;
}

/**
 * Send command to specific node and wait for response
 *
 * @return request completion code
 */
NXCPMessage LIBNXCC_EXPORTABLE *ClusterSendDirectCommandEx(UINT32 nodeId, NXCPMessage *msg)
{
   int index = FindClusterNode(nodeId);
   if (index == -1)
   {
      NXCPMessage *response = new NXCPMessage();
      response->setField(VID_RCC, NXCC_RCC_INVALID_NODE);
      return response;
   }

   ClusterNodeInfo *node = &g_nxccNodes[index];

   UINT32 requestId = (UINT32)InterlockedIncrement(&s_commandId);
   msg->setId(requestId);
   NXCP_MESSAGE *rawMsg = msg->createMessage();

   bool sent = false;
   MutexLock(node->m_mutex);
   if (node->m_socket != INVALID_SOCKET)
   {
      TCHAR buffer[64];
      ClusterDebug(7, _T("ClusterSendDirectCommandEx: sending message %s (%d) to peer %d [%s]"),
         NXCPMessageCodeName(msg->getCode(), buffer), msg->getId(),
         node->m_id, (const TCHAR *)node->m_addr->toString());

      if (SendEx(node->m_socket, rawMsg, ntohl(rawMsg->size), 0, NULL) > 0)
      {
         sent = true;
      }
      else
      {
         ClusterDebug(5, _T("ClusterDirectCommand: send failed for peer %d [%s]"), nodeId, (const TCHAR *)node->m_addr->toString());
         shutdown(node->m_socket, SHUT_RDWR);
         node->m_socket = INVALID_SOCKET; // current socket will be closed by receiver
         ChangeClusterNodeState(node, CLUSTER_NODE_DOWN);
      }
   }
   else
   {
      ClusterDebug(5, _T("ClusterDirectCommand: send failed for peer %d"), nodeId);
   }
   MutexUnlock(node->m_mutex);

   free(rawMsg);

   // Wait for responses
   NXCPMessage *response;
   if (sent)
   {
      response = node->m_msgWaitQueue->waitForMessage(CMD_REQUEST_COMPLETED, requestId, g_nxccCommandTimeout);
   }
   else
   {
      response = new NXCPMessage();
      response->setField(VID_RCC, NXCC_RCC_COMM_FAILURE);
   }

   return response;
}

/**
 * Send response to cluster peer
 */
void LIBNXCC_EXPORTABLE ClusterSendResponse(UINT32 nodeId, UINT32 requestId, UINT32 rcc)
{
   int index = FindClusterNode(nodeId);
   if (index == -1)
   {
      ClusterDebug(5, _T("ClusterSendResponse: peer node with ID %d not found (requestId=%d)"), nodeId, requestId);
      return;
   }

   ClusterNodeInfo *node = &g_nxccNodes[index];

   NXCPMessage msg;
   msg.setCode(CMD_REQUEST_COMPLETED);
   msg.setId(requestId);

   ClusterSendMessage(node, &msg);
}

/**
 * Send response to cluster peer
 */
void LIBNXCC_EXPORTABLE ClusterSendResponseEx(UINT32 nodeId, UINT32 requestId, NXCPMessage *msg)
{
   int index = FindClusterNode(nodeId);
   if (index == -1)
   {
      ClusterDebug(5, _T("ClusterSendResponseEx: peer node with ID %d not found (requestId=%d)"), nodeId, requestId);
      return;
   }

   ClusterNodeInfo *node = &g_nxccNodes[index];

   msg->setCode(CMD_REQUEST_COMPLETED);
   msg->setId(requestId);

   ClusterSendMessage(node, msg);
}

/**
 * Check if all cluster nodes connected
 */
bool LIBNXCC_EXPORTABLE ClusterAllNodesConnected()
{
   int total = 0, connected = 0;
   for(int i = 0; i < CLUSTER_MAX_NODE_ID; i++)
      if (g_nxccNodes[i].m_id > 0)
      {
         total++;
         if (g_nxccNodes[i].m_state >= CLUSTER_NODE_CONNECTED)
            connected++;
      }
   return total == connected;
}

/**
 * Join cluster
 *
 * @return true on successful join
 */
bool LIBNXCC_EXPORTABLE ClusterJoin()
{
   if (!g_nxccInitialized)
      return false;

   SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
   if (s == INVALID_SOCKET)
   {
      ClusterDebug(1, _T("ClusterJoin: cannot create socket"));
      return false;
   }

   SetSocketExclusiveAddrUse(s);
   SetSocketReuseFlag(s);

   struct sockaddr_in servAddr;
   memset(&servAddr, 0, sizeof(struct sockaddr_in));
   servAddr.sin_family = AF_INET;
   servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
   servAddr.sin_port = htons((UINT16)(47000 + g_nxccNodeId));
   if (bind(s, (struct sockaddr *)&servAddr, sizeof(struct sockaddr_in)) != 0)
   {
      ClusterDebug(1, _T("ClusterJoin: cannot bind listening socket (%s)"), _tcserror(WSAGetLastError()));
      closesocket(s);
      return false;
   }

   if (listen(s, SOMAXCONN) == 0)
   {
      ClusterDebug(1, _T("ClusterJoin: listening on port %d"), (int)ntohs(servAddr.sin_port));
   }
   else
   {
      ClusterDebug(1, _T("ClusterJoin: listen() failed (%s)"), _tcserror(WSAGetLastError()));
      closesocket(s);
      return false;
   }

   s_listenerThread = ThreadCreateEx(ClusterListenerThread, 0, CAST_TO_POINTER(s, void *));
   s_connectorThread = ThreadCreateEx(ClusterConnectorThread, 0, NULL);
   s_keepaliveThread = ThreadCreateEx(ClusterKeepaliveThread, 0, NULL);
   s_databaseTimeThread = ThreadCreateEx(DatabaseUpdateTimestampThread, 0, NULL);

   ClusterDebug(1, _T("ClusterJoin: waiting for other nodes"));
   if (ConditionWait(s_joinCondition, 60000))  // wait 1 minute for other nodes to join
   {
      ClusterDebug(1, _T("ClusterJoin: successfully joined"));
   }
   else
   {
      // no other nodes, declare self as master
      ClusterDebug(1, _T("ClusterJoin: cannot contact other nodes, declaring self as master"));
      PromoteClusterNode();
   }

   return true;
}

/**
 * Disconnect all sockets
 */
void ClusterDisconnect()
{
   ThreadJoin(s_listenerThread);
   ThreadJoin(s_connectorThread);
   ThreadJoin(s_keepaliveThread);
   ThreadJoin(s_databaseTimeThread);
}

/**
 * Set current node as running
 */
void LIBNXCC_EXPORTABLE ClusterSetRunning()
{
   g_nxccNeedSync = false;
   ClusterNotify(CN_NODE_RUNNING);
}

/**
 * Get node state from database table
 */
bool ClusterGetNodeStateFromDB(ClusterNodeInfo *node)
{
   bool result = false;
   UINT32 node_id = node->m_id;

   DB_HANDLE hdb = DBConnectionPoolAcquireConnection();
   TCHAR query[256];

   _sntprintf(query, 256, _T("SELECT VAR_NAME, VAR_VALUE FROM config WHERE ")
      _T("VAR_NAME='DBLockInfo.%d' OR ")
      _T("VAR_NAME='DBLockPID.%d' OR ")
      _T("VAR_NAME='DBLockStatus.%d' OR ")
      _T("VAR_NAME='DBLockTimestamp.%d' ORDER BY VAR_NAME ASC"),
      node_id, node_id, node_id, node_id);
   DB_RESULT hResult = DBSelect(hdb, query);

   if(hResult != NULL)
   {
      if (DBGetNumRows(hResult) >= 3)
      {
         TCHAR lockinfo[255], lockpid[255], lockstatus[255];
         INT32 locktime;

         DBGetField(hResult, 0, 1, lockinfo, 255);
         DBGetField(hResult, 1, 1, lockpid, 255);
         DBGetField(hResult, 2, 1, lockstatus, 255);
         locktime = DBGetFieldLong(hResult, 3, 1);

         if (_tcscmp(_T("#00"), lockinfo) || _tcscmp(_T("0"), lockpid) || _tcscmp(_T("UNLOCKED"), lockstatus))
         {
            // if lock time is not updated more than 60 seconds, seems that server is not running
            if (locktime > 0 && locktime - time(NULL) < 60)
            {
               result = true;
            }
         }
      }
      DBFreeResult(hResult);
   }
   DBConnectionPoolReleaseConnection(hdb);

   return result;
}

/**
 * Cluster keepalive thread
 */
static THREAD_RESULT THREAD_CALL DatabaseUpdateTimestampThread(void *arg)
{
   ClusterDebug(1, _T("Database update timestamp thread started"));

   while(!g_nxccShutdown)
   {
      ThreadSleepMs(g_nxccDBUpdateTimeout * 1000);
      if (g_nxccShutdown)
         break;

      DB_HANDLE hdb = DBConnectionPoolAcquireConnection();

      TCHAR query[1024];
      _sntprintf(query, 1024, _T("UPDATE CONFIG SET VAR_VALUE=%d WHERE VAR_NAME='DBLockTimestamp.%d'"), time(NULL), g_nxccNodeId);
      if (!DBQuery(hdb, query))
      {
         ClusterDebug(1, _T("Database update timestamp failed on update. [%d]"), g_nxccNodeId);
      }

      DBConnectionPoolReleaseConnection(hdb);
   }

   ClusterDebug(1, _T("Database update timestamp thread stopped"));
   return THREAD_OK;
}