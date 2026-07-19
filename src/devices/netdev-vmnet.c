/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

 /*
 * vmnet.framework based network backend for macOS
 *
 * Supports three modes:
 * - shared: NAT + DHCP (default)
 * - host: isolated network for VM-to-VM communication
 * - bridged: bridge with physical network interface
 *
 * Requires macOS 11.0+ and root privileges or com.apple.vm.networking
 * entitlement
 */

#if defined(__APPLE__)

#include "netdev.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <dispatch/dispatch.h>
#include <vmnet/vmnet.h>

#include "utils.h"

#define VMNET_BUF_SIZE 2048

static void vmnet_packet_handler(net_vmnet_state_t *state,
                                 uint8_t *buf,
                                 ssize_t len)
{
    if (len <= 0)
        return;

    pthread_mutex_lock(&state->lock);

    uint32_t pkt_len = (uint32_t) len;
    if (write(state->pipe_fds[1], &pkt_len, sizeof(pkt_len)) !=
        (ssize_t) sizeof(pkt_len)) {
        rv_log_error("vmnet: failed to write packet size to pipe");
        pthread_mutex_unlock(&state->lock);
        return;
    }

    ssize_t written = write(state->pipe_fds[1], buf, (size_t) len);
    if (written != len)
        rv_log_error("vmnet: failed to write packet to pipe: %zd/%zd", written,
                     len);

    pthread_mutex_unlock(&state->lock);
}

static int vmnet_start(net_vmnet_state_t *state,
                       uint64_t mode,
                       const char *iface_name)
{
    xpc_object_t iface_desc = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(iface_desc, vmnet_operation_mode_key, mode);

    if (mode == VMNET_BRIDGED_MODE && iface_name && iface_name[0])
        xpc_dictionary_set_string(iface_desc, vmnet_shared_interface_name_key,
                                  iface_name);

    state->sem = dispatch_semaphore_create(0);
    state->queue =
        dispatch_queue_create("org.rv32emu.vmnet", DISPATCH_QUEUE_SERIAL);

    __block interface_ref iface = NULL;
    __block vmnet_return_t status = VMNET_FAILURE;

    iface = vmnet_start_interface(
        iface_desc, state->queue, ^(vmnet_return_t ret, xpc_object_t param) {
          status = ret;
          if (ret == VMNET_SUCCESS) {
              const char *mac_str =
                  xpc_dictionary_get_string(param, vmnet_mac_address_key);
              if (mac_str) {
                  sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                         &state->mac[0], &state->mac[1], &state->mac[2],
                         &state->mac[3], &state->mac[4], &state->mac[5]);
              }

              vmnet_interface_set_event_callback(
                  iface, VMNET_INTERFACE_PACKETS_AVAILABLE, state->queue,
                  ^(interface_event_t event_id, xpc_object_t event) {
                    (void) event_id;
                    (void) event;

                    struct vmpktdesc pkts[32];
                    uint8_t bufs[32][VMNET_BUF_SIZE];
                    struct iovec iovs[32];
                    int pkt_cnt = (int) (sizeof(pkts) / sizeof(pkts[0]));

                    for (int i = 0; i < pkt_cnt; i++) {
                        iovs[i].iov_base = bufs[i];
                        iovs[i].iov_len = sizeof(bufs[i]);
                        pkts[i].vm_pkt_size = sizeof(bufs[i]);
                        pkts[i].vm_pkt_iov = &iovs[i];
                        pkts[i].vm_pkt_iovcnt = 1;
                        pkts[i].vm_flags = 0;
                    }

                    int received = pkt_cnt;
                    vmnet_return_t r = vmnet_read(iface, pkts, &received);
                    if (r != VMNET_SUCCESS) {
                        rv_log_error("vmnet: read failed: %d", r);
                        return;
                    }

                    for (int i = 0; i < received; i++) {
                        vmnet_packet_handler(state, bufs[i],
                                             pkts[i].vm_pkt_size);
                    }
                  });
          }

          dispatch_semaphore_signal(state->sem);
        });

    dispatch_semaphore_wait(state->sem, DISPATCH_TIME_FOREVER);

    if (status != VMNET_SUCCESS) {
        rv_log_error("vmnet: failed to create interface: %d", status);
        xpc_release(iface_desc);
        return -1;
    }

    state->iface = iface;
    xpc_release(iface_desc);

    rv_log_info("vmnet: interface started");
    return 0;
}

int net_vmnet_init(netdev_t *netdev,
                   net_vmnet_mode_t mode,
                   const char *iface_name)
{
    net_vmnet_state_t *state = (net_vmnet_state_t *) netdev->op;

    if (pipe(state->pipe_fds) < 0) {
        rv_log_error("vmnet: failed to create pipe: %s", strerror(errno));
        return -1;
    }

    int flags = fcntl(state->pipe_fds[0], F_GETFL, 0);
    if (flags < 0 || fcntl(state->pipe_fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        rv_log_error("vmnet: failed to set pipe non-blocking mode: %s",
                     strerror(errno));
        close(state->pipe_fds[0]);
        close(state->pipe_fds[1]);
        return -1;
    }

    pthread_mutex_init(&state->lock, NULL);
    state->running = true;

    uint64_t vmnet_mode = VMNET_SHARED_MODE;
    switch (mode) {
    case NET_VMNET_SHARED:
        vmnet_mode = VMNET_SHARED_MODE;
        break;
    case NET_VMNET_HOST:
        vmnet_mode = VMNET_HOST_MODE;
        break;
    case NET_VMNET_BRIDGED:
        vmnet_mode = VMNET_BRIDGED_MODE;
        break;
    default:
        rv_log_error("vmnet: unknown mode: %d", mode);
        return -1;
    }

    if (vmnet_start(state, vmnet_mode, iface_name) < 0) {
        close(state->pipe_fds[0]);
        close(state->pipe_fds[1]);
        pthread_mutex_destroy(&state->lock);
        return -1;
    }

    return 0;
}

ssize_t net_vmnet_read(net_vmnet_state_t *state, uint8_t *buf, size_t len)
{
    uint32_t pkt_len;
    ssize_t n = read(state->pipe_fds[0], &pkt_len, sizeof(pkt_len));

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -1;

        rv_log_error("vmnet: failed to read packet size: %s", strerror(errno));
        return -1;
    }

    if (n != (ssize_t) sizeof(pkt_len)) {
        rv_log_error("vmnet: partial read of packet size");
        return -1;
    }

    if (pkt_len > len) {
        rv_log_error("vmnet: packet too large: %u > %zu", pkt_len, len);

        uint8_t tmp[VMNET_BUF_SIZE];
        size_t remaining = pkt_len;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(tmp) ? sizeof(tmp) : remaining;
            n = read(state->pipe_fds[0], tmp, chunk);
            if (n <= 0)
                break;
            remaining -= (size_t) n;
        }

        return -1;
    }

    ssize_t total = 0;
    while (total < (ssize_t) pkt_len) {
        n = read(state->pipe_fds[0], buf + total, pkt_len - (size_t) total);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;

            rv_log_error("vmnet: failed to read packet data: %s",
                         strerror(errno));
            return -1;
        }

        total += n;
    }

    return total;
}

ssize_t net_vmnet_writev(net_vmnet_state_t *state,
                         const struct iovec *iov,
                         size_t iovcnt)
{
    if (!state->running || !state->iface)
        return -1;

    size_t total_len = 0;
    for (size_t i = 0; i < iovcnt; i++)
        total_len += iov[i].iov_len;

    struct vmpktdesc pkt = {
        .vm_pkt_size = total_len,
        .vm_pkt_iov = (struct iovec *) iov,
        .vm_pkt_iovcnt = (int) iovcnt,
        .vm_flags = 0,
    };

    int pkt_cnt = 1;
    vmnet_return_t ret =
        vmnet_write((interface_ref) state->iface, &pkt, &pkt_cnt);

    if (ret != VMNET_SUCCESS) {
        rv_log_error("vmnet: writev failed: %d", ret);
        return -1;
    }

    return pkt_cnt > 0 ? (ssize_t) total_len : -1;
}

int net_vmnet_get_fd(net_vmnet_state_t *state)
{
    return state->pipe_fds[0];
}

void net_vmnet_cleanup(net_vmnet_state_t *state)
{
    if (!state)
        return;

    state->running = false;

    if (state->iface) {
        vmnet_stop_interface((interface_ref) state->iface, state->queue,
                             ^(vmnet_return_t ret) { (void) ret; });
        state->iface = NULL;
    }

    if (state->queue) {
        dispatch_release((dispatch_queue_t) state->queue);
        state->queue = NULL;
    }

    if (state->sem) {
        dispatch_release((dispatch_semaphore_t) state->sem);
        state->sem = NULL;
    }

    if (state->pipe_fds[0] >= 0)
        close(state->pipe_fds[0]);
    if (state->pipe_fds[1] >= 0)
        close(state->pipe_fds[1]);

    pthread_mutex_destroy(&state->lock);
}

#endif /* defined(__APPLE__) */