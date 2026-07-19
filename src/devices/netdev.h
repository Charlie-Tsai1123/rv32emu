/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

/*
 * Networking backends follow semu's networking.md:
 *
 * Linux:
 *   - tap  : kernel TAP device
 *   - user : user-mode SLIRP
 *
 * macOS:
 *   - vmnet: vmnet.framework shared/NAT mode
 *   - user : user-mode SLIRP
 */
#if defined(__APPLE__)
#define NETDEV_SUPPORTED_DEVICES \
    _(vmnet)                     \
    _(user)
#else
#define NETDEV_SUPPORTED_DEVICES \
    _(tap)                       \
    _(user)
#endif

typedef struct netdev netdev_t;

typedef enum {
#define _(dev) NETDEV_IMPL_##dev,
    NETDEV_SUPPORTED_DEVICES
#undef _
} netdev_impl_t;

typedef struct {
    int tap_fd;
} net_tap_options_t;

/* user-mode SLIRP backend */
#define SLIRP_PKT_MAX 16384
#define SLIRP_READ_SIDE 0
#define SLIRP_WRITE_SIDE 1

typedef struct {
    void *slirp;
    int guest_to_host_channel[2];
    int host_to_guest_channel[2];
    struct pollfd *pfd;
    int pfd_len;
    int pfd_size;
    void *timer;
} net_user_options_t;

int net_slirp_init(net_user_options_t *usr);
void net_slirp_cleanup(net_user_options_t *usr);
int net_slirp_poll(net_user_options_t *usr);
int net_slirp_read(net_user_options_t *usr);

#if defined(__APPLE__)
#include <pthread.h>

typedef enum {
    NET_VMNET_SHARED = 0,
    NET_VMNET_HOST,
    NET_VMNET_BRIDGED,
} net_vmnet_mode_t;

typedef struct {
    void *iface;     /* interface_ref */
    void *queue;     /* dispatch_queue_t */
    void *sem;       /* dispatch_semaphore_t */
    int pipe_fds[2]; /* pipe for poll() integration */
    uint8_t mac[6];
    pthread_mutex_t lock;
    bool running;
} net_vmnet_options_t;

typedef net_vmnet_options_t net_vmnet_state_t;

int net_vmnet_init(netdev_t *netdev,
                   net_vmnet_mode_t mode,
                   const char *iface_name);
ssize_t net_vmnet_read(net_vmnet_state_t *state, uint8_t *buf, size_t len);
ssize_t net_vmnet_writev(net_vmnet_state_t *state,
                         const struct iovec *iov,
                         size_t iovcnt);
int net_vmnet_get_fd(net_vmnet_state_t *state);
void net_vmnet_cleanup(net_vmnet_state_t *state);
#endif

struct netdev {
    const char *name;
    netdev_impl_t type;
    void *op;
};

bool netdev_init(netdev_t *netdev, const char *net_type);

void netdev_delete(netdev_t *netdev);