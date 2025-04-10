/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2021. ALL RIGHTS RESERVED.
* Copyright (C) Huawei Technologies Co., Ltd. 2021.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ud_iface.h"
#include "ud_ep.h"
#include "ud_inl.h"

#include <ucs/arch/cpu.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/debug/log.h>
#include <ucs/type/class.h>
#include <ucs/datastruct/queue.h>
#include <ucs/vfs/base/vfs_obj.h>
#include <ucs/vfs/base/vfs_cb.h>
#include <sys/poll.h>


#define UCT_UD_IFACE_CEP_CONN_SN_MAX ((uct_ud_ep_conn_sn_t)-1)


#ifdef ENABLE_STATS
static ucs_stats_class_t uct_ud_iface_stats_class = {
    .name          = "ud_iface",
    .num_counters  = UCT_UD_IFACE_STAT_LAST,
    .class_id      = UCS_STATS_CLASS_ID_INVALID,
    .counter_names = {
        [UCT_UD_IFACE_STAT_RX_DROP] = "rx_drop"
    }
};
#endif


static void uct_ud_iface_free_pending_rx(uct_ud_iface_t *iface);
static void uct_ud_iface_free_async_comps(uct_ud_iface_t *iface);

ucs_status_t
uct_ud_iface_cep_get_peer_address(uct_ud_iface_t *iface,
                                  const uct_ib_address_t *ib_addr,
                                  const uct_ud_iface_addr_t *if_addr,
                                  int path_index, void *address_p)
{
    ucs_status_t status = uct_ud_iface_unpack_peer_address(iface, ib_addr,
                                                           if_addr, path_index,
                                                           address_p);

    if (status != UCS_OK) {
        ucs_diag("iface %p: failed to get peer address", iface);
    }

    return status;
}

static UCS_F_ALWAYS_INLINE ucs_conn_match_queue_type_t
uct_ud_iface_cep_ep_queue_type(uct_ud_ep_t *ep)
{
    return (ep->flags & UCT_UD_EP_FLAG_PRIVATE) ?
           UCS_CONN_MATCH_QUEUE_UNEXP :
           UCS_CONN_MATCH_QUEUE_EXP;
}

ucs_status_t
uct_ud_iface_cep_get_conn_sn(uct_ud_iface_t *iface,
                             const uct_ib_address_t *ib_addr,
                             const uct_ud_iface_addr_t *if_addr,
                             int path_index, uct_ud_ep_conn_sn_t *conn_sn_p)
{
    void *peer_address = ucs_alloca(iface->conn_match_ctx.address_length);
    ucs_status_t status;
    
    status = uct_ud_iface_cep_get_peer_address(iface, ib_addr, if_addr,
                                               path_index, peer_address);
    if (status != UCS_OK) {
        return status;
    } 

    *conn_sn_p = ucs_conn_match_get_next_sn(&iface->conn_match_ctx,
                                            peer_address);
    return UCS_OK;
}

ucs_status_t
uct_ud_iface_cep_insert_ep(uct_ud_iface_t *iface,
                           const uct_ib_address_t *ib_addr,
                           const uct_ud_iface_addr_t *if_addr,
                           int path_index, uct_ud_ep_conn_sn_t conn_sn,
                           uct_ud_ep_t *ep)
{
    ucs_conn_match_queue_type_t queue_type;
    ucs_status_t status;
    void *peer_address;
    int ret;

    queue_type   = uct_ud_iface_cep_ep_queue_type(ep);
    peer_address = ucs_alloca(iface->conn_match_ctx.address_length);
    status       = uct_ud_iface_cep_get_peer_address(iface, ib_addr, if_addr,
                                                     path_index, peer_address);
    if (status != UCS_OK) {
        return status;
    }

    ucs_assert(!(ep->flags & UCT_UD_EP_FLAG_ON_CEP));
    ret = ucs_conn_match_insert(&iface->conn_match_ctx, peer_address,
                                conn_sn, &ep->conn_match, queue_type);
    ucs_assert_always(ret == 1);

    ep->flags |= UCT_UD_EP_FLAG_ON_CEP;
    return UCS_OK;
}

uct_ud_ep_t *uct_ud_iface_cep_get_ep(uct_ud_iface_t *iface,
                                     const uct_ib_address_t *ib_addr,
                                     const uct_ud_iface_addr_t *if_addr,
                                     int path_index,
                                     uct_ud_ep_conn_sn_t conn_sn,
                                     int is_private)
{
    uct_ud_ep_t *ep                        = NULL;
    ucs_conn_match_queue_type_t queue_type = is_private ?
                                             UCS_CONN_MATCH_QUEUE_UNEXP :
                                             UCS_CONN_MATCH_QUEUE_ANY;
    ucs_conn_match_elem_t *conn_match;
    void *peer_address;
    ucs_status_t status;

    peer_address = ucs_alloca(iface->conn_match_ctx.address_length);
    status       = uct_ud_iface_cep_get_peer_address(iface, ib_addr,
                                                     if_addr, path_index,
                                                     peer_address);
    if (status != UCS_OK) {
        return NULL;
    }

    conn_match = ucs_conn_match_get_elem(&iface->conn_match_ctx, peer_address,
                                         conn_sn, queue_type, is_private);
    if (conn_match == NULL) {
        return NULL;
    }

    ep = ucs_container_of(conn_match, uct_ud_ep_t, conn_match);
    ucs_assert(ep->flags & UCT_UD_EP_FLAG_ON_CEP);

    if (is_private) {
        ep->flags &= ~UCT_UD_EP_FLAG_ON_CEP;
    }

    return ep;
}

void uct_ud_iface_cep_remove_ep(uct_ud_iface_t *iface, uct_ud_ep_t *ep)
{
    if (!(ep->flags & UCT_UD_EP_FLAG_ON_CEP)) {
        return;
    }

    ucs_conn_match_remove_elem(&iface->conn_match_ctx, &ep->conn_match,
                               uct_ud_iface_cep_ep_queue_type(ep));
    ep->flags &= ~UCT_UD_EP_FLAG_ON_CEP;
}

static void uct_ud_iface_send_skb_init(uct_iface_h tl_iface, void *obj,
                                       uct_mem_h memh)
{
    uct_ud_send_skb_t *skb = obj;

    skb->lkey  = uct_ib_memh_get_lkey(memh);
    skb->flags = UCT_UD_SEND_SKB_FLAG_INVALID;
}

static void uct_ud_iface_destroy_qp(uct_ud_iface_t *ud_iface)
{
    uct_ud_iface_ops_t *ops = ucs_derived_of(ud_iface->super.ops,
                                             uct_ud_iface_ops_t);

    ops->destroy_qp(ud_iface);
}

static ucs_status_t
uct_ud_iface_create_qp(uct_ud_iface_t *self, const uct_ud_iface_config_t *config)
{
    uct_ib_device_t *dev    = uct_ib_iface_device(&self->super);
    uct_ud_iface_ops_t *ops = ucs_derived_of(self->super.ops, uct_ud_iface_ops_t);
    uct_ib_qp_attr_t qp_init_attr = {};
    struct ibv_qp_attr qp_attr;
    ucs_status_t status;
    int ret;

    qp_init_attr.qp_type             = IBV_QPT_UD;
    qp_init_attr.sq_sig_all          = 0;
    qp_init_attr.cap.max_send_wr     = config->super.tx.queue_len;
    qp_init_attr.cap.max_recv_wr     = config->super.rx.queue_len;
    qp_init_attr.cap.max_send_sge    = ucs_min(config->super.tx.min_sge + 1,
                                               IBV_DEV_ATTR(dev, max_sge));
    qp_init_attr.cap.max_recv_sge    = 1;
    qp_init_attr.cap.max_inline_data = ucs_min(config->super.tx.min_inline,
                                               dev->max_inline_data);

    ucs_debug("create QP: max_send_sge=%u (config=%u, dev=%u) "
              "max_inline_data=%uB (config=%zuB, dev=%uB) ",
              qp_init_attr.cap.max_send_sge, config->super.tx.min_sge + 1,
              IBV_DEV_ATTR(dev, max_sge), qp_init_attr.cap.max_inline_data,
              config->super.tx.min_inline, dev->max_inline_data);

    status = ops->create_qp(&self->super, &qp_init_attr, &self->qp);
    if (status != UCS_OK) {
        return status;
    }

    self->config.max_inline = qp_init_attr.cap.max_inline_data;

    memset(&qp_attr, 0, sizeof(qp_attr));
    /* Modify QP to INIT state */
    qp_attr.qp_state   = IBV_QPS_INIT;
    qp_attr.pkey_index = self->super.pkey_index;
    qp_attr.port_num   = self->super.config.port_num;
    qp_attr.qkey       = UCT_IB_KEY;
    ret = ibv_modify_qp(self->qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY);
    if (ret) {
        ucs_error("Failed to modify UD QP to INIT: %m");
        goto err_destroy_qp;
    }

    /* Modify to RTR */
    qp_attr.qp_state = IBV_QPS_RTR;
    ret = ibv_modify_qp(self->qp, &qp_attr, IBV_QP_STATE);
    if (ret) {
        ucs_error("Failed to modify UD QP to RTR: %m");
        goto err_destroy_qp;
    }

    /* Modify to RTS */
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    ret = ibv_modify_qp(self->qp, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN);
    if (ret) {
        ucs_error("Failed to modify UD QP to RTS: %m");
        goto err_destroy_qp;
    }

    return UCS_OK;

err_destroy_qp:
    uct_ud_iface_destroy_qp(self);
    return UCS_ERR_INVALID_PARAM;
}

static void uct_ud_iface_timer(int timer_id, ucs_event_set_types_t events,
                               void *arg)
{
    uct_ud_iface_t *iface = arg;

    uct_ud_iface_async_progress(iface);
}

static ucs_conn_sn_t
uct_ud_iface_conn_match_get_conn_sn(const ucs_conn_match_elem_t *elem)
{
    uct_ud_ep_t *ep = ucs_container_of(elem, uct_ud_ep_t, conn_match);
    return ep->conn_sn;
}

static const char *
uct_ud_iface_conn_match_peer_address_str(const ucs_conn_match_ctx_t *conn_match_ctx,
                                         const void *address,
                                         char *str, size_t max_size)
{
    uct_ud_iface_t *iface = ucs_container_of(conn_match_ctx,
                                             uct_ud_iface_t,
                                             conn_match_ctx);
    return uct_iface_invoke_ops_func(&iface->super, uct_ud_iface_ops_t,
                                     peer_address_str,
                                     iface, address, str, max_size);
}

static void
uct_ud_iface_conn_match_purge_cb(ucs_conn_match_ctx_t *conn_match_ctx,
                                 ucs_conn_match_elem_t *elem)
{
    uct_ud_iface_t *iface = ucs_container_of(conn_match_ctx,
                                             uct_ud_iface_t,
                                             conn_match_ctx);
    uct_ud_ep_t *ep       = ucs_container_of(elem, uct_ud_ep_t,
                                             conn_match);

    ep->flags &= ~UCT_UD_EP_FLAG_ON_CEP;
    uct_iface_invoke_ops_func(&iface->super, uct_ud_iface_ops_t, ep_free,
                              &ep->super.super);
}

ucs_status_t uct_ud_iface_complete_init(uct_ud_iface_t *iface)
{
    ucs_conn_match_ops_t conn_match_ops = {
        .get_address = uct_ud_ep_get_peer_address,
        .get_conn_sn = uct_ud_iface_conn_match_get_conn_sn,
        .address_str = uct_ud_iface_conn_match_peer_address_str,
        .purge_cb    = uct_ud_iface_conn_match_purge_cb
    };
    ucs_status_t status;

    ucs_conn_match_init(&iface->conn_match_ctx,
                        uct_iface_invoke_ops_func(&iface->super,
                                                  uct_ud_iface_ops_t,
                                                  get_peer_address_length),
                        UCT_UD_IFACE_CEP_CONN_SN_MAX, &conn_match_ops);

    status = ucs_twheel_init(&iface->tx.timer, iface->tx.tick / 4,
                             ucs_get_time());
    if (status != UCS_OK) {
        goto err;
    }

    return UCS_OK;

err:
    return status;
}

ucs_status_t
uct_ud_iface_set_event_cb(uct_ud_iface_t *iface, ucs_async_event_cb_t event_cb)
{
    ucs_async_context_t *async  = iface->super.super.worker->async;
    ucs_async_mode_t async_mode = async->mode;
    ucs_status_t status;
    int event_fd;

    ucs_assert(iface->async.event_cb != NULL);

    status = uct_ib_iface_event_fd_get(&iface->super.super.super, &event_fd);
    if (status != UCS_OK) {
        return status;
    }

    return ucs_async_set_event_handler(async_mode, event_fd,
                                       UCS_EVENT_SET_EVREAD |
                                       UCS_EVENT_SET_EVERR,
                                       event_cb, iface, async);
}

void uct_ud_iface_remove_async_handlers(uct_ud_iface_t *iface)
{
    ucs_status_t status;
    int event_fd;

    uct_ud_iface_progress_disable(&iface->super.super.super,
                                  UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);
    if (iface->async.event_cb != NULL) {
        status = uct_ib_iface_event_fd_get(&iface->super.super.super,
                                           &event_fd);
        if (status == UCS_OK) {
            ucs_async_remove_handler(event_fd, 1);
        }
    }
}

static ucs_status_t uct_ud_iface_gid_hash_init(uct_ud_iface_t *iface,
                                               uct_md_h md)
{
    static const union ibv_gid zero_gid = { .raw = {0} };
    uct_ib_device_t *dev                = &ucs_derived_of(md, uct_ib_md_t)->dev;
    int port                            = iface->super.config.port_num;
    uct_ib_device_gid_info_t gid_info;
    int gid_idx, gid_tbl_len, kh_ret;
    ucs_status_t status;
    char gid_str[128];

    kh_init_inplace(uct_ud_iface_gid, &iface->gid_table.hash);

    gid_tbl_len = uct_ib_device_port_attr(dev, port)->gid_tbl_len;
    for (gid_idx = 0; gid_idx < gid_tbl_len; ++gid_idx) {
        status = uct_ib_device_query_gid_info(dev->ibv_context,
                                              uct_ib_device_name(dev),
                                              port, gid_idx, &gid_info);
        if (status != UCS_OK) {
            goto err;
        }

        if (!memcmp(&gid_info.gid, &zero_gid, sizeof(zero_gid))) {
            continue;
        }

        ucs_debug("iface %p: adding gid %s to hash on device %s port %d index "
                  "%d)", iface, uct_ib_gid_str(&gid_info.gid, gid_str,
                                                sizeof(gid_str)),
                  uct_ib_device_name(dev), port, gid_idx);
        kh_put(uct_ud_iface_gid, &iface->gid_table.hash, gid_info.gid,
               &kh_ret);
        if (kh_ret == UCS_KH_PUT_FAILED) {
            ucs_error("failed to add gid to hash on device %s port %d index %d",
                      uct_ib_device_name(dev), port, gid_idx);
            status = UCS_ERR_NO_MEMORY;
            goto err;
        }
    }

    iface->gid_table.last     = zero_gid;
    iface->gid_table.last_len = sizeof(zero_gid);
    return UCS_OK;

err:
    kh_destroy_inplace(uct_ud_iface_gid, &iface->gid_table.hash);
    return status;
}

UCS_CLASS_INIT_FUNC(uct_ud_iface_t, uct_ud_iface_ops_t *ops,
                    uct_iface_ops_t *tl_ops, uct_md_h md, uct_worker_h worker,
                    const uct_iface_params_t *params,
                    const uct_ud_iface_config_t *config,
                    uct_ib_iface_init_attr_t *init_attr)
{
    ucs_status_t status;
    size_t data_size;
    int mtu;

    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE,
                    "UCT_IFACE_PARAM_FIELD_OPEN_MODE is not defined");
    if (!(params->open_mode & UCT_IFACE_OPEN_MODE_DEVICE)) {
        ucs_error("only UCT_IFACE_OPEN_MODE_DEVICE is supported");
        return UCS_ERR_UNSUPPORTED;
    }

    ucs_trace_func("%s: iface=%p ops=%p worker=%p rx_headroom=%zu",
                   params->mode.device.dev_name, self, ops, worker,
                   (params->field_mask & UCT_IFACE_PARAM_FIELD_RX_HEADROOM) ?
                   params->rx_headroom : 0);

    if (config->super.tx.queue_len <= UCT_UD_TX_MODERATION) {
        ucs_error("%s ud iface tx queue is too short (%d <= %d)",
                  params->mode.device.dev_name,
                  config->super.tx.queue_len, UCT_UD_TX_MODERATION);
        return UCS_ERR_INVALID_PARAM;
    }

    status = uct_ib_device_mtu(params->mode.device.dev_name, md, &mtu);
    if (status != UCS_OK) {
        return status;
    }

    init_attr->rx_priv_len = sizeof(uct_ud_recv_skb_t) -
                             sizeof(uct_ib_iface_recv_desc_t);
    init_attr->rx_hdr_len  = UCT_UD_RX_HDR_LEN;
    init_attr->seg_size    = ucs_min(mtu, config->super.seg_size) + UCT_IB_GRH_LEN;
    init_attr->qp_type     = IBV_QPT_UD;

    UCS_CLASS_CALL_SUPER_INIT(uct_ib_iface_t, tl_ops, &ops->super, md, worker,
                              params, &config->super, init_attr);

    if (self->super.super.worker->async == NULL) {
        ucs_error("%s ud iface must have valid async context", params->mode.device.dev_name);
        return UCS_ERR_INVALID_PARAM;
    }

    self->tx.unsignaled         = 0;
    self->tx.available          = config->super.tx.queue_len;
    self->tx.timer_sweep_count  = 0;
    self->async.disable         = 0;

    self->rx.available          = config->super.rx.queue_len;
    self->rx.quota              = 0;
    self->config.rx_qp_len      = config->super.rx.queue_len;
    self->config.tx_qp_len      = config->super.tx.queue_len;
    self->config.min_poke_time  = ucs_time_from_sec(config->min_poke_time);
    self->config.check_grh_dgid = config->dgid_check &&
                                  uct_ib_iface_is_roce(&self->super);
    self->config.linger_timeout = ucs_time_from_sec(config->linger_timeout);
    self->config.peer_timeout   = ucs_time_from_sec(config->peer_timeout);

    if ((config->max_window < UCT_UD_CA_MIN_WINDOW) ||
        (config->max_window > UCT_UD_CA_MAX_WINDOW)) {
        ucs_error("Max congestion avoidance window should be >= %d and <= %d (%d)",
                  UCT_UD_CA_MIN_WINDOW, UCT_UD_CA_MAX_WINDOW, config->max_window);
        return UCS_ERR_INVALID_PARAM;
    }

    self->config.max_window = config->max_window;

    self->rx.async_max_poll = config->rx_async_max_poll;

    if (config->timer_tick <= 0.) {
        ucs_error("The timer tick should be > 0 (%lf)",
                  config->timer_tick);
        return UCS_ERR_INVALID_PARAM;
    } else {
        self->tx.tick = ucs_time_from_sec(config->timer_tick);
    }

    if (config->timer_backoff < UCT_UD_MIN_TIMER_TIMER_BACKOFF) {
        ucs_error("The timer back off must be >= %lf (%lf)",
                  UCT_UD_MIN_TIMER_TIMER_BACKOFF, config->timer_backoff);
        return UCS_ERR_INVALID_PARAM;
    } else {
        self->tx.timer_backoff = config->timer_backoff;
    }

    if (config->event_timer_tick <= 0.) {
        ucs_error("The event timer tick should be > 0 (%lf)",
                  config->event_timer_tick);
        return UCS_ERR_INVALID_PARAM;
    } else {
        self->async.tick = ucs_time_from_sec(config->event_timer_tick);
    }

    if (self->super.comp_channel != NULL) {
        uct_iface_set_async_event_params(params, &self->async.event_cb,
                                         &self->async.event_arg);
    } else {
        self->async.event_cb  = NULL;
        self->async.event_arg = NULL;
    }

    self->async.timer_id = 0;

    /* Redefine receive desc release callback */
    self->super.release_desc.cb = uct_ud_iface_release_desc;

    UCT_UD_IFACE_HOOK_INIT(self);

    ucs_ptr_array_init(&self->eps, "ud_eps");

    status = uct_ud_iface_create_qp(self, config);
    if (status != UCS_OK) {
        goto err_eps_array;
    }

    status = uct_ib_iface_recv_mpool_init(&self->super, &config->super, params,
                                          "ud_recv_skb", &self->rx.mp);
    if (status != UCS_OK) {
        goto err_qp;
    }

    self->rx.available = ucs_min(config->ud_common.rx_queue_len_init,
                                 config->super.rx.queue_len);
    self->rx.quota     = config->super.rx.queue_len - self->rx.available;
    ucs_mpool_grow(&self->rx.mp, self->rx.available);

    data_size = sizeof(uct_ud_ctl_hdr_t) + self->super.addr_size;
    data_size = ucs_max(data_size, self->super.config.seg_size);
    data_size = ucs_max(data_size,
                        sizeof(uct_ud_zcopy_desc_t) + self->config.max_inline);
    data_size = ucs_max(data_size,
                        sizeof(uct_ud_ctl_desc_t) + sizeof(uct_ud_neth_t));
    status = uct_iface_mpool_init(&self->super.super, &self->tx.mp,
                                  sizeof(uct_ud_send_skb_t) + data_size,
                                  sizeof(uct_ud_send_skb_t),
                                  UCT_UD_SKB_ALIGN,
                                  &config->super.tx.mp, self->config.tx_qp_len,
                                  uct_ud_iface_send_skb_init, "ud_tx_skb");
    if (status != UCS_OK) {
        goto err_rx_mpool;
    }

    self->tx.skb                  = NULL;
    self->tx.async_before_pending = 0;

    ucs_arbiter_init(&self->tx.pending_q);

    if (uct_ib_iface_device(&self->super)->ordered_send_comp) {
        ucs_queue_head_init(&self->tx.outstanding.queue);
    } else {
        kh_init_inplace(uct_ud_iface_ctl_desc_hash, &self->tx.outstanding.map);
    }

    ucs_queue_head_init(&self->tx.async_comp_q);
    ucs_queue_head_init(&self->rx.pending_q);

    status = UCS_STATS_NODE_ALLOC(&self->stats, &uct_ud_iface_stats_class,
                                  self->super.stats, "-%p", self);
    if (status != UCS_OK) {
        goto err_tx_mpool;
    }

    status = uct_ud_iface_gid_hash_init(self, md);
    if (status != UCS_OK) {
        goto err_release_stats;
    }

    return UCS_OK;

err_release_stats:
    UCS_STATS_NODE_FREE(self->stats);
err_tx_mpool:
    ucs_mpool_cleanup(&self->tx.mp, 1);
err_rx_mpool:
    ucs_mpool_cleanup(&self->rx.mp, 1);
err_qp:
    uct_ud_iface_destroy_qp(self);
err_eps_array:
    ucs_ptr_array_cleanup(&self->eps, 1);
    return status;
}

static void uct_ud_iface_delete_eps(uct_ud_iface_t *iface)
{
    uct_ud_ep_t *ep;
    int i;

    ucs_ptr_array_for_each(ep, i, &iface->eps) {
        ucs_assert(!(ep->flags & UCT_UD_EP_FLAG_ON_CEP));
        uct_iface_invoke_ops_func(&iface->super, uct_ud_iface_ops_t,
                                  ep_free, &ep->super.super);
    }
}

static UCS_CLASS_CLEANUP_FUNC(uct_ud_iface_t)
{
    ucs_trace_func("");

    uct_ud_iface_remove_async_handlers(self);

    /* TODO: proper flush and connection termination */
    uct_ud_enter(self);
    ucs_conn_match_cleanup(&self->conn_match_ctx);
    uct_ud_iface_delete_eps(self);
    ucs_twheel_cleanup(&self->tx.timer);
    ucs_debug("iface(%p): cep cleanup", self);
    uct_ud_iface_free_async_comps(self);
    ucs_mpool_cleanup(&self->tx.mp, 0);
    /* TODO: qp to error state and cleanup all wqes */
    uct_ud_iface_free_pending_rx(self);

    /*
     * Destroy QP before deregistering pre-posted receive buffers from
     * self->rx.mp, to avoid ibv_dereg_mr() errors on some devices.
     */
    uct_ud_iface_destroy_qp(self);
    ucs_mpool_cleanup(&self->rx.mp, 0);

    ucs_debug("iface(%p): ptr_array cleanup", self);
    ucs_ptr_array_cleanup(&self->eps, 1);
    ucs_arbiter_cleanup(&self->tx.pending_q);
    UCS_STATS_NODE_FREE(self->stats);
    kh_destroy_inplace(uct_ud_iface_gid, &self->gid_table.hash);
    if (!uct_ib_iface_device(&self->super)->ordered_send_comp) {
        kh_destroy_inplace(uct_ud_iface_ctl_desc_hash,
                           &self->tx.outstanding.map);
    }
    uct_ud_leave(self);
}

UCS_CLASS_DEFINE(uct_ud_iface_t, uct_ib_iface_t);

ucs_config_field_t uct_ud_iface_config_table[] = {
    {UCT_IB_CONFIG_PREFIX, "", NULL,
     ucs_offsetof(uct_ud_iface_config_t, super), UCS_CONFIG_TYPE_TABLE(uct_ib_iface_config_table)},

    {"UD_", "", NULL,
     ucs_offsetof(uct_ud_iface_config_t, ud_common),
     UCS_CONFIG_TYPE_TABLE(uct_ud_iface_common_config_table)},

    {"LINGER_TIMEOUT", "5.0m",
     "Keep the connection open internally for this amount of time after closing it",
     ucs_offsetof(uct_ud_iface_config_t, linger_timeout), UCS_CONFIG_TYPE_TIME},

    {"TIMEOUT", "30s",
     "Consider the remote peer as unreachable if an acknowledgment was not received\n"
     "after this amount of time",
     ucs_offsetof(uct_ud_iface_config_t, peer_timeout), UCS_CONFIG_TYPE_TIME},

    {"TIMER_TICK", "10ms", "Initial timeout for retransmissions",
     ucs_offsetof(uct_ud_iface_config_t, timer_tick), UCS_CONFIG_TYPE_TIME},

    {"TIMER_BACKOFF", "2.0",
     "Timeout multiplier for resending trigger (must be >= "
     UCS_PP_MAKE_STRING(UCT_UD_MIN_TIMER_TIMER_BACKOFF) ")",
     ucs_offsetof(uct_ud_iface_config_t, timer_backoff),
                  UCS_CONFIG_TYPE_DOUBLE},

    {"ASYNC_TIMER_TICK", "100ms", "Resolution for async timer",
     ucs_offsetof(uct_ud_iface_config_t, event_timer_tick), UCS_CONFIG_TYPE_TIME},

    {"MIN_POKE_TIME", "250ms",
     "Minimal interval to send ACK request with solicited flag, to wake up\n"
     "the remote peer in case it is not actively calling progress.\n"
     "Smaller values may incur performance overhead, while extremely large\n"
     "values can cause delays in presence of packet drops.",
     ucs_offsetof(uct_ud_iface_config_t, min_poke_time), UCS_CONFIG_TYPE_TIME},

    {"ETH_DGID_CHECK", "y",
     "Enable checking destination GID for incoming packets of Ethernet network.\n"
     "Mismatched packets are silently dropped.",
     ucs_offsetof(uct_ud_iface_config_t, dgid_check), UCS_CONFIG_TYPE_BOOL},

    {"MAX_WINDOW", UCS_PP_MAKE_STRING(UCT_UD_CA_MAX_WINDOW),
     "Max congestion avoidance window. Should be >= "
      UCS_PP_MAKE_STRING(UCT_UD_CA_MIN_WINDOW) " and <= "
      UCS_PP_MAKE_STRING(UCT_UD_CA_MAX_WINDOW),
     ucs_offsetof(uct_ud_iface_config_t, max_window), UCS_CONFIG_TYPE_UINT},

    {"RX_ASYNC_MAX_POLL", "64",
     "Max number of receive completions to pick during asynchronous TX poll",
     ucs_offsetof(uct_ud_iface_config_t, rx_async_max_poll), UCS_CONFIG_TYPE_UINT},

    {NULL}
};


ucs_status_t uct_ud_iface_query(uct_ud_iface_t *iface,
                                uct_iface_attr_t *iface_attr,
                                size_t am_max_iov, size_t am_max_hdr)
{
    ucs_status_t status;

    status = uct_ib_iface_query(&iface->super,
                                UCT_IB_DETH_LEN + sizeof(uct_ud_neth_t),
                                iface_attr);
    if (status != UCS_OK) {
        return status;
    }

    iface_attr->cap.flags              = UCT_IFACE_FLAG_AM_BCOPY         |
                                         UCT_IFACE_FLAG_AM_ZCOPY         |
                                         UCT_IFACE_FLAG_CONNECT_TO_EP    |
                                         UCT_IFACE_FLAG_CONNECT_TO_IFACE |
                                         UCT_IFACE_FLAG_PENDING          |
                                         UCT_IFACE_FLAG_EP_CHECK         |
                                         UCT_IFACE_FLAG_CB_SYNC          |
                                         UCT_IFACE_FLAG_CB_ASYNC         |
                                         UCT_IFACE_FLAG_ERRHANDLE_PEER_FAILURE |
                                         UCT_IFACE_FLAG_INTER_NODE;

    if (iface->super.comp_channel != NULL) {
        iface_attr->cap.event_flags = UCT_IFACE_FLAG_EVENT_SEND_COMP |
                                      UCT_IFACE_FLAG_EVENT_RECV |
                                      UCT_IFACE_FLAG_EVENT_ASYNC_CB;
    }

    iface_attr->cap.am.max_short       = uct_ib_iface_hdr_size(iface->config.max_inline,
                                                               sizeof(uct_ud_neth_t));
    iface_attr->cap.am.max_bcopy       = iface->super.config.seg_size - UCT_UD_RX_HDR_LEN;
    iface_attr->cap.am.min_zcopy       = 0;
    iface_attr->cap.am.max_zcopy       = iface->super.config.seg_size - UCT_UD_RX_HDR_LEN;
    iface_attr->cap.am.align_mtu       = uct_ib_mtu_value(uct_ib_iface_port_attr(&iface->super)->active_mtu);
    iface_attr->cap.am.opt_zcopy_align = UCS_SYS_PCI_MAX_PAYLOAD;
    iface_attr->cap.am.max_iov         = am_max_iov;
    iface_attr->cap.am.max_hdr         = am_max_hdr;

    iface_attr->cap.put.max_short      = uct_ib_iface_hdr_size(iface->config.max_inline,
                                                               sizeof(uct_ud_neth_t) +
                                                               sizeof(uct_ud_put_hdr_t));

    iface_attr->iface_addr_len         = sizeof(uct_ud_iface_addr_t);
    iface_attr->ep_addr_len            = sizeof(uct_ud_ep_addr_t);
    iface_attr->max_conn_priv          = 0;

    /* UD lacks of scatter to CQE support */
    iface_attr->latency.c             += 30e-9;

    if (iface_attr->cap.am.max_short) {
        iface_attr->cap.flags |= UCT_IFACE_FLAG_AM_SHORT;
    }

    return UCS_OK;
}

ucs_status_t
uct_ud_iface_get_address(uct_iface_h tl_iface, uct_iface_addr_t *iface_addr)
{
    uct_ud_iface_t *iface = ucs_derived_of(tl_iface, uct_ud_iface_t);
    uct_ud_iface_addr_t *addr = (uct_ud_iface_addr_t *)iface_addr;

    uct_ib_pack_uint24(addr->qp_num, iface->qp->qp_num);

    return UCS_OK;
}

static UCS_F_ALWAYS_INLINE int
uct_ud_iface_tx_outstanding_is_empty(uct_ud_iface_t *iface)
{
    return uct_ib_iface_device(&iface->super)->ordered_send_comp ?
                   ucs_queue_is_empty(&iface->tx.outstanding.queue) :
                   (kh_size(&iface->tx.outstanding.map) == 0);
}

ucs_status_t uct_ud_iface_flush(uct_iface_h tl_iface, unsigned flags,
                                uct_completion_t *comp)
{
    uct_ud_iface_t *iface = ucs_derived_of(tl_iface, uct_ud_iface_t);
    uct_ud_ep_t *ep;
    ucs_status_t status;
    int i, count;

    ucs_trace_func("");

    if (comp != NULL) {
        return UCS_ERR_UNSUPPORTED;
    }

    uct_ud_enter(iface);

    if (ucs_unlikely(uct_ud_iface_has_pending_async_ev(iface) ||
                     !uct_ud_iface_tx_outstanding_is_empty(iface))) {
        UCT_TL_IFACE_STAT_FLUSH_WAIT(&iface->super.super);
        uct_ud_leave(iface);
        return UCS_INPROGRESS;
    }

    count = 0;
    ucs_ptr_array_for_each(ep, i, &iface->eps) {
        /* ud ep flush returns either ok or in progress */
        status = uct_ud_ep_flush_nolock(iface, ep, flags, NULL);
        if ((status == UCS_INPROGRESS) || (status == UCS_ERR_NO_RESOURCE)) {
            ++count;
        }
    }

    uct_ud_leave(iface);
    if (count != 0) {
        UCT_TL_IFACE_STAT_FLUSH_WAIT(&iface->super.super);
        return UCS_INPROGRESS;
    }

    UCT_TL_IFACE_STAT_FLUSH(&iface->super.super);
    return UCS_OK;
}

void uct_ud_iface_add_ep(uct_ud_iface_t *iface, uct_ud_ep_t *ep)
{
    ep->ep_id = ucs_ptr_array_insert(&iface->eps, ep);
}

void uct_ud_iface_remove_ep(uct_ud_iface_t *iface, uct_ud_ep_t *ep)
{
    if (ep->ep_id != UCT_UD_EP_NULL_ID) {
        ucs_trace("iface(%p) remove ep: %p id %d", iface, ep, ep->ep_id);
        ucs_ptr_array_remove(&iface->eps, ep->ep_id);
    }
}

uct_ud_send_skb_t *uct_ud_iface_ctl_skb_get(uct_ud_iface_t *iface)
{
    uct_ud_send_skb_t *skb;

    /* grow reserved skb's queue on-demand */
    skb = ucs_mpool_get(&iface->tx.mp);
    if (skb == NULL) {
        ucs_fatal("failed to allocate control skb");
    }

    VALGRIND_MAKE_MEM_DEFINED(&skb->lkey, sizeof(skb->lkey));
    skb->flags = 0;
    return skb;
}

unsigned
uct_ud_iface_dispatch_async_comps_do(uct_ud_iface_t *iface, uct_ud_ep_t *ep)
{
    unsigned count = 0;
    uct_ud_comp_desc_t *cdesc;
    uct_ud_send_skb_t *skb;
    ucs_queue_iter_t iter;

    ucs_trace_func("ep=%p", ep);

    ucs_queue_for_each_safe(skb, iter, &iface->tx.async_comp_q, queue) {
        ucs_assert(!(skb->flags & UCT_UD_SEND_SKB_FLAG_RESENDING));
        cdesc = uct_ud_comp_desc(skb);
        ucs_assert(cdesc->ep != NULL);
        if ((ep == NULL) || (ep == cdesc->ep)) {
            ucs_trace("ep %p: dispatch async comp %p", ep, cdesc->comp);
            ucs_queue_del_iter(&iface->tx.async_comp_q, iter);
            uct_ud_iface_dispatch_comp(iface, cdesc->comp);
            uct_ud_skb_release(skb, 0);
            ++count;
        }
    }

    return count;
}

static void uct_ud_iface_free_async_comps(uct_ud_iface_t *iface)
{
    uct_ud_send_skb_t *skb;

    ucs_queue_for_each_extract(skb, &iface->tx.async_comp_q, queue, 1) {
        uct_ud_skb_release(skb, 0);
    }
}

unsigned uct_ud_iface_dispatch_pending_rx_do(uct_ud_iface_t *iface)
{
    unsigned max_poll = iface->super.config.rx_max_poll;
    int count         = 0;
    uct_ud_recv_skb_t *skb;
    uct_ud_neth_t *neth;
    void *hdr;

    do {
        skb  = ucs_queue_pull_elem_non_empty(&iface->rx.pending_q,
                                             uct_ud_recv_skb_t, u.am.queue);
        hdr  = uct_ib_iface_recv_desc_hdr(&iface->super,
                                          (uct_ib_iface_recv_desc_t*)skb);
        neth = (uct_ud_neth_t*)UCS_PTR_BYTE_OFFSET(hdr, UCT_IB_GRH_LEN);

        uct_ib_iface_invoke_am_desc(&iface->super, uct_ud_neth_get_am_id(neth),
                                    neth + 1, skb->u.am.len, &skb->super);
        ++count;
    } while ((count < max_poll) && !ucs_queue_is_empty(&iface->rx.pending_q));

    return count;
}

static void uct_ud_iface_free_pending_rx(uct_ud_iface_t *iface)
{
    uct_ud_recv_skb_t *skb;

    while (!ucs_queue_is_empty(&iface->rx.pending_q)) {
        skb = ucs_queue_pull_elem_non_empty(&iface->rx.pending_q, uct_ud_recv_skb_t, u.am.queue);
        ucs_mpool_put(skb);
    }
}

void uct_ud_iface_release_desc(uct_recv_desc_t *self, void *desc)
{
    uct_ud_iface_t *iface = ucs_container_of(self,
                                             uct_ud_iface_t, super.release_desc);

    uct_ud_enter(iface);
    uct_ib_iface_release_desc(self, desc);
    uct_ud_leave(iface);
}

ucs_status_t uct_ud_iface_event_arm_common(uct_ud_iface_t *iface,
                                           unsigned events, uint64_t *dirs_p)
{
    ucs_status_t status;
    uint64_t dirs;

    status = uct_ib_iface_pre_arm(&iface->super);
    if (status != UCS_OK) {
        ucs_trace("iface %p: pre arm failed status %s", iface,
                  ucs_status_string(status));
        return status;
    }

    /* Check if some receives were not delivered yet */
    if ((events & (UCT_EVENT_RECV | UCT_EVENT_RECV_SIG)) &&
        !ucs_queue_is_empty(&iface->rx.pending_q))
    {
        ucs_trace("iface %p: arm failed, has %lu unhandled receives", iface,
                  ucs_queue_length(&iface->rx.pending_q));
        return UCS_ERR_BUSY;
    }

    if (events & UCT_EVENT_SEND_COMP) {
        /* Check if some send completions were not delivered yet */
        if (!ucs_queue_is_empty(&iface->tx.async_comp_q)) {
            ucs_trace("iface %p: arm failed, has %lu async send comp", iface,
                      ucs_queue_length(&iface->tx.async_comp_q));
            return UCS_ERR_BUSY;
        }

        /* Check if we have pending operations which need to be progressed */
        if (iface->tx.async_before_pending) {
            ucs_trace("iface %p: arm failed, has async-before-pending flag",
                      iface);
            return UCS_ERR_BUSY;
        }
    }

    dirs = 0;
    if (events & UCT_EVENT_SEND_COMP) {
        dirs |= UCS_BIT(UCT_IB_DIR_TX);
    }

    if (events & (UCT_EVENT_SEND_COMP | UCT_EVENT_RECV)) {
        /* we may get send completion through ACKs as well */
        dirs |= UCS_BIT(UCT_IB_DIR_RX);
    }

    *dirs_p = dirs;
    return UCS_OK;
}

void uct_ud_iface_progress_enable(uct_iface_h tl_iface, unsigned flags)
{
    uct_ud_iface_t *iface       = ucs_derived_of(tl_iface, uct_ud_iface_t);
    ucs_async_context_t *async  = iface->super.super.worker->async;
    ucs_async_mode_t async_mode = async->mode;
    ucs_status_t status;

    uct_ud_enter(iface);

    if (flags & UCT_PROGRESS_RECV) {
        iface->rx.available += iface->rx.quota;
        iface->rx.quota      = 0;
        /* let progress (possibly async) post the missing receives */
    }

    if (iface->async.timer_id == 0) {
        status = ucs_async_add_timer(async_mode, iface->async.tick,
                                     uct_ud_iface_timer, iface, async,
                                     &iface->async.timer_id);
        if (status != UCS_OK) {
            ucs_fatal("iface(%p): unable to add iface timer handler - %s",
                      iface, ucs_status_string(status));
        }
        ucs_assert(iface->async.timer_id != 0);
    }

    uct_ud_leave(iface);

    uct_base_iface_progress_enable(tl_iface, flags);
}

void uct_ud_iface_progress_disable(uct_iface_h tl_iface, unsigned flags)
{
    uct_ud_iface_t *iface = ucs_derived_of(tl_iface, uct_ud_iface_t);
    ucs_status_t status;

    uct_ud_enter(iface);

    if (iface->async.timer_id != 0) {
        status = ucs_async_remove_handler(iface->async.timer_id, 1);
        if (status != UCS_OK) {
            ucs_fatal("iface(%p): unable to remove iface timer handler (%d) - %s",
                      iface, iface->async.timer_id, ucs_status_string(status));
        }
        iface->async.timer_id = 0;
    }

    uct_ud_leave(iface);

    uct_base_iface_progress_disable(tl_iface, flags);
}

void uct_ud_iface_vfs_refresh(uct_iface_h iface)
{
    uct_ud_iface_t *ud_iface = ucs_derived_of(iface, uct_ud_iface_t);
    uct_ud_ep_t *ep;
    int i;

    ucs_vfs_obj_add_ro_file(ud_iface, ucs_vfs_show_primitive,
                            &ud_iface->rx.available, UCS_VFS_TYPE_INT,
                            "rx_available");

    ucs_vfs_obj_add_ro_file(ud_iface, ucs_vfs_show_primitive,
                            &ud_iface->tx.available, UCS_VFS_TYPE_SHORT,
                            "tx_available");

    ucs_vfs_obj_add_ro_file(ud_iface, ucs_vfs_show_primitive,
                            &ud_iface->config.rx_qp_len, UCS_VFS_TYPE_INT,
                            "rx_qp_len");

    ucs_vfs_obj_add_ro_file(ud_iface, ucs_vfs_show_primitive,
                            &ud_iface->config.tx_qp_len, UCS_VFS_TYPE_INT,
                            "tx_qp_len");

    ucs_ptr_array_for_each(ep, i, &ud_iface->eps) {
        uct_ud_ep_vfs_populate(ep);
    }
}

void uct_ud_iface_ctl_skb_complete(uct_ud_iface_t *iface,
                                   uct_ud_ctl_desc_t *cdesc, int is_async)
{
    uct_ud_send_skb_t *resent_skb, *skb;

    skb = cdesc->self_skb;
    ucs_assert(!(skb->flags & UCT_UD_SEND_SKB_FLAG_INVALID));

    resent_skb = cdesc->resent_skb;
    ucs_assert(uct_ud_ctl_desc(skb) == cdesc);

    if (resent_skb != NULL) {
        ucs_assert(skb->flags        & UCT_UD_SEND_SKB_FLAG_CTL_RESEND);
        ucs_assert(resent_skb->flags & UCT_UD_SEND_SKB_FLAG_RESENDING);

        resent_skb->flags &= ~UCT_UD_SEND_SKB_FLAG_RESENDING;
        --cdesc->ep->tx.resend_count;
    } else {
        ucs_assert(skb->flags & UCT_UD_SEND_SKB_FLAG_CTL_ACK);
    }

    uct_ud_ep_window_release_completed(cdesc->ep, is_async);
    uct_ud_skb_release(skb, 0);

}

void uct_ud_iface_send_completion_ordered(uct_ud_iface_t *iface, uint16_t sn,
                                          int is_async)
{
    uct_ud_ctl_desc_t *cdesc;
    ucs_queue_for_each_extract(cdesc, &iface->tx.outstanding.queue, queue,
                               UCS_CIRCULAR_COMPARE16(cdesc->sn, <=, sn)) {
        uct_ud_iface_ctl_skb_complete(iface, cdesc, is_async);
    }
}

void uct_ud_iface_send_completion_unordered(uct_ud_iface_t *iface, uint16_t sn,
                                            int is_async)
{
    khiter_t khiter = kh_get(uct_ud_iface_ctl_desc_hash,
                             &iface->tx.outstanding.map, sn);
    uct_ud_ctl_desc_t *cdesc;

    if (ucs_likely(khiter != kh_end(&iface->tx.outstanding.map))) {
        cdesc = kh_value(&iface->tx.outstanding.map, khiter);
        uct_ud_iface_ctl_skb_complete(iface, cdesc, is_async);
        kh_del(uct_ud_iface_ctl_desc_hash, &iface->tx.outstanding.map, khiter);
    }
}

union ibv_gid* uct_ud_grh_get_dgid(struct ibv_grh *grh, size_t dgid_len)
{
    size_t i;

    /* Make sure that daddr in IPv4 resides in the last 4 bytes in GRH */
    UCS_STATIC_ASSERT((UCT_IB_GRH_LEN - (20 + offsetof(struct iphdr, daddr))) ==
                      UCS_IPV4_ADDR_LEN);

    /* Make sure that dgid resides in the last 16 bytes in GRH */
    UCS_STATIC_ASSERT((UCT_IB_GRH_LEN - offsetof(struct ibv_grh, dgid)) ==
                      UCS_IPV6_ADDR_LEN);

    ucs_assert((dgid_len == UCS_IPV4_ADDR_LEN) ||
               (dgid_len == UCS_IPV6_ADDR_LEN));

    /*
    * According to Annex17_RoCEv2 (A17.4.5.2):
    * "The first 40 bytes of user posted UD Receive Buffers are reserved for the L3
    * header of the incoming packet (as per the InfiniBand Spec Section 11.4.1.2).
    * In RoCEv2, this area is filled up with the IP header. IPv6 header uses the
    * entire 40 bytes. IPv4 headers use the 20 bytes in the second half of the
    * reserved 40 bytes area (i.e. offset 20 from the beginning of the receive
    * buffer). In this case, the content of the first 20 bytes is undefined. "
    */
    if (dgid_len == UCS_IPV4_ADDR_LEN) {
        /* IPv4 mapped to IPv6 looks like: 0000:0000:0000:0000:0000:ffff:????:????
           reset begin to make hash function working */
        for (i = 0; i < (sizeof(union ibv_gid) - UCS_IPV4_ADDR_LEN - 2);) {
            grh->dgid.raw[i++] = 0x00;
        }

        grh->dgid.raw[i++]     = 0xff;
        grh->dgid.raw[i++]     = 0xff;
    }

    return &grh->dgid;
}
