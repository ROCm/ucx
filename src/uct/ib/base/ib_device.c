/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2014. ALL RIGHTS RESERVED.
* Copyright (C) UT-Battelle, LLC. 2014. ALL RIGHTS RESERVED.
* Copyright (C) Huawei Technologies Co., Ltd. 2020.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "ib_device.h"
#include "ib_md.h"

#include <ucs/arch/bitops.h>
#include <ucs/debug/memtrack_int.h>
#include <ucs/debug/log.h>
#include <ucs/async/async.h>
#include <ucs/sys/compiler.h>
#include <ucs/sys/string.h>
#include <ucs/sys/netlink.h>
#include <ucs/sys/sock.h>
#include <ucs/sys/sys.h>
#include <sys/poll.h>
#include <libgen.h>
#include <pthread.h>
#include <sched.h>

#ifdef HAVE_NETLINK_RDMA
#include <rdma/rdma_netlink.h>
#endif

#define UCT_IB_DEVICE_LOOPBACK_NDEV_INDEX_INVALID 0


/* This table is according to "Encoding for RNR NAK Timer Field"
 * in IBTA specification */
const double uct_ib_qp_rnr_time_ms[] = {
    655.36,  0.01,  0.02,   0.03,   0.04,   0.06,   0.08,   0.12,
      0.16,  0.24,  0.32,   0.48,   0.64,   0.96,   1.28,   1.92,
      2.56,  3.84,  5.12,   7.68,  10.24,  15.36,  20.48,  30.72,
     40.96, 61.44, 81.92, 122.88, 163.84, 245.76, 327.68, 491.52
};


/* use both gid + lid data for key generation (lid - ib based, gid - RoCE) */
static UCS_F_ALWAYS_INLINE
khint32_t uct_ib_kh_ah_hash_func(struct ibv_ah_attr attr)
{
    return kh_int64_hash_func(attr.grh.dgid.global.subnet_prefix ^
                              attr.grh.dgid.global.interface_id  ^
                              attr.dlid);
}

static UCS_F_ALWAYS_INLINE
int uct_ib_kh_ah_hash_equal(struct ibv_ah_attr a, struct ibv_ah_attr b)
{
    return !memcmp(&a, &b, sizeof(a));
}

KHASH_IMPL(uct_ib_ah, struct ibv_ah_attr, struct ibv_ah*, 1,
           uct_ib_kh_ah_hash_func, uct_ib_kh_ah_hash_equal)


static UCS_F_ALWAYS_INLINE
khint32_t uct_ib_async_event_hash_func(uct_ib_async_event_t event)
{
    return kh_int64_hash_func(((uint64_t)event.event_type << 32) |
                              event.resource_id);
}

static UCS_F_ALWAYS_INLINE int
uct_ib_async_event_hash_equal(uct_ib_async_event_t event1,
                              uct_ib_async_event_t event2)
{
    return (event1.event_type  == event2.event_type) &&
           (event1.resource_id == event2.resource_id);
}

KHASH_IMPL(uct_ib_async_event, uct_ib_async_event_t, uct_ib_async_event_val_t, 1,
           uct_ib_async_event_hash_func, uct_ib_async_event_hash_equal)

typedef struct uct_ib_device_subnet {
    struct sockaddr_storage address;
    unsigned                prefix_length;
} uct_ib_device_subnet_t;

UCS_ARRAY_DECLARE_TYPE(uct_ib_device_subnet_array_t, unsigned,
                       uct_ib_device_subnet_t);

typedef struct {
    uint64_t    guid;
    uint8_t     port_num;
    uint8_t     gid_index;
} uct_ib_device_to_ndev_key_t;

static UCS_F_ALWAYS_INLINE khint32_t
uct_ib_device_to_ndev_cache_hash_func(uct_ib_device_to_ndev_key_t key)
{
    return kh_int_hash_func(((uint64_t)key.port_num << 24) ^
                            ((uint64_t)key.gid_index << 16) ^
                            key.guid);
}

static UCS_F_ALWAYS_INLINE int
uct_ib_device_to_ndev_cache_hash_equal(uct_ib_device_to_ndev_key_t key1,
                                       uct_ib_device_to_ndev_key_t key2)
{
    return (key1.port_num == key2.port_num) &&
           (key1.gid_index == key2.gid_index) &&
           (key1.guid == key2.guid);
}

KHASH_INIT(uct_ib_device_to_ndev, uct_ib_device_to_ndev_key_t, unsigned, 1,
           uct_ib_device_to_ndev_cache_hash_func,
           uct_ib_device_to_ndev_cache_hash_equal);

static khash_t(uct_ib_device_to_ndev) ib_dev_to_ndev_map;

#ifdef ENABLE_STATS
static ucs_stats_class_t uct_ib_device_stats_class = {
    .name          = "",
    .num_counters  = UCT_IB_DEVICE_STAT_LAST,
    .class_id      = UCS_STATS_CLASS_ID_INVALID,
    .counter_names = {
        [UCT_IB_DEVICE_STAT_ASYNC_EVENT] = "async_event"
    }
};
#endif

static uct_ib_device_spec_t uct_ib_builtin_device_specs[] = {
  {"ConnectX-3", {0x15b3, 4099},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX4_PRM, 10},
  {"ConnectX-3 Pro", {0x15b3, 4103},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX4_PRM, 11},
  {"Connect-IB", {0x15b3, 4113},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V1, 20},
  {"ConnectX-4", {0x15b3, 4115},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V1, 30},
  {"ConnectX-4", {0x15b3, 4116},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V1, 29},
  {"ConnectX-4 LX", {0x15b3, 4117},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V1, 28},
  {"ConnectX-4 LX VF", {0x15b3, 4118},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V1, 28},
  {"ConnectX-5", {0x15b3, 4119},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 38},
  {"ConnectX-5", {0x15b3, 4121},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 40},
  {"ConnectX-5", {0x15b3, 4120},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 39},
  {"ConnectX-5", {0x15b3, 41682},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 37},
  {"ConnectX-5", {0x15b3, 4122},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 36},
  {"ConnectX-6", {0x15b3, 4123},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 50},
  {"ConnectX-6 VF", {0x15b3, 4124},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 50},
  {"ConnectX-6 DX", {0x15b3, 4125},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 60},
  {"ConnectX-6 DX VF", {0x15b3, 4126},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 60},
  {"ConnectX-6 LX", {0x15b3, 4127},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 45},
  {"ConnectX-7", {0x15b3, 4129},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 70},
  {"ConnectX-8", {0x15b3, 4131},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 80},
  {"BlueField", {0x15b3, 0xa2d2},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 41},
  {"BlueField VF", {0x15b3, 0xa2d3},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 41},
  {"BlueField 2", {0x15b3, 0xa2d6},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 61},
  {"BlueField 3", {0x15b3, 0xa2dc},
   UCT_IB_DEVICE_FLAG_MELLANOX | UCT_IB_DEVICE_FLAG_MLX5_PRM |
   UCT_IB_DEVICE_FLAG_DC_V2, 61},
  {"Generic HCA", {0, 0}, 0, 0},
  {NULL}
};

static void
uct_ib_device_get_locality(const char *dev_name, ucs_sys_cpuset_t *cpu_mask)
{
    char *p, buf[ucs_max(CPU_SETSIZE, 10)];
    ssize_t nread;
    uint32_t word;
    int base, k;

    /* Read list of CPUs close to the device */
    CPU_ZERO(cpu_mask);
    nread = ucs_read_file(buf, sizeof(buf) - 1, 1, UCT_IB_DEVICE_SYSFS_FMT,
                          dev_name, "local_cpus");
    if (nread >= 0) {
        buf[CPU_SETSIZE - 1] = '\0';
        base = 0;
        do {
            p = strrchr(buf, ',');
            if (p == NULL) {
                p = buf;
            } else if (*p == ',') {
                *(p++) = 0;
            }

            word = strtoul(p, 0, 16);
            for (k = 0; word; ++k, word >>= 1) {
                if (word & 1) {
                    CPU_SET(base + k, cpu_mask);
                }
            }
            base += 32;
        } while ((base < CPU_SETSIZE) && (p != buf));
    } else {
        /* If affinity file is not present, treat all CPUs as local */
        for (k = 0; k < CPU_SETSIZE; ++k) {
            CPU_SET(k, cpu_mask);
        }
    }
}

static void
uct_ib_device_async_event_schedule_callback(uct_ib_device_t *dev,
                                            uct_ib_async_event_wait_t *wait_ctx)
{
    ucs_assert(ucs_spinlock_is_held(&dev->async_event_lock));
    ucs_assert(wait_ctx->cb_id == UCS_CALLBACKQ_ID_NULL);
    wait_ctx->cb_id = ucs_callbackq_add_safe(wait_ctx->cbq, wait_ctx->cb,
                                             wait_ctx);
}

static void
uct_ib_device_async_event_dispatch_nolock(uct_ib_device_t *dev,
                                          const uct_ib_async_event_t *event)
{
    khiter_t iter = kh_get(uct_ib_async_event, &dev->async_events_hash, *event);
    uct_ib_async_event_val_t *entry;

    if (iter == kh_end(&dev->async_events_hash)) {
        return;
    }

    entry        = &kh_value(&dev->async_events_hash, iter);
    entry->fired = 1;
    if (entry->wait_ctx != NULL) {
        uct_ib_device_async_event_schedule_callback(dev, entry->wait_ctx);
    }
}

static void
uct_ib_device_async_event_dispatch(uct_ib_device_t *dev,
                                   const uct_ib_async_event_t *event)
{
    ucs_spin_lock(&dev->async_event_lock);
    uct_ib_device_async_event_dispatch_nolock(dev, event);
    ucs_spin_unlock(&dev->async_event_lock);
}

static void
uct_ib_device_async_event_dispatch_fatal(uct_ib_device_t *dev)
{
    uct_ib_async_event_t event;

    ucs_spin_lock(&dev->async_event_lock);
    dev->flags |= UCT_IB_DEVICE_FAILED;
    kh_foreach_key(&dev->async_events_hash, event,
                   uct_ib_device_async_event_dispatch_nolock(dev, &event));
    ucs_spin_unlock(&dev->async_event_lock);
}

ucs_status_t
uct_ib_device_async_event_register(uct_ib_device_t *dev,
                                   enum ibv_event_type event_type,
                                   uint32_t resource_id)
{
    uct_ib_async_event_val_t *entry;
    uct_ib_async_event_t event;
    ucs_status_t status;
    khiter_t iter;
    int ret;

    event.event_type  = event_type;
    event.resource_id = resource_id;

    ucs_spin_lock(&dev->async_event_lock);
    iter = kh_put(uct_ib_async_event, &dev->async_events_hash, event, &ret);
    if (ret == UCS_KH_PUT_FAILED) {
        status = UCS_ERR_NO_MEMORY;
        goto out;
    }

    ucs_assert(ret != UCS_KH_PUT_KEY_PRESENT);
    entry           = &kh_value(&dev->async_events_hash, iter);
    entry->wait_ctx = NULL;
    entry->fired    = 0;
    status          = UCS_OK;

out:
    ucs_spin_unlock(&dev->async_event_lock);
    return status;
}

static int uct_ib_device_async_event_inprogress(uct_ib_async_event_val_t *entry)
{
    return (entry->wait_ctx != NULL) &&
           (entry->wait_ctx->cb_id != UCS_CALLBACKQ_ID_NULL);
}

ucs_status_t
uct_ib_device_async_event_wait(uct_ib_device_t *dev,
                               enum ibv_event_type event_type,
                               uint32_t resource_id,
                               uct_ib_async_event_wait_t *wait_ctx)
{
    uct_ib_async_event_val_t *entry;
    uct_ib_async_event_t event;
    ucs_status_t status;
    khiter_t iter;

    event.event_type  = event_type;
    event.resource_id = resource_id;

    ucs_spin_lock(&dev->async_event_lock);
    iter  = kh_get(uct_ib_async_event, &dev->async_events_hash, event);
    ucs_assert(iter != kh_end(&dev->async_events_hash));
    entry = &kh_value(&dev->async_events_hash, iter);

    if (uct_ib_device_async_event_inprogress(entry)) {
        status = UCS_ERR_BUSY;
        goto out_unlock;
    }

    status          = UCS_OK;
    wait_ctx->cb_id = UCS_CALLBACKQ_ID_NULL;
    entry->wait_ctx = wait_ctx;
    if (entry->fired) {
        uct_ib_device_async_event_schedule_callback(dev, wait_ctx);
    }

out_unlock:
    ucs_spin_unlock(&dev->async_event_lock);
    return status;
}

void uct_ib_device_async_event_unregister(uct_ib_device_t *dev,
                                          enum ibv_event_type event_type,
                                          uint32_t resource_id)
{
    uct_ib_async_event_val_t *entry;
    uct_ib_async_event_t event;
    khiter_t iter;

    event.event_type  = event_type;
    event.resource_id = resource_id;

    ucs_spin_lock(&dev->async_event_lock);
    iter = kh_get(uct_ib_async_event, &dev->async_events_hash, event);
    ucs_assert(iter != kh_end(&dev->async_events_hash));
    entry = &kh_value(&dev->async_events_hash, iter);
    if (uct_ib_device_async_event_inprogress(entry)) {
        /* cancel scheduled callback */
        ucs_callbackq_remove_safe(entry->wait_ctx->cbq, entry->wait_ctx->cb_id);
    }
    kh_del(uct_ib_async_event, &dev->async_events_hash, iter);
    ucs_spin_unlock(&dev->async_event_lock);
}

static void uct_ib_async_event_handler(int fd, ucs_event_set_types_t events,
                                       void *arg)
{
    uct_ib_device_t *dev = arg;
    struct ibv_async_event ibevent;
    uct_ib_async_event_t event;
    int ret;

    ret = ibv_get_async_event(dev->ibv_context, &ibevent);
    if (ret != 0) {
        if (errno != EAGAIN) {
            ucs_warn("ibv_get_async_event() failed: %m");
        }
        return;
    }

    event.event_type = ibevent.event_type;
    switch (event.event_type) {
    case IBV_EVENT_CQ_ERR:
        event.cookie = ibevent.element.cq;
        break;
    case IBV_EVENT_QP_FATAL:
    case IBV_EVENT_QP_REQ_ERR:
    case IBV_EVENT_QP_ACCESS_ERR:
    case IBV_EVENT_COMM_EST:
    case IBV_EVENT_SQ_DRAINED:
    case IBV_EVENT_PATH_MIG:
    case IBV_EVENT_PATH_MIG_ERR:
    case IBV_EVENT_QP_LAST_WQE_REACHED:
        event.qp_num = ibevent.element.qp->qp_num;
        break;
    case IBV_EVENT_SRQ_ERR:
    case IBV_EVENT_SRQ_LIMIT_REACHED:
        event.cookie = ibevent.element.srq;
        break;
    case IBV_EVENT_DEVICE_FATAL:
    case IBV_EVENT_PORT_ERR:
    case IBV_EVENT_PORT_ACTIVE:
#if HAVE_DECL_IBV_EVENT_GID_CHANGE
    case IBV_EVENT_GID_CHANGE:
#endif
    case IBV_EVENT_LID_CHANGE:
    case IBV_EVENT_PKEY_CHANGE:
    case IBV_EVENT_SM_CHANGE:
    case IBV_EVENT_CLIENT_REREGISTER:
        event.port_num = ibevent.element.port_num;
        break;
    default:
        break;
    };

    uct_ib_handle_async_event(dev, &event);
    ibv_ack_async_event(&ibevent);
}

void uct_ib_handle_async_event(uct_ib_device_t *dev, uct_ib_async_event_t *event)
{
    char event_info[200];
    ucs_log_level_t level;

    switch (event->event_type) {
    case IBV_EVENT_CQ_ERR:
        snprintf(event_info, sizeof(event_info), "%s on CQ %p",
                 ibv_event_type_str(event->event_type), event->cookie);
        level = UCS_LOG_LEVEL_ERROR;
        break;
    case IBV_EVENT_COMM_EST:
    case IBV_EVENT_QP_ACCESS_ERR:
        snprintf(event_info, sizeof(event_info), "%s on QPN 0x%x",
                 ibv_event_type_str(event->event_type), event->qp_num);
        level = UCS_LOG_LEVEL_DIAG;
        break;
    case IBV_EVENT_QP_FATAL:
    case IBV_EVENT_QP_REQ_ERR:
    case IBV_EVENT_SQ_DRAINED:
    case IBV_EVENT_PATH_MIG:
    case IBV_EVENT_PATH_MIG_ERR:
        snprintf(event_info, sizeof(event_info), "%s on QPN 0x%x",
                 ibv_event_type_str(event->event_type), event->qp_num);
        level = UCS_LOG_LEVEL_ERROR;
        break;
    case IBV_EVENT_QP_LAST_WQE_REACHED:
        snprintf(event_info, sizeof(event_info), "SRQ-attached QP 0x%x was flushed",
                 event->qp_num);
        uct_ib_device_async_event_dispatch(dev, event);
        level = UCS_LOG_LEVEL_DEBUG;
        break;
    case IBV_EVENT_SRQ_ERR:
        level = UCS_LOG_LEVEL_ERROR;
        snprintf(event_info, sizeof(event_info), "%s on SRQ %p",
                 ibv_event_type_str(event->event_type), event->cookie);
        break;
    case IBV_EVENT_SRQ_LIMIT_REACHED:
        snprintf(event_info, sizeof(event_info), "%s on SRQ %p",
                 ibv_event_type_str(event->event_type), event->cookie);
        level = UCS_LOG_LEVEL_DEBUG;
        break;
    case IBV_EVENT_DEVICE_FATAL:
        uct_ib_device_async_event_dispatch_fatal(dev);
        snprintf(event_info, sizeof(event_info), "%s on port %d",
                 ibv_event_type_str(event->event_type), event->port_num);
        level = UCS_LOG_LEVEL_DIAG;
        break;
    case IBV_EVENT_PORT_ACTIVE:
    case IBV_EVENT_PORT_ERR:
    case IBV_EVENT_SM_CHANGE:
    case IBV_EVENT_CLIENT_REREGISTER:
        snprintf(event_info, sizeof(event_info), "%s on port %d",
                 ibv_event_type_str(event->event_type), event->port_num);
        level = UCS_LOG_LEVEL_DIAG;
        break;
#if HAVE_DECL_IBV_EVENT_GID_CHANGE
    case IBV_EVENT_GID_CHANGE:
#endif
    case IBV_EVENT_LID_CHANGE:
    case IBV_EVENT_PKEY_CHANGE:
        snprintf(event_info, sizeof(event_info), "%s on port %d",
                 ibv_event_type_str(event->event_type), event->port_num);
        level = UCS_LOG_LEVEL_WARN;
        break;
    default:
        snprintf(event_info, sizeof(event_info), "%s (%d)",
                 ibv_event_type_str(event->event_type), event->event_type);
        level = UCS_LOG_LEVEL_INFO;
        break;
    };

    UCS_STATS_UPDATE_COUNTER(dev->stats, UCT_IB_DEVICE_STAT_ASYNC_EVENT, +1);
    ucs_log(level, "IB Async event on %s: %s", uct_ib_device_name(dev), event_info);
}

static void
uct_ib_device_set_pci_id(uct_ib_device_t *dev, const char *sysfs_path)
{
    const char *dev_name = uct_ib_device_name(dev);
    char pci_id_str[16];
    ucs_status_t status;

    status = ucs_sys_read_sysfs_file(dev_name, sysfs_path, "vendor", pci_id_str,
                                     sizeof(pci_id_str), UCS_LOG_LEVEL_WARN);
    dev->pci_id.vendor = (status == UCS_OK) ? strtol(pci_id_str, NULL, 0) : 0;

    status = ucs_sys_read_sysfs_file(dev_name, sysfs_path, "device", pci_id_str,
                                     sizeof(pci_id_str), UCS_LOG_LEVEL_WARN);
    dev->pci_id.device = (status == UCS_OK) ? strtol(pci_id_str, NULL, 0) : 0;

    ucs_debug("%s: vendor_id 0x%x device_id %d", uct_ib_device_name(dev),
              dev->pci_id.vendor, dev->pci_id.device);
}

ucs_status_t uct_ib_device_query(uct_ib_device_t *dev,
                                 struct ibv_device *ibv_device)
{
    const char *dev_name               = uct_ib_device_name(dev);
    const char *dev_path               = dev->ibv_context->device->ibdev_path;
    const unsigned sys_device_priority = 20;
    ucs_status_t status;
    char *path_buffer;
    const char *sysfs_path;
    uint8_t i;
    int ret;

    status = uct_ib_query_device(dev->ibv_context, &dev->dev_attr);
    if (status != UCS_OK) {
        goto out;
    }

    /* Check device type */
    switch (ibv_device->node_type) {
    case IBV_NODE_SWITCH:
        dev->first_port = 0;
        dev->num_ports  = 1;
        break;
    case IBV_NODE_CA:
    default:
        dev->first_port = UCT_IB_FIRST_PORT;
        dev->num_ports  = IBV_DEV_ATTR(dev, phys_port_cnt);
        break;
    }

    if (dev->num_ports > UCT_IB_DEV_MAX_PORTS) {
        ucs_debug("%s has %d ports, but only up to %d are supported",
                  dev_name, dev->num_ports, UCT_IB_DEV_MAX_PORTS);
        dev->num_ports = UCT_IB_DEV_MAX_PORTS;
    }

    /* Query all ports */
    for (i = 0; i < dev->num_ports; ++i) {
        ret = ibv_query_port(dev->ibv_context, i + dev->first_port,
                             &dev->port_attr[i]);
        if (ret != 0) {
            ucs_error("ibv_query_port() returned %d: %m", ret);
            status = UCS_ERR_IO_ERROR;
            goto out;
        }
    }

    status = ucs_string_alloc_path_buffer(&path_buffer, "path_buffer");
    if (status != UCS_OK) {
        goto out;
    }

    sysfs_path   = ucs_topo_resolve_sysfs_path(dev_path, path_buffer);
    dev->sys_dev = ucs_topo_get_sysfs_dev(dev_name, sysfs_path,
                                          sys_device_priority);
    uct_ib_device_set_pci_id(dev, sysfs_path);
    dev->pci_bw = ucs_topo_get_pci_bw(dev_name, sysfs_path);

    ucs_free(path_buffer);
out:
    return status;
}

ucs_status_t uct_ib_device_init(uct_ib_device_t *dev,
                                struct ibv_device *ibv_device, int async_events
                                UCS_STATS_ARG(ucs_stats_node_t *stats_parent))
{
    ucs_status_t status;

    dev->async_events = async_events;

    if (!dev->req_notify_cq_support) {
        ucs_trace("%s does not support async event handling",
                  uct_ib_device_name(dev));
    }

    uct_ib_device_get_locality(ibv_get_device_name(ibv_device),
                               &dev->local_cpus);

    status = UCS_STATS_NODE_ALLOC(&dev->stats, &uct_ib_device_stats_class,
                                  stats_parent, "device");
    if (status != UCS_OK) {
        goto err;
    }

    status = ucs_sys_fcntl_modfl(dev->ibv_context->async_fd, O_NONBLOCK, 0);
    if (status != UCS_OK) {
        goto err_release_stats;
    }

    /* Register to IB async events */
    if (dev->async_events) {
        status = ucs_async_set_event_handler(UCS_ASYNC_THREAD_LOCK_TYPE,
                                             dev->ibv_context->async_fd,
                                             UCS_EVENT_SET_EVREAD,
                                             uct_ib_async_event_handler, dev,
                                             NULL);
        if (status != UCS_OK) {
            goto err_release_stats;
        }
    }

    kh_init_inplace(uct_ib_ah, &dev->ah_hash);
    ucs_recursive_spinlock_init(&dev->ah_lock, 0);
    kh_init_inplace(uct_ib_async_event, &dev->async_events_hash);
    ucs_spinlock_init(&dev->async_event_lock, 0);

    ucs_debug("initialized device '%s' (%s) with %d ports", uct_ib_device_name(dev),
              ibv_node_type_str(ibv_device->node_type),
              dev->num_ports);
    return UCS_OK;

err_release_stats:
    UCS_STATS_NODE_FREE(dev->stats);
err:
    return status;
}

static void uct_ib_device_cleanup_ah_cached(uct_ib_device_t *dev)
{
    struct ibv_ah *ah;

    kh_foreach_value(&dev->ah_hash, ah, ibv_destroy_ah(ah));
    kh_destroy_inplace(uct_ib_ah, &dev->ah_hash);
}

void uct_ib_device_cleanup(uct_ib_device_t *dev)
{
    ucs_debug("destroying ib device %s", uct_ib_device_name(dev));

    if (kh_size(&dev->async_events_hash) != 0) {
        ucs_warn("async_events_hash not empty");
    }

    kh_destroy_inplace(uct_ib_async_event, &dev->async_events_hash);
    ucs_spinlock_destroy(&dev->async_event_lock);
    uct_ib_device_cleanup_ah_cached(dev);
    ucs_recursive_spinlock_destroy(&dev->ah_lock);

    if (dev->async_events) {
        ucs_async_remove_handler(dev->ibv_context->async_fd, 1);
    }
    UCS_STATS_NODE_FREE(dev->stats);
}

static inline int uct_ib_device_spec_match(uct_ib_device_t *dev,
                                           const uct_ib_device_spec_t *spec)
{
    return (spec->pci_id.vendor == dev->pci_id.vendor) &&
           (spec->pci_id.device == dev->pci_id.device);
}

const uct_ib_device_spec_t* uct_ib_device_spec(uct_ib_device_t *dev)
{
    uct_ib_md_t *md = ucs_container_of(dev, uct_ib_md_t, dev);
    uct_ib_device_spec_t *spec;

    /* search through devices specified in the configuration */
    for (spec = md->custom_devices.specs;
         spec < md->custom_devices.specs + md->custom_devices.count; ++spec) {
        if (uct_ib_device_spec_match(dev, spec)) {
            return spec;
        }
    }

    /* search through built-in list of device specifications */
    spec = uct_ib_builtin_device_specs;
    while ((spec->name != NULL) && !uct_ib_device_spec_match(dev, spec)) {
        ++spec;
    }
    return spec; /* if no match is found, return the last entry, which contains
                    default settings for unknown devices */
}

static unsigned long uct_ib_device_get_ib_gid_index(uct_ib_md_t *md)
{
    if (md->config.gid_index == UCS_ULUNITS_AUTO) {
        return UCT_IB_DEVICE_DEFAULT_GID_INDEX;
    } else {
        return md->config.gid_index;
    }
}

ucs_status_t uct_ib_device_port_check(uct_ib_device_t *dev, uint8_t port_num,
                                      unsigned flags)
{
    uct_ib_md_t *md = ucs_container_of(dev, uct_ib_md_t, dev);
    const uct_ib_device_spec_t *dev_info;
    uint8_t required_dev_flags;
    ucs_status_t status;
    unsigned gid_index;
    union ibv_gid gid;

    if (port_num < dev->first_port || port_num >= dev->first_port + dev->num_ports) {
        return UCS_ERR_NO_DEVICE;
    }

    if (uct_ib_device_port_attr(dev, port_num)->gid_tbl_len == 0) {
        ucs_debug("%s:%d has no gid", uct_ib_device_name(dev),
                  port_num);
        return UCS_ERR_UNSUPPORTED;
    }

    if (uct_ib_device_port_attr(dev, port_num)->state != IBV_PORT_ACTIVE) {
        ucs_trace("%s:%d is not active (state: %d)", uct_ib_device_name(dev),
                  port_num, uct_ib_device_port_attr(dev, port_num)->state);
        return UCS_ERR_UNREACHABLE;
    }

    if (flags & UCT_IB_DEVICE_FLAG_SRQ) {
        if (IBV_DEV_ATTR(dev, max_srq) == 0) {
            ucs_trace("%s:%d does not support SRQ", uct_ib_device_name(dev),
                      port_num);
            return UCS_ERR_UNSUPPORTED;
        }
    }

    if (!uct_ib_device_is_port_ib(dev, port_num) && (flags & UCT_IB_DEVICE_FLAG_LINK_IB)) {
        ucs_debug("%s:%d is not IB link layer", uct_ib_device_name(dev),
                  port_num);
        return UCS_ERR_UNSUPPORTED;
    }

    if (flags & UCT_IB_DEVICE_FLAG_DC) {
        if (!IBV_DEVICE_HAS_DC(dev)) {
            ucs_trace("%s:%d does not support DC", uct_ib_device_name(dev), port_num);
            return UCS_ERR_UNSUPPORTED;
        }
    }

    /* check generic device flags */
    dev_info           = uct_ib_device_spec(dev);
    required_dev_flags = flags & (UCT_IB_DEVICE_FLAG_MLX4_PRM |
                                  UCT_IB_DEVICE_FLAG_MLX5_PRM);
    if (!ucs_test_all_flags(dev_info->flags, required_dev_flags)) {
        ucs_trace("%s:%d (%s) does not support flags 0x%x", uct_ib_device_name(dev),
                  port_num, dev_info->name, required_dev_flags);
        return UCS_ERR_UNSUPPORTED;
    }

    gid_index = uct_ib_device_get_ib_gid_index(md);
    status    = uct_ib_device_query_gid(dev, port_num, gid_index, &gid,
                                        UCS_LOG_LEVEL_DIAG);
    if (status != UCS_OK) {
        return status;
    }

    if (md->check_subnet_filter && uct_ib_device_is_port_ib(dev, port_num) &&
        (md->subnet_filter != gid.global.subnet_prefix)) {
        ucs_trace("%s:%d subnet_prefix does not match", uct_ib_device_name(dev),
                  port_num);
        return UCS_ERR_UNSUPPORTED;
    }

    return UCS_OK;
}

ucs_status_t
uct_ib_device_set_ece(uct_ib_device_t *dev, struct ibv_qp *qp, uint32_t ece_val)
{
    uct_ib_md_t *md = ucs_container_of(dev, uct_ib_md_t, dev);
#if HAVE_DECL_IBV_SET_ECE
    struct ibv_ece ece;
#endif

    if (ece_val == UCT_IB_DEVICE_ECE_DEFAULT) {
        return UCS_OK;
    }

    ucs_assertv_always(md->ece_enable, "device=%s, ece=0x%x",
                       uct_ib_device_name(dev), ece_val);

#if HAVE_DECL_IBV_SET_ECE
    if (ibv_query_ece(qp, &ece)) {
        ucs_error("ibv_query_ece(device=%s qpn=0x%x) failed: %m",
                  uct_ib_device_name(dev), qp->qp_num);
        return UCS_ERR_IO_ERROR;
    }

    ece.options = ece_val;
    if (ibv_set_ece(qp, &ece)) {
        ucs_error("ibv_set_ece(device=%s qpn=0x%x) failed: %m",
                  uct_ib_device_name(dev), qp->qp_num);
        return UCS_ERR_INVALID_PARAM;
    }

    return UCS_OK;
#endif
    return UCS_ERR_UNSUPPORTED;
}

const char *uct_ib_roce_version_str(uct_ib_roce_version_t roce_ver)
{
    switch (roce_ver) {
    case UCT_IB_DEVICE_ROCE_V1:
        return "RoCE v1";
    case UCT_IB_DEVICE_ROCE_V1_5:
        return "RoCE v1.5";
    case UCT_IB_DEVICE_ROCE_V2:
        return "RoCE v2";
    default:
        return "<unknown RoCE version>";
    }
}

const char *uct_ib_gid_str(const union ibv_gid *gid, char *str, size_t max_size)
{
    inet_ntop(AF_INET6, gid, str, max_size);
    return str;
}

static int uct_ib_device_is_addr_ipv4_mcast(const struct in6_addr *raw,
                                            const uint32_t addr_last_bits)
{
    /* IPv4 encoded multicast addresses */
    return (raw->s6_addr32[0] == htonl(0xff0e0000)) &&
           !(raw->s6_addr32[1] | addr_last_bits);
}

static sa_family_t uct_ib_device_get_addr_family(union ibv_gid *gid, int gid_index)
{
    const struct in6_addr *raw    = (struct in6_addr *)gid->raw;
    const uint32_t addr_last_bits = raw->s6_addr32[2] ^ htonl(0x0000ffff);
    char p[128];

    ucs_trace_func("testing addr_family on gid index %d: %s",
                   gid_index, uct_ib_gid_str(gid, p, sizeof(p)));

    if (!((raw->s6_addr32[0] | raw->s6_addr32[1]) | addr_last_bits) ||
        uct_ib_device_is_addr_ipv4_mcast(raw, addr_last_bits)) {
        return AF_INET;
    } else {
        return AF_INET6;
    }
}

ucs_status_t
uct_ib_device_query_gid_info(struct ibv_context *ctx, const char *dev_name,
                             uint8_t port_num, unsigned gid_index,
                             uct_ib_device_gid_info_t *info)
{
    char buf[16];
    int ret;

    ret = ibv_query_gid(ctx, port_num, gid_index, &info->gid);
    if (ret == 0) {
        ret = ucs_read_file(buf, sizeof(buf) - 1, 1,
                            UCT_IB_DEVICE_SYSFS_GID_TYPE_FMT,
                            dev_name, port_num, gid_index);
        if (ret > 0) {
            if (!strncmp(buf, "IB/RoCE v1", 10)) {
                info->roce_info.ver = UCT_IB_DEVICE_ROCE_V1;
            } else if (!strncmp(buf, "RoCE v2", 7)) {
                info->roce_info.ver = UCT_IB_DEVICE_ROCE_V2;
            } else {
                ucs_error("failed to parse gid type '%s' (dev=%s port=%d index=%d)",
                          buf, dev_name, port_num, gid_index);
                return UCS_ERR_INVALID_PARAM;
            }
        } else {
            info->roce_info.ver = UCT_IB_DEVICE_ROCE_V1;
        }

        info->roce_info.addr_family =
                        uct_ib_device_get_addr_family(&info->gid, gid_index);
        info->gid_index            = gid_index;
        return UCS_OK;
    }

    ucs_error("ibv_query_gid(dev=%s port=%d index=%d) failed: %m",
              dev_name, port_num, gid_index);
    return UCS_ERR_INVALID_PARAM;
}

int uct_ib_device_test_roce_gid_index(uct_ib_device_t *dev, uint8_t port_num,
                                      const union ibv_gid *gid,
                                      uint8_t gid_index)
{
    struct ibv_ah_attr ah_attr;
    struct ibv_ah *ah;

    ucs_assert(uct_ib_device_is_port_roce(dev, port_num));

    memset(&ah_attr, 0, sizeof(ah_attr));
    ah_attr.port_num       = port_num;
    ah_attr.is_global      = 1;
    ah_attr.grh.dgid       = *gid;
    ah_attr.grh.sgid_index = gid_index;
    ah_attr.grh.hop_limit  = 255;
    ah_attr.grh.flow_label = 1;
    ah_attr.dlid           = UCT_IB_ROCE_UDP_SRC_PORT_BASE;

    ah = ibv_create_ah(ucs_container_of(dev, uct_ib_md_t, dev)->pd, &ah_attr);
    if (ah == NULL) {
        return 0; /* gid entry is not operational */
    }

    ibv_destroy_ah(ah);
    return 1;
}

ucs_status_t
uct_ib_device_roce_gid_to_sockaddr(sa_family_t af, const void *gid,
                                   struct sockaddr_storage *sock_storage)
{
    struct sockaddr *sa = (struct sockaddr*)sock_storage;
    const uint8_t *inet_addr;
    size_t addr_size;
    ucs_status_t status;

    /* Set address family */
    sa->sa_family = af;

    /* Set port to 0 as it's not relevant for RoCE */
    status = ucs_sockaddr_set_port(sa, 0);
    if (status != UCS_OK) {
        return status;
    }

    /* Get address size */
    status = ucs_sockaddr_inet_addr_size(af, &addr_size);
    if (status != UCS_OK) {
        return status;
    }

    /* Set IP address */
    inet_addr = UCS_PTR_BYTE_OFFSET(gid, sizeof(union ibv_gid) - addr_size);
    return ucs_sockaddr_set_inet_addr(sa, inet_addr);
}

static ucs_status_t
uct_ib_device_parse_subnet_filter(const ucs_config_allow_list_t *subnet_strs,
                                  uct_ib_device_subnet_array_t *subnets)
{
    char *address_str, *mask_str;
    char **subnet_str;
    char subnet_str_dup[UCS_SOCKADDR_STRING_LEN];
    uct_ib_device_subnet_t *subnet;
    ucs_status_t status;

    if (subnet_strs->mode == UCS_CONFIG_ALLOW_LIST_ALLOW_ALL) {
        return UCS_OK;
    }

    ucs_carray_for_each(subnet_str, subnet_strs->array.names,
                        subnet_strs->array.count) {
        ucs_strncpy_safe(subnet_str_dup, *subnet_str, sizeof(subnet_str_dup));

        /* Expect a string of the following pattern: x.x.x.x/y */
        ucs_string_split(subnet_str_dup, "/", 2, &address_str, &mask_str);
        if (mask_str == NULL) {
            status = UCS_ERR_INVALID_PARAM;
            goto err;
        }

        subnet = ucs_array_append_fixed(subnets);

        /* Parse subnet address */
        status = ucs_sock_ipstr_to_sockaddr(address_str, &subnet->address);
        if (status != UCS_OK) {
            goto err;
        }

        /* Parse subnet mask */
        if (sscanf(mask_str, "%u", &subnet->prefix_length) != 1) {
            status = UCS_ERR_INVALID_PARAM;
            goto err;
        }
    }

    return UCS_OK;

err:
    ucs_error("failed to parse RoCE subnet: %s", *subnet_str);
    return status;
}

static int
uct_ib_device_match_roce_subnet(const uct_ib_device_gid_info_t *gid_info,
                                const uct_ib_device_subnet_array_t *subnets,
                                ucs_config_allow_list_mode_t mode)
{
    const int is_allow_mode = (mode == UCS_CONFIG_ALLOW_LIST_ALLOW);
    static const char UCS_V_UNUSED *allow_mode_str[] = {"accepted",
                                                        "restricted"};
    const uct_ib_device_subnet_t *subnet;
    struct sockaddr_storage gid_sockaddr;
    char gid_str[UCS_SOCKADDR_STRING_LEN];
    char subnet_str[UCS_SOCKADDR_STRING_LEN];

    if (mode == UCS_CONFIG_ALLOW_LIST_ALLOW_ALL) {
        return 1;
    }

    /* Convert GID to sockaddr structure */
    if (uct_ib_device_roce_gid_to_sockaddr(gid_info->roce_info.addr_family,
                                           &gid_info->gid,
                                           &gid_sockaddr) != UCS_OK) {
        ucs_error("failed to convert GID %u to sockaddr", gid_info->gid_index);
        return 0;
    }

    /* Iterate over all subnets and compare them with GID */
    ucs_array_for_each(subnet, subnets) {
        if (!ucs_sockaddr_is_same_subnet(
                    (const struct sockaddr*)&gid_sockaddr,
                    (const struct sockaddr*)&subnet->address,
                    subnet->prefix_length)) {
            continue;
        }

        ucs_sockaddr_str((const struct sockaddr*)&gid_sockaddr, gid_str,
                         UCS_SOCKADDR_STRING_LEN);
        ucs_sockaddr_str((const struct sockaddr*)&subnet->address, subnet_str,
                         UCS_SOCKADDR_STRING_LEN);
        ucs_trace("address %s at gid[%u] was %s by subnet filter %s/%u",
                  gid_str, gid_info->gid_index, allow_mode_str[!is_allow_mode],
                  subnet_str, subnet->prefix_length);

        /* Accept/Restrict GID according to required policy */
        return is_allow_mode;
    }

    ucs_trace("gid index %u was %s due to no matching subnets",
              gid_info->gid_index, allow_mode_str[!is_allow_mode]);

    /* Handle non-matched GID according to policy */
    return !is_allow_mode;
}

ucs_status_t
uct_ib_device_select_gid(uct_ib_device_t *dev, uint8_t port_num,
                         const ucs_config_allow_list_t *subnet_strs,
                         uct_ib_device_gid_info_t *gid_info)
{
    static const size_t max_str_len                     = 200;
    static const uct_ib_roce_version_info_t roce_prio[] = {
        {UCT_IB_DEVICE_ROCE_V2, AF_INET},
        {UCT_IB_DEVICE_ROCE_V2, AF_INET6},
        {UCT_IB_DEVICE_ROCE_V1, AF_INET},
        {UCT_IB_DEVICE_ROCE_V1, AF_INET6}
    };
    int gid_tbl_len         = uct_ib_device_port_attr(dev, port_num)->gid_tbl_len;
    ucs_status_t status     = UCS_OK;
    int priorities_arr_len  = ucs_static_array_size(roce_prio);
    UCS_ARRAY_DEFINE_ONSTACK(uct_ib_device_subnet_array_t, subnets,
                             subnet_strs->array.count);
    uct_ib_device_gid_info_t gid_info_tmp;
    int i, prio_idx, res;
    char subnet_list_str[max_str_len];

    ucs_assert(uct_ib_device_is_port_roce(dev, port_num));

    status = uct_ib_device_parse_subnet_filter(subnet_strs, &subnets);
    if (status != UCS_OK) {
        return status;
    }

    /* search for matching GID table entries, according to the order defined
     * in priorities array
     */
    for (prio_idx = 0; prio_idx < priorities_arr_len; prio_idx++) {
        for (i = 0; i < gid_tbl_len; i++) {
            status = uct_ib_device_query_gid_info(dev->ibv_context,
                                                  uct_ib_device_name(dev),
                                                  port_num, i, &gid_info_tmp);
            if (status != UCS_OK) {
                goto out;
            }

            if ((roce_prio[prio_idx].ver         == gid_info_tmp.roce_info.ver) &&
                (roce_prio[prio_idx].addr_family == gid_info_tmp.roce_info.addr_family) &&
                uct_ib_device_test_roce_gid_index(dev, port_num, &gid_info_tmp.gid, i) &&
                uct_ib_device_match_roce_subnet(&gid_info_tmp, &subnets,
                                                subnet_strs->mode)) {
                gid_info->gid_index = i;
                gid_info->roce_info = gid_info_tmp.roce_info;
                goto out_print;
            }
        }
    }

    if (subnet_strs->mode != UCS_CONFIG_ALLOW_LIST_ALLOW_ALL) {
        res = ucs_config_sprintf_allow_list(subnet_list_str, max_str_len,
                                            subnet_strs,
                                            &ucs_config_array_string);
        ucs_error("failed to find a gid which matches/unmatches the following "
                  "subnet list: %s", res ? subnet_list_str : "<none>");
        return UCS_ERR_INVALID_PARAM;
    }

    gid_info->gid_index             = UCT_IB_DEVICE_DEFAULT_GID_INDEX;
    gid_info->roce_info.ver         = UCT_IB_DEVICE_ROCE_V1;
    gid_info->roce_info.addr_family = AF_INET;

out_print:
    ucs_debug("%s:%d using gid_index %d", uct_ib_device_name(dev), port_num,
              gid_info->gid_index);
out:
    return status;
}

int uct_ib_device_is_port_ib(uct_ib_device_t *dev, uint8_t port_num)
{
#if HAVE_DECL_IBV_LINK_LAYER_INFINIBAND
    return uct_ib_device_port_attr(dev, port_num)->link_layer == IBV_LINK_LAYER_INFINIBAND;
#else
    return 1;
#endif
}

int uct_ib_device_is_port_roce(uct_ib_device_t *dev, uint8_t port_num)
{
    return IBV_PORT_IS_LINK_LAYER_ETHERNET(uct_ib_device_port_attr(dev, port_num));
}

const char *uct_ib_device_name(uct_ib_device_t *dev)
{
    return ibv_get_device_name(dev->ibv_context->device);
}

size_t uct_ib_mtu_value(enum ibv_mtu mtu)
{
    switch (mtu) {
    case IBV_MTU_256:
        return 256;
    case IBV_MTU_512:
        return 512;
    case IBV_MTU_1024:
        return 1024;
    case IBV_MTU_2048:
        return 2048;
    case IBV_MTU_4096:
        return 4096;
    }
    ucs_fatal("Invalid MTU value (%d)", mtu);
}

uint8_t uct_ib_to_qp_fabric_time(double t)
{
    double to;

    to = log(t / 4.096e-6) / log(2.0);
    if (to < 1) {
        return 1; /* Very small timeout */
    } else if ((long)(to + 0.5) >= UCT_IB_FABRIC_TIME_MAX) {
        return 0; /* No timeout */
    } else {
        return (long)(to + 0.5);
    }
}

uint8_t uct_ib_to_rnr_fabric_time(double t)
{
    double time_ms = t * UCS_MSEC_PER_SEC;
    uint8_t idx, next_index;
    double avg_ms;

    for (idx = 1; idx < UCT_IB_FABRIC_TIME_MAX; idx++) {
        next_index = (idx + 1) % UCT_IB_FABRIC_TIME_MAX;

        if (time_ms <= uct_ib_qp_rnr_time_ms[next_index]) {
            avg_ms = (uct_ib_qp_rnr_time_ms[idx] +
                      uct_ib_qp_rnr_time_ms[next_index]) * 0.5;

            if (time_ms < avg_ms) {
                /* return previous index */
                return idx;
            } else {
                /* return current index */
                return next_index;
            }
        }
    }

    return 0; /* this is a special value that means the maximum value */
}

ucs_status_t uct_ib_modify_qp(struct ibv_qp *qp, enum ibv_qp_state state)
{
    struct ibv_qp_attr qp_attr;

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = state;
    if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE)) {
        ucs_warn("modify qp 0x%x to state %d failed: %m", qp->qp_num, state);
        return UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
}

ucs_status_t uct_ib_device_query_ports(uct_ib_device_t *dev, unsigned flags,
                                       uct_tl_device_resource_t **tl_devices_p,
                                       unsigned *num_tl_devices_p)
{
    uct_tl_device_resource_t *tl_devices;
    unsigned num_tl_devices;
    ucs_status_t status;
    uint8_t port_num;

    /* Allocate resources array
     * We may allocate more memory than really required, but it's not so bad. */
    tl_devices = ucs_calloc(dev->num_ports, sizeof(*tl_devices), "ib device resource");
    if (tl_devices == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    /* Second pass: fill port information */
    num_tl_devices = 0;
    for (port_num = dev->first_port; port_num < dev->first_port + dev->num_ports;
         ++port_num)
    {
        /* Check port capabilities */
        status = uct_ib_device_port_check(dev, port_num, flags);
        if (status != UCS_OK) {
           ucs_trace("%s:%d does not support flags 0x%x: %s",
                     uct_ib_device_name(dev), port_num, flags,
                     ucs_status_string(status));
           continue;
        }

        /* Save device information */
        ucs_snprintf_zero(tl_devices[num_tl_devices].name,
                          sizeof(tl_devices[num_tl_devices].name),
                          "%s:%d", uct_ib_device_name(dev), port_num);
        tl_devices[num_tl_devices].type       = UCT_DEVICE_TYPE_NET;
        tl_devices[num_tl_devices].sys_device = dev->sys_dev;
        ++num_tl_devices;
    }

    if (num_tl_devices == 0) {
        ucs_debug("no compatible IB ports found for flags 0x%x", flags);
        status = UCS_ERR_NO_DEVICE;
        goto err_free;
    }

    *num_tl_devices_p = num_tl_devices;
    *tl_devices_p     = tl_devices;
    return UCS_OK;

err_free:
    ucs_free(tl_devices);
err:
    return status;
}

ucs_status_t uct_ib_device_find_port(uct_ib_device_t *dev,
                                     const char *resource_dev_name,
                                     uint8_t *p_port_num)
{
    const char *ibdev_name;
    unsigned port_num;
    size_t devname_len;
    char *p;

    p = strrchr(resource_dev_name, ':');
    if (p == NULL) {
        goto err; /* Wrong device name format */
    }
    devname_len = p - resource_dev_name;

    ibdev_name = uct_ib_device_name(dev);
    if ((strlen(ibdev_name) != devname_len) ||
        strncmp(ibdev_name, resource_dev_name, devname_len))
    {
        goto err; /* Device name is wrong */
    }

    port_num = strtod(p + 1, &p);
    if (*p != '\0') {
        goto err; /* Failed to parse port number */
    }
    if ((port_num < dev->first_port) || (port_num >= dev->first_port + dev->num_ports)) {
        goto err; /* Port number out of range */
    }

    *p_port_num = port_num;
    return UCS_OK;

err:
    ucs_error("%s: failed to find port", resource_dev_name);
    return UCS_ERR_NO_DEVICE;
}

ucs_status_t uct_ib_device_mtu(const char *dev_name, uct_md_h md, int *p_mtu)
{
    uct_ib_device_t *dev = &ucs_derived_of(md, uct_ib_md_t)->dev;
    uint8_t port_num;
    ucs_status_t status;

    status = uct_ib_device_find_port(dev, dev_name, &port_num);
    if (status != UCS_OK) {
        return status;
    }

    *p_mtu = uct_ib_mtu_value(uct_ib_device_port_attr(dev, port_num)->active_mtu);
    return UCS_OK;
}

int uct_ib_device_is_gid_valid(const union ibv_gid *gid)
{
    return gid->global.interface_id != 0;
}

ucs_status_t uct_ib_device_query_gid(uct_ib_device_t *dev, uint8_t port_num,
                                     unsigned gid_index, union ibv_gid *gid,
                                     ucs_log_level_t error_level)
{
    uct_ib_device_gid_info_t gid_info;
    ucs_status_t status;

    status = uct_ib_device_query_gid_info(dev->ibv_context, uct_ib_device_name(dev),
                                          port_num, gid_index, &gid_info);
    if (status != UCS_OK) {
        return status;
    }

    if (!uct_ib_device_is_gid_valid(&gid_info.gid)) {
        ucs_log(error_level, "invalid gid[%d] on %s:%d", gid_index,
                uct_ib_device_name(dev), port_num);
        return UCS_ERR_INVALID_ADDR;
    }

    *gid = gid_info.gid;
    return UCS_OK;
}

const char *uct_ib_wc_status_str(enum ibv_wc_status wc_status)
{
    return ibv_wc_status_str(wc_status);
}

static ucs_status_t
uct_ib_device_create_ah(uct_ib_device_t *dev, struct ibv_ah_attr *ah_attr,
                        struct ibv_pd *pd, const char *usage,
                        struct ibv_ah **ah_p)
{
    struct ibv_ah *ah;
    char buf[128];

    ah = ibv_create_ah(pd, ah_attr);
    if (ah == NULL) {
        ucs_error("ibv_create_ah(%s) for %s on %s failed: %m",
                  uct_ib_ah_attr_str(buf, sizeof(buf), ah_attr), usage,
                  uct_ib_device_name(dev));
        return (errno == ETIMEDOUT) ?
                UCS_ERR_ENDPOINT_TIMEOUT : UCS_ERR_INVALID_ADDR;
    }

    *ah_p = ah;
    return UCS_OK;
}

ucs_status_t uct_ib_device_get_ah_cached(uct_ib_device_t *dev,
                                         struct ibv_ah_attr *ah_attr,
                                         struct ibv_ah **ah_p)
{
    ucs_status_t status = UCS_OK;
    khiter_t iter;

    ucs_recursive_spin_lock(&dev->ah_lock);

    /* looking for existing AH with same attributes */
    iter = kh_get(uct_ib_ah, &dev->ah_hash, *ah_attr);
    if (iter == kh_end(&dev->ah_hash)) {
        status = UCS_ERR_NO_ELEM;
        goto unlock;
    } else {
        /* found existing AH */
        *ah_p = kh_value(&dev->ah_hash, iter);
    }

unlock:
    ucs_recursive_spin_unlock(&dev->ah_lock);
    return status;
}

ucs_status_t
uct_ib_device_create_ah_cached(uct_ib_device_t *dev,
                               struct ibv_ah_attr *ah_attr, struct ibv_pd *pd,
                               const char *usage, struct ibv_ah **ah_p)
{
    ucs_status_t status = UCS_OK;
    khiter_t iter;
    int ret;

    ucs_recursive_spin_lock(&dev->ah_lock);

    /* looking for existing AH with same attributes */
    iter = kh_get(uct_ib_ah, &dev->ah_hash, *ah_attr);
    if (iter == kh_end(&dev->ah_hash)) {
        /* new AH */
        status = uct_ib_device_create_ah(dev, ah_attr, pd, usage, ah_p);
        if (status != UCS_OK) {
            goto unlock;
        }

        /* store AH in hash */
        iter = kh_put(uct_ib_ah, &dev->ah_hash, *ah_attr, &ret);

        /* failed to store - rollback */
        if (iter == kh_end(&dev->ah_hash)) {
            ibv_destroy_ah(*ah_p);
            status = UCS_ERR_NO_MEMORY;
            goto unlock;
        }

        kh_value(&dev->ah_hash, iter) = *ah_p;
    } else {
        /* found existing AH */
        *ah_p = kh_value(&dev->ah_hash, iter);
    }

unlock:
    ucs_recursive_spin_unlock(&dev->ah_lock);
    return status;
}

int uct_ib_get_cqe_size(int cqe_size_min)
{
    static int cqe_size_max = -1;
    int cqe_size;

    if (cqe_size_max == -1) {
#ifdef __aarch64__
        char arm_board_vendor[128];
        ucs_aarch64_cpuid_t cpuid;
        ucs_aarch64_cpuid(&cpuid);

        arm_board_vendor[0] = '\0';
        ucs_read_file(arm_board_vendor, sizeof(arm_board_vendor), 1,
                      "/sys/devices/virtual/dmi/id/board_vendor");
        ucs_debug("arm_board_vendor is '%s'", arm_board_vendor);

        cqe_size_max = ((strcasestr(arm_board_vendor, "Huawei")) &&
                        (cpuid.implementer == 0x41) && (cpuid.architecture == 8) &&
                        (cpuid.variant == 0)        && (cpuid.part == 0xd08)     &&
                        (cpuid.revision == 2))
                       ? 64 : 128;
#else
        cqe_size_max = 128;
#endif
        ucs_debug("max IB CQE size is %d", cqe_size_max);
    }

    /* Set cqe size according to inline size and cache line size. */
    cqe_size = ucs_max(cqe_size_min, UCS_SYS_CACHE_LINE_SIZE);
    cqe_size = ucs_max(cqe_size, 64);  /* at least 64 */
    cqe_size = ucs_min(cqe_size, cqe_size_max);

    return cqe_size;
}

ucs_status_t
uct_ib_device_get_roce_ndev_name(uct_ib_device_t *dev, uint8_t port_num,
                                 uint8_t gid_index, char *ndev_name, size_t max)
{
    ssize_t nread;

    ucs_assert_always(uct_ib_device_is_port_roce(dev, port_num));

    /* get the network device name which corresponds to a RoCE port */
    nread = ucs_read_file(ndev_name, max, 1,
                          UCT_IB_DEVICE_SYSFS_GID_NDEV_FMT,
                          uct_ib_device_name(dev), port_num, gid_index);
    if (nread < 0) {
        ucs_diag("failed to read " UCT_IB_DEVICE_SYSFS_GID_NDEV_FMT": %m",
                 uct_ib_device_name(dev), port_num, 0);
        return UCS_ERR_NO_DEVICE;
    }

    ucs_strtrim(ndev_name);
    return UCS_OK;
}

ucs_status_t uct_ib_iface_get_loopback_ndev_index(unsigned *ndev_index_p)
{
    static unsigned loopback_ndev_index = UCT_IB_DEVICE_LOOPBACK_NDEV_INDEX_INVALID;
    ucs_status_t status;

    if (loopback_ndev_index == UCT_IB_DEVICE_LOOPBACK_NDEV_INDEX_INVALID) {
        status = ucs_ifname_to_index("lo", &loopback_ndev_index);
        if (status != UCS_OK) {
            return status;
        }
    }

    *ndev_index_p = loopback_ndev_index;
    return UCS_OK;
}

ucs_status_t
uct_ib_device_get_roce_ndev_index(uct_ib_device_t *dev, uint8_t port_num,
                                  uint8_t gid_index, unsigned *ndev_index_p)
{
    uct_ib_device_to_ndev_key_t ib_dev = {.guid = IBV_DEV_ATTR(dev, node_guid),
                                          .port_num = port_num,
                                          .gid_index = gid_index};
    static pthread_mutex_t uct_ib_device_to_ndev_cache_lock =
                                          PTHREAD_MUTEX_INITIALIZER;
    ucs_status_t status;
    char ndev_name[IFNAMSIZ];
    unsigned ndev_index;
    khiter_t iter;
    unsigned khret;

    pthread_mutex_lock(&uct_ib_device_to_ndev_cache_lock);
    iter = kh_put(uct_ib_device_to_ndev, &ib_dev_to_ndev_map, ib_dev, &khret);
    if (khret == UCS_KH_PUT_FAILED) {
        status = UCS_ERR_IO_ERROR;
        goto out_unlock;
    }

    if (khret != UCS_KH_PUT_KEY_PRESENT) {
        status = uct_ib_device_get_roce_ndev_name(dev, port_num, gid_index,
                                                  ndev_name, sizeof(ndev_name));
        if (status != UCS_OK) {
            goto out_unlock;
        }

        status = ucs_ifname_to_index(ndev_name, &ndev_index);
        if (status != UCS_OK) {
            goto out_unlock;
        }

        kh_val(&ib_dev_to_ndev_map, iter) = ndev_index;
    }

    *ndev_index_p = kh_val(&ib_dev_to_ndev_map, iter);
    status        = UCS_OK;

out_unlock:
    pthread_mutex_unlock(&uct_ib_device_to_ndev_cache_lock);
    return status;
}

unsigned uct_ib_device_get_roce_lag_level(uct_ib_device_t *dev, uint8_t port_num,
                                          uint8_t gid_index)
{
    char ndev_name[IFNAMSIZ];
    unsigned roce_lag_level;
    ucs_status_t status;

    status = uct_ib_device_get_roce_ndev_name(dev, port_num, gid_index,
                                              ndev_name, sizeof(ndev_name));
    if (status != UCS_OK) {
        return 1;
    }

    roce_lag_level = ucs_netif_bond_ad_num_ports(ndev_name);
    ucs_debug("RoCE LAG level on %s:%d (%s) is %u", uct_ib_device_name(dev),
              port_num, ndev_name, roce_lag_level);
    return roce_lag_level;
}

const char* uct_ib_ah_attr_str(char *buf, size_t max,
                               const struct ibv_ah_attr *ah_attr)
{
    char *p    = buf;
    char *endp = buf + max;

    snprintf(p, endp - p, "dlid=%d sl=%d port=%d src_path_bits=%d",
             ah_attr->dlid, ah_attr->sl,
             ah_attr->port_num, ah_attr->src_path_bits);
    p += strlen(p);

    if (ah_attr->is_global) {
        snprintf(p, endp - p, " dgid=");
        p += strlen(p);
        uct_ib_gid_str(&ah_attr->grh.dgid, p, endp - p);
        p += strlen(p);
        snprintf(p, endp - p, " flow_label=0x%x sgid_index=%d "
                 "traffic_class=%d", ah_attr->grh.flow_label,
                 ah_attr->grh.sgid_index, ah_attr->grh.traffic_class);
    }

    return buf;
}

#ifdef HAVE_NETLINK_RDMA
static ucs_status_t
uct_ib_device_is_smi_cb(const struct nlmsghdr *nlh, void *arg)
{
    int *is_smi_p = (int*)arg;
    const struct nlattr *attr;
    uint8_t dev_type;

    for (attr = NLMSG_DATA(nlh); UCS_PTR_BYTE_DIFF(nlh, attr) < nlh->nlmsg_len;
         attr = UCS_PTR_BYTE_OFFSET(attr, NLA_ALIGN(attr->nla_len))) {
        if (attr->nla_type == RDMA_NLDEV_ATTR_DEV_TYPE /* 99 */) {
            dev_type = *(const uint8_t*)UCS_PTR_BYTE_OFFSET(attr, NLA_HDRLEN);
            if (dev_type == RDMA_DEVICE_TYPE_SMI /* 1 */) {
                *is_smi_p = 1;
                return UCS_OK;
            }
        }
    }

    return UCS_INPROGRESS;
}

int uct_ib_device_is_smi(struct ibv_device *ibv_device)
{
    struct nlattr *attr;
    uint32_t *dev_index_attr;
    size_t header_length;
    ucs_status_t status;
    int is_smi;

    header_length   = NLA_HDRLEN + sizeof(*dev_index_attr);
    attr            = ucs_alloca(header_length);
    dev_index_attr  = (uint32_t*)UCS_PTR_BYTE_OFFSET(attr, NLA_HDRLEN);
    attr->nla_type  = RDMA_NLDEV_ATTR_DEV_INDEX;
    attr->nla_len   = header_length;
    *dev_index_attr = ibv_get_device_index(ibv_device);
    if (*dev_index_attr == -1) {
        ucs_debug("%s: failed to get device index",
                  ibv_get_device_name(ibv_device));
        return 0;
    }

    is_smi = 0;
    status = ucs_netlink_send_request(
            NETLINK_RDMA, RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, RDMA_NLDEV_CMD_GET),
            0, attr, header_length, uct_ib_device_is_smi_cb, &is_smi);
    if (status != UCS_OK) {
        return 0;
    }

    return is_smi;
}
#else
int uct_ib_device_is_smi(struct ibv_device *ibv_device)
{
    return 0;
}
#endif
