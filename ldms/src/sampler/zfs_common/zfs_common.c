
/* -*- c-basic-offser: 8 -*- */
/* Copyright 2022 Lawrence Livermore National Security, LLC
 * See the top-level COPYING file for details.
 *
 * SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
 */

#define _GNU_SOURCE
/* Next we include the headers to bring in the zfslib in action */
#include <stdlib.h>
#include <ctype.h>
#include <glob.h>
#include <string.h>
#include <getopt.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <unistd.h>
#include <libzfs.h>
#include <libzutil.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ldms.h"
#include "ldmsd.h"
#include "config.h"
#include "sampler_base.h"
#include "zfs_common/zfs_common.h"
#include <stddef.h>

char * escape_string(char *s)
{
        char *c, *d;
        char *t = (char *)malloc(ZFS_MAX_DATASET_NAME_LEN * 2);
        if (t == NULL) {
                fprintf(stderr, "error: cannot allocate memory\n");
                exit(1);
        }

        for (c = s, d = t; *c != '\0'; c++, d++) {
                switch (*c) {
                case ' ':
                case ',':
                case '=':
                case '\\':
                        *d++ = '\\';
                        fallthrough;
                default:
                        *d = *c;
                }
        }
        *d = '\0';
        return (t);
}

static int get_zpool_vdevs_count(nvlist_t *nvroot, const char *pool_name, vdevs_count_ *vdevscount)
{
        uint_t     children;
        nvlist_t **child;
	char       vdev_name[256];


        if (nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
            &child, &children) == 0) {

		if (vdevscount->counttype == TOPVDEV_COUNT)
			vdevscount->vdev_count += children;
		if (vdevscount->counttype == LEAFVDEV_COUNT) {
			for (int c = 0; c < children; c++) {
				get_zpool_vdevs_count(child[c], pool_name,
							vdevscount);
			}
		}
        } else {
		if (vdevscount->counttype == LEAFVDEV_COUNT)
			vdevscount->vdev_count++;
	}
	if (vdevscount->counttype == TOPVDEV_COUNT)
		vdevscount->vdev_count++; /* add one for vdev root in zpool top.*/
        return (0);
}

int get_vdevs_count(zpool_handle_t *zhp, void *data)
{
        uint_t          c;
        int             err;
        boolean_t       missing;
        nvlist_t       *config, *nvroot;
        vdev_stat_t    *vs;
        struct timespec tv;
        char           *pool_name;

        if (zpool_refresh_stats(zhp, &missing) != 0) {
                zpool_close(zhp);
                return (1);
        }

        config = zpool_get_config(zhp, NULL);

        if (nvlist_lookup_nvlist(
            config, ZPOOL_CONFIG_VDEV_TREE, &nvroot) != 0) {
		zpool_close(zhp);
                return (2);
        }
        if (nvlist_lookup_uint64_array(nvroot, ZPOOL_CONFIG_VDEV_STATS,
            (uint64_t **)&vs, &c) != 0) {
		zpool_close(zhp);
		return (3);
        }

        pool_name = (char *)zpool_get_name(zhp);
        err = get_zpool_vdevs_count(nvroot, pool_name, (vdevs_count_ *) data);
        zpool_close(zhp);
        return (err);
}

/*
 * get a vdev name that corresponds to the top-level vdev names
 * printed by `zpool status`
 */
char * get_vdev_name(nvlist_t *nvroot, const char *parent_name)
{
        static char vdev_name[256];
        char *vdev_type = NULL;
        uint64_t vdev_id = 0;

        if (nvlist_lookup_string(nvroot, ZPOOL_CONFIG_TYPE,
            &vdev_type) != 0) {
                vdev_type = "unknown";
        }
        if (nvlist_lookup_uint64(
            nvroot, ZPOOL_CONFIG_ID, &vdev_id) != 0) {
                vdev_id = UINT64_MAX;
        }
        if (parent_name == NULL) {
                (void) snprintf(vdev_name, sizeof (vdev_name), "%s",
                    vdev_type);
        } else {
                (void) snprintf(vdev_name, sizeof (vdev_name),
                    "%s/%s-%llu",
                    parent_name, vdev_type, (u_longlong_t)vdev_id);
        }
        return (vdev_name);
}
