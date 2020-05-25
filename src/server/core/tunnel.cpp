/*
** NetXMS - Network Management System
** Copyright (C) 2003-2017 Victor Kirhenshtein
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
** File: tunnel.cpp
**/

#include "nxcore.h"
#include <agent_tunnel.h>

#define MAX_MSG_SIZE    268435456

/**
 * Tunnel registration
 */
static RefCountHashMap<UINT32, AgentTunnel> s_boundTunnels(true);
static Array s_unboundTunnels(16, 16, false);
static Mutex s_tunnelListLock;

/**
 * Callback for closing old tunnel
 */
static void CloseTunnelCallback(void *arg)
{
   ((AgentTunnel *)arg)->decRefCount();
}

/**
 * Register tunnel
 */
static void RegisterTunnel(AgentTunnel *tunnel)
{
   AgentTunnel *oldTunnel = NULL;
   tunnel->incRefCount();
   s_tunnelListLock.lock();
   if (tunnel->isBound())
   {
      oldTunnel = s_boundTunnels.get(tunnel->getNodeId());
      s_boundTunnels.set(tunnel->getNodeId(), tunnel);
      tunnel->decRefCount(); // set already increased ref count
   }
   else
   {
      s_unboundTunnels.add(tunnel);
   }
   s_tunnelListLock.unlock();

   // Execute decRefCount for old tunnel on separate thread to avoid
   // possible tunnel manager thread lock on old tunnel receiver thread join
   if (oldTunnel != NULL)
      ThreadPoolExecute(g_mainThreadPool, CloseTunnelCallback, oldTunnel);
}

/**
 * Unregister tunnel
 */
static void UnregisterTunnel(AgentTunnel *tunnel)
{
   s_tunnelListLock.lock();
   if (tunnel->isBound())
   {
      AgentTunnel *curr = s_boundTunnels.get(tunnel->getNodeId());
      if (curr == tunnel)
         s_boundTunnels.remove(tunnel->getNodeId());
      else
         curr->decRefCount();  // ref count was increased by get
   }
   else
   {
      s_unboundTunnels.remove(tunnel);
   }
   s_tunnelListLock.unlock();

   ThreadPoolExecute(g_mainThreadPool, CloseTunnelCallback, tunnel);
}

/**
 * Get tunnel for node. Caller must decrease reference counter on tunnel.
 */
AgentTunnel *GetTunnelForNode(UINT32 nodeId)
{
   s_tunnelListLock.lock();
   AgentTunnel *t = s_boundTunnels.get(nodeId);
   s_tunnelListLock.unlock();
   return t;
}

/**
 * Bind agent tunnel
 */
UINT32 BindAgentTunnel(UINT32 tunnelId, UINT32 nodeId)
{
   AgentTunnel *tunnel = NULL;
   s_tunnelListLock.lock();
   for(int i = 0; i < s_unboundTunnels.size(); i++)
   {
      if (((AgentTunnel *)s_unboundTunnels.get(i))->getId() == tunnelId)
      {
         tunnel = (AgentTunnel *)s_unboundTunnels.get(i);
         tunnel->incRefCount();
         break;
      }
   }
   s_tunnelListLock.unlock();

   if (tunnel == NULL)
   {
      nxlog_debug(4, _T("BindAgentTunnel: unbound tunnel with ID %d not found"), tunnelId);
      return RCC_INVALID_TUNNEL_ID;
   }

   UINT32 rcc = tunnel->bind(nodeId);
   tunnel->decRefCount();
   return rcc;
}

/**
 * Get list of unbound agent tunnels into NXCP message
 */
void GetUnboundAgentTunnels(NXCPMessage *msg)
{
   s_tunnelListLock.lock();
   UINT32 fieldId = VID_ELEMENT_LIST_BASE;
   for(int i = 0; i < s_unboundTunnels.size(); i++)
   {
      ((AgentTunnel *)s_unboundTunnels.get(i))->fillMessage(msg, fieldId);
      fieldId += 10;
   }
   msg->setField(VID_NUM_ELEMENTS, (UINT32)s_unboundTunnels.size());
   s_tunnelListLock.unlock();
}

/**
 * Show tunnels in console
 */
void ShowAgentTunnels(CONSOLE_CTX console)
{
   s_tunnelListLock.lock();

   ConsolePrintf(console,
            _T("\n\x1b[1mBOUND TUNNELS\x1b[0m\n")
            _T("ID   | Node ID | Peer IP Address          | System Name              | Platform Name    | Agent Version\n")
            _T("-----+---------+--------------------------+--------------------------+------------------+------------------------\n"));
   Iterator<AgentTunnel> *it = s_boundTunnels.iterator();
   while(it->hasNext())
   {
      AgentTunnel *t = it->next();
      TCHAR ipAddrBuffer[64];
      ConsolePrintf(console, _T("%4d | %7u | %-24s | %-24s | %-16s | %s\n"), t->getId(), t->getNodeId(), t->getAddress().toString(ipAddrBuffer), t->getSystemName(), t->getPlatformName(), t->getAgentVersion());
   }
   delete it;

   ConsolePrintf(console,
            _T("\n\x1b[1mUNBOUND TUNNELS\x1b[0m\n")
            _T("ID   | Peer IP Address          | System Name              | Platform Name    | Agent Version\n")
            _T("-----+--------------------------+--------------------------+------------------+------------------------\n"));
   for(int i = 0; i < s_unboundTunnels.size(); i++)
   {
      const AgentTunnel *t = (AgentTunnel *)s_unboundTunnels.get(i);
      TCHAR ipAddrBuffer[64];
      ConsolePrintf(console, _T("%4d | %-24s | %-24s | %-16s | %s\n"), t->getId(), t->getAddress().toString(ipAddrBuffer), t->getSystemName(), t->getPlatformName(), t->getAgentVersion());
   }

   s_tunnelListLock.unlock();
}

/**
 * Next free tunnel ID
 */
static VolatileCounter s_nextTunnelId = 0;

/**
 * Agent tunnel constructor
 */
AgentTunnel::AgentTunnel(SSL_CTX *context, SSL *ssl, SOCKET sock, const InetAddress& addr, UINT32 nodeId) : RefCountObject(), m_channels(true)
{
   m_id = InterlockedIncrement(&s_nextTunnelId);
   m_address = addr;
   m_socket = sock;
   m_context = context;
   m_ssl = ssl;
   m_sslLock = MutexCreate();
   m_recvThread = INVALID_THREAD_HANDLE;
   m_requestId = 0;
   m_nodeId = nodeId;
   m_state = AGENT_TUNNEL_INIT;
   m_systemName = NULL;
   m_platformName = NULL;
   m_systemInfo = NULL;
   m_agentVersion = NULL;
   m_bindRequestId = 0;
   m_channelLock = MutexCreate();
}

/**
 * Agent tunnel destructor
 */
AgentTunnel::~AgentTunnel()
{
   m_channels.clear();
   if (m_socket != INVALID_SOCKET)
      shutdown(m_socket, SHUT_RDWR);
   ThreadJoin(m_recvThread);
   SSL_CTX_free(m_context);
   SSL_free(m_ssl);
   MutexDestroy(m_sslLock);
   closesocket(m_socket);
   free(m_systemName);
   free(m_platformName);
   free(m_systemInfo);
   free(m_agentVersion);
   MutexDestroy(m_channelLock);
   debugPrintf(4, _T("Tunnel destroyed"));
}

/**
 * Debug output
 */
void AgentTunnel::debugPrintf(int level, const TCHAR *format, ...)
{
   va_list args;
   va_start(args, format);
   TCHAR buffer[8192];
   _vsntprintf(buffer, 8192, format, args);
   va_end(args);
   nxlog_debug(level, _T("[TUN-%d] %s"), m_id, buffer);
}

/**
 * Tunnel receiver thread
 */
void AgentTunnel::recvThread()
{
   TlsMessageReceiver receiver(m_socket, m_ssl, m_sslLock, 4096, MAX_MSG_SIZE);
   while(true)
   {
      MessageReceiverResult result;
      NXCPMessage *msg = receiver.readMessage(60000, &result);
      if (result != MSGRECV_SUCCESS)
      {
         if (result == MSGRECV_CLOSED)
            debugPrintf(4, _T("Tunnel closed by peer"));
         else
            debugPrintf(4, _T("Communication error (%s)"), AbstractMessageReceiver::resultToText(result));
         break;
      }

      if (nxlog_get_debug_level() >= 6)
      {
         TCHAR buffer[64];
         debugPrintf(6, _T("Received message %s"), NXCPMessageCodeName(msg->getCode(), buffer));
      }

      switch(msg->getCode())
      {
         case CMD_KEEPALIVE:
            {
               NXCPMessage response(CMD_KEEPALIVE, msg->getId());
               sendMessage(&response);
            }
            break;
         case CMD_SETUP_AGENT_TUNNEL:
            setup(msg);
            break;
         case CMD_REQUEST_CERTIFICATE:
            processCertificateRequest(msg);
            break;
         case CMD_CHANNEL_DATA:
            if (msg->isBinary())
            {
               MutexLock(m_channelLock);
               AgentTunnelCommChannel *channel = m_channels.get(msg->getId());
               MutexUnlock(m_channelLock);
               if (channel != NULL)
               {
                  channel->putData(msg->getBinaryData(), msg->getBinaryDataSize());
                  channel->decRefCount();
               }
               else
               {
                  debugPrintf(6, _T("Received channel data for non-existing channel %u"), msg->getId());
               }
            }
            break;
         default:
            m_queue.put(msg);
            msg = NULL; // prevent message deletion
            break;
      }
      delete msg;
   }
   UnregisterTunnel(this);
   debugPrintf(5, _T("Receiver thread stopped"));
}

/**
 * Tunnel receiver thread starter
 */
THREAD_RESULT THREAD_CALL AgentTunnel::recvThreadStarter(void *arg)
{
   ((AgentTunnel *)arg)->recvThread();
   return THREAD_OK;
}

/**
 * Send message on tunnel
 */
bool AgentTunnel::sendMessage(NXCPMessage *msg)
{
   if (nxlog_get_debug_level() >= 6)
   {
      TCHAR buffer[64];
      debugPrintf(6, _T("Sending message %s"), NXCPMessageCodeName(msg->getCode(), buffer));
   }
   NXCP_MESSAGE *data = msg->createMessage(true);
   MutexLock(m_sslLock);
   bool success = (SSL_write(m_ssl, data, ntohl(data->size)) == ntohl(data->size));
   MutexUnlock(m_sslLock);
   free(data);
   return success;
}

/**
 * Start tunel
 */
void AgentTunnel::start()
{
   debugPrintf(4, _T("Tunnel started"));
   m_recvThread = ThreadCreateEx(AgentTunnel::recvThreadStarter, 0, this);
}

/**
 * Process setup request
 */
void AgentTunnel::setup(const NXCPMessage *request)
{
   NXCPMessage response;
   response.setCode(CMD_REQUEST_COMPLETED);
   response.setId(request->getId());

   if (m_state == AGENT_TUNNEL_INIT)
   {
      m_systemName = request->getFieldAsString(VID_SYS_NAME);
      m_systemInfo = request->getFieldAsString(VID_SYS_DESCRIPTION);
      m_platformName = request->getFieldAsString(VID_PLATFORM_NAME);
      m_agentVersion = request->getFieldAsString(VID_AGENT_VERSION);

      m_state = (m_nodeId != 0) ? AGENT_TUNNEL_BOUND : AGENT_TUNNEL_UNBOUND;
      response.setField(VID_RCC, ERR_SUCCESS);
      response.setField(VID_IS_ACTIVE, m_state == AGENT_TUNNEL_BOUND);

      debugPrintf(3, _T("%s tunnel initialized"), (m_state == AGENT_TUNNEL_BOUND) ? _T("Bound") : _T("Unbound"));
      debugPrintf(5, _T("   System name:        %s"), m_systemName);
      debugPrintf(5, _T("   System information: %s"), m_systemInfo);
      debugPrintf(5, _T("   Platform name:      %s"), m_platformName);
      debugPrintf(5, _T("   Agent version:      %s"), m_agentVersion);
   }
   else
   {
      response.setField(VID_RCC, ERR_OUT_OF_STATE_REQUEST);
   }

   sendMessage(&response);
}

/**
 * Bind tunnel to node
 */
UINT32 AgentTunnel::bind(UINT32 nodeId)
{
   if ((m_state != AGENT_TUNNEL_UNBOUND) || (m_bindRequestId != 0))
      return RCC_OUT_OF_STATE_REQUEST;

   Node *node = (Node *)FindObjectById(nodeId, OBJECT_NODE);
   if (node == NULL)
      return RCC_INVALID_OBJECT_ID;

   NXCPMessage msg;
   msg.setCode(CMD_BIND_AGENT_TUNNEL);
   msg.setId(InterlockedIncrement(&m_requestId));
   msg.setField(VID_SERVER_ID, g_serverId);
   msg.setField(VID_GUID, node->getGuid());

   m_bindRequestId = msg.getId();
   m_bindGuid = node->getGuid();
   sendMessage(&msg);

   NXCPMessage *response = waitForMessage(CMD_REQUEST_COMPLETED, msg.getId());
   if (response == NULL)
      return RCC_TIMEOUT;

   UINT32 rcc = response->getFieldAsUInt32(VID_RCC);
   delete response;
   if (rcc == ERR_SUCCESS)
   {
      debugPrintf(4, _T("Bind successful, resetting tunnel"));
      msg.setCode(CMD_RESET_TUNNEL);
      msg.setId(InterlockedIncrement(&m_requestId));
      sendMessage(&msg);
   }
   else
   {
      debugPrintf(4, _T("Bind failed: agent error %d (%s)"), rcc, AgentErrorCodeToText(rcc));
   }
   return AgentErrorToRCC(rcc);
}

/**
 * Process certificate request
 */
void AgentTunnel::processCertificateRequest(NXCPMessage *request)
{
   NXCPMessage response(CMD_NEW_CERTIFICATE, request->getId());

   if ((request->getId() == m_bindRequestId) && (m_bindRequestId != 0) && (m_state == AGENT_TUNNEL_UNBOUND))
   {
      size_t certRequestLen;
      const BYTE *certRequestData = request->getBinaryFieldPtr(VID_CERTIFICATE, &certRequestLen);
      if (certRequestData != NULL)
      {
         X509_REQ *certRequest = d2i_X509_REQ(NULL, &certRequestData, (long)certRequestLen);
         if (certRequest != NULL)
         {
            char *cn = m_bindGuid.toString().getUTF8String();
            X509 *cert = IssueCertificate(certRequest, cn, 365);
            free(cn);
            if (cert != NULL)
            {
               BYTE *buffer = NULL;
               int len = i2d_X509(cert, &buffer);
               if (len > 0)
               {
                  response.setField(VID_RCC, ERR_SUCCESS);
                  response.setField(VID_CERTIFICATE, buffer, len);
                  OPENSSL_free(buffer);
                  debugPrintf(4, _T("Certificate issued"));
               }
               else
               {
                  debugPrintf(4, _T("Cannot encode certificate"));
                  response.setField(VID_RCC, ERR_ENCRYPTION_ERROR);
               }
               X509_free(cert);
            }
            else
            {
               debugPrintf(4, _T("Cannot issue certificate"));
               response.setField(VID_RCC, ERR_ENCRYPTION_ERROR);
            }
         }
         else
         {
            debugPrintf(4, _T("Cannot decode certificate request data"));
            response.setField(VID_RCC, ERR_BAD_ARGUMENTS);
         }
      }
      else
      {
         debugPrintf(4, _T("Missing certificate request data"));
         response.setField(VID_RCC, ERR_BAD_ARGUMENTS);
      }
   }
   else
   {
      response.setField(VID_RCC, ERR_OUT_OF_STATE_REQUEST);
   }

   sendMessage(&response);
}

/**
 * Create channel
 */
AgentTunnelCommChannel *AgentTunnel::createChannel()
{
   NXCPMessage request(CMD_CREATE_CHANNEL, InterlockedIncrement(&m_requestId));
   sendMessage(&request);
   NXCPMessage *response = waitForMessage(CMD_REQUEST_COMPLETED, request.getId());
   if (response == NULL)
   {
      debugPrintf(4, _T("createChannel: request timeout"));
      return NULL;
   }

   UINT32 rcc = response->getFieldAsUInt32(VID_RCC);
   if (rcc != ERR_SUCCESS)
   {
      delete response;
      debugPrintf(4, _T("createChannel: agent error %d (%s)"), rcc, AgentErrorCodeToText(rcc));
      return NULL;
   }

   AgentTunnelCommChannel *channel = new AgentTunnelCommChannel(this, response->getFieldAsUInt32(VID_CHANNEL_ID));
   MutexLock(m_channelLock);
   m_channels.set(channel->getId(), channel);
   MutexUnlock(m_channelLock);
   debugPrintf(4, _T("createChannel: new channel created (ID=%d)"), channel->getId());
   return channel;
}

/**
 * Close channel
 */
void AgentTunnel::closeChannel(AgentTunnelCommChannel *channel)
{
   MutexLock(m_channelLock);
   m_channels.remove(channel->getId());
   MutexUnlock(m_channelLock);

   // Inform agent that channel is closing
   NXCPMessage msg(CMD_CLOSE_CHANNEL, InterlockedIncrement(&m_requestId));
   msg.setField(VID_CHANNEL_ID, channel->getId());
   sendMessage(&msg);
}

/**
 * Send channel data
 */
int AgentTunnel::sendChannelData(UINT32 id, const void *data, size_t len)
{
   NXCP_MESSAGE *msg = CreateRawNXCPMessage(CMD_CHANNEL_DATA, id, 0, data, len, NULL, false);
   MutexLock(m_sslLock);
   int rc = SSL_write(m_ssl, msg, ntohl(msg->size));
   if (rc == ntohl(msg->size))
      rc = (int)len;  // adjust number of bytes to exclude tunnel overhead
   MutexUnlock(m_sslLock);
   free(msg);
   return rc;
}

/**
 * Fill NXCP message with tunnel data
 */
void AgentTunnel::fillMessage(NXCPMessage *msg, UINT32 baseId) const
{
   msg->setField(baseId, m_id);
   msg->setField(baseId + 1, m_nodeId);
   msg->setField(baseId + 2, m_systemName);
   msg->setField(baseId + 3, m_systemInfo);
   msg->setField(baseId + 4, m_platformName);
   msg->setField(baseId + 5, m_agentVersion);
}

/**
 * Channel constructor
 */
AgentTunnelCommChannel::AgentTunnelCommChannel(AgentTunnel *tunnel, UINT32 id)
{
   m_tunnel = tunnel;
   m_id = id;
   m_active = true;
   m_allocated = 256 * 1024;
   m_head = 0;
   m_size = 0;
   m_buffer = (BYTE *)malloc(m_allocated);
   m_bufferLock = MutexCreate();
   m_dataCondition = ConditionCreate(TRUE);
}

/**
 * Channel destructor
 */
AgentTunnelCommChannel::~AgentTunnelCommChannel()
{
   free(m_buffer);
   MutexDestroy(m_bufferLock);
   ConditionDestroy(m_dataCondition);
}

/**
 * Send data
 */
int AgentTunnelCommChannel::send(const void *data, size_t size, MUTEX mutex)
{
   return m_active ? m_tunnel->sendChannelData(m_id, data, size) : -1;
}

/**
 * Receive data
 */
int AgentTunnelCommChannel::recv(void *buffer, size_t size, UINT32 timeout)
{
   if (!m_active)
      return 0;

   if (!ConditionWait(m_dataCondition, timeout))
      return -2;

   MutexLock(m_bufferLock);
   size_t bytes = MIN(size, m_size);
   memcpy(buffer, &m_buffer[m_head], bytes);
   m_size -= bytes;
   if (m_size == 0)
   {
      m_head = 0;
      ConditionReset(m_dataCondition);
   }
   else
   {
      m_head += bytes;
   }
   MutexUnlock(m_bufferLock);
   return (int)bytes;
}

/**
 * Poll for data
 */
int AgentTunnelCommChannel::poll(UINT32 timeout, bool write)
{
   if (write)
      return 1;

   if (!m_active)
      return -1;

   return ConditionWait(m_dataCondition, timeout) ? 1 : 0;
}

/**
 * Shutdown channel
 */
int AgentTunnelCommChannel::shutdown()
{
   m_active = false;
   return 0;
}

/**
 * Close channel
 */
void AgentTunnelCommChannel::close()
{
   m_active = false;
   m_tunnel->closeChannel(this);
}

/**
 * Put data into buffer
 */
void AgentTunnelCommChannel::putData(const BYTE *data, size_t size)
{
   MutexLock(m_bufferLock);
   if (m_head + m_size + size > m_allocated)
   {
      m_allocated = m_head + m_size + size;
      m_buffer = (BYTE *)realloc(m_buffer, m_allocated);
   }
   memcpy(&m_buffer[m_head + m_size], data, size);
   m_size += size;
   MutexUnlock(m_bufferLock);
   ConditionSet(m_dataCondition);
}

/**
 * Incoming connection data
 */
struct ConnectionRequest
{
   SOCKET sock;
   InetAddress addr;
};

/**
 * Setup tunnel
 */
static void SetupTunnel(void *arg)
{
   ConnectionRequest *request = (ConnectionRequest *)arg;

   SSL_CTX *context = NULL;
   SSL *ssl = NULL;
   AgentTunnel *tunnel = NULL;
   int rc;
   UINT32 nodeId = 0;
   X509 *cert = NULL;

   // Setup secure connection
   const SSL_METHOD *method = SSLv23_method();
   if (method == NULL)
   {
      nxlog_debug(4, _T("SetupTunnel(%s): cannot obtain TLS method"), (const TCHAR *)request->addr.toString());
      goto failure;
   }

   context = SSL_CTX_new(method);
   if (context == NULL)
   {
      nxlog_debug(4, _T("SetupTunnel(%s): cannot create TLS context"), (const TCHAR *)request->addr.toString());
      goto failure;
   }
   SSL_CTX_set_options(context, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
   if (!SetupServerTlsContext(context))
   {
      nxlog_debug(4, _T("SetupTunnel(%s): cannot configure TLS context"), (const TCHAR *)request->addr.toString());
      goto failure;
   }

   ssl = SSL_new(context);
   if (ssl == NULL)
   {
      nxlog_debug(4, _T("SetupTunnel(%s): cannot create SSL object"), (const TCHAR *)request->addr.toString());
      goto failure;
   }

   SSL_set_accept_state(ssl);
   SSL_set_fd(ssl, (int)request->sock);

retry:
   rc = SSL_do_handshake(ssl);
   if (rc != 1)
   {
      int sslErr = SSL_get_error(ssl, rc);
      if (sslErr == SSL_ERROR_WANT_READ)
      {
         SocketPoller poller;
         poller.add(request->sock);
         if (poller.poll(5000) > 0)
            goto retry;
         nxlog_debug(4, _T("SetupTunnel(%s): TLS handshake failed (timeout)"), (const TCHAR *)request->addr.toString());
      }
      else
      {
         char buffer[128];
         nxlog_debug(4, _T("SetupTunnel(%s): TLS handshake failed (%hs)"),
                     (const TCHAR *)request->addr.toString(), ERR_error_string(sslErr, buffer));
      }
      goto failure;
   }

   cert = SSL_get_peer_certificate(ssl);
   if (cert != NULL)
   {
      if (ValidateAgentCertificate(cert))
      {
         TCHAR cn[256];
         if (GetCertificateCN(cert, cn, 256))
         {
            uuid guid = uuid::parse(cn);
            if (!guid.isNull())
            {
               Node *node = (Node *)FindObjectByGUID(guid, OBJECT_NODE);
               if (node != NULL)
               {
                  nxlog_debug(4, _T("SetupTunnel(%s): Tunnel attached to node %s [%d]"), (const TCHAR *)request->addr.toString(), node->getName(), node->getId());
                  nodeId = node->getId();
               }
               else
               {
                  nxlog_debug(4, _T("SetupTunnel(%s): Node with GUID %s not found"), (const TCHAR *)request->addr.toString(), (const TCHAR *)guid.toString());
               }
            }
            else
            {
               nxlog_debug(4, _T("SetupTunnel(%s): Certificate CN is not a valid GUID"), (const TCHAR *)request->addr.toString());
            }
         }
         else
         {
            nxlog_debug(4, _T("SetupTunnel(%s): Cannot get certificate CN"), (const TCHAR *)request->addr.toString());
         }
      }
      else
      {
         nxlog_debug(4, _T("SetupTunnel(%s): Agent certificate validation failed"), (const TCHAR *)request->addr.toString());
      }
      X509_free(cert);
   }
   else
   {
      nxlog_debug(4, _T("SetupTunnel(%s): Agent certificate not provided"), (const TCHAR *)request->addr.toString());
   }

   tunnel = new AgentTunnel(context, ssl, request->sock, request->addr, nodeId);
   RegisterTunnel(tunnel);
   tunnel->start();
   tunnel->decRefCount();

   delete request;
   return;

failure:
   if (ssl != NULL)
      SSL_free(ssl);
   if (context != NULL)
      SSL_CTX_free(context);
   shutdown(request->sock, SHUT_RDWR);
   closesocket(request->sock);
   delete request;
}

/**
 * Tunnel listener
 */
THREAD_RESULT THREAD_CALL TunnelListener(void *arg)
{
   UINT16 port = (UINT16)ConfigReadULong(_T("AgentTunnelListenPort"), 4703);

   // Create socket(s)
   SOCKET hSocket = socket(AF_INET, SOCK_STREAM, 0);
#ifdef WITH_IPV6
   SOCKET hSocket6 = socket(AF_INET6, SOCK_STREAM, 0);
#endif
   if ((hSocket == INVALID_SOCKET)
#ifdef WITH_IPV6
       && (hSocket6 == INVALID_SOCKET)
#endif
      )
   {
      nxlog_write(MSG_SOCKET_FAILED, EVENTLOG_ERROR_TYPE, "s", _T("TunnelListener"));
      return THREAD_OK;
   }

   SetSocketExclusiveAddrUse(hSocket);
   SetSocketReuseFlag(hSocket);
#ifndef _WIN32
   fcntl(hSocket, F_SETFD, fcntl(hSocket, F_GETFD) | FD_CLOEXEC);
#endif

#ifdef WITH_IPV6
   SetSocketExclusiveAddrUse(hSocket6);
   SetSocketReuseFlag(hSocket6);
#ifndef _WIN32
   fcntl(hSocket6, F_SETFD, fcntl(hSocket6, F_GETFD) | FD_CLOEXEC);
#endif
#ifdef IPV6_V6ONLY
   int on = 1;
   setsockopt(hSocket6, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&on, sizeof(int));
#endif
#endif

   // Fill in local address structure
   struct sockaddr_in servAddr;
   memset(&servAddr, 0, sizeof(struct sockaddr_in));
   servAddr.sin_family = AF_INET;

#ifdef WITH_IPV6
   struct sockaddr_in6 servAddr6;
   memset(&servAddr6, 0, sizeof(struct sockaddr_in6));
   servAddr6.sin6_family = AF_INET6;
#endif

   if (!_tcscmp(g_szListenAddress, _T("*")))
   {
      servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
#ifdef WITH_IPV6
      memset(servAddr6.sin6_addr.s6_addr, 0, 16);
#endif
   }
   else
   {
      InetAddress bindAddress = InetAddress::resolveHostName(g_szListenAddress, AF_INET);
      if (bindAddress.isValid() && (bindAddress.getFamily() == AF_INET))
      {
         servAddr.sin_addr.s_addr = htonl(bindAddress.getAddressV4());
      }
      else
      {
         servAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      }
#ifdef WITH_IPV6
      bindAddress = InetAddress::resolveHostName(g_szListenAddress, AF_INET6);
      if (bindAddress.isValid() && (bindAddress.getFamily() == AF_INET6))
      {
         memcpy(servAddr6.sin6_addr.s6_addr, bindAddress.getAddressV6(), 16);
      }
      else
      {
         memset(servAddr6.sin6_addr.s6_addr, 0, 15);
         servAddr6.sin6_addr.s6_addr[15] = 1;
      }
#endif
   }
   servAddr.sin_port = htons(port);
#ifdef WITH_IPV6
   servAddr6.sin6_port = htons(port);
#endif

   // Bind socket
   TCHAR buffer[64];
   int bindFailures = 0;
   nxlog_debug(1, _T("Trying to bind on %s:%d"), SockaddrToStr((struct sockaddr *)&servAddr, buffer), ntohs(servAddr.sin_port));
   if (bind(hSocket, (struct sockaddr *)&servAddr, sizeof(struct sockaddr_in)) != 0)
   {
      nxlog_write(MSG_BIND_ERROR, EVENTLOG_ERROR_TYPE, "e", WSAGetLastError());
      bindFailures++;
   }

#ifdef WITH_IPV6
   nxlog_debug(1, _T("Trying to bind on [%s]:%d"), SockaddrToStr((struct sockaddr *)&servAddr6, buffer), ntohs(servAddr6.sin6_port));
   if (bind(hSocket6, (struct sockaddr *)&servAddr6, sizeof(struct sockaddr_in6)) != 0)
   {
      nxlog_write(MSG_BIND_ERROR, EVENTLOG_ERROR_TYPE, "dse", port, _T("TunnelListener"), WSAGetLastError());
      bindFailures++;
   }
#else
   bindFailures++;
#endif

   // Abort if cannot bind to socket
   if (bindFailures == 2)
   {
      return THREAD_OK;
   }

   // Set up queue
   if (listen(hSocket, SOMAXCONN) == 0)
   {
      nxlog_write(MSG_LISTENING_FOR_AGENTS, NXLOG_INFO, "ad", ntohl(servAddr.sin_addr.s_addr), port);
   }
   else
   {
      closesocket(hSocket);
      hSocket = INVALID_SOCKET;
   }
#ifdef WITH_IPV6
   if (listen(hSocket6, SOMAXCONN) == 0)
   {
      nxlog_write(MSG_LISTENING_FOR_AGENTS, NXLOG_INFO, "Hd", servAddr6.sin6_addr.s6_addr, port);
   }
   else
   {
      closesocket(hSocket6);
      hSocket6 = INVALID_SOCKET;
   }
#endif

   // Wait for connection requests
   SocketPoller sp;
   int errorCount = 0;
   while(!(g_flags & AF_SHUTDOWN))
   {
      sp.reset();
      if (hSocket != INVALID_SOCKET)
         sp.add(hSocket);
#ifdef WITH_IPV6
      if (hSocket6 != INVALID_SOCKET)
         sp.add(hSocket6);
#endif

      int nRet = sp.poll(1000);
      if ((nRet > 0) && (!(g_flags & AF_SHUTDOWN)))
      {
         char clientAddr[128];
         socklen_t size = 128;
#ifdef WITH_IPV6
         SOCKET hClientSocket = accept(sp.isSet(hSocket) ? hSocket : hSocket6, (struct sockaddr *)clientAddr, &size);
#else
         SOCKET hClientSocket = accept(hSocket, (struct sockaddr *)clientAddr, &size);
#endif
         if (hClientSocket == INVALID_SOCKET)
         {
            int error = WSAGetLastError();

            if (error != WSAEINTR)
               nxlog_write(MSG_ACCEPT_ERROR, NXLOG_ERROR, "e", error);
            errorCount++;
            if (errorCount > 1000)
            {
               nxlog_write(MSG_TOO_MANY_ACCEPT_ERRORS, NXLOG_WARNING, NULL);
               errorCount = 0;
            }
            ThreadSleepMs(500);
            continue;
         }

         // Socket should be closed on successful exec
#ifndef _WIN32
         fcntl(hClientSocket, F_SETFD, fcntl(hClientSocket, F_GETFD) | FD_CLOEXEC);
#endif

         errorCount = 0;     // Reset consecutive errors counter
         InetAddress addr = InetAddress::createFromSockaddr((struct sockaddr *)clientAddr);
         nxlog_debug(5, _T("TunnelListener: incoming connection from %s"), addr.toString(buffer));

         ConnectionRequest *request = new ConnectionRequest();
         request->sock = hClientSocket;
         request->addr = addr;
         ThreadPoolExecute(g_mainThreadPool, SetupTunnel, request);
      }
      else if (nRet == -1)
      {
         int error = WSAGetLastError();

         // On AIX, select() returns ENOENT after SIGINT for unknown reason
#ifdef _WIN32
         if (error != WSAEINTR)
#else
         if ((error != EINTR) && (error != ENOENT))
#endif
         {
            ThreadSleepMs(100);
         }
      }
   }

   closesocket(hSocket);
#ifdef WITH_IPV6
   closesocket(hSocket6);
#endif
   nxlog_debug(1, _T("Tunnel listener thread terminated"));
   return THREAD_OK;
}

/**
 * Close all active agent tunnels
 */
void CloseAgentTunnels()
{
}
