/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2020. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ucs/debug/memtrack_int.h>
#include <ucs/vfs/sock/vfs_sock.h>
#include <ucs/vfs/base/vfs_obj.h>
#include <ucs/sys/compiler.h>
#include <ucs/sys/string.h>
#include <ucs/sys/sys.h>
#include <ucs/debug/log.h>
#include <sys/un.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <errno.h>
#include <fuse.h>

#ifdef HAVE_INOTIFY
#include <sys/inotify.h>
#endif


typedef struct {
    void            *buf;
    fuse_fill_dir_t filler;
} ucs_vfs_enum_dir_context_t;


static struct {
    pthread_t       thread_id;
    pthread_mutex_t mutex;
    struct fuse     *fuse;
    int             fuse_fd;
    int             stop;
    int             inotify_fd;
    int             watch_desc;
} ucs_vfs_fuse_context = {
    .thread_id  = -1,
    .mutex      = PTHREAD_MUTEX_INITIALIZER,
    .fuse       = NULL,
    .fuse_fd    = -1,
    .stop       = 0,
    .inotify_fd = -1,
    .watch_desc = -1
};

static void ucs_vfs_enum_dir_cb(const char *name, void *arg)
{
    ucs_vfs_enum_dir_context_t *ctx = arg;

    ctx->filler(ctx->buf, name, NULL, 0, 0);
}

static int ucs_vfs_fuse_getattr(const char *path, struct stat *stbuf,
                                struct fuse_file_info *fi)
{
    ucs_vfs_path_info_t info;
    ucs_status_t status;

    memset(stbuf, 0, sizeof(struct stat));
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode  = S_IFDIR | S_IRWXU;
        stbuf->st_nlink = 2;
        return 0;
    }

    status = ucs_vfs_path_get_info(path, &info);
    if (status != UCS_OK) {
        return -ENOENT;
    }

    stbuf->st_mode  = info.mode;
    stbuf->st_size  = info.size;
    stbuf->st_nlink = 1;

    return 0;
}

static int ucs_vfs_fuse_open(const char *path, struct fuse_file_info *fi)
{
    ucs_string_buffer_t strb;

    ucs_string_buffer_init(&strb);
    if (ucs_vfs_path_read_file(path, &strb) != UCS_OK) {
        return -ENOENT;
    }

    fi->fh = (uintptr_t)ucs_string_buffer_extract_mem(&strb);

    return 0;
}

static int ucs_vfs_fuse_read(const char *path, char *buf, size_t size,
                             off_t offset, struct fuse_file_info *fi)
{
    char *data    = (void*)fi->fh;
    size_t length = strlen(data);
    size_t nread;

    if (offset >= length) {
        return 0;
    }

    if ((offset + size) <= length) {
        nread = size; /* read does not pass end-of-file */
    } else {
        nread = length - offset; /* read truncated by end-of-file */
    }
    memcpy(buf, data + offset, nread);

    return nread;
}

static int ucs_vfs_fuse_readlink(const char *path, char *buf, size_t size)
{
    ucs_string_buffer_t strb;

    ucs_string_buffer_init_fixed(&strb, buf, size);
    if (ucs_vfs_path_get_link(path, &strb) != UCS_OK) {
        return -ENOENT;
    }

    return 0;
}

static int ucs_vfs_fuse_readdir(const char *path, void *buf,
                                fuse_fill_dir_t filler, off_t offset,
                                struct fuse_file_info *fi,
                                enum fuse_readdir_flags flags)
{
    ucs_vfs_enum_dir_context_t ctx;
    ucs_status_t status;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    ctx.buf    = buf;
    ctx.filler = filler;
    status     = ucs_vfs_path_list_dir(path, ucs_vfs_enum_dir_cb, &ctx);
    if (status != UCS_OK) {
        return -ENOENT;
    }

    return 0;
}

static int ucs_vfs_fuse_release(const char *path, struct fuse_file_info *fi)
{
    char *data = (void*)fi->fh;

    ucs_free(data);
    return 0;
}

static int ucs_vfs_fuse_write(const char *path, const char *buf, size_t size,
                              off_t offset, struct fuse_file_info *fi)
{
    ucs_status_t status;

    if (offset > 0) {
        ucs_warn("cannot write to %s with non-zero offset", path);
        return 0;
    }

    status = ucs_vfs_path_write_file(path, buf, size);
    if (status == UCS_ERR_NO_ELEM) {
        return -ENOENT;
    } else if (status == UCS_ERR_INVALID_PARAM) {
        return -EINVAL;
    } else if (status != UCS_OK) {
        return -EIO;
    }

    return size;
}

struct fuse_operations ucs_vfs_fuse_operations = {
    .getattr  = ucs_vfs_fuse_getattr,
    .open     = ucs_vfs_fuse_open,
    .read     = ucs_vfs_fuse_read,
    .readlink = ucs_vfs_fuse_readlink,
    .readdir  = ucs_vfs_fuse_readdir,
    .release  = ucs_vfs_fuse_release,
    .write    = ucs_vfs_fuse_write,
};

static void ucs_vfs_fuse_main()
{
    struct fuse_args fargs = FUSE_ARGS_INIT(0, NULL);
    char mountpoint_fd[64];
    int ret;

    fuse_opt_add_arg(&fargs, "");

    pthread_mutex_lock(&ucs_vfs_fuse_context.mutex);

    if (ucs_vfs_fuse_context.stop) {
        goto out_unlock;
    }

    ucs_vfs_fuse_context.fuse = fuse_new(&fargs, &ucs_vfs_fuse_operations,
                                         sizeof(ucs_vfs_fuse_operations), NULL);
    if (ucs_vfs_fuse_context.fuse == NULL) {
        ucs_error("fuse_new() failed");
        goto out_unlock;
    }

    ucs_snprintf_safe(mountpoint_fd, sizeof(mountpoint_fd), "/dev/fd/%d",
                      ucs_vfs_fuse_context.fuse_fd);
    ret = fuse_mount(ucs_vfs_fuse_context.fuse, mountpoint_fd);
    if (ret < 0) {
        ucs_error("fuse_mount(%s) failed: %d", mountpoint_fd, ret);
        goto out_destroy;
    }

    /* Drop the lock and execute main loop */
    pthread_mutex_unlock(&ucs_vfs_fuse_context.mutex);

    fuse_loop(ucs_vfs_fuse_context.fuse);

    pthread_mutex_lock(&ucs_vfs_fuse_context.mutex);
out_destroy:
    /* destroy when lock is held */
    fuse_destroy(ucs_vfs_fuse_context.fuse);
    ucs_vfs_fuse_context.fuse = NULL;
out_unlock:
    pthread_mutex_unlock(&ucs_vfs_fuse_context.mutex);
}

static ucs_status_t ucs_vfs_fuse_wait_for_path(const char *path)
{
#ifdef HAVE_INOTIFY
    const char *watch_dirname;
    char *dir_buf;
    char event_buf[sizeof(struct inotify_event) + NAME_MAX];
    const struct inotify_event *event;
    char watch_filename[NAME_MAX];
    ucs_status_t status;
    ssize_t nread;
    size_t offset;
    int ret;

    pthread_mutex_lock(&ucs_vfs_fuse_context.mutex);

    /* Check 'stop' flag before entering the loop. If the main thread sets
     * 'stop' flag before this thread created 'inotify_fd' fd, the execution
     * of the thread has to be stopped, otherwise - the thread hangs waiting
     * for the data on 'inotify_fd' fd.
     */
    if (ucs_vfs_fuse_context.stop) {
        status = UCS_ERR_CANCELED;
        goto out_unlock;
    }

    /* Create directory path */
    ret = ucs_vfs_sock_mkdir(path, UCS_LOG_LEVEL_DIAG);
    if (ret != 0) {
        status = UCS_ERR_IO_ERROR;
        goto out_unlock;
    }

    /* Create inotify channel */
    ucs_vfs_fuse_context.inotify_fd = inotify_init();
    if (ucs_vfs_fuse_context.inotify_fd < 0) {
        if ((errno == EMFILE) &&
            (ucs_sys_check_fd_limit_per_process() == UCS_OK)) {
            ucs_diag("inotify_init() failed: Too many inotify instances. "
                     "Please increase sysctl fs.inotify.max_user_instances to "
                     "avoid the error");
        } else {
            ucs_error("inotify_init() failed: %m");
        }
        status = UCS_ERR_IO_ERROR;
        goto out_unlock;
    }

    status = ucs_string_alloc_path_buffer_and_get_dirname(&dir_buf, "dir_buf",
                                                          path, &watch_dirname);
    if (status != UCS_OK) {
        goto out_unlock;
    }

    /* copy path components to 'watch_filename' */
    ucs_strncpy_safe(watch_filename, ucs_basename(path),
                     sizeof(watch_filename));

    /* Watch for new files in 'watch_dirname' and monitor if this watch gets
     * deleted explicitly or implicitly */
    ucs_vfs_fuse_context.watch_desc = inotify_add_watch(
            ucs_vfs_fuse_context.inotify_fd, watch_dirname,
            IN_CREATE | IN_IGNORED);
    if (ucs_vfs_fuse_context.watch_desc < 0) {
        ucs_error("inotify_add_watch(%s) failed: %m", watch_dirname);
        status = UCS_ERR_IO_ERROR;
        goto out_close_inotify_fd;
    }

    /* Read events from inotify channel and exit when either the main thread set
     * 'stop' flag, or the file was created
     */
    ucs_debug("waiting for creation of '%s' in '%s'", watch_filename,
              watch_dirname);
    for (;;) {
        pthread_mutex_unlock(&ucs_vfs_fuse_context.mutex);
        nread = read(ucs_vfs_fuse_context.inotify_fd, event_buf,
                     sizeof(event_buf));
        pthread_mutex_lock(&ucs_vfs_fuse_context.mutex);

        if (ucs_vfs_fuse_context.stop) {
            status = UCS_ERR_CANCELED;
            break;
        }

        if ((nread < 0) && (errno == EINTR)) {
            ucs_trace("inotify read() failed: %m");
            continue;
        } else if (nread < 0) {
            ucs_error("inotify read() failed: %m");
            status = UCS_ERR_IO_ERROR;
            break;
        }

        /* Go over new events in the buffer */
        for (offset  = 0; offset < nread;
             offset += (sizeof(*event) + event->len)) {
            event = UCS_PTR_BYTE_OFFSET(event_buf, offset);

            /* Watch was removed explicitly (inotify_rm_watch) or automatically
             * (file was deleted, or file system was unmounted). */
            if (event->mask & IN_IGNORED) {
                ucs_debug("inotify watch on '%s' was removed", watch_dirname);
                status = UCS_ERR_IO_ERROR;
                goto out_close_watch_id;
            }

            if (!(event->mask & IN_CREATE)) {
                ucs_trace("ignoring inotify event with mask 0x%x", event->mask);
                continue;
            }

            ucs_trace("file '%s' created", event->name);
            /* event->len is a multiple of 16, not the string length */
            /* coverity[tainted_data] */
            if ((event->len < (strlen(watch_filename) + 1)) ||
                (strncmp(event->name, watch_filename, event->len) != 0)) {
                ucs_trace("ignoring inotify create event of '%s'", event->name);
                continue;
            }

            status = UCS_OK;
            goto out_close_watch_id;
        }
    }

out_close_watch_id:
    inotify_rm_watch(ucs_vfs_fuse_context.inotify_fd,
                     ucs_vfs_fuse_context.watch_desc);
out_close_inotify_fd:
    close(ucs_vfs_fuse_context.inotify_fd);
    ucs_vfs_fuse_context.inotify_fd = -1;
    ucs_free(dir_buf);
out_unlock:
    pthread_mutex_unlock(&ucs_vfs_fuse_context.mutex);
    return status;
#else
    return UCS_ERR_UNSUPPORTED;
#endif
}

static void ucs_vfs_fuse_thread_reset_affinity()
{
    ucs_sys_cpuset_t cpuset;
    long i, num_cpus;

    num_cpus = ucs_sys_get_num_cpus();
    if (num_cpus == -1) {
        return;
    }

    CPU_ZERO(&cpuset);
    for (i = 0; i < num_cpus; ++i) {
        CPU_SET(i, &cpuset);
    }

    if (ucs_sys_setaffinity(&cpuset) == -1) {
        ucs_diag("failed to set affinity: %m");
    }
}

static void *ucs_vfs_fuse_thread_func(void *arg)
{
    ucs_vfs_sock_message_t vfs_msg_in, vfs_msg_out;
    struct sockaddr_un un_addr;
    ucs_status_t status;
    int connfd;
    int ret;

    ucs_log_set_thread_name("f");

    if (!ucs_global_opts.vfs_thread_affinity) {
        ucs_vfs_fuse_thread_reset_affinity();
    }

    connfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connfd < 0) {
        ucs_error("failed to create VFS socket: %m");
        goto out;
    }

again:
    ucs_vfs_sock_get_address(&un_addr);
    ucs_debug("connecting vfs socket %d to daemon on '%s'", connfd,
              un_addr.sun_path);
    ret = connect(connfd, (const struct sockaddr*)&un_addr, sizeof(un_addr));
    if (ret < 0) {
        /* VFS daemon is not listening. Set up a file watch on the unix socket
         * path, to retry when the daemon is started.
         */
        if ((errno == ECONNREFUSED) || (errno == ENOENT)) {
            ucs_debug("failed to connect to vfs socket '%s': %m",
                      un_addr.sun_path);
            status = ucs_vfs_fuse_wait_for_path(un_addr.sun_path);
            if (status == UCS_OK) {
                goto again;
            }

            if (!ucs_vfs_fuse_context.stop) {
                ucs_diag("failed to watch on '%s': %s, VFS will be disabled",
                         un_addr.sun_path, ucs_status_string(status));
            }
        } else {
            ucs_diag("failed to connect to vfs socket '%s': %m",
                     un_addr.sun_path);
        }
        goto out_close;
    }

    ucs_debug("sending vfs mount request on socket %d", connfd);
    vfs_msg_out.action = UCS_VFS_SOCK_ACTION_MOUNT;
    ret                = ucs_vfs_sock_send(connfd, &vfs_msg_out);
    if (ret < 0) {
        ucs_warn("failed to send mount action to vfs daemon: %s",
                 strerror(-ret));
        goto out_close;
    }

    ret = ucs_vfs_sock_recv(connfd, &vfs_msg_in);
    if (ret < 0) {
        ucs_warn("failed to receive mount reply from vfs daemon: %s",
                 strerror(-ret));
        goto out_close;
    }

    ucs_vfs_fuse_context.fuse_fd = vfs_msg_in.fd;
    ucs_vfs_fuse_main();
    close(vfs_msg_in.fd);

out_close:
    close(connfd);
out:
    return NULL;
}

static void ucs_fuse_replace_fd_devnull()
{
    int devnull_fd;

    devnull_fd = open("/dev/null", O_RDWR);
    if (devnull_fd < 0) {
        ucs_warn("failed to open /dev/null: %m");
        return;
    }

    /* force exiting from fuse event loop, which reads from fuse_fd */
    ucs_assert(ucs_vfs_fuse_context.fuse_fd != -1);
    ucs_debug("dup2(%d, %d)", devnull_fd, ucs_vfs_fuse_context.fuse_fd);
    dup2(devnull_fd, ucs_vfs_fuse_context.fuse_fd);
    close(devnull_fd);
}

static void ucs_fuse_thread_stop()
{
    sighandler_t orig_handler;
    int ret;

    orig_handler = signal(SIGUSR1, (sighandler_t)ucs_empty_function);

    pthread_mutex_lock(&ucs_vfs_fuse_context.mutex);

    ucs_vfs_fuse_context.stop = 1;

    /* If the thread is waiting in inotify loop, wake it */
    if (ucs_vfs_fuse_context.inotify_fd >= 0) {
#ifdef HAVE_INOTIFY
        ret = inotify_rm_watch(ucs_vfs_fuse_context.inotify_fd,
                               ucs_vfs_fuse_context.watch_desc);
        if (ret != 0) {
            ucs_warn("inotify_rm_watch(fd=%d, wd=%d) failed: %m",
                     ucs_vfs_fuse_context.inotify_fd,
                     ucs_vfs_fuse_context.watch_desc);
        }
#endif
    }

    /* If the thread is in fuse loop, terminate it */
    if (ucs_vfs_fuse_context.fuse != NULL) {
        fuse_exit(ucs_vfs_fuse_context.fuse);
        ucs_fuse_replace_fd_devnull();
        pthread_kill(ucs_vfs_fuse_context.thread_id, SIGUSR1);
    }

    pthread_mutex_unlock(&ucs_vfs_fuse_context.mutex);

    ret = pthread_join(ucs_vfs_fuse_context.thread_id, NULL);
    if (ret != 0) {
        ucs_warn("pthread_join(0x%lx) failed: %m",
                 ucs_vfs_fuse_context.thread_id);
    }

    signal(SIGUSR1, orig_handler);
}

static void ucs_vfs_fuse_atfork_child()
{
    /* Reset thread context at fork, since doing inotify_rm_watch() from child
       will prevent doing it later from the parent */
    ucs_vfs_fuse_context.thread_id  = -1;
    ucs_vfs_fuse_context.fuse       = NULL;
    ucs_vfs_fuse_context.fuse_fd    = -1;
    ucs_vfs_fuse_context.inotify_fd = -1;
    ucs_vfs_fuse_context.watch_desc = -1;
}

void UCS_F_CTOR ucs_vfs_fuse_init()
{
    if (ucs_global_opts.vfs_enable) {
        pthread_atfork(NULL, NULL, ucs_vfs_fuse_atfork_child);
        ucs_pthread_create(&ucs_vfs_fuse_context.thread_id,
                           ucs_vfs_fuse_thread_func, NULL, "fuse");
    }
}

void UCS_F_DTOR ucs_vfs_fuse_cleanup()
{
    if (ucs_vfs_fuse_context.thread_id != -1) {
        ucs_fuse_thread_stop();
    }
}
