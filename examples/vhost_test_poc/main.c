/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017-2018 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>
#include <getopt.h>

#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_vhost.h>

#include <cmdline_rdline.h>
#include <cmdline_parse.h>
#include <cmdline_parse_string.h>
#include <cmdline.h>

#include "virtual_vhost.h"

#define NB_VIRTIO_QUEUES		(1)
#define MAX_PKT_BURST			(64)
#define MAX_IV_LEN			(32)
#define NB_MEMPOOL_OBJS			(8192)
#define NB_CRYPTO_DESCRIPTORS		(4096)
#define NB_CACHE_OBJS			(128)
#define SESSION_MAP_ENTRIES		(1024)
#define REFRESH_TIME_SEC		(3)

#define MAX_NB_SOCKETS			(32)
#define DEF_SOCKET_FILE			"/tmp/vhost_test1.socket"

uint64_t vhost_cycles[2], last_v_cycles[2];
uint64_t outpkt_amount;

static int
new_device(int vid)
{
	char path[PATH_MAX];
	int ret;

	ret = rte_vhost_get_ifname(vid, path, PATH_MAX);
	if (ret) {
		RTE_LOG(ERR, USER1, "Cannot find matched socket\n");
		return ret;
	}

	/* Use only one socket for now */
	if (strcmp(path, DEF_SOCKET_FILE) == 0) {
		RTE_LOG(ERR, USER1, "Cannot find recorded socket\n");
		return -ENOENT;
	}

	RTE_LOG(INFO, USER1, "New Vhost-test Device %s, Device ID %d\n", path,
			vid);

	return 0;
}

static void
destroy_device(int vid)
{
	if (vid != 0) {
		RTE_LOG(ERR, USER1, "Cannot find socket file from list\n");
		return;
	}

	RTE_LOG(INFO, USER1, "Vhost Test Device %i Removed\n", vid);
}

static const struct vhost_device_ops virtio_test_device_ops = {
	.new_device =  new_device,
	.destroy_device = destroy_device,
};

static int
vhost_test_master(__rte_unused void *arg)
{
	struct virtual_vhost *vhost = NULL;
	ssize_t n;

	vhost = virtual_vhost_create(DEF_SOCKET_FILE);
	if (vhost == NULL) {
		printf("Booooo!!\n");
		return -1;
	}

	//FIXIT: wait until vhost slave finishes initialization except polling
	//       socket for connection
	while (virtual_vhost_connect(vhost) != 0);

	printf("[SLAVE] vring_num = %d\n", rte_vhost_get_vring_num(0));

	struct VhostUserMsg msg;
	msg.request.master = VHOST_USER_GET_FEATURES;
	msg.size = 0;

	virtual_vhost_send_message(vhost, &msg);
	n = virtual_vhost_recv_message(vhost, &msg);

	printf("Received %ld bytes (Message size %d)\n", n, msg.size);
	printf("Features: %" PRIx64 "\n", msg.payload.u64);


	msg.request.master = VHOST_USER_SET_FEATURES;
	msg.payload.u64 = 0;
	msg.size = sizeof(msg.payload.u64);

	virtual_vhost_send_message(vhost, &msg);

	/* Should we wait here? */
	sleep(1);

	msg.request.master = VHOST_USER_GET_FEATURES;
	msg.size = 0;
	virtual_vhost_send_message(vhost, &msg);
	n = virtual_vhost_recv_message(vhost, &msg);

	printf("Received %ld bytes (Message size %d)\n", n, msg.size);
	printf("Features: %" PRIx64 "\n", msg.payload.u64);


	memset(&msg, 0, sizeof(msg));
	msg.request.master = VHOST_USER_SET_MEM_TABLE;
	msg.size = sizeof(msg.payload.memory) + sizeof(msg.fds[0]);
	msg.payload.memory.nregions = 1;
	msg.payload.memory.padding = 0;
	msg.payload.memory.regions[0].guest_phys_addr = vhost->guest_phys_addr;
	msg.payload.memory.regions[0].memory_size = vhost->memory_size;
	msg.payload.memory.regions[0].mmap_offset = 0; /* FIXIT: change it! */
	msg.payload.memory.regions[0].userspace_addr = vhost->userspace_addr;
	msg.fds[0] = vhost->shmfd;

	printf("[MASTER] fd = %d\n", msg.fds[0]);

	virtual_vhost_send_message(vhost, &msg);


	memset(&msg, 0, sizeof(msg));
	msg.request.master = VHOST_USER_SET_VRING_ADDR;
	msg.size = sizeof(msg.payload.addr);

	msg.payload.addr.index = 0;
	msg.payload.addr.avail_user_addr = (uint64_t)vhost->avail;
	msg.payload.addr.desc_user_addr = (uint64_t)vhost->desc;
	msg.payload.addr.used_user_addr = (uint64_t)vhost->used;

	virtual_vhost_send_message(vhost, &msg);

	memset(&msg, 0, sizeof(msg));
	msg.request.master = VHOST_USER_SET_VRING_ENABLE;
	msg.payload.state.index = 0;
	msg.size = sizeof(msg.payload.state);
	virtual_vhost_send_message(vhost, &msg);

	memset(&msg, 0, sizeof(msg));
	msg.request.master = VHOST_USER_SET_OWNER;
	msg.size = 0;
	virtual_vhost_send_message(vhost, &msg);

	return 0;
}

#if 0
static int
vhost_test_worker(__rte_unused void *arg)
{
	uint32_t lcore_id = rte_lcore_id();
	int ret = 0;

	RTE_LOG(INFO, USER1, "Processing on Core %u started\n", lcore_id);

	while (1) {
		/* Only one vid */
		for (i = 0; i < 1; i++) {
		}
	}

	return ret;
}
#endif

static void
unregister_drivers(__rte_unused int socket_num)
{
	int ret;

	ret = rte_vhost_driver_unregister(DEF_SOCKET_FILE);
	if (ret != 0)
		RTE_LOG(ERR, USER1,
			"Fail to unregister vhost driver for %s.\n",
			DEF_SOCKET_FILE);
}

int
main(int argc, char *argv[])
{
	uint32_t worker_lcore;
	int ret;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		return -1;
	argc -= ret;
	argv += ret;

	worker_lcore = rte_get_next_lcore(0, 1, 0);
	if (worker_lcore == RTE_MAX_LCORE)
		rte_exit(EXIT_FAILURE, "Not enough lcore\n");

	if (rte_eal_remote_launch(vhost_test_master, NULL, worker_lcore)
			< 0) {
		RTE_LOG(ERR, USER1, "Failed to start worker lcore");
		goto error_exit;
	}

	unlink(DEF_SOCKET_FILE);

	if (rte_vhost_driver_register(DEF_SOCKET_FILE,
			RTE_VHOST_USER_DEQUEUE_ZERO_COPY) < 0) {
		RTE_LOG(ERR, USER1, "socket %s already exists\n",
				DEF_SOCKET_FILE);
		goto error_exit;
	}

	rte_vhost_driver_callback_register(DEF_SOCKET_FILE,
			&virtio_test_device_ops);

	if (rte_vhost_driver_start(DEF_SOCKET_FILE) < 0) {
		RTE_LOG(ERR, USER1, "failed to start vhost driver.\n");
		goto error_exit;
	}

	RTE_LCORE_FOREACH(worker_lcore)
		rte_eal_wait_lcore(worker_lcore);

	return 0;

error_exit:
	unregister_drivers(0);

	return -1;
}
