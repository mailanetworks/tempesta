/**
 *		Tempesta FW
 *
 * Generic connection management.
 *
 * Copyright (C) 2012-2014 NatSys Lab. (info@natsys-lab.com).
 * Copyright (C) 2015 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITFWOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "classifier.h"
#include "client.h"
#include "connection.h"
#include "gfsm.h"
#include "log.h"
#include "server.h"

#include "sync_socket.h"

#define TFW_CONN_MAX_PROTOS	TFW_GFSM_FSM_N

static struct kmem_cache *conn_cache;
static TfwConnHooks *conn_hooks[TFW_CONN_MAX_PROTOS];

/*
 * ------------------------------------------------------------------------
 *  	Utilities
 * ------------------------------------------------------------------------
 */
static TfwConnection *
tfw_connection_alloc(int type)
{
	TfwConnection *c = kmem_cache_alloc(conn_cache,
					    GFP_ATOMIC | __GFP_ZERO);
	if (!c)
		return NULL;

	TFW_CONN_TYPE(c) = type;
	INIT_LIST_HEAD(&c->list);
	INIT_LIST_HEAD(&c->msg_queue);

	return c;
}

/**
 * TfwConnection must be CPU local, so do not synchronize this.
 */
static void
tfw_connection_free(TfwConnection *c)
{
	TFW_DBG("Free connection: %p\n", c);

	kmem_cache_free(conn_cache, c);
}

/*
 * ------------------------------------------------------------------------
 *  	Connection Downcalls
 * ------------------------------------------------------------------------
 */

/**
 * A downcall for new connection called to set necessary callbacks
 * when a traditional Sockets connect() is calling.
 *
 * @destructor Is a function placed to sk->sk_destruct.
 * The original callback is saved to TfwConnection->sk_destruct and the passed
 * function must call it manually.
 */
TfwConnection *
tfw_connection_new(struct sock *sk, int type,
		   void (*destructor)(struct sock *s))
{
	TfwConnection *conn;

	BUG_ON(!(type & (Conn_Clnt | Conn_Srv)));

	/* Type: connection direction BitwiseOR protocol. */
	if (sk->sk_user_data) {
		SsProto *proto = sk->sk_user_data;
		type |= proto->type;
	}

	conn = tfw_connection_alloc(type);
	if (!conn)
		return NULL;

	sk->sk_user_data = conn;
	if (destructor) {
		conn->sk_destruct = sk->sk_destruct;
		sk->sk_destruct = destructor;
	}
	conn->sock = sk;

	sock_set_flag(sk, SOCK_DBG);

	conn_hooks[TFW_CONN_TYPE2IDX(type)]->conn_init(conn);

	return conn;
}

static int
tfw_connection_close(struct sock *sk)
{
	TfwConnection *c = sk->sk_user_data;

	TFW_DBG("Close socket %p, conn=%p\n", sk, c);

	/*
	 * Classify the connection closing while all data structures
	 * are alive.
	 */
	if (tfw_classify_conn_close(sk) == TFW_BLOCK)
		return -EPERM;

	conn_hooks[TFW_CONN_TYPE2IDX(TFW_CONN_TYPE(c))]->conn_destruct(c);

	tfw_peer_del_conn(c->peer, &c->list);

	tfw_connection_free(c);

	sk->sk_user_data = NULL;

	return 0;
}

void
tfw_connection_send(TfwConnection *conn, TfwMsg *msg)
{
	ss_send(conn->sock, &msg->skb_list);
}

/*
 * ------------------------------------------------------------------------
 * 	Connection Upcalls
 * ------------------------------------------------------------------------
 */
/**
 * An upcall for new connection accepting.
 *
 * This is an upcall for new connection, i.e. we open the connection
 * passively. So this is client connection.
 */
static int
tfw_connection_new_upcall(struct sock *sk)
{
	TfwAddr addr;
	TfwConnection *conn;

	/* Classify the connection before any resource allocations. */
	if (tfw_classify_conn_estab(sk) == TFW_BLOCK)
		return -EPERM;

	conn = tfw_connection_new(sk, Conn_Clnt, tfw_client_put);
	if (!conn) {
		TFW_ERR("Cannot create new client connection\n");
		ss_close(sk);
		return -ENOMEM;
	}

	/*
	 * TODO: currently there is one to one socket-client
	 * mapping, which isn't appropriate since a client can
	 * have more than one socket with the server.
	 *
	 * We have too lookup the client by the socket and create a new one
	 * only if it's really new.
	 *
	 * Derive the client address from @sk and properly set @addr.
	 */
	memset(&addr, 0, sizeof(addr));
	if (!tfw_create_client(conn, &addr)) {
		TFW_ERR("Can't allocate a new client");
		ss_close(sk);
		return -EINVAL;
	}

	TFW_DBG("New client socket %p (state=%u)\n", sk, sk->sk_state);

	return 0;
}

static int
tfw_connection_recv(struct sock *sk, unsigned char *data, size_t len)
{
	TfwConnection *conn = sk->sk_user_data;

	return tfw_gfsm_dispatch(conn, data, len);
}

static int
tfw_connection_put_skb_to_msg(SsProto *proto, struct sk_buff *skb)
{
	TfwConnection *conn = (TfwConnection *)proto;

	if (!conn->msg) {
		int i = TFW_CONN_TYPE2IDX(TFW_CONN_TYPE(conn));
		conn->msg = conn_hooks[i]->conn_msg_alloc(conn);
		if (!conn->msg)
			return -ENOMEM;
		TFW_DBG("Link new msg %p with connection %p\n",
			conn->msg, conn);
	}

	TFW_DBG("Add skb %p to message %p\n", skb, conn->msg);

	ss_skb_queue_tail(&conn->msg->skb_list, skb);

	return 0;
}

static int
tfw_connection_postpone_skb(SsProto *proto, struct sk_buff *skb)
{
	TfwConnection *conn = (TfwConnection *)proto;

	TFW_DBG("postpone skb %p\n", skb);

	ss_skb_queue_tail(&conn->msg->skb_list, skb);

	return 0;
}

static SsHooks ssocket_hooks = {
	.connection_new		= tfw_connection_new_upcall,
	.connection_drop	= tfw_connection_close,
	.connection_recv	= tfw_connection_recv,
	.put_skb_to_msg		= tfw_connection_put_skb_to_msg,
	.postpone_skb		= tfw_connection_postpone_skb,
};

/*
 * ------------------------------------------------------------------------
 * 	Connection API (frontend for synchronous sockets) initialization
 * ------------------------------------------------------------------------
 */
void
tfw_connection_hooks_register(TfwConnHooks *hooks, int type)
{
	unsigned hid = TFW_CONN_TYPE2IDX(type);

	BUG_ON(hid >= TFW_CONN_MAX_PROTOS || conn_hooks[hid]);

	conn_hooks[hid] = hooks;
}

int __init
tfw_connection_init(void)
{
	int r;

	conn_cache = kmem_cache_create("tfw_conn_cache", sizeof(TfwConnection),
				       0, 0, NULL);
	if (!conn_cache)
		return -ENOMEM;

	r = ss_hooks_register(&ssocket_hooks);
	if (r)
		kmem_cache_destroy(conn_cache);

	return r;
}

void
tfw_connection_exit(void)
{
	ss_hooks_unregister(&ssocket_hooks);
	kmem_cache_destroy(conn_cache);
}
