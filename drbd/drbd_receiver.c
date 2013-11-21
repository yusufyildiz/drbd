/*
   drbd_receiver.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <linux/module.h>

#include <asm/uaccess.h>
#include <net/sock.h>

#include <linux/drbd.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/slab.h>
#include <linux/pkt_sched.h>
#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <net/ipv6.h>
#include "drbd_int.h"
#include "drbd_protocol.h"
#include "drbd_req.h"
#include "drbd_vli.h"
#include <linux/scatterlist.h>

#define PRO_FEATURES (FF_TRIM)

struct flush_work {
	struct drbd_work w;
	struct drbd_device *device;
	struct drbd_epoch *epoch;
};

struct packet_info {
	enum drbd_packet cmd;
	unsigned int size;
	int vnr;
	void *data;
};

enum finish_epoch {
	FE_STILL_LIVE,
	FE_DESTROYED,
	FE_RECYCLED,
};

struct listener {
	struct kref kref;
	struct drbd_resource *resource;
	struct socket *s_listen;
	struct sockaddr_storage listen_addr;
	void (*original_sk_state_change)(struct sock *sk);
	struct list_head list; /* link for resource->listeners */
	struct list_head waiters; /* list head for waiter structs*/
	int pending_accepts;
};

struct waiter {
	struct drbd_connection *connection;
	wait_queue_head_t wait;
	struct list_head list;
	struct listener *listener;
	struct socket *socket;
};

static int drbd_do_features(struct drbd_connection *connection);
static int drbd_do_auth(struct drbd_connection *connection);
static int drbd_disconnected(struct drbd_peer_device *);

static enum finish_epoch drbd_may_finish_epoch(struct drbd_connection *, struct drbd_epoch *, enum epoch_event);
static int e_end_block(struct drbd_work *, int);
static void cleanup_unacked_peer_requests(struct drbd_connection *connection);
static void cleanup_peer_ack_list(struct drbd_connection *connection);
static u64 node_ids_to_bitmap(struct drbd_device *device, u64 node_ids);

static struct drbd_epoch *previous_epoch(struct drbd_connection *connection, struct drbd_epoch *epoch)
{
	struct drbd_epoch *prev;
	spin_lock(&connection->epoch_lock);
	prev = list_entry(epoch->list.prev, struct drbd_epoch, list);
	if (prev == epoch || prev == connection->current_epoch)
		prev = NULL;
	spin_unlock(&connection->epoch_lock);
	return prev;
}

#define GFP_TRY	(__GFP_HIGHMEM | __GFP_NOWARN)

/*
 * some helper functions to deal with single linked page lists,
 * page->private being our "next" pointer.
 */

/* If at least n pages are linked at head, get n pages off.
 * Otherwise, don't modify head, and return NULL.
 * Locking is the responsibility of the caller.
 */
static struct page *page_chain_del(struct page **head, int n)
{
	struct page *page;
	struct page *tmp;

	BUG_ON(!n);
	BUG_ON(!head);

	page = *head;

	if (!page)
		return NULL;

	while (page) {
		tmp = page_chain_next(page);
		if (--n == 0)
			break; /* found sufficient pages */
		if (tmp == NULL)
			/* insufficient pages, don't use any of them. */
			return NULL;
		page = tmp;
	}

	/* add end of list marker for the returned list */
	set_page_private(page, 0);
	/* actual return value, and adjustment of head */
	page = *head;
	*head = tmp;
	return page;
}

/* may be used outside of locks to find the tail of a (usually short)
 * "private" page chain, before adding it back to a global chain head
 * with page_chain_add() under a spinlock. */
static struct page *page_chain_tail(struct page *page, int *len)
{
	struct page *tmp;
	int i = 1;
	while ((tmp = page_chain_next(page)))
		++i, page = tmp;
	if (len)
		*len = i;
	return page;
}

static int page_chain_free(struct page *page)
{
	struct page *tmp;
	int i = 0;
	page_chain_for_each_safe(page, tmp) {
		put_page(page);
		++i;
	}
	return i;
}

static void page_chain_add(struct page **head,
		struct page *chain_first, struct page *chain_last)
{
#if 1
	struct page *tmp;
	tmp = page_chain_tail(chain_first, NULL);
	BUG_ON(tmp != chain_last);
#endif

	/* add chain to head */
	set_page_private(chain_last, (unsigned long)*head);
	*head = chain_first;
}

static struct page *__drbd_alloc_pages(struct drbd_device *device,
				       unsigned int number)
{
	struct page *page = NULL;
	struct page *tmp = NULL;
	unsigned int i = 0;

	/* Yes, testing drbd_pp_vacant outside the lock is racy.
	 * So what. It saves a spin_lock. */
	if (drbd_pp_vacant >= number) {
		spin_lock(&drbd_pp_lock);
		page = page_chain_del(&drbd_pp_pool, number);
		if (page)
			drbd_pp_vacant -= number;
		spin_unlock(&drbd_pp_lock);
		if (page)
			return page;
	}

	/* GFP_TRY, because we must not cause arbitrary write-out: in a DRBD
	 * "criss-cross" setup, that might cause write-out on some other DRBD,
	 * which in turn might block on the other node at this very place.  */
	for (i = 0; i < number; i++) {
		tmp = alloc_page(GFP_TRY);
		if (!tmp)
			break;
		set_page_private(tmp, (unsigned long)page);
		page = tmp;
	}

	if (i == number)
		return page;

	/* Not enough pages immediately available this time.
	 * No need to jump around here, drbd_alloc_pages will retry this
	 * function "soon". */
	if (page) {
		tmp = page_chain_tail(page, NULL);
		spin_lock(&drbd_pp_lock);
		page_chain_add(&drbd_pp_pool, page, tmp);
		drbd_pp_vacant += i;
		spin_unlock(&drbd_pp_lock);
	}
	return NULL;
}

/* kick lower level device, if we have more than (arbitrary number)
 * reference counts on it, which typically are locally submitted io
 * requests.  don't use unacked_cnt, so we speed up proto A and B, too. */
static void maybe_kick_lo(struct drbd_device *device)
{
	struct disk_conf *dc;
	unsigned int watermark = 1000000;

	rcu_read_lock();
	dc = rcu_dereference(device->ldev->disk_conf);
	if (dc)
		min_not_zero(dc->unplug_watermark, watermark);
	rcu_read_unlock();

	if (atomic_read(&device->local_cnt) >= watermark)
		drbd_kick_lo(device);
}

static void reclaim_finished_net_peer_reqs(struct drbd_device *device,
					   struct list_head *to_be_freed)
{
	struct drbd_peer_request *peer_req, *tmp;

	/* The EEs are always appended to the end of the list. Since
	   they are sent in order over the wire, they have to finish
	   in order. As soon as we see the first not finished we can
	   stop to examine the list... */

	list_for_each_entry_safe(peer_req, tmp, &device->net_ee, w.list) {
		if (drbd_peer_req_has_active_page(peer_req))
			break;
		list_move(&peer_req->w.list, to_be_freed);
	}
}

static void drbd_kick_lo_and_reclaim_net(struct drbd_device *device)
{
	LIST_HEAD(reclaimed);
	struct drbd_peer_request *peer_req, *t;

	maybe_kick_lo(device);
	spin_lock_irq(&device->resource->req_lock);
	reclaim_finished_net_peer_reqs(device, &reclaimed);
	spin_unlock_irq(&device->resource->req_lock);

	list_for_each_entry_safe(peer_req, t, &reclaimed, w.list)
		drbd_free_net_peer_req(device, peer_req);
}

/**
 * drbd_alloc_pages() - Returns @number pages, retries forever (or until signalled)
 * @device:	DRBD device.
 * @number:	number of pages requested
 * @retry:	whether to retry, if not enough pages are available right now
 *
 * Tries to allocate number pages, first from our own page pool, then from
 * the kernel.
 * Possibly retry until DRBD frees sufficient pages somewhere else.
 *
 * If this allocation would exceed the max_buffers setting, we throttle
 * allocation (schedule_timeout) to give the system some room to breathe.
 *
 * We do not use max-buffers as hard limit, because it could lead to
 * congestion and further to a distributed deadlock during online-verify or
 * (checksum based) resync, if the max-buffers, socket buffer sizes and
 * resync-rate settings are mis-configured.
 *
 * Returns a page chain linked via page->private.
 */
struct page *drbd_alloc_pages(struct drbd_peer_device *peer_device, unsigned int number,
			      bool retry)
{
	struct drbd_device *device = peer_device->device;
	struct page *page = NULL;
	DEFINE_WAIT(wait);
	unsigned int mxb;

	mxb = device->device_conf.max_buffers;

	if (atomic_read(&device->pp_in_use) < mxb)
		page = __drbd_alloc_pages(device, number);

	while (page == NULL) {
		prepare_to_wait(&drbd_pp_wait, &wait, TASK_INTERRUPTIBLE);

		drbd_kick_lo_and_reclaim_net(device);

		if (atomic_read(&device->pp_in_use) < device->device_conf.max_buffers) {
			page = __drbd_alloc_pages(device, number);
			if (page)
				break;
		}

		if (!retry)
			break;

		if (signal_pending(current)) {
			drbd_warn(device, "drbd_alloc_pages interrupted!\n");
			break;
		}

		if (schedule_timeout(HZ/10) == 0)
			mxb = UINT_MAX;
	}
	finish_wait(&drbd_pp_wait, &wait);

	if (page)
		atomic_add(number, &device->pp_in_use);
	return page;
}

/* Must not be used from irq, as that may deadlock: see drbd_alloc_pages.
 * Is also used from inside an other spin_lock_irq(&resource->req_lock);
 * Either links the page chain back to the global pool,
 * or returns all pages to the system. */
static void drbd_free_pages(struct drbd_device *device, struct page *page, int is_net)
{
	atomic_t *a = is_net ? &device->pp_in_use_by_net : &device->pp_in_use;
	int i;

	if (page == NULL)
		return;

	if (drbd_pp_vacant > (DRBD_MAX_BIO_SIZE/PAGE_SIZE) * minor_count)
		i = page_chain_free(page);
	else {
		struct page *tmp;
		tmp = page_chain_tail(page, &i);
		spin_lock(&drbd_pp_lock);
		page_chain_add(&drbd_pp_pool, page, tmp);
		drbd_pp_vacant += i;
		spin_unlock(&drbd_pp_lock);
	}
	i = atomic_sub_return(i, a);
	if (i < 0)
		drbd_warn(device, "ASSERTION FAILED: %s: %d < 0\n",
			is_net ? "pp_in_use_by_net" : "pp_in_use", i);
	wake_up(&drbd_pp_wait);
}

/*
You need to hold the req_lock:
 _drbd_wait_ee_list_empty()

You must not have the req_lock:
 drbd_free_peer_req()
 drbd_alloc_peer_req()
 drbd_free_peer_reqs()
 drbd_ee_fix_bhs()
 drbd_finish_peer_reqs()
 drbd_clear_done_ee()
 drbd_wait_ee_list_empty()
*/

struct drbd_peer_request *
drbd_alloc_peer_req(struct drbd_peer_device *peer_device, u64 id, sector_t sector,
		    unsigned int data_size, bool has_payload, gfp_t gfp_mask) __must_hold(local)
{
	struct drbd_device *device = peer_device->device;
	struct drbd_peer_request *peer_req;
	struct page *page = NULL;
	unsigned nr_pages = (data_size + PAGE_SIZE -1) >> PAGE_SHIFT;

	if (drbd_insert_fault(device, DRBD_FAULT_AL_EE))
		return NULL;

	peer_req = mempool_alloc(drbd_ee_mempool, gfp_mask & ~__GFP_HIGHMEM);
	if (!peer_req) {
		if (!(gfp_mask & __GFP_NOWARN))
			drbd_err(device, "%s: allocation failed\n", __func__);
		return NULL;
	}

	if (has_payload && data_size) {
		page = drbd_alloc_pages(peer_device, nr_pages, (gfp_mask & __GFP_WAIT));
		if (!page)
			goto fail;
	}

	drbd_clear_interval(&peer_req->i);
	peer_req->i.size = data_size;
	peer_req->i.sector = sector;
	peer_req->i.local = false;
	peer_req->i.waiting = false;

	INIT_LIST_HEAD(&peer_req->recv_order);
	peer_req->epoch = NULL;
	peer_req->peer_device = peer_device;
	peer_req->pages = page;
	atomic_set(&peer_req->pending_bios, 0);
	peer_req->flags = 0;
	/*
	 * The block_id is opaque to the receiver.  It is not endianness
	 * converted, and sent back to the sender unchanged.
	 */
	peer_req->block_id = id;

	return peer_req;

 fail:
	mempool_free(peer_req, drbd_ee_mempool);
	return NULL;
}

void __drbd_free_peer_req(struct drbd_device *device, struct drbd_peer_request *peer_req,
		       int is_net)
{
	if (peer_req->flags & EE_HAS_DIGEST)
		kfree(peer_req->digest);
	drbd_free_pages(device, peer_req->pages, is_net);
	D_ASSERT(device, atomic_read(&peer_req->pending_bios) == 0);
	D_ASSERT(device, drbd_interval_empty(&peer_req->i));
	mempool_free(peer_req, drbd_ee_mempool);
}

int drbd_free_peer_reqs(struct drbd_device *device, struct list_head *list)
{
	LIST_HEAD(work_list);
	struct drbd_peer_request *peer_req, *t;
	int count = 0;
	int is_net = list == &device->net_ee;

	spin_lock_irq(&device->resource->req_lock);
	list_splice_init(list, &work_list);
	spin_unlock_irq(&device->resource->req_lock);

	list_for_each_entry_safe(peer_req, t, &work_list, w.list) {
		__drbd_free_peer_req(device, peer_req, is_net);
		count++;
	}
	return count;
}

/*
 * See also comments in _req_mod(,BARRIER_ACKED) and receive_Barrier.
 */
static int drbd_finish_peer_reqs(struct drbd_device *device)
{
	LIST_HEAD(work_list);
	LIST_HEAD(reclaimed);
	struct drbd_peer_request *peer_req, *t;
	int err = 0;

	spin_lock_irq(&device->resource->req_lock);
	reclaim_finished_net_peer_reqs(device, &reclaimed);
	list_splice_init(&device->done_ee, &work_list);
	spin_unlock_irq(&device->resource->req_lock);

	list_for_each_entry_safe(peer_req, t, &reclaimed, w.list)
		drbd_free_net_peer_req(device, peer_req);

	/* possible callbacks here:
	 * e_end_block, and e_end_resync_block, e_send_discard_write.
	 * all ignore the last argument.
	 */
	list_for_each_entry_safe(peer_req, t, &work_list, w.list) {
		int err2;

		/* list_del not necessary, next/prev members not touched */
		err2 = peer_req->w.cb(&peer_req->w, !!err);
		if (!err)
			err = err2;
		if (!list_empty(&peer_req->recv_order)) {
			drbd_free_pages(device, peer_req->pages, 0);
			peer_req->pages = NULL;
		} else
			drbd_free_peer_req(device, peer_req);
	}
	wake_up(&device->ee_wait);

	return err;
}

static void _drbd_wait_ee_list_empty(struct drbd_device *device,
				     struct list_head *head)
{
	DEFINE_WAIT(wait);

	/* avoids spin_lock/unlock
	 * and calling prepare_to_wait in the fast path */
	while (!list_empty(head)) {
		prepare_to_wait(&device->ee_wait, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_irq(&device->resource->req_lock);
		drbd_kick_lo(device);
		schedule();
		finish_wait(&device->ee_wait, &wait);
		spin_lock_irq(&device->resource->req_lock);
	}
}

static void drbd_wait_ee_list_empty(struct drbd_device *device,
				    struct list_head *head)
{
	spin_lock_irq(&device->resource->req_lock);
	_drbd_wait_ee_list_empty(device, head);
	spin_unlock_irq(&device->resource->req_lock);
}

static int drbd_recv_short(struct socket *sock, void *buf, size_t size, int flags)
{
	mm_segment_t oldfs;
	struct kvec iov = {
		.iov_base = buf,
		.iov_len = size,
	};
	struct msghdr msg = {
		.msg_iovlen = 1,
		.msg_iov = (struct iovec *)&iov,
		.msg_flags = (flags ? flags : MSG_WAITALL | MSG_NOSIGNAL)
	};
	int rv;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	rv = sock_recvmsg(sock, &msg, size, msg.msg_flags);
	set_fs(oldfs);

	return rv;
}

static int drbd_recv(struct drbd_connection *connection, void *buf, size_t size)
{
	int rv;

	rv = drbd_recv_short(connection->data.socket, buf, size, 0);

	if (rv < 0) {
		if (rv == -ECONNRESET)
			drbd_info(connection, "sock was reset by peer\n");
		else if (rv != -ERESTARTSYS)
			drbd_info(connection, "sock_recvmsg returned %d\n", rv);
	} else if (rv == 0) {
		if (test_bit(DISCONNECT_EXPECTED, &connection->flags)) {
			long t;
			rcu_read_lock();
			t = rcu_dereference(connection->net_conf)->ping_timeo * HZ/10;
			rcu_read_unlock();

			t = wait_event_timeout(connection->ping_wait, connection->cstate[NOW] < C_CONNECTED, t);

			if (t)
				goto out;
		}
		drbd_info(connection, "sock was shut down by peer\n");
	}

	if (rv != size)
		change_cstate(connection, C_BROKEN_PIPE, CS_HARD);

out:
	return rv;
}

static int drbd_recv_all(struct drbd_connection *connection, void *buf, size_t size)
{
	int err;

	err = drbd_recv(connection, buf, size);
	if (err != size) {
		if (err >= 0)
			err = -EIO;
	} else
		err = 0;
	return err;
}

static int drbd_recv_all_warn(struct drbd_connection *connection, void *buf, size_t size)
{
	int err;

	err = drbd_recv_all(connection, buf, size);
	if (err && !signal_pending(current))
		drbd_warn(connection, "short read (expected size %d)\n", (int)size);
	return err;
}

/* quoting tcp(7):
 *   On individual connections, the socket buffer size must be set prior to the
 *   listen(2) or connect(2) calls in order to have it take effect.
 * This is our wrapper to do so.
 */
static void drbd_setbufsize(struct socket *sock, unsigned int snd,
		unsigned int rcv)
{
	/* open coded SO_SNDBUF, SO_RCVBUF */
	if (snd) {
		sock->sk->sk_sndbuf = snd;
		sock->sk->sk_userlocks |= SOCK_SNDBUF_LOCK;
	}
	if (rcv) {
		sock->sk->sk_rcvbuf = rcv;
		sock->sk->sk_userlocks |= SOCK_RCVBUF_LOCK;
	}
}

static struct socket *drbd_try_connect(struct drbd_connection *connection)
{
	const char *what;
	struct socket *sock;
	struct sockaddr_in6 src_in6;
	struct sockaddr_in6 peer_in6;
	struct net_conf *nc;
	int err, peer_addr_len, my_addr_len;
	int sndbuf_size, rcvbuf_size, connect_int;
	int disconnect_on_error = 1;

	rcu_read_lock();
	nc = rcu_dereference(connection->net_conf);
	if (!nc) {
		rcu_read_unlock();
		return NULL;
	}
	sndbuf_size = nc->sndbuf_size;
	rcvbuf_size = nc->rcvbuf_size;
	connect_int = nc->connect_int;
	rcu_read_unlock();

	my_addr_len = min_t(int, connection->my_addr_len, sizeof(src_in6));
	memcpy(&src_in6, &connection->my_addr, my_addr_len);

	if (((struct sockaddr *)&connection->my_addr)->sa_family == AF_INET6)
		src_in6.sin6_port = 0;
	else
		((struct sockaddr_in *)&src_in6)->sin_port = 0; /* AF_INET & AF_SCI */

	peer_addr_len = min_t(int, connection->peer_addr_len, sizeof(src_in6));
	memcpy(&peer_in6, &connection->peer_addr, peer_addr_len);

	what = "sock_create_kern";
	err = sock_create_kern(((struct sockaddr *)&src_in6)->sa_family,
			       SOCK_STREAM, IPPROTO_TCP, &sock);
	if (err < 0) {
		sock = NULL;
		goto out;
	}

	sock->sk->sk_rcvtimeo =
	sock->sk->sk_sndtimeo = connect_int * HZ;
	drbd_setbufsize(sock, sndbuf_size, rcvbuf_size);

       /* explicitly bind to the configured IP as source IP
	*  for the outgoing connections.
	*  This is needed for multihomed hosts and to be
	*  able to use lo: interfaces for drbd.
	* Make sure to use 0 as port number, so linux selects
	*  a free one dynamically.
	*/
	what = "bind before connect";
	err = sock->ops->bind(sock, (struct sockaddr *) &src_in6, my_addr_len);
	if (err < 0)
		goto out;

	/* connect may fail, peer not yet available.
	 * stay C_CONNECTING, don't go Disconnecting! */
	disconnect_on_error = 0;
	what = "connect";
	err = sock->ops->connect(sock, (struct sockaddr *) &peer_in6, peer_addr_len, 0);

out:
	if (err < 0) {
		if (sock) {
			sock_release(sock);
			sock = NULL;
		}
		switch (-err) {
			/* timeout, busy, signal pending */
		case ETIMEDOUT: case EAGAIN: case EINPROGRESS:
		case EINTR: case ERESTARTSYS:
			/* peer not (yet) available, network problem */
		case ECONNREFUSED: case ENETUNREACH:
		case EHOSTDOWN:    case EHOSTUNREACH:
			disconnect_on_error = 0;
			break;
		default:
			drbd_err(connection, "%s failed, err = %d\n", what, err);
		}
		if (disconnect_on_error)
			change_cstate(connection, C_DISCONNECTING, CS_HARD);
	}

	return sock;
}

static void drbd_incoming_connection(struct sock *sk)
{
	struct listener *listener = sk->sk_user_data;
	void (*state_change)(struct sock *sk);

	state_change = listener->original_sk_state_change;
	if (sk->sk_state == TCP_ESTABLISHED) {
		struct waiter *waiter;

		spin_lock(&listener->resource->listeners_lock);
		listener->pending_accepts++;
		waiter = list_entry(listener->waiters.next, struct waiter, list);
		wake_up(&waiter->wait);
		spin_unlock(&listener->resource->listeners_lock);
	}
	state_change(sk);
}

static int prepare_listener(struct drbd_connection *connection, struct listener *listener)
{
	int err, sndbuf_size, rcvbuf_size, my_addr_len;
	struct sockaddr_in6 my_addr;
	struct socket *s_listen;
	struct net_conf *nc;
	const char *what;

	rcu_read_lock();
	nc = rcu_dereference(connection->net_conf);
	if (!nc) {
		rcu_read_unlock();
		return -EIO;
	}
	sndbuf_size = nc->sndbuf_size;
	rcvbuf_size = nc->rcvbuf_size;
	rcu_read_unlock();

	my_addr_len = min_t(int, connection->my_addr_len, sizeof(struct sockaddr_in6));
	memcpy(&my_addr, &connection->my_addr, my_addr_len);

	what = "sock_create_kern";
	err = sock_create_kern(((struct sockaddr *)&my_addr)->sa_family,
			       SOCK_STREAM, IPPROTO_TCP, &s_listen);
	if (err) {
		s_listen = NULL;
		goto out;
	}

	s_listen->sk->sk_reuse = SK_CAN_REUSE; /* SO_REUSEADDR */
	drbd_setbufsize(s_listen, sndbuf_size, rcvbuf_size);

	what = "bind before listen";
	err = s_listen->ops->bind(s_listen, (struct sockaddr *)&my_addr, my_addr_len);
	if (err < 0)
		goto out;

	listener->s_listen = s_listen;
	write_lock_bh(&s_listen->sk->sk_callback_lock);
	listener->original_sk_state_change = s_listen->sk->sk_state_change;
	s_listen->sk->sk_state_change = drbd_incoming_connection;
	s_listen->sk->sk_user_data = listener;
	write_unlock_bh(&s_listen->sk->sk_callback_lock);

	what = "listen";
	err = s_listen->ops->listen(s_listen, 5);
	if (err < 0)
		goto out;

	memcpy(&listener->listen_addr, &my_addr, my_addr_len);

	return 0;
out:
	if (s_listen)
		sock_release(s_listen);
	if (err < 0) {
		if (err != -EAGAIN && err != -EINTR && err != -ERESTARTSYS &&
		    err != -EADDRINUSE) {
			drbd_err(connection, "%s failed, err = %d\n", what, err);
			change_cstate(connection, C_DISCONNECTING, CS_HARD);
		}
	}

	return err;
}

static struct listener* find_listener(struct drbd_connection *connection)
{
	struct drbd_resource *resource = connection->resource;
	struct listener *listener;

	list_for_each_entry(listener, &resource->listeners, list) {
		if (!memcmp(&listener->listen_addr, &connection->my_addr, connection->my_addr_len)) {
			kref_get(&listener->kref);
			return listener;
		}
	}
	return NULL;
}

static int get_listener(struct drbd_connection *connection, struct waiter *waiter)
{
	struct drbd_resource *resource = connection->resource;
	struct listener *listener, *new_listener = NULL;
	int err;

	waiter->connection = connection;
	waiter->socket = NULL;
	init_waitqueue_head(&waiter->wait);

	while (1) {
		spin_lock_bh(&resource->listeners_lock);
		listener = find_listener(connection);
		if (!listener && new_listener) {
			list_add(&new_listener->list, &resource->listeners);
			listener = new_listener;
			new_listener = NULL;
		}
		if (listener) {
			list_add(&waiter->list, &listener->waiters);
			waiter->listener = listener;
		}
		spin_unlock_bh(&resource->listeners_lock);

		if (new_listener) {
			sock_release(new_listener->s_listen);
			kfree(new_listener);
		}

		if (listener)
			return 0;

		new_listener = kmalloc(sizeof(*new_listener), GFP_KERNEL);
		if (!new_listener)
			return -ENOMEM;

		err = prepare_listener(connection, new_listener);
		if (err < 0) {
			kfree(new_listener);
			new_listener = NULL;
			if (err != -EADDRINUSE)
				return err;
			schedule_timeout_interruptible(HZ / 10);
		} else {
			kref_init(&new_listener->kref);
			INIT_LIST_HEAD(&new_listener->waiters);
			new_listener->resource = resource;
			new_listener->pending_accepts = 0;
		}
	}
}

static void drbd_listener_destroy(struct kref *kref)
{
	struct listener *listener = container_of(kref, struct listener, kref);
	struct drbd_resource *resource = listener->resource;

	list_del(&listener->list);
	spin_unlock_bh(&resource->listeners_lock);
	sock_release(listener->s_listen);
	kfree(listener);
	spin_lock_bh(&resource->listeners_lock);
}

static void put_listener(struct waiter *waiter)
{
	struct drbd_resource *resource;

	if (!waiter->listener)
		return;

	resource = waiter->listener->resource;
	spin_lock_bh(&resource->listeners_lock);
	list_del(&waiter->list);
	if (!list_empty(&waiter->listener->waiters) && waiter->listener->pending_accepts) {
		/* This receiver no longer does accept calls. In case we got woken up to do
		   one, and there are more receivers, wake one of the other guys to do it */
		struct waiter *ad2;
		ad2 = list_entry(waiter->listener->waiters.next, struct waiter, list);
		wake_up(&ad2->wait);
	}
	kref_put(&waiter->listener->kref, drbd_listener_destroy);
	spin_unlock_bh(&resource->listeners_lock);
	waiter->listener = NULL;
	if (waiter->socket) {
		sock_release(waiter->socket);
		waiter->socket = NULL;
	}
}

static void unregister_state_change(struct sock *sk, struct listener *listener)
{
	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_state_change = listener->original_sk_state_change;
	sk->sk_user_data = NULL;
	write_unlock_bh(&sk->sk_callback_lock);
}

static bool addr_equal(const struct sockaddr *addr1, const struct sockaddr *addr2)
{
	if (addr1->sa_family != addr2->sa_family)
		return false;

	if (addr1->sa_family == AF_INET6) {
		const struct sockaddr_in6 *v6a1 = (const struct sockaddr_in6 *)addr1;
		const struct sockaddr_in6 *v6a2 = (const struct sockaddr_in6 *)addr2;

		if (!ipv6_addr_equal(&v6a1->sin6_addr, &v6a2->sin6_addr))
			return false;
		else if (ipv6_addr_type(&v6a1->sin6_addr) & IPV6_ADDR_LINKLOCAL)
			return v6a1->sin6_scope_id == v6a2->sin6_scope_id;
		return true;
	} else /* AF_INET, AF_SSOCKS, AF_SDP */ {
		const struct sockaddr_in *v4a1 = (const struct sockaddr_in *)addr1;
		const struct sockaddr_in *v4a2 = (const struct sockaddr_in *)addr2;
		return v4a1->sin_addr.s_addr == v4a2->sin_addr.s_addr;
	}
}

static struct waiter *
	find_waiter_by_addr(struct listener *listener, struct sockaddr *addr)
{
	struct waiter *waiter;

	list_for_each_entry(waiter, &listener->waiters, list) {
		if (addr_equal((struct sockaddr *)&waiter->connection->peer_addr, addr))
			return waiter;
	}

	return NULL;
}

static bool _wait_connect_cond(struct waiter *waiter)
{
	struct drbd_connection *connection = waiter->connection;
	struct drbd_resource *resource = connection->resource;
	bool rv;

	spin_lock_bh(&resource->listeners_lock);
	rv = waiter->listener->pending_accepts > 0 || waiter->socket != NULL;
	spin_unlock_bh(&resource->listeners_lock);

	return rv;
}

static struct socket *drbd_wait_for_connect(struct waiter *waiter)
{
	struct drbd_connection *connection = waiter->connection;
	struct drbd_resource *resource = connection->resource;
	struct sockaddr_storage peer_addr;
	int timeo, connect_int, peer_addr_len, err = 0;
	struct socket *s_estab;
	struct net_conf *nc;
	struct waiter *waiter2;

	rcu_read_lock();
	nc = rcu_dereference(connection->net_conf);
	if (!nc) {
		rcu_read_unlock();
		return NULL;
	}
	connect_int = nc->connect_int;
	rcu_read_unlock();

	timeo = connect_int * HZ;
	timeo += (prandom_u32() & 1) ? timeo / 7 : -timeo / 7; /* 28.5% random jitter */

retry:
	timeo = wait_event_interruptible_timeout(waiter->wait, _wait_connect_cond(waiter), timeo);
	if (timeo <= 0)
		return NULL;

	spin_lock_bh(&resource->listeners_lock);
	if (waiter->socket) {
		s_estab = waiter->socket;
		waiter->socket = NULL;
	} else if (waiter->listener->pending_accepts > 0) {
		waiter->listener->pending_accepts--;
		spin_unlock_bh(&resource->listeners_lock);

		s_estab = NULL;
		err = kernel_accept(waiter->listener->s_listen, &s_estab, 0);
		if (err < 0) {
			if (err != -EAGAIN && err != -EINTR && err != -ERESTARTSYS) {
				drbd_err(connection, "accept failed, err = %d\n", err);
				change_cstate(connection, C_DISCONNECTING, CS_HARD);
			}
		}

		if (!s_estab)
			return NULL;

		unregister_state_change(s_estab->sk, waiter->listener);

		s_estab->ops->getname(s_estab, (struct sockaddr *)&peer_addr, &peer_addr_len, 2);

		spin_lock_bh(&resource->listeners_lock);
		waiter2 = find_waiter_by_addr(waiter->listener, (struct sockaddr *)&peer_addr);
		if (!waiter2) {
			struct sockaddr_in6 *from_sin6, *to_sin6;
			struct sockaddr_in *from_sin, *to_sin;
			struct drbd_connection *connection2;

			connection2 = conn_get_by_addrs(
				&connection->my_addr, connection->my_addr_len,
				&peer_addr, peer_addr_len);
			if (connection2) {
				/* conn_get_by_addrs() does a get, put follows here... no debug */
				drbd_info(connection2,
					  "Receiver busy; rejecting incoming connection\n");
				kref_put(&connection2->kref, drbd_destroy_connection);
				goto retry_locked;
			}

			switch(peer_addr.ss_family) {
			case AF_INET6:
				from_sin6 = (struct sockaddr_in6 *)&peer_addr;
				to_sin6 = (struct sockaddr_in6 *)&connection->my_addr;
				drbd_err(resource, "Closing unexpected connection from "
					 "%pI6 to port %u\n",
					 &from_sin6->sin6_addr,
					 be16_to_cpu(to_sin6->sin6_port));
				break;
			default:
				from_sin = (struct sockaddr_in *)&peer_addr;
				to_sin = (struct sockaddr_in *)&connection->my_addr;
				drbd_err(resource, "Closing unexpected connection from "
					 "%pI4 to port %u\n",
					 &from_sin->sin_addr,
					 be16_to_cpu(to_sin->sin_port));
				break;
			}

			goto retry_locked;
		}
		if (waiter2 != waiter) {
			if (waiter2->socket) {
				drbd_err(waiter2->connection,
					 "Receiver busy; rejecting incoming connection\n");
				goto retry_locked;
			}
			waiter2->socket = s_estab;
			s_estab = NULL;
			wake_up(&waiter2->wait);
			goto retry_locked;
		}
	}
	spin_unlock_bh(&resource->listeners_lock);
	return s_estab;

retry_locked:
	spin_unlock_bh(&resource->listeners_lock);
	if (s_estab) {
		sock_release(s_estab);
		s_estab = NULL;
	}
	goto retry;
}

static int decode_header(struct drbd_connection *, void *, struct packet_info *);

static int send_first_packet(struct drbd_connection *connection, struct drbd_socket *sock,
			     enum drbd_packet cmd)
{
	if (!conn_prepare_command(connection, sock))
		return -EIO;
	return send_command(connection, -1, sock, cmd, 0, NULL, 0);
}

static int receive_first_packet(struct drbd_connection *connection, struct socket *sock)
{
	unsigned int header_size = drbd_header_size(connection);
	struct packet_info pi;
	int err;

	err = drbd_recv_short(sock, connection->data.rbuf, header_size, 0);
	if (err != header_size) {
		if (err >= 0)
			err = -EIO;
		return err;
	}
	err = decode_header(connection, connection->data.rbuf, &pi);
	if (err)
		return err;
	return pi.cmd;
}

/**
 * drbd_socket_okay() - Free the socket if its connection is not okay
 * @sock:	pointer to the pointer to the socket.
 */
static int drbd_socket_okay(struct socket **sock)
{
	int rr;
	char tb[4];

	if (!*sock)
		return false;

	rr = drbd_recv_short(*sock, tb, 4, MSG_DONTWAIT | MSG_PEEK);

	if (rr > 0 || rr == -EAGAIN) {
		return true;
	} else {
		sock_release(*sock);
		*sock = NULL;
		return false;
	}
}
/* Gets called if a connection is established, or if a new minor gets created
   in a connection */
int drbd_connected(struct drbd_peer_device *peer_device)
{
	struct drbd_device *device = peer_device->device;
	int err;

	atomic_set(&peer_device->packet_seq, 0);
	peer_device->peer_seq = 0;

	err = drbd_send_sync_param(peer_device);
	if (!err)
		err = drbd_send_sizes(peer_device, 0, 0);
	if (!err) {
		if (device->disk_state[NOW] > D_DISKLESS) {
			err = drbd_send_uuids(peer_device, 0, 0);
		} else {
			set_bit(INITIAL_STATE_SENT, &peer_device->flags);
			err = drbd_send_current_state(peer_device);
		}
	}

	clear_bit(USE_DEGR_WFC_T, &peer_device->flags);
	clear_bit(RESIZE_PENDING, &peer_device->flags);
	mod_timer(&device->request_timer, jiffies + HZ); /* just start it here. */
	return err;
}

static int connect_timeout_work(struct drbd_work *work, int cancel)
{
	struct drbd_connection *connection =
		container_of(work, struct drbd_connection, connect_timer_work);
	struct drbd_resource *resource = connection->resource;
	enum drbd_conn_state cstate;

	spin_lock_irq(&resource->req_lock);
	cstate = connection->cstate[NOW];
	spin_unlock_irq(&resource->req_lock);
	if (cstate == C_CONNECTING) {
		drbd_info(connection, "Failure to connect; retrying\n");
		change_cstate(connection, C_NETWORK_FAILURE, CS_HARD);
	}
	kref_debug_put(&connection->kref_debug, 11);
	kref_put(&connection->kref, drbd_destroy_connection);
	return 0;
}

void connect_timer_fn(unsigned long data)
{
	struct drbd_connection *connection = (struct drbd_connection *) data;
	struct drbd_resource *resource = connection->resource;
	unsigned long irq_flags;

	spin_lock_irqsave(&resource->req_lock, irq_flags);
	drbd_queue_work(&connection->sender_work, &connection->connect_timer_work);
	spin_unlock_irqrestore(&resource->req_lock, irq_flags);
}

static void conn_connect2(struct drbd_connection *connection)
{
	struct drbd_resource *resource = connection->resource;
	struct drbd_peer_device *peer_device;
	int vnr;

	atomic_set(&connection->ap_in_flight, 0);

	/* Prevent a race between resync-handshake and
	 * being promoted to Primary.
	 *
	 * Grab the state semaphore, so we know that any current
	 * drbd_set_role() is finished, and any incoming drbd_set_role
	 * will see the INITIAL_STATE_SENT flag, and wait for it to be cleared.
	 */
	down(&resource->state_sem);
	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		struct drbd_device *device = peer_device->device;
		kobject_get(&device->kobj);
		/* peer_device->connection cannot go away: caller holds a reference. */
		rcu_read_unlock();
		drbd_connected(peer_device);
		rcu_read_lock();
		kobject_put(&device->kobj);
	}
	rcu_read_unlock();
	up(&resource->state_sem);
}

static void conn_disconnect(struct drbd_connection *connection);

/*
 * Returns true if we have a valid connection.
 */
static bool conn_connect(struct drbd_connection *connection)
{
	struct drbd_resource *resource = connection->resource;
	struct drbd_peer_device *peer_device;
	struct drbd_socket sock, msock;
	bool discard_my_data;
	struct net_conf *nc;
	int timeout, h, ok, vnr;
	struct waiter waiter;

start:
	clear_bit(DISCONNECT_EXPECTED, &connection->flags);
	if (change_cstate(connection, C_CONNECTING, CS_VERBOSE) < SS_SUCCESS) {
		/* We do not have a network config. */
		return false;
	}

	mutex_init(&sock.mutex);
	sock.sbuf = connection->data.sbuf;
	sock.rbuf = connection->data.rbuf;
	sock.socket = NULL;
	mutex_init(&msock.mutex);
	msock.sbuf = connection->meta.sbuf;
	msock.rbuf = connection->meta.rbuf;
	msock.socket = NULL;

	/* Assume that the peer only understands protocol 80 until we know better.  */
	connection->agreed_pro_version = 80;

	if (get_listener(connection, &waiter)) {
		h = 0;  /* retry */
		goto out;
	}

	do {
		struct socket *s;

		s = drbd_try_connect(connection);
		if (s) {
			if (!sock.socket) {
				sock.socket = s;
				send_first_packet(connection, &sock, P_INITIAL_DATA);
			} else if (!msock.socket) {
				clear_bit(RESOLVE_CONFLICTS, &connection->flags);
				msock.socket = s;
				send_first_packet(connection, &msock, P_INITIAL_META);
			} else {
				drbd_err(connection, "Logic error in conn_connect()\n");
				goto out_release_sockets;
			}
		}

		if (sock.socket && msock.socket) {
			rcu_read_lock();
			nc = rcu_dereference(connection->net_conf);
			timeout = nc->ping_timeo * HZ / 10;
			rcu_read_unlock();
			schedule_timeout_interruptible(timeout);
			ok = drbd_socket_okay(&sock.socket);
			ok = drbd_socket_okay(&msock.socket) && ok;
			if (ok)
				break;
		}

retry:
		s = drbd_wait_for_connect(&waiter);
		if (s) {
			int fp = receive_first_packet(connection, s);
			drbd_socket_okay(&sock.socket);
			drbd_socket_okay(&msock.socket);
			switch (fp) {
			case P_INITIAL_DATA:
				if (sock.socket) {
					drbd_warn(connection, "initial packet S crossed\n");
					sock_release(sock.socket);
					sock.socket = s;
					goto randomize;
				}
				sock.socket = s;
				break;
			case P_INITIAL_META:
				set_bit(RESOLVE_CONFLICTS, &connection->flags);
				if (msock.socket) {
					drbd_warn(connection, "initial packet M crossed\n");
					sock_release(msock.socket);
					msock.socket = s;
					goto randomize;
				}
				msock.socket = s;
				break;
			default:
				drbd_warn(connection, "Error receiving initial packet\n");
				sock_release(s);
randomize:
				if (prandom_u32() & 1)
					goto retry;
			}
		}

		if (connection->cstate[NOW] <= C_DISCONNECTING)
			goto out_release_sockets;
		if (signal_pending(current)) {
			flush_signals(current);
			smp_rmb();
			if (get_t_state(&connection->receiver) == EXITING)
				goto out_release_sockets;
		}

		ok = drbd_socket_okay(&sock.socket);
		ok = drbd_socket_okay(&msock.socket) && ok;
	} while (!ok);

	put_listener(&waiter);

	sock.socket->sk->sk_reuse = SK_CAN_REUSE; /* SO_REUSEADDR */
	msock.socket->sk->sk_reuse = SK_CAN_REUSE; /* SO_REUSEADDR */

	sock.socket->sk->sk_allocation = GFP_NOIO;
	msock.socket->sk->sk_allocation = GFP_NOIO;

	sock.socket->sk->sk_priority = TC_PRIO_INTERACTIVE_BULK;
	msock.socket->sk->sk_priority = TC_PRIO_INTERACTIVE;

	/* NOT YET ...
	 * sock.socket->sk->sk_sndtimeo = connection->net_conf->timeout*HZ/10;
	 * sock.socket->sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;
	 * first set it to the P_CONNECTION_FEATURES timeout,
	 * which we set to 4x the configured ping_timeout. */
	rcu_read_lock();
	nc = rcu_dereference(connection->net_conf);

	sock.socket->sk->sk_sndtimeo =
	sock.socket->sk->sk_rcvtimeo = nc->ping_timeo*4*HZ/10;

	msock.socket->sk->sk_rcvtimeo = nc->ping_int*HZ;
	timeout = nc->timeout * HZ / 10;
	rcu_read_unlock();

	msock.socket->sk->sk_sndtimeo = timeout;

	/* we don't want delays.
	 * we use TCP_CORK where appropriate, though */
	drbd_tcp_nodelay(sock.socket);
	drbd_tcp_nodelay(msock.socket);

	connection->data.socket = sock.socket;
	connection->meta.socket = msock.socket;
	connection->last_received = jiffies;

	h = drbd_do_features(connection);
	if (h <= 0)
		goto out;

	if (connection->cram_hmac_tfm) {
		switch (drbd_do_auth(connection)) {
		case -1:
			drbd_err(connection, "Authentication of peer failed\n");
			h = -1;  /* give up; go standalone */
			goto out;
		case 0:
			drbd_err(connection, "Authentication of peer failed, trying again.\n");
			h = 0;  /* retry */
			goto out;
		}
	}

	connection->data.socket->sk->sk_sndtimeo = timeout;
	connection->data.socket->sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;
	connection->primary_mask_sent = -1; /* make sure to send it out soon */

	rcu_read_lock();
	nc = rcu_dereference(connection->net_conf);
	discard_my_data = nc->discard_my_data;
	rcu_read_unlock();

	if (drbd_send_protocol(connection) == -EOPNOTSUPP) {
		/* give up; go standalone */
		change_cstate(connection, C_DISCONNECTING, CS_HARD);
		return -1;
	}

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		clear_bit(INITIAL_STATE_SENT, &peer_device->flags);
		clear_bit(INITIAL_STATE_RECEIVED, &peer_device->flags);
	}
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		struct drbd_device *device = peer_device->device;

		if (discard_my_data)
			set_bit(DISCARD_MY_DATA, &device->flags);
		else
			clear_bit(DISCARD_MY_DATA, &device->flags);
	}
	rcu_read_unlock();

	if (mutex_lock_interruptible(&resource->conf_update) == 0) {
		/* The discard_my_data flag is a single-shot modifier to the next
		 * connection attempt, the handshake of which is now well underway.
		 * No need for rcu style copying of the whole struct
		 * just to clear a single value. */
		connection->net_conf->discard_my_data = 0;
		mutex_unlock(&resource->conf_update);
	}

	drbd_thread_start(&connection->asender);

	if (connection->agreed_pro_version >= 110) {
		if (resource->res_opts.node_id < connection->net_conf->peer_node_id) {
			timeout = twopc_retry_timeout(resource, 0);
			drbd_debug(connection, "Waiting for %ums to avoid transaction "
				   "conflicts\n", jiffies_to_msecs(timeout));
			schedule_timeout_interruptible(timeout);

			if (connect_transaction(connection) < SS_SUCCESS) {
				h = 0;
				goto out;
			}
			conn_connect2(connection);
		} else {
			kref_get(&connection->kref);
			kref_debug_get(&connection->kref_debug, 11);
			connection->connect_timer_work.cb = connect_timeout_work;
			mod_timer(&connection->connect_timer, jiffies + twopc_timeout(resource));
		}
	} else {
		enum drbd_state_rv rv;
		rv = change_cstate(connection, C_CONNECTED,
				   CS_VERBOSE | CS_WAIT_COMPLETE | CS_SERIALIZE);
		if (rv < SS_SUCCESS || connection->cstate[NOW] != C_CONNECTED) {
			h = 0;
			goto out;
		}
		conn_connect2(connection);
	}
	return 1;

out_release_sockets:
	put_listener(&waiter);
	if (sock.socket)
		sock_release(sock.socket);
	if (msock.socket)
		sock_release(msock.socket);
	h = -1;  /* give up; go standalone */

out:
	if (h == 0) {
		conn_disconnect(connection);
		schedule_timeout_interruptible(HZ);
		goto start;
	}
	if (h == -1)
		change_cstate(connection, C_DISCONNECTING, CS_HARD);
	return h > 0;
}

static int decode_header(struct drbd_connection *connection, void *header, struct packet_info *pi)
{
	unsigned int header_size = drbd_header_size(connection);

	if (header_size == sizeof(struct p_header100) &&
	    *(__be32 *)header == cpu_to_be32(DRBD_MAGIC_100)) {
		struct p_header100 *h = header;
		if (h->pad != 0) {
			drbd_err(connection, "Header padding is not zero\n");
			return -EINVAL;
		}
		pi->vnr = (s16)be16_to_cpu(h->volume);
		pi->cmd = be16_to_cpu(h->command);
		pi->size = be32_to_cpu(h->length);
	} else if (header_size == sizeof(struct p_header95) &&
		   *(__be16 *)header == cpu_to_be16(DRBD_MAGIC_BIG)) {
		struct p_header95 *h = header;
		pi->cmd = be16_to_cpu(h->command);
		pi->size = be32_to_cpu(h->length);
		pi->vnr = 0;
	} else if (header_size == sizeof(struct p_header80) &&
		   *(__be32 *)header == cpu_to_be32(DRBD_MAGIC)) {
		struct p_header80 *h = header;
		pi->cmd = be16_to_cpu(h->command);
		pi->size = be16_to_cpu(h->length);
		pi->vnr = 0;
	} else {
		drbd_err(connection, "Wrong magic value 0x%08x in protocol version %d\n",
			 be32_to_cpu(*(__be32 *)header),
			 connection->agreed_pro_version);
		return -EINVAL;
	}
	pi->data = header + header_size;
	return 0;
}

static int drbd_recv_header(struct drbd_connection *connection, struct packet_info *pi)
{
	void *buffer = connection->data.rbuf;
	int err;

	err = drbd_recv_all_warn(connection, buffer, drbd_header_size(connection));
	if (err)
		return err;

	err = decode_header(connection, buffer, pi);
	connection->last_received = jiffies;

	return err;
}

static enum finish_epoch drbd_flush_after_epoch(struct drbd_connection *connection, struct drbd_epoch *epoch)
{
	int rv;
	struct drbd_resource *resource = connection->resource;
	struct drbd_device *device;
	int vnr;

	if (resource->write_ordering >= WO_BDEV_FLUSH) {
		rcu_read_lock();
		idr_for_each_entry(&resource->devices, device, vnr) {
			if (!get_ldev(device))
				continue;
			kobject_get(&device->kobj);
			rcu_read_unlock();

			rv = blkdev_issue_flush(device->ldev->backing_bdev, GFP_KERNEL,
						NULL);
			if (rv) {
				drbd_info(device, "local disk flush failed with status %d\n", rv);
				/* would rather check on EOPNOTSUPP, but that is not reliable.
				 * don't try again for ANY return value != 0
				 * if (rv == -EOPNOTSUPP) */
				drbd_bump_write_ordering(resource, WO_DRAIN_IO);
			}
			put_ldev(device);
			kobject_put(&device->kobj);

			rcu_read_lock();
			if (rv)
				break;
		}
		rcu_read_unlock();
	}

	return drbd_may_finish_epoch(connection, epoch, EV_BARRIER_DONE);
}

static int w_flush(struct drbd_work *w, int cancel)
{
	struct flush_work *fw = container_of(w, struct flush_work, w);
	struct drbd_epoch *epoch = fw->epoch;
	struct drbd_connection *connection = epoch->connection;

	kfree(fw);

	if (!test_and_set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags))
		drbd_flush_after_epoch(connection, epoch);

	drbd_may_finish_epoch(connection, epoch, EV_PUT |
			      (connection->cstate[NOW] < C_CONNECTED ? EV_CLEANUP : 0));

	return 0;
}

/**
 * drbd_may_finish_epoch() - Applies an epoch_event to the epoch's state, eventually finishes it.
 * @connection:	DRBD connection.
 * @epoch:	Epoch object.
 * @ev:		Epoch event.
 */
static enum finish_epoch drbd_may_finish_epoch(struct drbd_connection *connection,
					       struct drbd_epoch *epoch,
					       enum epoch_event ev)
{
	int finish, epoch_size;
	struct drbd_epoch *next_epoch;
	int schedule_flush = 0;
	enum finish_epoch rv = FE_STILL_LIVE;
	struct drbd_resource *resource = connection->resource;

	spin_lock(&connection->epoch_lock);
	do {
		next_epoch = NULL;
		finish = 0;

		epoch_size = atomic_read(&epoch->epoch_size);

		switch (ev & ~EV_CLEANUP) {
		case EV_PUT:
			atomic_dec(&epoch->active);
			break;
		case EV_GOT_BARRIER_NR:
			set_bit(DE_HAVE_BARRIER_NUMBER, &epoch->flags);

			/* Special case: If we just switched from WO_BIO_BARRIER to
			   WO_BDEV_FLUSH we should not finish the current epoch */
			if (test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags) && epoch_size == 1 &&
			    resource->write_ordering != WO_BIO_BARRIER &&
			    epoch == connection->current_epoch)
				clear_bit(DE_CONTAINS_A_BARRIER, &epoch->flags);
			break;
		case EV_BARRIER_DONE:
			set_bit(DE_BARRIER_IN_NEXT_EPOCH_DONE, &epoch->flags);
			break;
		case EV_BECAME_LAST:
			/* nothing to do*/
			break;
		}

		if (epoch_size != 0 &&
		    atomic_read(&epoch->active) == 0 &&
		    (test_bit(DE_HAVE_BARRIER_NUMBER, &epoch->flags) || ev & EV_CLEANUP) &&
		    epoch->list.prev == &connection->current_epoch->list &&
		    !test_bit(DE_IS_FINISHING, &epoch->flags)) {
			/* Nearly all conditions are met to finish that epoch... */
			if (test_bit(DE_BARRIER_IN_NEXT_EPOCH_DONE, &epoch->flags) ||
			    resource->write_ordering == WO_NONE ||
			    (epoch_size == 1 && test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags)) ||
			    ev & EV_CLEANUP) {
				finish = 1;
				set_bit(DE_IS_FINISHING, &epoch->flags);
			} else if (!test_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags) &&
				 resource->write_ordering == WO_BIO_BARRIER) {
				atomic_inc(&epoch->active);
				schedule_flush = 1;
			}
		}
		if (finish) {
			if (!(ev & EV_CLEANUP)) {
				spin_unlock(&connection->epoch_lock);
				drbd_send_b_ack(epoch->connection, epoch->barrier_nr, epoch_size);
				spin_lock(&connection->epoch_lock);
			}
#if 0
			/* FIXME: dec unacked on connection, once we have
			 * something to count pending connection packets in. */
			if (test_bit(DE_HAVE_BARRIER_NUMBER, &epoch->flags))
				dec_unacked(epoch->connection);
#endif

			if (connection->current_epoch != epoch) {
				next_epoch = list_entry(epoch->list.next, struct drbd_epoch, list);
				list_del(&epoch->list);
				ev = EV_BECAME_LAST | (ev & EV_CLEANUP);
				connection->epochs--;
				kfree(epoch);

				if (rv == FE_STILL_LIVE)
					rv = FE_DESTROYED;
			} else {
				epoch->flags = 0;
				atomic_set(&epoch->epoch_size, 0);
				/* atomic_set(&epoch->active, 0); is alrady zero */
				if (rv == FE_STILL_LIVE)
					rv = FE_RECYCLED;
			}
		}

		if (!next_epoch)
			break;

		epoch = next_epoch;
	} while (1);

	spin_unlock(&connection->epoch_lock);

	if (schedule_flush) {
		struct flush_work *fw;
		fw = kmalloc(sizeof(*fw), GFP_ATOMIC);
		if (fw) {
			fw->w.cb = w_flush;
			fw->epoch = epoch;
			fw->device = NULL; /* FIXME drop this member, it is unused. */
			drbd_queue_work(&resource->work, &fw->w);
		} else {
			drbd_warn(resource, "Could not kmalloc a flush_work obj\n");
			set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags);
			/* That is not a recursion, only one level */
			drbd_may_finish_epoch(connection, epoch, EV_BARRIER_DONE);
			drbd_may_finish_epoch(connection, epoch, EV_PUT);
		}
	}

	return rv;
}

/**
 * drbd_bump_write_ordering() - Fall back to an other write ordering method
 * @resource:	DRBD resource.
 * @wo:		Write ordering method to try.
 */
void drbd_bump_write_ordering(struct drbd_resource *resource, enum write_ordering_e wo) __must_hold(local)
{
	struct disk_conf *dc;
	struct drbd_device *device;
	enum write_ordering_e pwo;
	int vnr, i = 0;
	static char *write_ordering_str[] = {
		[WO_NONE] = "none",
		[WO_DRAIN_IO] = "drain",
		[WO_BDEV_FLUSH] = "flush",
		[WO_BIO_BARRIER] = "barrier",
	};

	pwo = resource->write_ordering;
	wo = min(pwo, wo);
	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, vnr) {
		if (i++ == 1 && wo == WO_BIO_BARRIER)
			wo = WO_BDEV_FLUSH; /* WO = barrier does not handle multiple volumes */
		if (!get_ldev_if_state(device, D_ATTACHING))
			continue;

		dc = rcu_dereference(device->ldev->disk_conf);

		if (wo == WO_BIO_BARRIER && !dc->disk_barrier)
			wo = WO_BDEV_FLUSH;
		if (wo == WO_BDEV_FLUSH && !dc->disk_flushes)
			wo = WO_DRAIN_IO;
		if (wo == WO_DRAIN_IO && !dc->disk_drain)
			wo = WO_NONE;
		put_ldev(device);
	}
	rcu_read_unlock();
	resource->write_ordering = wo;
	if (pwo != resource->write_ordering || wo == WO_BIO_BARRIER)
		drbd_info(resource, "Method to ensure write ordering: %s\n", write_ordering_str[resource->write_ordering]);
}

void conn_wait_active_ee_empty(struct drbd_connection *connection);

/**
 * drbd_submit_peer_request()
 * @device:	DRBD device.
 * @peer_req:	peer request
 * @rw:		flag field, see bio->bi_rw
 *
 * May spread the pages to multiple bios,
 * depending on bio_add_page restrictions.
 *
 * Returns 0 if all bios have been submitted,
 * -ENOMEM if we could not allocate enough bios,
 * -ENOSPC (any better suggestion?) if we have not been able to bio_add_page a
 *  single page to an empty bio (which should never happen and likely indicates
 *  that the lower level IO stack is in some way broken). This has been observed
 *  on certain Xen deployments.
 */
/* TODO allocate from our own bio_set. */
int drbd_submit_peer_request(struct drbd_device *device,
			     struct drbd_peer_request *peer_req,
			     const unsigned rw, const int fault_type)
{
	struct bio *bios = NULL;
	struct bio *bio;
	struct page *page = peer_req->pages;
	sector_t sector = peer_req->i.sector;
	unsigned ds = peer_req->i.size;
	unsigned n_bios = 0;
	unsigned nr_pages = (ds + PAGE_SIZE -1) >> PAGE_SHIFT;
	int err = -ENOMEM;

	if (peer_req->flags & EE_IS_TRIM_USE_ZEROOUT) {
		/* wait for all pending IO completions, before we start
		 * zeroing things out. */
		conn_wait_active_ee_empty(peer_req->peer_device->connection);
		if (blkdev_issue_zeroout(device->ldev->backing_bdev,
			sector, ds >> 9, GFP_NOIO))
			peer_req->flags |= EE_WAS_ERROR;
		drbd_endio_write_sec_final(peer_req);
		return 0;
	}

	if (peer_req->flags & EE_IS_TRIM)
		nr_pages = 0; /* discards don't have any payload. */

	/* In most cases, we will only need one bio.  But in case the lower
	 * level restrictions happen to be different at this offset on this
	 * side than those of the sending peer, we may need to submit the
	 * request in more than one bio.
	 *
	 * Plain bio_alloc is good enough here, this is no DRBD internally
	 * generated bio, but a bio allocated on behalf of the peer.
	 */
next_bio:
	bio = bio_alloc(GFP_NOIO, nr_pages);
	if (!bio) {
		drbd_err(device, "submit_ee: Allocation of a bio failed (nr_pages=%u)\n", nr_pages);
		goto fail;
	}
	/* > peer_req->i.sector, unless this is the first bio */
	bio->bi_sector = sector;
	bio->bi_bdev = device->ldev->backing_bdev;
	/* we special case some flags in the multi-bio case, see below
	 * (REQ_UNPLUG, REQ_FLUSH, or BIO_RW_BARRIER in older kernels) */
	bio->bi_rw = rw;
	bio->bi_private = peer_req;
	bio->bi_end_io = drbd_peer_request_endio;

	bio->bi_next = bios;
	bios = bio;
	++n_bios;

	if (rw & DRBD_REQ_DISCARD) {
		bio->bi_size = ds;
		goto submit;
	}

	page_chain_for_each(page) {
		unsigned len = min_t(unsigned, ds, PAGE_SIZE);
		if (!bio_add_page(bio, page, len, 0)) {
			/* A single page must always be possible!
			 * But in case it fails anyways,
			 * we deal with it, and complain (below). */
			if (bio->bi_vcnt == 0) {
				drbd_err(device,
					"bio_add_page failed for len=%u, "
					"bi_vcnt=0 (bi_sector=%llu)\n",
					len, (unsigned long long)bio->bi_sector);
				err = -ENOSPC;
				goto fail;
			}
			goto next_bio;
		}
		ds -= len;
		sector += len >> 9;
		--nr_pages;
	}
	D_ASSERT(device, ds == 0);
submit:
	D_ASSERT(device, page == NULL);

	atomic_set(&peer_req->pending_bios, n_bios);
	do {
		bio = bios;
		bios = bios->bi_next;
		bio->bi_next = NULL;

		/* strip off REQ_UNPLUG unless it is the last bio */
		if (bios)
			bio->bi_rw &= ~DRBD_REQ_UNPLUG;
		drbd_generic_make_request(device, fault_type, bio);

		/* strip off REQ_FLUSH,
		 * unless it is the first or last bio */
		if (bios && bios->bi_next)
			bios->bi_rw &= ~DRBD_REQ_FLUSH;
	} while (bios);
	maybe_kick_lo(device);
	return 0;

fail:
	while (bios) {
		bio = bios;
		bios = bios->bi_next;
		bio_put(bio);
	}
	return err;
}

static void drbd_remove_peer_req_interval(struct drbd_device *device,
					  struct drbd_peer_request *peer_req)
{
	struct drbd_interval *i = &peer_req->i;

	drbd_remove_interval(&device->write_requests, i);
	drbd_clear_interval(i);

	/* Wake up any processes waiting for this peer request to complete.  */
	if (i->waiting)
		wake_up(&device->misc_wait);
}

/**
 * w_e_reissue() - Worker callback; Resubmit a bio, without REQ_HARDBARRIER set
 * @device:	DRBD device.
 * @dw:		work object.
 * @cancel:	The connection will be closed anyways (unused in this callback)
 */
int w_e_reissue(struct drbd_work *w, int cancel) __releases(local)
{
	struct drbd_peer_request *peer_req =
		container_of(w, struct drbd_peer_request, w);
	struct drbd_peer_device *peer_device = peer_req->peer_device;
	struct drbd_device *device = peer_device->device;
	int err;
	/* We leave DE_CONTAINS_A_BARRIER and EE_IS_BARRIER in place,
	   (and DE_BARRIER_IN_NEXT_EPOCH_ISSUED in the previous Epoch)
	   so that we can finish that epoch in drbd_may_finish_epoch().
	   That is necessary if we already have a long chain of Epochs, before
	   we realize that BARRIER is actually not supported */

	/* As long as the -ENOTSUPP on the barrier is reported immediately
	   that will never trigger. If it is reported late, we will just
	   print that warning and continue correctly for all future requests
	   with WO_BDEV_FLUSH */
	if (previous_epoch(peer_device->connection, peer_req->epoch))
		drbd_warn(device, "Write ordering was not enforced (one time event)\n");

	/* we still have a local reference,
	 * get_ldev was done in receive_Data. */

	peer_req->w.cb = e_end_block;
	err = drbd_submit_peer_request(device, peer_req, WRITE, DRBD_FAULT_DT_WR);
	switch (err) {
	case -ENOMEM:
		peer_req->w.cb = w_e_reissue;
		drbd_queue_work(&peer_device->connection->sender_work,
				&peer_req->w);
		/* retry later; fall through */
	case 0:
		/* keep worker happy and connection up */
		return 0;

	case -ENOSPC:
		/* no other error expected, but anyways: */
	default:
		/* forget the object,
		 * and cause a "Network failure" */
		spin_lock_irq(&device->resource->req_lock);
		list_del(&peer_req->w.list);
		drbd_remove_peer_req_interval(device, peer_req);
		spin_unlock_irq(&device->resource->req_lock);
		drbd_al_complete_io(device, &peer_req->i);
		drbd_may_finish_epoch(peer_device->connection, peer_req->epoch, EV_PUT + EV_CLEANUP);
		drbd_free_peer_req(device, peer_req);
		drbd_err(device, "submit failed, triggering re-connect\n");
		return err;
	}
}

void conn_wait_active_ee_empty(struct drbd_connection *connection)
{
	struct drbd_peer_device *peer_device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		struct drbd_device *device = peer_device->device;
		kobject_get(&device->kobj);
		rcu_read_unlock();
		drbd_wait_ee_list_empty(device, &device->active_ee);
		kobject_put(&device->kobj);
		rcu_read_lock();
	}
	rcu_read_unlock();
}

void conn_wait_done_ee_empty(struct drbd_connection *connection)
{
	struct drbd_peer_device *peer_device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		struct drbd_device *device = peer_device->device;
		kobject_get(&device->kobj);
		rcu_read_unlock();
		drbd_wait_ee_list_empty(device, &device->done_ee);
		kobject_put(&device->kobj);
		rcu_read_lock();
	}
	rcu_read_unlock();
}

#ifdef blk_queue_plugged
static void drbd_unplug_all_devices(struct drbd_resource *resource)
{
	struct drbd_device *device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&resource->devices, device, vnr) {
		kobject_get(&device->kobj);
		rcu_read_unlock();
		drbd_kick_lo(device);
		kobject_put(&device->kobj);
		rcu_read_lock();
	}
	rcu_read_unlock();
}
#else
static void drbd_unplug_all_devices(struct drbd_resource *resource)
{
}
#endif

static int receive_Barrier(struct drbd_connection *connection, struct packet_info *pi)
{
	int rv, issue_flush;
	struct p_barrier *p = pi->data;
	struct drbd_epoch *epoch;

	drbd_unplug_all_devices(connection->resource);

	/* FIXME these are unacked on connection,
	 * not a specific (peer)device.
	 */
	connection->current_epoch->barrier_nr = p->barrier;
	connection->current_epoch->connection = connection;
	rv = drbd_may_finish_epoch(connection, connection->current_epoch, EV_GOT_BARRIER_NR);

	/* P_BARRIER_ACK may imply that the corresponding extent is dropped from
	 * the activity log, which means it would not be resynced in case the
	 * R_PRIMARY crashes now.
	 * Therefore we must send the barrier_ack after the barrier request was
	 * completed. */
	switch (connection->resource->write_ordering) {
	case WO_BIO_BARRIER:
	case WO_NONE:
		if (rv == FE_RECYCLED)
			return 0;
		break;

	case WO_BDEV_FLUSH:
	case WO_DRAIN_IO:
		if (rv == FE_STILL_LIVE) {
			set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &connection->current_epoch->flags);
			conn_wait_active_ee_empty(connection);
			rv = drbd_flush_after_epoch(connection, connection->current_epoch);
		}
		if (rv == FE_RECYCLED)
			return 0;

		/* The asender will send all the ACKs and barrier ACKs out, since
		   all EEs moved from the active_ee to the done_ee. We need to
		   provide a new epoch object for the EEs that come in soon */
		break;
	}

	/* receiver context, in the writeout path of the other node.
	 * avoid potential distributed deadlock */
	epoch = kmalloc(sizeof(struct drbd_epoch), GFP_NOIO);
	if (!epoch) {
		drbd_warn(connection, "Allocation of an epoch failed, slowing down\n");
		issue_flush = !test_and_set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &connection->current_epoch->flags);
		conn_wait_active_ee_empty(connection);
		if (issue_flush) {
			rv = drbd_flush_after_epoch(connection, connection->current_epoch);
			if (rv == FE_RECYCLED)
				return 0;
		}

		conn_wait_done_ee_empty(connection);

		return 0;
	}

	epoch->flags = 0;
	atomic_set(&epoch->epoch_size, 0);
	atomic_set(&epoch->active, 0);

	spin_lock(&connection->epoch_lock);
	if (atomic_read(&connection->current_epoch->epoch_size)) {
		list_add(&epoch->list, &connection->current_epoch->list);
		connection->current_epoch = epoch;
		connection->epochs++;
	} else {
		/* The current_epoch got recycled while we allocated this one... */
		kfree(epoch);
	}
	spin_unlock(&connection->epoch_lock);

	return 0;
}

/* used from receive_RSDataReply (recv_resync_read)
 * and from receive_Data */
static struct drbd_peer_request *
read_in_block(struct drbd_peer_device *peer_device, u64 id, sector_t sector,
	      struct packet_info *pi) __must_hold(local)
{
	struct drbd_device *device = peer_device->device;
	const sector_t capacity = drbd_get_capacity(device->this_bdev);
	struct drbd_peer_request *peer_req;
	struct page *page;
	int dgs, ds, err;
	int data_size = pi->size;
	void *dig_in = peer_device->connection->int_dig_in;
	void *dig_vv = peer_device->connection->int_dig_vv;
	unsigned long *data;
	struct p_trim *trim = (pi->cmd == P_TRIM) ? pi->data : NULL;

	dgs = 0;
	if (!trim && peer_device->connection->peer_integrity_tfm) {
		dgs = crypto_hash_digestsize(peer_device->connection->peer_integrity_tfm);
		/*
		 * FIXME: Receive the incoming digest into the receive buffer
		 *	  here, together with its struct p_data?
		 */
		err = drbd_recv_all_warn(peer_device->connection, dig_in, dgs);
		if (err)
			return NULL;
		data_size -= dgs;
	}

	if (trim) {
		D_ASSERT(peer_device, data_size == 0);
		data_size = be32_to_cpu(trim->size);
	}

	if (!expect(peer_device, IS_ALIGNED(data_size, 512)))
		return NULL;
	/* prepare for larger trim requests. */
	if (!trim && !expect(peer_device, data_size <= DRBD_MAX_BIO_SIZE))
		return NULL;

	/* even though we trust out peer,
	 * we sometimes have to double check. */
	if (sector + (data_size>>9) > capacity) {
		drbd_err(device, "request from peer beyond end of local disk: "
			"capacity: %llus < sector: %llus + size: %u\n",
			(unsigned long long)capacity,
			(unsigned long long)sector, data_size);
		return NULL;
	}

	/* GFP_NOIO, because we must not cause arbitrary write-out: in a DRBD
	 * "criss-cross" setup, that might cause write-out on some other DRBD,
	 * which in turn might block on the other node at this very place.  */
	peer_req = drbd_alloc_peer_req(peer_device, id, sector, data_size, trim == NULL, GFP_NOIO);
	if (!peer_req)
		return NULL;

	if (trim)
		return peer_req;

	ds = data_size;
	page = peer_req->pages;
	page_chain_for_each(page) {
		unsigned len = min_t(int, ds, PAGE_SIZE);
		data = kmap(page);
		err = drbd_recv_all_warn(peer_device->connection, data, len);
		if (drbd_insert_fault(device, DRBD_FAULT_RECEIVE)) {
			drbd_err(device, "Fault injection: Corrupting data on receive\n");
			data[0] = data[0] ^ (unsigned long)-1;
		}
		kunmap(page);
		if (err) {
			drbd_free_peer_req(device, peer_req);
			return NULL;
		}
		ds -= len;
	}

	if (dgs) {
		drbd_csum_ee(peer_device->connection->peer_integrity_tfm, peer_req, dig_vv);
		if (memcmp(dig_in, dig_vv, dgs)) {
			drbd_err(device, "Digest integrity check FAILED: %llus +%u\n",
				(unsigned long long)sector, data_size);
			drbd_free_peer_req(device, peer_req);
			return NULL;
		}
	}
	peer_device->recv_cnt += data_size >> 9;
	return peer_req;
}

/* drbd_drain_block() just takes a data block
 * out of the socket input buffer, and discards it.
 */
static int drbd_drain_block(struct drbd_peer_device *peer_device, int data_size)
{
	struct page *page;
	int err = 0;
	void *data;

	if (!data_size)
		return 0;

	page = drbd_alloc_pages(peer_device, 1, 1);

	data = kmap(page);
	while (data_size) {
		unsigned int len = min_t(int, data_size, PAGE_SIZE);

		err = drbd_recv_all_warn(peer_device->connection, data, len);
		if (err)
			break;
		data_size -= len;
	}
	kunmap(page);
	drbd_free_pages(peer_device->device, page, 0);
	return err;
}

static int recv_dless_read(struct drbd_peer_device *peer_device, struct drbd_request *req,
			   sector_t sector, int data_size)
{
	struct bio_vec *bvec;
	struct bio *bio;
	int dgs, err, i, expect;
	void *dig_in = peer_device->connection->int_dig_in;
	void *dig_vv = peer_device->connection->int_dig_vv;

	dgs = 0;
	if (peer_device->connection->peer_integrity_tfm) {
		dgs = crypto_hash_digestsize(peer_device->connection->peer_integrity_tfm);
		err = drbd_recv_all_warn(peer_device->connection, dig_in, dgs);
		if (err)
			return err;
		data_size -= dgs;
	}

	/* optimistically update recv_cnt.  if receiving fails below,
	 * we disconnect anyways, and counters will be reset. */
	peer_device->recv_cnt += data_size >> 9;

	bio = req->master_bio;
	D_ASSERT(peer_device->device, sector == bio->bi_sector);

	bio_for_each_segment(bvec, bio, i) {
		void *mapped = kmap(bvec->bv_page) + bvec->bv_offset;
		expect = min_t(int, data_size, bvec->bv_len);
		err = drbd_recv_all_warn(peer_device->connection, mapped, expect);
		kunmap(bvec->bv_page);
		if (err)
			return err;
		data_size -= expect;
	}

	if (dgs) {
		drbd_csum_bio(peer_device->connection->peer_integrity_tfm, bio, dig_vv);
		if (memcmp(dig_in, dig_vv, dgs)) {
			drbd_err(peer_device, "Digest integrity check FAILED. Broken NICs?\n");
			return -EINVAL;
		}
	}

	D_ASSERT(peer_device->device, data_size == 0);
	return 0;
}

/*
 * e_end_resync_block() is called in asender context via
 * drbd_finish_peer_reqs().
 */
static int e_end_resync_block(struct drbd_work *w, int unused)
{
	struct drbd_peer_request *peer_req =
		container_of(w, struct drbd_peer_request, w);
	struct drbd_peer_device *peer_device = peer_req->peer_device;
	struct drbd_device *device = peer_device->device;
	sector_t sector = peer_req->i.sector;
	int err;

	D_ASSERT(device, drbd_interval_empty(&peer_req->i));

	if (likely((peer_req->flags & EE_WAS_ERROR) == 0)) {
		drbd_set_in_sync(peer_device, sector, peer_req->i.size);
		err = drbd_send_ack(peer_device, P_RS_WRITE_ACK, peer_req);
	} else {
		/* Record failure to sync */
		drbd_rs_failed_io(peer_device, sector, peer_req->i.size);

		err  = drbd_send_ack(peer_device, P_NEG_ACK, peer_req);
	}
	dec_unacked(peer_device);

	return err;
}

static int recv_resync_read(struct drbd_peer_device *peer_device, sector_t sector,
			    struct packet_info *pi) __releases(local)
{
	struct drbd_device *device = peer_device->device;
	struct drbd_peer_request *peer_req;

	peer_req = read_in_block(peer_device, ID_SYNCER, sector, pi);
	if (!peer_req)
		goto fail;

	dec_rs_pending(peer_device);

	inc_unacked(peer_device);
	/* corresponding dec_unacked() in e_end_resync_block()
	 * respective _drbd_clear_done_ee */

	peer_req->w.cb = e_end_resync_block;

	spin_lock_irq(&device->resource->req_lock);
	list_add(&peer_req->w.list, &device->sync_ee);
	spin_unlock_irq(&device->resource->req_lock);

	atomic_add(pi->size >> 9, &device->rs_sect_ev);

	/* Seting all peer out of sync here. Sync source peer will be set
	   in sync when the write completes. Other peers will be set in
	   sync by the sync source with a P_PEERS_IN_SYNC packet soon. */
	drbd_set_all_out_of_sync(device, peer_req->i.sector, peer_req->i.size);

	if (drbd_submit_peer_request(device, peer_req, WRITE, DRBD_FAULT_RS_WR) == 0)
		return 0;

	/* don't care for the reason here */
	drbd_err(device, "submit failed, triggering re-connect\n");
	spin_lock_irq(&device->resource->req_lock);
	list_del(&peer_req->w.list);
	spin_unlock_irq(&device->resource->req_lock);

	drbd_free_peer_req(device, peer_req);
fail:
	put_ldev(device);
	return -EIO;
}

static struct drbd_request *
find_request(struct drbd_device *device, struct rb_root *root, u64 id,
	     sector_t sector, bool missing_ok, const char *func)
{
	struct drbd_request *req;

	/* Request object according to our peer */
	req = (struct drbd_request *)(unsigned long)id;
	if (drbd_contains_interval(root, sector, &req->i) && req->i.local)
		return req;
	if (!missing_ok) {
		drbd_err(device, "%s: failed to find request 0x%lx, sector %llus\n", func,
			(unsigned long)id, (unsigned long long)sector);
	}
	return NULL;
}

static int receive_DataReply(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct drbd_request *req;
	sector_t sector;
	int err;
	struct p_data *p = pi->data;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	sector = be64_to_cpu(p->sector);

	spin_lock_irq(&device->resource->req_lock);
	req = find_request(device, &device->read_requests, p->block_id, sector, false, __func__);
	spin_unlock_irq(&device->resource->req_lock);
	if (unlikely(!req))
		return -EIO;

	/* drbd_remove_request_interval() is done in _req_may_be_done, to avoid
	 * special casing it there for the various failure cases.
	 * still no race with drbd_fail_pending_reads */
	err = recv_dless_read(peer_device, req, sector, pi->size);
	if (!err)
		req_mod(req, DATA_RECEIVED, peer_device);
	/* else: nothing. handled from drbd_disconnect...
	 * I don't think we may complete this just yet
	 * in case we are "on-disconnect: freeze" */

	return err;
}

static int receive_RSDataReply(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	sector_t sector;
	int err;
	struct p_data *p = pi->data;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	sector = be64_to_cpu(p->sector);
	D_ASSERT(device, p->block_id == ID_SYNCER);

	if (get_ldev(device)) {
		/* data is submitted to disk within recv_resync_read.
		 * corresponding put_ldev done below on error,
		 * or in drbd_peer_request_endio. */
		err = recv_resync_read(peer_device, sector, pi);
	} else {
		if (drbd_ratelimit())
			drbd_err(device, "Can not write resync data to local disk.\n");

		err = drbd_drain_block(peer_device, pi->size);

		drbd_send_ack_dp(peer_device, P_NEG_ACK, p, pi->size);
	}

	atomic_add(pi->size >> 9, &peer_device->rs_sect_in);

	return err;
}

static void restart_conflicting_writes(struct drbd_peer_request *peer_req)
{
	struct drbd_interval *i;
	struct drbd_request *req;
	struct drbd_device *device = peer_req->peer_device->device;
	const sector_t sector = peer_req->i.sector;
	const unsigned int size = peer_req->i.size;

	drbd_for_each_overlap(i, &device->write_requests, sector, size) {
		if (!i->local)
			continue;
		req = container_of(i, struct drbd_request, i);
		if ((req->rq_state[0] & RQ_LOCAL_PENDING) ||
		   !(req->rq_state[0] & RQ_POSTPONED))
			continue;
		/* as it is RQ_POSTPONED, this will cause it to
		 * be queued on the retry workqueue. */
		__req_mod(req, DISCARD_WRITE, peer_req->peer_device, NULL);
	}
}

/*
 * e_end_block() is called in asender context via drbd_finish_peer_reqs().
 */
static int e_end_block(struct drbd_work *w, int cancel)
{
	struct drbd_peer_request *peer_req =
		container_of(w, struct drbd_peer_request, w);
	struct drbd_peer_device *peer_device = peer_req->peer_device;
	struct drbd_device *device = peer_device->device;
	sector_t sector = peer_req->i.sector;
	struct drbd_epoch *epoch;
	int err = 0, pcmd;

	if (peer_req->flags & EE_IS_BARRIER) {
		epoch = previous_epoch(peer_device->connection, peer_req->epoch);
		if (epoch)
			drbd_may_finish_epoch(peer_device->connection, epoch, EV_BARRIER_DONE + (cancel ? EV_CLEANUP : 0));
	}

	if (peer_req->flags & EE_SEND_WRITE_ACK) {
		if (likely((peer_req->flags & EE_WAS_ERROR) == 0)) {
			pcmd = (peer_device->repl_state[NOW] >= L_SYNC_SOURCE &&
				peer_device->repl_state[NOW] <= L_PAUSED_SYNC_T &&
				peer_req->flags & EE_MAY_SET_IN_SYNC) ?
				P_RS_WRITE_ACK : P_WRITE_ACK;
			err = drbd_send_ack(peer_device, pcmd, peer_req);
			if (pcmd == P_RS_WRITE_ACK)
				drbd_set_in_sync(peer_device, sector, peer_req->i.size);
		} else {
			err = drbd_send_ack(peer_device, P_NEG_ACK, peer_req);
			/* we expect it to be marked out of sync anyways...
			 * maybe assert this?  */
		}
		dec_unacked(peer_device);
	}
	/* we delete from the conflict detection hash _after_ we sent out the
	 * P_WRITE_ACK / P_NEG_ACK, to get the sequence number right.  */
	if (peer_req->flags & EE_IN_INTERVAL_TREE) {
		spin_lock_irq(&device->resource->req_lock);
		D_ASSERT(device, !drbd_interval_empty(&peer_req->i));
		drbd_remove_peer_req_interval(device, peer_req);
		if (peer_req->flags & EE_RESTART_REQUESTS)
			restart_conflicting_writes(peer_req);
		spin_unlock_irq(&device->resource->req_lock);
	} else
		D_ASSERT(device, drbd_interval_empty(&peer_req->i));

	drbd_may_finish_epoch(peer_device->connection, peer_req->epoch, EV_PUT + (cancel ? EV_CLEANUP : 0));

	return err;
}

static int e_send_ack(struct drbd_work *w, enum drbd_packet ack)
{
	struct drbd_peer_request *peer_req =
		container_of(w, struct drbd_peer_request, w);
	struct drbd_peer_device *peer_device = peer_req->peer_device;
	int err;

	err = drbd_send_ack(peer_device, ack, peer_req);
	dec_unacked(peer_device);

	return err;
}

static int e_send_discard_write(struct drbd_work *w, int unused)
{
	return e_send_ack(w, P_SUPERSEDED);
}

static int e_send_retry_write(struct drbd_work *w, int unused)
{

	struct drbd_peer_request *peer_request =
		container_of(w, struct drbd_peer_request, w);
	struct drbd_connection *connection = peer_request->peer_device->connection;

	return e_send_ack(w, connection->agreed_pro_version >= 100 ?
			     P_RETRY_WRITE : P_SUPERSEDED);
}

static bool seq_greater(u32 a, u32 b)
{
	/*
	 * We assume 32-bit wrap-around here.
	 * For 24-bit wrap-around, we would have to shift:
	 *  a <<= 8; b <<= 8;
	 */
	return (s32)a - (s32)b > 0;
}

static u32 seq_max(u32 a, u32 b)
{
	return seq_greater(a, b) ? a : b;
}

static void update_peer_seq(struct drbd_peer_device *peer_device, unsigned int peer_seq)
{
	unsigned int newest_peer_seq;

	if (test_bit(RESOLVE_CONFLICTS, &peer_device->connection->flags)) {
		spin_lock(&peer_device->peer_seq_lock);
		newest_peer_seq = seq_max(peer_device->peer_seq, peer_seq);
		peer_device->peer_seq = newest_peer_seq;
		spin_unlock(&peer_device->peer_seq_lock);
		/* wake up only if we actually changed peer_device->peer_seq */
		if (peer_seq == newest_peer_seq)
			wake_up(&peer_device->device->seq_wait);
	}
}

static inline int overlaps(sector_t s1, int l1, sector_t s2, int l2)
{
	return !((s1 + (l1>>9) <= s2) || (s1 >= s2 + (l2>>9)));
}

/* maybe change sync_ee into interval trees as well? */
static bool overlapping_resync_write(struct drbd_device *device, struct drbd_peer_request *peer_req)
{
	struct drbd_peer_request *rs_req;
	bool rv = 0;

	spin_lock_irq(&device->resource->req_lock);
	list_for_each_entry(rs_req, &device->sync_ee, w.list) {
		if (overlaps(peer_req->i.sector, peer_req->i.size,
			     rs_req->i.sector, rs_req->i.size)) {
			rv = 1;
			break;
		}
	}
	spin_unlock_irq(&device->resource->req_lock);

	return rv;
}

/* Called from receive_Data.
 * Synchronize packets on sock with packets on msock.
 *
 * This is here so even when a P_DATA packet traveling via sock overtook an Ack
 * packet traveling on msock, they are still processed in the order they have
 * been sent.
 *
 * Note: we don't care for Ack packets overtaking P_DATA packets.
 *
 * In case packet_seq is larger than peer_device->peer_seq number, there are
 * outstanding packets on the msock. We wait for them to arrive.
 * In case we are the logically next packet, we update peer_device->peer_seq
 * ourselves. Correctly handles 32bit wrap around.
 *
 * Assume we have a 10 GBit connection, that is about 1<<30 byte per second,
 * about 1<<21 sectors per second. So "worst" case, we have 1<<3 == 8 seconds
 * for the 24bit wrap (historical atomic_t guarantee on some archs), and we have
 * 1<<9 == 512 seconds aka ages for the 32bit wrap around...
 *
 * returns 0 if we may process the packet,
 * -ERESTARTSYS if we were interrupted (by disconnect signal). */
static int wait_for_and_update_peer_seq(struct drbd_peer_device *peer_device, const u32 peer_seq)
{
	struct drbd_connection *connection = peer_device->connection;
	DEFINE_WAIT(wait);
	long timeout;
	int ret = 0, tp;

	if (!test_bit(RESOLVE_CONFLICTS, &connection->flags))
		return 0;

	spin_lock(&peer_device->peer_seq_lock);
	for (;;) {
		if (!seq_greater(peer_seq - 1, peer_device->peer_seq)) {
			peer_device->peer_seq = seq_max(peer_device->peer_seq, peer_seq);
			break;
		}

		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}

		rcu_read_lock();
		tp = rcu_dereference(connection->net_conf)->two_primaries;
		rcu_read_unlock();

		if (!tp)
			break;

		/* Only need to wait if two_primaries is enabled */
		prepare_to_wait(&peer_device->device->seq_wait, &wait, TASK_INTERRUPTIBLE);
		spin_unlock(&peer_device->peer_seq_lock);
		rcu_read_lock();
		timeout = rcu_dereference(connection->net_conf)->ping_timeo*HZ/10;
		rcu_read_unlock();
		timeout = schedule_timeout(timeout);
		spin_lock(&peer_device->peer_seq_lock);
		if (!timeout) {
			ret = -ETIMEDOUT;
			drbd_err(peer_device, "Timed out waiting for missing ack packets; disconnecting\n");
			break;
		}
	}
	spin_unlock(&peer_device->peer_seq_lock);
	finish_wait(&peer_device->device->seq_wait, &wait);
	return ret;
}

/* see also bio_flags_to_wire()
 * DRBD_REQ_*, because we need to semantically map the flags to data packet
 * flags and back. We may replicate to other kernel versions. */
static unsigned long wire_flags_to_bio(struct drbd_connection *connection, u32 dpf)
{
	if (connection->agreed_pro_version >= 95)
		return  (dpf & DP_RW_SYNC ? DRBD_REQ_SYNC : 0) |
			(dpf & DP_UNPLUG ? DRBD_REQ_UNPLUG : 0) |
			(dpf & DP_FUA ? DRBD_REQ_FUA : 0) |
			(dpf & DP_FLUSH ? DRBD_REQ_FLUSH : 0) |
			(dpf & DP_DISCARD ? DRBD_REQ_DISCARD : 0);

	/* else: we used to communicate one bit only in older DRBD */
	return dpf & DP_RW_SYNC ? (DRBD_REQ_SYNC | DRBD_REQ_UNPLUG) : 0;
}

static void fail_postponed_requests(struct drbd_peer_request *peer_req)
{
	struct drbd_device *device = peer_req->peer_device->device;
	struct drbd_interval *i;
	const sector_t sector = peer_req->i.sector;
	const unsigned int size = peer_req->i.size;

    repeat:
	drbd_for_each_overlap(i, &device->write_requests, sector, size) {
		struct drbd_request *req;
		struct bio_and_error m;

		if (!i->local)
			continue;
		req = container_of(i, struct drbd_request, i);
		if (!(req->rq_state[0] & RQ_POSTPONED))
			continue;
		req->rq_state[0] &= ~RQ_POSTPONED;
		__req_mod(req, NEG_ACKED, peer_req->peer_device, &m);
		spin_unlock_irq(&device->resource->req_lock);
		if (m.bio)
			complete_master_bio(device, &m);
		spin_lock_irq(&device->resource->req_lock);
		goto repeat;
	}
}

static int handle_write_conflicts(struct drbd_peer_request *peer_req)
{
	struct drbd_peer_device *peer_device = peer_req->peer_device;
	struct drbd_device *device = peer_device->device;
	struct drbd_connection *connection = peer_device->connection;
	bool resolve_conflicts = test_bit(RESOLVE_CONFLICTS, &connection->flags);
	sector_t sector = peer_req->i.sector;
	const unsigned int size = peer_req->i.size;
	struct drbd_interval *i;
	bool equal;
	int err;

	/*
	 * Inserting the peer request into the write_requests tree will prevent
	 * new conflicting local requests from being added.
	 */
	drbd_insert_interval(&device->write_requests, &peer_req->i);

    repeat:
	drbd_for_each_overlap(i, &device->write_requests, sector, size) {
		if (i == &peer_req->i)
			continue;

		if (!i->local) {
			/*
			 * Our peer has sent a conflicting remote request; this
			 * should not happen in a two-node setup.  Wait for the
			 * earlier peer request to complete.
			 */
			err = drbd_wait_misc(device, peer_device, i);
			if (err)
				goto out;
			goto repeat;
		}

		equal = i->sector == sector && i->size == size;
		if (resolve_conflicts) {
			/*
			 * If the peer request is fully contained within the
			 * overlapping request, it can be discarded; otherwise,
			 * it will be retried once all overlapping requests
			 * have completed.
			 */
			bool discard = i->sector <= sector && i->sector +
				       (i->size >> 9) >= sector + (size >> 9);

			if (!equal)
				drbd_alert(device, "Concurrent writes detected: "
					       "local=%llus +%u, remote=%llus +%u, "
					       "assuming %s came first\n",
					  (unsigned long long)i->sector, i->size,
					  (unsigned long long)sector, size,
					  discard ? "local" : "remote");

			inc_unacked(peer_device);
			peer_req->w.cb = discard ? e_send_discard_write :
						   e_send_retry_write;
			list_add_tail(&peer_req->w.list, &device->done_ee);
			wake_asender(connection);

			err = -ENOENT;
			goto out;
		} else {
			struct drbd_request *req =
				container_of(i, struct drbd_request, i);

			if (!equal)
				drbd_alert(device, "Concurrent writes detected: "
					       "local=%llus +%u, remote=%llus +%u\n",
					  (unsigned long long)i->sector, i->size,
					  (unsigned long long)sector, size);

			if (req->rq_state[0] & RQ_LOCAL_PENDING ||
			    !(req->rq_state[0] & RQ_POSTPONED)) {
				/*
				 * Wait for the node with the discard flag to
				 * decide if this request will be discarded or
				 * retried.  Requests that are discarded will
				 * disappear from the write_requests tree.
				 *
				 * In addition, wait for the conflicting
				 * request to finish locally before submitting
				 * the conflicting peer request.
				 */
				err = drbd_wait_misc(device, NULL, &req->i);
				if (err) {
					begin_state_change_locked(connection->resource, CS_HARD);
					__change_cstate(connection, C_TIMEOUT);
					end_state_change_locked(connection->resource);
					fail_postponed_requests(peer_req);
					goto out;
				}
				goto repeat;
			}
			/*
			 * Remember to restart the conflicting requests after
			 * the new peer request has completed.
			 */
			peer_req->flags |= EE_RESTART_REQUESTS;
		}
	}
	err = 0;

    out:
	if (err)
		drbd_remove_peer_req_interval(device, peer_req);
	return err;
}

/* mirrored write */
static int receive_Data(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	sector_t sector;
	struct drbd_peer_request *peer_req;
	struct p_data *p = pi->data;
	u32 peer_seq = be32_to_cpu(p->seq_num);
	int rw = WRITE;
	u32 dp_flags;
	int err, tp;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	if (!get_ldev(device)) {
		int err2;

		err = wait_for_and_update_peer_seq(peer_device, peer_seq);
		drbd_send_ack_dp(peer_device, P_NEG_ACK, p, pi->size);
		atomic_inc(&connection->current_epoch->epoch_size);
		err2 = drbd_drain_block(peer_device, pi->size);
		if (!err)
			err = err2;
		return err;
	}

	/*
	 * Corresponding put_ldev done either below (on various errors), or in
	 * drbd_peer_request_endio, if we successfully submit the data at the
	 * end of this function.
	 */

	sector = be64_to_cpu(p->sector);
	peer_req = read_in_block(peer_device, p->block_id, sector, pi);
	if (!peer_req) {
		put_ldev(device);
		return -EIO;
	}

	peer_req->dagtag_sector = connection->last_dagtag_sector + (pi->size >> 9);
	connection->last_dagtag_sector = peer_req->dagtag_sector;

	peer_req->w.cb = e_end_block;

	dp_flags = be32_to_cpu(p->dp_flags);
	rw |= wire_flags_to_bio(connection, dp_flags);
	if (pi->cmd == P_TRIM) {
		struct request_queue *q = bdev_get_queue(device->ldev->backing_bdev);
		peer_req->flags |= EE_IS_TRIM;
		if (!blk_queue_discard(q))
			peer_req->flags |= EE_IS_TRIM_USE_ZEROOUT;
		D_ASSERT(peer_device, peer_req->i.size > 0);
		D_ASSERT(peer_device, rw & DRBD_REQ_DISCARD);
		D_ASSERT(peer_device, peer_req->pages == NULL);
	} else if (peer_req->pages == NULL) {
		D_ASSERT(device, peer_req->i.size == 0);
		D_ASSERT(device, dp_flags & DP_FLUSH);
	}

	if (dp_flags & DP_MAY_SET_IN_SYNC)
		peer_req->flags |= EE_MAY_SET_IN_SYNC;

	/* last "fixes" to rw flags.
	 * Strip off BIO_RW_BARRIER unconditionally,
	 * it is not supposed to be here anyways.
	 * (Was FUA or FLUSH on the peer,
	 * and got translated to BARRIER on this side).
	 * Note that the epoch handling code below
	 * may add it again, though.
	 */
	rw &= ~DRBD_REQ_HARDBARRIER;

	spin_lock(&connection->epoch_lock);
	peer_req->epoch = connection->current_epoch;
	atomic_inc(&peer_req->epoch->epoch_size);
	atomic_inc(&peer_req->epoch->active);

	if (connection->resource->write_ordering == WO_BIO_BARRIER &&
	    atomic_read(&peer_req->epoch->epoch_size) == 1) {
		struct drbd_epoch *epoch;
		/* Issue a barrier if we start a new epoch, and the previous epoch
		   was not a epoch containing a single request which already was
		   a Barrier. */
		epoch = list_entry(peer_req->epoch->list.prev, struct drbd_epoch, list);
		if (epoch == peer_req->epoch) {
			set_bit(DE_CONTAINS_A_BARRIER, &peer_req->epoch->flags);
			rw |= DRBD_REQ_FLUSH | DRBD_REQ_FUA;
			peer_req->flags |= EE_IS_BARRIER;
		} else {
			if (atomic_read(&epoch->epoch_size) > 1 ||
			    !test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags)) {
				set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags);
				set_bit(DE_CONTAINS_A_BARRIER, &peer_req->epoch->flags);
				rw |= DRBD_REQ_FLUSH | DRBD_REQ_FUA;
				peer_req->flags |= EE_IS_BARRIER;
			}
		}
	}
	spin_unlock(&connection->epoch_lock);

	rcu_read_lock();
	tp = rcu_dereference(peer_device->connection->net_conf)->two_primaries;
	rcu_read_unlock();
	if (tp) {
		peer_req->flags |= EE_IN_INTERVAL_TREE;
		err = wait_for_and_update_peer_seq(peer_device, peer_seq);
		if (err)
			goto out_interrupted;
		spin_lock_irq(&device->resource->req_lock);
		err = handle_write_conflicts(peer_req);
		if (err) {
			spin_unlock_irq(&device->resource->req_lock);
			if (err == -ENOENT) {
				put_ldev(device);
				return 0;
			}
			goto out_interrupted;
		}
	} else {
		update_peer_seq(peer_device, peer_seq);
		spin_lock_irq(&device->resource->req_lock);
	}
	/* if we use the zeroout fallback code, we process synchronously
	 * and we wait for all pending requests, respectively wait for
	 * active_ee to become empty in drbd_submit_peer_request();
	 * better not add ourselves here. */
	if ((peer_req->flags & EE_IS_TRIM_USE_ZEROOUT) == 0)
		list_add(&peer_req->w.list, &device->active_ee);
	if (connection->agreed_pro_version >= 110)
		list_add_tail(&peer_req->recv_order, &connection->peer_requests);
	spin_unlock_irq(&device->resource->req_lock);

	if (peer_device->repl_state[NOW] == L_SYNC_TARGET)
		wait_event(device->ee_wait, !overlapping_resync_write(device, peer_req));

	if (peer_device->connection->agreed_pro_version < 100) {
		rcu_read_lock();
		switch (rcu_dereference(peer_device->connection->net_conf)->wire_protocol) {
		case DRBD_PROT_C:
			dp_flags |= DP_SEND_WRITE_ACK;
			break;
		case DRBD_PROT_B:
			dp_flags |= DP_SEND_RECEIVE_ACK;
			break;
		}
		rcu_read_unlock();
	}

	if (dp_flags & DP_SEND_WRITE_ACK) {
		peer_req->flags |= EE_SEND_WRITE_ACK;
		inc_unacked(peer_device);
		/* corresponding dec_unacked() in e_end_block()
		 * respective _drbd_clear_done_ee */
	}

	if (dp_flags & DP_SEND_RECEIVE_ACK) {
		/* I really don't like it that the receiver thread
		 * sends on the msock, but anyways */
		drbd_send_ack(peer_device, P_RECV_ACK, peer_req);
	}

	drbd_al_begin_io_for_peer(peer_device, &peer_req->i);

	err = drbd_submit_peer_request(device, peer_req, rw, DRBD_FAULT_DT_WR);
	if (!err)
		return 0;

	/* don't care for the reason here */
	drbd_err(device, "submit failed, triggering re-connect\n");
	spin_lock_irq(&device->resource->req_lock);
	list_del(&peer_req->w.list);
	list_del_init(&peer_req->recv_order);
	drbd_remove_peer_req_interval(device, peer_req);
	spin_unlock_irq(&device->resource->req_lock);
	drbd_al_complete_io(device, &peer_req->i);

out_interrupted:
	drbd_may_finish_epoch(connection, peer_req->epoch, EV_PUT + EV_CLEANUP);
	put_ldev(device);
	drbd_free_peer_req(device, peer_req);
	return err;
}

/* We may throttle resync, if the lower device seems to be busy,
 * and current sync rate is above c_min_rate.
 *
 * To decide whether or not the lower device is busy, we use a scheme similar
 * to MD RAID is_mddev_idle(): if the partition stats reveal "significant"
 * (more than 64 sectors) of activity we cannot account for with our own resync
 * activity, it obviously is "busy".
 *
 * The current sync rate used here uses only the most recent two step marks,
 * to have a short time average so we can react faster.
 */
bool drbd_rs_should_slow_down(struct drbd_peer_device *peer_device, sector_t sector)
{
	if (!drbd_rs_c_min_rate_throttle(peer_device))
		return false;

	return !drbd_sector_has_priority(peer_device, sector);
}

bool drbd_rs_c_min_rate_throttle(struct drbd_peer_device *peer_device)
{
	struct drbd_device *device = peer_device->device;
	unsigned long db, dt, dbdt;
	unsigned int c_min_rate;
	int curr_events;

	rcu_read_lock();
	c_min_rate = rcu_dereference(device->ldev->disk_conf)->c_min_rate;
	rcu_read_unlock();

	/* feature disabled? */
	if (c_min_rate == 0)
		return false;

	curr_events = drbd_backing_bdev_events(device->ldev->backing_bdev->bd_contains->bd_disk)
		    - atomic_read(&device->rs_sect_ev);

	if (!peer_device->rs_last_events ||
	    curr_events - peer_device->rs_last_events > 64) {
		unsigned long rs_left;
		int i;

		peer_device->rs_last_events = curr_events;

		/* sync speed average over the last 2*DRBD_SYNC_MARK_STEP,
		 * approx. */
		i = (peer_device->rs_last_mark + DRBD_SYNC_MARKS-1) % DRBD_SYNC_MARKS;

		if (peer_device->repl_state[NOW] == L_VERIFY_S || peer_device->repl_state[NOW] == L_VERIFY_T)
			rs_left = peer_device->ov_left;
		else
			rs_left = drbd_bm_total_weight(peer_device) - peer_device->rs_failed;

		dt = ((long)jiffies - (long)peer_device->rs_mark_time[i]) / HZ;
		if (!dt)
			dt++;
		db = peer_device->rs_mark_left[i] - rs_left;
		dbdt = Bit2KB(db/dt);

		if (dbdt > c_min_rate)
			return true;
	}
	return false;
}

static int receive_DataRequest(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	sector_t sector;
	sector_t capacity;
	struct drbd_peer_request *peer_req;
	struct digest_info *di = NULL;
	int size, verb;
	unsigned int fault_type;
	struct p_block_req *p =	pi->data;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;
	capacity = drbd_get_capacity(device->this_bdev);

	sector = be64_to_cpu(p->sector);
	size   = be32_to_cpu(p->blksize);

	if (size <= 0 || !IS_ALIGNED(size, 512) || size > DRBD_MAX_BIO_SIZE) {
		drbd_err(device, "%s:%d: sector: %llus, size: %u\n", __FILE__, __LINE__,
				(unsigned long long)sector, size);
		return -EINVAL;
	}
	if (sector + (size>>9) > capacity) {
		drbd_err(device, "%s:%d: sector: %llus, size: %u\n", __FILE__, __LINE__,
				(unsigned long long)sector, size);
		return -EINVAL;
	}

	if (!get_ldev_if_state(device, D_UP_TO_DATE)) {
		verb = 1;
		switch (pi->cmd) {
		case P_DATA_REQUEST:
			drbd_send_ack_rp(peer_device, P_NEG_DREPLY, p);
			break;
		case P_RS_DATA_REQUEST:
		case P_CSUM_RS_REQUEST:
		case P_OV_REQUEST:
			drbd_send_ack_rp(peer_device, P_NEG_RS_DREPLY , p);
			break;
		case P_OV_REPLY:
			verb = 0;
			dec_rs_pending(peer_device);
			drbd_send_ack_ex(peer_device, P_OV_RESULT, sector, size, ID_IN_SYNC);
			break;
		default:
			BUG();
		}
		if (verb && drbd_ratelimit())
			drbd_err(device, "Can not satisfy peer's read request, "
			    "no local data.\n");

		/* drain possibly payload */
		return drbd_drain_block(peer_device, pi->size);
	}

	/* GFP_NOIO, because we must not cause arbitrary write-out: in a DRBD
	 * "criss-cross" setup, that might cause write-out on some other DRBD,
	 * which in turn might block on the other node at this very place.  */
	peer_req = drbd_alloc_peer_req(peer_device, p->block_id, sector, size,
			true /* has real payload */, GFP_NOIO);
	if (!peer_req) {
		put_ldev(device);
		return -ENOMEM;
	}

	switch (pi->cmd) {
	case P_DATA_REQUEST:
		peer_req->w.cb = w_e_end_data_req;
		fault_type = DRBD_FAULT_DT_RD;
		/* application IO, don't drbd_rs_begin_io */
		goto submit;

	case P_RS_DATA_REQUEST:
		peer_req->w.cb = w_e_end_rsdata_req;
		fault_type = DRBD_FAULT_RS_RD;
		/* used in the sector offset progress display */
		device->bm_resync_fo = BM_SECT_TO_BIT(sector);
		break;

	case P_OV_REPLY:
	case P_CSUM_RS_REQUEST:
		fault_type = DRBD_FAULT_RS_RD;
		di = kmalloc(sizeof(*di) + pi->size, GFP_NOIO);
		if (!di)
			goto out_free_e;

		di->digest_size = pi->size;
		di->digest = (((char *)di)+sizeof(struct digest_info));

		peer_req->digest = di;
		peer_req->flags |= EE_HAS_DIGEST;

		if (drbd_recv_all(peer_device->connection, di->digest, pi->size))
			goto out_free_e;

		if (pi->cmd == P_CSUM_RS_REQUEST) {
			D_ASSERT(device, peer_device->connection->agreed_pro_version >= 89);
			peer_req->w.cb = w_e_end_csum_rs_req;
			/* used in the sector offset progress display */
			device->bm_resync_fo = BM_SECT_TO_BIT(sector);
		} else if (pi->cmd == P_OV_REPLY) {
			/* track progress, we may need to throttle */
			atomic_add(size >> 9, &peer_device->rs_sect_in);
			peer_req->w.cb = w_e_end_ov_reply;
			dec_rs_pending(peer_device);
			/* drbd_rs_begin_io done when we sent this request,
			 * but accounting still needs to be done. */
			goto submit_for_resync;
		}
		break;

	case P_OV_REQUEST:
		if (peer_device->ov_start_sector == ~(sector_t)0 &&
		    peer_device->connection->agreed_pro_version >= 90) {
			unsigned long now = jiffies;
			int i;
			peer_device->ov_start_sector = sector;
			peer_device->ov_position = sector;
			peer_device->ov_left = drbd_bm_bits(device) - BM_SECT_TO_BIT(sector);
			peer_device->rs_total = peer_device->ov_left;
			for (i = 0; i < DRBD_SYNC_MARKS; i++) {
				peer_device->rs_mark_left[i] = peer_device->ov_left;
				peer_device->rs_mark_time[i] = now;
			}
			drbd_info(device, "Online Verify start sector: %llu\n",
					(unsigned long long)sector);
		}
		peer_req->w.cb = w_e_end_ov_req;
		fault_type = DRBD_FAULT_RS_RD;
		break;

	default:
		BUG();
	}

	/* Throttle, drbd_rs_begin_io and submit should become asynchronous
	 * wrt the receiver, but it is not as straightforward as it may seem.
	 * Various places in the resync start and stop logic assume resync
	 * requests are processed in order, requeuing this on the worker thread
	 * introduces a bunch of new code for synchronization between threads.
	 *
	 * Unlimited throttling before drbd_rs_begin_io may stall the resync
	 * "forever", throttling after drbd_rs_begin_io will lock that extent
	 * for application writes for the same time.  For now, just throttle
	 * here, where the rest of the code expects the receiver to sleep for
	 * a while, anyways.
	 */

	/* Throttle before drbd_rs_begin_io, as that locks out application IO;
	 * this defers syncer requests for some time, before letting at least
	 * on request through.  The resync controller on the receiving side
	 * will adapt to the incoming rate accordingly.
	 *
	 * We cannot throttle here if remote is Primary/SyncTarget:
	 * we would also throttle its application reads.
	 * In that case, throttling is done on the SyncTarget only.
	 */
	if (connection->peer_role[NOW] != R_PRIMARY &&
	    drbd_rs_should_slow_down(peer_device, sector))
		schedule_timeout_uninterruptible(HZ/10);
	if (drbd_rs_begin_io(peer_device, sector))
		goto out_free_e;

submit_for_resync:
	atomic_add(size >> 9, &device->rs_sect_ev);

submit:
	inc_unacked(peer_device);
	spin_lock_irq(&device->resource->req_lock);
	list_add_tail(&peer_req->w.list, &device->read_ee);
	spin_unlock_irq(&device->resource->req_lock);

	if (drbd_submit_peer_request(device, peer_req, READ, fault_type) == 0)
		return 0;

	/* don't care for the reason here */
	drbd_err(device, "submit failed, triggering re-connect\n");
	spin_lock_irq(&device->resource->req_lock);
	list_del(&peer_req->w.list);
	spin_unlock_irq(&device->resource->req_lock);
	/* no drbd_rs_complete_io(), we are dropping the connection anyways */

out_free_e:
	put_ldev(device);
	drbd_free_peer_req(device, peer_req);
	return -EIO;
}

/**
 * drbd_asb_recover_0p  -  Recover after split-brain with no remaining primaries
 */
static int drbd_asb_recover_0p(struct drbd_peer_device *peer_device) __must_hold(local)
{
	const int node_id = peer_device->device->resource->res_opts.node_id;
	int self, peer, rv = -100;
	unsigned long ch_self, ch_peer;
	enum drbd_after_sb_p after_sb_0p;

	self = drbd_bitmap_uuid(peer_device) & 1;
	peer = peer_device->bitmap_uuids[node_id] & 1;

	ch_peer = peer_device->dirty_bits;
	ch_self = peer_device->comm_bm_set;

	rcu_read_lock();
	after_sb_0p = rcu_dereference(peer_device->connection->net_conf)->after_sb_0p;
	rcu_read_unlock();
	switch (after_sb_0p) {
	case ASB_CONSENSUS:
	case ASB_DISCARD_SECONDARY:
	case ASB_CALL_HELPER:
	case ASB_VIOLENTLY:
		drbd_err(peer_device, "Configuration error.\n");
		break;
	case ASB_DISCONNECT:
		break;
	case ASB_DISCARD_YOUNGER_PRI:
		if (self == 0 && peer == 1) {
			rv = -1;
			break;
		}
		if (self == 1 && peer == 0) {
			rv =  1;
			break;
		}
		/* Else fall through to one of the other strategies... */
	case ASB_DISCARD_OLDER_PRI:
		if (self == 0 && peer == 1) {
			rv = 1;
			break;
		}
		if (self == 1 && peer == 0) {
			rv = -1;
			break;
		}
		/* Else fall through to one of the other strategies... */
		drbd_warn(peer_device, "Discard younger/older primary did not find a decision\n"
			  "Using discard-least-changes instead\n");
	case ASB_DISCARD_ZERO_CHG:
		if (ch_peer == 0 && ch_self == 0) {
			rv = test_bit(RESOLVE_CONFLICTS, &peer_device->connection->flags)
				? -1 : 1;
			break;
		} else {
			if (ch_peer == 0) { rv =  1; break; }
			if (ch_self == 0) { rv = -1; break; }
		}
		if (after_sb_0p == ASB_DISCARD_ZERO_CHG)
			break;
	case ASB_DISCARD_LEAST_CHG:
		if	(ch_self < ch_peer)
			rv = -1;
		else if (ch_self > ch_peer)
			rv =  1;
		else /* ( ch_self == ch_peer ) */
		     /* Well, then use something else. */
			rv = test_bit(RESOLVE_CONFLICTS, &peer_device->connection->flags)
				? -1 : 1;
		break;
	case ASB_DISCARD_LOCAL:
		rv = -1;
		break;
	case ASB_DISCARD_REMOTE:
		rv =  1;
	}

	return rv;
}

/**
 * drbd_asb_recover_1p  -  Recover after split-brain with one remaining primary
 */
static int drbd_asb_recover_1p(struct drbd_peer_device *peer_device) __must_hold(local)
{
	struct drbd_device *device = peer_device->device;
	struct drbd_connection *connection = peer_device->connection;
	struct drbd_resource *resource = device->resource;
	int hg, rv = -100;
	enum drbd_after_sb_p after_sb_1p;

	rcu_read_lock();
	after_sb_1p = rcu_dereference(connection->net_conf)->after_sb_1p;
	rcu_read_unlock();
	switch (after_sb_1p) {
	case ASB_DISCARD_YOUNGER_PRI:
	case ASB_DISCARD_OLDER_PRI:
	case ASB_DISCARD_LEAST_CHG:
	case ASB_DISCARD_LOCAL:
	case ASB_DISCARD_REMOTE:
	case ASB_DISCARD_ZERO_CHG:
		drbd_err(device, "Configuration error.\n");
		break;
	case ASB_DISCONNECT:
		break;
	case ASB_CONSENSUS:
		hg = drbd_asb_recover_0p(peer_device);
		if (hg == -1 && resource->role[NOW] == R_SECONDARY)
			rv = hg;
		if (hg == 1  && resource->role[NOW] == R_PRIMARY)
			rv = hg;
		break;
	case ASB_VIOLENTLY:
		rv = drbd_asb_recover_0p(peer_device);
		break;
	case ASB_DISCARD_SECONDARY:
		return resource->role[NOW] == R_PRIMARY ? 1 : -1;
	case ASB_CALL_HELPER:
		hg = drbd_asb_recover_0p(peer_device);
		if (hg == -1 && resource->role[NOW] == R_PRIMARY) {
			enum drbd_state_rv rv2;

			 /* drbd_change_state() does not sleep while in SS_IN_TRANSIENT_STATE,
			  * we might be here in L_OFF which is transient.
			  * we do not need to wait for the after state change work either. */
			rv2 = change_role(resource, R_SECONDARY, CS_VERBOSE, false);
			if (rv2 != SS_SUCCESS) {
				drbd_khelper(device, connection, "pri-lost-after-sb");
			} else {
				drbd_warn(device, "Successfully gave up primary role.\n");
				rv = hg;
			}
		} else
			rv = hg;
	}

	return rv;
}

/**
 * drbd_asb_recover_2p  -  Recover after split-brain with two remaining primaries
 */
static int drbd_asb_recover_2p(struct drbd_peer_device *peer_device) __must_hold(local)
{
	struct drbd_device *device = peer_device->device;
	struct drbd_connection *connection = peer_device->connection;
	int hg, rv = -100;
	enum drbd_after_sb_p after_sb_2p;

	rcu_read_lock();
	after_sb_2p = rcu_dereference(connection->net_conf)->after_sb_2p;
	rcu_read_unlock();
	switch (after_sb_2p) {
	case ASB_DISCARD_YOUNGER_PRI:
	case ASB_DISCARD_OLDER_PRI:
	case ASB_DISCARD_LEAST_CHG:
	case ASB_DISCARD_LOCAL:
	case ASB_DISCARD_REMOTE:
	case ASB_CONSENSUS:
	case ASB_DISCARD_SECONDARY:
	case ASB_DISCARD_ZERO_CHG:
		drbd_err(device, "Configuration error.\n");
		break;
	case ASB_VIOLENTLY:
		rv = drbd_asb_recover_0p(peer_device);
		break;
	case ASB_DISCONNECT:
		break;
	case ASB_CALL_HELPER:
		hg = drbd_asb_recover_0p(peer_device);
		if (hg == -1) {
			enum drbd_state_rv rv2;

			 /* drbd_change_state() does not sleep while in SS_IN_TRANSIENT_STATE,
			  * we might be here in L_OFF which is transient.
			  * we do not need to wait for the after state change work either. */
			rv2 = change_role(device->resource, R_SECONDARY, CS_VERBOSE, false);
			if (rv2 != SS_SUCCESS) {
				drbd_khelper(device, connection, "pri-lost-after-sb");
			} else {
				drbd_warn(device, "Successfully gave up primary role.\n");
				rv = hg;
			}
		} else
			rv = hg;
	}

	return rv;
}

static void drbd_uuid_dump_self(struct drbd_peer_device *peer_device, u64 bits, u64 flags)
{
	struct drbd_device *device = peer_device->device;

	drbd_info(peer_device, "self %016llX:%016llX:%016llX:%016llX bits:%llu flags:%llX\n",
		  (unsigned long long)drbd_current_uuid(peer_device->device),
		  (unsigned long long)drbd_bitmap_uuid(peer_device),
		  (unsigned long long)drbd_history_uuid(device, 0),
		  (unsigned long long)drbd_history_uuid(device, 1),
		  (unsigned long long)bits,
		  (unsigned long long)flags);
}


static void drbd_uuid_dump_peer(struct drbd_peer_device *peer_device, u64 bits, u64 flags)
{
	const int node_id = peer_device->device->resource->res_opts.node_id;

	drbd_info(peer_device, "peer %016llX:%016llX:%016llX:%016llX bits:%llu flags:%llX\n",
	     (unsigned long long)peer_device->current_uuid,
	     (unsigned long long)peer_device->bitmap_uuids[node_id],
	     (unsigned long long)peer_device->history_uuids[0],
	     (unsigned long long)peer_device->history_uuids[1],
	     (unsigned long long)bits,
	     (unsigned long long)flags);
}

static int uuid_fixup_resync_end(struct drbd_peer_device *peer_device, int *rule_nr) __must_hold(local)
{
	struct drbd_device *device = peer_device->device;
	const int node_id = device->resource->res_opts.node_id;

	if (peer_device->bitmap_uuids[node_id] == (u64)0 && drbd_bitmap_uuid(peer_device) != (u64)0) {

		if (peer_device->connection->agreed_pro_version < 91)
			return -1091;

		if ((drbd_bitmap_uuid(peer_device) & ~((u64)1)) == (peer_device->history_uuids[0] & ~((u64)1)) &&
		    (drbd_history_uuid(device, 0) & ~((u64)1)) == (peer_device->history_uuids[0] & ~((u64)1))) {
			struct drbd_peer_md *peer_md = &device->ldev->md.peers[peer_device->bitmap_index];

			drbd_info(device, "was SyncSource, missed the resync finished event, corrected myself:\n");
			_drbd_uuid_push_history(peer_device, peer_md->bitmap_uuid);
			peer_md->bitmap_uuid = 0;

			drbd_uuid_dump_self(peer_device,
					    device->disk_state[NOW] >= D_NEGOTIATING ? drbd_bm_total_weight(peer_device) : 0, 0);
			*rule_nr = 34;
		} else {
			drbd_info(device, "was SyncSource (peer failed to write sync_uuid)\n");
			*rule_nr = 36;
		}

		return 1;
	}

	if (drbd_bitmap_uuid(peer_device) == (u64)0 && peer_device->bitmap_uuids[node_id] != (u64)0) {

		if (peer_device->connection->agreed_pro_version < 91)
			return -1091;

		if ((drbd_history_uuid(device, 0) & ~((u64)1)) == (peer_device->bitmap_uuids[node_id] & ~((u64)1)) &&
		    (drbd_history_uuid(device, 1) & ~((u64)1)) == (peer_device->history_uuids[0] & ~((u64)1))) {
			int i;

			drbd_info(device, "was SyncTarget, peer missed the resync finished event, corrected peer:\n");

			for (i = ARRAY_SIZE(peer_device->history_uuids) - 1; i > 0; i--)
				peer_device->history_uuids[i] = peer_device->history_uuids[i - 1];
			peer_device->history_uuids[i] = peer_device->bitmap_uuids[node_id];
			peer_device->bitmap_uuids[node_id] = 0;

			drbd_uuid_dump_peer(peer_device, peer_device->dirty_bits, peer_device->uuid_flags);
			*rule_nr = 35;
		} else {
			drbd_info(device, "was SyncTarget (failed to write sync_uuid)\n");
			*rule_nr = 37;
		}

		return -1;
	}

	return -2000;
}

static int uuid_fixup_resync_start1(struct drbd_peer_device *peer_device, int *rule_nr) __must_hold(local)
{
	struct drbd_device *device = peer_device->device;
	const int node_id = peer_device->device->resource->res_opts.node_id;
	u64 self, peer;

	self = drbd_current_uuid(device) & ~((u64)1);
	peer = peer_device->history_uuids[0] & ~((u64)1);

	if (self == peer) {
		if (peer_device->connection->agreed_pro_version < 96 ?
		    (drbd_history_uuid(device, 0) & ~((u64)1)) ==
		    (peer_device->history_uuids[1] & ~((u64)1)) :
		    peer + UUID_NEW_BM_OFFSET == (peer_device->bitmap_uuids[node_id] & ~((u64)1))) {
			int i;

			/* The last P_SYNC_UUID did not get though. Undo the last start of
			   resync as sync source modifications of the peer's UUIDs. */
			*rule_nr = 51;

			if (peer_device->connection->agreed_pro_version < 91)
				return -1091;

			peer_device->bitmap_uuids[node_id] = peer_device->history_uuids[0];
			for (i = 0; i < ARRAY_SIZE(peer_device->history_uuids) - 1; i++)
				peer_device->history_uuids[i] = peer_device->history_uuids[i + 1];
			peer_device->history_uuids[i] = 0;

			drbd_info(device, "Lost last syncUUID packet, corrected:\n");
			drbd_uuid_dump_peer(peer_device, peer_device->dirty_bits, peer_device->uuid_flags);

			return -1;
		}
	}

	return -2000;
}

static int uuid_fixup_resync_start2(struct drbd_peer_device *peer_device, int *rule_nr) __must_hold(local)
{
	struct drbd_device *device = peer_device->device;
	u64 self, peer;

	self = drbd_history_uuid(device, 0) & ~((u64)1);
	peer = peer_device->current_uuid & ~((u64)1);

	if (self == peer) {
		if (peer_device->connection->agreed_pro_version < 96 ?
		    (drbd_history_uuid(device, 1) & ~((u64)1)) ==
		    (peer_device->history_uuids[0] & ~((u64)1)) :
		    self + UUID_NEW_BM_OFFSET == (drbd_bitmap_uuid(peer_device) & ~((u64)1))) {
			u64 bitmap_uuid;

			/* The last P_SYNC_UUID did not get though. Undo the last start of
			   resync as sync source modifications of our UUIDs. */
			*rule_nr = 71;

			if (peer_device->connection->agreed_pro_version < 91)
				return -1091;

			bitmap_uuid = _drbd_uuid_pull_history(peer_device);
			__drbd_uuid_set_bitmap(peer_device, bitmap_uuid);

			drbd_info(device, "Last syncUUID did not get through, corrected:\n");
			drbd_uuid_dump_self(peer_device,
					    device->disk_state[NOW] >= D_NEGOTIATING ? drbd_bm_total_weight(peer_device) : 0, 0);

			return 1;
		}
	}

	return -2000;
}

/*
  100	after split brain try auto recover
    3   L_SYNC_SOURCE copy BitMap from
    2	L_SYNC_SOURCE set BitMap
    1	L_SYNC_SOURCE use BitMap
    0	no Sync
   -1	L_SYNC_TARGET use BitMap
   -2	L_SYNC_TARGET set BitMap
   -3   L_SYNC_TARGET clear BitMap
 -100	after split brain, disconnect
-1000	unrelated data
-1091   requires proto 91
-1096   requires proto 96
 */
static int drbd_uuid_compare(struct drbd_peer_device *peer_device,
			     int *rule_nr, int *peer_node_id) __must_hold(local)
{
	struct drbd_connection *connection = peer_device->connection;
	struct drbd_device *device = peer_device->device;
	const int node_id = device->resource->res_opts.node_id;
	const int max_peers = device->bitmap->bm_max_peers;
	u64 self, peer;
	int i, j;

	self = drbd_current_uuid(device) & ~((u64)1);
	peer = peer_device->current_uuid & ~((u64)1);

	*rule_nr = 10;
	if (self == UUID_JUST_CREATED && peer == UUID_JUST_CREATED)
		return 0;

	*rule_nr = 20;
	if ((self == UUID_JUST_CREATED || self == (u64)0) &&
	     peer != UUID_JUST_CREATED)
		return -2;

	*rule_nr = 30;
	if (self != UUID_JUST_CREATED &&
	    (peer == UUID_JUST_CREATED || peer == (u64)0))
		return 2;

	if (self == peer) {
		int rct, dc; /* roles at crash time */

		if (connection->agreed_pro_version < 110) {
			int rv = uuid_fixup_resync_end(peer_device, rule_nr);
			if (rv > -2000)
				return rv;
		}

		/* Common power [off|failure] */
		rct = (test_bit(CRASHED_PRIMARY, &device->flags) ? 1 : 0) +
			(peer_device->uuid_flags & UUID_FLAG_CRASHED_PRIMARY);
		/* lowest bit is set when we were primary,
		 * next bit (weight 2) is set when peer was primary */
		*rule_nr = 40;

		switch (rct) {
		case 0: /* !self_pri && !peer_pri */ return 0;
		case 1: /*  self_pri && !peer_pri */ return 1;
		case 2: /* !self_pri &&  peer_pri */ return -1;
		case 3: /*  self_pri &&  peer_pri */
			dc = test_bit(RESOLVE_CONFLICTS, &peer_device->connection->flags);
			return dc ? -1 : 1;
		}
	}

	*rule_nr = 50;
	peer = peer_device->bitmap_uuids[node_id] & ~((u64)1);
	if (self == peer)
		return -1;

	*rule_nr = 52;
	for (i = 0; i < MAX_PEERS; i++) {
		peer = peer_device->bitmap_uuids[i] & ~((u64)1);
		if (self == peer) {
			*peer_node_id = i;
			return -3;
		}
	}

	if (connection->agreed_pro_version < 110) {
		int rv = uuid_fixup_resync_start1(peer_device, rule_nr);
		if (rv > -2000)
			return rv;
	}

	*rule_nr = 60;
	self = drbd_current_uuid(device) & ~((u64)1);
	for (i = 0; i < ARRAY_SIZE(peer_device->history_uuids); i++) {
		peer = peer_device->history_uuids[i] & ~((u64)1);
		if (self == peer)
			return -2;
	}

	*rule_nr = 70;
	self = drbd_bitmap_uuid(peer_device) & ~((u64)1);
	peer = peer_device->current_uuid & ~((u64)1);
	if (self == peer)
		return 1;

	*rule_nr = 72;
	for (i = 0; i < max_peers; i++) {
		if (i == peer_device->bitmap_index)
			continue;
		self = device->ldev->md.peers[i].bitmap_uuid & ~((u64)1);
		if (self == peer) {
			*peer_node_id = device->ldev->md.peers[i].node_id;
			return 3;
		}
	}

	if (connection->agreed_pro_version < 110) {
		int rv = uuid_fixup_resync_start2(peer_device, rule_nr);
		if (rv > -2000)
			return rv;
	}

	*rule_nr = 80;
	peer = peer_device->current_uuid & ~((u64)1);
	for (i = 0; i < HISTORY_UUIDS; i++) {
		self = drbd_history_uuid(device, i) & ~((u64)1);
		if (self == peer)
			return 2;
	}

	*rule_nr = 90;
	self = drbd_bitmap_uuid(peer_device) & ~((u64)1);
	peer = peer_device->bitmap_uuids[node_id] & ~((u64)1);
	if (self == peer && self != ((u64)0))
		return 100;

	*rule_nr = 100;
	for (i = 0; i < HISTORY_UUIDS; i++) {
		self = drbd_history_uuid(device, i) & ~((u64)1);
		for (j = 0; j < ARRAY_SIZE(peer_device->history_uuids); j++) {
			peer = peer_device->history_uuids[j] & ~((u64)1);
			if (self == peer)
				return -100;
		}
	}

	return -1000;
}

/* drbd_sync_handshake() returns the new replication state on success, and -1
 * on failure.
 */
static enum drbd_repl_state drbd_sync_handshake(struct drbd_peer_device *peer_device,
						enum drbd_role peer_role,
						enum drbd_disk_state peer_disk_state) __must_hold(local)
{
	struct drbd_device *device = peer_device->device;
	struct drbd_connection *connection = peer_device->connection;
	enum drbd_repl_state rv = -1;
	enum drbd_disk_state disk_state;
	struct net_conf *nc;
	int hg, rule_nr, rr_conflict, tentative, peer_node_id = 0;

	disk_state = device->disk_state[NOW];
	if (disk_state == D_NEGOTIATING)
		disk_state = disk_state_from_md(device);

	drbd_info(device, "drbd_sync_handshake:\n");
	spin_lock_irq(&device->ldev->md.uuid_lock);
	drbd_uuid_dump_self(peer_device, peer_device->comm_bm_set, 0);
	drbd_uuid_dump_peer(peer_device, peer_device->dirty_bits, peer_device->uuid_flags);

	hg = drbd_uuid_compare(peer_device, &rule_nr, &peer_node_id);
	spin_unlock_irq(&device->ldev->md.uuid_lock);

	drbd_info(device, "uuid_compare()=%d by rule %d\n", hg, rule_nr);

	if (hg == -1000) {
		drbd_alert(device, "Unrelated data, aborting!\n");
		return -1;
	}
	if (hg < -1000) {
		drbd_alert(device, "To resolve this both sides have to support at least protocol %d\n", -hg - 1000);
		return -1;
	}

	if ((disk_state == D_INCONSISTENT && peer_disk_state > D_INCONSISTENT) ||
	    (peer_disk_state == D_INCONSISTENT && disk_state > D_INCONSISTENT)) {
		int f = (hg == -100) || abs(hg) == 2;
		hg = disk_state > D_INCONSISTENT ? 1 : -1;
		if (f)
			hg = hg*2;
		drbd_info(device, "Becoming sync %s due to disk states.\n",
		     hg > 0 ? "source" : "target");
	}

	if (abs(hg) == 100)
		drbd_khelper(device, connection, "initial-split-brain");

	rcu_read_lock();
	nc = rcu_dereference(peer_device->connection->net_conf);

	if (hg == 100 || (hg == -100 && nc->always_asbp)) {
		int pcount = (device->resource->role[NOW] == R_PRIMARY)
			   + (peer_role == R_PRIMARY);
		int forced = (hg == -100);

		switch (pcount) {
		case 0:
			hg = drbd_asb_recover_0p(peer_device);
			break;
		case 1:
			hg = drbd_asb_recover_1p(peer_device);
			break;
		case 2:
			hg = drbd_asb_recover_2p(peer_device);
			break;
		}
		if (abs(hg) < 100) {
			drbd_warn(device, "Split-Brain detected, %d primaries, "
			     "automatically solved. Sync from %s node\n",
			     pcount, (hg < 0) ? "peer" : "this");
			if (forced) {
				drbd_warn(device, "Doing a full sync, since"
				     " UUIDs where ambiguous.\n");
				hg = hg*2;
			}
		}
	}

	if (hg == -100) {
		if (test_bit(DISCARD_MY_DATA, &device->flags) &&
		    !(peer_device->uuid_flags & UUID_FLAG_DISCARD_MY_DATA))
			hg = -1;
		if (!test_bit(DISCARD_MY_DATA, &device->flags) &&
		    (peer_device->uuid_flags & UUID_FLAG_DISCARD_MY_DATA))
			hg = 1;

		if (abs(hg) < 100)
			drbd_warn(device, "Split-Brain detected, manually solved. "
			     "Sync from %s node\n",
			     (hg < 0) ? "peer" : "this");
	}
	rr_conflict = nc->rr_conflict;
	tentative = nc->tentative;
	rcu_read_unlock();

	if (hg == -100) {
		/* FIXME this log message is not correct if we end up here
		 * after an attempted attach on a diskless node.
		 * We just refuse to attach -- well, we drop the "connection"
		 * to that disk, in a way... */
		drbd_alert(device, "Split-Brain detected but unresolved, dropping connection!\n");
		drbd_khelper(device, connection, "split-brain");
		return -1;
	}

	if (hg > 0 && disk_state <= D_INCONSISTENT) {
		drbd_err(device, "I shall become SyncSource, but I am inconsistent!\n");
		return -1;
	}

	if (hg < 0 && /* by intention we do not use disk_state here. */
	    device->resource->role[NOW] == R_PRIMARY && device->disk_state[NOW] >= D_CONSISTENT) {
		switch (rr_conflict) {
		case ASB_CALL_HELPER:
			drbd_khelper(device, connection, "pri-lost");
			/* fall through */
		case ASB_DISCONNECT:
			drbd_err(device, "I shall become SyncTarget, but I am primary!\n");
			return -1;
		case ASB_VIOLENTLY:
			drbd_warn(device, "Becoming SyncTarget, violating the stable-data"
			     "assumption\n");
		}
	}

	if (tentative || test_bit(CONN_DRY_RUN, &peer_device->connection->flags)) {
		if (hg == 0)
			drbd_info(device, "dry-run connect: No resync, would become Connected immediately.\n");
		else
			drbd_info(device, "dry-run connect: Would become %s, doing a %s resync.",
				 drbd_repl_str(hg > 0 ? L_SYNC_SOURCE : L_SYNC_TARGET),
				 abs(hg) >= 2 ? "full" : "bit-map based");
		return -1;
	}

	if (hg == 3) {
		drbd_info(device, "Peer synced up with node %d, copying bitmap\n", peer_node_id);
		drbd_suspend_io(device);
		drbd_bm_slot_lock(peer_device, "bm_copy_slot from sync_handshake", BM_LOCK_BULK);
		drbd_bm_copy_slot(device, device->ldev->id_to_bit[peer_node_id], peer_device->bitmap_index);
		drbd_bm_write(device, NULL);
		drbd_bm_slot_unlock(peer_device);
		drbd_resume_io(device);
	} else if (hg == -3) {
		drbd_info(device, "synced up with node %d in the mean time\n", peer_node_id);
		drbd_suspend_io(device);
		drbd_bm_slot_lock(peer_device, "bm_clear_many_bits from sync_handshake", BM_LOCK_BULK);
		drbd_bm_clear_many_bits(peer_device, 0, -1UL);
		drbd_bm_write(device, NULL);
		drbd_bm_slot_unlock(peer_device);
		drbd_resume_io(device);
	} else if (abs(hg) >= 2) {
		drbd_info(device, "Writing the whole bitmap, full sync required after drbd_sync_handshake.\n");
		if (drbd_bitmap_io(device, &drbd_bmio_set_n_write, "set_n_write from sync_handshake",
					BM_LOCK_CLEAR | BM_LOCK_BULK, peer_device))
			return -1;
	}

	if (hg > 0) { /* become sync source. */
		rv = L_WF_BITMAP_S;
	} else if (hg < 0) { /* become sync target */
		rv = L_WF_BITMAP_T;
	} else {
		rv = L_ESTABLISHED;
		if (drbd_bitmap_uuid(peer_device)) {
			drbd_info(peer_device, "clearing bitmap UUID and bitmap content (%lu bits)\n",
				  drbd_bm_total_weight(peer_device));
			drbd_uuid_set_bitmap(peer_device, 0);
			drbd_bm_clear_many_bits(peer_device, 0, -1UL);
		} else if (drbd_bm_total_weight(peer_device)) {
			drbd_info(device, "No resync, but %lu bits in bitmap!\n",
				  drbd_bm_total_weight(peer_device));
		}
	}

	return rv;
}

static enum drbd_after_sb_p convert_after_sb(enum drbd_after_sb_p peer)
{
	/* ASB_DISCARD_REMOTE - ASB_DISCARD_LOCAL is valid */
	if (peer == ASB_DISCARD_REMOTE)
		return ASB_DISCARD_LOCAL;

	/* any other things with ASB_DISCARD_REMOTE or ASB_DISCARD_LOCAL are invalid */
	if (peer == ASB_DISCARD_LOCAL)
		return ASB_DISCARD_REMOTE;

	/* everything else is valid if they are equal on both sides. */
	return peer;
}

static int receive_protocol(struct drbd_connection *connection, struct packet_info *pi)
{
	struct p_protocol *p = pi->data;
	enum drbd_after_sb_p p_after_sb_0p, p_after_sb_1p, p_after_sb_2p;
	int p_proto, p_discard_my_data, p_two_primaries, cf;
	struct net_conf *nc, *old_net_conf, *new_net_conf = NULL;
	char integrity_alg[SHARED_SECRET_MAX] = "";
	struct crypto_hash *peer_integrity_tfm = NULL;
	void *int_dig_in = NULL, *int_dig_vv = NULL;

	p_proto		= be32_to_cpu(p->protocol);
	p_after_sb_0p	= be32_to_cpu(p->after_sb_0p);
	p_after_sb_1p	= be32_to_cpu(p->after_sb_1p);
	p_after_sb_2p	= be32_to_cpu(p->after_sb_2p);
	p_two_primaries = be32_to_cpu(p->two_primaries);
	cf		= be32_to_cpu(p->conn_flags);
	p_discard_my_data = cf & CF_DISCARD_MY_DATA;

	if (connection->agreed_pro_version >= 87) {
		int err;

		if (pi->size > sizeof(integrity_alg))
			return -EIO;
		err = drbd_recv_all(connection, integrity_alg, pi->size);
		if (err)
			return err;
		integrity_alg[SHARED_SECRET_MAX - 1] = 0;
	}

	if (pi->cmd != P_PROTOCOL_UPDATE) {
		clear_bit(CONN_DRY_RUN, &connection->flags);

		if (cf & CF_DRY_RUN)
			set_bit(CONN_DRY_RUN, &connection->flags);

		rcu_read_lock();
		nc = rcu_dereference(connection->net_conf);

		if (p_proto != nc->wire_protocol) {
			drbd_err(connection, "incompatible %s settings\n", "protocol");
			goto disconnect_rcu_unlock;
		}

		if (convert_after_sb(p_after_sb_0p) != nc->after_sb_0p) {
			drbd_err(connection, "incompatible %s settings\n", "after-sb-0pri");
			goto disconnect_rcu_unlock;
		}

		if (convert_after_sb(p_after_sb_1p) != nc->after_sb_1p) {
			drbd_err(connection, "incompatible %s settings\n", "after-sb-1pri");
			goto disconnect_rcu_unlock;
		}

		if (convert_after_sb(p_after_sb_2p) != nc->after_sb_2p) {
			drbd_err(connection, "incompatible %s settings\n", "after-sb-2pri");
			goto disconnect_rcu_unlock;
		}

		if (p_discard_my_data && nc->discard_my_data) {
			drbd_err(connection, "incompatible %s settings\n", "discard-my-data");
			goto disconnect_rcu_unlock;
		}

		if (p_two_primaries != nc->two_primaries) {
			drbd_err(connection, "incompatible %s settings\n", "allow-two-primaries");
			goto disconnect_rcu_unlock;
		}

		if (strcmp(integrity_alg, nc->integrity_alg)) {
			drbd_err(connection, "incompatible %s settings\n", "data-integrity-alg");
			goto disconnect_rcu_unlock;
		}

		rcu_read_unlock();
	}

	if (integrity_alg[0]) {
		int hash_size;

		/*
		 * We can only change the peer data integrity algorithm
		 * here.  Changing our own data integrity algorithm
		 * requires that we send a P_PROTOCOL_UPDATE packet at
		 * the same time; otherwise, the peer has no way to
		 * tell between which packets the algorithm should
		 * change.
		 */

		peer_integrity_tfm = crypto_alloc_hash(integrity_alg, 0, CRYPTO_ALG_ASYNC);
		if (!peer_integrity_tfm) {
			drbd_err(connection, "peer data-integrity-alg %s not supported\n",
				 integrity_alg);
			goto disconnect;
		}

		hash_size = crypto_hash_digestsize(peer_integrity_tfm);
		int_dig_in = kmalloc(hash_size, GFP_KERNEL);
		int_dig_vv = kmalloc(hash_size, GFP_KERNEL);
		if (!(int_dig_in && int_dig_vv)) {
			drbd_err(connection, "Allocation of buffers for data integrity checking failed\n");
			goto disconnect;
		}
	}

	new_net_conf = kmalloc(sizeof(struct net_conf), GFP_KERNEL);
	if (!new_net_conf) {
		drbd_err(connection, "Allocation of new net_conf failed\n");
		goto disconnect;
	}

	if (mutex_lock_interruptible(&connection->resource->conf_update)) {
		drbd_err(connection, "Interrupted while waiting for conf_update\n");
		goto disconnect;
	}

	mutex_lock(&connection->data.mutex);
	old_net_conf = connection->net_conf;
	*new_net_conf = *old_net_conf;

	new_net_conf->wire_protocol = p_proto;
	new_net_conf->after_sb_0p = convert_after_sb(p_after_sb_0p);
	new_net_conf->after_sb_1p = convert_after_sb(p_after_sb_1p);
	new_net_conf->after_sb_2p = convert_after_sb(p_after_sb_2p);
	new_net_conf->two_primaries = p_two_primaries;

	rcu_assign_pointer(connection->net_conf, new_net_conf);
	mutex_unlock(&connection->data.mutex);
	mutex_unlock(&connection->resource->conf_update);

	crypto_free_hash(connection->peer_integrity_tfm);
	kfree(connection->int_dig_in);
	kfree(connection->int_dig_vv);
	connection->peer_integrity_tfm = peer_integrity_tfm;
	connection->int_dig_in = int_dig_in;
	connection->int_dig_vv = int_dig_vv;

	if (strcmp(old_net_conf->integrity_alg, integrity_alg))
		drbd_info(connection, "peer data-integrity-alg: %s\n",
			  integrity_alg[0] ? integrity_alg : "(none)");

	synchronize_rcu();
	kfree(old_net_conf);
	return 0;

disconnect_rcu_unlock:
	rcu_read_unlock();
disconnect:
	crypto_free_hash(peer_integrity_tfm);
	kfree(int_dig_in);
	kfree(int_dig_vv);
	change_cstate(connection, C_DISCONNECTING, CS_HARD);
	return -EIO;
}

/* helper function
 * input: alg name, feature name
 * return: NULL (alg name was "")
 *         ERR_PTR(error) if something goes wrong
 *         or the crypto hash ptr, if it worked out ok. */
struct crypto_hash *drbd_crypto_alloc_digest_safe(const struct drbd_device *device,
		const char *alg, const char *name)
{
	struct crypto_hash *tfm;

	if (!alg[0])
		return NULL;

	tfm = crypto_alloc_hash(alg, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		drbd_err(device, "Can not allocate \"%s\" as %s (reason: %ld)\n",
			alg, name, PTR_ERR(tfm));
		return tfm;
	}
	return tfm;
}

static int ignore_remaining_packet(struct drbd_connection *connection, struct packet_info *pi)
{
	void *buffer = connection->data.rbuf;
	int size = pi->size;

	while (size) {
		int s = min_t(int, size, DRBD_SOCKET_BUFFER_SIZE);
		s = drbd_recv(connection, buffer, s);
		if (s <= 0) {
			if (s < 0)
				return s;
			break;
		}
		size -= s;
	}
	if (size)
		return -EIO;
	return 0;
}

/*
 * config_unknown_volume  -  device configuration command for unknown volume
 *
 * When a device is added to an existing connection, the node on which the
 * device is added first will send configuration commands to its peer but the
 * peer will not know about the device yet.  It will warn and ignore these
 * commands.  Once the device is added on the second node, the second node will
 * send the same device configuration commands, but in the other direction.
 *
 * (We can also end up here if drbd is misconfigured.)
 */
static int config_unknown_volume(struct drbd_connection *connection, struct packet_info *pi)
{
	drbd_warn(connection, "%s packet received for volume %d, which is not configured locally\n",
		  cmdname(pi->cmd), pi->vnr);
	return ignore_remaining_packet(connection, pi);
}

static int receive_SyncParam(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_rs_param_95 *p;
	unsigned int header_size, data_size, exp_max_sz;
	struct crypto_hash *verify_tfm = NULL;
	struct crypto_hash *csums_tfm = NULL;
	struct net_conf *old_net_conf, *new_net_conf = NULL;
	struct disk_conf *old_disk_conf = NULL, *new_disk_conf = NULL;
	const int apv = connection->agreed_pro_version;
	struct fifo_buffer *old_plan = NULL, *new_plan = NULL;
	int fifo_size = 0;
	int err;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return config_unknown_volume(connection, pi);
	device = peer_device->device;

	exp_max_sz  = apv <= 87 ? sizeof(struct p_rs_param)
		    : apv == 88 ? sizeof(struct p_rs_param)
					+ SHARED_SECRET_MAX
		    : apv <= 94 ? sizeof(struct p_rs_param_89)
		    : /* apv >= 95 */ sizeof(struct p_rs_param_95);

	if (pi->size > exp_max_sz) {
		drbd_err(device, "SyncParam packet too long: received %u, expected <= %u bytes\n",
		    pi->size, exp_max_sz);
		return -EIO;
	}

	if (apv <= 88) {
		header_size = sizeof(struct p_rs_param);
		data_size = pi->size - header_size;
	} else if (apv <= 94) {
		header_size = sizeof(struct p_rs_param_89);
		data_size = pi->size - header_size;
		D_ASSERT(device, data_size == 0);
	} else {
		header_size = sizeof(struct p_rs_param_95);
		data_size = pi->size - header_size;
		D_ASSERT(device, data_size == 0);
	}

	/* initialize verify_alg and csums_alg */
	p = pi->data;
	memset(p->verify_alg, 0, 2 * SHARED_SECRET_MAX);

	err = drbd_recv_all(peer_device->connection, p, header_size);
	if (err)
		return err;

	err = mutex_lock_interruptible(&connection->resource->conf_update);
	if (err) {
		drbd_err(connection, "Interrupted while waiting for conf_update\n");
		return err;
	}
	old_net_conf = peer_device->connection->net_conf;
	if (get_ldev(device)) {
		new_disk_conf = kzalloc(sizeof(struct disk_conf), GFP_KERNEL);
		if (!new_disk_conf) {
			put_ldev(device);
			mutex_unlock(&connection->resource->conf_update);
			drbd_err(device, "Allocation of new disk_conf failed\n");
			return -ENOMEM;
		}

		old_disk_conf = device->ldev->disk_conf;
		*new_disk_conf = *old_disk_conf;

		new_disk_conf->resync_rate = be32_to_cpu(p->resync_rate);
	}

	if (apv >= 88) {
		if (apv == 88) {
			if (data_size > SHARED_SECRET_MAX || data_size == 0) {
				drbd_err(device, "verify-alg too long, "
					 "peer wants %u, accepting only %u byte\n",
					 data_size, SHARED_SECRET_MAX);
				err = -EIO;
				goto reconnect;
			}

			err = drbd_recv_all(peer_device->connection, p->verify_alg, data_size);
			if (err)
				goto reconnect;
			/* we expect NUL terminated string */
			/* but just in case someone tries to be evil */
			D_ASSERT(device, p->verify_alg[data_size-1] == 0);
			p->verify_alg[data_size-1] = 0;

		} else /* apv >= 89 */ {
			/* we still expect NUL terminated strings */
			/* but just in case someone tries to be evil */
			D_ASSERT(device, p->verify_alg[SHARED_SECRET_MAX-1] == 0);
			D_ASSERT(device, p->csums_alg[SHARED_SECRET_MAX-1] == 0);
			p->verify_alg[SHARED_SECRET_MAX-1] = 0;
			p->csums_alg[SHARED_SECRET_MAX-1] = 0;
		}

		if (strcmp(old_net_conf->verify_alg, p->verify_alg)) {
			if (peer_device->repl_state[NOW] == L_OFF) {
				drbd_err(device, "Different verify-alg settings. me=\"%s\" peer=\"%s\"\n",
				    old_net_conf->verify_alg, p->verify_alg);
				goto disconnect;
			}
			verify_tfm = drbd_crypto_alloc_digest_safe(device,
					p->verify_alg, "verify-alg");
			if (IS_ERR(verify_tfm)) {
				verify_tfm = NULL;
				goto disconnect;
			}
		}

		if (apv >= 89 && strcmp(old_net_conf->csums_alg, p->csums_alg)) {
			if (peer_device->repl_state[NOW] == L_OFF) {
				drbd_err(device, "Different csums-alg settings. me=\"%s\" peer=\"%s\"\n",
				    old_net_conf->csums_alg, p->csums_alg);
				goto disconnect;
			}
			csums_tfm = drbd_crypto_alloc_digest_safe(device,
					p->csums_alg, "csums-alg");
			if (IS_ERR(csums_tfm)) {
				csums_tfm = NULL;
				goto disconnect;
			}
		}

		if (apv > 94 && new_disk_conf) {
			new_disk_conf->c_plan_ahead = be32_to_cpu(p->c_plan_ahead);
			new_disk_conf->c_delay_target = be32_to_cpu(p->c_delay_target);
			new_disk_conf->c_fill_target = be32_to_cpu(p->c_fill_target);
			new_disk_conf->c_max_rate = be32_to_cpu(p->c_max_rate);

			fifo_size = (new_disk_conf->c_plan_ahead * 10 * SLEEP_TIME) / HZ;
			old_plan = rcu_dereference(peer_device->rs_plan_s);
			if (!old_plan || fifo_size != old_plan->size) {
				new_plan = fifo_alloc(fifo_size);
				if (!new_plan) {
					drbd_err(device, "kmalloc of fifo_buffer failed");
					goto disconnect;
				}
			}
		}

		if (verify_tfm || csums_tfm) {
			new_net_conf = kzalloc(sizeof(struct net_conf), GFP_KERNEL);
			if (!new_net_conf) {
				drbd_err(device, "Allocation of new net_conf failed\n");
				goto disconnect;
			}

			*new_net_conf = *old_net_conf;

			if (verify_tfm) {
				strcpy(new_net_conf->verify_alg, p->verify_alg);
				new_net_conf->verify_alg_len = strlen(p->verify_alg) + 1;
				crypto_free_hash(peer_device->connection->verify_tfm);
				peer_device->connection->verify_tfm = verify_tfm;
				drbd_info(device, "using verify-alg: \"%s\"\n", p->verify_alg);
			}
			if (csums_tfm) {
				strcpy(new_net_conf->csums_alg, p->csums_alg);
				new_net_conf->csums_alg_len = strlen(p->csums_alg) + 1;
				crypto_free_hash(peer_device->connection->csums_tfm);
				peer_device->connection->csums_tfm = csums_tfm;
				drbd_info(device, "using csums-alg: \"%s\"\n", p->csums_alg);
			}
			rcu_assign_pointer(connection->net_conf, new_net_conf);
		}
	}

	if (new_disk_conf) {
		rcu_assign_pointer(device->ldev->disk_conf, new_disk_conf);
		put_ldev(device);
	}

	if (new_plan)
		rcu_assign_pointer(peer_device->rs_plan_s, new_plan);

	mutex_unlock(&connection->resource->conf_update);
	synchronize_rcu();
	if (new_net_conf)
		kfree(old_net_conf);
	kfree(old_disk_conf);
	if (new_plan)
		kfree(old_plan);

	return 0;

reconnect:
	if (new_disk_conf) {
		put_ldev(device);
		kfree(new_disk_conf);
	}
	mutex_unlock(&connection->resource->conf_update);
	return -EIO;

disconnect:
	kfree(new_plan);
	if (new_disk_conf) {
		put_ldev(device);
		kfree(new_disk_conf);
	}
	mutex_unlock(&connection->resource->conf_update);
	/* just for completeness: actually not needed,
	 * as this is not reached if csums_tfm was ok. */
	crypto_free_hash(csums_tfm);
	/* but free the verify_tfm again, if csums_tfm did not work out */
	crypto_free_hash(verify_tfm);
	change_cstate(peer_device->connection, C_DISCONNECTING, CS_HARD);
	return -EIO;
}

static void drbd_setup_order_type(struct drbd_device *device, int peer)
{
	/* sorry, we currently have no working implementation
	 * of distributed TCQ */
}

/* warn if the arguments differ by more than 12.5% */
static void warn_if_differ_considerably(struct drbd_device *device,
	const char *s, sector_t a, sector_t b)
{
	sector_t d;
	if (a == 0 || b == 0)
		return;
	d = (a > b) ? (a - b) : (b - a);
	if (d > (a>>3) || d > (b>>3))
		drbd_warn(device, "Considerable difference in %s: %llus vs. %llus\n", s,
		     (unsigned long long)a, (unsigned long long)b);
}

/* Maximum bio size that a protocol version supports. */
static unsigned int conn_max_bio_size(struct drbd_connection *connection)
{
	if (connection->agreed_pro_version >= 100)
		return DRBD_MAX_BIO_SIZE;
	else if (connection->agreed_pro_version >= 95)
		return DRBD_MAX_BIO_SIZE_P95;
	else
		return DRBD_MAX_SIZE_H80_PACKET;
}

static int receive_sizes(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_sizes *p = pi->data;
	enum determine_dev_size dd = DS_UNCHANGED;
	int ldsc = 0; /* local disk size changed */
	enum dds_flags ddsf;
	unsigned int protocol_max_bio_size;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return config_unknown_volume(connection, pi);
	device = peer_device->device;

	/* just store the peer's disk size for now.
	 * we still need to figure out whether we accept that. */
	peer_device->max_size = be64_to_cpu(p->d_size);

	if (get_ldev(device)) {
		sector_t p_usize = be64_to_cpu(p->u_size), my_usize;

		rcu_read_lock();
		my_usize = rcu_dereference(device->ldev->disk_conf)->disk_size;
		rcu_read_unlock();

		warn_if_differ_considerably(device, "lower level device sizes",
			   peer_device->max_size, drbd_get_max_capacity(device->ldev));
		warn_if_differ_considerably(device, "user requested size",
					    p_usize, my_usize);

		/* if this is the first connect, or an otherwise expected
		 * param exchange, choose the minimum */
		if (peer_device->repl_state[NOW] == L_OFF)
			p_usize = min_not_zero(my_usize, p_usize);

		/* Never shrink a device with usable data during connect.
		   But allow online shrinking if we are connected. */
		if (drbd_new_dev_size(device, p_usize, 0) <
		    drbd_get_capacity(device->this_bdev) &&
		    device->disk_state[NOW] >= D_OUTDATED &&
		    peer_device->repl_state[NOW] < L_ESTABLISHED) {
			drbd_err(device, "The peer's disk size is too small!\n");
			change_cstate(peer_device->connection, C_DISCONNECTING, CS_HARD);
			put_ldev(device);
			return -EIO;
		}

		if (my_usize != p_usize) {
			struct disk_conf *old_disk_conf, *new_disk_conf;
			int err;

			new_disk_conf = kzalloc(sizeof(struct disk_conf), GFP_KERNEL);
			if (!new_disk_conf) {
				drbd_err(device, "Allocation of new disk_conf failed\n");
				put_ldev(device);
				return -ENOMEM;
			}

			err = mutex_lock_interruptible(&connection->resource->conf_update);
			if (err) {
				drbd_err(connection, "Interrupted while waiting for conf_update\n");
				return err;
			}
			old_disk_conf = device->ldev->disk_conf;
			*new_disk_conf = *old_disk_conf;
			new_disk_conf->disk_size = p_usize;

			rcu_assign_pointer(device->ldev->disk_conf, new_disk_conf);
			mutex_unlock(&connection->resource->conf_update);
			synchronize_rcu();
			kfree(old_disk_conf);

			drbd_info(device, "Peer sets u_size to %lu sectors\n",
				 (unsigned long)my_usize);
		}

		put_ldev(device);
	}

	peer_device->max_bio_size = be32_to_cpu(p->max_bio_size);
	drbd_reconsider_max_bio_size(device);
	/* Leave drbd_reconsider_max_bio_size() before drbd_determine_dev_size().
	   In case we cleared the QUEUE_FLAG_DISCARD from our queue in
	   drbd_reconsider_max_bio_size(), we can be sure that after
	   drbd_determine_dev_size() no REQ_DISCARDs are in the queue. */

	ddsf = be16_to_cpu(p->dds_flags);
	dd = drbd_determine_dev_size(device, ddsf, NULL);
	if (dd == DS_ERROR)
		return -EIO;
	drbd_md_sync(device);

	/* The protocol version limits how big requests can be.  In addition,
	 * peers before protocol version 94 cannot split large requests into
	 * multiple bios; their reported max_bio_size is a hard limit.
	 */
	protocol_max_bio_size = conn_max_bio_size(connection);
	peer_device->max_bio_size = min(be32_to_cpu(p->max_bio_size), protocol_max_bio_size);
	if (device->device_conf.max_bio_size > protocol_max_bio_size ||
	    (connection->agreed_pro_version < 94 &&
	     device->device_conf.max_bio_size > peer_device->max_bio_size)) {
		drbd_err(device, "Peer cannot deal with requests bigger than %u. "
			 "Please reduce max_bio_size in the configuration.\n",
			 peer_device->max_bio_size);
		change_cstate(peer_device->connection, C_DISCONNECTING, CS_HARD);
		put_ldev(device);
		return -EIO;
	}

	if (get_ldev(device)) {
		if (device->ldev->known_size != drbd_get_capacity(device->ldev->backing_bdev)) {
			device->ldev->known_size = drbd_get_capacity(device->ldev->backing_bdev);
			ldsc = 1;
		}

		drbd_setup_order_type(device, be16_to_cpu(p->queue_order_type));
		put_ldev(device);
	}

	if (peer_device->repl_state[NOW] > L_OFF) {
		if (be64_to_cpu(p->c_size) !=
		    drbd_get_capacity(device->this_bdev) || ldsc) {
			/* we have different sizes, probably peer
			 * needs to know my new size... */
			drbd_send_sizes(peer_device, 0, ddsf);
		}
		if (test_and_clear_bit(RESIZE_PENDING, &peer_device->flags) ||
		    (dd == DS_GREW && peer_device->repl_state[NOW] == L_ESTABLISHED)) {
			if (peer_device->disk_state[NOW] >= D_INCONSISTENT &&
			    device->disk_state[NOW] >= D_INCONSISTENT) {
				if (ddsf & DDSF_NO_RESYNC)
					drbd_info(device, "Resync of new storage suppressed with --assume-clean\n");
				else
					resync_after_online_grow(peer_device);
			} else
				set_bit(RESYNC_AFTER_NEG, &peer_device->flags);
		}
	}

	return 0;
}

static int __receive_uuids(struct drbd_peer_device *peer_device, u64 mask)
{
	struct drbd_device *device = device = peer_device->device;
	int updated_uuids = 0, err = 0;

	if (peer_device->repl_state[NOW] < L_ESTABLISHED &&
	    device->disk_state[NOW] < D_INCONSISTENT &&
	    device->resource->role[NOW] == R_PRIMARY &&
	    (device->exposed_data_uuid & ~((u64)1)) != (peer_device->current_uuid & ~((u64)1))) {
		drbd_err(device, "Can only connect to data with current UUID=%016llX\n",
		    (unsigned long long)device->exposed_data_uuid);
		change_cstate(peer_device->connection, C_DISCONNECTING, CS_HARD);
		return -EIO;
	}

	if (get_ldev(device)) {
		int skip_initial_sync =
			peer_device->repl_state[NOW] == L_ESTABLISHED &&
			peer_device->connection->agreed_pro_version >= 90 &&
			drbd_current_uuid(device) == UUID_JUST_CREATED &&
			(peer_device->uuid_flags & UUID_FLAG_SKIP_INITIAL_SYNC);
		if (skip_initial_sync) {
			unsigned long irq_flags;

			drbd_info(device, "Accepted new current UUID, preparing to skip initial sync\n");
			drbd_bitmap_io(device, &drbd_bmio_clear_n_write,
					"clear_n_write from receive_uuids",
					BM_LOCK_SET | BM_LOCK_CLEAR | BM_LOCK_BULK, NULL);
			_drbd_uuid_set_current(device, peer_device->current_uuid);
			_drbd_uuid_set_bitmap(peer_device, 0);
			begin_state_change(device->resource, &irq_flags, CS_VERBOSE);
			/* FIXME: Note that req_lock was not taken here before! */
			__change_disk_state(device, D_UP_TO_DATE);
			__change_peer_disk_state(peer_device, D_UP_TO_DATE);
			end_state_change(device->resource, &irq_flags);
			drbd_md_sync(device);
			updated_uuids = 1;
		}

		if (peer_device->uuid_flags & UUID_FLAG_NEW_DATAGEN) {
			drbd_warn(peer_device, "received new current UUID: %llX\n", peer_device->current_uuid);
			drbd_uuid_received_new_current(device, peer_device->current_uuid, mask);
		}

		put_ldev(device);
	} else if (device->disk_state[NOW] < D_INCONSISTENT) {
		struct drbd_resource *resource = device->resource;

		spin_lock_irq(&resource->req_lock);
		if (resource->state_change_flags) {
			drbd_info(peer_device, "Delaying update of exposed data uuid\n");
			device->next_exposed_data_uuid = peer_device->current_uuid;
		} else
			updated_uuids = drbd_set_exposed_data_uuid(device, peer_device->current_uuid);
		spin_unlock_irq(&resource->req_lock);

	}

	if (updated_uuids)
		drbd_print_uuids(peer_device, "receiver updated UUIDs to");

	if (!test_bit(INITIAL_STATE_RECEIVED, &peer_device->flags)) {
		if (!test_bit(INITIAL_STATE_SENT, &peer_device->flags)) {
			set_bit(INITIAL_STATE_SENT, &peer_device->flags);
			err = drbd_send_current_state(peer_device);
		}
	}

	return err;
}

static int receive_uuids(struct drbd_connection *connection, struct packet_info *pi)
{
	const int node_id = connection->resource->res_opts.node_id;
	struct drbd_peer_device *peer_device;
	struct p_uuids *p = pi->data;
	int history_uuids, i;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return config_unknown_volume(connection, pi);

	history_uuids = min_t(int, HISTORY_UUIDS_V08,
			      ARRAY_SIZE(peer_device->history_uuids));

	peer_device->current_uuid = be64_to_cpu(p->current_uuid);
	peer_device->bitmap_uuids[node_id] = be64_to_cpu(p->bitmap_uuid);
	for (i = 0; i < history_uuids; i++)
		peer_device->history_uuids[i] = be64_to_cpu(p->history_uuids[i]);
	for (; i < ARRAY_SIZE(peer_device->history_uuids); i++)
		peer_device->history_uuids[i] = 0;
	peer_device->dirty_bits = be64_to_cpu(p->dirty_bits);
	peer_device->uuid_flags = be64_to_cpu(p->uuid_flags);
	peer_device->uuids_received = true;

	return __receive_uuids(peer_device, 0);
}

static int receive_uuids110(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct p_uuids110 *p = pi->data;
	int other_uuids, i, rest, pos = 0;
	u64 bitmap_uuids_mask;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return config_unknown_volume(connection, pi);

	other_uuids = min(pi->size / sizeof(p->other_uuids[0]),
			  ARRAY_SIZE(peer_device->history_uuids) +
			  ARRAY_SIZE(peer_device->bitmap_uuids));

	if (drbd_recv_all_warn(peer_device->connection, p->other_uuids,
			       other_uuids * sizeof(p->other_uuids[0])))
		return -EIO;
	rest = pi->size - other_uuids * sizeof(p->other_uuids[0]);
	if (rest > 0 && !drbd_drain_block(peer_device, rest))
		return -EIO;

	peer_device->current_uuid = be64_to_cpu(p->current_uuid);
	peer_device->dirty_bits = be64_to_cpu(p->dirty_bits);
	peer_device->uuid_flags = be64_to_cpu(p->uuid_flags);
	bitmap_uuids_mask = be64_to_cpu(p->bitmap_uuids_mask);

	for (i = 0; i < MAX_PEERS; i++) {
		if (bitmap_uuids_mask & (1ULL << i))
			peer_device->bitmap_uuids[i] = be64_to_cpu(p->other_uuids[pos++]);
		else
			peer_device->bitmap_uuids[i] = 0;
	}

	i = 0;
	while (pos < other_uuids)
		peer_device->history_uuids[i++] = be64_to_cpu(p->other_uuids[pos++]);

	while (i < HISTORY_UUIDS)
		peer_device->history_uuids[i++] = 0;
	peer_device->uuids_received = true;

	return __receive_uuids(peer_device, be64_to_cpu(p->offline_mask));
}

/**
 * convert_state() - Converts the peer's view of the cluster state to our point of view
 * @peer_state:	The state as seen by the peer.
 */
static union drbd_state convert_state(union drbd_state peer_state)
{
	union drbd_state state;

	static enum drbd_conn_state c_tab[] = {
		[L_OFF] = L_OFF,
		[L_ESTABLISHED] = L_ESTABLISHED,

		[L_STARTING_SYNC_S] = L_STARTING_SYNC_T,
		[L_STARTING_SYNC_T] = L_STARTING_SYNC_S,
		[C_DISCONNECTING] = C_TEAR_DOWN, /* C_NETWORK_FAILURE, */
		[C_CONNECTING] = C_CONNECTING,
		[L_VERIFY_S]       = L_VERIFY_T,
		[C_MASK]   = C_MASK,
	};

	state.i = peer_state.i;

	state.conn = c_tab[peer_state.conn];
	state.peer = peer_state.role;
	state.role = peer_state.peer;
	state.pdsk = peer_state.disk;
	state.disk = peer_state.pdsk;
	state.peer_isp = (peer_state.aftr_isp | peer_state.user_isp);

	return state;
}

static union drbd_state
__change_connection_state(struct drbd_connection *connection,
			  union drbd_state mask, union drbd_state val,
			  enum chg_state_flags flags)
{
	struct drbd_resource *resource = connection->resource;

	if (mask.role) {
		/* not allowed */
	}
	if (mask.susp) {
		mask.susp ^= -1;
		__change_io_susp_user(resource, val.susp);
	}
	if (mask.susp_nod) {
		mask.susp_nod ^= -1;
		__change_io_susp_no_data(resource, val.susp_nod);
	}
	if (mask.susp_fen) {
		mask.susp_fen ^= -1;
		__change_io_susp_fencing(resource, val.susp_fen);
	}
	if (flags & CS_WEAK_NODES) {
		__change_weak(resource,
			resource->twopc_reply.weak_nodes &
			NODE_MASK(resource->res_opts.node_id));
	}

	if (mask.conn) {
		mask.conn ^= -1;
		__change_cstate(connection,
				min_t(enum drbd_conn_state, val.conn, C_CONNECTED));
	}
	if (mask.peer) {
		mask.peer ^= -1;
		__change_peer_role(connection, val.peer);
	}
	return mask;
}

static union drbd_state
__change_peer_device_state(struct drbd_peer_device *peer_device,
			   union drbd_state mask, union drbd_state val)
{
	struct drbd_device *device = peer_device->device;

	if (mask.disk) {
		mask.disk ^= -1;
		__change_disk_state(device, val.disk);
	}

	if (mask.conn) {
		mask.conn ^= -1;
		__change_repl_state(peer_device,
				max_t(enum drbd_repl_state, val.conn, L_OFF));
	}
	if (mask.pdsk) {
		mask.pdsk ^= -1;
		__change_peer_disk_state(peer_device, val.pdsk);
	}
	if (mask.user_isp) {
		mask.user_isp ^= -1;
		__change_resync_susp_user(peer_device, val.user_isp);
	}
	if (mask.peer_isp) {
		mask.peer_isp ^= -1;
		__change_resync_susp_peer(peer_device, val.peer_isp);
	}
	if (mask.aftr_isp) {
		mask.aftr_isp ^= -1;
		__change_resync_susp_dependency(peer_device, val.aftr_isp);
	}
	return mask;
}

/**
 * change_connection_state()  -  change state of a connection and all its peer devices
 *
 * Also changes the state of the peer devices' devices and of the resource.
 * Cluster-wide state changes are not supported.
 */
static enum drbd_state_rv
change_connection_state(struct drbd_connection *connection,
			union drbd_state mask,
			union drbd_state val,
			enum chg_state_flags flags)
{
	struct drbd_peer_device *peer_device;
	union drbd_state mask_unused = mask;
	unsigned long irq_flags;
	int vnr;

	mask = convert_state(mask);
	val = convert_state(val);

	begin_state_change(connection->resource, &irq_flags, flags);
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr)
		mask_unused.i &= __change_peer_device_state(peer_device, mask, val).i;
	mask_unused.i &= __change_connection_state(connection, mask, val, flags).i;
	if (mask_unused.i) {
		abort_state_change(connection->resource, &irq_flags);
		return SS_NOT_SUPPORTED;
	}
	return end_state_change(connection->resource, &irq_flags);
}

/**
 * change_peer_device_state()  -  change state of a peer and its connection
 *
 * Also changes the state of the peer device's device and of the resource.
 * Cluster-wide state changes are not supported.
 */
static enum drbd_state_rv
change_peer_device_state(struct drbd_peer_device *peer_device,
			 union drbd_state mask,
			 union drbd_state val,
			 enum chg_state_flags flags)
{
	struct drbd_connection *connection = peer_device->connection;
	union drbd_state mask_unused = mask;
	unsigned long irq_flags;

	mask = convert_state(mask);
	val = convert_state(val);

	begin_state_change(connection->resource, &irq_flags, flags);
	mask_unused.i &= __change_peer_device_state(peer_device, mask, val).i;
	mask_unused.i &= __change_connection_state(connection, mask, val, flags).i;
	if (mask_unused.i) {
		abort_state_change(connection->resource, &irq_flags);
		return SS_NOT_SUPPORTED;
	}
	return end_state_change(connection->resource, &irq_flags);
}

static int receive_req_state(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_resource *resource = connection->resource;
	struct drbd_peer_device *peer_device = NULL;
	struct p_req_state *p = pi->data;
	union drbd_state mask, val;
	enum chg_state_flags flags = CS_VERBOSE | CS_LOCAL_ONLY | CS_TWOPC;
	enum drbd_state_rv rv;
	int vnr = -1;

	if (!expect(connection, connection->agreed_pro_version >= 110)) {
		drbd_err(connection, "Packet %s not allowed in protocol version %d\n",
			 cmdname(pi->cmd),
			 connection->agreed_pro_version);
		return -EIO;
	}

	/* P_STATE_CHG_REQ packets must have a valid vnr.  P_CONN_ST_CHG_REQ
	 * packets have an undefined vnr.  In the other packets, vnr == -1
	 * means that the packet applies to the connection.  */
	if (pi->cmd == P_STATE_CHG_REQ || (pi->cmd != P_CONN_ST_CHG_REQ && pi->vnr != -1)) {
		peer_device = conn_peer_device(connection, pi->vnr);
		if (!peer_device)
			return -EIO;
		vnr = peer_device->device->vnr;
	}

	rv = SS_SUCCESS;
	spin_lock_irq(&resource->req_lock);
	if (resource->remote_state_change)
		rv = SS_CONCURRENT_ST_CHG;
	else
		resource->remote_state_change = true;
	spin_unlock_irq(&resource->req_lock);

	if (rv != SS_SUCCESS) {
		drbd_info(connection, "Rejecting concurrent remote state change\n");
		drbd_send_sr_reply(connection, vnr, rv);
		return 0;
	}

	mask.i = be32_to_cpu(p->mask);
	val.i = be32_to_cpu(p->val);

	/* Send the reply before carrying out the state change: this is needed
	 * for connection state changes which close the network connection.  */
	if (peer_device) {
		rv = change_peer_device_state(peer_device, mask, val, flags | CS_PREPARE);
		drbd_send_sr_reply(connection, vnr, rv);
		rv = change_peer_device_state(peer_device, mask, val, flags | CS_PREPARED);
		if (rv >= SS_SUCCESS)
			drbd_md_sync(peer_device->device);
	} else {
		flags |= CS_IGN_OUTD_FAIL;
		rv = change_connection_state(connection, mask, val, flags | CS_PREPARE);
		drbd_send_sr_reply(connection, vnr, rv);
		change_connection_state(connection, mask, val, flags | CS_PREPARED);
	}

	spin_lock_irq(&resource->req_lock);
	resource->remote_state_change = false;
	spin_unlock_irq(&resource->req_lock);
	wake_up(&resource->twopc_wait);

	return 0;
}

int abort_nested_twopc_work(struct drbd_work *work, int cancel)
{
	struct drbd_resource *resource =
		container_of(work, struct drbd_resource, twopc_work);
	bool prepared = false;

	spin_lock_irq(&resource->req_lock);
	if (resource->twopc_reply.initiator_node_id != -1) {
		resource->remote_state_change = false;
		resource->twopc_reply.initiator_node_id = -1;
		if (resource->twopc_parent) {
			kref_debug_put(&resource->twopc_parent->kref_debug, 9);
			kref_put(&resource->twopc_parent->kref,
				 drbd_destroy_connection);
			resource->twopc_parent = NULL;
		}
		prepared = true;
	}
	spin_unlock_irq(&resource->req_lock);
	wake_up(&resource->twopc_wait);

	if (prepared)
		abort_prepared_state_change(resource);
	return 0;
}

void twopc_timer_fn(unsigned long data)
{
	struct drbd_resource *resource = (struct drbd_resource *) data;
	unsigned long irq_flags;

	spin_lock_irqsave(&resource->req_lock, irq_flags);
	if (resource->twopc_reply.tid != -1) {
		drbd_debug(resource, "Two-phase commit %u timeout\n",
			   resource->twopc_reply.tid);
	}
	resource->twopc_work.cb = abort_nested_twopc_work;
	drbd_queue_work(&resource->work, &resource->twopc_work);
	spin_unlock_irqrestore(&resource->req_lock, irq_flags);
}

static void update_reachability(struct drbd_connection *connection, u64 mask)
{
	struct drbd_resource *resource = connection->resource;

	spin_lock_irq(&resource->req_lock);
	if (connection->cstate[NOW] >= C_CONNECTED) {
		mask &= ~((u64)1 << resource->res_opts.node_id);
		connection->primary_mask = mask;
	}
	spin_unlock_irq(&resource->req_lock);
}

static int receive_twopc(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_connection *affected_connection = connection;
	struct drbd_resource *resource = connection->resource;
	struct drbd_peer_device *peer_device = NULL;
	struct p_twopc_request *p = pi->data;
	struct twopc_reply reply;
	union drbd_state mask = {}, val = {};
	enum chg_state_flags flags = CS_VERBOSE | CS_LOCAL_ONLY | CS_TWOPC;
	enum drbd_state_rv rv;

	reply.vnr = pi->vnr;
	reply.tid = be32_to_cpu(p->tid);
	reply.initiator_node_id = be32_to_cpu(p->initiator_node_id);
	reply.target_node_id = be32_to_cpu(p->target_node_id);
	reply.primary_nodes = be64_to_cpu(p->primary_nodes);
	reply.weak_nodes = be64_to_cpu(p->weak_nodes);
	reply.reachable_nodes = directly_connected_nodes(resource) |
				NODE_MASK(resource->res_opts.node_id);
	reply.is_disconnect = 0;

	/* Check for concurrent transactions and duplicate packets. */
	spin_lock_irq(&resource->req_lock);

	if (resource->remote_state_change) {
		if (resource->twopc_reply.initiator_node_id != reply.initiator_node_id ||
		    resource->twopc_reply.tid != reply.tid) {
			spin_unlock_irq(&resource->req_lock);
			if (pi->cmd == P_TWOPC_PREPARE) {
				drbd_info(connection, "Rejecting concurrent "
					  "remote state change %u\n", reply.tid);
				drbd_send_twopc_reply(connection, P_TWOPC_RETRY, &reply);
			} else {
				drbd_info(connection, "Ignoring %s packet %u\n",
					  cmdname(pi->cmd),
					  reply.tid);
			}
			return 0;
		}
		if (pi->cmd == P_TWOPC_PREPARE) {
			/* We have prepared this transaction already. */
			spin_unlock_irq(&resource->req_lock);
			drbd_send_twopc_reply(connection, P_TWOPC_YES, &reply);
			return 0;
		}
		flags |= CS_PREPARED;
	} else {
		if (pi->cmd != P_TWOPC_PREPARE) {
			/* We have committed or aborted this transaction already. */
			spin_unlock_irq(&resource->req_lock);
			drbd_debug(connection, "Ignoring %s packet %u\n",
				   cmdname(pi->cmd),
				   reply.tid);
			update_reachability(connection, reply.primary_nodes);
			return 0;
		}
		resource->remote_state_change = true;
	}

	if (reply.initiator_node_id != connection->net_conf->peer_node_id) {
		/*
		 * This is an indirect request.  Unless we are directly
		 * connected to the initiator as well as indirectly, we don't
		 * have connection or peer device objects for this peer.
		 */
		for_each_connection(affected_connection, resource) {
			if (reply.initiator_node_id ==
			    affected_connection->net_conf->peer_node_id)
				goto directly_connected;
		}
		/* only indirectly connected */
		affected_connection = NULL;
		goto next;
	}

    directly_connected:
	if (reply.target_node_id != -1 &&
	    reply.target_node_id != resource->res_opts.node_id) {
		affected_connection = NULL;
		goto next;
	}

	mask.i = be32_to_cpu(p->mask);
	val.i = be32_to_cpu(p->val);

	if (mask.conn == conn_MASK) {
		u64 m = NODE_MASK(reply.initiator_node_id);

		if (val.conn == C_CONNECTED)
			reply.reachable_nodes |= m;
		if (val.conn == C_DISCONNECTING) {
			reply.reachable_nodes &= ~m;
			reply.is_disconnect = 1;
		}
	}

	if (pi->vnr != -1) {
		peer_device = conn_peer_device(affected_connection, pi->vnr);
		if (!peer_device) {
			spin_unlock_irq(&resource->req_lock);
			return -EIO;
		}
	}

    next:
	if (pi->cmd == P_TWOPC_PREPARE) {
		if ((mask.peer == role_MASK &&
		     val.peer == R_PRIMARY) ||
		    (mask.peer != role_MASK &&
		     resource->role[NOW] == R_PRIMARY)) {
			u64 m = NODE_MASK(resource->res_opts.node_id);
			reply.primary_nodes |= m;
			m |= reply.reachable_nodes;
			reply.weak_nodes |= ~m;
		}
	}

	resource->twopc_reply = reply;
	spin_unlock_irq(&resource->req_lock);
	del_connect_timer(connection);

	switch(pi->cmd) {
	case P_TWOPC_PREPARE:
		drbd_info(connection, "Preparing remote state change %u\n",
			  reply.tid);
		flags |= CS_PREPARE;
		break;
	case P_TWOPC_ABORT:
		drbd_info(connection, "Aborting remote state change %u\n",
			  reply.tid);
		flags |= CS_ABORT;
		break;
	default:
		drbd_info(connection, "Committing remote state change %u "
				"(primary_nodes=%lX, weak_nodes=%lX)\n",
			  reply.tid,
			  (unsigned long)reply.primary_nodes,
			  (unsigned long)reply.weak_nodes);
		flags |= CS_WEAK_NODES;
		break;
	}

	if (!(flags & CS_PREPARE))
		nested_twopc_request(resource, pi->vnr, pi->cmd, p);

	if (peer_device)
		rv = change_peer_device_state(peer_device, mask, val, flags);
	else
		rv = change_connection_state(
			affected_connection ? affected_connection : connection,
			mask, val, flags | CS_IGN_OUTD_FAIL);

	if (flags & CS_PREPARE) {
		if (rv >= SS_SUCCESS) {
			spin_lock_irq(&resource->req_lock);
			kref_get(&connection->kref);
			kref_debug_get(&connection->kref_debug, 9);
			resource->twopc_parent = connection;
			resource->twopc_timer.expires = jiffies + twopc_timeout(resource);
			add_timer(&resource->twopc_timer);
			spin_unlock_irq(&resource->req_lock);

			nested_twopc_request(resource, pi->vnr, pi->cmd, p);
		} else {
			enum drbd_packet cmd = (rv == SS_IN_TRANSIENT_STATE) ?
				P_TWOPC_RETRY : P_TWOPC_NO;
			drbd_send_twopc_reply(connection, cmd, &reply);
		}
	} else {
		if (peer_device && rv >= SS_SUCCESS && !(flags & (CS_PREPARE | CS_ABORT)))
			drbd_md_sync(peer_device->device);

		if (flags & CS_PREPARED) {
			struct drbd_device *device;
			int vnr;

			del_timer(&resource->twopc_timer);

			if (affected_connection &&
			    mask.conn == conn_MASK && val.conn == C_CONNECTED)
				conn_connect2(connection);

			update_reachability(connection, reply.primary_nodes);

			idr_for_each_entry(&resource->devices, device, vnr) {
				u64 nedu = device->next_exposed_data_uuid;
				if (!nedu)
					continue;
				if (device->disk_state[NOW] < D_INCONSISTENT)
					drbd_set_exposed_data_uuid(device, nedu);
				device->next_exposed_data_uuid = 0;
			}

		}
	}

	return 0;
}

static int receive_state(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_resource *resource = connection->resource;
	struct drbd_peer_device *peer_device = NULL;
	enum drbd_repl_state *repl_state;
	struct drbd_device *device = NULL;
	struct p_state *p = pi->data;
	union drbd_state os, peer_state;
	enum drbd_disk_state peer_disk_state;
	enum drbd_repl_state new_repl_state;
	int rv;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return config_unknown_volume(connection, pi);
	device = peer_device->device;

	peer_state.i = be32_to_cpu(p->state);

	peer_disk_state = peer_state.disk;
	if (peer_state.disk == D_NEGOTIATING) {
		peer_disk_state = peer_device->uuid_flags & UUID_FLAG_INCONSISTENT ?
			D_INCONSISTENT : D_CONSISTENT;
		drbd_info(device, "real peer disk state = %s\n", drbd_disk_str(peer_disk_state));
	}

	spin_lock_irq(&resource->req_lock);
	os = drbd_get_peer_device_state(peer_device, NOW);
	spin_unlock_irq(&resource->req_lock);
 retry:
	new_repl_state = max_t(enum drbd_repl_state, os.conn, L_OFF);

	/* If some other part of the code (asender thread, timeout)
	 * already decided to close the connection again,
	 * we must not "re-establish" it here. */
	if (os.conn <= C_TEAR_DOWN)
		return -ECONNRESET;

	/* If this is the "end of sync" confirmation, usually the peer disk
	 * was D_INCONSISTENT or D_CONSISTENT. (Since the peer might be
	 * weak we do not know anything about its new disk state)
	 */
	if ((os.pdsk == D_INCONSISTENT || os.pdsk == D_CONSISTENT) &&
	    os.conn > L_ESTABLISHED && os.disk == D_UP_TO_DATE) {
		/* If we are (becoming) SyncSource, but peer is still in sync
		 * preparation, ignore its uptodate-ness to avoid flapping, it
		 * will change to inconsistent once the peer reaches active
		 * syncing states.
		 * It may have changed syncer-paused flags, however, so we
		 * cannot ignore this completely. */
		if (peer_state.conn > L_ESTABLISHED &&
		    peer_state.conn < L_SYNC_SOURCE)
			peer_disk_state = D_INCONSISTENT;

		/* if peer_state changes to connected at the same time,
		 * it explicitly notifies us that it finished resync.
		 * Maybe we should finish it up, too? */
		else if (os.conn >= L_SYNC_SOURCE &&
			 peer_state.conn == L_ESTABLISHED) {
			if (drbd_bm_total_weight(peer_device) <= peer_device->rs_failed)
				drbd_resync_finished(peer_device, peer_state.disk);
			return 0;
		}
	}

	/* explicit verify finished notification, stop sector reached. */
	if (os.conn == L_VERIFY_T && os.disk == D_UP_TO_DATE &&
	    peer_state.conn == C_CONNECTED && peer_disk_state == D_UP_TO_DATE) {
		ov_out_of_sync_print(peer_device);
		drbd_resync_finished(peer_device, D_MASK);
		return 0;
	}

	/* peer says his disk is inconsistent, while we think it is uptodate,
	 * and this happens while the peer still thinks we have a sync going on,
	 * but we think we are already done with the sync.
	 * We ignore this to avoid flapping pdsk.
	 * This should not happen, if the peer is a recent version of drbd. */
	if (os.pdsk == D_UP_TO_DATE && peer_disk_state == D_INCONSISTENT &&
	    os.conn == L_ESTABLISHED && peer_state.conn > L_SYNC_SOURCE)
		peer_disk_state = D_UP_TO_DATE;

	if (new_repl_state == L_OFF)
		new_repl_state = L_ESTABLISHED;

	if (peer_state.conn == L_AHEAD)
		new_repl_state = L_BEHIND;

	if (peer_state.conn == L_PAUSED_SYNC_T && peer_state.disk == D_OUTDATED &&
	    os.conn == L_ESTABLISHED) {
		/* Looks like the peer was invalidated with drbdadm */
		drbd_info(peer_device, "Setting bits\n");
		drbd_bitmap_io(device, &drbd_bmio_set_n_write, "set_n_write from receive_state",
			       BM_LOCK_CLEAR | BM_LOCK_BULK, peer_device);
		new_repl_state = L_PAUSED_SYNC_S;
	}

	if (peer_device->uuids_received &&
	    peer_state.disk >= D_NEGOTIATING &&
	    get_ldev_if_state(device, D_NEGOTIATING)) {
		bool consider_resync;

		/* if we established a new connection */
		consider_resync = (os.conn < L_ESTABLISHED);
		/* if we had an established connection
		 * and one of the nodes newly attaches a disk */
		consider_resync |= (os.conn == L_ESTABLISHED &&
				    (peer_state.disk == D_NEGOTIATING ||
				     os.disk == D_NEGOTIATING));
		/* if we have both been inconsistent, and the peer has been
		 * forced to be UpToDate with --force */
		consider_resync |= test_bit(CONSIDER_RESYNC, &peer_device->flags);
		/* if we had been plain connected, and the admin requested to
		 * start a sync by "invalidate" or "invalidate-remote" */
		consider_resync |= (os.conn == L_ESTABLISHED &&
				    (peer_state.conn == L_STARTING_SYNC_S ||
				     peer_state.conn == L_STARTING_SYNC_T));

		if (consider_resync)
			new_repl_state = drbd_sync_handshake(peer_device, peer_state.role, peer_disk_state);
		else if (os.conn == L_ESTABLISHED && peer_state.conn == L_WF_BITMAP_T &&
			 connection->peer_weak[NOW] && !peer_state.weak) {
			drbd_info(peer_device, "Resync because peer leaves weak state\n");
			new_repl_state = L_WF_BITMAP_S;
		}

		put_ldev(device);
		if (new_repl_state == -1) {
			new_repl_state = L_ESTABLISHED;
			if (device->disk_state[NOW] == D_NEGOTIATING) {
				change_disk_state(device, D_FAILED, CS_HARD);
			} else if (peer_state.disk == D_NEGOTIATING) {
				drbd_err(device, "Disk attach process on the peer node was aborted.\n");
				peer_state.disk = D_DISKLESS;
				peer_disk_state = D_DISKLESS;
			} else {
				if (test_and_clear_bit(CONN_DRY_RUN, &connection->flags))
					return -EIO;
				D_ASSERT(device, os.conn == L_OFF);
				change_cstate(connection, C_DISCONNECTING, CS_HARD);
				return -EIO;
			}
		}
	}

	spin_lock_irq(&resource->req_lock);
	begin_state_change_locked(resource, CS_VERBOSE);
	if (os.i != drbd_get_peer_device_state(peer_device, NOW).i) {
		os = drbd_get_peer_device_state(peer_device, NOW);
		abort_state_change_locked(resource);
		spin_unlock_irq(&resource->req_lock);
		goto retry;
	}
	clear_bit(CONSIDER_RESYNC, &peer_device->flags);
	if (device->disk_state[NOW] == D_NEGOTIATING) {
		set_bit(NEGOTIATION_RESULT_TOCHED, &resource->flags);
		peer_device->negotiation_result = new_repl_state;
	} else
		__change_repl_state(peer_device, new_repl_state);
	if (connection->peer_role[NOW] == R_UNKNOWN)
		__change_peer_role(connection, peer_state.role);
	__change_peer_weak(connection, peer_state.weak);
	__change_peer_disk_state(peer_device, peer_disk_state);
	__change_resync_susp_peer(peer_device, peer_state.aftr_isp | peer_state.user_isp);
	repl_state = peer_device->repl_state;
	if (repl_state[OLD] < L_ESTABLISHED && repl_state[NEW] >= L_ESTABLISHED)
		resource->state_change_flags |= CS_HARD;
	if (peer_device->disk_state[NEW] == D_CONSISTENT &&
	    drbd_suspended(device) &&
	    repl_state[OLD] < L_ESTABLISHED && repl_state[NEW] == L_ESTABLISHED &&
	    test_bit(NEW_CUR_UUID, &device->flags)) {
		unsigned long irq_flags;

		/* Do not allow RESEND for a rebooted peer. We can only allow this
		   for temporary network outages! */
		abort_state_change_locked(resource);
		spin_unlock_irq(&resource->req_lock);

		drbd_err(device, "Aborting Connect, can not thaw IO with an only Consistent peer\n");
		tl_clear(connection);
		drbd_uuid_new_current(device);
		clear_bit(NEW_CUR_UUID, &device->flags);
		begin_state_change(resource, &irq_flags, CS_HARD);
		__change_cstate(connection, C_PROTOCOL_ERROR);
		__change_io_susp_user(resource, false);
		end_state_change(resource, &irq_flags);
		return -EIO;
	}
	rv = end_state_change_locked(resource);
	new_repl_state = peer_device->repl_state[NOW];
	set_bit(INITIAL_STATE_RECEIVED, &peer_device->flags);
	spin_unlock_irq(&resource->req_lock);

	if (rv < SS_SUCCESS) {
		change_cstate(connection, C_DISCONNECTING, CS_HARD);
		return -EIO;
	}

	if (os.conn > L_OFF) {
		if (new_repl_state > L_ESTABLISHED && peer_state.conn <= L_ESTABLISHED &&
		    peer_state.disk != D_NEGOTIATING ) {
			/* we want resync, peer has not yet decided to sync... */
			/* Nowadays only used when forcing a node into primary role and
			   setting its disk to UpToDate with that */
			drbd_send_uuids(peer_device, 0, 0);
			drbd_send_current_state(peer_device);
		}
	}

	clear_bit(DISCARD_MY_DATA, &device->flags);

	drbd_md_sync(device); /* update connected indicator, effective_size, ... */

	return 0;
}

static int receive_sync_uuid(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_uuid *p = pi->data;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	wait_event(device->misc_wait,
		   peer_device->repl_state[NOW] == L_WF_SYNC_UUID ||
		   peer_device->repl_state[NOW] == L_BEHIND ||
		   peer_device->repl_state[NOW] < L_ESTABLISHED ||
		   device->disk_state[NOW] < D_NEGOTIATING);

	/* D_ASSERT(device,  peer_device->repl_state[NOW] == L_WF_SYNC_UUID ); */

	/* Here the _drbd_uuid_ functions are right, current should
	   _not_ be rotated into the history */
	if (get_ldev_if_state(device, D_NEGOTIATING)) {
		_drbd_uuid_set_current(device, be64_to_cpu(p->uuid));
		_drbd_uuid_set_bitmap(peer_device, 0UL);

		drbd_print_uuids(peer_device, "updated sync uuid");
		drbd_start_resync(peer_device, L_SYNC_TARGET);

		put_ldev(device);
	} else
		drbd_err(device, "Ignoring SyncUUID packet!\n");

	return 0;
}

/**
 * receive_bitmap_plain
 *
 * Return 0 when done, 1 when another iteration is needed, and a negative error
 * code upon failure.
 */
static int
receive_bitmap_plain(struct drbd_peer_device *peer_device, unsigned int size,
		     unsigned long *p, struct bm_xfer_ctx *c)
{
	unsigned int data_size = DRBD_SOCKET_BUFFER_SIZE -
				 drbd_header_size(peer_device->connection);
	unsigned int num_words = min_t(size_t, data_size / sizeof(*p),
				       c->bm_words - c->word_offset);
	unsigned int want = num_words * sizeof(*p);
	int err;

	if (want != size) {
		drbd_err(peer_device, "%s:want (%u) != size (%u)\n", __func__, want, size);
		return -EIO;
	}
	if (want == 0)
		return 0;
	err = drbd_recv_all(peer_device->connection, p, want);
	if (err)
		return err;

	drbd_bm_merge_lel(peer_device, c->word_offset, num_words, p);

	c->word_offset += num_words;
	c->bit_offset = c->word_offset * BITS_PER_LONG;
	if (c->bit_offset > c->bm_bits)
		c->bit_offset = c->bm_bits;

	return 1;
}

static enum drbd_bitmap_code dcbp_get_code(struct p_compressed_bm *p)
{
	return (enum drbd_bitmap_code)(p->encoding & 0x0f);
}

static int dcbp_get_start(struct p_compressed_bm *p)
{
	return (p->encoding & 0x80) != 0;
}

static int dcbp_get_pad_bits(struct p_compressed_bm *p)
{
	return (p->encoding >> 4) & 0x7;
}

/**
 * recv_bm_rle_bits
 *
 * Return 0 when done, 1 when another iteration is needed, and a negative error
 * code upon failure.
 */
static int
recv_bm_rle_bits(struct drbd_peer_device *peer_device,
		struct p_compressed_bm *p,
		 struct bm_xfer_ctx *c,
		 unsigned int len)
{
	struct bitstream bs;
	u64 look_ahead;
	u64 rl;
	u64 tmp;
	unsigned long s = c->bit_offset;
	unsigned long e;
	int toggle = dcbp_get_start(p);
	int have;
	int bits;

	bitstream_init(&bs, p->code, len, dcbp_get_pad_bits(p));

	bits = bitstream_get_bits(&bs, &look_ahead, 64);
	if (bits < 0)
		return -EIO;

	for (have = bits; have > 0; s += rl, toggle = !toggle) {
		bits = vli_decode_bits(&rl, look_ahead);
		if (bits <= 0)
			return -EIO;

		if (toggle) {
			e = s + rl -1;
			if (e >= c->bm_bits) {
				drbd_err(peer_device, "bitmap overflow (e:%lu) while decoding bm RLE packet\n", e);
				return -EIO;
			}
			drbd_bm_set_many_bits(peer_device, s, e);
		}

		if (have < bits) {
			drbd_err(peer_device, "bitmap decoding error: h:%d b:%d la:0x%08llx l:%u/%u\n",
				have, bits, look_ahead,
				(unsigned int)(bs.cur.b - p->code),
				(unsigned int)bs.buf_len);
			return -EIO;
		}
		/* if we consumed all 64 bits, assign 0; >> 64 is "undefined"; */
		if (likely(bits < 64))
			look_ahead >>= bits;
		else
			look_ahead = 0;
		have -= bits;

		bits = bitstream_get_bits(&bs, &tmp, 64 - have);
		if (bits < 0)
			return -EIO;
		look_ahead |= tmp << have;
		have += bits;
	}

	c->bit_offset = s;
	bm_xfer_ctx_bit_to_word_offset(c);

	return (s != c->bm_bits);
}

/**
 * decode_bitmap_c
 *
 * Return 0 when done, 1 when another iteration is needed, and a negative error
 * code upon failure.
 */
static int
decode_bitmap_c(struct drbd_peer_device *peer_device,
		struct p_compressed_bm *p,
		struct bm_xfer_ctx *c,
		unsigned int len)
{
	if (dcbp_get_code(p) == RLE_VLI_Bits)
		return recv_bm_rle_bits(peer_device, p, c, len - sizeof(*p));

	/* other variants had been implemented for evaluation,
	 * but have been dropped as this one turned out to be "best"
	 * during all our tests. */

	drbd_err(peer_device, "receive_bitmap_c: unknown encoding %u\n", p->encoding);
	change_cstate(peer_device->connection, C_PROTOCOL_ERROR, CS_HARD);
	return -EIO;
}

void INFO_bm_xfer_stats(struct drbd_peer_device *peer_device,
		const char *direction, struct bm_xfer_ctx *c)
{
	/* what would it take to transfer it "plaintext" */
	unsigned int header_size = drbd_header_size(peer_device->connection);
	unsigned int data_size = DRBD_SOCKET_BUFFER_SIZE - header_size;
	unsigned int plain =
		header_size * (DIV_ROUND_UP(c->bm_words, data_size) + 1) +
		c->bm_words * sizeof(unsigned long);
	unsigned int total = c->bytes[0] + c->bytes[1];
	unsigned int r;

	/* total can not be zero. but just in case: */
	if (total == 0)
		return;

	/* don't report if not compressed */
	if (total >= plain)
		return;

	/* total < plain. check for overflow, still */
	r = (total > UINT_MAX/1000) ? (total / (plain/1000))
		                    : (1000 * total / plain);

	if (r > 1000)
		r = 1000;

	r = 1000 - r;
	drbd_info(peer_device, "%s bitmap stats [Bytes(packets)]: plain %u(%u), RLE %u(%u), "
	     "total %u; compression: %u.%u%%\n",
			direction,
			c->bytes[1], c->packets[1],
			c->bytes[0], c->packets[0],
			total, r/10, r % 10);
}

static enum drbd_disk_state read_disk_state(struct drbd_device *device)
{
	struct drbd_resource *resource = device->resource;
	enum drbd_disk_state disk_state;

	spin_lock_irq(&resource->req_lock);
	disk_state = device->disk_state[NOW];
	spin_unlock_irq(&resource->req_lock);

	return disk_state;
}

/* Since we are processing the bitfield from lower addresses to higher,
   it does not matter if the process it in 32 bit chunks or 64 bit
   chunks as long as it is little endian. (Understand it as byte stream,
   beginning with the lowest byte...) If we would use big endian
   we would need to process it from the highest address to the lowest,
   in order to be agnostic to the 32 vs 64 bits issue.

   returns 0 on failure, 1 if we successfully received it. */
static int receive_bitmap(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct bm_xfer_ctx c;
	int err;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	/* Final repl_states become visible when the disk leaves NEGOTIATING state */
	wait_event_interruptible(device->resource->state_wait,
				 read_disk_state(device) != D_NEGOTIATING);

	drbd_bm_slot_lock(peer_device, "receive bitmap", BM_LOCK_CLEAR | BM_LOCK_BULK);
	/* you are supposed to send additional out-of-sync information
	 * if you actually set bits during this phase */

	c = (struct bm_xfer_ctx) {
		.bm_bits = drbd_bm_bits(device),
		.bm_words = drbd_bm_words(device),
	};

	for(;;) {
		if (pi->cmd == P_BITMAP)
			err = receive_bitmap_plain(peer_device, pi->size, pi->data, &c);
		else if (pi->cmd == P_COMPRESSED_BITMAP) {
			/* MAYBE: sanity check that we speak proto >= 90,
			 * and the feature is enabled! */
			struct p_compressed_bm *p = pi->data;

			if (pi->size > DRBD_SOCKET_BUFFER_SIZE - drbd_header_size(connection)) {
				drbd_err(device, "ReportCBitmap packet too large\n");
				err = -EIO;
				goto out;
			}
			if (pi->size <= sizeof(*p)) {
				drbd_err(device, "ReportCBitmap packet too small (l:%u)\n", pi->size);
				err = -EIO;
				goto out;
			}
			err = drbd_recv_all(peer_device->connection, p, pi->size);
			if (err)
			       goto out;
			err = decode_bitmap_c(peer_device, p, &c, pi->size);
		} else {
			drbd_warn(device, "receive_bitmap: cmd neither ReportBitMap nor ReportCBitMap (is 0x%x)", pi->cmd);
			err = -EIO;
			goto out;
		}

		c.packets[pi->cmd == P_BITMAP]++;
		c.bytes[pi->cmd == P_BITMAP] += drbd_header_size(connection) + pi->size;

		if (err <= 0) {
			if (err < 0)
				goto out;
			break;
		}
		err = drbd_recv_header(peer_device->connection, pi);
		if (err)
			goto out;
	}

	INFO_bm_xfer_stats(peer_device, "receive", &c);

	if (peer_device->repl_state[NOW] == L_WF_BITMAP_T) {
		enum drbd_state_rv rv;

		err = drbd_send_bitmap(device, peer_device);
		if (err)
			goto out;
		/* Omit CS_WAIT_COMPLETE and CS_SERIALIZE with this state
		 * transition to avoid deadlocks. */

		if (connection->agreed_pro_version < 110) {
			rv = stable_change_repl_state(peer_device, L_WF_SYNC_UUID, CS_VERBOSE);
			D_ASSERT(device, rv == SS_SUCCESS);
		} else {
			drbd_start_resync(peer_device, L_SYNC_TARGET);
		}
	} else if (peer_device->repl_state[NOW] != L_WF_BITMAP_S) {
		/* admin may have requested C_DISCONNECTING,
		 * other threads may have noticed network errors */
		drbd_info(device, "unexpected repl_state (%s) in receive_bitmap\n",
		    drbd_repl_str(peer_device->repl_state[NOW]));
	}
	err = 0;

 out:
	drbd_bm_slot_unlock(peer_device);
	if (!err && peer_device->repl_state[NOW] == L_WF_BITMAP_S)
		drbd_start_resync(peer_device, L_SYNC_SOURCE);
	return err;
}

static int receive_skip(struct drbd_connection *connection, struct packet_info *pi)
{
	drbd_warn(connection, "skipping unknown optional packet type %d, l: %d!\n",
		 pi->cmd, pi->size);

	return ignore_remaining_packet(connection, pi);
}

static int receive_UnplugRemote(struct drbd_connection *connection, struct packet_info *pi)
{
	/* just unplug all devices always, regardless which volume number */
	drbd_unplug_all_devices(connection->resource);

	/* Make sure we've acked all the TCP data associated
	 * with the data requests being unplugged */
	drbd_tcp_quickack(connection->data.socket);

	return 0;
}

static int receive_out_of_sync(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_block_desc *p = pi->data;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	switch (peer_device->repl_state[NOW]) {
	case L_WF_SYNC_UUID:
	case L_WF_BITMAP_T:
	case L_BEHIND:
			break;
	default:
		drbd_err(device, "ASSERT FAILED cstate = %s, expected: WFSyncUUID|WFBitMapT|Behind\n",
				drbd_repl_str(peer_device->repl_state[NOW]));
	}

	drbd_set_out_of_sync(peer_device, be64_to_cpu(p->sector), be32_to_cpu(p->blksize));

	return 0;
}

static int receive_dagtag(struct drbd_connection *connection, struct packet_info *pi)
{
	struct p_dagtag *p = pi->data;

	connection->last_dagtag_sector = be64_to_cpu(p->dagtag);
	return 0;
}

struct drbd_connection *drbd_connection_by_node_id(struct drbd_resource *resource, int node_id)
{
	struct drbd_connection *connection;
	struct net_conf *nc;

	rcu_read_lock();
	for_each_connection_rcu(connection, resource) {
		nc = rcu_dereference(connection->net_conf);
		if (nc && nc->peer_node_id == node_id) {
			rcu_read_unlock();
			return connection;
		}
	}
	rcu_read_unlock();

	return NULL;
}

static int receive_peer_dagtag(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_resource *resource = connection->resource;
	struct drbd_peer_device *peer_device;
	enum drbd_repl_state new_repl_state;
	struct p_peer_dagtag *p = pi->data;
	struct drbd_connection *lost_peer;
	s64 dagtag_offset;
	int vnr = 0;

	lost_peer = drbd_connection_by_node_id(resource, be32_to_cpu(p->node_id));
	if (!lost_peer)
		return 0;

	if (lost_peer->cstate[NOW] == C_CONNECTED) {
		drbd_ping_peer(lost_peer);
		if (lost_peer->cstate[NOW] == C_CONNECTED)
			return 0;
	}

	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		if (peer_device->repl_state[NOW] > L_ESTABLISHED)
			return 0;
		if (peer_device->current_uuid != drbd_current_uuid(peer_device->device)) {
			if (!connection->resource->weak[NOW]) {
				drbd_err(peer_device, "ASSERT FAILED not weak and non matching current UUIDs\n");
				drbd_uuid_dump_self(peer_device, 0, 0);
				drbd_uuid_dump_peer(peer_device, 0, 0);
			}
			return 0;
		}
	}

	/* Need to wait until the other receiver thread has called the
	   cleanup_unacked_peer_requests() function */
	wait_event(resource->state_wait,
		   lost_peer->cstate[NOW] <= C_UNCONNECTED || lost_peer->cstate[NOW] == C_CONNECTING);

	dagtag_offset = (s64)lost_peer->last_dagtag_sector - (s64)be64_to_cpu(p->dagtag);
	if (dagtag_offset > 0)
		new_repl_state = L_WF_BITMAP_S;
	else if (dagtag_offset < 0)
		new_repl_state = L_WF_BITMAP_T;
	else
		new_repl_state = L_ESTABLISHED;

	if (new_repl_state != L_ESTABLISHED) {
		unsigned long irq_flags;

		drbd_info(connection, "Reconciliation resync because \'%s\' disappeared. (o=%d)\n",
			  lost_peer->net_conf->name, (int)dagtag_offset);

		begin_state_change(resource, &irq_flags, CS_VERBOSE);
		idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
			__change_repl_state(peer_device, new_repl_state);
			set_bit(RECONCILIATION_RESYNC, &peer_device->flags);
		}
		end_state_change(resource, &irq_flags);
	} else {
		drbd_info(connection, "No reconciliation resync even though \'%s\' disappeared. (o=%d)\n",
			  lost_peer->net_conf->name, (int)dagtag_offset);

		idr_for_each_entry(&connection->peer_devices, peer_device, vnr)
			drbd_bm_clear_many_bits(peer_device, 0, -1UL);
	}

	return 0;
}

/* Accept a new current UUID generated on a diskless node, that just became primary */
static int receive_current_uuid(struct drbd_connection *connection, struct packet_info *pi)
{
	const int node_id = connection->resource->res_opts.node_id;
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_uuid *p = pi->data;
	u64 current_uuid;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	current_uuid = be64_to_cpu(p->uuid);
	if (current_uuid == drbd_current_uuid(peer_device->device))
		return 0;
	peer_device->current_uuid = current_uuid;

	drbd_warn(peer_device, "received new current UUID: %llX\n", current_uuid);
	if (get_ldev(device)) {
		if (connection->peer_role[NOW] == R_PRIMARY) {
			drbd_warn(peer_device, "received new current UUID: %llX\n", current_uuid);
			drbd_uuid_received_new_current(device, current_uuid, 0);
		} else {
			if (peer_device->bitmap_uuids[node_id] == 0 && connection->resource->weak[NOW])
				peer_device->bitmap_uuids[node_id] = peer_device->current_uuid;
		}
		put_ldev(device);
	} else if (device->resource->role[NOW] == R_PRIMARY) {
		drbd_set_exposed_data_uuid(device, peer_device->current_uuid);
	}

	return 0;
}

static int receive_reachability(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_resource *resource = connection->resource;
	const int my_node_id = resource->res_opts.node_id;
	const int peer_node_id = connection->net_conf->peer_node_id;
	struct p_pri_reachable *p = pi->data;
	unsigned long irq_flags;

	begin_state_change(resource, &irq_flags, CS_VERBOSE);
	connection->primary_mask = be64_to_cpu(p->primary_mask) & ~(1ULL << my_node_id);
	__change_weak(resource, drbd_calc_weak(resource));
	if (!(connection->primary_mask & NODE_MASK(peer_node_id)) &&
	    connection->peer_role[NOW] != R_SECONDARY)
		__change_peer_role(connection, R_SECONDARY);
	end_state_change(resource, &irq_flags);

	return 0;
}

struct data_cmd {
	int expect_payload;
	size_t pkt_size;
	int (*fn)(struct drbd_connection *, struct packet_info *);
};

static struct data_cmd drbd_cmd_handler[] = {
	[P_DATA]	    = { 1, sizeof(struct p_data), receive_Data },
	[P_DATA_REPLY]	    = { 1, sizeof(struct p_data), receive_DataReply },
	[P_RS_DATA_REPLY]   = { 1, sizeof(struct p_data), receive_RSDataReply } ,
	[P_BARRIER]	    = { 0, sizeof(struct p_barrier), receive_Barrier } ,
	[P_BITMAP]	    = { 1, 0, receive_bitmap } ,
	[P_COMPRESSED_BITMAP] = { 1, 0, receive_bitmap } ,
	[P_UNPLUG_REMOTE]   = { 0, 0, receive_UnplugRemote },
	[P_DATA_REQUEST]    = { 0, sizeof(struct p_block_req), receive_DataRequest },
	[P_RS_DATA_REQUEST] = { 0, sizeof(struct p_block_req), receive_DataRequest },
	[P_SYNC_PARAM]	    = { 1, 0, receive_SyncParam },
	[P_SYNC_PARAM89]    = { 1, 0, receive_SyncParam },
	[P_PROTOCOL]        = { 1, sizeof(struct p_protocol), receive_protocol },
	[P_UUIDS]	    = { 0, sizeof(struct p_uuids), receive_uuids },
	[P_SIZES]	    = { 0, sizeof(struct p_sizes), receive_sizes },
	[P_STATE]	    = { 0, sizeof(struct p_state), receive_state },
	[P_STATE_CHG_REQ]   = { 0, sizeof(struct p_req_state), receive_req_state },
	[P_SYNC_UUID]       = { 0, sizeof(struct p_uuid), receive_sync_uuid },
	[P_OV_REQUEST]      = { 0, sizeof(struct p_block_req), receive_DataRequest },
	[P_OV_REPLY]        = { 1, sizeof(struct p_block_req), receive_DataRequest },
	[P_CSUM_RS_REQUEST] = { 1, sizeof(struct p_block_req), receive_DataRequest },
	[P_DELAY_PROBE]     = { 0, sizeof(struct p_delay_probe93), receive_skip },
	[P_OUT_OF_SYNC]     = { 0, sizeof(struct p_block_desc), receive_out_of_sync },
	[P_CONN_ST_CHG_REQ] = { 0, sizeof(struct p_req_state), receive_req_state },
	[P_PROTOCOL_UPDATE] = { 1, sizeof(struct p_protocol), receive_protocol },
	[P_TWOPC_PREPARE] = { 0, sizeof(struct p_twopc_request), receive_twopc },
	[P_TWOPC_ABORT] = { 0, sizeof(struct p_twopc_request), receive_twopc },
	[P_DAGTAG]	    = { 0, sizeof(struct p_dagtag), receive_dagtag },
	[P_UUIDS110]	    = { 1, sizeof(struct p_uuids110), receive_uuids110 },
	[P_PEER_DAGTAG]     = { 0, sizeof(struct p_peer_dagtag), receive_peer_dagtag },
	[P_CURRENT_UUID]    = { 0, sizeof(struct p_uuid), receive_current_uuid },
	[P_TWOPC_COMMIT]    = { 0, sizeof(struct p_twopc_request), receive_twopc },
	[P_PRI_REACHABLE]   = { 0, sizeof(struct p_pri_reachable), receive_reachability },
	[P_TRIM]	    = { 0, sizeof(struct p_trim), receive_Data },
};

static void drbdd(struct drbd_connection *connection)
{
	struct packet_info pi;
	size_t shs; /* sub header size */
	int err;

	while (get_t_state(&connection->receiver) == RUNNING) {
		struct data_cmd *cmd;
		long start;

		drbd_thread_current_set_cpu(&connection->receiver);
		if (drbd_recv_header(connection, &pi))
			goto err_out;

		cmd = &drbd_cmd_handler[pi.cmd];
		if (unlikely(pi.cmd >= ARRAY_SIZE(drbd_cmd_handler) || !cmd->fn)) {
			drbd_err(connection, "Unexpected data packet %s (0x%04x)",
				 cmdname(pi.cmd), pi.cmd);
			goto err_out;
		}

		shs = cmd->pkt_size;
		if (pi.size > shs && !cmd->expect_payload) {
			drbd_err(connection, "No payload expected %s l:%d\n",
				 cmdname(pi.cmd), pi.size);
			goto err_out;
		}

		if (shs) {
			err = drbd_recv_all_warn(connection, pi.data, shs);
			if (err)
				goto err_out;
			pi.size -= shs;
		}

		start = jiffies;
		err = cmd->fn(connection, &pi);
		if (err) {
			drbd_err(connection, "error receiving %s, e: %d l: %d!\n",
				 cmdname(pi.cmd), err, pi.size);
			goto err_out;
		}
		if (jiffies - start > HZ) {
			drbd_debug(connection, "Request %s took %ums\n",
				   cmdname(pi.cmd), jiffies_to_msecs(jiffies - start));
		}
	}
	return;

    err_out:
	change_cstate(connection, C_PROTOCOL_ERROR, CS_HARD);
}

static void conn_disconnect(struct drbd_connection *connection)
{
	struct drbd_resource *resource = connection->resource;
	struct drbd_peer_device *peer_device;
	enum drbd_conn_state oc;
	unsigned long irq_flags;
	int vnr;

	if (connection->cstate[NOW] == C_STANDALONE)
		return;

	/* We are about to start the cleanup after connection loss.
	 * Make sure drbd_make_request knows about that.
	 * Usually we should be in some network failure state already,
	 * but just in case we are not, we fix it up here.
	 */
	spin_lock_irq(&resource->req_lock);
	del_timer(&connection->connect_timer);
	spin_unlock_irq(&resource->req_lock);

	change_cstate(connection, C_NETWORK_FAILURE, CS_HARD);

	/* asender does not clean up anything. it must not interfere, either */
	drbd_thread_stop(&connection->asender);
	drbd_free_sock(connection);

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		struct drbd_device *device = peer_device->device;
		kobject_get(&device->kobj);
		rcu_read_unlock();
		drbd_disconnected(peer_device);
		kobject_put(&device->kobj);
		rcu_read_lock();
	}
	rcu_read_unlock();

	cleanup_unacked_peer_requests(connection);
	cleanup_peer_ack_list(connection);

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		struct drbd_device *device = peer_device->device;
		int i;

		i = atomic_read(&device->pp_in_use);
		if (i)
			drbd_info(device, "pp_in_use = %d, expected 0\n", i);
	}
	rcu_read_unlock();

	if (!list_empty(&connection->current_epoch->list))
		drbd_err(connection, "ASSERTION FAILED: connection->current_epoch->list not empty\n");
	/* ok, no more ee's on the fly, it is safe to reset the epoch_size */
	atomic_set(&connection->current_epoch->epoch_size, 0);
	connection->send.seen_any_write_yet = false;

	drbd_info(connection, "Connection closed\n");

	if (resource->role[NOW] == R_PRIMARY && conn_highest_pdsk(connection) >= D_UNKNOWN)
		conn_try_outdate_peer_async(connection);

	begin_state_change(resource, &irq_flags, CS_VERBOSE | CS_LOCAL_ONLY);
	oc = connection->cstate[NOW];
	if (oc >= C_UNCONNECTED) {
		__change_cstate(connection, C_UNCONNECTED);
		/* drbd_receiver() has to be restarted after it returns */
		drbd_thread_restart_nowait(&connection->receiver);
	}
	end_state_change(resource, &irq_flags);

	if (oc == C_DISCONNECTING)
		change_cstate(connection, C_STANDALONE, CS_VERBOSE | CS_HARD | CS_LOCAL_ONLY);
}

static int drbd_disconnected(struct drbd_peer_device *peer_device)
{
	struct drbd_device *device = peer_device->device;
	unsigned int i;

	/* wait for current activity to cease. */
	spin_lock_irq(&device->resource->req_lock);
	_drbd_wait_ee_list_empty(device, &device->active_ee);
	_drbd_wait_ee_list_empty(device, &device->sync_ee);
	_drbd_wait_ee_list_empty(device, &device->read_ee);
	spin_unlock_irq(&device->resource->req_lock);

	/* We do not have data structures that would allow us to
	 * get the rs_pending_cnt down to 0 again.
	 *  * On L_SYNC_TARGET we do not have any data structures describing
	 *    the pending RSDataRequest's we have sent.
	 *  * On L_SYNC_SOURCE there is no data structure that tracks
	 *    the P_RS_DATA_REPLY blocks that we sent to the SyncTarget.
	 *  And no, it is not the sum of the reference counts in the
	 *  resync_LRU. The resync_LRU tracks the whole operation including
	 *  the disk-IO, while the rs_pending_cnt only tracks the blocks
	 *  on the fly. */
	drbd_rs_cancel_all(peer_device);
	peer_device->rs_total = 0;
	peer_device->rs_failed = 0;
	atomic_set(&peer_device->rs_pending_cnt, 0);
	wake_up(&device->misc_wait);

	del_timer_sync(&peer_device->resync_timer);
	resync_timer_fn((unsigned long)peer_device);
	del_timer_sync(&peer_device->start_resync_timer);

	/* wait for all w_e_end_data_req, w_e_end_rsdata_req, w_send_barrier,
	 * w_make_resync_request etc. which may still be on the worker queue
	 * to be "canceled" */
	drbd_flush_workqueue(&peer_device->connection->sender_work);

	drbd_finish_peer_reqs(device);

	/* This second workqueue flush is necessary, since drbd_finish_peer_reqs()
	   might have issued a work again. The one before drbd_finish_peer_reqs() is
	   necessary to reclain net_ee in drbd_finish_peer_reqs(). */
	drbd_flush_workqueue(&peer_device->connection->sender_work);

	/* need to do it again, drbd_finish_peer_reqs() may have populated it
	 * again via drbd_try_clear_on_disk_bm(). */
	drbd_rs_cancel_all(peer_device);

	peer_device->uuids_received = false;

	if (!drbd_suspended(device))
		tl_clear(peer_device->connection);

	drbd_md_sync(device);

	/* serialize with bitmap writeout triggered by the state change,
	 * if any. */
	wait_event(device->misc_wait, list_empty(&device->pending_bitmap_work));

	/* tcp_close and release of sendpage pages can be deferred.  I don't
	 * want to use SO_LINGER, because apparently it can be deferred for
	 * more than 20 seconds (longest time I checked).
	 *
	 * Actually we don't care for exactly when the network stack does its
	 * put_page(), but release our reference on these pages right here.
	 */
	i = drbd_free_peer_reqs(device, &device->net_ee);
	if (i)
		drbd_info(device, "net_ee not empty, killed %u entries\n", i);
	i = atomic_read(&device->pp_in_use_by_net);
	if (i)
		drbd_info(device, "pp_in_use_by_net = %d, expected 0\n", i);

	D_ASSERT(device, list_empty(&device->read_ee));
	D_ASSERT(device, list_empty(&device->active_ee));
	D_ASSERT(device, list_empty(&device->sync_ee));
	D_ASSERT(device, list_empty(&device->done_ee));

	return 0;
}

/*
 * We support PRO_VERSION_MIN to PRO_VERSION_MAX. The protocol version
 * we can agree on is stored in agreed_pro_version.
 *
 * feature flags and the reserved array should be enough room for future
 * enhancements of the handshake protocol, and possible plugins...
 *
 * for now, they are expected to be zero, but ignored.
 */
static int drbd_send_features(struct drbd_connection *connection, int peer_node_id)
{
	struct drbd_socket *sock;
	struct p_connection_features *p;

	sock = &connection->data;
	p = conn_prepare_command(connection, sock);
	if (!p)
		return -EIO;
	memset(p, 0, sizeof(*p));
	p->protocol_min = cpu_to_be32(PRO_VERSION_MIN);
	p->protocol_max = cpu_to_be32(PRO_VERSION_MAX);
	p->sender_node_id = cpu_to_be32(connection->resource->res_opts.node_id);
	p->receiver_node_id = cpu_to_be32(peer_node_id);
	p->feature_flags = cpu_to_be32(PRO_FEATURES);
	return send_command(connection, -1, sock, P_CONNECTION_FEATURES, sizeof(*p), NULL, 0);
}

/*
 * return values:
 *   1 yes, we have a valid connection
 *   0 oops, did not work out, please try again
 *  -1 peer talks different language,
 *     no point in trying again, please go standalone.
 */
static int drbd_do_features(struct drbd_connection *connection)
{
	/* ASSERT current == connection->receiver ... */
	struct drbd_resource *resource = connection->resource;
	struct p_connection_features *p;
	const int expect = sizeof(struct p_connection_features);
	struct packet_info pi;
	struct net_conf *nc;
	int peer_node_id = -1, err;

	rcu_read_lock();
	nc = rcu_dereference(connection->net_conf);
	if (nc)
		peer_node_id = nc->peer_node_id;
	rcu_read_unlock();

	err = drbd_send_features(connection, peer_node_id);
	if (err)
		return 0;

	err = drbd_recv_header(connection, &pi);
	if (err)
		return 0;

	if (pi.cmd != P_CONNECTION_FEATURES) {
		drbd_err(connection, "expected ConnectionFeatures packet, received: %s (0x%04x)\n",
			 cmdname(pi.cmd), pi.cmd);
		return -1;
	}

	if (pi.size != expect) {
		drbd_err(connection, "expected ConnectionFeatures length: %u, received: %u\n",
		     expect, pi.size);
		return -1;
	}

	p = pi.data;
	err = drbd_recv_all_warn(connection, p, expect);
	if (err)
		return 0;

	p->protocol_min = be32_to_cpu(p->protocol_min);
	p->protocol_max = be32_to_cpu(p->protocol_max);
	if (p->protocol_max == 0)
		p->protocol_max = p->protocol_min;

	if (PRO_VERSION_MAX < p->protocol_min ||
	    PRO_VERSION_MIN > p->protocol_max) {
		drbd_err(connection, "incompatible DRBD dialects: "
		    "I support %d-%d, peer supports %d-%d\n",
		    PRO_VERSION_MIN, PRO_VERSION_MAX,
		    p->protocol_min, p->protocol_max);
		return -1;
	}

	connection->agreed_pro_version = min_t(int, PRO_VERSION_MAX, p->protocol_max);
	connection->agreed_features = PRO_FEATURES & be32_to_cpu(p->feature_flags);

	if (connection->agreed_pro_version < 110) {
		struct drbd_connection *connection2;

		for_each_connection(connection2, resource) {
			if (connection == connection2)
				continue;
			drbd_err(connection, "Peer supports protocols %d-%d, but "
				 "multiple connections are only supported in protocol "
				 "110 and above\n", p->protocol_min, p->protocol_max);
			return -1;
		}
	}

	if (connection->agreed_pro_version >= 110) {
		if (be32_to_cpu(p->sender_node_id) != peer_node_id) {
			drbd_err(connection, "Peer presented a node_id of %d instead of %d\n",
				 be32_to_cpu(p->sender_node_id), peer_node_id);
			return 0;
		}
		if (be32_to_cpu(p->receiver_node_id) != resource->res_opts.node_id) {
			drbd_err(connection, "Peer expects me to have a node_id of %d instead of %d\n",
				 be32_to_cpu(p->receiver_node_id), resource->res_opts.node_id);
			return 0;
		}
	}

	drbd_info(connection, "Handshake successful: "
	     "Agreed network protocol version %d\n", connection->agreed_pro_version);

	drbd_info(connection, "Agreed to%ssupport TRIM on protocol level\n",
		  connection->agreed_features & FF_TRIM ? " " : " not ");

	return 1;
}

#if !defined(CONFIG_CRYPTO_HMAC) && !defined(CONFIG_CRYPTO_HMAC_MODULE)
static int drbd_do_auth(struct drbd_connection *connection)
{
	drbd_err(connection, "This kernel was build without CONFIG_CRYPTO_HMAC.\n");
	drbd_err(connection, "You need to disable 'cram-hmac-alg' in drbd.conf.\n");
	return -1;
}
#else
#define CHALLENGE_LEN 64 /* must be multiple of 4 */

/* Return value:
	1 - auth succeeded,
	0 - failed, try again (network error),
	-1 - auth failed, don't try again.
*/

static int drbd_do_auth(struct drbd_connection *connection)
{
	struct drbd_socket *sock;
	u32 my_challenge[CHALLENGE_LEN / sizeof(u32) + 1];  /* 68 Bytes... */
	struct scatterlist sg;
	char *response = NULL;
	char *right_response = NULL;
	u32 *peers_ch = NULL;
	unsigned int key_len;
	char secret[SHARED_SECRET_MAX]; /* 64 byte */
	unsigned int resp_size;
	struct hash_desc desc;
	struct packet_info pi;
	struct net_conf *nc;
	int err, rv, peer_node_id;
	bool peer_is_drbd_9 = connection->agreed_pro_version >= 110;

	/* FIXME: Put the challenge/response into the preallocated socket buffer.  */

	rcu_read_lock();
	nc = rcu_dereference(connection->net_conf);
	peer_node_id = nc->peer_node_id;
	key_len = strlen(nc->shared_secret);
	memcpy(secret, nc->shared_secret, key_len);
	rcu_read_unlock();

	desc.tfm = connection->cram_hmac_tfm;
	desc.flags = 0;

	rv = crypto_hash_setkey(connection->cram_hmac_tfm, (u8 *)secret, key_len);
	if (rv) {
		drbd_err(connection, "crypto_hash_setkey() failed with %d\n", rv);
		rv = -1;
		goto fail;
	}

	get_random_bytes(my_challenge, CHALLENGE_LEN);

	sock = &connection->data;
	if (!conn_prepare_command(connection, sock)) {
		rv = 0;
		goto fail;
	}
	rv = !send_command(connection, -1, sock, P_AUTH_CHALLENGE, 0,
			   my_challenge, CHALLENGE_LEN);
	if (!rv)
		goto fail;

	err = drbd_recv_header(connection, &pi);
	if (err) {
		rv = 0;
		goto fail;
	}

	if (pi.cmd != P_AUTH_CHALLENGE) {
		drbd_err(connection, "expected AuthChallenge packet, received: %s (0x%04x)\n",
			 cmdname(pi.cmd), pi.cmd);
		rv = 0;
		goto fail;
	}

	if (pi.size > CHALLENGE_LEN * 2) {
		drbd_err(connection, "expected AuthChallenge payload too big.\n");
		rv = -1;
		goto fail;
	}

	if (pi.size < CHALLENGE_LEN) {
		drbd_err(connection, "AuthChallenge payload too small.\n");
		rv = -1;
		goto fail;
	}

	peers_ch = kmalloc(pi.size + sizeof(u32), GFP_NOIO);
	if (peers_ch == NULL) {
		drbd_err(connection, "kmalloc of peers_ch failed\n");
		rv = -1;
		goto fail;
	}

	err = drbd_recv_all_warn(connection, peers_ch, pi.size);
	if (err) {
		rv = 0;
		goto fail;
	}

	if (!memcmp(my_challenge, peers_ch, CHALLENGE_LEN)) {
		drbd_err(connection, "Peer presented the same challenge!\n");
		rv = -1;
		goto fail;
	}

	resp_size = crypto_hash_digestsize(connection->cram_hmac_tfm);
	response = kmalloc(resp_size, GFP_NOIO);
	if (response == NULL) {
		drbd_err(connection, "kmalloc of response failed\n");
		rv = -1;
		goto fail;
	}

	sg_init_table(&sg, 1);
	if (peer_is_drbd_9)
		peers_ch[pi.size / sizeof(u32)] =
			cpu_to_be32(connection->resource->res_opts.node_id);
	sg_set_buf(&sg, peers_ch, pi.size + peer_is_drbd_9 ? sizeof(u32) : 0);

	rv = crypto_hash_digest(&desc, &sg, sg.length, response);
	if (rv) {
		drbd_err(connection, "crypto_hash_digest() failed with %d\n", rv);
		rv = -1;
		goto fail;
	}

	if (!conn_prepare_command(connection, sock)) {
		rv = 0;
		goto fail;
	}
	rv = !send_command(connection, -1, sock, P_AUTH_RESPONSE, 0,
			   response, resp_size);
	if (!rv)
		goto fail;

	err = drbd_recv_header(connection, &pi);
	if (err) {
		rv = 0;
		goto fail;
	}

	if (pi.cmd != P_AUTH_RESPONSE) {
		drbd_err(connection, "expected AuthResponse packet, received: %s (0x%04x)\n",
			 cmdname(pi.cmd), pi.cmd);
		rv = 0;
		goto fail;
	}

	if (pi.size != resp_size) {
		drbd_err(connection, "expected AuthResponse payload of wrong size\n");
		rv = 0;
		goto fail;
	}

	err = drbd_recv_all_warn(connection, response , resp_size);
	if (err) {
		rv = 0;
		goto fail;
	}

	right_response = kmalloc(resp_size, GFP_NOIO);
	if (right_response == NULL) {
		drbd_err(connection, "kmalloc of right_response failed\n");
		rv = -1;
		goto fail;
	}

	if (peer_is_drbd_9)
		my_challenge[CHALLENGE_LEN / sizeof(u32)] = cpu_to_be32(peer_node_id);
	sg_set_buf(&sg, my_challenge, CHALLENGE_LEN + peer_is_drbd_9 ? sizeof(u32) : 0);

	rv = crypto_hash_digest(&desc, &sg, sg.length, right_response);
	if (rv) {
		drbd_err(connection, "crypto_hash_digest() failed with %d\n", rv);
		rv = -1;
		goto fail;
	}

	rv = !memcmp(response, right_response, resp_size);

	if (rv)
		drbd_info(connection, "Peer authenticated using %d bytes HMAC\n",
		     resp_size);
	else
		rv = -1;

 fail:
	kfree(peers_ch);
	kfree(response);
	kfree(right_response);

	return rv;
}
#endif

int drbd_receiver(struct drbd_thread *thi)
{
	struct drbd_connection *connection = thi->connection;

	if (conn_connect(connection))
		drbdd(connection);
	conn_disconnect(connection);
	return 0;
}

/* ********* acknowledge sender ******** */

static int process_peer_ack_list(struct drbd_connection *connection)
{
	struct drbd_resource *resource = connection->resource;
	struct drbd_request *req;
	unsigned int idx;
	int err;

	rcu_read_lock();
	idx = 1 + connection->net_conf->peer_node_id;
	rcu_read_unlock();

restart:
	spin_lock_irq(&resource->req_lock);
	list_for_each_entry(req, &resource->peer_ack_list, tl_requests) {
		bool destroy;

		if (!(req->rq_state[idx] & RQ_PEER_ACK))
			continue;
		req->rq_state[idx] &= ~RQ_PEER_ACK;
		destroy = atomic_dec_and_test(&req->kref.refcount);
		if (destroy)
			list_del(&req->tl_requests);
		spin_unlock_irq(&resource->req_lock);

		err = drbd_send_peer_ack(connection, req);
		if (destroy)
			mempool_free(req, drbd_request_mempool);
		if (err)
			return err;
		goto restart;

	}
	spin_unlock_irq(&resource->req_lock);
	return 0;
}

static int got_peers_in_sync(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_peer_block_desc *p = pi->data;
	sector_t sector;
	u64 in_sync_b;
	int size;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;

	device = peer_device->device;

	if (get_ldev(device)) {
		sector = be64_to_cpu(p->sector);
		size = be32_to_cpu(p->size);
		in_sync_b = node_ids_to_bitmap(device, be64_to_cpu(p->mask));

		drbd_set_sync(device, sector, size, 0, in_sync_b);
		put_ldev(device);
	}

	return 0;
}

static int got_RqSReply(struct drbd_connection *connection, struct packet_info *pi)
{
	struct p_req_state_reply *p = pi->data;
	int retcode = be32_to_cpu(p->retcode);

	if (retcode >= SS_SUCCESS)
		set_bit(TWOPC_YES, &connection->flags);
	else {
		set_bit(TWOPC_NO, &connection->flags);
		drbd_debug(connection, "Requested state change failed by peer: %s (%d)\n",
			   drbd_set_st_err_str(retcode), retcode);
	}

	wake_up(&connection->resource->state_wait);
	wake_up(&connection->ping_wait);

	return 0;
}

static int got_twopc_reply(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_resource *resource = connection->resource;
	struct p_twopc_reply *p = pi->data;

	spin_lock_irq(&resource->req_lock);
	if (resource->twopc_reply.initiator_node_id == be32_to_cpu(p->initiator_node_id) &&
	    resource->twopc_reply.tid == be32_to_cpu(p->tid)) {
		drbd_debug(connection, "Got a %s reply\n",
			   cmdname(pi->cmd));

		if (pi->cmd == P_TWOPC_YES) {
			u64 reachable_nodes =
				be64_to_cpu(p->reachable_nodes);

			if (resource->res_opts.node_id ==
			    resource->twopc_reply.initiator_node_id &&
			    connection->net_conf->peer_node_id ==
			    resource->twopc_reply.target_node_id) {
				resource->twopc_reply.target_reachable_nodes |=
					reachable_nodes;
				resource->twopc_reply.target_weak_nodes |=
					be64_to_cpu(p->weak_nodes);
			} else {
				resource->twopc_reply.reachable_nodes |=
					reachable_nodes;
				resource->twopc_reply.weak_nodes |=
					be64_to_cpu(p->weak_nodes);
			}
			resource->twopc_reply.primary_nodes |=
				be64_to_cpu(p->primary_nodes);
		}

		if (pi->cmd == P_TWOPC_YES)
			set_bit(TWOPC_YES, &connection->flags);
		else if (pi->cmd == P_TWOPC_NO)
			set_bit(TWOPC_NO, &connection->flags);
		else if (pi->cmd == P_TWOPC_RETRY)
			set_bit(TWOPC_RETRY, &connection->flags);
		if (cluster_wide_reply_ready(resource)) {
			del_timer(&resource->twopc_timer);
			drbd_queue_work(&resource->work,
					&resource->twopc_work);
		}
	} else {
		drbd_debug(connection, "Ignoring %s reply for initiator=%d, tid=%u\n",
			   cmdname(pi->cmd),
			   be32_to_cpu(p->initiator_node_id),
			   be32_to_cpu(p->tid));
	}
	spin_unlock_irq(&resource->req_lock);

	return 0;
}

static int got_Ping(struct drbd_connection *connection, struct packet_info *pi)
{
	return drbd_send_ping_ack(connection);

}

static int got_PingAck(struct drbd_connection *connection, struct packet_info *pi)
{
	if (!test_and_set_bit(GOT_PING_ACK, &connection->flags))
		wake_up(&connection->ping_wait);

	return 0;
}

static int got_IsInSync(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_block_ack *p = pi->data;
	sector_t sector = be64_to_cpu(p->sector);
	int blksize = be32_to_cpu(p->blksize);

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	D_ASSERT(device, peer_device->connection->agreed_pro_version >= 89);

	update_peer_seq(peer_device, be32_to_cpu(p->seq_num));

	if (get_ldev(device)) {
		drbd_rs_complete_io(peer_device, sector);
		drbd_set_in_sync(peer_device, sector, blksize);
		/* rs_same_csums is supposed to count in units of BM_BLOCK_SIZE */
		peer_device->rs_same_csum += (blksize >> BM_BLOCK_SHIFT);
		put_ldev(device);
	}
	dec_rs_pending(peer_device);
	atomic_add(blksize >> 9, &peer_device->rs_sect_in);

	return 0;
}

static int
validate_req_change_req_state(struct drbd_peer_device *peer_device, u64 id, sector_t sector,
			      struct rb_root *root, const char *func,
			      enum drbd_req_event what, bool missing_ok)
{
	struct drbd_device *device = peer_device->device;
	struct drbd_request *req;
	struct bio_and_error m;

	spin_lock_irq(&device->resource->req_lock);
	req = find_request(device, root, id, sector, missing_ok, func);
	if (unlikely(!req)) {
		spin_unlock_irq(&device->resource->req_lock);
		return -EIO;
	}
	__req_mod(req, what, peer_device, &m);
	spin_unlock_irq(&device->resource->req_lock);

	if (m.bio)
		complete_master_bio(device, &m);
	return 0;
}

static int got_BlockAck(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_block_ack *p = pi->data;
	sector_t sector = be64_to_cpu(p->sector);
	int blksize = be32_to_cpu(p->blksize);
	enum drbd_req_event what;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	update_peer_seq(peer_device, be32_to_cpu(p->seq_num));

	if (p->block_id == ID_SYNCER) {
		drbd_set_in_sync(peer_device, sector, blksize);
		dec_rs_pending(peer_device);
		return 0;
	}
	switch (pi->cmd) {
	case P_RS_WRITE_ACK:
		what = WRITE_ACKED_BY_PEER_AND_SIS;
		break;
	case P_WRITE_ACK:
		what = WRITE_ACKED_BY_PEER;
		break;
	case P_RECV_ACK:
		what = RECV_ACKED_BY_PEER;
		break;
	case P_SUPERSEDED:
		what = DISCARD_WRITE;
		break;
	case P_RETRY_WRITE:
		what = POSTPONE_WRITE;
		break;
	default:
		BUG();
	}

	return validate_req_change_req_state(peer_device, p->block_id, sector,
					     &device->write_requests, __func__,
					     what, false);
}

static int got_NegAck(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_block_ack *p = pi->data;
	sector_t sector = be64_to_cpu(p->sector);
	int size = be32_to_cpu(p->blksize);
	int err;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	update_peer_seq(peer_device, be32_to_cpu(p->seq_num));

	if (p->block_id == ID_SYNCER) {
		dec_rs_pending(peer_device);
		drbd_rs_failed_io(peer_device, sector, size);
		return 0;
	}

	err = validate_req_change_req_state(peer_device, p->block_id, sector,
					    &device->write_requests, __func__,
					    NEG_ACKED, true);
	if (err) {
		/* Protocol A has no P_WRITE_ACKs, but has P_NEG_ACKs.
		   The master bio might already be completed, therefore the
		   request is no longer in the collision hash. */
		/* In Protocol B we might already have got a P_RECV_ACK
		   but then get a P_NEG_ACK afterwards. */
		drbd_set_out_of_sync(peer_device, sector, size);
	}
	return 0;
}

static int got_NegDReply(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_block_ack *p = pi->data;
	sector_t sector = be64_to_cpu(p->sector);

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	update_peer_seq(peer_device, be32_to_cpu(p->seq_num));

	drbd_err(device, "Got NegDReply; Sector %llus, len %u.\n",
		 (unsigned long long)sector, be32_to_cpu(p->blksize));

	return validate_req_change_req_state(peer_device, p->block_id, sector,
					     &device->read_requests, __func__,
					     NEG_ACKED, false);
}

static int got_NegRSDReply(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	sector_t sector;
	int size;
	struct p_block_ack *p = pi->data;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	sector = be64_to_cpu(p->sector);
	size = be32_to_cpu(p->blksize);

	update_peer_seq(peer_device, be32_to_cpu(p->seq_num));

	dec_rs_pending(peer_device);

	if (get_ldev_if_state(device, D_FAILED)) {
		drbd_rs_complete_io(peer_device, sector);
		switch (pi->cmd) {
		case P_NEG_RS_DREPLY:
			drbd_rs_failed_io(peer_device, sector, size);
		case P_RS_CANCEL:
			break;
		default:
			BUG();
		}
		put_ldev(device);
	}

	return 0;
}

static int got_BarrierAck(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct p_barrier_ack *p = pi->data;
	int vnr;

	tl_release(connection, p->barrier, be32_to_cpu(p->set_size));

	rcu_read_lock();
	idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
		struct drbd_device *device = peer_device->device;
		if (peer_device->repl_state[NOW] == L_AHEAD &&
		    atomic_read(&connection->ap_in_flight) == 0 &&
		    !test_and_set_bit(AHEAD_TO_SYNC_SOURCE, &device->flags)) {
			peer_device->start_resync_work.side = L_SYNC_SOURCE;
			peer_device->start_resync_timer.expires = jiffies + HZ;
			add_timer(&peer_device->start_resync_timer);
		}
	}
	rcu_read_unlock();

	return 0;
}

static int got_OVResult(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_peer_device *peer_device;
	struct drbd_device *device;
	struct p_block_ack *p = pi->data;
	sector_t sector;
	int size;

	peer_device = conn_peer_device(connection, pi->vnr);
	if (!peer_device)
		return -EIO;
	device = peer_device->device;

	sector = be64_to_cpu(p->sector);
	size = be32_to_cpu(p->blksize);

	update_peer_seq(peer_device, be32_to_cpu(p->seq_num));

	if (be64_to_cpu(p->block_id) == ID_OUT_OF_SYNC)
		drbd_ov_out_of_sync_found(peer_device, sector, size);
	else
		ov_out_of_sync_print(peer_device);

	if (!get_ldev(device))
		return 0;

	drbd_rs_complete_io(peer_device, sector);
	dec_rs_pending(peer_device);

	--peer_device->ov_left;

	/* let's advance progress step marks only for every other megabyte */
	if ((peer_device->ov_left & 0x200) == 0x200)
		drbd_advance_rs_marks(peer_device, peer_device->ov_left);

	if (peer_device->ov_left == 0) {
		struct drbd_peer_device_work *dw = kmalloc(sizeof(*dw), GFP_NOIO);
		if (dw) {
			dw->w.cb = w_ov_finished;
			dw->peer_device = peer_device;
			drbd_queue_work(&peer_device->connection->sender_work, &dw->w);
		} else {
			drbd_err(device, "kmalloc(dw) failed.");
			ov_out_of_sync_print(peer_device);
			drbd_resync_finished(peer_device, D_MASK);
		}
	}
	put_ldev(device);
	return 0;
}

static int got_skip(struct drbd_connection *connection, struct packet_info *pi)
{
	return 0;
}

static u64 node_ids_to_bitmap(struct drbd_device *device, u64 node_ids) __must_hold(local)
{
	char *id_to_bit = device->ldev->id_to_bit;
	u64 bitmap_bits = 0;
	int node_id;

	for_each_set_bit(node_id, (unsigned long *)&node_ids,
			 sizeof(node_ids) * BITS_PER_BYTE) {
		int bitmap_bit = id_to_bit[node_id];
		if (bitmap_bit >= 0)
			bitmap_bits |= NODE_MASK(bitmap_bit);
	}
	return bitmap_bits;
}

static int got_peer_ack(struct drbd_connection *connection, struct packet_info *pi)
{
	struct drbd_resource *resource = connection->resource;
	struct p_peer_ack *p = pi->data;
	u64 dagtag, in_sync;
	struct drbd_peer_request *peer_req, *tmp;
	struct list_head work_list;

	dagtag = be64_to_cpu(p->dagtag);
	in_sync = be64_to_cpu(p->mask);

	spin_lock_irq(&resource->req_lock);
	list_for_each_entry(peer_req, &connection->peer_requests, recv_order) {
		if (dagtag == peer_req->dagtag_sector)
			goto found;
	}
	spin_unlock_irq(&resource->req_lock);

	drbd_err(connection, "peer request with dagtag %llu not found\n", dagtag);
	return -EIO;

found:
	list_cut_position(&work_list, &connection->peer_requests, &peer_req->recv_order);
	spin_unlock_irq(&resource->req_lock);

	list_for_each_entry_safe(peer_req, tmp, &work_list, recv_order) {
		struct drbd_peer_device *peer_device = peer_req->peer_device;
		struct drbd_device *device = peer_device->device;
		u64 in_sync_b;

		if (get_ldev(device)) {
			in_sync_b = node_ids_to_bitmap(device, in_sync);

			drbd_set_sync(device, peer_req->i.sector,
				      peer_req->i.size, ~in_sync_b, -1);
			put_ldev(device);
		}
		list_del(&peer_req->recv_order);
		drbd_al_complete_io(device, &peer_req->i);
		drbd_free_peer_req(device, peer_req);
	}
	return 0;
}

/* Caller has to hold resource->req_lock */
void apply_unacked_peer_requests(struct drbd_connection *connection)
{
	struct drbd_peer_request *peer_req;

	list_for_each_entry(peer_req, &connection->peer_requests, recv_order) {
		struct drbd_peer_device *peer_device = peer_req->peer_device;
		struct drbd_device *device = peer_device->device;
		u64 mask = ~(1 << peer_device->bitmap_index);

		drbd_set_sync(device, peer_req->i.sector, peer_req->i.size,
			      mask, mask);
	}
}

static void cleanup_unacked_peer_requests(struct drbd_connection *connection)
{
	struct drbd_resource *resource = connection->resource;
	struct drbd_peer_request *peer_req, *tmp;
	LIST_HEAD(work_list);

	spin_lock_irq(&resource->req_lock);
	list_splice_init(&connection->peer_requests, &work_list);
	spin_unlock_irq(&resource->req_lock);

	list_for_each_entry_safe(peer_req, tmp, &work_list, recv_order) {
		struct drbd_peer_device *peer_device = peer_req->peer_device;
		struct drbd_device *device = peer_device->device;
		u64 mask = ~(1 << peer_device->bitmap_index);

		drbd_set_sync(device, peer_req->i.sector, peer_req->i.size,
			      mask, mask);

		list_del(&peer_req->recv_order);
		drbd_free_peer_req(device, peer_req);
	}
}

static void destroy_request(struct kref *kref)
{
	struct drbd_request *req =
		container_of(kref, struct drbd_request, kref);

	list_del(&req->tl_requests);
	mempool_free(req, drbd_request_mempool);
}

static void cleanup_peer_ack_list(struct drbd_connection *connection)
{
	struct drbd_resource *resource = connection->resource;
	struct drbd_request *req, *tmp;
	int idx;

	spin_lock_irq(&resource->req_lock);
	idx = 1 + connection->net_conf->peer_node_id;
	list_for_each_entry_safe(req, tmp, &resource->peer_ack_list, tl_requests) {
		if (!(req->rq_state[idx] & RQ_PEER_ACK))
			continue;
		req->rq_state[idx] &= ~RQ_PEER_ACK;
		kref_put(&req->kref, destroy_request);
	}
	spin_unlock_irq(&resource->req_lock);
}

static int connection_finish_peer_reqs(struct drbd_connection *connection)
{
	struct drbd_peer_device *peer_device;
	int vnr, not_empty = 0;

	do {
		clear_bit(SIGNAL_ASENDER, &connection->flags);
		flush_signals(current);

		rcu_read_lock();
		idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
			struct drbd_device *device = peer_device->device;
			kobject_get(&device->kobj);
			rcu_read_unlock();
			if (drbd_finish_peer_reqs(device)) {
				kobject_put(&device->kobj);
				return 1;
			}
			kobject_put(&device->kobj);
			rcu_read_lock();
		}
		set_bit(SIGNAL_ASENDER, &connection->flags);

		spin_lock_irq(&connection->resource->req_lock);
		idr_for_each_entry(&connection->peer_devices, peer_device, vnr) {
			struct drbd_device *device = peer_device->device;
			not_empty = !list_empty(&device->done_ee);
			if (not_empty)
				break;
		}
		spin_unlock_irq(&connection->resource->req_lock);
		rcu_read_unlock();
	} while (not_empty);

	return 0;
}

struct asender_cmd {
	size_t pkt_size;
	int (*fn)(struct drbd_connection *connection, struct packet_info *);
};

static struct asender_cmd asender_tbl[] = {
	[P_PING]	    = { 0, got_Ping },
	[P_PING_ACK]	    = { 0, got_PingAck },
	[P_RECV_ACK]	    = { sizeof(struct p_block_ack), got_BlockAck },
	[P_WRITE_ACK]	    = { sizeof(struct p_block_ack), got_BlockAck },
	[P_RS_WRITE_ACK]    = { sizeof(struct p_block_ack), got_BlockAck },
	[P_SUPERSEDED]      = { sizeof(struct p_block_ack), got_BlockAck },
	[P_NEG_ACK]	    = { sizeof(struct p_block_ack), got_NegAck },
	[P_NEG_DREPLY]	    = { sizeof(struct p_block_ack), got_NegDReply },
	[P_NEG_RS_DREPLY]   = { sizeof(struct p_block_ack), got_NegRSDReply },
	[P_OV_RESULT]	    = { sizeof(struct p_block_ack), got_OVResult },
	[P_BARRIER_ACK]	    = { sizeof(struct p_barrier_ack), got_BarrierAck },
	[P_STATE_CHG_REPLY] = { sizeof(struct p_req_state_reply), got_RqSReply },
	[P_RS_IS_IN_SYNC]   = { sizeof(struct p_block_ack), got_IsInSync },
	[P_DELAY_PROBE]     = { sizeof(struct p_delay_probe93), got_skip },
	[P_RS_CANCEL]       = { sizeof(struct p_block_ack), got_NegRSDReply },
	[P_CONN_ST_CHG_REPLY]={ sizeof(struct p_req_state_reply), got_RqSReply },
	[P_RETRY_WRITE]	    = { sizeof(struct p_block_ack), got_BlockAck },
	[P_PEER_ACK]	    = { sizeof(struct p_peer_ack), got_peer_ack },
	[P_PEERS_IN_SYNC]   = { sizeof(struct p_peer_block_desc), got_peers_in_sync },
	[P_TWOPC_YES]       = { sizeof(struct p_twopc_reply), got_twopc_reply },
	[P_TWOPC_NO]        = { sizeof(struct p_twopc_reply), got_twopc_reply },
	[P_TWOPC_RETRY]     = { sizeof(struct p_twopc_reply), got_twopc_reply },
};

int drbd_asender(struct drbd_thread *thi)
{
	struct drbd_connection *connection = thi->connection;
	struct asender_cmd *cmd = NULL;
	struct packet_info pi;
	int rv;
	void *buf    = connection->meta.rbuf;
	int received = 0;
	unsigned int header_size = drbd_header_size(connection);
	int expect   = header_size;
	bool ping_timeout_active = false;
	struct net_conf *nc;
	int ping_timeo, tcp_cork, ping_int;
	struct sched_param param = { .sched_priority = 2 };

	rv = sched_setscheduler(current, SCHED_RR, &param);
	if (rv < 0)
		drbd_err(connection, "drbd_asender: ERROR set priority, ret=%d\n", rv);

	while (get_t_state(thi) == RUNNING) {
		drbd_thread_current_set_cpu(thi);

		rcu_read_lock();
		nc = rcu_dereference(connection->net_conf);
		ping_timeo = nc->ping_timeo;
		tcp_cork = nc->tcp_cork;
		ping_int = nc->ping_int;
		rcu_read_unlock();

		if (test_and_clear_bit(SEND_PING, &connection->flags)) {
			if (drbd_send_ping(connection)) {
				drbd_err(connection, "drbd_send_ping has failed\n");
				goto reconnect;
			}
			connection->meta.socket->sk->sk_rcvtimeo = ping_timeo * HZ / 10;
			ping_timeout_active = true;
		}

		/* TODO: conditionally cork; it may hurt latency if we cork without
		   much to send */
		if (tcp_cork)
			drbd_tcp_cork(connection->meta.socket);
		if (connection_finish_peer_reqs(connection)) {
			drbd_err(connection, "connection_finish_peer_reqs() failed\n");
			goto reconnect;
		}
		if (process_peer_ack_list(connection))
			goto reconnect;

		/* but unconditionally uncork unless disabled */
		if (tcp_cork)
			drbd_tcp_uncork(connection->meta.socket);

		/* short circuit, recv_msg would return EINTR anyways. */
		if (signal_pending(current))
			continue;

		rv = drbd_recv_short(connection->meta.socket, buf, expect-received, 0);
		clear_bit(SIGNAL_ASENDER, &connection->flags);

		flush_signals(current);

		/* Note:
		 * -EINTR	 (on meta) we got a signal
		 * -EAGAIN	 (on meta) rcvtimeo expired
		 * -ECONNRESET	 other side closed the connection
		 * -ERESTARTSYS  (on data) we got a signal
		 * rv <  0	 other than above: unexpected error!
		 * rv == expected: full header or command
		 * rv <  expected: "woken" by signal during receive
		 * rv == 0	 : "connection shut down by peer"
		 */
		if (likely(rv > 0)) {
			received += rv;
			buf	 += rv;
		} else if (rv == 0) {
			if (test_bit(DISCONNECT_EXPECTED, &connection->flags)) {
				long t;
				rcu_read_lock();
				t = rcu_dereference(connection->net_conf)->ping_timeo * HZ/10;
				rcu_read_unlock();

				t = wait_event_timeout(connection->ping_wait,
						       connection->cstate[NOW] < C_CONNECTED,
						       t);
				if (t)
					break;
			}
			drbd_err(connection, "meta connection shut down by peer.\n");
			goto reconnect;
		} else if (rv == -EAGAIN) {
			/* If the data socket received something meanwhile,
			 * that is good enough: peer is still alive. */
			if (time_after(connection->last_received,
				jiffies - connection->meta.socket->sk->sk_rcvtimeo))
				continue;
			if (ping_timeout_active) {
				drbd_err(connection, "PingAck did not arrive in time.\n");
				goto reconnect;
			}
			set_bit(SEND_PING, &connection->flags);
			continue;
		} else if (rv == -EINTR) {
			continue;
		} else {
			drbd_err(connection, "sock_recvmsg returned %d\n", rv);
			goto reconnect;
		}

		if (received == expect && cmd == NULL) {
			if (decode_header(connection, connection->meta.rbuf, &pi))
				goto reconnect;
			cmd = &asender_tbl[pi.cmd];
			if (pi.cmd >= ARRAY_SIZE(asender_tbl) || !cmd->fn) {
				drbd_err(connection, "Unexpected meta packet %s (0x%04x)\n",
					 cmdname(pi.cmd), pi.cmd);
				goto disconnect;
			}
			expect = header_size + cmd->pkt_size;
			if (pi.size != expect - header_size) {
				drbd_err(connection, "Wrong packet size on meta (c: %d, l: %d)\n",
					pi.cmd, pi.size);
				goto reconnect;
			}
		}
		if (received == expect) {
			bool err;

			err = cmd->fn(connection, &pi);
			if (err) {
				drbd_err(connection, "%pf failed\n", cmd->fn);
				goto reconnect;
			}

			connection->last_received = jiffies;

			if (cmd == &asender_tbl[P_PING_ACK]) {
				/* restore idle timeout */
				connection->meta.socket->sk->sk_rcvtimeo = ping_int * HZ;
				ping_timeout_active = false;
			}

			buf	 = connection->meta.rbuf;
			received = 0;
			expect	 = header_size;
			cmd	 = NULL;
		}
	}

	if (0) {
reconnect:
		change_cstate(connection, C_NETWORK_FAILURE, CS_HARD);
	}
	if (0) {
disconnect:
		change_cstate(connection, C_DISCONNECTING, CS_HARD);
	}
	clear_bit(SIGNAL_ASENDER, &connection->flags);

	drbd_info(connection, "asender terminated\n");

	return 0;
}
