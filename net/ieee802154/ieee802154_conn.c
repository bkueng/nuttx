/****************************************************************************
 * net/ieee802154/ieee802154_conn.c
 *
 *   Copyright (C) 2017 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <semaphore.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <arch/irq.h>

#include <nuttx/net/netconfig.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/wireless/ieee802154/ieee802154_mac.h>

#include "devif/devif.h"
#include "ieee802154/ieee802154.h"

#ifdef CONFIG_NET_IEEE802154

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The array containing all packet socket connections */

static struct ieee802154_conn_s
  g_ieee802154_connections[CONFIG_NET_IEEE802154_NCONNS];

/* A list of all free packet socket connections */

static dq_queue_t g_free_ieee802154_connections;
static sem_t g_free_sem;

/* A list of all allocated packet socket connections */

static dq_queue_t g_active_ieee802154_connections;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: _ieee802154_semtake() and _ieee802154_semgive()
 *
 * Description:
 *   Take/give semaphore
 *
 ****************************************************************************/

static inline void _ieee802154_semtake(sem_t *sem)
{
  /* Take the semaphore (perhaps waiting) */

  while (net_lockedwait(sem) != 0)
    {
      /* The only case that an error should occur here is if
       * the wait was awakened by a signal.
       */

      DEBUGASSERT(get_errno() == EINTR);
    }
}

#define _ieee802154_semgive(sem) sem_post(sem)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ieee802154_initialize()
 *
 * Description:
 *   Initialize the packet socket connection structures.  Called once and
 *   only from the network logic early in initialization.
 *
 ****************************************************************************/

void ieee802154_initialize(void)
{
  int i;

  /* Initialize the queues */

  dq_init(&g_free_ieee802154_connections);
  dq_init(&g_active_ieee802154_connections);
  sem_init(&g_free_sem, 0, 1);

  for (i = 0; i < CONFIG_NET_IEEE802154_NCONNS; i++)
    {
      /* Link each pre-allocated connection structure into the free list. */

      dq_addlast(&g_ieee802154_connections[i].node,
                 &g_free_ieee802154_connections);
    }
}

/****************************************************************************
 * Name: ieee802154_alloc()
 *
 * Description:
 *   Allocate a new, uninitialized packet socket connection structure. This
 *   is normally something done by the implementation of the socket() API
 *
 ****************************************************************************/

FAR struct ieee802154_conn_s *ieee802154_alloc(void)
{
  FAR struct ieee802154_conn_s *conn;

  /* The free list is only accessed from user, non-interrupt level and
   * is protected by a semaphore (that behaves like a mutex).
   */

  _ieee802154_semtake(&g_free_sem);
  conn = (FAR struct ieee802154_conn_s *)
    dq_remfirst(&g_free_ieee802154_connections);

  if (conn)
    {
      /* Enqueue the connection into the active list */

      dq_addlast(&conn->node, &g_active_ieee802154_connections);
    }

  _ieee802154_semgive(&g_free_sem);
  return conn;
}

/****************************************************************************
 * Name: ieee802154_free()
 *
 * Description:
 *   Free a packet socket connection structure that is no longer in use.
 *   This should be done by the implementation of close().
 *
 ****************************************************************************/

void ieee802154_free(FAR struct ieee802154_conn_s *conn)
{
  /* The free list is only accessed from user, non-interrupt level and
   * is protected by a semaphore (that behaves like a mutex).
   */

  DEBUGASSERT(conn->crefs == 0);

  _ieee802154_semtake(&g_free_sem);

  /* Remove the connection from the active list */

  dq_rem(&conn->node, &g_active_ieee802154_connections);

  /* Free the connection */

  dq_addlast(&conn->node, &g_free_ieee802154_connections);
  _ieee802154_semgive(&g_free_sem);
}

/****************************************************************************
 * Name: ieee802154_active()
 *
 * Description:
 *   Find a connection structure that is the appropriate
 *   connection to be used with the provided IEEE 802.15.4 header
 *
 * Assumptions:
 *   This function is called from network logic at with the network locked.
 *
 ****************************************************************************/

FAR struct ieee802154_conn_s *
  ieee802154_active(FAR const struct ieee802154_data_ind_s *meta)
{
  FAR struct ieee802154_conn_s *conn;

  DEBUGASSERT(meta != NULL);

  for (conn  = (FAR struct ieee802154_conn_s *)g_active_ieee802154_connections.head;
       conn != NULL;
       conn = (FAR struct ieee802154_conn_s *)conn->node.flink)
    {
      /* Does the destination address match the bound address of the socket. */
      /* REVISIT: Currently and explict address must be assigned.  Should we
       * support some moral equivalent to INADDR_ANY?
       */

      if (meta->dest.mode != conn->laddr.s_mode)
        {
          continue;
        }

      if (meta->dest.mode == IEEE802154_ADDRMODE_SHORT &&
          !IEEE802154_SADDRCMP(meta->dest.saddr, conn->laddr.s_saddr))
        {
          continue;
        }

      if (meta->dest.mode == IEEE802154_ADDRMODE_EXTENDED &&
          !IEEE802154_EADDRCMP(meta->dest.saddr, conn->laddr.s_eaddr))
        {
          continue;
        }

      /* Is the socket "connected?" to a remote peer?  If so, check if the
       * source address matches the connected remote adress.
       */

      switch (conn->raddr.s_mode)
        {
          case IEEE802154_ADDRMODE_NONE:
            return conn;  /* No.. accept the connection */

          case IEEE802154_ADDRMODE_SHORT:
            if (IEEE802154_SADDRCMP(meta->dest.saddr, conn->raddr.s_saddr))
              {
                return conn;
              }
            break;

          case IEEE802154_ADDRMODE_EXTENDED:
            if (IEEE802154_EADDRCMP(meta->dest.eaddr, conn->raddr.s_eaddr))
              {
                return conn;
              }
            break;

           default:
             nerr("ERROR: Invalid address mode: %u\n", conn->raddr.s_mode);
             return NULL;
        }
    }

  return conn;
}

/****************************************************************************
 * Name: ieee802154_nextconn()
 *
 * Description:
 *   Traverse the list of allocated packet connections
 *
 * Assumptions:
 *   This function is called from network logic at with the network locked.
 *
 ****************************************************************************/

FAR struct ieee802154_conn_s *
  ieee802154_nextconn(FAR struct ieee802154_conn_s *conn)
{
  if (!conn)
    {
      return (FAR struct ieee802154_conn_s *)
        g_active_ieee802154_connections.head;
    }
  else
    {
      return (FAR struct ieee802154_conn_s *)conn->node.flink;
    }
}

#endif /* CONFIG_NET_IEEE802154 */