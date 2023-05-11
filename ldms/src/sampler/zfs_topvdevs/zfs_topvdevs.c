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


#define SAMP "zpools_stats"
#define MAX_LINE_LEN 1024

#ifndef IFNAMSIZ
/* from "linux/if.h" */
#define IFNAMSIZ 16
#endif

#ifndef PATH_MAX
#define PATH_MAX 2048
#endif

#define ZPOOL_VDEV_LIST_SNAME  "zpool_vdev_list"
/* Function prototypes */

int
get_stats(zpool_handle_t *zhp, void *data);

#define POOL_MEASUREMENT         "zpool_stats"
#define SCAN_MEASUREMENT         "zpool_scan_stats"
#define VDEV_MEASUREMENT         "zpool_vdev_stats"
#define POOL_LATENCY_MEASUREMENT "zpool_latency"
#define POOL_QUEUE_MEASUREMENT   "zpool_vdev_queue"
#define MIN_LAT_INDEX   10  /* minimum latency index 10 = 1024ns */
#define POOL_IO_SIZE_MEASUREMENT        "zpool_io_size"
#define MIN_SIZE_INDEX  9  /* minimum size index 9 = 512 bytes */

typedef int (*stat_printer_f)(nvlist_t *, const char *, const char *);

static ldmsd_msg_log_f  log_fn;
static base_data_t      sampler_base;
static libzfs_handle_t *g_zfs;

static struct {
        int vdev_list_idx;
	int vdev_rec_idx;
} index_store;

static ldms_mval_t list_handle;

/* metric templates for a virtual device */
static struct ldms_metric_template_s zpool_vdev_metrics[] = {
    {"zpoolname",                0, LDMS_V_CHAR_ARRAY, "", MAX_LINE_LEN },
    {"state",                    0, LDMS_V_CHAR_ARRAY, "", MAX_LINE_LEN },
    {"vdevname",                 0, LDMS_V_CHAR_ARRAY, "", MAX_LINE_LEN },
    {"alloc",                    0, LDMS_V_U64,        "",            1 },
    {"free",                     0, LDMS_V_U64,        "",            1 },
    {"size",                     0, LDMS_V_U64,        "",            1 },
    {"read_bytes",               0, LDMS_V_U64,        "",            1 },
    {"read_errors",              0, LDMS_V_U64,        "",            1 },
    {"read_ops",                 0, LDMS_V_U64,        "",            1 },
    {"write_bytes",              0, LDMS_V_U64,        "",            1 },
    {"write_errors",             0, LDMS_V_U64,        "",            1 },
    {"write_ops",                0, LDMS_V_U64,        "",            1 },
    {"checksum_errors",          0, LDMS_V_U64,        "",            1 },
    {"fragmentation",            0, LDMS_V_U64,        "",            1 },
    {"init_errors",              0, LDMS_V_U64,        "",            1 },
    {0},
};

/* need to find a better way more intuitive than that
 * to manage heap. Like auto resize as a base function */

#define VDEV_METRICS_LEN (ARRAY_LEN(zpool_vdev_metrics) - 1)
static int    vdev_metric_ids[VDEV_METRICS_LEN];
static size_t zpool_vdev_heap_sz;

static vdevs_count_ zpool_vdev_list_len; /* Aggregated number of vdev per zpool */

/*****************************************************************************
 * Initialize the structure as schema and add them to the base schema.
 * Also calculate the size of memory needed per schema and add it to the ldms
 * schema list.
 ****************************************************************************/

static int initialize_ldms_structs()
{
        /*ldms_record_t zpool_def;  a pointer */
        ldms_record_t   zpool_vdev_def;  /* a pointer */
        int rc;

        log_fn(LDMSD_LDEBUG, SAMP" initialize()\n");

        /* Create the schema */
        base_schema_new(sampler_base);
        if (sampler_base->schema == NULL)
            goto err1;

        /* create the vdev record */
        zpool_vdev_def  = ldms_record_from_template("zpool_vdevs_stats",
			                            zpool_vdev_metrics,
						    vdev_metric_ids);
        if (zpool_vdev_def == NULL)
            goto err2;

        zpool_vdev_heap_sz = ldms_record_heap_size_get(zpool_vdev_def);
        rc = ldms_schema_record_add(sampler_base->schema, zpool_vdev_def);
        if (rc < 0)
            goto err3;

	index_store.vdev_rec_idx = rc;
	zpool_vdev_list_len.counttype  = TOPVDEV_COUNT;
	zpool_vdev_list_len.vdev_count = 0;
        rc = zpool_iter(g_zfs, get_vdevs_count, &zpool_vdev_list_len);
	/* add error for iter in case here */
        rc = ldms_schema_metric_list_add(sampler_base->schema,
                                         "zpool_vdev_list",
                                         NULL,
                                         zpool_vdev_list_len.vdev_count  * zpool_vdev_heap_sz);
        if (rc < 0)
            goto err2;

	index_store.vdev_list_idx = rc;

        /* Create the metric set */
        base_set_new(sampler_base);
        if (sampler_base->set == NULL)
                goto err2;

        return 0;

err3:
        /* We only manually delete record template when it
         * hasn't been added to the schema yet */
        ldms_record_delete(zpool_vdev_def);
err2:
        base_schema_delete(sampler_base);
err1:
        log_fn(LDMSD_LERROR, SAMP" initialization failed\n");
        return -1;
}



/*****************************************************************************
 * WHAT:
 * 1) Initialize the sampler base schema.
 * 2) Initialize all structure and memory.
 * 3) initialize the zfslib to sample the zpools stats.
 * CALLER:
 * ldms daemon. In error the plugin is aborted.
 ****************************************************************************/


static int config(struct ldmsd_plugin *self,
                  struct attr_value_list *kwl, struct attr_value_list *avl)
{
        int rc = 0;

        log_fn(LDMSD_LDEBUG, SAMP" config() called\n");

        sampler_base = base_config(avl, SAMP, "zpools_metrics", log_fn);
        if ((g_zfs = libzfs_init()) == NULL) {
            rc = errno;
            ldmsd_log(LDMSD_LERROR,
                      SAMP" : Failed to initialize libzfs: %d\n", errno);
            ldmsd_log(LDMSD_LERROR,
                      SAMP" : Is the zfs module loaded or zrepl running?\n");
        } else {
            rc = initialize_ldms_structs();
        }

        if (rc < 0) {
                base_del(sampler_base);
                sampler_base = NULL;
        }

        return rc;
}


/*****************************************************************************
 * WHAT:
 * reallocate heap size plus 1 zpool struct and one vdev struct
 * CALLER:
 * self, (plugin)
 ****************************************************************************/
static int resize_metric_set()
{
        size_t previous_heap_size;
        size_t new_heap_size;
        int    rc = 0;

        previous_heap_size = ldms_set_heap_size_get(sampler_base->set);
        base_set_delete(sampler_base);

        new_heap_size  = previous_heap_size;
        new_heap_size += zpool_vdev_heap_sz;

        if (base_set_new_heap(sampler_base, new_heap_size) == NULL) {
            rc = errno;
           ldmsd_log(LDMSD_LERROR,
                     SAMP" : Failed to resize metric set heap: %d\n", errno);
        } else {
		log_fn(LDMSD_LDEBUG, "ldms resize of list successful\n");
	}
        return rc;
}


static int sample(struct ldmsd_sampler *self)
{
        int rc = 0;

        base_sample_begin(sampler_base);

	list_handle = ldms_metric_get(sampler_base->set, index_store.vdev_list_idx);
        ldms_list_purge(sampler_base->set, list_handle);

        rc = zpool_iter(g_zfs, get_stats, NULL);
        if (rc != 0) {
            log_fn(LDMSD_LERROR, SAMP" sample():zfs_pool print_stat() failed: %d\n", rc);
            base_sample_end(sampler_base);
            goto err1;
        }
	/* this is where the rubber meets the pavement */

        base_sample_end(sampler_base);

err1:
        return rc;
}

static void term(struct ldmsd_plugin *self)
{
        log_fn(LDMSD_LDEBUG, SAMP" term() called\n");
        base_set_delete(sampler_base);
        base_del(sampler_base);
        sampler_base = NULL;
}

static ldms_set_t get_set(struct ldmsd_sampler *self)
{
	return NULL;
}

static const char *usage(struct ldmsd_plugin *self)
{
        log_fn(LDMSD_LDEBUG, SAMP" usage() called\n");
	return  "config name=" SAMP " " BASE_CONFIG_SYNOPSIS
                BASE_CONFIG_DESC
                ;
}

struct ldmsd_plugin *get_plugin(ldmsd_msg_log_f pf)
{
        static struct ldmsd_sampler plugin = {
                .base = {
                        .name = SAMP,
                        .type = LDMSD_PLUGIN_SAMPLER,
                        .term = term,
                        .config = config,
                        .usage = usage,
                },
                .get_set = get_set,
                .sample = sample,
        };

        log_fn = pf;
        log_fn(LDMSD_LDEBUG, SAMP" get_plugin() called ("PACKAGE_STRING")\n");

        return &plugin.base;
}


/*
 * vdev summary stats are a combination of the data shown by
 *  zpool status` and `zpool list -v
 *  zpoolname
 *  state
 *  vdevname
 *  alloc
 *  free
 *  size
 *  read_bytes
 *  read_errors
 *  read_ops
 *  write_bytes
 *  write_errors
 *  write_ops
 *  checksum_errors
 *  fragmentation
 *  init_errors
 */
static int get_vdev_stats(nvlist_t *nvroot, const char *pool_name,
    const char *parent_name)
{
        uint_t c;
        vdev_stat_t *vs;
        char *vdev_name = NULL;
	ldms_mval_t record_instance;
        int rc=0; /*return code*/

	vdev_name = get_vdev_name(nvroot, parent_name);

	if (nvlist_lookup_uint64_array(	nvroot,
					ZPOOL_CONFIG_VDEV_STATS,
					(uint64_t **)&vs, &c) != 0) {
                rc = 1;
        }

        record_instance = ldms_record_alloc(sampler_base->set,
                                            index_store.vdev_rec_idx);

        if (record_instance == NULL) {
                log_fn(LDMSD_LDEBUG, SAMP": ldms_record_alloc() failed, resizing metric set\n");
                resize_metric_set();
		record_instance = ldms_record_alloc(sampler_base->set,
				index_store.vdev_rec_idx);
		if (record_instance == NULL)
		  rc = 2;
        }

	if (rc == 0) {
		rc = ldms_list_append_record(sampler_base->set, list_handle,
						record_instance);

		/* zpoolname    0 */
		ldms_record_array_set_str(record_instance, vdev_metric_ids[0],
				pool_name);
	        /* zpool state  1 */
		ldms_record_array_set_str(record_instance, vdev_metric_ids[1],
				zpool_state_to_name((vdev_state_t)vs->vs_state, (vdev_aux_t)vs->vs_aux));
	        /* vdevname     2 */
		ldms_record_array_set_str(record_instance, vdev_metric_ids[2],
						vdev_name);
	        /* alloc        3 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[3], vs->vs_alloc);
	        /* free         4 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[4], vs->vs_space - vs->vs_alloc);
		/* size         5 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[5], vs->vs_space);
		/* read_bytes   6 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[6], vs->vs_bytes[ZIO_TYPE_READ]);
		/* iread_errors 7 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[7], vs->vs_read_errors);
		/* read_ops     8 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[8], vs->vs_ops[ZIO_TYPE_READ]);
		/* write_bytes  9 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[9], vs->vs_bytes[ZIO_TYPE_WRITE]);
		/* write_errors 10 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[10], vs->vs_write_errors);
		/* write_ops    11 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[11], vs->vs_ops[ZIO_TYPE_WRITE]);
		/* checksum errors 12 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[12], vs->vs_checksum_errors);
		/* fragmentation 13 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[13], vs->vs_fragmentation);
		/* initialization errors 14 */
		ldms_record_set_u64(record_instance, vdev_metric_ids[14], vs->vs_initialize_errors);
	}

        return (rc);
}


/*
 * recursive stats printer
 */
static int get_recursive_stats(stat_printer_f func, nvlist_t *nvroot,
    const char *pool_name, const char *parent_name, int descend)
{
        uint_t c, children;
        nvlist_t **child;
        char vdev_name[256];
        int err = 0;

        if (descend && nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
            &child, &children) == 0) {
                (void) strlcpy(vdev_name, get_vdev_name(nvroot, parent_name),
                    sizeof (vdev_name));
                for (c = 0; c < children; c++) {
			err = func(child[c], pool_name, "root");
                }
        }
        return (err);
}


/*
 * call-back to print the stats from the pool config
 *
 * Note: if the pool is broken, this can hang indefinitely and perhaps in an
 * unkillable state.
 */

int get_stats(zpool_handle_t *zhp, void *data)
{
        uint_t          c;
        int             err;
        boolean_t       missing;
        nvlist_t       *config, *nvroot;
        vdev_stat_t    *vs;
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
        err = get_recursive_stats(get_vdev_stats, nvroot,
					pool_name, NULL, 1);
        /*free(pool_name);*/
        zpool_close(zhp);
        return (err);
}
