/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2010-15 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2010-15 Sandia Corporation. All rights reserved.
 * Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
 * license for use of this work by or on behalf of the U.S. Government.
 * Export of this program may require a license from the United States
 * Government.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of Sandia nor the names of any contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *      Neither the name of Open Grid Computing nor the names of any
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *      Modified source versions must be plainly marked as such, and
 *      must not be misrepresented as being the original software.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <inttypes.h>
#include <stdarg.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/un.h>
#include <ctype.h>
#include <netdb.h>
#include <dlfcn.h>
#include <assert.h>
#include <libgen.h>
#include <time.h>
#include <event2/thread.h>
#include <coll/rbt.h>
#include <coll/str_map.h>
#include "event.h"
#include "ldms.h"
#include "ldmsd.h"
#include "ldms_xprt.h"
#include "config.h"

char myhostname[HOST_NAME_MAX+1];
pthread_t ctrl_thread = (pthread_t)-1;
int muxr_s = -1;
char *sockname = NULL;
struct attr_value_list *av_list;
struct attr_value_list *kw_list;

int bind_succeeded;

extern struct event_base *get_ev_base(int idx);
extern void release_ev_base(int idx);

pthread_mutex_t host_list_lock = PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(host_list_s, hostspec) host_list;
LIST_HEAD(ldmsd_store_policy_list, ldmsd_store_policy) sp_list;
pthread_mutex_t sp_list_lock = PTHREAD_MUTEX_INITIALIZER;

LIST_HEAD(plugin_list, ldmsd_plugin_cfg) plugin_list;

void ldmsd_config_cleanup()
{
	if (ctrl_thread != (pthread_t)-1) {
		void *dontcare;
		pthread_cancel(ctrl_thread);
		pthread_join(ctrl_thread, &dontcare);
	}

	if (muxr_s >= 0)
		close(muxr_s);
	if (sockname && bind_succeeded) {
		ldms_log("LDMS Daemon deleting socket file %s\n", sockname);
		unlink(sockname);
	}
}

static char replybuf[4096];
int send_reply(int sock, struct sockaddr *sa, ssize_t sa_len,
	       char *msg, ssize_t msg_len)
{
	struct msghdr reply;
	struct iovec iov;

	reply.msg_name = sa;
	reply.msg_namelen = sa_len;
	iov.iov_base = msg;
	iov.iov_len = msg_len;
	reply.msg_iov = &iov;
	reply.msg_iovlen = 1;
	reply.msg_control = NULL;
	reply.msg_controllen = 0;
	reply.msg_flags = 0;
	sendmsg(sock, &reply, 0);
	return 0;
}

struct ldmsd_plugin_cfg *ldmsd_get_plugin(char *name)
{
	struct ldmsd_plugin_cfg *p;
	LIST_FOREACH(p, &plugin_list, entry) {
		if (0 == strcmp(p->name, name))
			return p;
	}
	return NULL;
}

static char library_name[PATH_MAX];
struct ldmsd_plugin_cfg *new_plugin(char *plugin_name, char err_str[LEN_ERRSTR])
{
	struct ldmsd_plugin *lpi;
	struct ldmsd_plugin_cfg *pi = NULL;
	char *path = getenv("LDMSD_PLUGIN_LIBPATH");
	if (!path)
		path = LDMSD_PLUGIN_LIBPATH_DEFAULT;

	sprintf(library_name, "%s/lib%s.so", path, plugin_name);
	void *d = dlopen(library_name, RTLD_NOW);
	if (!d) {
		sprintf(err_str, "dlerror %s", dlerror());
		goto err;
	}
	ldmsd_plugin_get_f pget = dlsym(d, "get_plugin");
	if (!pget) {
		sprintf(err_str,
			"The library is missing the get_plugin() function.");
		goto err;
	}
	lpi = pget(ldmsd_msg_logger);
	if (!lpi) {
		sprintf(err_str, "The plugin could not be loaded.");
		goto err;
	}
	pi = calloc(1, sizeof *pi);
	if (!pi)
		goto enomem;
	pthread_mutex_init(&pi->lock, NULL);
	pi->thread_id = -1;
	pi->handle = d;
	pi->name = strdup(plugin_name);
	if (!pi->name)
		goto enomem;
	pi->libpath = strdup(library_name);
	if (!pi->libpath)
		goto enomem;
	pi->plugin = lpi;
	pi->sample_interval_us = 1000000;
	pi->sample_offset_us = 0;
	pi->synchronous = 0;
	LIST_INSERT_HEAD(&plugin_list, pi, entry);
	return pi;
enomem:
	sprintf(err_str, "No memory");
err:
	if (pi) {
		if (pi->name)
			free(pi->name);
		if (pi->libpath)
			free(pi->libpath);
		free(pi);
	}
	return NULL;
}

void destroy_plugin(struct ldmsd_plugin_cfg *p)
{
	free(p->libpath);
	free(p->name);
	LIST_REMOVE(p, entry);
	dlclose(p->handle);
	free(p);
}

/* NOTE: The implementation of this function is in ldmsd_store.c as all of the
 * flush_thread information are in ldmsd_store.c. */
extern void process_info_flush_thread(void);


/**
 * Return information about the state of the daemon
 */
int process_info(int fd,
		 struct sockaddr *sa, ssize_t sa_len,
		 char *command)
{
	extern int ev_thread_count;
	extern pthread_t *ev_thread;
	extern int *ev_count;
	int i;
	struct hostspec *hs;
	int verbose = 0;
	char *vb = av_value(av_list, "verbose");
	if (vb && (strcasecmp(vb, "true") == 0 ||
			strcasecmp(vb, "t") == 0))
		verbose = 1;

	ldms_log("Event Thread Info:\n");
	ldms_log("%-16s %s\n", "----------------", "------------");
	ldms_log("%-16s %s\n", "Thread", "Task Count");
	ldms_log("%-16s %s\n", "----------------", "------------");
	for (i = 0; i < ev_thread_count; i++) {
		ldms_log("%-16p %d\n",
			 (void *)ev_thread[i], ev_count[i]);
	}
	/* For flush_thread information */
	process_info_flush_thread();

	ldms_log("Host List Info:\n");
	ldms_log("%-12s %-12s %-12s %-12s\n",
		 "------------", "------------", "------------",
		 "------------");
	ldms_log("%-12s %-12s %-12s %-12s\n",
			"Hostname", "Transport", "Set", "Stat");
	ldms_log("%-12s %-12s %-12s %-12s\n",
		 "------------", "------------", "------------",
		 "------------");
	pthread_mutex_lock(&host_list_lock);
	uint64_t total_curr_busy = 0;
	uint64_t grand_total_busy = 0;
	LIST_FOREACH(hs, &host_list, link) {
		struct hostset *hset;
		ldms_log("%-12s %-12s\n", hs->hostname, hs->xprt_name);
		LIST_FOREACH(hset, &hs->set_list, entry) {
			ldms_log("%-12s %-12s %-12s\n",
				 "", "", hset->name);
			if (verbose) {
				ldms_log("%-12s %-12s %-12s %.12s %-12Lu\n",
						"", "", "", "curr_busy_count",
						hset->curr_busy_count);
				ldms_log("%-12s %-12s %-12s %.12s %-12Lu\n",
						"", "", "", "total_busy_count",
						hset->total_busy_count);
			}
			total_curr_busy += hset->curr_busy_count;
			grand_total_busy += hset->total_busy_count;
		}
	}
	ldms_log("%-12s %-12s %-12s %-12s\n",
		 "------------", "------------", "------------",
		 "------------");
	ldms_log("Total Current Busy Count: %Lu\n", total_curr_busy);
	ldms_log("Grand Total Busy Count: %Lu\n", grand_total_busy);
	pthread_mutex_unlock(&host_list_lock);

	pthread_mutex_lock(&sp_list_lock);
	struct ldmsd_store_policy *sp;
	LIST_FOREACH(sp, &sp_list, link) {
		ldms_log("%-12s %-12s -%12s %d ",
			  sp->name, sp->container, sp->schema,  sp->metric_count);
		struct ldmsd_store_metric *m;
		i = 0;
		LIST_FOREACH(m, &sp->metric_list, entry) {
			if (i > 0)
				ldms_log(",");
			i++;
			ldms_log("%s", m->name);
		}
	}
	pthread_mutex_unlock(&sp_list_lock);

	sprintf(replybuf, "0");
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

/*
 * Load a plugin
 */
int ldmsd_load_plugin(char *plugin_name, char err_str[LEN_ERRSTR])
{

	int rc = 0;
	err_str[0] = '\0';

	struct ldmsd_plugin_cfg *pi = ldmsd_get_plugin(plugin_name);
	if (pi) {
		snprintf(err_str, LEN_ERRSTR, "Plugin already loaded");
		rc = EEXIST;
		goto out;
	}
	pi = new_plugin(plugin_name, err_str);
	if (!pi)
		rc = 1;
out:
	return rc;
}

/*
 * Destroy and unload the plugin
 */
int ldmsd_term_plugin(char *plugin_name, char err_str[LEN_ERRSTR])
{
	int rc = 0;
	struct ldmsd_plugin_cfg *pi;
	err_str[0] = '\0';

	pi = ldmsd_get_plugin(plugin_name);
	if (!pi) {
		rc = ENOENT;
		snprintf(err_str, LEN_ERRSTR, "plugin not found.");
		return rc;
	}
	pthread_mutex_lock(&pi->lock);
	if (pi->ref_count) {
		snprintf(err_str, LEN_ERRSTR, "The specified plugin has "
				"active users and cannot be terminated.");
		rc = EINVAL;
		pthread_mutex_unlock(&pi->lock);
		goto out;
	}
	pi->plugin->term();
	pthread_mutex_unlock(&pi->lock);
	destroy_plugin(pi);
out:
	return rc;
}

/*
 * Configure a plugin
 */
int ldmsd_config_plugin(char *plugin_name,
			struct attr_value_list *_av_list,
			struct attr_value_list *_kw_list,
			char err_str[LEN_ERRSTR])
{
	int rc = 0;
	struct ldmsd_plugin_cfg *pi;
	err_str[0] = '\0';

	pi = ldmsd_get_plugin(plugin_name);
	if (!pi) {
		rc = ENOENT;
		snprintf(err_str, LEN_ERRSTR, "The plugin was not found.");
		goto out;
	}
	pthread_mutex_lock(&pi->lock);
	rc = pi->plugin->config(_kw_list, _av_list);
	if (rc)
		snprintf(err_str, LEN_ERRSTR, "Plugin configuration error.");
	pthread_mutex_unlock(&pi->lock);
out:
	return rc;
}

int _ldmsd_set_udata(ldms_set_t set, char *metric_name, uint64_t udata,
						char err_str[LEN_ERRSTR])
{
	int i = ldms_metric_by_name(set, metric_name);
	if (i < 0) {
		snprintf(err_str, LEN_ERRSTR, "Metric '%s' not found.",
			 metric_name);
		return ENOENT;
	}

	ldms_metric_user_data_set(set, i, udata);
	return 0;
}

/*
 * Assign user data to a metric
 */
int ldmsd_set_udata(char *set_name, char *metric_name,
		char *udata_s, char err_str[LEN_ERRSTR])
{
	ldms_set_t set;
	err_str[0] = '\0';
	set = ldms_set_by_name(set_name);
	if (!set) {
		snprintf(err_str, LEN_ERRSTR, "Set '%s' not found.", set_name);
		return ENOENT;
	}

	char *endptr;
	uint64_t udata = strtoull(udata_s, &endptr, 0);
	if (endptr[0] != '\0') {
		snprintf(err_str, LEN_ERRSTR, "User data '%s' invalid.",
								udata_s);
		return EINVAL;
	}
	return _ldmsd_set_udata(set, metric_name, udata, err_str);
}

#define LDMSD_DEFAULT_GATHER_INTERVAL 1000000

int str_to_host_type(char *type)
{
	if (0 == strcmp(type, "active"))
		return ACTIVE;
	if (0 == strcmp(type, "passive"))
		return PASSIVE;
	if (0 == strcmp(type, "bridging"))
		return BRIDGING;
	if (0 == strcmp(type, "local"))
		return LOCAL;
	return -1;
}

struct hostset *find_host_set(struct hostspec *hs, const char *set_name);
struct hostset *hset_new();
int resolve(const char *hostname, struct sockaddr_in *sin)
{
	struct hostent *h;

	h = gethostbyname(hostname);
	if (!h) {
		printf("Error resolving hostname '%s'\n", hostname);
		return -1;
	}

	if (h->h_addrtype != AF_INET) {
		printf("Hostname '%s' resolved to an unsupported address family\n",
		       hostname);
		return -1;
	}

	memset(sin, 0, sizeof *sin);
	sin->sin_addr.s_addr = *(unsigned int *)(h->h_addr_list[0]);
	sin->sin_family = h->h_addrtype;
	return 0;
}

/*
 * aggregator mask
 * If the value of bit 'i' is 1,
 * all added hosts with the standby aggregator no of 'i + 1'
 * will be aggregated by this aggregator.
 */
unsigned long saggs_mask = 0;

/* The max and min of standby aggregator counts */
#define STANDBY_MAX 64
#define STANDBY_MIN 1
/* Verify if x is a valid standby number */
#define VALID_STANDBY_NO(x) (x >= STANDBY_MIN && x <= STANDBY_MAX)
/* Verify if x is a valid standby state. */
#define VALID_STANDBY_STATE(x) (x == 0 || x == 1)

/*
 * Add a host
 */
int ldmsd_add_host(char *host, char *type, char *xprt_s, char *port,
				char *sets, char *interval_s, char *offset_s,
				char *standby_no_s, char err_str[LEN_ERRSTR])
{
	int rc;
	struct sockaddr_in sin;
	struct hostspec *hs;
	int host_type;
	unsigned long interval = LDMSD_DEFAULT_GATHER_INTERVAL;
	long offset = 0;
	char *xprt;
	char *endptr;
	int synchronous = 0;
	long port_no = LDMS_DEFAULT_PORT;
	unsigned long standby_no = 0;
	err_str[0] = '\0';

	host_type = str_to_host_type(type);
	if (host_type < 0) {
		snprintf(err_str, LEN_ERRSTR, "'%s' is an invalid host type.",
									type);
		return EINVAL;
	}

	/*
	 * If the connection type is either active or passive,
	 * need a set list to create hostsets.
	 */
	if (host_type != BRIDGING) {
		if (!sets) {
			snprintf(err_str, LEN_ERRSTR, "The attribute 'sets' "
								"is required.");
			return EINVAL;
		}
	} else {
		if (sets) {
			snprintf(err_str, LEN_ERRSTR, "Aborted!. "
				"Use type=ACTIVE to collect the sets.");
			return EPERM;
		}
	}

	if (interval_s) {
		interval = strtoul(interval_s, &endptr, 0);
		if (!endptr) {
			snprintf(err_str, LEN_ERRSTR, "Interval '%s' invalid",
								interval_s);
			return EINVAL;
		}
	}

	if (offset_s) {
		offset = strtol(offset_s, &endptr, 0);
		if (!endptr) {
			snprintf(err_str, LEN_ERRSTR, "Interval '%s' invalid",
								interval_s);
			return EINVAL;
		}
		if ( !((interval >= 10) && (interval >= labs(offset)*2)) ){
			snprintf(err_str, LEN_ERRSTR,
				"Parameters interval and offset are incompatible.");
			return EINVAL;
		}
		synchronous = 1;
	}

	if (standby_no_s) {
		standby_no = strtoul(standby_no_s, &endptr, 0);
		if (!endptr) {
			snprintf(err_str, LEN_ERRSTR,
					"Parameter for standby '%s' "
					"is invalid.", standby_no_s);
			return EINVAL;
		}
		if (!VALID_STANDBY_NO(standby_no)) {
			snprintf(err_str, LEN_ERRSTR,
					"Parameter for standby needs to be "
					"between %d and %d inclusive.",
					STANDBY_MIN, STANDBY_MAX);
			return EINVAL;
		} else {
			standby_no |= 1 << (standby_no - 1);
		}
	}

	hs = calloc(1, sizeof(*hs));
	if (!hs)
		goto enomem;
	hs->hostname = strdup(host);
	if (!hs->hostname)
		goto enomem;

	if (host_type != LOCAL) {
		rc = resolve(hs->hostname, &sin);
		if (rc) {
			snprintf(err_str, LEN_ERRSTR,
				"The host '%s' could not be resolved "
				"due to error %d.\n", hs->hostname, rc);
			goto err;
		}
		if (port)
			port_no = strtol(port, NULL, 0);
		sin.sin_port = port_no;

		if (xprt_s)
			xprt = strdup(xprt_s);
		else
			xprt = strdup("sock");
		if (!xprt)
			goto enomem;

		sin.sin_port = htons(port_no);
		hs->sin = sin;
		hs->xprt_name = xprt;
		hs->conn_state = HOST_DISCONNECTED;
	} else {
		/* local host always connected */
		hs->conn_state = HOST_CONNECTED;
	}

	hs->type = host_type;
	hs->sample_interval = interval;
	hs->sample_offset = offset;
	hs->synchronous = synchronous;
	hs->standby = standby_no;
	hs->connect_interval = LDMSD_CONNECT_TIMEOUT;

	pthread_mutex_init(&hs->set_list_lock, 0);
	pthread_mutex_init(&hs->conn_state_lock, NULL);

	hs->thread_id = find_least_busy_thread();
	hs->event = evtimer_new(get_ev_base(hs->thread_id),
				ldmsd_host_sampler_cb, hs);
	/* First connection attempt happens 'right away' */
	hs->timeout.tv_sec = LDMSD_INITIAL_CONNECT_TIMEOUT / 1000000;
	hs->timeout.tv_usec = LDMSD_INITIAL_CONNECT_TIMEOUT % 1000000;

	/* No hostsets will be created if the connection type is bridging. */
	if (host_type == BRIDGING)
		goto add_timeout;

	char *set_name = strtok(sets, ",");
	struct hostset *hset;
	while (set_name) {
		/* Check to see if it's already there */
		hset = find_host_set(hs, set_name);
		if (!hset) {
			hset = hset_new();
			if (!hset) {
				goto clean_set_list;
			}

			hset->name = strdup(set_name);
			if (!hset->name) {
				free(hset);
				goto clean_set_list;
			}

			hset->host = hs;

			LIST_INSERT_HEAD(&hs->set_list, hset, entry);
		}
		set_name = strtok(NULL, ",");
	}
add_timeout:
	evtimer_add(hs->event, &hs->timeout);

	pthread_mutex_lock(&host_list_lock);
	LIST_INSERT_HEAD(&host_list, hs, link);
	pthread_mutex_unlock(&host_list_lock);
	return 0;
clean_set_list:
	while ((hset = LIST_FIRST(&hs->set_list))) {
		LIST_REMOVE(hset, entry);
		free(hset->name);
		free(hset);
	}
enomem:
	rc = ENOMEM;
	snprintf(err_str, LEN_ERRSTR, "Memory allocation failure.");
err:
	if (hs->hostname)
		free(hs->hostname);
	if (hs->xprt_name)
		free(hs->xprt_name);
	free(hs);
	return rc;
}


struct ldmsd_store_policy *alloc_store_policy(const char *policy_name,
					      const char *container,
					      const char *schema)
{
	struct ldmsd_store_policy *sp;
	sp = calloc(1, sizeof(*sp));
	if (!sp)
		goto err0;
	sp->name = strdup(policy_name);
	if (!sp->name)
		goto err1;
	sp->container = strdup(container);
	if (!sp->container)
		goto err2;
	sp->schema = strdup(schema);
	if (!sp->schema)
		goto err3;
	sp->metric_count = 0;
	sp->state = STORE_POLICY_CONFIGURING;
	pthread_mutex_init(&sp->cfg_lock, NULL);
	return sp;
 err3:
	free(sp->container);
 err2:
	free(sp->name);
 err1:
	free(sp);
 err0:
	return sp;
}

/*
 * Take a reference on the hostset
 */
void hset_ref_get(struct hostset *hset)
{
	pthread_mutex_lock(&hset->refcount_lock);
	assert(hset->refcount > 0);
	hset->refcount++;
	pthread_mutex_unlock(&hset->refcount_lock);
}

/*
 * Release a reference on the hostset. If the reference count goes to
 * zero, destroy the hostset
 *
 * This cannot be called with the host set_list_lock held.
 */
void hset_ref_put(struct hostset *hset)
{
	int destroy = 0;

	pthread_mutex_lock(&hset->refcount_lock);
	hset->refcount--;
	if (hset->refcount == 0)
		destroy = 1;
	pthread_mutex_unlock(&hset->refcount_lock);

	if (destroy) {
		ldms_log("Destroying hostset '%s'.\n", hset->name);
		/*
		 * Take the host set_list_lock since we are modifying the host
		 * set_list
		 */
		pthread_mutex_lock(&hset->host->set_list_lock);
		LIST_REMOVE(hset, entry);
		pthread_mutex_unlock(&hset->host->set_list_lock);
		struct ldmsd_store_policy_ref *lsp_ref;
		while (lsp_ref = LIST_FIRST(&hset->lsp_list)) {
			LIST_REMOVE(lsp_ref, entry);
			free(lsp_ref);
		}
		free(hset->name);
		free(hset);
	}
}

void destroy_metric_list(struct ldmsd_store_metric_list *list)
{
	struct ldmsd_store_metric *smi;
	while (smi = LIST_FIRST(list)) {
		LIST_REMOVE(smi, entry);
		free(smi->name);
		free(smi);
	}
}

int policy_add_metrics(struct ldmsd_store_policy *sp, char *metric_list,
		       char *err_str)
{
	int rc;
	LIST_INIT(&sp->metric_list);
	sp->metric_count = 0;
	if (!metric_list || 0 == strlen(metric_list))
		/* None specified, all metrics in set will be used */
		return 0;

	char *metric_name, *ptr;
	metric_name = strtok_r(metric_list, ",", &ptr);
	while (metric_name) {
		rc = new_store_metric(sp, metric_name);
		if (rc)
			goto err;
		metric_name = strtok_r(NULL, ",", &ptr);
	}
	return 0;
err:
	return ENOMEM;
}

/*
 * Parse the host_list string and add each hostname to the storage
 * policy host tree.
 */
int str_cmp(void *a, const void *b)
{
	return strcmp((char *)a, (char *)b);
}
int policy_add_hosts(struct ldmsd_store_policy *sp, char *host_list,
		     char *err_str)
{
	char *ptr;
	struct ldmsd_store_host *host;
	char *name;

	rbt_init(&sp->host_tree, str_cmp);
	if (!host_list || 0 == strlen(host_list))
		return 0;
	for (name = strtok_r(host_list, ",", &ptr); name;
	     name = strtok_r(NULL, ",", &ptr)) {
		host = malloc(sizeof *host);
		if (!host)
			goto err;
		host->name = strdup(name);
		if (!host->name) {
			free(host);
			goto err;
		}
		rbn_init(&host->rbn, host->name);
		rbt_ins(&sp->host_tree, &host->rbn);
	}
	return 0;
 err:
	return ENOMEM;
}


/*
 * Apply the storage policy to the hostset if the schema and hostname
 * match
 */
int apply_store_policy(struct hostset *hset, struct ldmsd_store_policy *sp)
{
	struct rbn *rbn;
	struct ldmsd_store_policy_ref *ref;

	/* Check if the policy is for this schema */
	if (strcmp(ldms_set_schema_name_get(hset->set), sp->schema))
		return 0;

	/* Check if the policy is for this host */
	if (!rbt_empty(&sp->host_tree)) {
		rbn = rbt_find(&sp->host_tree, hset->host->hostname);
		if (!rbn)
			return 0;
	}

	/* This policy is for us, add it to our list */
	ref = calloc(1, sizeof *ref);
	if (!ref)
		return ENOMEM;

	ref->lsp = sp;
	LIST_INSERT_HEAD(&hset->lsp_list, ref, entry);

	/*
	 * If we are the first hostset for the policy, update
	 * the policy's metric indices
	 */
	pthread_mutex_lock(&sp->cfg_lock);
	if (sp->state == STORE_POLICY_CONFIGURING)
		if (update_policy_metrics(sp, hset))
			ldms_log("Error updating policy metrics for "
				 "policy %s.\n", sp->name);
	pthread_mutex_unlock(&sp->cfg_lock);
	return 0;
}

/*
 * Apply all matching storage policies to the hset
 */
int apply_store_policies(struct hostset *hset)
{
	int rc;
	struct ldmsd_store_policy *sp;
	/*
	 * Search the storage policy list for policies that
	 * apply to this host set.
	 */
	pthread_mutex_lock(&sp_list_lock);
	rc = 0;
	LIST_FOREACH(sp, &sp_list, link) {
		rc = apply_store_policy(hset, sp);
		if (rc)
			break;
	}
	pthread_mutex_unlock(&sp_list_lock);
	return rc;
}

/*
 * Apply the storage policy to all matching host sets
 */
int update_hsets_with_policy(struct ldmsd_store_policy *sp)
{
	int rc;
	struct hostspec *hs;
	rc = 0;
	pthread_mutex_lock(&host_list_lock);
	LIST_FOREACH(hs, &host_list, link) {
		struct hostset *hset;
		LIST_FOREACH(hset, &hs->set_list, entry) {
			pthread_mutex_lock(&hset->state_lock);
			if (hset->state == LDMSD_SET_READY) {
				rc = apply_store_policy(hset, sp);
				if (rc)
					ldms_log("The storage policy %s could "
						"not be applied to hostset "
						"%s:%s\n", sp->name,
						hs->hostname, hset->name);
			}
			pthread_mutex_unlock(&hset->state_lock);
		}
	}
	pthread_mutex_unlock(&host_list_lock);
	return rc;
}

void destroy_store_policy(struct ldmsd_store_policy *sp)
{
	free(sp->schema);
	free(sp->container);
	free(sp->name);
	destroy_metric_list(&sp->metric_list);
	if (sp->metric_arry)
		free(sp->metric_arry);
	struct rbn *rbn;
	while (rbn = rbt_min(&sp->host_tree)) {
		struct ldmsd_store_host *h =
			container_of(rbn, struct ldmsd_store_host, rbn);
		rbt_del(&sp->host_tree, rbn);
		free(h->name);
		free(h);
	}
	free(sp);
}

/*
 * Configure a new storage policy
 */
int config_store_policy(char *plugin_name, char *policy_name,
			char *container, char *schema, char *metrics, char *hosts,
			char err_str[LEN_ERRSTR])
{
	struct ldmsd_store_policy *sp;
	struct hostspec *hs;
	struct ldmsd_plugin_cfg *store;
	int rc;

	err_str[0] = '\0';

	pthread_mutex_lock(&sp_list_lock);
	LIST_FOREACH(sp, &sp_list, link) {
		if (0 == strcmp(sp->name, policy_name)) {
			pthread_mutex_unlock(&sp_list_lock);
			snprintf(err_str, LEN_ERRSTR,
				 "A policy with this name already exists.");
			return EEXIST;
		}
	}
	sp = alloc_store_policy(policy_name, container, schema);
	if (!sp) {
		snprintf(err_str, LEN_ERRSTR,
			 "Insufficient resources to allocate the policy");
		return ENOMEM;
	}
	store = ldmsd_get_plugin(plugin_name);
	if (!store) {
		snprintf(err_str, LEN_ERRSTR,
			 "The storage plugin '%s' was not found.", plugin_name);
		goto err;
	}
	sp->plugin = store->store;
	rc = policy_add_metrics(sp, metrics, err_str);
	if (rc) {
		snprintf(err_str, LEN_ERRSTR,
			 "Could not parse the metric list.");
		goto err;
	}
	rc = policy_add_hosts(sp, hosts, err_str);
	if (rc) {
		snprintf(err_str, LEN_ERRSTR,
			 "Could not parse the hosts list.");
		goto err;
	}
	rc = update_hsets_with_policy(sp);
	if (rc) {
		snprintf(err_str, LEN_ERRSTR,
			 "Could not apply the storage policy.");
		goto err;
	}
	LIST_INSERT_HEAD(&sp_list, sp, link);
	pthread_mutex_unlock(&sp_list_lock);

	ldms_log("Added the store policy '%s' successfully.\n", policy_name);
	return 0;

 err:
	pthread_mutex_unlock(&sp_list_lock);
	destroy_store_policy(sp);
	return rc;
}

/*
 * Functions to process ldmsctl commands
 */
/* load plugin */
int process_load_plugin(int fd,
			struct sockaddr *sa, ssize_t sa_len,
			char *command)
{
	char *plugin_name;
	char err_str[LEN_ERRSTR];

	plugin_name = av_value(av_list, "name");
	if (!plugin_name) {
		sprintf(replybuf, "%d The plugin name was not specified\n",
								-EINVAL);
		goto out;
	}

	int rc = ldmsd_load_plugin(plugin_name, err_str);
	sprintf(replybuf, "%d%s", -rc, err_str);
out:
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

/* terminate a plugin */
int process_term_plugin(int fd,
			struct sockaddr *sa, ssize_t sa_len,
			char *command)
{
	char *plugin_name;
	char err_str[LEN_ERRSTR];

	plugin_name = av_value(av_list, "name");
	if (!plugin_name) {
		sprintf(replybuf, "%d The plugin name must be specified.",
								-EINVAL);
		goto out;
	}

	int rc = ldmsd_term_plugin(plugin_name, err_str);
	sprintf(replybuf, "%d%s", -rc, err_str);
out:
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

/* configure a plugin */
int process_config_plugin(int fd,
			  struct sockaddr *sa, ssize_t sa_len,
			  char *command)
{
	char *plugin_name;
	char err_str[LEN_ERRSTR];

	plugin_name = av_value(av_list, "name");
	if (!plugin_name) {
		sprintf(replybuf, "%d The plugin name must be specified.",
								-EINVAL);
		goto out;
	}

	int rc = ldmsd_config_plugin(plugin_name, av_list, kw_list, err_str);
	sprintf(replybuf, "%d%s", -rc, err_str);
out:
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

/* Assign user data to a metric */
int process_set_udata(int fd,
			struct sockaddr *sa, ssize_t sa_len,
			char *command)
{
	char *set_name, *metric_name, *udata;
	char err_str[LEN_ERRSTR];
	char *attr;
	int rc = 0;

	attr = "set";
	set_name = av_value(av_list, attr);
	if (!set_name)
		goto einval;

	attr = "metric";
	metric_name = av_value(av_list, attr);
	if (!metric_name)
		goto einval;

	attr = "udata";
	udata = av_value(av_list, attr);
	if (!udata)
		goto einval;

	rc = ldmsd_set_udata(set_name, metric_name, udata, err_str);
	sprintf(replybuf, "%d%s", -rc, err_str);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
	einval:
		sprintf(replybuf, "%dThe attribute '%s' is required.\n", -EINVAL, attr);
		send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
		return 0;
}

/* Start a sampler */
int process_start_sampler(int fd,
			 struct sockaddr *sa, ssize_t sa_len,
			 char *command)
{
	char *plugin_name, *interval, *offset;
	char err_str[LEN_ERRSTR];
	char *attr;

	attr = "name";
	plugin_name = av_value(av_list, "name");
	if (!plugin_name)
		goto einval;

	attr = "interval";
	interval = av_value(av_list, attr);
	if (!interval)
		goto einval;

	attr = "offset";
	offset = av_value(av_list, attr);

	int rc = ldmsd_start_sampler(plugin_name, interval, offset, err_str);
	sprintf(replybuf, "%d%s", -rc, err_str);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
einval:
	sprintf(replybuf, "%dThe attribute '%s' is required.\n", -EINVAL, attr);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

int process_oneshot_sample(int fd,
		struct sockaddr *sa, ssize_t sa_len,
		char *command)
{
	char *attr;
	char *plugin_name, *ts;
	char err_str[LEN_ERRSTR];

	attr = "name";
	plugin_name = av_value(av_list, attr);
	if (!plugin_name)
		goto einval;

	attr = "time";
	ts = av_value(av_list, attr);
	if (!ts)
		goto einval;

	int rc = ldmsd_oneshot_sample(plugin_name, ts, err_str);
	sprintf(replybuf, "%d%s", -rc, err_str);
	goto out;

einval:
	sprintf(replybuf, "%dThe attribute '%s' is required.\n", -EINVAL, attr);
out:
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

/* stop a sampler */
int process_stop_sampler(int fd,
			 struct sockaddr *sa, ssize_t sa_len,
			 char *command)
{
	char *plugin_name;
	char err_str[LEN_ERRSTR];

	plugin_name = av_value(av_list, "name");
	if (!plugin_name) {
		sprintf(replybuf, "%d The plugin name must be specified.",
								-EINVAL);
		goto out;
	}

	int rc = ldmsd_stop_sampler(plugin_name, err_str);
	sprintf(replybuf, "%d%s", -rc, err_str);
out:
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

int process_ls_plugins(int fd,
		       struct sockaddr *sa, ssize_t sa_len,
		       char *command)
{
	struct ldmsd_plugin_cfg *p;
	sprintf(replybuf, "0");
	LIST_FOREACH(p, &plugin_list, entry) {
		strcat(replybuf, p->name);
		strcat(replybuf, "\n");
		if (p->plugin->usage)
			strcat(replybuf, p->plugin->usage());
	}
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

/* add a host */
int process_add_host(int fd,
		     struct sockaddr *sa, ssize_t sa_len,
		     char *command)
{
	char *host, *type, *xprt, *port, *sets, *interval_s, *offset_s, *agg_no;
	char err_str[LEN_ERRSTR];
	char *attr;

	attr = "type";
	type = av_value(av_list, attr);
	if (!type)
		goto einval;

	attr = "host";
	host = av_value(av_list, attr);
	if (!host)
		goto einval;

	sets = av_value(av_list, "sets");
	interval_s = av_value(av_list, "interval");
	offset_s = av_value(av_list, "offset");
	port = av_value(av_list, "port");
	xprt = av_value(av_list, "xprt");
	agg_no = av_value(av_list, "standby");

	int rc = ldmsd_add_host(host, type, xprt, port, sets,
				interval_s, offset_s, agg_no, err_str);

	sprintf(replybuf, "%d%s", -rc, err_str);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
einval:
	sprintf(replybuf, "%dThe attribute '%s' is required.\n", -EINVAL, attr);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

int process_update_standby(int fd,
				struct sockaddr *sa, ssize_t sa_len,
				char *command)
{
	char *attr, *value;
	int agg_no;
	int active;

	attr = "agg_no";
	value = av_value(av_list, attr);
	if (!value)
		goto enoent;

	agg_no = atoi(value);
	if (!VALID_STANDBY_NO(agg_no))
		goto einval;

	attr = "state";
	value = av_value(av_list, attr);
	if (!value)
		goto enoent;
	active = atoi(value);
	if (!VALID_STANDBY_STATE(active))
		goto einval;

	if (active == 1)
		saggs_mask |= 1 << (agg_no - 1);
	else
		saggs_mask &= ~(1 << (agg_no -1));

	sprintf(replybuf, "0");
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf) + 1);
	return 0;
einval:
	sprintf(replybuf, "%dThe value '%s' for '%s' is invalid.",
						-EINVAL, value, attr);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf) + 1);
	return 0;
enoent:
	sprintf(replybuf, "%dThe attribute '%s' is required.", -EINVAL, attr);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf) + 1);
	return 0;
}

/*
 * Start a store instance
 * name=      The storage plugin name.
 * policy=    The storage policy name
 * container= The container name
 * schema=    The schema name of the storage record
 * set=       The set name containing the desired metric(s).
 * metrics=   A comma separated list of metric names. If not specified,
 *            all metrics in the metric set will be saved.
 * hosts=     The set of hosts whose data will be stored. If hosts is not
 *            specified, the metric will be saved for all hosts. If
 *            specified, the value should be a comma separated list of
 *            host names.
 */
int process_store(int fd,
		  struct sockaddr *sa, ssize_t sa_len,
		  char *command)
{
	int rc;
	char *attr;
	char *plugin_name, *policy_name, *schema_name, *container_name, *metrics, *hosts;
	char err_str[LEN_ERRSTR];

	attr = "name";
	plugin_name = av_value(av_list, attr);
	if (!plugin_name)
		goto einval;
	attr = "policy";
	policy_name = av_value(av_list, attr);
	if (!policy_name)
		goto einval;
	attr = "container";
	container_name = av_value(av_list, attr);
	if (!container_name)
		goto einval;
	attr = "schema";
	schema_name = av_value(av_list, attr);
	if (!schema_name)
		goto einval;

	metrics = av_value(av_list, "metrics");
	hosts = av_value(av_list, "hosts");

	rc = config_store_policy(plugin_name, policy_name, container_name, schema_name,
				 metrics, hosts, err_str);

	sprintf(replybuf, "%d%s", -rc, err_str);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
einval:
	sprintf(replybuf, "%dThe attribute '%s' is required.\n", -EINVAL, attr);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

int process_remove_host(int fd,
			 struct sockaddr *sa, ssize_t sa_len,
			 char *command)
{
	return -1;
}

int process_exit(int fd,
		 struct sockaddr *sa, ssize_t sa_len,
		 char *command)
{
	cleanup(0);
	return 0;
}

ldmsctl_cmd_fn cmd_table[] = {
	[LDMSCTL_LIST_PLUGINS] = process_ls_plugins,
	[LDMSCTL_LOAD_PLUGIN] =	process_load_plugin,
	[LDMSCTL_TERM_PLUGIN] =	process_term_plugin,
	[LDMSCTL_CFG_PLUGIN] =	process_config_plugin,
	[LDMSCTL_START_SAMPLER] = process_start_sampler,
	[LDMSCTL_STOP_SAMPLER] = process_stop_sampler,
	[LDMSCTL_ADD_HOST] = process_add_host,
	[LDMSCTL_REM_HOST] = process_remove_host,
	[LDMSCTL_STORE] = process_store,
	[LDMSCTL_INFO_DAEMON] = process_info,
	[LDMSCTL_SET_UDATA] = process_set_udata,
	[LDMSCTL_EXIT_DAEMON] = process_exit,
	[LDMSCTL_UPDATE_STANDBY] = process_update_standby,
	[LDMSCTL_ONESHOT_SAMPLE] = process_oneshot_sample,
};

int process_record(int fd,
		   struct sockaddr *sa, ssize_t sa_len,
		   char *command, ssize_t cmd_len)
{
	char *cmd_s;
	long cmd_id;
	int rc = tokenize(command, kw_list, av_list);
	if (rc) {
		ldms_log("Memory allocation failure processing '%s'\n",
			 command);
		rc = ENOMEM;
		goto out;
	}

	cmd_s = av_name(kw_list, 0);
	if (!cmd_s) {
		ldms_log("Request is missing Id '%s'\n", command);
		rc = EINVAL;
		goto out;
	}

	cmd_id = strtoul(cmd_s, NULL, 0);
	if (cmd_id >= 0 && cmd_id <= LDMSCTL_LAST_COMMAND) {
		rc = cmd_table[cmd_id](fd, sa, sa_len, cmd_s);
		goto out;
	}

	sprintf(replybuf, "-22Invalid command Id %ld\n", cmd_id);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
 out:
	return rc;
}

struct hostset *hset_new()
{
	struct hostset *hset = calloc(1, sizeof *hset);
	if (!hset)
		return NULL;

	hset->state = LDMSD_SET_CONFIGURED;
	hset->refcount = 1;
	pthread_mutex_init(&hset->refcount_lock, NULL);
	pthread_mutex_init(&hset->state_lock, NULL);
	return hset;
}

struct hostset *find_host_set(struct hostspec *hs, const char *set_name)
{
	struct hostset *hset;
	pthread_mutex_lock(&hs->set_list_lock);
	LIST_FOREACH(hset, &hs->set_list, entry) {
		if (0 == strcmp(set_name, hset->name)) {
			pthread_mutex_unlock(&hs->set_list_lock);
			hset_ref_get(hset);
			return hset;
		}
	}
	pthread_mutex_unlock(&hs->set_list_lock);
	return NULL;
}

int new_store_metric(struct ldmsd_store_policy *sp, const char *name)
{
	struct ldmsd_store_metric *smi;
	smi = malloc(sizeof(*smi));
	if (!smi)
		return ENOMEM;

	smi->name = strdup(name);
	if (!smi->name) {
		free(smi);
		return ENOMEM;
	}
	LIST_INSERT_HEAD(&sp->metric_list, smi, entry);
	sp->metric_count++;
	return 0;
}

int update_policy_metrics(struct ldmsd_store_policy *sp, struct hostset *hset)
{
	struct ldmsd_store_metric *smi;
	const char *metric;
	int i, rc;
	const char *name;

	if (sp->metric_arry)
		free(sp->metric_arry);
	sp->metric_count = 0;
	sp->metric_arry = calloc(ldms_set_card_get(hset->set), sizeof(int *));
	name = NULL;
	if (LIST_EMPTY(&sp->metric_list)) {
		/* No metric list was given. Add all metrics in the set */
		for (i = 0; i < ldms_set_card_get(hset->set); i++) {
			name = ldms_metric_name_get(hset->set, i);
			rc = new_store_metric(sp, name);
			if (rc)
				goto err;
			sp->metric_arry[i] = i;
		}
	} else {
		i = 0;
		smi = LIST_FIRST(&sp->metric_list);
		while (smi) {
			int idx;
			name = smi->name;
			idx = ldms_metric_by_name(hset->set, name);
			if (idx < 0)
				goto err;
			smi->type = ldms_metric_type_get(hset->set, idx);
			sp->metric_arry[i++] = idx;
			smi = LIST_NEXT(smi, entry);
		}
	}
	/* NB: The storage instance can only be created after the
	 * metrics have been retrieved from the metric set dictionary
	 * above.
	 */
	sp->si = ldmsd_store_instance_get(sp->plugin, sp);
	if (!sp->si) {
		ldms_log("Could not allocate the store instance");
		goto err;
	}
	sp->state = STORE_POLICY_READY;
	return 0;
err:
	ldms_log("Store '%s': Could not configure storage policy for "
		 "'%s'.\n", sp->container, (name ? name : "NULL"));
	sp->state = STORE_POLICY_ERROR;
	if (sp->metric_arry)
		free(sp->metric_arry);
	while (smi = LIST_FIRST(&sp->metric_list)) {
		LIST_REMOVE(smi, entry);
		free(smi->name);
		free(smi);
	}
	sp->metric_count = 0;
	return ENOENT;
}

int process_message(int sock, struct msghdr *msg, ssize_t msglen)
{
	return process_record(sock,
			      msg->msg_name, msg->msg_namelen,
			      msg->msg_iov->iov_base, msglen);
}

void *ctrl_thread_proc(void *v)
{
	struct msghdr msg;
	struct iovec iov;
	static unsigned char lbuf[256];
	struct sockaddr_storage ss;
	iov.iov_base = lbuf;
	do {
		ssize_t msglen;
		ss.ss_family = AF_UNIX;
		msg.msg_name = &ss;
		msg.msg_namelen = sizeof(ss);
		iov.iov_len = sizeof(lbuf);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags = 0;
		msglen = recvmsg(muxr_s, &msg, 0);
		if (msglen <= 0)
			break;
		process_message(muxr_s, &msg, msglen);
	} while (1);
	return NULL;
}

void *inet_ctrl_thread_proc(void *v)
{
	struct msghdr msg;
	struct iovec iov;
	static unsigned char lbuf[256];
	struct sockaddr_in sin;
	iov.iov_base = lbuf;
	do {
		ssize_t msglen;
		sin.sin_family = AF_INET;
		msg.msg_name = &sin;
		msg.msg_namelen = sizeof(sin);
		iov.iov_len = sizeof(lbuf);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags = 0;
		msglen = recvmsg(muxr_s, &msg, 0);
		if (msglen <= 0)
			break;
		process_message(muxr_s, &msg, msglen);
	} while (1);
	return NULL;
}

int ldmsd_config_init(char *name)
{
	struct sockaddr_un sun;
	int ret;

	/* Create the control socket parsing structures */
	av_list = av_new(128);
	kw_list = av_new(128);
	if (!av_list || !kw_list)
		return ENOMEM;

	if (!name) {
		char *sockpath = getenv("LDMSD_SOCKPATH");
		if (!sockpath)
			sockpath = "/var/run";
		sockname = malloc(sizeof(LDMSD_CONTROL_SOCKNAME) + strlen(sockpath) + 2);
		sprintf(sockname, "%s/%s", sockpath, LDMSD_CONTROL_SOCKNAME);
	} else {
		sockname = strdup(name);
	}

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strncpy(sun.sun_path, sockname,
			sizeof(struct sockaddr_un) - sizeof(short));

	/* Create listener */
	muxr_s = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (muxr_s < 0) {
		ldms_log("Error %d creating muxr socket.\n", muxr_s);
		return -1;
	}

	/* Bind to our public name */
	ret = bind(muxr_s, (struct sockaddr *)&sun, sizeof(struct sockaddr_un));
	if (ret < 0) {
		ldms_log("Error %d binding to socket named '%s'.\n",
						errno, sockname);
		return -1;
	}
	bind_succeeded = 1;

	ret = pthread_create(&ctrl_thread, NULL, ctrl_thread_proc, 0);
	if (ret) {
		ldms_log("Error %d creating the control pthread'.\n");
		return -1;
	}
	return 0;
}
