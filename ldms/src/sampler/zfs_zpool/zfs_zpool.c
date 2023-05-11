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
static int get_zpool_stats(zpool_handle_t *zhp, void *data);
static int get_zpool_count(zpool_handle_t *zhp, void *data);
static int get_pool_scan_status(nvlist_t *nvroot, const char *pool_name, ldms_mval_t *record_instance);

#define POOL_MEASUREMENT         "zpool_stats"
#define SCAN_MEASUREMENT         "zpool_scan_stats"
#define VDEV_MEASUREMENT         "zpool_vdev_stats"
#define POOL_LATENCY_MEASUREMENT "zpool_latency"
#define POOL_QUEUE_MEASUREMENT   "zpool_vdev_queue"
#define MIN_LAT_INDEX   10  /* minimum latency index 10 = 1024ns */
#define POOL_IO_SIZE_MEASUREMENT        "zpool_io_size"
#define MIN_SIZE_INDEX  9  /* minimum size index 9 = 512 bytes */

/* global options */
int execd_mode 			= 0;
int no_histograms 		= 1;
int sum_histogram_buckets 	= 0;
char metric_data_type		= 'u';
uint64_t metric_value_mask	= UINT64_MAX;
uint64_t timestamp 		= 0;
int complained_about_sync 	= 0;
char *tags 			= "";

typedef int (*stat_printer_f)(nvlist_t *, const char *, const char *);

static ldmsd_msg_log_f  log_fn;
static base_data_t      sampler_base;
static libzfs_handle_t *g_zfs;

static struct {
        int vdev_list_idx;
	int vdev_rec_idx;
} index_store;

static ldms_mval_t list_handle;

typedef enum op_func_type {
        FUNC_NOFUNCREQ = 0,
        FUNC_SCRUB     = 1,
        FUNC_RESILVER  = 2,
        FUNC_REBUILD   = 3,
        FUNC_SCAN      = 4
} op_func_type_;

static const char * const operation_types[] = {
	[FUNC_NOFUNCREQ] = "none",
	[FUNC_SCRUB]     = "scrub",
	[FUNC_RESILVER]  = "resilver",
	[FUNC_REBUILD]   = "rebuild",
	[FUNC_SCAN]      = "scan"
};

/* metric templates for a zpool and scan status if any */
static struct ldms_metric_template_s zpool_vdev_metrics[] = {
    {"pool",              0, LDMS_V_CHAR_ARRAY, "", MAX_LINE_LEN },
    {"state",             0, LDMS_V_CHAR_ARRAY, "", MAX_LINE_LEN },
    {"total",             0, LDMS_V_U64,        "",            1 },
    {"allocated",         0, LDMS_V_U64,        "",            1 },
    {"free",              0, LDMS_V_U64,        "",            1 },
    {"used",              0, LDMS_V_U64,        "",            1 },
    {"scan_func",         0, LDMS_V_CHAR_ARRAY, "", MAX_LINE_LEN },
    {"scan_status",       0, LDMS_V_CHAR_ARRAY, "", MAX_LINE_LEN },
    {"scan_repaired",     0, LDMS_V_U64,        "",            1 },
    {"scan_completed_in", 0, LDMS_V_U64,        "",            1 },
    {"scan_errors",       0, LDMS_V_U32,        "",            1 },
    {"scan_completed_on", 0, LDMS_V_U64,        "",            1 },
    {0}
};

#define ZPOOL_METRICS_LEN (ARRAY_LEN(zpool_vdev_metrics) - 1)
static int    zpool_metric_ids[ZPOOL_METRICS_LEN];
static size_t zpool_heap_sz;

static int zpool_list_len  = 0; /* Aggregated number of vdev per zpool */

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
						    zpool_metric_ids);
        if (zpool_vdev_def == NULL)
            goto err2;

        zpool_heap_sz = ldms_record_heap_size_get(zpool_vdev_def);
        rc = ldms_schema_record_add(sampler_base->schema, zpool_vdev_def);
        if (rc < 0)
            goto err3;

	index_store.vdev_rec_idx = rc;
        rc = zpool_iter(g_zfs, get_zpool_count, NULL);
	/* add error for iter in case here */
        rc = ldms_schema_metric_list_add(sampler_base->schema,
                                         "zpool_list",
                                         NULL,
                                         zpool_list_len * zpool_heap_sz);
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
        char *value;

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
        new_heap_size += zpool_heap_sz;

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

        rc = zpool_iter(g_zfs, get_zpool_stats, NULL);
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
 * get a string suitable for an influxdb tag that describes this vdev
 *
 * By default only the vdev hierarchical name is shown, separated by '/'
 * If the vdev has an associated path, which is typical of leaf vdevs,
 * then the path is added.
 * It would be nice to have the devid instead of the path, but under
 * Linux we cannot be sure a devid will exist and we'd rather have
 * something than nothing, so we'll use path instead.
 */
static char *get_vdev_desc(nvlist_t *nvroot, const char *parent_name)
{
        static char vdev_desc[2 * MAXPATHLEN];
        char *vdev_type = NULL;
        uint64_t vdev_id = 0;
        char vdev_value[MAXPATHLEN];
        char *vdev_path = NULL;
        char *s, *t;

        if (nvlist_lookup_string(nvroot, ZPOOL_CONFIG_TYPE, &vdev_type) != 0) {
                vdev_type = "unknown";
        }
        if (nvlist_lookup_uint64(nvroot, ZPOOL_CONFIG_ID, &vdev_id) != 0) {
                vdev_id = UINT64_MAX;
        }
        if (nvlist_lookup_string(
            nvroot, ZPOOL_CONFIG_PATH, &vdev_path) != 0) {
                vdev_path = NULL;
        }

        if (parent_name == NULL) {
                s = escape_string(vdev_type);
                (void) snprintf(vdev_value, sizeof (vdev_value), "%s", s);
                free(s);
        } else {
                s = escape_string((char *)parent_name);
                t = escape_string(vdev_type);
                (void) snprintf(vdev_value, sizeof (vdev_value),
                    "vdev=%s/%s-%llu", s, t, (u_longlong_t)vdev_id);
                free(s);
                free(t);
        }
        if (vdev_path == NULL) {
                (void) snprintf(vdev_desc, sizeof (vdev_desc), "%s",
                    vdev_value);
        } else {
                s = escape_string(vdev_path);
                (void) snprintf(vdev_desc, sizeof (vdev_desc), "path=%s,%s",
                    s, vdev_value);
                free(s);
	}
        return (vdev_desc);
}


/*
 * top-level vdev stats are at the pool level moving to its own plugin
 */

static int get_detailed_pool_stats(nvlist_t *nvroot, const char *pool_name)
{
        nvlist_t *nv_ex;
        uint64_t value, cap;
	ldms_mval_t record_instance;
        uint_t c;
        vdev_stat_t *vs;
	int   rc = 0;
        char *vdev_desc = NULL;
        vdev_desc = get_vdev_desc(nvroot, NULL);

	if (nvlist_lookup_uint64_array(nvroot, ZPOOL_CONFIG_VDEV_STATS,
                (uint64_t **)&vs, &c) != 0) {
		return (1);
        }

        if (nvlist_lookup_nvlist(nvroot,
				ZPOOL_CONFIG_VDEV_STATS_EX, &nv_ex) != 0) {
                rc = 6;
        }

	if (rc == 0) {
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
	}

	if (rc == 0) {
		rc = ldms_list_append_record(sampler_base->set, list_handle,
						record_instance);

		/* zpoolname    0 */
		ldms_record_array_set_str(record_instance, zpool_metric_ids[0],
				pool_name);
	        /* zpool state  1 */
		ldms_record_array_set_str(record_instance, zpool_metric_ids[1],
					zpool_state_to_name((vdev_state_t)vs->vs_state,
					(vdev_aux_t)vs->vs_aux));
	        /* total     2 */
		ldms_record_set_u64(record_instance, zpool_metric_ids[2],
					vs->vs_space);
	        /* allocated 3 */
		ldms_record_set_u64(record_instance, zpool_metric_ids[3],
					vs->vs_alloc);
	        /* free     4 */
		ldms_record_set_u64(record_instance, zpool_metric_ids[4],
					vs->vs_space - vs->vs_alloc);
	        /* used     5 */
		cap = (vs->vs_space == 0) ? 0 : (vs->vs_alloc * 10000 / vs->vs_space)/100;
		ldms_record_set_u64(record_instance, zpool_metric_ids[5], cap);


		/* Here we call the get_pool_scan function to fill the rest of the
		 * record */
		rc = get_pool_scan_status(nvroot, pool_name, &record_instance);

	}
        return (0);
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
        int err;

        err = func(nvroot, pool_name, parent_name);
        if (err)
                return (err);

        if (descend && nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
            &child, &children) == 0) {
                (void) strlcpy(vdev_name, get_vdev_name(nvroot, parent_name),
                    sizeof (vdev_name));

                for (c = 0; c < children; c++) {
                        get_recursive_stats(func, child[c], pool_name,
                                              vdev_name, descend);
                }
        }
        return (0);
}


static int get_zpool_count(zpool_handle_t *zhp, void *data)
{
	int rc = 0;

	if (zhp != NULL) {
		zpool_list_len++;
	} else {
		rc = 1;
	}
        return (rc);
}

static int get_pool_scan_status(nvlist_t *nvroot, const char *pool_name, ldms_mval_t *record_instance)
{
	uint_t c;
	int64_t elapsed;
	uint64_t examined, pass_exam, paused_time, paused_ts, rate;
	uint64_t remaining_time;
	pool_scan_stat_t *ps = NULL;
	double pct_done;
	char *state[DSS_NUM_STATES] = {
	    "NONE",
	    "scanning",
	    "finished",
	    "canceled"
	};
/*	operation_types func;*/
	const char *func;

	(void) nvlist_lookup_uint64_array(nvroot,
	    ZPOOL_CONFIG_SCAN_STATS,
	    (uint64_t **)&ps, &c);

	/*
	 * ignore if there are no stats
	 */
	if (ps == NULL)
		return (0);

	/*
	 * return error if state is bogus
	 */
	if (ps->pss_state >= DSS_NUM_STATES ||
	    ps->pss_func >= POOL_SCAN_FUNCS) {
		if (complained_about_sync % 1000 == 0) {
			fprintf(stderr, "error: cannot decode scan stats: "
			    "ZFS is out of sync with compiled zfs_zpool (ldms)");
			complained_about_sync++;
		}
		return (1);
	}

	switch (ps->pss_func) {

	case POOL_SCAN_NONE:
		func = operation_types[FUNC_NOFUNCREQ];
		break;
	case POOL_SCAN_SCRUB:
		func = operation_types[FUNC_SCRUB];
		break;
	case POOL_SCAN_RESILVER:
		func = operation_types[FUNC_RESILVER];
		break;
#ifdef POOL_SCAN_REBUILD
	case POOL_SCAN_REBUILD:
		func = operation_types[FUNC_REBUILD];
		break;
#endif
	default:
		func = operation_types[FUNC_SCAN];
	}

	/* overall progress */
	examined = ps->pss_examined ? ps->pss_examined : 1;
	pct_done = 0.0;
	if (ps->pss_to_examine > 0)
		pct_done = 100.0 * examined / ps->pss_to_examine;

#ifdef EZFS_SCRUB_PAUSED
	paused_ts = ps->pss_pass_scrub_pause;
	paused_time = ps->pss_pass_scrub_spent_paused;
#else
	paused_ts = 0;
	paused_time = 0;
#endif

	/* calculations for this pass */
	if (ps->pss_state == DSS_SCANNING) {
		elapsed = (int64_t)time(NULL) - (int64_t)ps->pss_pass_start -
		    (int64_t)paused_time;
		elapsed = (elapsed > 0) ? elapsed : 1;
		pass_exam = ps->pss_pass_exam ? ps->pss_pass_exam : 1;
		rate = pass_exam / elapsed;
		rate = (rate > 0) ? rate : 1;
		remaining_time = ps->pss_to_examine - examined / rate;
	} else {
		elapsed =
		    (int64_t)ps->pss_end_time - (int64_t)ps->pss_pass_start -
		    (int64_t)paused_time;
		elapsed = (elapsed > 0) ? elapsed : 1;
		pass_exam = ps->pss_pass_exam ? ps->pss_pass_exam : 1;
		rate = pass_exam / elapsed;
		remaining_time = 0;
	}
	rate = rate ? rate : 1;

	/* scan_func         6 */
	ldms_record_array_set_str(*record_instance, zpool_metric_ids[6], func);
	/* scan_status       7 */
	ldms_record_array_set_str(*record_instance, zpool_metric_ids[7],
				state[ps->pss_state]);
	/* scan_repaired     8 */
	ldms_record_set_u64(*record_instance, zpool_metric_ids[8],
				ps->pss_processed);
	/* scan_completed_in 9 */
	ldms_record_set_u64(*record_instance, zpool_metric_ids[9],
				ps->pss_end_time - ps->pss_start_time);
	/* scan_errors       10 */
	ldms_record_set_u64(*record_instance, zpool_metric_ids[10],
				ps->pss_errors);
	/* scan_completed_on 11 */
	ldms_record_set_u64(*record_instance, zpool_metric_ids[11],
				ps->pss_end_time);

	return (0);
}


/*
 * call-back to print the stats from the pool config
 *
 * Note: if the pool is broken, this can hang indefinitely and perhaps in an
 * unkillable state.
 */

static int get_zpool_stats(zpool_handle_t *zhp, void *data)
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

	err = get_detailed_pool_stats(nvroot, pool_name);
        zpool_close(zhp);
        return (err);
}
