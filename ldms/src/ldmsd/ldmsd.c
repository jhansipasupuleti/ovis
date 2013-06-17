/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2010 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2010 Sandia Corporation. All rights reserved.
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
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
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
#include "event.h"
#include "ldms.h"
#include "ldmsd.h"
#include "ldms_xprt.h"
#include "config.h"
#include <event2/thread.h>

#ifdef ENABLE_YAML
#include <yaml.h>
#endif

/**
 * \brief Convenient error report and exit macro.
 * \param fmt The format (as in printf format).
 * \param args The arguments of fmt (same as printf).
 */
#define LDMSD_ERROR_EXIT(fmt, args...) \
	{ \
		fprintf(stderr, fmt "\n", ##args); \
		exit(-1); \
	}

/**
 * \brief Convenient assert macro for LDMS.
 * \param X The assert expression.
 */
#define LDMS_ASSERT(X) \
	if (!(X)) { \
		fprintf(stderr, "Assert %s failed at %s:%d(%s)\n", \
		#X, __FILE__, __LINE__, __func__); \
		exit(-1); \
	}

#define LDMSD_SETFILE "/proc/sys/kldms/set_list"
#define LDMSD_LOGFILE "/var/log/ldmsd.log"
#define FMT "H:i:l:S:s:x:T:M:t:P:I:m:vFkNC:"
#define LDMSD_MEM_SIZE_DEFAULT 512 * 1024
/* YAML needs instance number to differentiate configuration for an instnace
 * from other instances' configuration in the same configuration file
 */
int instance_number = 1;

char myhostname[80];
char ldmstype[20];
int foreground;
int bind_succeeded;
pthread_t ctrl_thread = (pthread_t)-1;
pthread_t event_thread = (pthread_t)-1;
pthread_t relay_thread = (pthread_t)-1;
char *test_set_name;
int test_set_count=1;
int test_metric_count=1;
int notify=0;
int muxr_s = -1;
char *logfile;
char *sockname = NULL;
size_t max_mem_size = LDMSD_MEM_SIZE_DEFAULT;
ldms_t ldms;
FILE *log_fp;
struct attr_value_list *av_list;
struct attr_value_list *kw_list;

/* YAML configuration needs these variables to be global */
int do_kernel = 0;
char *setfile = NULL;
char *listen_arg = NULL;

#ifdef ENABLE_YAML
yaml_parser_t yaml_parser;
#endif

pthread_mutex_t host_list_lock = PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(host_list_s, hostspec) host_list;

int passive = 0;
int quiet = 1;
void ldms_log(const char *fmt, ...)
{
	va_list ap;
	time_t t;
	struct tm *tm;
	char dtsz[200];

	t = time(NULL);
	tm = localtime(&t);
	if (strftime(dtsz, sizeof(dtsz), "%a %b %d %H:%M:%S %Y", tm))
		fprintf(log_fp, "%s: ", dtsz);
	va_start(ap, fmt);
	vfprintf(log_fp, fmt, ap);
	fflush(log_fp);
}

void cleanup(int x)
{
	ldms_log("LDMS Daemon exiting...status %d\n", x);
	if (ctrl_thread != (pthread_t)-1) {
		void *dontcare;
		pthread_cancel(ctrl_thread);
		pthread_join(ctrl_thread, &dontcare);
	}

	if (muxr_s >= 0)
		close(muxr_s);
	if (sockname && bind_succeeded)
		unlink(sockname);
	if (ldms)
		ldms_release_xprt(ldms);

	exit(x);
}

void usage(char *argv[])
{
	printf("%s: [%s]\n", argv[0], FMT);
	printf("    -C config_file    The path to the configuration file. \n");
	printf("    -P thr_count   Count of event threads to start.\n");
	printf("    -i             Test metric set sample interval.\n");
	printf("    -k             Publish publish kernel metrics.\n");
	printf("    -s setfile     Text file containing kernel metric sets to publish.\n"
	       "                   [" LDMSD_SETFILE "]\n");
	printf("    -S sockname    Specifies the unix domain socket name to\n"
	       "                   use for ldmsctl access.\n");
	printf("    -x xprt:port   Specifies the transport type to listen on. May be specified\n"
	       "                   more than once for multiple transports. The transport string\n"
	       "                   is one of 'rdma', or 'sock'. A transport specific port number\n"
	       "                   is optionally specified following a ':', e.g. rdma:50000.\n");
	printf("    -l log_file    The path to the log file for status messages.\n"
	       "                   [" LDMSD_LOGFILE "]\n");
	printf("    -v             Verbose mode, i.e. print requests as they are processed.\n"
	       "                   [false].\n");
	printf("    -F             Foreground mode, don't daemonize the program [false].\n");
	printf("    -t count       Number of test sets to create.\n");
	printf("    -T set_name    Test set prefix.\n");
	printf("    -N             Notify registered monitors of the test metric sets\n");
	printf("    -t set_count   Create set_count instances of set_name.\n");
	printf("    -I instance    The instance number");
	printf("    -m memory size   Maximum size of pre-allocated memory for metric sets.\n"
            "                     The given size must be less than 1 Petabytes.\n"
            "                     For example, 20M or 20mb are 20 megabytes.\n");
	cleanup(1);
}

int ev_thread_count = 1;
struct event_base **ev_base;	/* event bases */
pthread_t *ev_thread;		/* sampler threads */
int *ev_count;			/* number of hosts/samplers assigned to each thread */

int find_least_busy_thread()
{
	int i;
	int idx = 0;
	int count = ev_count[0];
	for (i = 1; i < ev_thread_count; i++) {
		if (ev_count[i] < count) {
			idx = i;
			count = ev_count[i];
		}
	}
	return idx;
}

struct event_base *get_ev_base(int idx)
{
	ev_count[idx] = ev_count[idx] + 1;
	return ev_base[idx];
}

void release_ev_base(int idx)
{
	ev_count[idx] = ev_count[idx] - 1;
}

pthread_t get_thread(int idx)
{
	return ev_thread[idx];
}

/*
 * This function opens the device file specified by 'devname' and
 * mmaps the metric set 'set_no'.
 */
int map_fd;
ldms_set_t map_set;
int publish_kernel(const char *setfile)
{
	int rc;
	int i, j;
	void *meta_addr;
	void *data_addr;
	int set_no;
	int set_size;
	struct ldms_set_hdr *sh;
	unsigned char *p;
	char set_name[80];
	FILE *fp;

	fp = fopen(setfile, "r");
	if (!fp) {
		ldms_log("The specified kernel metric set file '%s' could not be opened.\n",
			 setfile);
		return 0;
	}

	map_fd = open("/dev/kldms0", O_RDWR);
	if (map_fd < 0) {
		ldms_log("Error %d opening the KLDMS device file '/dev/kldms0'\n", map_fd);
		return map_fd;
	}

	while (3 == fscanf(fp, "%d %d %s", &set_no, &set_size, set_name)) {
		int id = set_no << 13;
		ldms_log("Mapping set %d name %s\n", set_no, set_name);
		meta_addr = mmap((void *)0, 8192, PROT_READ|PROT_WRITE, MAP_SHARED, map_fd, id);
		if (meta_addr == MAP_FAILED)
			return -ENOMEM;
		sh = meta_addr;
		if (set_name[0] == '/')
			sprintf(sh->name, "%s%s", myhostname, set_name);
		else
			sprintf(sh->name, "%s/%s", myhostname, set_name);
		data_addr = mmap((void *)0, 8192, PROT_READ|PROT_WRITE,
				 MAP_SHARED, map_fd,
				 id | LDMS_SET_ID_DATA);
		if (data_addr == MAP_FAILED) {
			munmap(meta_addr, 8192);
			return -ENOMEM;
		}
		rc = ldms_mmap_set(meta_addr, data_addr, &map_set);
		if (rc) {
			ldms_log("Error encountered mmaping the set '%s', rc %d\n",
				 set_name, rc);
			return rc;
		}
		sh = meta_addr;
		p = meta_addr;
		ldms_log("addr: %p\n", meta_addr);
		for (i = 0; i < 256; i = i + j) {
			for (j = 0; j < 16; j++)
				ldms_log("%02x ", p[i+j]);
			ldms_log("\n");
			for (j = 0; j < 16; j++) {
				if (isalnum(p[i+j]))
					ldms_log("%2c ", p[i+j]);
				else
					ldms_log("%2s ", ".");
			}
			ldms_log("\n");
		}
		ldms_log("name: '%s'\n", sh->name);
		ldms_log("size: %d\n", sh->meta_size);
	}
	return 0;
}


char *skip_space(char *s)
{
	while (*s != '\0' && isspace(*s)) s++;
	if (*s == '\0')
		return NULL;
	return s;
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

struct plugin {
	void *handle;
	char *name;
	char *libpath;
	unsigned long sample_interval_us;
	int thread_id;
	int ref_count;
	union {
		struct ldmsd_plugin *plugin;
		struct ldmsd_sampler *sampler;
		struct ldmsd_store *store;
	};
	struct timeval timeout;
	struct event *event;
	pthread_mutex_t lock;
	LIST_ENTRY(plugin) entry;
};
LIST_HEAD(plugin_list, plugin) plugin_list;

struct plugin *get_plugin(char *name)
{
	struct plugin *p;
	LIST_FOREACH(p, &plugin_list, entry) {
		if (0 == strcmp(p->name, name))
			return p;
	}
	return NULL;
}

static char msg_buf[4096];
static void msg_logger(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsprintf(msg_buf, fmt, ap);
	ldms_log(msg_buf);
}

static char library_name[PATH_MAX];
struct plugin *new_plugin(char *plugin_name, char *err_str)
{
	struct ldmsd_plugin *lpi;
	struct plugin *pi = NULL;
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
	lpi = pget(msg_logger);
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

void destroy_plugin(struct plugin *p)
{
	free(p->libpath);
	free(p->name);
	LIST_REMOVE(p, entry);
	dlclose(p->handle);
	free(p);
}

/*
 * Return information about the state of the daemon
 */
int process_info(int fd,
		 struct sockaddr *sa, ssize_t sa_len,
		 char *command)
{
	int i;
	struct hostspec *hs;
	ldms_log("%-16s %s\n", "Thread", "Task Count");
	ldms_log("%-16s %s\n", "----------------", "------------");
	for (i = 0; i < ev_thread_count; i++) {
		ldms_log("%-16p %d\n",
			 (void *)ev_thread[i], ev_count[i]);
	}
	ldms_log("%-12s %-12s %-12s %-12s %-12s %-12s\n",
		 "Hostname", "Transport", "Set", "Metric",
		 "Store", "CompType", "CompId");
	ldms_log("%-12s %-12s %-12s %-12s %-12s %-12s\n",
		 "------------", "------------", "------------", "------------",
		 "------------", "------------", "------------");
	pthread_mutex_lock(&host_list_lock);
	LIST_FOREACH(hs, &host_list, link) {
		struct hostset *hset;
		ldms_log("%-12s %-12s\n", hs->hostname, hs->xprt_name);
		LIST_FOREACH(hset, &hs->set_list, entry) {
			struct hset_metric *hsm;
			ldms_log("%-12s %-12s %-12s\n",
				 "", "", hset->name);
			LIST_FOREACH(hsm, &hset->metric_list, entry) {
				ldms_log("%-12s %-12s %-12s "
					 "%-12s %-12s %-12s %-12d\n",
					 "", "", "", hsm->name,
					 hsm->metric_store->store->base.name,
					 hsm->metric_store->comp_type,
					 (int)hsm->comp_id);
			}
		}
	}
	pthread_mutex_unlock(&host_list_lock);
	sprintf(replybuf, "0");
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

/*
 * Load a plugin
 */
int process_load_plugin(int fd,
			struct sockaddr *sa, ssize_t sa_len,
			char *command)
{
	char *plugin_name;
	char err_str[128];
	char reply[128];
	int rc = 0;

	err_str[0] = '\0';

	plugin_name = av_value(av_list, "name");
	if (!plugin_name) {
		sprintf(err_str, "The plugin name was not specified\n");
		rc = EINVAL;
		goto out;
	}
	struct plugin *pi = get_plugin(plugin_name);
	if (pi) {
		sprintf(err_str, "Plugin already loaded");
		rc = EEXIST;
		goto out;
	}
	pi = new_plugin(plugin_name, err_str);
 out:
	sprintf(reply, "%d%s", -rc, err_str);
	send_reply(fd, sa, sa_len, reply, strlen(reply)+1);
	return 0;
}

/*
 * Destroy and unload the plugin
 */
int process_term_plugin(int fd,
			struct sockaddr *sa, ssize_t sa_len,
			char *command)
{
	char *plugin_name;
	char *err_str = "";
	int rc = 0;
	struct plugin *pi;

	plugin_name = av_value(av_list, "name");
	if (!plugin_name) {
		err_str = "The plugin name must be specified.";
		rc = EINVAL;
		goto out;
	}
	pi = get_plugin(plugin_name);
	if (!pi) {
		rc = ENOENT;
		err_str = "Plugin not found.";
		goto out;
	}
	pthread_mutex_lock(&pi->lock);
	if (pi->ref_count) {
		err_str = "The specified plugin has active users "
			"and cannot be terminated.\n";
		goto out;
	}
	pi->plugin->term();
	pthread_mutex_unlock(&pi->lock);
	destroy_plugin(pi);
	rc = 0;
 out:
	sprintf(replybuf, "%d%s", rc, err_str);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

/*
 * Configure a plugin
 */
int process_config_plugin(int fd,
			  struct sockaddr *sa, ssize_t sa_len,
			  char *command)
{
	char *plugin_name;
	char *err_str = "";
	int rc = 0;
	struct plugin *pi;

	plugin_name = av_value(av_list, "name");
	if (!plugin_name) {
		err_str = "The plugin name must be specified.";
		rc = EINVAL;
		goto out;
	}
	pi = get_plugin(plugin_name);
	if (!pi) {
		rc = ENOENT;
		err_str = "The plugin was not found.";
		goto out;
	}
	pthread_mutex_lock(&pi->lock);
	rc = pi->plugin->config(kw_list, av_list);
	if (rc)
		err_str = "Plugin configuration error.";
	pthread_mutex_unlock(&pi->lock);
 out:
	sprintf(replybuf, "%d%s", rc, err_str);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

void plugin_sampler_cb(int fd, short sig, void *arg)
{
	struct plugin *pi = arg;
	pthread_mutex_lock(&pi->lock);
	assert(pi->plugin->type == LDMSD_PLUGIN_SAMPLER);
	pi->sampler->sample();
	(void)evtimer_add(pi->event, &pi->timeout);
	pthread_mutex_unlock(&pi->lock);
}

/*
 * Start the sampler
 */
int process_start_sampler(int fd,
			 struct sockaddr *sa, ssize_t sa_len,
			 char *command)
{
	char *attr;
	char *err_str = "";
	int rc = 0;
	long sample_interval;
	struct plugin *pi;

	attr = av_value(av_list, "name");
	if (!attr) {
		err_str = "Invalid request syntax";
		rc = EINVAL;
		goto out_nolock;
	}
	pi = get_plugin(attr);
	if (!pi) {
		rc = ENOENT;
		err_str = "Sampler not found.";
		goto out_nolock;
	}
	pthread_mutex_lock(&pi->lock);
	if (pi->plugin->type != LDMSD_PLUGIN_SAMPLER) {
		rc = EINVAL;
		err_str = "The specified plugin is not a sampler.";
		goto out;
	}
	if (pi->thread_id >= 0) {
		rc = EBUSY;
		err_str = "Sampler is already running.";
		goto out;
	}
	attr = av_value(av_list, "interval");
	if (!attr) {
		rc = EINVAL;
		err_str = "The sample interval must be specified.";
		goto out;
	}
	sample_interval = strtol(attr, NULL, 0);
	pi->timeout.tv_sec = sample_interval / 1000000;
	pi->timeout.tv_usec = sample_interval % 1000000;
	pi->ref_count++;

	pi->thread_id = find_least_busy_thread();
	pi->event = evtimer_new(get_ev_base(pi->thread_id), plugin_sampler_cb, pi);
	rc = evtimer_add(pi->event, &pi->timeout);
 out:
	pthread_mutex_unlock(&pi->lock);
 out_nolock:
	sprintf(replybuf, "%d%s", rc, err_str);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

/*
 * Stop the sampler
 */
int process_stop_sampler(int fd,
			 struct sockaddr *sa, ssize_t sa_len,
			 char *command)
{
	char *plugin_name;
	char *err_str = "";
	int rc = 0;
	struct plugin *pi;

	plugin_name = av_value(av_list, "name");
	if (!plugin_name) {
		err_str = "The plugin name must be specified.";
		rc = EINVAL;
		goto out_nolock;
	}
	pi = get_plugin(plugin_name);
	if (!pi) {
		rc = ENOENT;
		err_str = "Sampler not found.";
		goto out_nolock;
	}
	pthread_mutex_lock(&pi->lock);
	/* Ensure this is a sampler */
	if (pi->plugin->type != LDMSD_PLUGIN_SAMPLER) {
		rc = EINVAL;
		err_str = "The specified plugin is not a sampler.";
		goto out;
	}
	if (pi->event) {
		evtimer_del(pi->event);
		event_free(pi->event);
		pi->event = NULL;
		release_ev_base(pi->thread_id);
		pi->thread_id = -1;
		pi->ref_count--;
	} else
		err_str = "The sampler is not running.";
 out:
	pthread_mutex_unlock(&pi->lock);
 out_nolock:
	sprintf(replybuf, "%d%s", rc, err_str);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
}

int process_ls_plugins(int fd,
		       struct sockaddr *sa, ssize_t sa_len,
		       char *command)
{
	struct plugin *p;
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

#define LDMSD_DEFAULT_GATHER_INTERVAL 1000000

int str_to_host_type(char *type)
{
	if (0 == strcmp(type, "active"))
		return ACTIVE;
	if (0 == strcmp(type, "passive"))
		return PASSIVE;
	if (0 == strcmp(type, "bridging"))
		return BRIDGING;
	return -1;
}

void do_active_host(struct hostspec *hs);
void do_passive_host(struct hostspec *hs);
void do_bridging_host(struct hostspec *hs);
void host_sampler_cb(int fd, short sig, void *arg)
{
	struct hostspec *hs = arg;

	switch (hs->type) {
	case ACTIVE:
		do_active_host(hs);
		break;
	case PASSIVE:
		do_passive_host(hs);
		break;
	case BRIDGING:
		do_bridging_host(hs);
		break;
	}

	if (!ldms_xprt_connected(hs->x)) {
		hs->timeout.tv_sec = hs->connect_interval / 1000000;
		hs->timeout.tv_usec = hs->connect_interval % 1000000;
	} else {
		hs->timeout.tv_sec = hs->sample_interval / 1000000;
		hs->timeout.tv_usec = hs->sample_interval % 1000000;
	}
	evtimer_add(hs->event, &hs->timeout);
}

int process_add_host(int fd,
		     struct sockaddr *sa, ssize_t sa_len,
		     char *command)
{
	int rc;
	struct sockaddr_in sin;
	struct hostspec *hs;
	char *attr;
	char *type;
	char *host;
	char *xprt;
	int host_type;
	long interval = LDMSD_DEFAULT_GATHER_INTERVAL;
	long port_no = LDMS_DEFAULT_PORT;

	/* Handle all the EINVAL cases first */
	attr = "type";
	type = av_value(av_list, attr);
	if (!type)
		goto einval;
	host_type = str_to_host_type(type);
 	if (host_type < 0) {
		sprintf(replybuf, "%d '%s' is an invalid host type.\n",
			-EINVAL, type);
		send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
		return EINVAL;
	}
	attr = "host";
	host = av_value(av_list, attr);
	if (!host) {
	einval:
		sprintf(replybuf, "%d The %s attribute must be specified\n",
			-EINVAL, attr);
		send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
		return EINVAL;
	}
	hs = calloc(1, sizeof(*hs));
	if (!hs)
		goto enomem;

	hs->hostname = strdup(host);
	if (!hs->hostname)
		goto enomem;

	rc = resolve(hs->hostname, &sin);
	if (rc) {
		sprintf(replybuf,
			"%d The host '%s' could not be resolved "
			"due to error %d.\n", -rc, hs->hostname, rc);
		send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
		rc = EINVAL;
		goto err;
	}
	attr = av_value(av_list, "port");
	if (attr)
		port_no = strtol(attr, NULL, 0);
	sin.sin_port = port_no;

	xprt = av_value(av_list, "xprt");
	if (xprt)
		xprt = strdup(xprt);
	else
		xprt = strdup("sock");
	if (!xprt)
		goto enomem;

	attr = av_value(av_list, "interval");
	if (attr)
		interval = strtol(attr, NULL, 0);

	sin.sin_port = htons(port_no);
	hs->type = host_type;
	hs->sin = sin;
	hs->xprt_name = xprt;
	hs->sample_interval = interval;
	hs->connect_interval = 20000000; /* twenty seconds */
	pthread_mutex_init(&hs->set_list_lock, 0);

	hs->thread_id = find_least_busy_thread();
	hs->event = evtimer_new(get_ev_base(hs->thread_id),
				host_sampler_cb, hs);
	/* First connection attempt happens 'right away' */
	hs->timeout.tv_sec = 0; // hs->connect_interval / 1000000;
	hs->timeout.tv_usec = 500000; // hs->connect_interval % 1000000;
	evtimer_add(hs->event, &hs->timeout);

	pthread_mutex_lock(&host_list_lock);
	LIST_INSERT_HEAD(&host_list, hs, link);
	pthread_mutex_unlock(&host_list_lock);

	sprintf(replybuf, "0");
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
 enomem:
	rc = ENOMEM;
	sprintf(replybuf, "%dMemory allocation failure.", -ENOMEM);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
 err:
	if (hs->hostname)
		free(hs->hostname);
	if (hs->xprt_name)
		free(hs->xprt_name);
	free(hs);
	return rc;
}

int store_set_metrics(struct hostset *hset,
		      char *comp_type,
		      char *metrics)
{
	char *metric_name;
	struct metric_store *ms;
	ldms_metric_t *m;
	struct ldms_value_desc *vd;
	struct hset_metric *hset_metric;
	struct ldms_iterator i;
	ldms_metric_t comp_id = ldms_get_metric(hset->set, "component_id");
	for (vd = ldms_first(&i, hset->set); vd; vd = ldms_next(&i)) {
		/* Ignore the component_id metric */
		if (0 == strcmp(vd->name, "component_id"))
			continue;
		if (!metrics || strstr(vd->name, metrics)) {
			/*
			 * Strip off the 'component index' from the
			 * metric name when searching for the store
			 */
			metric_name = strstr(vd->name, ":");
			if (metric_name)
				metric_name++;
			else
				metric_name = vd->name;
			ms = ldmsd_metric_store_get(hset->store, comp_type,
						    metric_name);
			if (!ms) {
				ldms_log("A metric store for comp_type %s and "
					 "metric_name %s could not be created.",
					 comp_type, vd->name);
				continue;
			}
			hset_metric = malloc(sizeof *hset_metric);
			if (!hset_metric) {
				ldms_log("ENOMEM adding metric %s.",
					 vd->name);
				continue;
			}
			m = ldms_make_metric(hset->set, vd);
			if (!m) {
				ldms_log("ENOMEM creating metric %s from value "
					 "descriptor.", vd->name);
				continue;
			}
			if (comp_id)
				hset_metric->comp_id = ldms_get_u64(comp_id);
			else
				hset_metric->comp_id = 0;
			int smidge = strtoul(vd->name, NULL, 0);
			hset_metric->name = strdup(vd->name);
			hset_metric->comp_id += smidge;
			hset_metric->metric = m;
			hset_metric->metric_store = ms;
			LIST_INSERT_HEAD(&hset->metric_list, hset_metric, entry);
		}
	}
	if (comp_id)
		ldms_metric_release(comp_id);
	return 0;
}

int store_host_set_metrics(struct hostspec *hs,
			   struct ldmsd_store *store,
			   char *set_name,
			   char *comp_type, char *metrics)
{
	int rc;
	struct hostset *hset;
	pthread_mutex_lock(&hs->set_list_lock);
	LIST_FOREACH(hset, &hs->set_list, entry) {
		char *tmp;
		char *hset_name;

		tmp = strdup(ldms_get_set_name(hset->set));
		hset_name = basename(tmp);
		if (0 != strcmp(set_name, hset_name)) {
			free(tmp);
			continue;
		}
		hset->store = store;
		rc = store_set_metrics(hset, comp_type, metrics);
		if (rc) {
			hset->store = NULL;
			free(tmp);
			return rc;
		}
		free(tmp);
	}
	pthread_mutex_unlock(&hs->set_list_lock);
	return ENOENT;
}

/*
 * store [attribute=value ...]
 * name=      The storage plugin name.
 * comp_type= The component type of the metric set.
 * set=       The set name containing the desired metric(s).
 * metrics=   A comma separated list of metric names. If not specified,
 *            all metrics in the metric set will be saved.
 * hosts=     The set of hosts whose data will be stoerd. If hosts is not
 *            specified, the metric will be saved for all hosts. If
 *            specified, the value should be a comma separated list of
 *            host names.
 */
int process_store(int fd,
		  struct sockaddr *sa, ssize_t sa_len,
		  char *command)
{
	char *err_str;
	char *set_name;
	char *store_name;
	char *comp_type;
	char *attr;
	char *metrics;
	char *hosts;
	struct hostspec *hs;
	struct plugin *store;

	attr = "name";
	store_name = av_value(av_list, attr);
	if (!store_name)
		goto einval;
	attr = "comp_type";
	comp_type = av_value(av_list, attr);
	if (!comp_type)
		goto einval;
	attr = "set";
	set_name = av_value(av_list, attr);
	if (!set_name)
		goto einval;

	store = get_plugin(store_name);
	if (!store) {
		err_str = "The storage plugin was not found.";
		goto enoent;
	}

	metrics = av_value(av_list, "metrics");
	hosts = av_value(av_list, "hosts");

	pthread_mutex_lock(&host_list_lock);
	LIST_FOREACH(hs, &host_list, link) {
		if (!hosts || strstr(hs->hostname, hosts))
			store_host_set_metrics(hs, store->store,
					       set_name, comp_type, metrics);
	}
	pthread_mutex_unlock(&host_list_lock);
	sprintf(replybuf, "0");
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return 0;
 einval:
	sprintf(replybuf, "-22 The '%s' attribute must be specified.", attr);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return EINVAL;
 enoent:
	sprintf(replybuf, "%d The plugin was not found.", -ENOENT);
	send_reply(fd, sa, sa_len, replybuf, strlen(replybuf)+1);
	return ENOENT;
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
	[LDMSCTL_EXIT_DAEMON] = process_exit,
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

	hset->refcount = 1;
	pthread_mutex_init(&hset->refcount_lock, NULL);
	return hset;
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

		while (!LIST_EMPTY(&hset->metric_list)) {
			struct hset_metric *hsm = LIST_FIRST(&hset->metric_list);
			LIST_REMOVE(hsm, entry);
			free(hsm->name);
			if (hsm->metric)
				ldms_metric_release(hsm->metric);
			if (hsm->metric_store)
				ldmsd_close_metric_store(hsm->metric_store);
		}
		free(hset->name);
		free(hset);
	}
}

/*
 * Host Type Descriptions:
 *
 * 'active' -
 *    - ldms_connect to a specified peer
 *    - ldms_dir and ldms_lookup the peer's metric sets
 *    - periodically performs an ldms_update of the peer's metric data
 *
 * 'bridging' - Designed to 'hop over' fire walls by initiating the connection
 *    - ldms_connect to a specified peer
 *
 * 'passive' - Designed as target side of 'bridging' host
 *    - searches list of incoming connections (connections it
 *      ldms_accepted) to find the matching peer (the bridging host
 *      that connected to it)
 *    - ldms_dir and ldms_lookup of the peer's metric data
 *    - periodically performs an ldms_update of the peer's metric data
 */

int sample_interval = 2000000;
void lookup_cb(ldms_t t, enum ldms_lookup_status status, ldms_set_t s, void *arg)
{
	struct hset_metric *hsm;
	struct hostset *hset = arg;
	if (status != LDMS_LOOKUP_OK){
		ldms_log("Error doing lookup for set\n",
			 ldms_get_set_name(s));
		hset->set = NULL;
		hset_ref_put(hset);
		return;
	}
	hset->set = s;
	/*
	 * Run the list of stored metrics and refresh the metric
	 * handle.
	 */
	LIST_FOREACH(hsm, &hset->metric_list, entry) {
		if (hsm->metric)
			ldms_metric_release(hsm->metric);
		hsm->metric = ldms_get_metric(hset->set, hsm->name);
	}
	hset_ref_put(hset);
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

void _add_cb(ldms_t t, struct hostspec *hs, const char *set_name)
{
	struct hostset *hset;
	int rc;

	ldms_log("Adding the metric set '%s'\n", set_name);

	/* Check to see if it's already there */
	hset = find_host_set(hs, set_name);
	if (!hset) {
		hset = hset_new();
		if (!hset) {
			ldms_log("Memory allocation failure in %s for set_name %s\n",
				 __FUNCTION__, set_name);
			return;
		}
		hset->name = strdup(set_name);
		hset->host = hs;

		pthread_mutex_lock(&hs->set_list_lock);
		LIST_INSERT_HEAD(&hs->set_list, hset, entry);
		pthread_mutex_unlock(&hs->set_list_lock);

		/* Take a lookup reference. Find takes one for us. */
		hset_ref_get(hset);
	}

	/* Refresh the set with a lookup */
	rc = ldms_lookup(hs->x, set_name, lookup_cb, hset);
	if (rc)
		ldms_log("Synchronous error %d from ldms_lookup\n", rc);
}

/*
 * Release the ldms set and metrics from a hostset record.
 */
void reset_set_metrics(struct hostset *hset)
{
	struct hset_metric *hsm;
	LIST_FOREACH(hsm, &hset->metric_list, entry) {
		if (hsm->metric) {
			ldms_metric_release(hsm->metric);
			hsm->metric = NULL;
		}
	}
	if (hset->set) {
		ldms_destroy_set(hset->set);
		hset->set = NULL;
	}
}

/*
 * Destroy the set and metrics associated with the set named in the
 * directory update.
 */
void _dir_cb_del(ldms_t t, struct hostspec *hs, const char *set_name)
{
	struct hostset *hset = find_host_set(hs, set_name);
	ldms_log("%s removing set '%s'\n", __FUNCTION__, set_name);
	if (hset) {
		reset_set_metrics(hset);
		hset_ref_put(hset);
	}
}

/*
 * Release all existing sets for the host. This is done when the
 * connection to the host has been lost. The hostset record is
 * retained, but the set and metrics are released. When the host comes
 * back, the set and metrics will be rebuilt.
 */
void reset_host(struct hostspec *hs)
{
	struct hostset *hset;
	LIST_FOREACH(hset, &hs->set_list, entry) {
		reset_set_metrics(hset);
	}
}

/*
 * Process the directory list and add or restore specified sets.
 */
void dir_cb_list(ldms_t t, ldms_dir_t dir, void *arg)
{
	struct hostspec *hs = arg;
	int i;

	for (i = 0; i < dir->set_count; i++)
		_add_cb(t, hs, dir->set_names[i]);
}

/*
 * Process the directory list and add or restore specified sets.
 */
void dir_cb_add(ldms_t t, ldms_dir_t dir, void *arg)
{
	struct hostspec *hs = arg;
	int i;
	for (i = 0; i < dir->set_count; i++)
		_add_cb(t, hs, dir->set_names[i]);
}

/*
 * Process the directory list and release the sets and metrics
 * associated with the specified sets.
 */
void dir_cb_del(ldms_t t, ldms_dir_t dir, void *arg)
{
	struct hostspec *hs = arg;
	int i;

	for (i = 0; i < dir->set_count; i++)
		_dir_cb_del(t, hs, dir->set_names[i]);
}

/*
 * The ldms_dir has completed. Decode the directory type and call the
 * appropriate handler function.
 */
void dir_cb(ldms_t t, int status, ldms_dir_t dir, void *arg)
{
	struct hostspec *hs = arg;
	if (status) {
		ldms_log("Error %d in lookup on host %s.\n",
		       status, hs->hostname);
		return;
	}
	switch (dir->type) {
	case LDMS_DIR_LIST:
		dir_cb_list(t, dir, hs);
		break;
	case LDMS_DIR_ADD:
		dir_cb_add(t, dir, hs);
		break;
	case LDMS_DIR_DEL:
		dir_cb_del(t, dir, hs);
		break;
	}
	ldms_dir_release(t, dir);
}

int do_connect(struct hostspec *hs, int do_dir)
{
	int ret;

	if (!hs->x) {
		hs->x = ldms_create_xprt(hs->xprt_name, ldms_log);
		if (hs->x)
			/* Take a reference since we're caching the handle */
			ldms_xprt_get(hs->x);
	}
	if (!hs->x) {
		ldms_log("Error creating transport '%s'.\n", hs->xprt_name);
		return -1;
	}
	ret  = ldms_connect(hs->x, (struct sockaddr *)&hs->sin,
			    sizeof(hs->sin));
	if (ret)
		return -1;

	ldms_log("Connected to host '%s:%hu'\n", hs->hostname,
		 ntohs(hs->sin.sin_port));
	if (do_dir)
		return ldms_dir(hs->x, dir_cb, hs, 1);
	return 0;
}

void update_complete_cb(ldms_t t, ldms_set_t s, int status, void *arg)
{
	struct hostset *hset = arg;
	struct hset_metric *hsm;
	struct ldmsd_store_tuple_s tuple;
	uint64_t gn;

	if (status) {
		ldms_log("Updated failed for set %s.\n",
			 (s ? ldms_get_set_name(s) : "UNKNOWN"));
		goto out;
	}

	gn = ldms_get_data_gn(hset->set);
	if (hset->gn == gn)
		goto out;

	if (!ldms_is_set_consistent(hset->set))
		goto out;

	hset->gn = gn;

	LIST_FOREACH(hsm, &hset->metric_list, entry) {
		struct ldms_timestamp const *ts;
		if (!hsm->metric) {
			hsm->metric = ldms_get_metric(hset->set, hsm->name);
			if (!hsm->metric)
				continue;
		}
		tuple.value = hsm->metric;
		tuple.comp_id  = hsm->comp_id;
		ts = ldms_get_timestamp(hset->set);
		tuple.tv.tv_sec = ts->sec;
		tuple.tv.tv_usec = ts->usec;
		ldmsd_store_tuple_add(hsm->metric_store, &tuple);
	}
 out:
	/* Put the reference taken at the call to ldms_update() */
	hset_ref_put(hset);
}

void update_data(struct hostspec *hs)
{
	int ret;
	struct hostset *hset;

	if (!hs->x)
		return;

	/* Take the host lock to protect the set_list */
	pthread_mutex_lock(&hs->set_list_lock);
	LIST_FOREACH(hset, &hs->set_list, entry) {
		if (!hset->set)
			continue;
		hset_ref_get(hset);
		ret = ldms_update(hset->set, update_complete_cb, hset);
		if (ret)
			ldms_log("Error %d updating metric set "
				 "on host %s:%d[%s].\n", ret, hs->hostname,
				 ntohs(hs->sin.sin_port), hs->xprt_name);
	}
	pthread_mutex_unlock(&hs->set_list_lock);
}

void do_active_host(struct hostspec *hs)
{
	if (!hs->x || !ldms_xprt_connected(hs->x)) {
		reset_host(hs);
		do_connect(hs, 1);
	} else
		/* Don't update immediately after connecting to
		 * provide time for dir/lookups to complete */
		update_data(hs);
}

int do_passive_connect(struct hostspec *hs)
{
	ldms_t l = ldms_xprt_find(&hs->sin);
	if (!l)
		return -1;

	/*
	 * ldms_xprt_find takes a reference on the transport so we can
	 * cache it here.
	 */
	hs->x = l;

	return ldms_dir(hs->x, dir_cb, hs, 1);
}

void do_passive_host(struct hostspec *hs)
{
	if (!hs->x) {
		do_passive_connect(hs);
		return;
	}
	if (!ldms_xprt_connected(hs->x)) {
		/* Transport closed by our bridge peer, release our
		 * reference and wait for reconnect */
		ldms_release_xprt(hs->x);
		hs->x = 0;
		return;
	}
	update_data(hs);
}

void do_bridging_host(struct hostspec *hs)
{
	if (!hs->x)
		do_connect(hs, 0);
	if (!ldms_xprt_connected(hs->x))
		do_connect(hs, 0);
}

int process_message(int sock, struct msghdr *msg, ssize_t msglen)
{
	return process_record(sock,
			      msg->msg_name, msg->msg_namelen,
			      msg->msg_iov->iov_base, msglen);
}

void keepalive_cb(int fd, short sig, void *arg)
{
	struct event *keepalive = arg;
	struct timeval keepalive_to;

	keepalive_to.tv_sec = 10;
	keepalive_to.tv_usec = 0;
	evtimer_add(keepalive, &keepalive_to);
}

void *event_proc(void *v)
{
	struct event_base *sampler_base = v;
	struct timeval keepalive_to;
	struct event *keepalive;
	keepalive = evtimer_new(sampler_base, keepalive_cb, NULL);
	keepalive_to.tv_sec = 10;
	keepalive_to.tv_usec = 0;
	evtimer_assign(keepalive, sampler_base, keepalive_cb, keepalive);
	evtimer_add(keepalive, &keepalive_to);
	event_base_loop(sampler_base, 0);
	ldms_log("Exiting the sampler thread.\n");
	return NULL;
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

void listen_on_transport(char *transport_str)
{
	char *name;
	char *port_s;
	int port_no;
	ldms_t l;
	int ret;
	struct sockaddr_in sin;

	ldms_log("Listening on transport %s\n", transport_str);
	name = strtok(transport_str, ":");
	port_s = strtok(NULL, ":");
	if (!port_s)
		port_no = LDMS_DEFAULT_PORT;
	else
		port_no = atoi(port_s);

	l = ldms_create_xprt(name, ldms_log);
	if (!l) {
		ldms_log("The transport specified, '%s', is invalid.\n", name);
		cleanup(6);
	}
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = 0;
	sin.sin_port = htons(port_no);
	ret = ldms_listen(l, (struct sockaddr *)&sin, sizeof(sin));
	if (ret) {
		ldms_log("Error %d listening on the '%s' transport.\n",
			 ret, name);
		cleanup(7);
	}
}

void ev_log_cb(int sev, const char *msg)
{
	const char *sev_s[] = {
		"EV_DEBUG",
		"EV_MSG",
		"EV_WARN",
		"EV_ERR"
	};
	ldms_log("%s: %s\n", sev_s[sev], msg);
}

int setup_control(char *sockname)
{
	struct sockaddr_un sun;
	int ret;

	if (!sockname) {
		char *sockpath = getenv("LDMSD_SOCKPATH");
		if (!sockpath)
			sockpath = "/var/run";
		sockname = malloc(sizeof(LDMSD_CONTROL_SOCKNAME) + strlen(sockpath) + 2);
		sprintf(sockname, "%s/%s", sockpath, LDMSD_CONTROL_SOCKNAME);
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

int setup_inet_control(void)
{
	struct sockaddr_in sin;
	int ret;
	/* Create listener */
	muxr_s = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (muxr_s < 0) {
		ldms_log("Error %d creating muxr socket.\n", muxr_s);
		return -1;
	}

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = 0;
	sin.sin_port = 31415;

	/* Bind to our public name */
	ret = bind(muxr_s, (struct sockaddr *)&sin, sizeof(sin));
	if (ret < 0) {
		ldms_log("Error %d binding control socket.\n", errno);
		return -1;
	}
	bind_succeeded = 1;

	ret = pthread_create(&ctrl_thread, NULL, inet_ctrl_thread_proc, 0);
	if (ret) {
		ldms_log("Error %d creating the control thread.\n");
		return -1;
	}
	return 0;
}

typedef enum {
	LDMS_HOSTNAME=0,
	LDMS_HOSTTYPE,
	LDMS_THREAD_COUNT,
	LDMS_INTERVAL,
	LDMS_KERNEL_METRIC,
	LDMS_KERNEL_METRIC_SET,
	LDMS_TEST_SET_COUNT,
	LDMS_TEST_SET_PREFIX,
	LDMS_TEST_METRIC_COUNT,
	LDMS_NOTIFY,
	LDMS_TRANSPORT,
	LDMS_SOCKNAME,
	LDMS_LOGFILE,
	LDMS_VERBOSE,
	LDMS_FOREGROUND,
	LDMS_CONFIG,
	LDMS_INSTANCE,
	LDMS_N_OPTIONS
} LDMS_OPTION;

char *YAML_TYPE_STR[] = {
	"NONE",
	"SCALAR",
	"SEQUENCE",
	"MAPPING"
};

/*
 * has_arg will be used to indicate if the command-line argument has been
 * set or not. The command-line arguments override those in configuration
 * file.
 */
int has_arg[LDMS_N_OPTIONS] = {[0] = 0}; // According to gcc doc (v. 4.4.7
                                         // section 5.23 Designated
					 // Initializersi) initialize array
					 // by some indices
					 // will zero-out the other elements
					 // not specified by the indices.

#ifdef ENABLE_YAML

/**
 * Get attribute value node from a given mapping node.
 *
 * return NULL if there is no such attribute, otherwise return the pointer
 *        to the value node.
 */
yaml_node_t* yaml_node_get_attr_value_node(yaml_document_t *yaml_document,
		yaml_node_t *node, char *attr)
{
	if (node->type != YAML_MAPPING_NODE)
		return NULL;

	yaml_node_t *key_node, *value_node;
	yaml_node_pair_t *itr;

	for (itr = node->data.mapping.pairs.start;
			itr < node->data.mapping.pairs.top;
		 	itr++) {
		key_node = yaml_document_get_node(yaml_document, itr->key);
		value_node = yaml_document_get_node(yaml_document, itr->value);
		if (key_node->type == YAML_SCALAR_NODE &&
				strcmp(key_node->data.scalar.value, attr) == 0) {
			// found it
			return value_node;
		}
	}

	return NULL;
}



/**
 * Get attribute value from a mapping node, identified by 'attr' key.
 *
 * return NULL if no such attribute or the value of the attribute is non-scalar
 *        otherwise, return the string value.
 */
char* yaml_node_get_attr_value_str(yaml_document_t *yaml_document,
		yaml_node_t *node, char *attr)
{
	yaml_node_t *value_node
		= yaml_node_get_attr_value_node(yaml_document, node, attr);
	if (value_node && value_node->type == YAML_SCALAR_NODE)
		return value_node->data.scalar.value;
	return NULL;
}

int yaml_node_get_attr_value_int(yaml_document_t *yaml_document,
		yaml_node_t *node, char *attr)
{
	yaml_node_t *value_node
		= yaml_node_get_attr_value_node(yaml_document, node, attr);
	if (value_node && value_node->type == YAML_SCALAR_NODE)
		return atoi(value_node->data.scalar.value);
	return 0;
}

/**
 * Parse the interval string (e.g. "60 m").
 *
 * return The number of microseconds
 */
int parse_interval(char *str)
{
	char unit[32];
	int number;

	sscanf(str, "%d %s", &number, unit);

	if (strcmp(unit, "hr") == 0) {
		number = number * 3600 * 1000000;
	} else if (strcmp(unit, "m") == 0) {
		number = number * 60 * 1000000;
	} else if (strcmp(unit, "s") == 0) {
		number = number * 1000000;
	} else if (strcmp(unit, "ms") == 0) {
		number = number * 1000;
	} else if (strcmp(unit, "us") == 0) {
		/* already microseconds: do nothing */
	} else {
		LDMSD_ERROR_EXIT("Unknown interval unit: %s\n", unit);
	}

	return number;
}

/**
 * parse_config_file parses the given configuration file (\a cfg_file)
 * into \a yaml_document structure.
 *
 * \param cfg_file A path of a confguration file
 *
 * return A pointer to the parsed yaml_document.
 */
yaml_document_t* parse_config_file(char *cfg_file)
{
	FILE *fcfg = fopen(cfg_file, "rt");
	if (!fcfg) {
		fprintf(stderr, "Cannot open file: %s", cfg_file);
		return NULL;
	}

	if (!yaml_parser_initialize(&yaml_parser))
		LDMSD_ERROR_EXIT("yaml initialization error");

	yaml_parser_set_input_file(&yaml_parser, fcfg);

	yaml_document_t *yaml_document = calloc(1, sizeof(yaml_document_t));

	if (!yaml_parser_load(&yaml_parser, yaml_document))
		LDMSD_ERROR_EXIT("yaml load error");

	return yaml_document;
}

void ldms_yaml_cmdline_option_handling(yaml_node_t *key_node,
	yaml_node_t *value_node)
{
	char *key_str = key_node->data.scalar.value;
	// Abusively set the value_str for shorter code.
	// Each case will also check the type first anyway.
	char *value_str = value_node->data.scalar.value;
	yaml_node_type_t node_type = value_node->type;

	if (strcmp(key_str, "ldmstype") == 0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_HOSTTYPE])
			strcpy(ldmstype, value_str);
	} else 	if (strcmp(key_str, "hostname")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_HOSTNAME])
			strcpy(myhostname, value_str);
	} else if (strcmp(key_str, "thread_count")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_THREAD_COUNT])
			ev_thread_count = atoi(value_str);
	} else if (strcmp(key_str, "interval")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_INTERVAL])
			sample_interval = parse_interval(value_str);
	} else if (strcmp(key_str, "kernel_metric")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_KERNEL_METRIC])
			if (strcasecmp("true", value_str) == 0
					|| atoi(value_str))
				do_kernel = 1;
	} else if (strcmp(key_str, "kernel_metric_set")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_KERNEL_METRIC_SET])
			setfile = strdup(value_str);
	} else if (strcmp(key_str, "test_set_count")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_TEST_SET_COUNT])
			test_set_count = atoi(value_str);
	} else if (strcmp(key_str, "test_set_prefix")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_TEST_SET_PREFIX])
			test_set_name = strdup(value_str);
	} else if (strcmp(key_str, "test_metric_count")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_TEST_METRIC_COUNT])
			test_metric_count = atoi(value_str);
	} else if (strcmp(key_str, "notify")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_NOTIFY])
			notify = 1;
	} else if (strcmp(key_str, "transport")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_TRANSPORT])
			listen_arg = strdup(value_str);
	} else if (strcmp(key_str, "sockname")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_SOCKNAME])
			sockname = strdup(value_str);
	} else if (strcmp(key_str, "logfile")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_LOGFILE])
			logfile = strdup(value_str);
	} else if (strcmp(key_str, "verbose")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_VERBOSE])
			if (strcasecmp("true", value_str) == 0 ||
					atoi(value_str))
				quiet = 0;
	} else if (strcmp(key_str, "foreground")==0) {
		LDMS_ASSERT(node_type == YAML_SCALAR_NODE);
		if (!has_arg[LDMS_FOREGROUND])
			if (strcasecmp("true", value_str) == 0 ||
					atoi(value_str))
				foreground = 1;
	}
}

/**
 * A routine for initializing ldmsd with options specified in
 * \a yaml_document. Note that those plugin loading/configuration will
 * be handled later in config_file_routine(yaml_document_t*).
 *
 * \param yaml_document The (pointer to) yaml document structure.
 *
 */
void initial_config_file_routine(yaml_document_t *yaml_document)
{

	yaml_node_t *root = yaml_document_get_root_node(yaml_document);

	/*
	 * For empty document, just return with a warning
	 */
	if (!root) {
		fprintf(stderr, "WARNING: yaml document is empty\n");
		return;
	}

	LDMS_ASSERT(root->type == YAML_SEQUENCE_NODE);

    yaml_node_item_t *itr = root->data.sequence.items.start;
	char hostname[256];

	if (gethostname(hostname, 256))
		LDMSD_ERROR_EXIT("Cannot get hostname, err(%d): %s\n",
			errno, sys_errlist[errno]);

    for (itr = root->data.sequence.items.start;
			itr < root->data.sequence.items.top;
			itr++) {
        yaml_node_t *node = yaml_document_get_node(yaml_document, *itr);
		// Now, each item should be a mapping
		if (node->type != YAML_MAPPING_NODE)
			continue;

		// Check if this is a ldmsd configuration for this host
		char *yaml_hostname
			= yaml_node_get_attr_value_str(yaml_document, node, "hostname");
		int inst
			= yaml_node_get_attr_value_int(yaml_document, node, "instance");
		if (!inst)
			inst = 1;

		if (!yaml_hostname
				|| strcmp(hostname, yaml_hostname) != 0
				|| inst != instance_number )
			continue; // skip if this is not for this host

        yaml_node_pair_t *itr2 = node->data.mapping.pairs.start;
        for (itr2 = node->data.mapping.pairs.start;
				itr2 < node->data.mapping.pairs.top;
				itr2++) {
            yaml_node_t *key_node = yaml_document_get_node(yaml_document,
                itr2->key);
            yaml_node_t *value_node = yaml_document_get_node(yaml_document,
                itr2->value);
			LDMS_ASSERT(key_node->type == YAML_SCALAR_NODE);
			ldms_yaml_cmdline_option_handling(key_node, value_node);
        }

		// After the configuration for this host is found, no need to
		// continue looking anymore .. hence break it
		break;

    } // end for itr1
}

/**
 * similar to sprintf, but specificly for printing key=value
 * pair (or key=value1,value2,... in case of sequence) into
 * the input \a buff.
 *
 * \param buff The character buffer to print into
 * \param yaml_document The pointer to the working yaml document.
 * \param key_node The pointer to the key node.
 * \param value_node The pointer to the value node.
 * \return The number of bytes printed to \a buff.
 */
int sprint_attribute(char *buff, yaml_document_t *yaml_document,
		yaml_node_t *key_node, yaml_node_t *value_node)
{
	int bytes = 0;
	yaml_node_item_t *itr;
	yaml_node_t *node;
	int not_first = 0;
	LDMS_ASSERT(key_node->type == YAML_SCALAR_NODE);
	bytes += sprintf(buff+bytes, "%s=", key_node->data.scalar.value);
	switch (value_node->type) {
	case YAML_SCALAR_NODE:
		bytes += sprintf(buff+bytes, "%s", value_node->data.scalar.value);
		break;
	case YAML_SEQUENCE_NODE:
		for (itr = value_node->data.sequence.items.start;
				itr < value_node->data.sequence.items.top;
				itr++) {
			node = yaml_document_get_node(yaml_document, *itr);
			LDMS_ASSERT(node->type == YAML_SCALAR_NODE);
			if (not_first)
				bytes += sprintf(buff+bytes, ",");
			else
				not_first = 1;
			bytes += sprintf(buff+bytes, "%s", node->data.scalar.value);
		}
		break;
	default:
		LDMSD_ERROR_EXIT("Unexpexted node type %s",
				YAML_TYPE_STR[value_node->type]);
	}

	return bytes;
}

/**
 * Handling the host list from the configuration file.
 *
 * \param hlist_node The yaml node to the host list (sequence).
 */
void ldms_yaml_host_list_handling(yaml_document_t *yaml_document,
		yaml_node_t *hlist_node)
{
	LDMS_ASSERT(hlist_node->type == YAML_SEQUENCE_NODE);
	char *host;
	char *port;
	char *type;
	char *interval;
	int interval_int;
	char *xport;
	char *h_inst;

	char add_host_cmd[1024];

	yaml_node_item_t *itr;
	for (itr = hlist_node->data.sequence.items.start;
			itr < hlist_node->data.sequence.items.top;
			itr++) {
		yaml_node_t *node = yaml_document_get_node(yaml_document, *itr);
		LDMS_ASSERT(node->type == YAML_MAPPING_NODE);
		host = yaml_node_get_attr_value_str(yaml_document, node, "host");
		port = yaml_node_get_attr_value_str(yaml_document, node, "port");
		type = yaml_node_get_attr_value_str(yaml_document, node, "type");
		h_inst = yaml_node_get_attr_value_str(yaml_document, node, "instance");
		interval = yaml_node_get_attr_value_str(yaml_document,
				node, "interval");
		interval_int = (interval)?parse_interval(interval):0;
		xport = yaml_node_get_attr_value_str(yaml_document, node, "xprt");
		char *cmd = add_host_cmd;
		cmd[0] = 0;
		cmd += sprintf(cmd, "%d", LDMSCTL_ADD_HOST);

		if (host)
			cmd += sprintf(cmd, " host=%s", host);
		else
			LDMSD_ERROR_EXIT("host is needed in each item in the hostlist");

		if (port)
			cmd += sprintf(cmd, " port=%s", port);

		if (h_inst)
			cmd += sprintf(cmd, " instance=%s", h_inst);

		if (type)
			cmd += sprintf(cmd, " type=%s", type);
		else
			LDMSD_ERROR_EXIT("type is needed in each item in the hostlist");

		if (interval_int)
			cmd += sprintf(cmd, " interval=%d", interval_int);

		if (xport)
			cmd += sprintf(cmd, " xprt=%s", xport);

		if (process_record(0, 0, 0, add_host_cmd, 0) != 0)
			fprintf(stderr, "WARNING: Some error in list\n");
	} // end for (host list)
}

int is_scalar_seq(yaml_document_t *yaml_document, yaml_node_t *node)
{
	if (node->type != YAML_SEQUENCE_NODE)
		return 0;
	yaml_node_item_t *itr;
	yaml_node_t *child;
	for (itr = node->data.sequence.items.start;
			itr < node->data.sequence.items.top;
			itr++) {
		child = yaml_document_get_node(yaml_document, *itr);
		if (child->type != YAML_SCALAR_NODE)
			return 0;
	}
	return 1;
}

void ldms_yaml_plugin_single_handling(yaml_document_t *yaml_document,
		yaml_node_t *node)
{
	char buff[1024];

	// First: load the plugin (if not existed)
	char *name = yaml_node_get_attr_value_str(yaml_document, node, "name");
	if (!name)
		LDMSD_ERROR_EXIT("Attribute name is needed for each plugin");
	char *type = yaml_node_get_attr_value_str(yaml_document, node, "type");
	if (!type)
		LDMSD_ERROR_EXIT("Attribute type is needed for each plugin");

	if (!get_plugin(name))
		if (!new_plugin(name, buff))
			LDMSD_ERROR_EXIT("Cannot load plugin %s, error: %s", name, buff);

	// Second: configure
	//  -> need to construct the command string
	char *cmd = buff;
	cmd += sprintf(cmd, "%d", LDMSCTL_CFG_PLUGIN);
	yaml_node_pair_t *itr2;
	for (itr2 = node->data.mapping.pairs.start;
			itr2 < node->data.mapping.pairs.top;
			itr2++) {
		yaml_node_t *key_node
			= yaml_document_get_node(yaml_document, itr2->key);
		yaml_node_t *value_node
			= yaml_document_get_node(yaml_document, itr2->value);
		// The value can be only scalar or sequence of scalar

		if (value_node->type == YAML_SCALAR_NODE
			|| is_scalar_seq(yaml_document, value_node)) {
			cmd += sprintf(cmd, " ");
			cmd += sprint_attribute(cmd, yaml_document, key_node, value_node);
		}
	}

	process_record(0, 0, 0, buff, 0); // Now, issue the config command

/* The following codes has been disabled due to the lack of 'on_update' support
 * in LDMS. This on_update hook has been introduced into LDMS on build2, but not
 * on hekili (or it has been added and taken out).
 *
 * Narate: I decide to keep the code to enable it later, after we add the
 * on_update feature back in.
 */
#if 0
	// Third: the update command
	yaml_node_t *update_node = yaml_node_get_attr_value_node(yaml_document,
			node, "on_update");

	if (update_node) {
		if (update_node->type != YAML_SEQUENCE_NODE)
			LDMSD_ERROR_EXIT("on_update attribute need to be a sequence"
					", but got %s", YAML_TYPE_STR[update_node->type]);

		yaml_node_item_t *seq_itr;
		yaml_node_pair_t *map_itr;
		yaml_node_t *map_node;
		for (seq_itr = update_node->data.sequence.items.start;
				seq_itr < update_node->data.sequence.items.top;
				seq_itr++) {
			cmd = buff;
			cmd += sprintf(cmd, "%d %s=%s", LDMSCTL_UPDATE, type, name);
			map_node = yaml_document_get_node(yaml_document, *seq_itr);
			for (map_itr = map_node->data.mapping.pairs.start;
					map_itr < map_node->data.mapping.pairs.top;
					map_itr++) {
				cmd += sprintf(cmd, " ");
				yaml_node_t *key_node = yaml_document_get_node(yaml_document,
						map_itr->key);
				yaml_node_t *value_node = yaml_document_get_node(yaml_document,
						map_itr->value);
				cmd += sprint_attribute(cmd,
						yaml_document, key_node, value_node);
			}
			// Then, issue the update command
			process_record(0, 0, 0, buff, 0);
		} // for (update)
	}
#endif
	// (Another) Third: start command for metric plugins
	if (strcmp(type, "metric") == 0) {
		char *intv_str = yaml_node_get_attr_value_str(yaml_document, node, "interval");
		int intv = (intv_str)?parse_interval(intv_str):0;
		cmd = buff;
		cmd += sprintf(cmd, "%d name=%s",
				LDMSCTL_START_SAMPLER, name);
		if (intv)
			cmd += sprintf(cmd, " interval=%d", intv);
		process_record(0, 0, 0, buff, 0);
	}

}

/**
 * Processing the list of plugins
 */
void ldms_yaml_plugin_list_handling(yaml_document_t *yaml_document,
		yaml_node_t *plist_node)
{
	LDMS_ASSERT(plist_node->type == YAML_SEQUENCE_NODE);
	// For each plugin, do the loading first (if it does not exists)
	// then config and udpate commands.


	yaml_node_item_t *itr;
	for (itr = plist_node->data.sequence.items.start;
			itr < plist_node->data.sequence.items.top;
			itr++) {
		// EXPECTING A MAPPING
		yaml_node_t *node = yaml_document_get_node(yaml_document, *itr);
		LDMS_ASSERT(node->type == YAML_MAPPING_NODE);

		ldms_yaml_plugin_single_handling(yaml_document, node);

	} // for (plugin)
}

/**
 * Configure the ldmsd's non-commandline options, e.g. adding hosts and
 * plugins.
 *
 * \param yaml_document The (pointer to) yaml document structure.
 */
void config_file_routine(yaml_document_t *yaml_document)
{
	yaml_node_t *root = yaml_document_get_root_node(yaml_document);

	/*
	 * For empty document, just return with a warning
	 */
	if (!root) {
		ldms_log("WARNING: yaml document is empty\n");
		return;
	}

	LDMS_ASSERT(root->type == YAML_SEQUENCE_NODE);

    yaml_node_item_t *itr = root->data.sequence.items.start;
	char hostname[256];

	if (gethostname(hostname, 256))
		LDMSD_ERROR_EXIT("Cannot get hostname, err(%d): %s\n",
				errno, sys_errlist[errno]);

    for (itr = root->data.sequence.items.start;
			itr < root->data.sequence.items.top;
			itr++) {
		yaml_node_t *node = yaml_document_get_node(yaml_document, *itr);
		// Now, each item should be a mapping
		if (node->type != YAML_MAPPING_NODE)
			continue;

		// Check if this is a ldmsd configuration for this host
		char *yaml_hostname
			= yaml_node_get_attr_value_str(yaml_document, node, "hostname");
		int inst
			= yaml_node_get_attr_value_int(yaml_document, node, "instance");
		if (!inst)
			inst = 1;

		if (!yaml_hostname
				|| strcmp(hostname, yaml_hostname) != 0
				|| inst != instance_number )
			continue; // skip if this is not for this host / instance

		// Now, looking for hosts and plugins configurations

		// For add host commands:
		yaml_node_t *host_list_node =
			yaml_node_get_attr_value_node(yaml_document, node, "host_list");

		if (host_list_node) {
			ldms_yaml_host_list_handling(yaml_document, host_list_node);
			sleep(10);
		}

		// For plugin commands:
		yaml_node_t *plugin_list_node =
			yaml_node_get_attr_value_node(yaml_document, node, "plugin_list");

		if (plugin_list_node)
			ldms_yaml_plugin_list_handling(yaml_document, plugin_list_node);

		ldms_log("Done plugin_list_handling\n");

		// After the configuration for this host is found, no need to
		// continue looking anymore .. hence break it
		break;

    } // end for itr1
}
#endif /* ENABLE_YAML */

size_t __get_mem_size(const char *s)
{
    char unit;
    char tmp[256];
    sprintf(tmp, "%s%s", s, "B");
    size_t size;
    sscanf(tmp, "%lu %c", &size, &unit);
    switch (unit) {
    case 'b':
    case 'B':
            return size;
    case 'k':
    case 'K':
            return size * 1024L;
    case 'm':
    case 'M':
            return size * 1024L * 1024L;
    case 'g':
    case 'G':
            return size * 1024L * 1024L * 1024L;
    case 't':
    case 'T':
            return size * 1024L * 1024L * 1024L;
    default:
            return 0;
    }
}

int main(int argc, char *argv[])
{
	int ret;
	int op;
	ldms_set_t test_set;
	log_fp = stdout;
	char *cfg_file = NULL;

	signal(SIGHUP, cleanup);
	signal(SIGINT, cleanup);
	signal(SIGTERM, cleanup);

	opterr = 0;
	while ((op = getopt(argc, argv, FMT)) != -1) {
		switch (op) {
		case 'I':
			// Assigned instance number
			instance_number = atoi(optarg);
			if (!instance_number)
				instance_number = 1;
			has_arg[LDMS_INSTANCE] = 1;
			break;
		case 'H':
			strcpy(myhostname, optarg);
			has_arg[LDMS_HOSTNAME] = 1;
			break;
		case 'i':
			sample_interval = atoi(optarg);
			has_arg[LDMS_INTERVAL] = 1;
			break;
		case 'k':
			do_kernel = 1;
			has_arg[LDMS_KERNEL_METRIC] = 1;
			break;
		case 'x':
			listen_arg = strdup(optarg);
			has_arg[LDMS_TRANSPORT] = 1;
			break;
		case 'S':
			/* Set the SOCKNAME to listen on */
			sockname = strdup(optarg);
			has_arg[LDMS_SOCKNAME] = 1;
			break;
		case 'l':
			logfile = strdup(optarg);
			has_arg[LDMS_LOGFILE] = 1;
			break;
		case 's':
			setfile = strdup(optarg);
			has_arg[LDMS_KERNEL_METRIC_SET] = 1;
			break;
		case 'v':
			quiet = 0;
			has_arg[LDMS_VERBOSE] = 1;
			break;
		case 'C':
		#ifdef ENABLE_YAML
			cfg_file = strdup(optarg);
			has_arg[LDMS_CONFIG] = 1;
		#else
			fprintf(stderr, "ERROR: ldmsd was compiled without"
					" yaml support\n");
			return -1;
		#endif
			break;
		case 'F':
			foreground = 1;
			has_arg[LDMS_FOREGROUND] = 1;
			break;
		case 'T':
			test_set_name = strdup(optarg);
			has_arg[LDMS_TEST_SET_PREFIX] = 1;
			break;
		case 't':
			test_set_count = atoi(optarg);
			has_arg[LDMS_TEST_SET_COUNT] = 1;
			break;
		case 'P':
			ev_thread_count = atoi(optarg);
			has_arg[LDMS_THREAD_COUNT] = 1;
			break;
		case 'N':
			notify = 1;
			has_arg[LDMS_NOTIFY] = 1;
			break;
		case 'M':
			test_metric_count = atoi(optarg);
			has_arg[LDMS_TEST_METRIC_COUNT] = 1;
			break;
		case 'm':
			if ((max_mem_size = __get_mem_size(optarg)) == 0) {
				printf("Invalid memory size '%s'\n", optarg);
				usage(argv);
			}
			break;
		default:
			usage(argv);
		}
	}
#ifdef ENABLE_YAML
	yaml_document_t *yaml_document = NULL;
	if (cfg_file) {
		yaml_document = parse_config_file(cfg_file);
		if (yaml_document)
			initial_config_file_routine(yaml_document);
	}
#endif
	if (!foreground) {
		if (daemon(1, 1)) {
			perror("ldmsd: ");
			cleanup(8);
		}
	}
	if (logfile) {
		log_fp = fopen(logfile, "a");
		if (!log_fp) {
			log_fp = stdout;
			ldms_log("Could not open the log file named '%s'\n", logfile);
			cleanup(9);
		}
		stdout = log_fp;
	}

	/* Initialize LDMS */
	if (ldms_init(max_mem_size)) {
		ldms_log("LDMS could not pre-allocate the memory of size %lu.\n",
								max_mem_size);
		exit(1);
	}

	evthread_use_pthreads();
	event_set_log_callback(ev_log_cb);

	ev_count = calloc(ev_thread_count, sizeof(int));
	if (!ev_count) {
		ldms_log("Memory allocation failure.\n");
		exit(1);
	}
	ev_base = calloc(ev_thread_count, sizeof(struct event_base *));
	if (!ev_base) {
		ldms_log("Memory allocation failure.\n");
		exit(1);
	}
	ev_thread = calloc(ev_thread_count, sizeof(pthread_t));
	if (!ev_thread) {
		ldms_log("Memory allocation failure.\n");
		exit(1);
	}
	for (op = 0; op < ev_thread_count; op++) {
		ev_base[op] = event_init();
		if (!ev_base[op]) {
			ldms_log("Error creating an event base.\n");
			cleanup(6);
		}
		ret = pthread_create(&ev_thread[op], NULL, event_proc, ev_base[op]);
		if (ret) {
			ldms_log("Error %d creating the event thread.\n", ret);
			cleanup(7);
		}
	}

	if (myhostname[0] == '\0') {
		ret = gethostname(myhostname, sizeof(myhostname));
		if (ret)
			myhostname[0] = '\0';
	}

	/* Create the control socket parsing structures */
	av_list = av_new(128);
	kw_list = av_new(128);

	/* Create the test sets */
	ldms_set_t *test_sets = calloc(test_set_count, sizeof(ldms_set_t));
	ldms_metric_t *test_metrics = calloc(test_set_count, sizeof(ldms_metric_t));
	if (!test_metrics) {
		ldms_log("Could not create test_metrics table to contain %d items\n",
			 test_set_count);
		cleanup(10);
	}
	if (test_set_name) {
		int set_no;
		static char test_set_name_no[1024];
		for (set_no = 1; set_no <= test_set_count; set_no++) {
			int j;
			ldms_metric_t m;
			char metric_name[32];
			sprintf(test_set_name_no, "%s/%s_%d",
				myhostname, test_set_name, set_no);
			ldms_create_set(test_set_name_no, 2048, 2048, &test_set);
			test_sets[set_no-1] = test_set;
			if (test_metric_count > 0) {
				m = ldms_add_metric(test_set, "component_id",
						    LDMS_V_U64);
				ldms_set_u64(m, (uint64_t)1);
				test_metrics[set_no-1] = m;
			}
			for (j = 1; j <= test_metric_count; j++) {
				sprintf(metric_name, "metric_no_%d", j);
				m = ldms_add_metric(test_set, metric_name,
						    LDMS_V_U64);
				ldms_set_u64(m, (uint64_t)(set_no * j));
			}
		}
	} else
		test_set_count = 0;

	if (!setfile)
		setfile = LDMSD_SETFILE;

	if (!logfile)
		logfile = LDMSD_LOGFILE;

	ldms_log("Started LDMS Daemon version " VERSION "\n");
	if (do_kernel && publish_kernel(setfile))
		cleanup(3);

	if (setup_control(sockname))
		cleanup(4);

	if (ldmsd_store_init()) {
		ldms_log("Could not initialize the storage subsystem.\n");
		cleanup(7);
	}

	if (listen_arg)
		listen_on_transport(listen_arg);

#ifdef ENABLE_YAML
	// Now handle the other configuration

	if (yaml_document)
		config_file_routine(yaml_document);
#endif

	uint64_t count = 1;
	do {
		int set_no;
		for (set_no = 0; set_no < test_set_count; set_no++) {
			ldms_begin_transaction(test_sets[set_no]);
			ldms_set_u64(test_metrics[set_no], count);
			ldms_end_transaction(test_sets[set_no]);
			if (notify) {
				struct ldms_notify_event_s event;
				ldms_init_notify_modified(&event);
				ldms_notify(test_sets[set_no], &event);
			}
		}
		count++;
		usleep(sample_interval);
	} while (1);

	cleanup(0);
	return 0;
}