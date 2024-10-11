/*
 *  vmnet - Tun device emulation for Darwin
 *  Copyright (C) 2024 Eric Karge
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "vmnet.h"
#include <sys/uio.h>
#include <sys/types.h>
#include <stdint.h>
#include <vmnet/vmnet.h>
#include <dispatch/dispatch.h>
#include <sys/socket.h>
#include "../logger.h"
#include <errno.h>

static volatile vmnet_return_t if_status = VMNET_SETUP_INCOMPLETE;
static dispatch_queue_t if_queue;
static interface_ref vmnet_if;
static size_t max_packet_size;
static struct iovec read_iov_in;
static int read_socket[2];

static void macos_vmnet_read();
static const char *str_vmnet_status(vmnet_return_t status);

int macos_vmnet_open(void) {
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, read_socket)) {
		logger(LOG_ERR, "Unable to create socket: %s", strerror(errno));
		return -1;
	}

	xpc_object_t if_desc = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_uint64(if_desc, vmnet_operation_mode_key, VMNET_HOST_MODE);
	xpc_dictionary_set_bool(if_desc, vmnet_enable_isolation_key, 1);
	xpc_dictionary_set_bool(if_desc, vmnet_allocate_mac_address_key, false);
    xpc_dictionary_set_string(if_desc, vmnet_start_address_key, "10.255.2.77");
    xpc_dictionary_set_string(if_desc, vmnet_end_address_key, "10.255.2.255");
    xpc_dictionary_set_string(if_desc, vmnet_subnet_mask_key, "255.255.255.0");

	if_queue = dispatch_queue_create("org.tinc-vpn.vmnet.if_queue", DISPATCH_QUEUE_SERIAL);
	dispatch_semaphore_t if_started_sem = dispatch_semaphore_create(0);
	vmnet_if = vmnet_start_interface(if_desc, if_queue, ^(vmnet_return_t status, xpc_object_t interface_param) {
		if_status = status;
		if (status == VMNET_SUCCESS && interface_param) {
    		max_packet_size = xpc_dictionary_get_uint64(interface_param, vmnet_max_packet_size_key);
		}
		dispatch_semaphore_signal(if_started_sem);
	});
	dispatch_semaphore_wait(if_started_sem, DISPATCH_TIME_FOREVER);
	dispatch_release(if_started_sem);

	xpc_release(if_desc);

	if(if_status != VMNET_SUCCESS) {
		logger(LOG_ERR, "Unable to create vmnet device: %s", str_vmnet_status(if_status));
		return -1;
	}

    read_iov_in.iov_base = malloc(max_packet_size);
    read_iov_in.iov_len = max_packet_size;

	vmnet_interface_set_event_callback(
		vmnet_if, VMNET_INTERFACE_PACKETS_AVAILABLE, if_queue,
		^(interface_event_t event_id, xpc_object_t event) {
	    	macos_vmnet_read();
    	});

	return read_socket[0];
}

int macos_vmnet_close(int fd) {
	dispatch_semaphore_t if_stopped_sem;

	if (vmnet_if == NULL || fd != read_socket[0]) {
		logger(LOG_ERR, "Attempt to close broken vmnet device: %s", strerror(errno));
		return -1;
	}
	vmnet_interface_set_event_callback(vmnet_if, VMNET_INTERFACE_PACKETS_AVAILABLE, NULL, NULL);

	if_stopped_sem = dispatch_semaphore_create(0);
	vmnet_stop_interface(vmnet_if, if_queue, ^(vmnet_return_t status) {
        if_status = status;
        dispatch_semaphore_signal(if_stopped_sem);
    });
	dispatch_semaphore_wait(if_stopped_sem, DISPATCH_TIME_FOREVER);
	dispatch_release(if_stopped_sem);

	if (if_status != VMNET_SUCCESS) {
		logger(LOG_ERR, "Unable to properly close vmnet device: %s", strerror(errno));
		return -1;
	}

	dispatch_release(if_queue);

    read_iov_in.iov_len = 0;
    free(read_iov_in.iov_base);
    read_iov_in.iov_base = NULL;

	close(read_socket[0]);
	close(read_socket[1]);

	return 0;
}

void macos_vmnet_read() {
    if (if_status != VMNET_SUCCESS) {
        return;
    }

    struct vmpktdesc packet = {
        .vm_flags = 0,
        .vm_pkt_size = max_packet_size,
        .vm_pkt_iov = &read_iov_in,
        .vm_pkt_iovcnt = 1,
    };

    int count = 1;
    if_status = vmnet_read(vmnet_if, &packet, &count);
    if (if_status != VMNET_SUCCESS) {
        logger(LOG_ERR, "Unable to read packet: %s", str_vmnet_status(if_status));
        return;
    }

    if ( count && packet.vm_pkt_iovcnt ) {
        struct iovec iov_out = {
            .iov_base = packet.vm_pkt_iov->iov_base,
            .iov_len = packet.vm_pkt_size,
        };
        if(writev(read_socket[1], &iov_out, 1) < 0) {
            logger(LOG_ERR, "Unable to write to read socket: %s", strerror(errno));
            return;
        }
    }
}

ssize_t macos_vmnet_write(uint8_t *buffer, size_t buflen) {
	struct vmpktdesc packet;
	struct iovec iov = {
		.iov_base = (char *) buffer,
		.iov_len = buflen,
	};
	int pkt_cnt;
	vmnet_return_t if_status;

	if (buflen > max_packet_size) {
		logger(LOG_ERR, "Max packet size (%zd) exceeded: %zd", max_packet_size, buflen);
		return -1;
	}

	packet.vm_pkt_iovcnt = 1;
	packet.vm_flags = 0;
	packet.vm_pkt_size = buflen;
	packet.vm_pkt_iov = &iov;
	pkt_cnt = 1;

	if_status = vmnet_write(vmnet_if, &packet, &pkt_cnt);
	if (if_status != VMNET_SUCCESS) {
		logger(LOG_ERR, "Write failed");
		return -1;
	}

	return pkt_cnt ? buflen : 00;
}

const char *str_vmnet_status(vmnet_return_t status) {
    switch (status) {
    case VMNET_SUCCESS:
        return "success";
    case VMNET_FAILURE:
        return "general failure (possibly not enough privileges)";
    case VMNET_MEM_FAILURE:
        return "memory allocation failure";
    case VMNET_INVALID_ARGUMENT:
        return "invalid argument specified";
    case VMNET_SETUP_INCOMPLETE:
        return "interface setup is not complete";
    case VMNET_INVALID_ACCESS:
        return "invalid access, permission denied";
    case VMNET_PACKET_TOO_BIG:
        return "packet size is larger than MTU";
    case VMNET_BUFFER_EXHAUSTED:
        return "buffers exhausted in kernel";
    case VMNET_TOO_MANY_PACKETS:
        return "packet count exceeds limit";
    case VMNET_SHARING_SERVICE_BUSY:
        return "conflict, sharing service is in use";
    default:
    	return "unknown vmnet error";
    }
}
