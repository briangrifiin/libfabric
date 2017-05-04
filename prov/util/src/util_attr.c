/*
 * Copyright (c) 2015-2016 Intel Corporation. All rights reserved.
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

#include <stdio.h>

#include <fi_util.h>

static int fi_valid_addr_format(uint32_t prov_format, uint32_t user_format)
{
	if (user_format == FI_FORMAT_UNSPEC)
		return 1;

	switch (prov_format) {
	case FI_SOCKADDR:
		/* Provider supports INET and INET6 */
		return user_format <= FI_SOCKADDR_IN6;
	case FI_SOCKADDR_IN:
		/* Provider supports INET only */
		return user_format <= FI_SOCKADDR_IN;
	case FI_SOCKADDR_IN6:
		/* Provider supports INET6 only */
		return user_format <= FI_SOCKADDR_IN6;
	case FI_SOCKADDR_IB:
		/* Provider must support IB, INET, and INET6 */
		return user_format <= FI_SOCKADDR_IB;
	default:
		return prov_format == user_format;
	}
}

/*
char *ofi_strdup_head(const char *str)
{
	char *delim;
	delim = strchr(str, OFI_NAME_DELIM);
	return delim ? strndup(str, delim - str) : strdup(str);
}

char *ofi_strdup_tail(const char *str)
{
	char *delim;
	delim = strchr(str, OFI_NAME_DELIM);
	return delim ? strup(delim + 1) : strdup(str);
}
*/

char *ofi_strdup_append(const char *head, const char *tail)
{
	char *str;
	size_t len;

	len = strlen(head) + strlen(tail) + 2;
	str = malloc(len);
	if (str)
		sprintf(str, "%s%c%s", head, OFI_NAME_DELIM, tail);
	return str;
}

static int ofi_has_util_prefix(const char *str)
{
	return !strncasecmp(str, OFI_UTIL_PREFIX, strlen(OFI_UTIL_PREFIX));
}

const char *ofi_util_name(const char *str, size_t *len)
{
	char *delim;

	delim = strchr(str, OFI_NAME_DELIM);
	if (delim) {
		if (ofi_has_util_prefix(delim + 1)) {
			*len = strlen(delim + 1);
			return delim + 1;
		} else if (ofi_has_util_prefix(str)) {
			*len = delim - str;
			return str;
		}
	} else if (ofi_has_util_prefix(str)) {
		*len = strlen(str);
		return str;
	}
	*len = 0;
	return NULL;
}

const char *ofi_core_name(const char *str, size_t *len)
{
	char *delim;

	delim = strchr(str, OFI_NAME_DELIM);
	if (delim) {
		if (!ofi_has_util_prefix(delim + 1)) {
			*len = strlen(delim + 1);
			return delim + 1;
		} else if (!ofi_has_util_prefix(str)) {
			*len = delim - str;
			return str;
		}
	} else if (!ofi_has_util_prefix(str)) {
		*len = strlen(str);
		return str;
	}
	*len = 0;
	return NULL;

}

static int ofi_dup_addr(struct fi_info *info, struct fi_info *dup)
{
	dup->addr_format = info->addr_format;
	if (info->src_addr) {
		dup->src_addrlen = info->src_addrlen;
		dup->src_addr = mem_dup(info->src_addr, info->src_addrlen);
		if (dup->src_addr == NULL)
			return -FI_ENOMEM;
	}
	if (info->dest_addr) {
		dup->dest_addrlen = info->dest_addrlen;
		dup->dest_addr = mem_dup(info->dest_addr, info->dest_addrlen);
		if (dup->dest_addr == NULL) {
			free(dup->src_addr);
			dup->src_addr = NULL;
			return -FI_ENOMEM;
		}
	}
	return 0;
}

static int ofi_info_to_core(uint32_t version, const struct fi_provider *prov,
			    struct fi_info *util_info,
			    ofi_alter_info_t info_to_core,
			    struct fi_info **core_hints)
{
	const char *core_name;
	size_t len;

	if (!(*core_hints = fi_allocinfo()))
		return -FI_ENOMEM;

	if (info_to_core(version, util_info, *core_hints))
		goto err;

	if (!util_info)
		return 0;

	if (ofi_dup_addr(util_info, *core_hints))
		goto err;

	if (util_info->fabric_attr) {
		if (util_info->fabric_attr->name) {
			(*core_hints)->fabric_attr->name =
				strdup(util_info->fabric_attr->name);
			if (!(*core_hints)->fabric_attr->name) {
				FI_WARN(prov, FI_LOG_FABRIC,
					"Unable to allocate fabric name\n");
				goto err;
			}
		}

		if (util_info->fabric_attr->prov_name) {
			core_name = ofi_core_name(util_info->fabric_attr->
						  prov_name, &len);
			if (core_name) {
				(*core_hints)->fabric_attr->prov_name =
					strndup(core_name, len);
				if (!(*core_hints)->fabric_attr->prov_name) {
					FI_WARN(prov, FI_LOG_FABRIC,
						"Unable to alloc prov name\n");
					goto err;
				}
			}
		}
	}

	if (util_info->domain_attr && util_info->domain_attr->name) {
		(*core_hints)->domain_attr->name =
			strdup(util_info->domain_attr->name);
		if (!(*core_hints)->domain_attr->name) {
			FI_WARN(prov, FI_LOG_FABRIC,
				"Unable to allocate domain name\n");
			goto err;
		}
	}
	return 0;

err:
	fi_freeinfo(*core_hints);
	return -FI_ENOMEM;
}

static int ofi_info_to_util(uint32_t version, const struct fi_provider *prov,
			    struct fi_info *core_info,
			    ofi_alter_info_t info_to_util,
			    struct fi_info **util_info)
{
	if (!(*util_info = fi_allocinfo()))
		return -FI_ENOMEM;

	if (info_to_util(version, core_info, *util_info))
		goto err;

	if (ofi_dup_addr(core_info, *util_info))
		goto err;

	(*util_info)->domain_attr->name = strdup(core_info->domain_attr->name);
	if (!(*util_info)->domain_attr->name) {
		FI_WARN(prov, FI_LOG_FABRIC,
			"Unable to allocate domain name\n");
		goto err;
	}

	(*util_info)->fabric_attr->name = strdup(core_info->fabric_attr->name);
	if (!(*util_info)->fabric_attr->name) {
		FI_WARN(prov, FI_LOG_FABRIC,
			"Unable to allocate fabric name\n");
		goto err;
	}

	(*util_info)->fabric_attr->prov_name = strdup(core_info->fabric_attr->
						      prov_name);
	if (!(*util_info)->fabric_attr->prov_name) {
		FI_WARN(prov, FI_LOG_FABRIC,
			"Unable to allocate fabric name\n");
		goto err;
	}

	return 0;
err:
	fi_freeinfo(*util_info);
	return -FI_ENOMEM;
}

int ofi_get_core_info(uint32_t version, const char *node, const char *service,
		      uint64_t flags, const struct util_prov *util_prov,
		      struct fi_info *util_hints, ofi_alter_info_t info_to_core,
		      struct fi_info **core_info)
{
	struct fi_info *core_hints = NULL;
	int ret;

	ret = ofi_check_info(util_prov, version, util_hints);
	if (ret)
		return ret;

	ret = ofi_info_to_core(version, util_prov->prov, util_hints, info_to_core,
			       &core_hints);
	if (ret)
		return ret;

	ret = fi_getinfo(version, node, service, flags | OFI_CORE_PROV_ONLY,
			 core_hints, core_info);
	fi_freeinfo(core_hints);
	return ret;
}

int ofix_getinfo(uint32_t version, const char *node, const char *service,
		 uint64_t flags, const struct util_prov *util_prov,
		 struct fi_info *hints, ofi_alter_info_t info_to_core,
		 ofi_alter_info_t info_to_util, struct fi_info **info)
{
	struct fi_info *core_info, *util_info, *cur, *tail;
	int ret;

	ret = ofi_get_core_info(version, node, service, flags, util_prov,
				hints, info_to_core, &core_info);
	if (ret)
		return ret;

	*info = tail = NULL;
	for (cur = core_info; cur; cur = cur->next) {
		ret = ofi_info_to_util(version, util_prov->prov, cur,
				       info_to_util, &util_info);
		if (ret) {
			fi_freeinfo(*info);
			break;
		}

		ofi_alter_info(util_info, hints, version);
		if (!*info)
			*info = util_info;
		else
			tail->next = util_info;
		tail = util_info;
	}
	fi_freeinfo(core_info);
	return ret;
}

int ofi_check_fabric_attr(const struct fi_provider *prov,
			  const struct fi_fabric_attr *prov_attr,
			  const struct fi_fabric_attr *user_attr)
{
	/* Provider names are checked by the framework */

	if (user_attr->prov_version > prov_attr->prov_version) {
		FI_INFO(prov, FI_LOG_CORE, "Unsupported provider version\n");
		return -FI_ENODATA;
	}

	if (FI_VERSION_LT(user_attr->api_version, prov_attr->api_version)) {
		FI_INFO(prov, FI_LOG_CORE, "Unsupported api version\n");
		return -FI_ENODATA;
	}

	return 0;
}

/*
 * Threading models ranked by order of parallelism.
 */
static int fi_thread_level(enum fi_threading thread_model)
{
	switch (thread_model) {
	case FI_THREAD_SAFE:
		return 1;
	case FI_THREAD_FID:
		return 2;
	case FI_THREAD_ENDPOINT:
		return 3;
	case FI_THREAD_COMPLETION:
		return 4;
	case FI_THREAD_DOMAIN:
		return 5;
	case FI_THREAD_UNSPEC:
		return 6;
	default:
		return -1;
	}
}

/*
 * Progress models ranked by order of automation.
 */
static int fi_progress_level(enum fi_progress progress_model)
{
	switch (progress_model) {
	case FI_PROGRESS_AUTO:
		return 1;
	case FI_PROGRESS_MANUAL:
		return 2;
	case FI_PROGRESS_UNSPEC:
		return 3;
	default:
		return -1;
	}
}

/*
 * Resource management models ranked by order of enablement.
 */
static int fi_resource_mgmt_level(enum fi_resource_mgmt rm_model)
{
	switch (rm_model) {
	case FI_RM_ENABLED:
		return 1;
	case FI_RM_DISABLED:
		return 2;
	case FI_RM_UNSPEC:
		return 3;
	default:
		return -1;
	}
}

/* If a provider supports basic registration mode it should set the FI_MR_BASIC
 * mode bit in prov_mode. Support for FI_MR_SCALABLE is indicated by not setting
 * any of OFI_MR_BASIC_MAP bits. */
int ofi_check_mr_mode(uint32_t api_version, uint32_t prov_mode,
		      uint32_t user_mode)
{
	if (FI_VERSION_LT(api_version, FI_VERSION(1, 5))) {
		prov_mode &= ~FI_MR_LOCAL; /* ignore local bit */

		switch (user_mode) {
		case FI_MR_UNSPEC:
			return OFI_CHECK_MR_SCALABLE(prov_mode) ||
				OFI_CHECK_MR_BASIC(prov_mode) ?
				0 : -FI_ENODATA;
		case FI_MR_BASIC:
			return OFI_CHECK_MR_BASIC(prov_mode) ? 0 : -FI_ENODATA;
		case FI_MR_SCALABLE:
			return OFI_CHECK_MR_SCALABLE(prov_mode) ? 0 : -FI_ENODATA;
		default:
			return -FI_ENODATA;
		}
	} else {
		if (user_mode & FI_MR_BASIC) {
			if (!OFI_CHECK_MR_BASIC(prov_mode))
				return -FI_ENODATA;
			if ((user_mode & prov_mode & ~OFI_MR_BASIC_MAP) ==
			    (prov_mode & ~OFI_MR_BASIC_MAP))
				return 0;
			return -FI_ENODATA;
		} else {
			return (((user_mode | FI_MR_BASIC) & prov_mode) == prov_mode) ?
				0 : -FI_ENODATA;
		}
	}
}

int ofi_check_domain_attr(const struct fi_provider *prov, uint32_t api_version,
			  const struct fi_domain_attr *prov_attr,
			  const struct fi_domain_attr *user_attr)
{
	if (prov_attr->name && user_attr->name &&
	    strcasecmp(user_attr->name, prov_attr->name)) {
		FI_INFO(prov, FI_LOG_CORE, "Unknown domain name\n");
		FI_INFO_NAME(prov, prov_attr, user_attr);
		return -FI_ENODATA;
	}

	if (fi_thread_level(user_attr->threading) <
	    fi_thread_level(prov_attr->threading)) {
		FI_INFO(prov, FI_LOG_CORE, "Invalid threading model\n");
		return -FI_ENODATA;
	}

	if (fi_progress_level(user_attr->control_progress) <
	    fi_progress_level(prov_attr->control_progress)) {
		FI_INFO(prov, FI_LOG_CORE, "Invalid control progress model\n");
		return -FI_ENODATA;
	}

	if (fi_progress_level(user_attr->data_progress) <
	    fi_progress_level(prov_attr->data_progress)) {
		FI_INFO(prov, FI_LOG_CORE, "Invalid data progress model\n");
		return -FI_ENODATA;
	}

	if (fi_resource_mgmt_level(user_attr->resource_mgmt) <
	    fi_resource_mgmt_level(prov_attr->resource_mgmt)) {
		FI_INFO(prov, FI_LOG_CORE, "Invalid resource mgmt model\n");
		return -FI_ENODATA;
	}

	if ((prov_attr->av_type != FI_AV_UNSPEC) &&
	    (user_attr->av_type != FI_AV_UNSPEC) &&
	    (prov_attr->av_type != user_attr->av_type)) {
		FI_INFO(prov, FI_LOG_CORE, "Invalid AV type\n");
	   	return -FI_ENODATA;
	}

	if (user_attr->cq_data_size > prov_attr->cq_data_size) {
		FI_INFO(prov, FI_LOG_CORE, "CQ data size too large\n");
		return -FI_ENODATA;
	}

	if (ofi_check_mr_mode(api_version, prov_attr->mr_mode,
			       user_attr->mr_mode)) {
		FI_INFO(prov, FI_LOG_CORE, "Invalid memory registration mode\n");
		FI_INFO_MR_MODE(prov, prov_attr->mr_mode, user_attr->mr_mode);
		return -FI_ENODATA;
	}

	/* following checks only apply to api 1.5 and beyond */
	if (FI_VERSION_LT(api_version, FI_VERSION(1, 5)))
		return 0;

	if (user_attr->cntr_cnt > prov_attr->cntr_cnt) {
		FI_INFO(prov, FI_LOG_CORE, "Cntr count too large\n");
		return -FI_ENODATA;
	}

	if (user_attr->mr_iov_limit > prov_attr->mr_iov_limit) {
		FI_INFO(prov, FI_LOG_CORE, "MR iov limit too large\n");
		return -FI_ENODATA;
	}

	if (user_attr->caps & ~(prov_attr->caps)) {
		FI_INFO(prov, FI_LOG_CORE, "Requested domain caps not supported\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, caps, FI_TYPE_CAPS);
		return -FI_ENODATA;
	}

	if ((user_attr->mode & prov_attr->mode) != prov_attr->mode) {
		FI_INFO(prov, FI_LOG_CORE, "Required domain mode missing\n");
		FI_INFO_MODE(prov, prov_attr->mode, user_attr->mode);
		return -FI_ENODATA;
	}

	return 0;
}

int ofi_check_ep_attr(const struct util_prov *util_prov, uint32_t api_version,
		     const struct fi_ep_attr *user_attr)
{
	const struct fi_provider *prov = util_prov->prov;
	const struct fi_ep_attr *prov_attr = util_prov->info->ep_attr;

	if (user_attr->type && (user_attr->type != prov_attr->type)) {
		FI_INFO(prov, FI_LOG_CORE, "Unsupported endpoint type\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, type, FI_TYPE_EP_TYPE);
		return -FI_ENODATA;
	}

	if (user_attr->protocol && (user_attr->protocol != prov_attr->protocol)) {
		FI_INFO(prov, FI_LOG_CORE, "Unsupported protocol\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, protocol, FI_TYPE_PROTOCOL);
		return -FI_ENODATA;
	}

	if (user_attr->protocol_version &&
	    (user_attr->protocol_version > prov_attr->protocol_version)) {
		FI_INFO(prov, FI_LOG_CORE, "Unsupported protocol version\n");
		return -FI_ENODATA;
	}

	if (user_attr->max_msg_size > prov_attr->max_msg_size) {
		FI_INFO(prov, FI_LOG_CORE, "Max message size too large\n");
		return -FI_ENODATA;
	}

	if (user_attr->tx_ctx_cnt > util_prov->info->domain_attr->max_ep_tx_ctx) {
		if (user_attr->tx_ctx_cnt == FI_SHARED_CONTEXT) {
			if (!(util_prov->flags & UTIL_TX_SHARED_CTX)) {
				FI_INFO(prov, FI_LOG_CORE,
					"Shared tx context not supported\n");
				return -FI_ENODATA;
			}
		} else {
			FI_INFO(prov, FI_LOG_CORE,
				"Requested tx_ctx_cnt exceeds supported."
				" Expected:%d, supported%d\n",
				util_prov->info->domain_attr->max_ep_tx_ctx,
				user_attr->tx_ctx_cnt);
			return -FI_ENODATA;
		}
	}

	if (user_attr->rx_ctx_cnt > util_prov->info->domain_attr->max_ep_rx_ctx) {
		if (user_attr->rx_ctx_cnt == FI_SHARED_CONTEXT) {
			if (!(util_prov->flags & UTIL_RX_SHARED_CTX)) {
				FI_INFO(prov, FI_LOG_CORE,
					"Shared rx context not supported\n");
				return -FI_ENODATA;
			}
		} else {
			FI_INFO(prov, FI_LOG_CORE,
				"Requested rx_ctx_cnt exceeds supported\n");
			return -FI_ENODATA;
		}
	}

	return 0;
}

int ofi_check_rx_attr(const struct fi_provider *prov,
		      const struct fi_rx_attr *prov_attr,
		      const struct fi_rx_attr *user_attr, uint64_t info_mode)
{
	if (user_attr->caps & ~(prov_attr->caps)) {
		FI_INFO(prov, FI_LOG_CORE, "caps not supported\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, caps, FI_TYPE_CAPS);
		return -FI_ENODATA;
	}

	info_mode = user_attr->mode ? user_attr->mode : info_mode;
	if ((info_mode & prov_attr->mode) != prov_attr->mode) {
		FI_INFO(prov, FI_LOG_CORE, "needed mode not set\n");
		FI_INFO_MODE(prov, prov_attr->mode, user_attr->mode);
		return -FI_ENODATA;
	}

	if (prov_attr->op_flags & ~(prov_attr->op_flags)) {
		FI_INFO(prov, FI_LOG_CORE, "op_flags not supported\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, op_flags,
			     FI_TYPE_OP_FLAGS);
		return -FI_ENODATA;
	}

	if (user_attr->msg_order & ~(prov_attr->msg_order)) {
		FI_INFO(prov, FI_LOG_CORE, "msg_order not supported\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, msg_order,
			     FI_TYPE_MSG_ORDER);
		return -FI_ENODATA;
	}

	if (user_attr->comp_order & ~(prov_attr->comp_order)) {
		FI_INFO(prov, FI_LOG_CORE, "comp_order not supported\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, comp_order,
			     FI_TYPE_MSG_ORDER);
		return -FI_ENODATA;
	}

	if (user_attr->total_buffered_recv > prov_attr->total_buffered_recv) {
		FI_INFO(prov, FI_LOG_CORE, "total_buffered_recv too large\n");
		return -FI_ENODATA;
	}

	if (user_attr->size > prov_attr->size) {
		FI_INFO(prov, FI_LOG_CORE, "size is greater than supported\n");
		return -FI_ENODATA;
	}

	if (user_attr->iov_limit > prov_attr->iov_limit) {
		FI_INFO(prov, FI_LOG_CORE, "iov_limit too large\n");
		return -FI_ENODATA;
	}

	return 0;
}

int ofi_check_tx_attr(const struct fi_provider *prov,
		      const struct fi_tx_attr *prov_attr,
		      const struct fi_tx_attr *user_attr, uint64_t info_mode)
{
	if (user_attr->caps & ~(prov_attr->caps)) {
		FI_INFO(prov, FI_LOG_CORE, "caps not supported\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, caps, FI_TYPE_CAPS);
		return -FI_ENODATA;
	}

	info_mode = user_attr->mode ? user_attr->mode : info_mode;
	if ((info_mode & prov_attr->mode) != prov_attr->mode) {
		FI_INFO(prov, FI_LOG_CORE, "needed mode not set\n");
		FI_INFO_MODE(prov, prov_attr->mode, user_attr->mode);
		return -FI_ENODATA;
	}

	if (prov_attr->op_flags & ~(prov_attr->op_flags)) {
		FI_INFO(prov, FI_LOG_CORE, "op_flags not supported\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, op_flags,
			     FI_TYPE_OP_FLAGS);
		return -FI_ENODATA;
	}

	if (user_attr->msg_order & ~(prov_attr->msg_order)) {
		FI_INFO(prov, FI_LOG_CORE, "msg_order not supported\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, msg_order,
			     FI_TYPE_MSG_ORDER);
		return -FI_ENODATA;
	}

	if (user_attr->comp_order & ~(prov_attr->comp_order)) {
		FI_INFO(prov, FI_LOG_CORE, "comp_order not supported\n");
		FI_INFO_CHECK(prov, prov_attr, user_attr, comp_order,
			     FI_TYPE_MSG_ORDER);
		return -FI_ENODATA;
	}

	if (user_attr->inject_size > prov_attr->inject_size) {
		FI_INFO(prov, FI_LOG_CORE, "inject_size too large\n");
		return -FI_ENODATA;
	}

	if (user_attr->size > prov_attr->size) {
		FI_INFO(prov, FI_LOG_CORE, "size is greater than supported\n");
		return -FI_ENODATA;
	}

	if (user_attr->iov_limit > prov_attr->iov_limit) {
		FI_INFO(prov, FI_LOG_CORE, "iov_limit too large\n");
		return -FI_ENODATA;
	}

	if (user_attr->rma_iov_limit > prov_attr->rma_iov_limit) {
		FI_INFO(prov, FI_LOG_CORE, "rma_iov_limit too large\n");
		return -FI_ENODATA;
	}

	return 0;
}

int ofi_check_info(const struct util_prov *util_prov, uint32_t api_version,
		   const struct fi_info *user_info)
{
	const struct fi_info *prov_info = util_prov->info;
	const struct fi_provider *prov = util_prov->prov;
	uint64_t prov_mode;
	int ret;

	if (!user_info)
		return 0;

	if (user_info->caps & ~(prov_info->caps)) {
		FI_INFO(prov, FI_LOG_CORE, "Unsupported capabilities\n");
		FI_INFO_CHECK(prov, prov_info, user_info, caps, FI_TYPE_CAPS);
		return -FI_ENODATA;
	}

	if (FI_VERSION_LT(api_version, FI_VERSION(1, 5)))
		prov_mode = (prov_info->domain_attr->mr_mode & FI_MR_LOCAL) ?
			prov_info->mode | FI_LOCAL_MR : prov_info->mode;
	else
		prov_mode = prov_info->mode;

	if ((user_info->mode & prov_mode) != prov_mode) {
		FI_INFO(prov, FI_LOG_CORE, "needed mode not set\n");
		FI_INFO_MODE(prov, prov_mode, user_info->mode);
		return -FI_ENODATA;
	}

	if (!fi_valid_addr_format(prov_info->addr_format,
				  user_info->addr_format)) {
		FI_INFO(prov, FI_LOG_CORE, "address format not supported\n");
		return -FI_ENODATA;
	}

	if (user_info->fabric_attr) {
		ret = ofi_check_fabric_attr(prov, prov_info->fabric_attr,
					    user_info->fabric_attr);
		if (ret)
			return ret;
	}

	if (user_info->domain_attr) {
		ret = ofi_check_domain_attr(prov, api_version,
					    prov_info->domain_attr,
					    user_info->domain_attr);
		if (ret)
			return ret;
	}

	if (user_info->ep_attr) {
		ret = ofi_check_ep_attr(util_prov, api_version,
					user_info->ep_attr);
		if (ret)
			return ret;
	}

	if (user_info->rx_attr) {
		ret = ofi_check_rx_attr(prov, prov_info->rx_attr,
					user_info->rx_attr, user_info->mode);
		if (ret)
			return ret;
	}

	if (user_info->tx_attr) {
		ret = ofi_check_tx_attr(prov, prov_info->tx_attr,
					user_info->tx_attr, user_info->mode);
		if (ret)
			return ret;
	}

	return 0;
}

static void fi_alter_domain_attr(struct fi_domain_attr *attr,
			     const struct fi_domain_attr *hints,
			     uint64_t info_caps, uint32_t api_version)
{
	if (FI_VERSION_LT(api_version, FI_VERSION(1, 5)))
		attr->mr_mode = attr->mr_mode ? FI_MR_BASIC : FI_MR_SCALABLE;

	if (!hints) {
		attr->caps = (info_caps & attr->caps & FI_PRIMARY_CAPS) |
			     (attr->caps & FI_SECONDARY_CAPS);
		return;
	}

	if (hints->threading)
		attr->threading = hints->threading;
	if (hints->control_progress)
		attr->control_progress = hints->control_progress;
	if (hints->data_progress)
		attr->data_progress = hints->data_progress;
	if (hints->av_type)
		attr->av_type = hints->av_type;
	attr->caps = (hints->caps & FI_PRIMARY_CAPS) |
		     (attr->caps & FI_SECONDARY_CAPS);
}

static void fi_alter_ep_attr(struct fi_ep_attr *attr,
			     const struct fi_ep_attr *hints)
{
	if (!hints)
		return;

	if (hints->tx_ctx_cnt)
		attr->tx_ctx_cnt = hints->tx_ctx_cnt;
	if (hints->rx_ctx_cnt)
		attr->rx_ctx_cnt = hints->rx_ctx_cnt;
}

static void fi_alter_rx_attr(struct fi_rx_attr *attr,
			     const struct fi_rx_attr *hints,
			     uint64_t info_caps)
{
	if (!hints) {
		attr->caps = (info_caps & attr->caps & FI_PRIMARY_CAPS) |
			     (attr->caps & FI_SECONDARY_CAPS);
		return;
	}

	attr->op_flags = hints->op_flags;
	attr->caps = (hints->caps & FI_PRIMARY_CAPS) |
		     (attr->caps & FI_SECONDARY_CAPS);
	attr->total_buffered_recv = hints->total_buffered_recv;
	if (hints->size)
		attr->size = hints->size;
	if (hints->iov_limit)
		attr->iov_limit = hints->iov_limit;
}

static void fi_alter_tx_attr(struct fi_tx_attr *attr,
			     const struct fi_tx_attr *hints,
			     uint64_t info_caps)
{
	if (!hints) {
		attr->caps = (info_caps & attr->caps & FI_PRIMARY_CAPS) |
			     (attr->caps & FI_SECONDARY_CAPS);
		return;
	}

	attr->op_flags = hints->op_flags;
	attr->caps = (hints->caps & FI_PRIMARY_CAPS) |
		     (attr->caps & FI_SECONDARY_CAPS);
	if (hints->inject_size)
		attr->inject_size = hints->inject_size;
	if (hints->size)
		attr->size = hints->size;
	if (hints->iov_limit)
		attr->iov_limit = hints->iov_limit;
	if (hints->rma_iov_limit)
		attr->rma_iov_limit = hints->rma_iov_limit;
}

/*
 * Alter the returned fi_info based on the user hints.  We assume that
 * the hints have been validated and the starting fi_info is properly
 * configured by the provider.
 */
void ofi_alter_info(struct fi_info *info, const struct fi_info *hints,
		    uint32_t api_version)
{
	if (!hints)
		return;

	for (; info; info = info->next) {
		info->caps = (hints->caps & FI_PRIMARY_CAPS) |
			     (info->caps & FI_SECONDARY_CAPS);

		if (FI_VERSION_LT(api_version, FI_VERSION(1, 5))) {
			if (info->domain_attr->mr_mode & FI_MR_LOCAL)
				info->mode |= FI_LOCAL_MR;
		}

		info->handle = hints->handle;

		fi_alter_domain_attr(info->domain_attr, hints->domain_attr,
				     info->caps, api_version);
		fi_alter_ep_attr(info->ep_attr, hints->ep_attr);
		fi_alter_rx_attr(info->rx_attr, hints->rx_attr, info->caps);
		fi_alter_tx_attr(info->tx_attr, hints->tx_attr, info->caps);
	}
}
