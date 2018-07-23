/*
 * Copyright (c) 2016 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>

#include <ofi.h>
#include <ofi_util.h>
#include "rxm.h"

static int rxm_msg_ep_open(struct rxm_ep *rxm_ep, struct fi_info *msg_info,
			   struct rxm_conn *rxm_conn, void *context)
{
	struct rxm_domain *rxm_domain;
	struct fid_ep *msg_ep;
	int ret;

	rxm_domain = container_of(rxm_ep->util_ep.domain, struct rxm_domain,
			util_domain);
	ret = fi_endpoint(rxm_domain->msg_domain, msg_info, &msg_ep, context);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to create msg_ep\n");
		return ret;
	}

	ret = fi_ep_bind(msg_ep, &rxm_ep->msg_eq->fid, 0);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to bind msg EP to EQ\n");
		goto err;
	}

	if (rxm_ep->srx_ctx) {
		ret = fi_ep_bind(msg_ep, &rxm_ep->srx_ctx->fid, 0);
		if (ret) {
			FI_WARN(&rxm_prov, FI_LOG_FABRIC,
				"Unable to bind msg EP to shared RX ctx\n");
			goto err;
		}
	}

	// TODO add other completion flags
	ret = fi_ep_bind(msg_ep, &rxm_ep->msg_cq->fid, FI_TRANSMIT | FI_RECV);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
				"Unable to bind msg_ep to msg_cq\n");
		goto err;
	}

	ret = fi_enable(msg_ep);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to enable msg_ep\n");
		goto err;
	}

	if (!rxm_ep->srx_ctx) {
		ret = rxm_ep_prepost_buf(rxm_ep, msg_ep, &rxm_conn->posted_rx_list);
		if (ret)
			goto err;
	}

	rxm_conn->msg_ep = msg_ep;
	return 0;
err:
	fi_close(&msg_ep->fid);
	return ret;
}

static void rxm_txe_init(struct rxm_tx_entry *entry, void *arg)
{
	struct rxm_send_queue *send_queue = arg;
	entry->conn 	= send_queue->rxm_conn;
	entry->ep 	= send_queue->rxm_ep;
}

static int
rxm_send_queue_init(struct rxm_ep *rxm_ep, struct rxm_conn *rxm_conn,
		    struct rxm_send_queue *send_queue, size_t size)
{
	send_queue->rxm_conn = rxm_conn;
	send_queue->rxm_ep = rxm_ep;
	send_queue->fs = rxm_txe_fs_create(size, rxm_txe_init, send_queue);
	if (!send_queue->fs)
		return -FI_ENOMEM;

	fastlock_init(&send_queue->lock);
	return 0;
}

static void rxm_send_queue_close(struct rxm_send_queue *send_queue)
{
	if (send_queue->fs) {
		struct rxm_tx_entry *tx_entry;
		ssize_t i;

		for (i = send_queue->fs->size - 1; i >= 0; i--) {
			tx_entry = &send_queue->fs->entry[i].buf;
			if (tx_entry->tx_buf) {
				rxm_tx_buf_release(tx_entry->ep, tx_entry->tx_buf);
				tx_entry->tx_buf = NULL;
			}
		}
		rxm_txe_fs_free(send_queue->fs);
	}
	fastlock_destroy(&send_queue->lock);
}

static void rxm_conn_close(struct util_cmap_handle *handle)
{
	struct rxm_conn *rxm_conn = container_of(handle, struct rxm_conn, handle);
	struct rxm_ep *rxm_ep = container_of(handle->cmap->ep, struct rxm_ep, util_ep);

	if (!rxm_conn->msg_ep)
		return;
	rxm_ep_cleanup_posted_rx_list(rxm_ep, &rxm_conn->posted_rx_list);
	fi_close(&rxm_conn->msg_ep->fid);
	FI_DBG(&rxm_prov, FI_LOG_EP_CTRL,
	       "Saved MSG EP fid for further deletion in main thread\n");
	rxm_conn->msg_ep = NULL;
}

static void rxm_conn_free(struct util_cmap_handle *handle)
{
	struct rxm_ep *rxm_ep = container_of(handle->cmap->ep, struct rxm_ep, util_ep);
	struct rxm_conn *rxm_conn = container_of(handle, struct rxm_conn, handle);

	if (!rxm_conn->msg_ep)
		return;
	rxm_ep_cleanup_posted_rx_list(rxm_ep, &rxm_conn->posted_rx_list);
	/* Assuming fi_close also shuts down the connection gracefully if the
	 * endpoint is in connected state */
	if (fi_close(&rxm_conn->msg_ep->fid))
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to close msg_ep\n");
	FI_DBG(&rxm_prov, FI_LOG_EP_CTRL, "Closed msg_ep\n");
	rxm_conn->msg_ep = NULL;
	rxm_send_queue_close(&rxm_conn->send_queue);

	free(container_of(handle, struct rxm_conn, handle));
}

static void rxm_conn_connected_handler(struct util_cmap_handle *handle)
{
}

static int rxm_conn_reprocess_directed_recvs(struct rxm_recv_queue *recv_queue)
{
	struct rxm_rx_buf *rx_buf;
	struct dlist_entry *entry, *tmp_entry;
	struct rxm_recv_match_attr match_attr;
	struct dlist_entry rx_buf_list;
	struct fi_cq_err_entry err_entry = {0};
	int ret, count = 0;

	dlist_init(&rx_buf_list);

	recv_queue->rxm_ep->res_fastlock_acquire(&recv_queue->lock);

	dlist_foreach_container_safe(&recv_queue->unexp_msg_list,
				     struct rxm_rx_buf, rx_buf,
				     unexp_msg.entry, tmp_entry) {
		if (rx_buf->unexp_msg.addr == rx_buf->conn->handle.fi_addr)
			continue;

		assert(rx_buf->unexp_msg.addr == FI_ADDR_NOTAVAIL);

		match_attr.addr = rx_buf->unexp_msg.addr =
			rx_buf->conn->handle.fi_addr;
		match_attr.tag = rx_buf->unexp_msg.tag;

		entry = dlist_remove_first_match(&recv_queue->recv_list,
						 recv_queue->match_recv,
						 &match_attr);
		if (!entry)
			continue;

		dlist_remove(&rx_buf->unexp_msg.entry);
		rx_buf->recv_entry = container_of(entry, struct rxm_recv_entry,
						  entry);
		dlist_insert_tail(&rx_buf->unexp_msg.entry, &rx_buf_list);
	}
	recv_queue->rxm_ep->res_fastlock_release(&recv_queue->lock);

	while (!dlist_empty(&rx_buf_list)) {
		dlist_pop_front(&rx_buf_list, struct rxm_rx_buf,
				rx_buf, unexp_msg.entry);
		ret = rxm_cq_handle_rx_buf(rx_buf);
		if (ret) {
			err_entry.op_context = rx_buf;
			err_entry.flags = rx_buf->recv_entry->comp_flags;
			err_entry.len = rx_buf->pkt.hdr.size;
			err_entry.data = rx_buf->pkt.hdr.data;
			err_entry.tag = rx_buf->pkt.hdr.tag;
			err_entry.err = ret;
			err_entry.prov_errno = ret;
			ofi_cq_write_error(recv_queue->rxm_ep->util_ep.rx_cq,
					   &err_entry);
			if (rx_buf->ep->util_ep.flags & OFI_CNTR_ENABLED)
				rxm_cntr_incerr(rx_buf->ep->util_ep.rx_cntr);

			rx_buf->ep->res_fastlock_acquire(&rx_buf->ep->util_ep.lock);
			dlist_insert_tail(&rx_buf->repost_entry,
					  &rx_buf->ep->repost_ready_list);
			rx_buf->ep->res_fastlock_release(&rx_buf->ep->util_ep.lock);

			if (!(rx_buf->recv_entry->flags & FI_MULTI_RECV))
				rxm_recv_entry_release(recv_queue,
						       rx_buf->recv_entry);
		}
		count++;
	}
	return count;
}

static int rxm_conn_reprocess_recv_queues(struct rxm_ep *rxm_ep)
{
	int count = 0;

	if (rxm_ep->rxm_info->caps & FI_DIRECTED_RECV) {
		fastlock_acquire(&rxm_ep->util_ep.cmap->lock);

		if (!rxm_ep->util_ep.cmap->av_updated)
			goto unlock;

		rxm_ep->util_ep.cmap->av_updated = 0;

		count += rxm_conn_reprocess_directed_recvs(&rxm_ep->recv_queue);
		count += rxm_conn_reprocess_directed_recvs(&rxm_ep->trecv_queue);
unlock:
		fastlock_release(&rxm_ep->util_ep.cmap->lock);
	}
	return count;
}

static void
rxm_conn_av_updated_handler(struct util_cmap_handle *handle)
{
	struct rxm_ep *rxm_ep = container_of(handle->cmap->ep, struct rxm_ep, util_ep);

	rxm_conn_reprocess_recv_queues(rxm_ep);
}

static struct util_cmap_handle *rxm_conn_alloc(struct util_cmap *cmap)
{
	int ret;
	struct rxm_ep *rxm_ep = container_of(cmap->ep, struct rxm_ep, util_ep);
	struct rxm_conn *rxm_conn = calloc(1, sizeof(*rxm_conn));
	if (OFI_UNLIKELY(!rxm_conn))
		return NULL;
	ret = rxm_send_queue_init(rxm_ep, rxm_conn, &rxm_conn->send_queue,
				  rxm_ep->rxm_info->tx_attr->size);
	if (ret) {
		free(rxm_conn);
		return NULL;
	}
	dlist_init(&rxm_conn->posted_rx_list);
	dlist_init(&rxm_conn->sar_rx_msg_list);
	dlist_init(&rxm_conn->deferred_op_list);
	return &rxm_conn->handle;
}

static inline int
rxm_conn_verify_cm_data(struct rxm_cm_data *remote_cm_data,
			struct rxm_cm_data *local_cm_data)
{
	if (remote_cm_data->proto.endianness != local_cm_data->proto.endianness) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
			"endianness of two peers (%"PRIu8" vs %"PRIu8")"
			"are mismatched\n",
			remote_cm_data->proto.endianness,
			local_cm_data->proto.endianness);
		goto err;
	}
	if (remote_cm_data->proto.ctrl_version != local_cm_data->proto.ctrl_version) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
			"ctrl_version of two peers (%"PRIu8" vs %"PRIu8")"
			"are mismatched\n",
			remote_cm_data->proto.ctrl_version,
			local_cm_data->proto.ctrl_version);
		goto err;
	}
	if (remote_cm_data->proto.op_version != local_cm_data->proto.op_version) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
			"op_version of two peers (%"PRIu8" vs %"PRIu8")"
			"are mismatched\n",
			remote_cm_data->proto.op_version,
			local_cm_data->proto.op_version);
		goto err;
	}
	if (remote_cm_data->proto.eager_size != local_cm_data->proto.eager_size) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
			"inject_size of two peers (%"PRIu64" vs %"PRIu64")"
			"are mismatched\n",
			remote_cm_data->proto.eager_size,
			local_cm_data->proto.eager_size);
		goto err;
	}
	return FI_SUCCESS;
err:
	return -FI_EINVAL;
}

static int
rxm_msg_process_connreq(struct rxm_ep *rxm_ep, struct fi_info *msg_info,
			void *data)
{
	struct rxm_conn *rxm_conn;
	struct rxm_cm_data *remote_cm_data = data;
	struct rxm_cm_data cm_data = {
		.proto = {
			.ctrl_version = RXM_CTRL_VERSION,
			.op_version = RXM_OP_VERSION,
			.endianness = ofi_detect_endianness(),
			.eager_size = rxm_ep->eager_size,
		},
	};
	struct util_cmap_handle *handle;
	int ret;

	remote_cm_data->proto.eager_size = ntohll(remote_cm_data->proto.eager_size);

	if (rxm_conn_verify_cm_data(remote_cm_data, &cm_data)) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
			"CM data mismatch was detected\n");
		ret = -FI_EINVAL;
		goto err1;
	}

	ret = ofi_cmap_process_connreq(rxm_ep->util_ep.cmap,
				       &remote_cm_data->name, &handle);
	if (ret)
		goto err1;

	rxm_conn = container_of(handle, struct rxm_conn, handle);

	rxm_conn->handle.remote_key = remote_cm_data->conn_id;

	ret = rxm_msg_ep_open(rxm_ep, msg_info, rxm_conn, handle);
	if (ret)
		goto err2;

	cm_data.conn_id = rxm_conn->handle.key;
	cm_data.proto.eager_size = htonll(cm_data.proto.eager_size);

	ret = fi_accept(rxm_conn->msg_ep, &cm_data, sizeof(cm_data));
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC,
				"Unable to accept incoming connection\n");
		goto err2;
	}
	return ret;
err2:
	ofi_cmap_del_handle(&rxm_conn->handle);
err1:
	FI_DBG(&rxm_prov, FI_LOG_EP_CTRL,
		"Rejecting incoming connection request\n");
	if (fi_reject(rxm_ep->msg_pep, msg_info->handle, NULL, 0))
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
				"Unable to reject incoming connection\n");
	return ret;
}

static int rxm_conn_handle_notify(struct fi_eq_entry *eq_entry)
{
	switch((enum ofi_cmap_signal)eq_entry->data) {
	case OFI_CMAP_FREE:
		FI_DBG(&rxm_prov, FI_LOG_FABRIC, "Freeing handle\n");
		rxm_conn_free((struct util_cmap_handle *)eq_entry->context);
		return 0;
	case OFI_CMAP_EXIT:
		FI_TRACE(&rxm_prov, FI_LOG_FABRIC, "Closing event handler\n");
		return 1;
	default:
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unknown cmap signal\n");
		return 1;
	}
}

static void rxm_conn_handle_eq_err(struct rxm_ep *rxm_ep, ssize_t rd)
{
	struct fi_eq_err_entry err_entry = {0};

	if (rd != -FI_EAVAIL) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to fi_eq_sread\n");
		return;
	}
	OFI_EQ_READERR(&rxm_prov, FI_LOG_FABRIC, rxm_ep->msg_eq, rd, err_entry);
	if (err_entry.err == ECONNREFUSED) {
		FI_DBG(&rxm_prov, FI_LOG_FABRIC, "Connection refused\n");
		ofi_cmap_process_reject(rxm_ep->util_ep.cmap,
					err_entry.fid->context);
	}
}

static void rxm_conn_wake_up_wait_obj(struct rxm_ep *rxm_ep)
{
	if (rxm_ep->util_ep.tx_cq->wait)
		util_cq_signal(rxm_ep->util_ep.tx_cq);
	if (rxm_ep->util_ep.tx_cntr && rxm_ep->util_ep.tx_cntr->wait)
		util_cntr_signal(rxm_ep->util_ep.tx_cntr);
}

static void *rxm_conn_event_handler(void *arg)
{
	struct fi_eq_cm_entry *entry;
	size_t datalen = sizeof(struct rxm_cm_data);
	size_t len = sizeof(*entry) + datalen;
	struct rxm_ep *rxm_ep = container_of(arg, struct rxm_ep, util_ep);
	struct rxm_cm_data *cm_data;
	uint32_t event;
	ssize_t rd;

	entry = calloc(1, len);
	if (!entry) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to allocate memory!\n");
		return NULL;
	}

	FI_DBG(&rxm_prov, FI_LOG_FABRIC, "Starting conn event handler\n");
	while (1) {
		rd = fi_eq_sread(rxm_ep->msg_eq, &event, entry, len, -1, 0);
		/* We would receive more bytes than sizeof *entry during CONNREQ */
		if (rd < 0) {
			rxm_conn_handle_eq_err(rxm_ep, rd);
			continue;
		}

		switch(event) {
		case FI_NOTIFY:
			if (rxm_conn_handle_notify((struct fi_eq_entry *)entry))
				goto exit;
			break;
		case FI_CONNREQ:
			FI_DBG(&rxm_prov, FI_LOG_FABRIC, "Got new connection\n");
			if ((size_t)rd != len) {
				FI_WARN(&rxm_prov, FI_LOG_FABRIC,
					"Received size (%zd) not matching "
					"expected (%zu)\n", rd, len);
				goto exit;
			}
			rxm_msg_process_connreq(rxm_ep, entry->info, entry->data);
			fi_freeinfo(entry->info);
			break;
		case FI_CONNECTED:
			FI_DBG(&rxm_prov, FI_LOG_FABRIC,
			       "Connection successful\n");
			fastlock_acquire(&rxm_ep->util_ep.cmap->lock);
			cm_data = (void *)entry->data;
			ofi_cmap_process_connect(rxm_ep->util_ep.cmap,
						 entry->fid->context,
						 ((rd - sizeof(*entry)) ?
						  &cm_data->conn_id : NULL));
			rxm_conn_wake_up_wait_obj(rxm_ep);
			fastlock_release(&rxm_ep->util_ep.cmap->lock);
			break;
		case FI_SHUTDOWN:
			FI_DBG(&rxm_prov, FI_LOG_FABRIC,
			       "Received connection shutdown\n");
			ofi_cmap_process_shutdown(rxm_ep->util_ep.cmap,
						  entry->fid->context);
			break;
		default:
			FI_WARN(&rxm_prov, FI_LOG_FABRIC,
				"Unknown event: %u\n", event);
			goto exit;
		}
	}
exit:
	free(entry);
	return NULL;
}

static int rxm_prepare_cm_data(struct fid_pep *pep, struct util_cmap_handle *handle,
		struct rxm_cm_data *cm_data)
{
	size_t cm_data_size = 0;
	size_t name_size = sizeof(cm_data->name);
	size_t opt_size = sizeof(cm_data_size);
	int ret;

	ret = fi_getopt(&pep->fid, FI_OPT_ENDPOINT, FI_OPT_CM_DATA_SIZE,
			&cm_data_size, &opt_size);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "fi_getopt failed\n");
		return ret;
	}

	if (cm_data_size < sizeof(*cm_data)) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "MSG EP CM data size too small\n");
		return -FI_EOTHER;
	}

	ret = fi_getname(&pep->fid, &cm_data->name, &name_size);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to get msg pep name\n");
		return ret;
	}

	cm_data->conn_id = handle->key;
	return 0;
}

static int
rxm_conn_connect(struct util_ep *util_ep, struct util_cmap_handle *handle,
		 const void *addr, size_t addrlen)
{
	struct fi_info *msg_info;
	int ret;
	struct rxm_ep *rxm_ep =
		container_of(util_ep, struct rxm_ep, util_ep);
	struct rxm_conn *rxm_conn =
		container_of(handle, struct rxm_conn, handle);
	struct rxm_cm_data cm_data = {
		.proto = {
			.ctrl_version = RXM_CTRL_VERSION,
			.op_version = RXM_OP_VERSION,
			.endianness = ofi_detect_endianness(),
			.eager_size = rxm_ep->eager_size,
		},
	};

	free(rxm_ep->msg_info->dest_addr);
	rxm_ep->msg_info->dest_addrlen = addrlen;

	rxm_ep->msg_info->dest_addr = mem_dup(addr, rxm_ep->msg_info->dest_addrlen);
	if (!rxm_ep->msg_info->dest_addr)
		return -FI_ENOMEM;

	ret = fi_getinfo(rxm_ep->util_ep.domain->fabric->fabric_fid.api_version,
			 NULL, NULL, 0, rxm_ep->msg_info, &msg_info);
	if (ret)
		return ret;

	ret = rxm_msg_ep_open(rxm_ep, msg_info, rxm_conn, &rxm_conn->handle);
	if (ret)
		goto err1;

	/* We have to send passive endpoint's address to the server since the
	 * address from which connection request would be sent would have a
	 * different port. */
	ret = rxm_prepare_cm_data(rxm_ep->msg_pep, &rxm_conn->handle, &cm_data);
	if (ret)
		goto err2;

	cm_data.proto.eager_size = htonll(cm_data.proto.eager_size);

	ret = fi_connect(rxm_conn->msg_ep, msg_info->dest_addr, &cm_data, sizeof(cm_data));
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to connect msg_ep\n");
		goto err2;
	}
	fi_freeinfo(msg_info);
	return 0;
err2:
	fi_close(&rxm_conn->msg_ep->fid);
	rxm_conn->msg_ep = NULL;
err1:
	fi_freeinfo(msg_info);
	return ret;
}

static int rxm_conn_signal(struct util_ep *util_ep, void *context,
			   enum ofi_cmap_signal signal)
{
	struct rxm_ep *rxm_ep = container_of(util_ep, struct rxm_ep, util_ep);
	struct fi_eq_entry entry = {0};
	ssize_t rd;

	entry.context = context;
	entry.data = (uint64_t)signal;

	rd = fi_eq_write(rxm_ep->msg_eq, FI_NOTIFY, &entry, sizeof(entry), 0);
	if (rd != sizeof(entry)) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to signal\n");
		return (int)rd;
	}
	return 0;
}

struct util_cmap *rxm_conn_cmap_alloc(struct rxm_ep *rxm_ep)
{
	struct util_cmap_attr attr;
	struct util_cmap *cmap = NULL;
	void *name;
	size_t len;
	int ret;

	len = rxm_ep->msg_info->src_addrlen;
	name = calloc(1, len);
	if (!name) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
			"Unable to allocate memory for EP name\n");
		return NULL;
	}

	/* Passive endpoint should already have fi_setname or fi_listen
	 * called on it for this to work */
	ret = fi_getname(&rxm_ep->msg_pep->fid, name, &len);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
			"Unable to fi_getname on msg_ep\n");
		goto fn;
	}
	ofi_straddr_dbg(&rxm_prov, FI_LOG_EP_CTRL, "local_name", name);

	attr.name		= name;
	attr.alloc 		= rxm_conn_alloc;
	attr.close 		= rxm_conn_close;
	attr.free 		= rxm_conn_free;
	attr.connect 		= rxm_conn_connect;
	attr.connected_handler	= rxm_conn_connected_handler;
	attr.event_handler	= rxm_conn_event_handler;
	attr.signal		= rxm_conn_signal;
	attr.av_updated_handler	= rxm_conn_av_updated_handler;

	cmap = ofi_cmap_alloc(&rxm_ep->util_ep, &attr);
	if (!cmap)
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
			"Unable to allocate CMAP\n");
fn:
	free(name);
	return cmap;
}
