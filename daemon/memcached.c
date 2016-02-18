/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *  memcached - memory caching daemon
 *
 *       http://www.danga.com/memcached/
 *
 *  Copyright 2003 Danga Interactive, Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Anatoly Vorobey <mellon@pobox.com>
 *      Brad Fitzpatrick <brad@danga.com>
 */
#include "config.h"
#include "memcached.h"
#include "memcached/extension_loggers.h"
#include "alloc_hooks.h"
#include "utilities/engine_loader.h"
#include "utilities/protocol2text.h"
#include "timings.h"
#include "cmdline.h"
#include "mc_time.h"

#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <snappy-c.h>
#include <JSON_checker.h>

// MB-14649: log crashing on windows..
#include <math.h>

static bool grow_dynamic_buffer(conn *c, size_t needed);
static void cookie_set_admin(const void *cookie);
static bool cookie_is_admin(const void *cookie);

typedef union {
    item_info info;
    char bytes[sizeof(item_info) + ((IOV_MAX - 1) * sizeof(struct iovec))];
} item_info_holder;

static void item_set_cas(const void *cookie, item *it, uint64_t cas) {
    settings.engine.v1->item_set_cas(settings.engine.v0, cookie, it, cas);
}

#define MAX_SASL_MECH_LEN 32

/* The item must always be called "it" */
#define SLAB_GUTS(conn, thread_stats, slab_op, thread_op) \
    thread_stats->slab_stats[info.info.clsid].slab_op++;

#define THREAD_GUTS(conn, thread_stats, slab_op, thread_op) \
    thread_stats->thread_op++;

#define THREAD_GUTS2(conn, thread_stats, slab_op, thread_op) \
    thread_stats->slab_op++; \
    thread_stats->thread_op++;

#define SLAB_THREAD_GUTS(conn, thread_stats, slab_op, thread_op) \
    SLAB_GUTS(conn, thread_stats, slab_op, thread_op) \
    THREAD_GUTS(conn, thread_stats, slab_op, thread_op)

#define STATS_INCR1(GUTS, conn, slab_op, thread_op, key, nkey) { \
    struct thread_stats *thread_stats = get_thread_stats(conn); \
    cb_mutex_enter(&thread_stats->mutex); \
    GUTS(conn, thread_stats, slab_op, thread_op); \
    cb_mutex_exit(&thread_stats->mutex); \
}

#define STATS_INCR(conn, op, key, nkey) \
    STATS_INCR1(THREAD_GUTS, conn, op, op, key, nkey)

#define SLAB_INCR(conn, op, key, nkey) \
    STATS_INCR1(SLAB_GUTS, conn, op, op, key, nkey)

#define STATS_TWO(conn, slab_op, thread_op, key, nkey) \
    STATS_INCR1(THREAD_GUTS2, conn, slab_op, thread_op, key, nkey)

#define SLAB_TWO(conn, slab_op, thread_op, key, nkey) \
    STATS_INCR1(SLAB_THREAD_GUTS, conn, slab_op, thread_op, key, nkey)

#define STATS_HIT(conn, op, key, nkey) \
    SLAB_TWO(conn, op##_hits, cmd_##op, key, nkey)

#define STATS_MISS(conn, op, key, nkey) \
    STATS_TWO(conn, op##_misses, cmd_##op, key, nkey)

#define STATS_NOKEY(conn, op) { \
    struct thread_stats *thread_stats = \
        get_thread_stats(conn); \
    cb_mutex_enter(&thread_stats->mutex); \
    thread_stats->op++; \
    cb_mutex_exit(&thread_stats->mutex); \
}

#define STATS_NOKEY2(conn, op1, op2) { \
    struct thread_stats *thread_stats = \
        get_thread_stats(conn); \
    cb_mutex_enter(&thread_stats->mutex); \
    thread_stats->op1++; \
    thread_stats->op2++; \
    cb_mutex_exit(&thread_stats->mutex); \
}

#define STATS_ADD(conn, op, amt) { \
    struct thread_stats *thread_stats = \
        get_thread_stats(conn); \
    cb_mutex_enter(&thread_stats->mutex); \
    thread_stats->op += amt; \
    cb_mutex_exit(&thread_stats->mutex); \
}

volatile sig_atomic_t memcached_shutdown;

/* Lock for global stats */
static cb_mutex_t stats_lock;

/**
 * Structure to save ns_server's session cas token.
 */
static struct session_cas {
    uint64_t value;
    uint64_t ctr;
    cb_mutex_t mutex;
} session_cas;

void STATS_LOCK() {
    cb_mutex_enter(&stats_lock);
}

void STATS_UNLOCK() {
    cb_mutex_exit(&stats_lock);
}

#ifdef WIN32
static int is_blocking(DWORD dw) {
    return (dw == WSAEWOULDBLOCK);
}
static int is_emfile(DWORD dw) {
    return (dw == WSAEMFILE);
}
static int is_closed_conn(DWORD dw) {
    return (dw == WSAENOTCONN || WSAECONNRESET);
}
static int is_addrinuse(DWORD dw) {
    return (dw == WSAEADDRINUSE);
}
static void set_ewouldblock(void) {
    WSASetLastError(WSAEWOULDBLOCK);
}
static void set_econnreset(void) {
    WSASetLastError(WSAECONNRESET);
}
#else
static int is_blocking(int dw) {
    return (dw == EAGAIN || dw == EWOULDBLOCK);
}
static int is_emfile(int dw) {
    return (dw == EMFILE);
}
static int is_closed_conn(int dw) {
    return  (dw == ENOTCONN || dw != ECONNRESET);
}
static int is_addrinuse(int dw) {
    return (dw == EADDRINUSE);
}
static void set_ewouldblock(void) {
    errno = EWOULDBLOCK;
}
static void set_econnreset(void) {
    errno = ECONNRESET;
}
#endif

/*
 * forward declarations
 */
static SOCKET new_socket(struct addrinfo *ai);
static int try_read_command(conn *c);
static struct thread_stats* get_independent_stats(conn *c);
static struct thread_stats* get_thread_stats(conn *c);
static void register_callback(ENGINE_HANDLE *eh,
                              ENGINE_EVENT_TYPE type,
                              EVENT_CALLBACK cb, const void *cb_data);
static SERVER_HANDLE_V1 *get_server_api(void);


enum try_read_result {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,            /** an error occured (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR      /** failed to allocate more memory */
};

static enum try_read_result try_read_network(conn *c);

/* stats */
static void stats_init(void);
static void server_stats(ADD_STAT add_stats, conn *c, bool aggregate);
static void process_stat_settings(ADD_STAT add_stats, void *c);


/* defaults */
static void settings_init(void);

/* event handling, network IO */
static void event_handler(evutil_socket_t fd, short which, void *arg);
static void complete_nread(conn *c);
static void write_and_free(conn *c, char *buf, size_t bytes);
static int ensure_iov_space(conn *c);
static int add_iov(conn *c, const void *buf, size_t len);
static int add_msghdr(conn *c);

/** exported globals **/
struct stats stats;
struct settings settings;

/** file scope variables **/
static conn *listen_conn = NULL;
static struct event_base *main_base;
static struct thread_stats *default_independent_stats;

static struct engine_event_handler *engine_event_handlers[MAX_ENGINE_EVENT_TYPE + 1];

enum transmit_result {
    TRANSMIT_COMPLETE,   /** All done writing. */
    TRANSMIT_INCOMPLETE, /** More data remaining to write. */
    TRANSMIT_SOFT_ERROR, /** Can't write any more right now. */
    TRANSMIT_HARD_ERROR  /** Can't write (c->state is set to conn_closing) */
};

static enum transmit_result transmit(conn *c);

static void report_op(conn *c) {
    hrtime_t msec = collect_timing(c->cmd, gethrtime() - c->start);
    if ((msec / 500) > 1) {
        const char *opcode = memcached_opcode_2_text(c->cmd);
        char opcodetext[10];
        if (opcode == NULL) {
            snprintf(opcodetext, sizeof(opcodetext), "0x%0X", c->cmd);
            opcode = opcodetext;
        }
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                        "%u: Slow %s operation on connection: %lu ms",
                                        (unsigned int)c->sfd,
                                        opcode,
                                        (unsigned long)msec);
    }
}

/* Perform all callbacks of a given type for the given connection. */
void perform_callbacks(ENGINE_EVENT_TYPE type,
                       const void *data,
                       const void *c) {
    struct engine_event_handler *h;
    for (h = engine_event_handlers[type]; h; h = h->next) {
        h->cb(c, type, data, h->cb_data);
    }
}

/**
 * Return the TCP or domain socket listening_port structure that
 * has a given port number
 */
static struct listening_port *get_listening_port_instance(const int port) {
    struct listening_port *port_ins = NULL;
    int i;
    for (i = 0; i < settings.num_interfaces; ++i) {
        if (stats.listening_ports[i].port == port) {
            port_ins = &stats.listening_ports[i];
        }
    }
    return port_ins;
}

static void stats_init(void) {
    stats.daemon_conns = 0;
    stats.rejected_conns = 0;
    stats.curr_conns = stats.total_conns = 0;
    stats.listening_ports = calloc(settings.num_interfaces, sizeof(struct listening_port));

    stats_prefix_init();
}

static void stats_reset(const void *cookie) {
    struct conn *conn = (struct conn*)cookie;
    STATS_LOCK();
    stats.rejected_conns = 0;
    stats.total_conns = 0;
    stats_prefix_clear();
    STATS_UNLOCK();
    threadlocal_stats_reset(get_independent_stats(conn));
    settings.engine.v1->reset_stats(settings.engine.v0, cookie);
}

static int get_number_of_worker_threads(void) {
    int ret;
    char *override = getenv("MEMCACHED_NUM_CPUS");
    if (override == NULL) {
#ifdef WIN32
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        ret = (int)sysinfo.dwNumberOfProcessors;
#else
        ret = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
        if (ret > 4) {
            ret = (int)(ret * 0.75f);
        }
        if (ret < 4) {
            ret = 4;
        }
    } else {
        ret = atoi(override);
        if (ret == 0) {
            ret = 4;
        }
    }

    return ret;
}

static void settings_init(void) {
    static struct interface default_interface;
    default_interface.port = 11211;
    default_interface.maxconn = 1000;
    default_interface.backlog = 1024;

    settings.num_interfaces = 1;
    settings.interfaces = &default_interface;
    settings.daemonize = false;
    settings.pid_file = NULL;
    settings.bio_drain_buffer_sz = 8192;

    settings.verbose = 0;
    settings.num_threads = get_number_of_worker_threads();
    settings.prefix_delimiter = ':';
    settings.detail_enabled = 0;
    settings.allow_detailed = true;
    settings.reqs_per_event_high_priority = 50;
    settings.reqs_per_event_med_priority = 5;
    settings.reqs_per_event_low_priority = 1;
    settings.default_reqs_per_event = 20;
    settings.require_sasl = false;
    settings.extensions.logger = get_stderr_logger();
    settings.tcp_nodelay = getenv("MEMCACHED_DISABLE_TCP_NODELAY") == NULL;
    settings.engine_module = "default_engine.so";
    settings.engine_config = NULL;
    settings.config = NULL;
    settings.admin = NULL;
    settings.disable_admin = false;
    settings.datatype = false;
}

/*
 * Adds a message header to a connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int add_msghdr(conn *c)
{
    struct msghdr *msg;

    cb_assert(c != NULL);

    if (c->msgsize == c->msgused) {
        cb_assert(c->msgsize > 0);
        msg = realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
        if (! msg)
            return -1;
        c->msglist = msg;
        c->msgsize *= 2;
    }

    msg = c->msglist + c->msgused;

    /* this wipes msg_iovlen, msg_control, msg_controllen, and
       msg_flags, the last 3 of which aren't defined on solaris: */
    memset(msg, 0, sizeof(struct msghdr));

    msg->msg_iov = &c->iov[c->iovused];

    if (c->request_addr_size > 0) {
        msg->msg_name = &c->request_addr;
        msg->msg_namelen = c->request_addr_size;
    }

    c->msgbytes = 0;
    c->msgused++;

    return 0;
}

struct {
    cb_mutex_t mutex;
    bool disabled;
    ssize_t count;
    uint64_t num_disable;
} listen_state;

static bool is_listen_disabled(void) {
    bool ret;
    cb_mutex_enter(&listen_state.mutex);
    ret = listen_state.disabled;
    cb_mutex_exit(&listen_state.mutex);
    return ret;
}

static uint64_t get_listen_disabled_num(void) {
    uint64_t ret;
    cb_mutex_enter(&listen_state.mutex);
    ret = listen_state.num_disable;
    cb_mutex_exit(&listen_state.mutex);
    return ret;
}

static void disable_listen(void) {
    conn *next;
    cb_mutex_enter(&listen_state.mutex);
    listen_state.disabled = true;
    listen_state.count = 10;
    ++listen_state.num_disable;
    cb_mutex_exit(&listen_state.mutex);

    for (next = listen_conn; next; next = next->next) {
        update_event(next, 0);
        if (listen(next->sfd, 1) != 0) {
            log_socket_error(EXTENSION_LOG_WARNING, NULL,
                             "listen() failed: %s");
        }
    }
}

void safe_close(SOCKET sfd) {
    if (sfd != INVALID_SOCKET) {
        int rval;
        while ((rval = closesocket(sfd)) == SOCKET_ERROR &&
               (errno == EINTR || errno == EAGAIN)) {
            /* go ahead and retry */
        }

        if (rval == SOCKET_ERROR) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Failed to close socket %d (%%s)!!", (int)sfd);
            log_socket_error(EXTENSION_LOG_WARNING, NULL,
                             msg);
        } else {
            STATS_LOCK();
            stats.curr_conns--;
            STATS_UNLOCK();

            if (is_listen_disabled()) {
                notify_dispatcher();
            }
        }
    }
}

/**
 * Reset all of the dynamic buffers used by a connection back to their
 * default sizes. The strategy for resizing the buffers is to allocate a
 * new one of the correct size and free the old one if the allocation succeeds
 * instead of using realloc to change the buffer size (because realloc may
 * not shrink the buffers, and will also copy the memory). If the allocation
 * fails the buffer will be unchanged.
 *
 * @param c the connection to resize the buffers for
 * @return true if all allocations succeeded, false if one or more of the
 *         allocations failed.
 */
static bool conn_reset_buffersize(conn *c) {
    bool ret = true;

    if (c->rsize != DATA_BUFFER_SIZE) {
        void *ptr = malloc(DATA_BUFFER_SIZE);
        if (ptr != NULL) {
            free(c->rbuf);
            c->rbuf = ptr;
            c->rsize = DATA_BUFFER_SIZE;
        } else {
            ret = false;
        }
    }

    if (c->wsize != DATA_BUFFER_SIZE) {
        void *ptr = malloc(DATA_BUFFER_SIZE);
        if (ptr != NULL) {
            free(c->wbuf);
            c->wbuf = ptr;
            c->wsize = DATA_BUFFER_SIZE;
        } else {
            ret = false;
        }
    }

    if (c->isize != ITEM_LIST_INITIAL) {
        void *ptr = malloc(sizeof(item *) * ITEM_LIST_INITIAL);
        if (ptr != NULL) {
            free(c->ilist);
            c->ilist = ptr;
            c->isize = ITEM_LIST_INITIAL;
        } else {
            ret = false;
        }
    }

    if (c->temp_alloc_size != TEMP_ALLOC_LIST_INITIAL) {
        void *ptr = malloc(sizeof(char *) * TEMP_ALLOC_LIST_INITIAL);
        if (ptr != NULL) {
            free(c->temp_alloc_list);
            c->temp_alloc_list = ptr;
            c->temp_alloc_size = TEMP_ALLOC_LIST_INITIAL;
        } else {
            ret = false;
        }
    }

    if (c->iovsize != IOV_LIST_INITIAL) {
        void *ptr = malloc(sizeof(struct iovec) * IOV_LIST_INITIAL);
        if (ptr != NULL) {
            free(c->iov);
            c->iov = ptr;
            c->iovsize = IOV_LIST_INITIAL;
        } else {
            ret = false;
        }
    }

    if (c->msgsize != MSG_LIST_INITIAL) {
        void *ptr = malloc(sizeof(struct msghdr) * MSG_LIST_INITIAL);
        if (ptr != NULL) {
            free(c->msglist);
            c->msglist = ptr;
            c->msgsize = MSG_LIST_INITIAL;
        } else {
            ret = false;
        }
    }

    return ret;
}

/**
 * Constructor for all memory allocations of connection objects. Initialize
 * all members and allocate the transfer buffers.
 *
 * @param buffer The memory allocated by the object cache
 * @return 0 on success, 1 if we failed to allocate memory
 */
static int conn_constructor(conn *c) {
    memset(c, 0, sizeof(*c));
    MEMCACHED_CONN_CREATE(c);

    c->state = conn_immediate_close;
    c->sfd = INVALID_SOCKET;
    if (!conn_reset_buffersize(c)) {
        free(c->rbuf);
        free(c->wbuf);
        free(c->ilist);
        free(c->temp_alloc_list);
        free(c->iov);
        free(c->msglist);
        settings.extensions.logger->log(EXTENSION_LOG_WARNING,
                                        NULL,
                                        "Failed to allocate buffers for connection\n");
        return 1;
    }

    STATS_LOCK();
    stats.conn_structs++;
    STATS_UNLOCK();

    return 0;
}

/**
 * Destructor for all connection objects. Release all allocated resources.
 *
 * @param buffer The memory allocated by the objec cache
 */
static void conn_destructor(conn *c) {
    free(c->rbuf);
    free(c->wbuf);
    free(c->ilist);
    free(c->temp_alloc_list);
    free(c->iov);
    free(c->msglist);

    STATS_LOCK();
    stats.conn_structs--;
    STATS_UNLOCK();
}

/*
 * Free list management for connections.
 */
struct connections {
    conn* free;
    conn** all;
    cb_mutex_t mutex;
    int next;
} connections;

static void initialize_connections(void)
{
    int preallocate;

    cb_mutex_initialize(&connections.mutex);
    connections.all = calloc(settings.maxconns, sizeof(conn*));
    if (connections.all == NULL) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                        "Failed to allocate memory for connections");
        exit(EX_OSERR);
    }

    preallocate = settings.maxconns / 2;
    if (preallocate < 1000) {
        preallocate = settings.maxconns;
    } else if (preallocate > 5000) {
        preallocate = 5000;
    }

    for (connections.next = 0; connections.next < preallocate; ++connections.next) {
        connections.all[connections.next] = malloc(sizeof(conn));
        if (conn_constructor(connections.all[connections.next]) != 0) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                            "Failed to allocate memory for connections");
            exit(EX_OSERR);
        }
        connections.all[connections.next]->next = connections.free;
        connections.free = connections.all[connections.next];
    }
}

static void destroy_connections(void)
{
    int ii;
    for (ii = 0; ii < settings.maxconns; ++ii) {
        if (connections.all[ii]) {
            conn *c = connections.all[ii];
            conn_destructor(c);
            free(c);
        }
    }

    free(connections.all);
}

static conn *allocate_connection(void) {
    conn *ret;

    cb_mutex_enter(&connections.mutex);
    ret = connections.free;
    if (ret != NULL) {
        connections.free = connections.free->next;
        ret->next = NULL;
    }
    cb_mutex_exit(&connections.mutex);

    if (ret == NULL) {
        ret = malloc(sizeof(conn));
        if (ret == NULL) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                            "Failed to allocate memory for connection");
            return NULL;
        }

        if (conn_constructor(ret) != 0) {
            free(ret);
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                            "Failed to allocate memory for connection");
            return NULL;
        }

        cb_mutex_enter(&connections.mutex);
        if (connections.next == settings.maxconns) {
            free(ret);
            ret = NULL;
        } else {
            connections.all[connections.next++] = ret;
        }
        cb_mutex_exit(&connections.mutex);
    }

    return ret;
}

static void release_connection(conn *c) {
    c->sfd = INVALID_SOCKET;
    cb_mutex_enter(&connections.mutex);
    c->next = connections.free;
    connections.free = c;
    cb_mutex_exit(&connections.mutex);
}

static const char *substate_text(enum bin_substates state) {
    switch (state) {
    case bin_no_state: return "bin_no_state";
    case bin_reading_set_header: return "bin_reading_set_header";
    case bin_reading_cas_header: return "bin_reading_cas_header";
    case bin_read_set_value: return "bin_read_set_value";
    case bin_reading_sasl_auth: return "bin_reading_sasl_auth";
    case bin_reading_sasl_auth_data: return "bin_reading_sasl_auth_data";
    case bin_reading_packet: return "bin_reading_packet";
    default:
        return "illegal";
    }
}

static void add_connection_stats(ADD_STAT add_stats, conn *d, conn *c) {
    append_stat("conn", add_stats, d, "%p", c);
    if (c->sfd == INVALID_SOCKET) {
        append_stat("socket", add_stats, d, "disconnected");
    } else {
        append_stat("socket", add_stats, d, "%lu", (long)c->sfd);
        append_stat("protocol", add_stats, d, "%s", "binary");
        append_stat("transport", add_stats, d, "TCP");
        append_stat("nevents", add_stats, d, "%u", c->nevents);
        if (c->sasl_conn != NULL) {
            append_stat("sasl_conn", add_stats, d, "%p", c->sasl_conn);
        }
        append_stat("state", add_stats, d, "%s", state_text(c->state));
        append_stat("substate", add_stats, d, "%s", substate_text(c->substate));
        append_stat("registered_in_libevent", add_stats, d, "%d",
                    (int)c->registered_in_libevent);
        append_stat("ev_flags", add_stats, d, "%x", c->ev_flags);
        append_stat("which", add_stats, d, "%x", c->which);
        append_stat("rbuf", add_stats, d, "%p", c->rbuf);
        append_stat("rcurr", add_stats, d, "%p", c->rcurr);
        append_stat("rsize", add_stats, d, "%u", c->rsize);
        append_stat("rbytes", add_stats, d, "%u", c->rbytes);
        append_stat("wbuf", add_stats, d, "%p", c->wbuf);
        append_stat("wcurr", add_stats, d, "%p", c->wcurr);
        append_stat("wsize", add_stats, d, "%u", c->wsize);
        append_stat("wbytes", add_stats, d, "%u", c->wbytes);
        append_stat("write_and_go", add_stats, d, "%p", c->write_and_go);
        append_stat("write_and_free", add_stats, d, "%p", c->write_and_free);
        append_stat("ritem", add_stats, d, "%p", c->ritem);
        append_stat("rlbytes", add_stats, d, "%u", c->rlbytes);
        append_stat("item", add_stats, d, "%p", c->item);
        append_stat("store_op", add_stats, d, "%u", c->store_op);
        append_stat("sbytes", add_stats, d, "%u", c->sbytes);
        append_stat("iov", add_stats, d, "%p", c->iov);
        append_stat("iovsize", add_stats, d, "%u", c->iovsize);
        append_stat("iovused", add_stats, d, "%u", c->iovused);
        append_stat("msglist", add_stats, d, "%p", c->msglist);
        append_stat("msgsize", add_stats, d, "%u", c->msgsize);
        append_stat("msgused", add_stats, d, "%u", c->msgused);
        append_stat("msgcurr", add_stats, d, "%u", c->msgcurr);
        append_stat("msgbytes", add_stats, d, "%u", c->msgbytes);
        append_stat("ilist", add_stats, d, "%p", c->ilist);
        append_stat("isize", add_stats, d, "%u", c->isize);
        append_stat("icurr", add_stats, d, "%p", c->icurr);
        append_stat("ileft", add_stats, d, "%u", c->ileft);
        append_stat("temp_alloc_list", add_stats, d, "%p", c->temp_alloc_list);
        append_stat("temp_alloc_size", add_stats, d, "%u", c->temp_alloc_size);
        append_stat("temp_alloc_curr", add_stats, d, "%p", c->temp_alloc_curr);
        append_stat("temp_alloc_left", add_stats, d, "%u", c->temp_alloc_left);

        append_stat("noreply", add_stats, d, "%d", c->noreply);
        append_stat("refcount", add_stats, d, "%u", (int)c->refcount);
        append_stat("dynamic_buffer.buffer", add_stats, d, "%p",
                    c->dynamic_buffer.buffer);
        append_stat("dynamic_buffer.size", add_stats, d, "%zu",
                    c->dynamic_buffer.size);
        append_stat("dynamic_buffer.offset", add_stats, d, "%zu",
                    c->dynamic_buffer.offset);
        append_stat("engine_storage", add_stats, d, "%p", c->engine_storage);
        /* @todo we should decode the binary header */
        append_stat("cas", add_stats, d, "%"PRIu64, c->cas);
        append_stat("cmd", add_stats, d, "%u", c->cmd);
        append_stat("opaque", add_stats, d, "%u", c->opaque);
        append_stat("keylen", add_stats, d, "%u", c->keylen);
        append_stat("list_state", add_stats, d, "%u", c->list_state);
        append_stat("next", add_stats, d, "%p", c->next);
        append_stat("thread", add_stats, d, "%p", c->thread);
        append_stat("aiostat", add_stats, d, "%u", c->aiostat);
        append_stat("ewouldblock", add_stats, d, "%u", c->ewouldblock);
        append_stat("tap_iterator", add_stats, d, "%p", c->tap_iterator);
    }
}

/**
 * Do a full stats of all of the connections.
 * Do _NOT_ try to follow _ANY_ of the pointers in the conn structure
 * because we read all of the values _DIRTY_. We preallocated the array
 * of all of the connection pointers during startup, so we _KNOW_ that
 * we can iterate through all of them. All of the conn structs will
 * only appear in the connections.all array when we've allocated them,
 * and we don't release them so it's safe to look at them.
 */
static void connection_stats(ADD_STAT add_stats, conn *c) {
    int ii;
    for (ii = 0; ii < settings.maxconns && connections.all[ii]; ++ii) {
        add_connection_stats(add_stats, c, connections.all[ii]);
    }
}

conn *conn_new(const SOCKET sfd, in_port_t parent_port,
               STATE_FUNC init_state, int event_flags,
               unsigned int read_buffer_size, struct event_base *base,
               struct timeval *timeout) {
    conn *c = allocate_connection();
    if (c == NULL) {
        return NULL;
    }

    c->admin = false;
    cb_assert(c->thread == NULL);
    c->max_reqs_per_event = settings.default_reqs_per_event;

    if (c->rsize < read_buffer_size) {
        void *mem = malloc(read_buffer_size);
        if (mem) {
            c->rsize = read_buffer_size;
            free(c->rbuf);
            c->rbuf = mem;
        } else {
            cb_assert(c->thread == NULL);
            release_connection(c);
            return NULL;
        }
    }

    memset(&c->ssl, 0, sizeof(c->ssl));
    if (init_state != conn_listening) {
        int ii;
        for (ii = 0; ii < settings.num_interfaces; ++ii) {
            if (parent_port == settings.interfaces[ii].port) {
                if (settings.interfaces[ii].ssl.cert != NULL) {
                    const char *cert = settings.interfaces[ii].ssl.cert;
                    const char *pkey = settings.interfaces[ii].ssl.key;

                    c->ssl.ctx = SSL_CTX_new(SSLv23_server_method());
                    /* MB-12359 - Disable SSLv2 & SSLv3 due to POODLE */
                    SSL_CTX_set_options(c->ssl.ctx,
                                        SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

                    /* @todo don't read files, but use in-memory-copies */
                    if (!SSL_CTX_use_certificate_chain_file(c->ssl.ctx, cert) ||
                        !SSL_CTX_use_PrivateKey_file(c->ssl.ctx, pkey, SSL_FILETYPE_PEM)) {
                        release_connection(c);
                        return NULL;
                    }

                    set_ssl_ctx_cipher_list(c->ssl.ctx,
                                            settings.extensions.logger);

                    c->ssl.enabled = true;
                    c->ssl.error = false;
                    c->ssl.client = NULL;

                    c->ssl.in.buffer = malloc(settings.bio_drain_buffer_sz);
                    c->ssl.out.buffer = malloc(settings.bio_drain_buffer_sz);

                    if (c->ssl.in.buffer == NULL || c->ssl.out.buffer == NULL) {
                        release_connection(c);
                        return NULL;
                    }

                    c->ssl.in.buffsz = settings.bio_drain_buffer_sz;
                    c->ssl.out.buffsz = settings.bio_drain_buffer_sz;
                    BIO_new_bio_pair(&c->ssl.application,
                                     settings.bio_drain_buffer_sz,
                                     &c->ssl.network,
                                     settings.bio_drain_buffer_sz);

                    c->ssl.client = SSL_new(c->ssl.ctx);
                    SSL_set_bio(c->ssl.client,
                                c->ssl.application,
                                c->ssl.application);
                }
            }
        }
    }

    c->request_addr_size = 0;

    if (settings.verbose > 1) {
        if (init_state == conn_listening) {
            settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                            "<%d server listening", sfd);
        } else {
            settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                            "<%d new client connection", sfd);
        }
    }

    c->sfd = sfd;
    c->parent_port = parent_port;
    c->state = init_state;
    c->rlbytes = 0;
    c->cmd = -1;
    c->rbytes = c->wbytes = 0;
    c->wcurr = c->wbuf;
    c->rcurr = c->rbuf;
    c->ritem = 0;
    c->icurr = c->ilist;
    c->temp_alloc_curr = c->temp_alloc_list;
    c->ileft = 0;
    c->temp_alloc_left = 0;
    c->iovused = 0;
    c->msgcurr = 0;
    c->msgused = 0;
    c->next = NULL;
    c->list_state = 0;

    c->write_and_go = init_state;
    c->write_and_free = 0;
    c->item = 0;
    c->supports_datatype = false;
    c->noreply = false;

    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = event_flags;

    if (!register_event(c, timeout)) {
        cb_assert(c->thread == NULL);
        release_connection(c);
        return NULL;
    }

    STATS_LOCK();
    stats.total_conns++;
    STATS_UNLOCK();

    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;
    c->refcount = 1;

    MEMCACHED_CONN_ALLOCATE(c->sfd);
    c->clustermap_revno = -2;

    perform_callbacks(ON_CONNECT, NULL, c);

    return c;
}

static void conn_cleanup_engine_allocations(conn* c) {
   if (c->item) {
        settings.engine.v1->release(settings.engine.v0, c, c->item);
        c->item = 0;
    }

    if (c->ileft != 0) {
        for (; c->ileft > 0; c->ileft--,c->icurr++) {
            settings.engine.v1->release(settings.engine.v0, c, *(c->icurr));
        }
    }
}

static void conn_cleanup(conn *c) {
    assert(c != NULL);
    c->admin = false;

    if (c->temp_alloc_left != 0) {
        for (; c->temp_alloc_left > 0; c->temp_alloc_left--, c->temp_alloc_curr++) {
            free(*(c->temp_alloc_curr));
        }
    }

    if (c->write_and_free) {
        free(c->write_and_free);
        c->write_and_free = 0;
    }

    if (c->sasl_conn) {
        cbsasl_dispose(&c->sasl_conn);
        c->sasl_conn = NULL;
    }

    c->engine_storage = NULL;
    c->tap_iterator = NULL;
    c->thread = NULL;
    cb_assert(c->next == NULL);
    c->sfd = INVALID_SOCKET;
    c->dcp = 0;
    c->start = 0;
    if (c->ssl.enabled) {
        BIO_free_all(c->ssl.network);
        SSL_free(c->ssl.client);
        c->ssl.enabled = false;
        c->ssl.error = false;
        free(c->ssl.in.buffer);
        free(c->ssl.out.buffer);
        memset(&c->ssl, 0, sizeof(c->ssl));
    }
    c->clustermap_revno = -2;
}

void conn_close(conn *c) {
    cb_assert(c != NULL);
    cb_assert(c->sfd == INVALID_SOCKET);
    cb_assert(c->state == conn_immediate_close);

    cb_assert(c->thread);
    /* remove from pending-io list */
    if (settings.verbose > 1 && list_contains(c->thread->pending_io, c)) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "Current connection was in the pending-io list.. Nuking it\n");
    }
    c->thread->pending_io = list_remove(c->thread->pending_io, c);

    conn_cleanup(c);

    /*
     * The contract with the object cache is that we should return the
     * object in a constructed state. Reset the buffers to the default
     * size
     */
    conn_reset_buffersize(c);
    cb_assert(c->thread == NULL);
    release_connection(c);
}

/*
 * Shrinks a connection's buffers if they're too big.  This prevents
 * periodic large "get" requests from permanently chewing lots of server
 * memory.
 *
 * This should only be called in between requests since it can wipe output
 * buffers!
 */
static void conn_shrink(conn *c) {
    cb_assert(c != NULL);

    if (c->rsize > READ_BUFFER_HIGHWAT && c->rbytes < DATA_BUFFER_SIZE) {
        void *newbuf;

        if (c->rcurr != c->rbuf) {
            /* Pack the buffer */
            memmove(c->rbuf, c->rcurr, (size_t)c->rbytes);
        }

        newbuf = realloc(c->rbuf, DATA_BUFFER_SIZE);

        if (newbuf) {
            c->rbuf = newbuf;
            c->rsize = DATA_BUFFER_SIZE;
        }
        c->rcurr = c->rbuf;
    }

    /* isize is no longer dynamic */
    cb_assert(c->isize == ITEM_LIST_INITIAL);

    if (c->msgsize > MSG_LIST_HIGHWAT) {
        void *newbuf = realloc(c->msglist,
                               MSG_LIST_INITIAL * sizeof(c->msglist[0]));
        if (newbuf) {
            c->msglist = newbuf;
            c->msgsize = MSG_LIST_INITIAL;
        }
    }

    if (c->iovsize > IOV_LIST_HIGHWAT) {
        void *newbuf = realloc(c->iov, IOV_LIST_INITIAL * sizeof(c->iov[0]));
        if (newbuf) {
            c->iov = newbuf;
            c->iovsize = IOV_LIST_INITIAL;
        }
    }
}

/**
 * Convert a state name to a human readable form.
 */
const char *state_text(STATE_FUNC state) {
    if (state == conn_listening) {
        return "conn_listening";
    } else if (state == conn_new_cmd) {
        return "conn_new_cmd";
    } else if (state == conn_waiting) {
        return "conn_waiting";
    } else if (state == conn_read) {
        return "conn_read";
    } else if (state == conn_parse_cmd) {
        return "conn_parse_cmd";
    } else if (state == conn_write) {
        return "conn_write";
    } else if (state == conn_nread) {
        return "conn_nread";
    } else if (state == conn_swallow) {
        return "conn_swallow";
    } else if (state == conn_closing) {
        return "conn_closing";
    } else if (state == conn_mwrite) {
        return "conn_mwrite";
    } else if (state == conn_ship_log) {
        return "conn_ship_log";
    } else if (state == conn_setup_tap_stream) {
        return "conn_setup_tap_stream";
    } else if (state == conn_pending_close) {
        return "conn_pending_close";
    } else if (state == conn_immediate_close) {
        return "conn_immediate_close";
    } else if (state == conn_refresh_cbsasl) {
        return "conn_refresh_cbsasl";
    } else if (state == conn_refresh_ssl_certs) {
        return "conn_refresh_ssl_cert";
    } else {
        return "Unknown";
    }
}

/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
void conn_set_state(conn *c, STATE_FUNC state) {
    cb_assert(c != NULL);

    if (state != c->state) {
        /*
         * The connections in the "tap thread" behaves differently than
         * normal connections because they operate in a full duplex mode.
         * New messages may appear from both sides, so we can't block on
         * read from the nework / engine
         */
        if (c->tap_iterator != NULL || c->dcp) {
            if (state == conn_waiting) {
                c->which = EV_WRITE;
                state = conn_ship_log;
            }
        }

        if (settings.verbose > 2 || c->state == conn_closing
            || c->state == conn_setup_tap_stream) {
            settings.extensions.logger->log(EXTENSION_LOG_DETAIL, c,
                                            "%d: going from %s to %s\n",
                                            c->sfd, state_text(c->state),
                                            state_text(state));
        }

        if (state == conn_write || state == conn_mwrite) {
            if (c->start != 0) {
                report_op(c);
                c->start = 0;
            }
            MEMCACHED_PROCESS_COMMAND_END(c->sfd, c->wbuf, c->wbytes);
        }

        c->state = state;
    }
}

/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn *c) {
    cb_assert(c != NULL);

    if (c->iovused >= c->iovsize) {
        int i, iovnum;
        struct iovec *new_iov = (struct iovec *)realloc(c->iov,
                                (c->iovsize * 2) * sizeof(struct iovec));
        if (! new_iov)
            return -1;
        c->iov = new_iov;
        c->iovsize *= 2;

        /* Point all the msghdr structures at the new list. */
        for (i = 0, iovnum = 0; i < c->msgused; i++) {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}


/*
 * Adds data to the list of pending data that will be written out to a
 * connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */

static int add_iov(conn *c, const void *buf, size_t len) {
    struct msghdr *m;
    size_t leftover;
    bool limit_to_mtu;

    cb_assert(c != NULL);

    if (len == 0) {
        return 0;
    }

    do {
        m = &c->msglist[c->msgused - 1];

        /*
         * Limit the first payloads of TCP replies, to
         * UDP_MAX_PAYLOAD_SIZE bytes.
         */
        limit_to_mtu = (1 == c->msgused);

        /* We may need to start a new msghdr if this one is full. */
        if (m->msg_iovlen == IOV_MAX ||
            (limit_to_mtu && c->msgbytes >= UDP_MAX_PAYLOAD_SIZE)) {
            add_msghdr(c);
        }

        if (ensure_iov_space(c) != 0)
            return -1;

        /* If the fragment is too big to fit in the datagram, split it up */
        if (limit_to_mtu && len + c->msgbytes > UDP_MAX_PAYLOAD_SIZE) {
            leftover = len + c->msgbytes - UDP_MAX_PAYLOAD_SIZE;
            len -= leftover;
        } else {
            leftover = 0;
        }

        m = &c->msglist[c->msgused - 1];
        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;

        c->msgbytes += (int)len;
        c->iovused++;
        m->msg_iovlen++;

        buf = ((char *)buf) + len;
        len = leftover;
    } while (leftover > 0);

    return 0;
}

/**
 * get a pointer to the start of the request struct for the current command
 */
static void* binary_get_request(conn *c) {
    char *ret = c->rcurr;
    ret -= (sizeof(c->binary_header) + c->binary_header.request.keylen +
            c->binary_header.request.extlen);

    cb_assert(ret >= c->rbuf);
    return ret;
}

/**
 * get a pointer to the key in this request
 */
static char* binary_get_key(conn *c) {
    return c->rcurr - (c->binary_header.request.keylen);
}

/**
 * Insert a key into a buffer, but replace all non-printable characters
 * with a '.'.
 *
 * @param dest where to store the output
 * @param destsz size of destination buffer
 * @param prefix string to insert before the data
 * @param client the client we are serving
 * @param from_client set to true if this data is from the client
 * @param key the key to add to the buffer
 * @param nkey the number of bytes in the key
 * @return number of bytes in dest if success, -1 otherwise
 */
static ssize_t key_to_printable_buffer(char *dest, size_t destsz,
                                       SOCKET client, bool from_client,
                                       const char *prefix,
                                       const char *key,
                                       size_t nkey)
{
    char *ptr;
    ssize_t ii;
    ssize_t nw = snprintf(dest, destsz, "%c%d %s ", from_client ? '>' : '<',
                          (int)client, prefix);
    if (nw == -1) {
        return -1;
    }

    ptr = dest + nw;
    destsz -= nw;
    if (nkey > destsz) {
        nkey = destsz;
    }

    for (ii = 0; ii < nkey; ++ii, ++key, ++ptr) {
        if (isgraph(*key)) {
            *ptr = *key;
        } else {
            *ptr = '.';
        }
    }

    *ptr = '\0';
    return (ssize_t)(ptr - dest);
}

/**
 * Convert a byte array to a text string
 *
 * @param dest where to store the output
 * @param destsz size of destination buffer
 * @param prefix string to insert before the data
 * @param client the client we are serving
 * @param from_client set to true if this data is from the client
 * @param data the data to add to the buffer
 * @param size the number of bytes in data to print
 * @return number of bytes in dest if success, -1 otherwise
 */
static ssize_t bytes_to_output_string(char *dest, size_t destsz,
                                      SOCKET client, bool from_client,
                                      const char *prefix,
                                      const char *data,
                                      size_t size)
{
    ssize_t nw = snprintf(dest, destsz, "%c%d %s", from_client ? '>' : '<',
                          (int)client, prefix);
    ssize_t offset = nw;
    ssize_t ii;

    if (nw == -1) {
        return -1;
    }

    for (ii = 0; ii < size; ++ii) {
        if (ii % 4 == 0) {
            if ((nw = snprintf(dest + offset, destsz - offset, "\n%c%d  ",
                               from_client ? '>' : '<', client)) == -1) {
                return  -1;
            }
            offset += nw;
        }
        if ((nw = snprintf(dest + offset, destsz - offset,
                           " 0x%02x", (unsigned char)data[ii])) == -1) {
            return -1;
        }
        offset += nw;
    }

    if ((nw = snprintf(dest + offset, destsz - offset, "\n")) == -1) {
        return -1;
    }

    return offset + nw;
}

static int add_bin_header(conn *c,
                          uint16_t err,
                          uint8_t hdr_len,
                          uint16_t key_len,
                          uint32_t body_len,
                          uint8_t datatype) {
    protocol_binary_response_header* header;

    cb_assert(c);

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
        return -1;
    }

    header = (protocol_binary_response_header *)c->wbuf;

    header->response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header->response.opcode = c->binary_header.request.opcode;
    header->response.keylen = (uint16_t)htons(key_len);

    header->response.extlen = (uint8_t)hdr_len;
    header->response.datatype = datatype;
    header->response.status = (uint16_t)htons(err);

    header->response.bodylen = htonl(body_len);
    header->response.opaque = c->opaque;
    header->response.cas = htonll(c->cas);

    if (settings.verbose > 1) {
        char buffer[1024];
        if (bytes_to_output_string(buffer, sizeof(buffer), c->sfd, false,
                                   "Writing bin response:",
                                   (const char*)header->bytes,
                                   sizeof(header->bytes)) != -1) {
            settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                            "%s", buffer);
        }
    }

    return add_iov(c, c->wbuf, sizeof(header->response));
}

/**
 * Convert an error code generated from the storage engine to the corresponding
 * error code used by the protocol layer.
 * @param e the error code as used in the engine
 * @return the error code as used by the protocol layer
 */
static protocol_binary_response_status engine_error_2_protocol_error(ENGINE_ERROR_CODE e) {
    protocol_binary_response_status ret;

    switch (e) {
    case ENGINE_SUCCESS:
        return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    case ENGINE_KEY_ENOENT:
        return PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
    case ENGINE_KEY_EEXISTS:
        return PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
    case ENGINE_ENOMEM:
        return PROTOCOL_BINARY_RESPONSE_ENOMEM;
    case ENGINE_TMPFAIL:
        return PROTOCOL_BINARY_RESPONSE_ETMPFAIL;
    case ENGINE_NOT_STORED:
        return PROTOCOL_BINARY_RESPONSE_NOT_STORED;
    case ENGINE_EINVAL:
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    case ENGINE_ENOTSUP:
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    case ENGINE_E2BIG:
        return PROTOCOL_BINARY_RESPONSE_E2BIG;
    case ENGINE_NOT_MY_VBUCKET:
        return PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET;
    case ENGINE_ERANGE:
        return PROTOCOL_BINARY_RESPONSE_ERANGE;
    case ENGINE_ROLLBACK:
        return PROTOCOL_BINARY_RESPONSE_ROLLBACK;
    default:
        ret = PROTOCOL_BINARY_RESPONSE_EINTERNAL;
    }

    return ret;
}

static int get_clustermap_revno(const char *map, size_t mapsize) {
    /* Try to locate the "rev": field in the map. Unfortunately
     * we can't use the function strnstr because it's not available
     * on all platforms
     */
    const char* prefix = "\"rev\":";
    size_t plen = strlen(prefix);
    size_t index;

    if (mapsize == 0 || *map != '{' || mapsize < (plen + 1)) {
        /* This doesn't look like our cluster map */
        return -1;
    }
    mapsize -= plen;

    for (index = 1; index < mapsize; ++index) {
        if (memcmp(map + index, prefix, plen) == 0) {
            index += plen;
            /* Found :-) */
            while (isspace(map[index])) {
                ++index;
            }

            if (!isdigit(map[index])) {
                return -1;
            }

            return atoi(map + index);
        }
    }

    /* not found */
    return -1;
}

static ENGINE_ERROR_CODE get_vb_map_cb(const void *cookie,
                                       const void *map,
                                       size_t mapsize)
{
    char *buf;
    conn *c = (conn*)cookie;
    protocol_binary_response_header header;
    int revno = get_clustermap_revno(map, mapsize);
    size_t needed = sizeof(protocol_binary_response_header);

    if (revno == c->clustermap_revno) {
        /* The client already have this map... */
        mapsize = 0;
    } else if (revno != -1) {
        c->clustermap_revno = revno;
    }

    needed += mapsize;

    if (!grow_dynamic_buffer(c, needed)) {
        if (settings.verbose > 0) {
            settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                    "<%d ERROR: Failed to allocate memory for response\n",
                    c->sfd);
        }
        return ENGINE_ENOMEM;
    }

    buf = c->dynamic_buffer.buffer + c->dynamic_buffer.offset;
    memset(&header, 0, sizeof(header));

    header.response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header.response.opcode = c->binary_header.request.opcode;
    header.response.status = (uint16_t)htons(PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET);
    header.response.bodylen = htonl((uint32_t)mapsize);
    header.response.opaque = c->opaque;

    memcpy(buf, header.bytes, sizeof(header.response));
    buf += sizeof(header.response);
    memcpy(buf, map, mapsize);
    c->dynamic_buffer.offset += needed;

    return ENGINE_SUCCESS;
}

static void write_bin_packet(conn *c, protocol_binary_response_status err, int swallow) {
    if (err == PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET) {
        ENGINE_ERROR_CODE ret;
        cb_assert(swallow == 0);

        ret = settings.engine.v1->get_engine_vb_map(settings.engine.v0, c,
                                                    get_vb_map_cb);
        if (ret == ENGINE_SUCCESS) {
            write_and_free(c, c->dynamic_buffer.buffer,
                           c->dynamic_buffer.offset);
            c->dynamic_buffer.buffer = NULL;
        } else {
            conn_set_state(c, conn_closing);
        }
    } else {
        ssize_t len = 0;
        const char *errtext = NULL;

        if (err != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            errtext = memcached_protocol_errcode_2_text(err);
            if (errtext != NULL) {
                len = (ssize_t)strlen(errtext);
            }
        }

        if (errtext && settings.verbose > 1) {
            settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                            ">%d Writing an error: %s\n", c->sfd,
                                            errtext);
        }

        add_bin_header(c, err, 0, 0, len, PROTOCOL_BINARY_RAW_BYTES);
        if (errtext) {
            add_iov(c, errtext, len);
        }
        conn_set_state(c, conn_mwrite);
        if (swallow > 0) {
            c->sbytes = swallow;
            c->write_and_go = conn_swallow;
        } else {
            c->write_and_go = conn_new_cmd;
        }
    }
}

/* Form and send a response to a command over the binary protocol */
static void write_bin_response(conn *c, const void *d, int hlen, int keylen, int dlen) {
    if (!c->noreply || c->cmd == PROTOCOL_BINARY_CMD_GET ||
        c->cmd == PROTOCOL_BINARY_CMD_GETK) {
        if (add_bin_header(c, 0, hlen, keylen, dlen, PROTOCOL_BINARY_RAW_BYTES) == -1) {
            conn_set_state(c, conn_closing);
            return;
        }
        add_iov(c, d, dlen);
        conn_set_state(c, conn_mwrite);
        c->write_and_go = conn_new_cmd;
    } else {
        if (c->start != 0) {
            report_op(c);
            c->start = 0;
        }
        conn_set_state(c, conn_new_cmd);
    }
}

static void complete_update_bin(conn *c) {
    protocol_binary_response_status eno = PROTOCOL_BINARY_RESPONSE_EINVAL;
    ENGINE_ERROR_CODE ret;
    item *it;
    item_info_holder info;

    cb_assert(c != NULL);
    it = c->item;
    memset(&info, 0, sizeof(info));
    info.info.nvalue = 1;
    if (!settings.engine.v1->get_item_info(settings.engine.v0, c, it,
                                           (void*)&info)) {
        settings.engine.v1->release(settings.engine.v0, c, it);
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "%d: Failed to get item info",
                                        c->sfd);
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINTERNAL, 0);
        return;
    }

    ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    if (ret == ENGINE_SUCCESS) {
        if (!c->supports_datatype) {
            if (checkUTF8JSON((void*)info.info.value[0].iov_base,
                              (int)info.info.value[0].iov_len)) {
                info.info.datatype = PROTOCOL_BINARY_DATATYPE_JSON;
                if (!settings.engine.v1->set_item_info(settings.engine.v0, c,
                                                       it, &info.info)) {
                    settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                            "%d: Failed to set item info",
                            c->sfd);
                }
            }
        }
        ret = settings.engine.v1->store(settings.engine.v0, c,
                                        it, &c->cas, c->store_op,
                                        c->binary_header.request.vbucket);
    }

#ifdef ENABLE_DTRACE
    switch (c->cmd) {
    case OPERATION_ADD:
        MEMCACHED_COMMAND_ADD(c->sfd, info.info.key, info.info.nkey,
                              (ret == ENGINE_SUCCESS) ? info.info.nbytes : -1, c->cas);
        break;
    case OPERATION_REPLACE:
        MEMCACHED_COMMAND_REPLACE(c->sfd, info.info.key, info.info.nkey,
                                  (ret == ENGINE_SUCCESS) ? info.info.nbytes : -1, c->cas);
        break;
    case OPERATION_APPEND:
        MEMCACHED_COMMAND_APPEND(c->sfd, info.info.key, info.info.nkey,
                                 (ret == ENGINE_SUCCESS) ? info.info.nbytes : -1, c->cas);
        break;
    case OPERATION_PREPEND:
        MEMCACHED_COMMAND_PREPEND(c->sfd, info.info.key, info.info.nkey,
                                  (ret == ENGINE_SUCCESS) ? info.info.nbytes : -1, c->cas);
        break;
    case OPERATION_SET:
        MEMCACHED_COMMAND_SET(c->sfd, info.info.key, info.info.nkey,
                              (ret == ENGINE_SUCCESS) ? info.info.nbytes : -1, c->cas);
        break;
    }
#endif

    switch (ret) {
    case ENGINE_SUCCESS:
        /* Stored */
        write_bin_response(c, NULL, 0, 0, 0);
        break;
    case ENGINE_KEY_EEXISTS:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, 0);
        break;
    case ENGINE_KEY_ENOENT:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, 0);
        break;
    case ENGINE_ENOMEM:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, 0);
        break;
    case ENGINE_TMPFAIL:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ETMPFAIL, 0);
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        break;
    case ENGINE_DISCONNECT:
        c->state = conn_closing;
        break;
    case ENGINE_ENOTSUP:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
        break;
    case ENGINE_NOT_MY_VBUCKET:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET, 0);
        break;
    case ENGINE_E2BIG:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_E2BIG, 0);
        break;
    default:
        if (c->store_op == OPERATION_ADD) {
            eno = PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
        } else if(c->store_op == OPERATION_REPLACE) {
            eno = PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
        } else {
            eno = PROTOCOL_BINARY_RESPONSE_NOT_STORED;
        }
        write_bin_packet(c, eno, 0);
    }

    if (c->store_op == OPERATION_CAS) {
        switch (ret) {
        case ENGINE_SUCCESS:
            SLAB_INCR(c, cas_hits, info.info.key, info.info.nkey);
            break;
        case ENGINE_KEY_EEXISTS:
            SLAB_INCR(c, cas_badval, info.info.key, info.info.nkey);
            break;
        case ENGINE_KEY_ENOENT:
            STATS_NOKEY(c, cas_misses);
            break;
        default:
            ;
        }
    } else {
        SLAB_INCR(c, cmd_set, info.info.key, info.info.nkey);
    }

    if (!c->ewouldblock) {
        /* release the c->item reference */
        settings.engine.v1->release(settings.engine.v0, c, c->item);
        c->item = 0;
    }
}

static void process_bin_get(conn *c) {
    item *it;
    protocol_binary_response_get* rsp = (protocol_binary_response_get*)c->wbuf;
    char* key = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;
    uint16_t keylen;
    uint32_t bodylen;
    item_info_holder info;
    int ii;
    ENGINE_ERROR_CODE ret;
    uint8_t datatype;
    bool need_inflate = false;

    memset(&info, 0, sizeof(info));
    if (settings.verbose > 1) {
        char buffer[1024];
        if (key_to_printable_buffer(buffer, sizeof(buffer), c->sfd, true,
                                    "GET", key, nkey) != -1) {
            settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c, "%s\n",
                                            buffer);
        }
    }

    ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    if (ret == ENGINE_SUCCESS) {
        ret = settings.engine.v1->get(settings.engine.v0, c, &it, key, (int)nkey,
                                      c->binary_header.request.vbucket);
    }

    info.info.nvalue = IOV_MAX;
    switch (ret) {
    case ENGINE_SUCCESS:
        STATS_HIT(c, get, key, nkey);

        if (!settings.engine.v1->get_item_info(settings.engine.v0, c, it,
                                               (void*)&info)) {
            settings.engine.v1->release(settings.engine.v0, c, it);
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                            "%d: Failed to get item info",
                                            c->sfd);
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINTERNAL, 0);
            break;
        }

        datatype = info.info.datatype;
        if (!c->supports_datatype) {
            if ((datatype & PROTOCOL_BINARY_DATATYPE_COMPRESSED) == PROTOCOL_BINARY_DATATYPE_COMPRESSED) {
                need_inflate = true;
            } else {
                datatype = PROTOCOL_BINARY_RAW_BYTES;
            }
        }

        keylen = 0;
        bodylen = sizeof(rsp->message.body) + info.info.nbytes;

        if (c->cmd == PROTOCOL_BINARY_CMD_GETK) {
            bodylen += (uint32_t)nkey;
            keylen = (uint16_t)nkey;
        }

        if (need_inflate) {
            if (info.info.nvalue != 1) {
                write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINTERNAL, 0);
            } else if (binary_response_handler(key, keylen,
                                               &info.info.flags, 4,
                                               info.info.value[0].iov_base,
                                               (uint32_t)info.info.value[0].iov_len,
                                               datatype,
                                               PROTOCOL_BINARY_RESPONSE_SUCCESS,
                                               info.info.cas, c)) {
                write_and_free(c, c->dynamic_buffer.buffer, c->dynamic_buffer.offset);
                c->dynamic_buffer.buffer = NULL;
                settings.engine.v1->release(settings.engine.v0, c, it);
            } else {
                write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINTERNAL, 0);
            }
        } else {
            if (add_bin_header(c, 0, sizeof(rsp->message.body),
                               keylen, bodylen, datatype) == -1) {
                conn_set_state(c, conn_closing);
                return;
            }
            rsp->message.header.response.cas = htonll(info.info.cas);

            /* add the flags */
            rsp->message.body.flags = info.info.flags;
            add_iov(c, &rsp->message.body, sizeof(rsp->message.body));

            if (c->cmd == PROTOCOL_BINARY_CMD_GETK) {
                add_iov(c, info.info.key, nkey);
            }

            for (ii = 0; ii < info.info.nvalue; ++ii) {
                add_iov(c, info.info.value[ii].iov_base,
                        info.info.value[ii].iov_len);
            }
            conn_set_state(c, conn_mwrite);
            /* Remember this item so we can garbage collect it later */
            c->item = it;
        }
        break;
    case ENGINE_KEY_ENOENT:
        STATS_MISS(c, get, key, nkey);

        MEMCACHED_COMMAND_GET(c->sfd, key, nkey, -1, 0);

        if (c->noreply) {
            conn_set_state(c, conn_new_cmd);
        } else {
            if (c->cmd == PROTOCOL_BINARY_CMD_GETK) {
                char *ofs = c->wbuf + sizeof(protocol_binary_response_header);
                if (add_bin_header(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT,
                                   0, (uint16_t)nkey,
                                   (uint32_t)nkey, PROTOCOL_BINARY_RAW_BYTES) == -1) {
                    conn_set_state(c, conn_closing);
                    return;
                }
                memcpy(ofs, key, nkey);
                add_iov(c, ofs, nkey);
                conn_set_state(c, conn_mwrite);
            } else {
                write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, 0);
            }
        }
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        break;
    case ENGINE_DISCONNECT:
        c->state = conn_closing;
        break;
    default:
        write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
    }

    if (settings.detail_enabled && ret != ENGINE_EWOULDBLOCK) {
        stats_prefix_record_get(key, nkey, ret == ENGINE_SUCCESS);
    }
}

static void append_bin_stats(const char *key, const uint16_t klen,
                             const char *val, const uint32_t vlen,
                             conn *c) {
    char *buf = c->dynamic_buffer.buffer + c->dynamic_buffer.offset;
    uint32_t bodylen = klen + vlen;
    protocol_binary_response_header header;

    memset(&header, 0, sizeof(header));
    header.response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header.response.opcode = PROTOCOL_BINARY_CMD_STAT;
    header.response.keylen = (uint16_t)htons(klen);
    header.response.datatype = (uint8_t)PROTOCOL_BINARY_RAW_BYTES;
    header.response.bodylen = htonl(bodylen);
    header.response.opaque = c->opaque;

    memcpy(buf, header.bytes, sizeof(header.response));
    buf += sizeof(header.response);

    if (klen > 0) {
        cb_assert(key != NULL);
        memcpy(buf, key, klen);
        buf += klen;

        if (vlen > 0) {
            memcpy(buf, val, vlen);
        }
    }

    c->dynamic_buffer.offset += sizeof(header.response) + bodylen;
}

static bool grow_dynamic_buffer(conn *c, size_t needed) {
    size_t nsize = c->dynamic_buffer.size;
    size_t available = nsize - c->dynamic_buffer.offset;
    bool rv = true;

    /* Special case: No buffer -- need to allocate fresh */
    if (c->dynamic_buffer.buffer == NULL) {
        nsize = 1024;
        available = c->dynamic_buffer.size = c->dynamic_buffer.offset = 0;
    }

    while (needed > available) {
        cb_assert(nsize > 0);
        nsize = nsize << 1;
        available = nsize - c->dynamic_buffer.offset;
    }

    if (nsize != c->dynamic_buffer.size) {
        char *ptr = realloc(c->dynamic_buffer.buffer, nsize);
        if (ptr) {
            c->dynamic_buffer.buffer = ptr;
            c->dynamic_buffer.size = nsize;
        } else {
            rv = false;
        }
    }

    return rv;
}

static void append_stats(const char *key, const uint16_t klen,
                         const char *val, const uint32_t vlen,
                         const void *cookie)
{
    size_t needed;
    conn *c = (conn*)cookie;
    /* value without a key is invalid */
    if (klen == 0 && vlen > 0) {
        return ;
    }

    needed = vlen + klen + sizeof(protocol_binary_response_header);
    if (!grow_dynamic_buffer(c, needed)) {
        return ;
    }
    append_bin_stats(key, klen, val, vlen, c);
    cb_assert(c->dynamic_buffer.offset <= c->dynamic_buffer.size);
}

static void bin_read_chunk(conn *c,
                           enum bin_substates next_substate,
                           uint32_t chunk) {
    ptrdiff_t offset;
    cb_assert(c);
    c->substate = next_substate;
    c->rlbytes = chunk;

    /* Ok... do we have room for everything in our buffer? */
    offset = c->rcurr + sizeof(protocol_binary_request_header) - c->rbuf;
    if (c->rlbytes > c->rsize - offset) {
        size_t nsize = c->rsize;
        size_t size = c->rlbytes + sizeof(protocol_binary_request_header);

        while (size > nsize) {
            nsize *= 2;
        }

        if (nsize != c->rsize) {
            char *newm;
            if (settings.verbose > 1) {
                settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                        "%d: Need to grow buffer from %lu to %lu\n",
                        c->sfd, (unsigned long)c->rsize, (unsigned long)nsize);
            }
            newm = realloc(c->rbuf, nsize);
            if (newm == NULL) {
                if (settings.verbose) {
                    settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                            "%d: Failed to grow buffer.. closing connection\n",
                            c->sfd);
                }
                conn_set_state(c, conn_closing);
                return;
            }

            c->rbuf= newm;
            /* rcurr should point to the same offset in the packet */
            c->rcurr = c->rbuf + offset - sizeof(protocol_binary_request_header);
            c->rsize = (int)nsize;
        }
        if (c->rbuf != c->rcurr) {
            memmove(c->rbuf, c->rcurr, c->rbytes);
            c->rcurr = c->rbuf;
            if (settings.verbose > 1) {
                settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                                "%d: Repack input buffer\n",
                                                c->sfd);
            }
        }
    }

    /* preserve the header in the buffer.. */
    c->ritem = c->rcurr + sizeof(protocol_binary_request_header);
    conn_set_state(c, conn_nread);
}

static void bin_read_key(conn *c, enum bin_substates next_substate, int extra) {
    bin_read_chunk(c, next_substate, c->keylen + extra);
}


/* Just write an error message and disconnect the client */
static void handle_binary_protocol_error(conn *c) {
    write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
    if (settings.verbose) {
        settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                "%d: Protocol error (opcode %02x), close connection\n",
                c->sfd, c->binary_header.request.opcode);
    }
    c->write_and_go = conn_closing;
}

static void get_auth_data(const void *cookie, auth_data_t *data) {
    conn *c = (conn*)cookie;
    if (c->sasl_conn) {
        cbsasl_getprop(c->sasl_conn, CBSASL_USERNAME, (void*)&data->username);
        cbsasl_getprop(c->sasl_conn, CBSASL_CONFIG, (void*)&data->config);
    } else {
        data->username = NULL;
        data->config = NULL;
    }
}

struct sasl_tmp {
    int ksize;
    int vsize;
    char data[1]; /* data + ksize == value */
};

static void process_bin_sasl_auth(conn *c) {
    int nkey;
    int vlen;
    char *key;
    size_t buffer_size;
    struct sasl_tmp *data;

    cb_assert(c->binary_header.request.extlen == 0);
    nkey = c->binary_header.request.keylen;
    vlen = c->binary_header.request.bodylen - nkey;

    if (nkey > MAX_SASL_MECH_LEN || vlen < 0) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, vlen);
        c->write_and_go = conn_swallow;
        return;
    }

    key = binary_get_key(c);
    cb_assert(key);

    buffer_size = sizeof(struct sasl_tmp) + nkey + vlen + 2;
    data = calloc(sizeof(struct sasl_tmp) + buffer_size, 1);
    if (!data) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, vlen);
        c->write_and_go = conn_swallow;
        return;
    }

    data->ksize = nkey;
    data->vsize = vlen;
    memcpy(data->data, key, nkey);

    c->item = data;
    c->ritem = data->data + nkey;
    c->rlbytes = vlen;
    conn_set_state(c, conn_nread);
    c->substate = bin_reading_sasl_auth_data;
}

static void process_bin_complete_sasl_auth(conn *c) {
    auth_data_t data;
    const char *out = NULL;
    unsigned int outlen = 0;
    int nkey;
    int vlen;
    struct sasl_tmp *stmp;
    char mech[1024];
    const char *challenge;
    int result=-1;

    cb_assert(c->item);

    nkey = c->binary_header.request.keylen;
    if (nkey > 1023) {
        /* too big.. */
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                "%d: sasl error. key: %d > 1023", c->sfd, nkey);
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, 0);
        return;
    }
    vlen = c->binary_header.request.bodylen - nkey;

    stmp = c->item;
    memcpy(mech, stmp->data, nkey);
    mech[nkey] = 0x00;

    if (settings.verbose) {
        settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                "%d: mech: ``%s'' with %d bytes of data\n", c->sfd, mech, vlen);
    }

    challenge = vlen == 0 ? NULL : (stmp->data + nkey);
    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SASL_AUTH:
        result = cbsasl_server_start(&c->sasl_conn, mech,
                                     challenge, vlen,
                                     (unsigned char **)&out, &outlen);
        break;
    case PROTOCOL_BINARY_CMD_SASL_STEP:
        result = cbsasl_server_step(c->sasl_conn, challenge,
                                    vlen, &out, &outlen);
        break;
    default:
        cb_assert(false); /* CMD should be one of the above */
        /* This code is pretty much impossible, but makes the compiler
           happier */
        if (settings.verbose) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                    "%d: Unhandled command %d with challenge %s\n",
                    c->sfd, c->cmd, challenge);
        }
        break;
    }

    free(c->item);
    c->item = NULL;
    c->ritem = NULL;

    if (settings.verbose) {
        settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                        "%d: sasl result code:  %d\n",
                                        c->sfd, result);
    }

    switch(result) {
    case SASL_OK:
        write_bin_response(c, "Authenticated", 0, 0, (uint32_t)strlen("Authenticated"));
        get_auth_data(c, &data);
        if (settings.disable_admin) {
            /* "everyone is admins" */
            cookie_set_admin(c);
        } else if (settings.admin != NULL && data.username != NULL) {
            if (strcmp(settings.admin, data.username) == 0) {
                cookie_set_admin(c);
            }
        }
        perform_callbacks(ON_AUTH, (const void*)&data, c);
        STATS_NOKEY(c, auth_cmds);
        break;
    case SASL_CONTINUE:
        if (add_bin_header(c, PROTOCOL_BINARY_RESPONSE_AUTH_CONTINUE, 0, 0,
                           outlen, PROTOCOL_BINARY_RAW_BYTES) == -1) {
            conn_set_state(c, conn_closing);
            return;
        }
        add_iov(c, out, outlen);
        conn_set_state(c, conn_mwrite);
        c->write_and_go = conn_new_cmd;
        break;
    case SASL_BADPARAM:
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "%d: Bad sasl params: %d\n",
                                        c->sfd, result);
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
        STATS_NOKEY2(c, auth_cmds, auth_errors);
        break;
    default:
        if (result == SASL_NOUSER || result == SASL_PWERR) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                            "%d: Invalid username/password combination",
                                            c->sfd);
        } else {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                            "%d: Unknown sasl response: %d",
                                            c->sfd, result);
        }
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, 0);
        STATS_NOKEY2(c, auth_cmds, auth_errors);
    }
}

static bool authenticated(conn *c) {
    bool rv = false;

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_SASL_AUTH:       /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_SASL_STEP:       /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_VERSION:         /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_HELLO:
        rv = true;
        break;
    default:
        if (c->sasl_conn) {
            const void *uname = NULL;
            cbsasl_getprop(c->sasl_conn, CBSASL_USERNAME, &uname);
            rv = uname != NULL;
        }
    }

    if (settings.verbose > 1) {
        settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                "%d: authenticated() in cmd 0x%02x is %s\n",
                c->sfd, c->cmd, rv ? "true" : "false");
    }

    return rv;
}

bool binary_response_handler(const void *key, uint16_t keylen,
                             const void *ext, uint8_t extlen,
                             const void *body, uint32_t bodylen,
                             uint8_t datatype, uint16_t status,
                             uint64_t cas, const void *cookie)
{
    protocol_binary_response_header header;
    char *buf;
    conn *c = (conn*)cookie;
    /* Look at append_bin_stats */
    size_t needed;
    bool need_inflate = false;
    size_t inflated_length;

    if (!c->supports_datatype) {
        if ((datatype & PROTOCOL_BINARY_DATATYPE_COMPRESSED) == PROTOCOL_BINARY_DATATYPE_COMPRESSED) {
            need_inflate = true;
        }
        /* We may silently drop the knowledge about a JSON item */
        datatype = PROTOCOL_BINARY_RAW_BYTES;
    }

    needed = keylen + extlen + sizeof(protocol_binary_response_header);
    if (need_inflate) {
        if (snappy_uncompressed_length(body, bodylen,
                                       &inflated_length) != SNAPPY_OK) {
            settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                    "<%d ERROR: Failed to determine inflated size",
                    c->sfd);
            return false;
        }
        needed += inflated_length;
    } else {
        needed += bodylen;
    }

    if (!grow_dynamic_buffer(c, needed)) {
        if (settings.verbose > 0) {
            settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                    "<%d ERROR: Failed to allocate memory for response",
                    c->sfd);
        }
        return false;
    }

    buf = c->dynamic_buffer.buffer + c->dynamic_buffer.offset;
    memset(&header, 0, sizeof(header));
    header.response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header.response.opcode = c->binary_header.request.opcode;
    header.response.keylen = (uint16_t)htons(keylen);
    header.response.extlen = extlen;
    header.response.datatype = datatype;
    header.response.status = (uint16_t)htons(status);
    if (need_inflate) {
        header.response.bodylen = htonl((uint32_t)(inflated_length + keylen + extlen));
    } else {
        header.response.bodylen = htonl(bodylen + keylen + extlen);
    }
    header.response.opaque = c->opaque;
    header.response.cas = htonll(cas);

    memcpy(buf, header.bytes, sizeof(header.response));
    buf += sizeof(header.response);

    if (extlen > 0) {
        memcpy(buf, ext, extlen);
        buf += extlen;
    }

    if (keylen > 0) {
        cb_assert(key != NULL);
        memcpy(buf, key, keylen);
        buf += keylen;
    }

    if (bodylen > 0) {
        if (need_inflate) {
            if (snappy_uncompress(body, bodylen, buf, &inflated_length) != SNAPPY_OK) {
                settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                        "<%d ERROR: Failed to inflate item", c->sfd);
                return false;
            }
        } else {
            memcpy(buf, body, bodylen);
        }
    }

    c->dynamic_buffer.offset += needed;
    return true;
}

/**
 * Tap stats (these are only used by the tap thread, so they don't need
 * to be in the threadlocal struct right now...
 */
struct tap_cmd_stats {
    uint64_t connect;
    uint64_t mutation;
    uint64_t checkpoint_start;
    uint64_t checkpoint_end;
    uint64_t delete;
    uint64_t flush;
    uint64_t opaque;
    uint64_t vbucket_set;
};

struct tap_stats {
    cb_mutex_t mutex;
    struct tap_cmd_stats sent;
    struct tap_cmd_stats received;
} tap_stats;

static void ship_tap_log(conn *c) {
    bool more_data = true;
    bool send_data = false;
    bool disconnect = false;
    item *it;
    uint32_t bodylen;
    int ii = 0;

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
        if (settings.verbose) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                            "%d: Failed to create output headers. Shutting down tap connection\n", c->sfd);
        }
        conn_set_state(c, conn_closing);
        return ;
    }
    /* @todo add check for buffer overflow of c->wbuf) */
    c->wbytes = 0;
    c->wcurr = c->wbuf;
    c->icurr = c->ilist;
    do {
        /* @todo fixme! */
        void *engine;
        uint16_t nengine;
        uint8_t ttl;
        uint16_t tap_flags;
        uint32_t seqno;
        uint16_t vbucket;
        tap_event_t event;
        bool inflate = false;
        size_t inflated_length = 0;

        union {
            protocol_binary_request_tap_mutation mutation;
            protocol_binary_request_tap_delete delete;
            protocol_binary_request_tap_flush flush;
            protocol_binary_request_tap_opaque opaque;
            protocol_binary_request_noop noop;
        } msg;
        item_info_holder info;
        memset(&info, 0, sizeof(info));

        if (ii++ == 10) {
            break;
        }

        event = c->tap_iterator(settings.engine.v0, c, &it,
                                            &engine, &nengine, &ttl,
                                            &tap_flags, &seqno, &vbucket);
        memset(&msg, 0, sizeof(msg));
        msg.opaque.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
        msg.opaque.message.header.request.opaque = htonl(seqno);
        msg.opaque.message.body.tap.enginespecific_length = htons(nengine);
        msg.opaque.message.body.tap.ttl = ttl;
        msg.opaque.message.body.tap.flags = htons(tap_flags);
        msg.opaque.message.header.request.extlen = 8;
        msg.opaque.message.header.request.vbucket = htons(vbucket);
        info.info.nvalue = IOV_MAX;

        switch (event) {
        case TAP_NOOP :
            send_data = true;
            msg.noop.message.header.request.opcode = PROTOCOL_BINARY_CMD_NOOP;
            msg.noop.message.header.request.extlen = 0;
            msg.noop.message.header.request.bodylen = htonl(0);
            memcpy(c->wcurr, msg.noop.bytes, sizeof(msg.noop.bytes));
            add_iov(c, c->wcurr, sizeof(msg.noop.bytes));
            c->wcurr += sizeof(msg.noop.bytes);
            c->wbytes += sizeof(msg.noop.bytes);
            break;
        case TAP_PAUSE :
            more_data = false;
            break;
        case TAP_CHECKPOINT_START:
        case TAP_CHECKPOINT_END:
        case TAP_MUTATION:
            if (!settings.engine.v1->get_item_info(settings.engine.v0, c, it,
                                                   (void*)&info)) {
                settings.engine.v1->release(settings.engine.v0, c, it);
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                "%d: Failed to get item info\n", c->sfd);
                break;
            }
            send_data = true;
            c->ilist[c->ileft++] = it;

            if (event == TAP_CHECKPOINT_START) {
                msg.mutation.message.header.request.opcode =
                    PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_START;
                cb_mutex_enter(&tap_stats.mutex);
                tap_stats.sent.checkpoint_start++;
                cb_mutex_exit(&tap_stats.mutex);
            } else if (event == TAP_CHECKPOINT_END) {
                msg.mutation.message.header.request.opcode =
                    PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_END;
                cb_mutex_enter(&tap_stats.mutex);
                tap_stats.sent.checkpoint_end++;
                cb_mutex_exit(&tap_stats.mutex);
            } else if (event == TAP_MUTATION) {
                msg.mutation.message.header.request.opcode = PROTOCOL_BINARY_CMD_TAP_MUTATION;
                cb_mutex_enter(&tap_stats.mutex);
                tap_stats.sent.mutation++;
                cb_mutex_exit(&tap_stats.mutex);
            }

            msg.mutation.message.header.request.cas = htonll(info.info.cas);
            msg.mutation.message.header.request.keylen = htons(info.info.nkey);
            msg.mutation.message.header.request.extlen = 16;
            if (c->supports_datatype) {
                msg.mutation.message.header.request.datatype = info.info.datatype;
            } else {
                switch (info.info.datatype) {
                case 0:
                    break;
                case PROTOCOL_BINARY_DATATYPE_JSON:
                    break;
                case PROTOCOL_BINARY_DATATYPE_COMPRESSED:
                case PROTOCOL_BINARY_DATATYPE_COMPRESSED_JSON:
                    inflate = true;
                    break;
                default:
                    settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                    "%d: shipping data with"
                                                    " an invalid datatype "
                                                    "(stripping info)",
                                                    c->sfd);
                }
                msg.mutation.message.header.request.datatype = 0;
            }

            bodylen = 16 + info.info.nkey + nengine;
            if ((tap_flags & TAP_FLAG_NO_VALUE) == 0) {
                if (inflate) {
                    if (snappy_uncompressed_length(info.info.value[0].iov_base,
                                                   info.info.nbytes,
                                                   &inflated_length) == SNAPPY_OK) {
                        bodylen += inflated_length;
                    } else {
                        settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                                        "<%d ERROR: Failed to determine inflated size. Sending as compressed",
                                                        c->sfd);
                        inflate = false;
                        bodylen += info.info.nbytes;
                    }
                } else {
                    bodylen += info.info.nbytes;
                }
            }
            msg.mutation.message.header.request.bodylen = htonl(bodylen);

            if ((tap_flags & TAP_FLAG_NETWORK_BYTE_ORDER) == 0) {
                msg.mutation.message.body.item.flags = htonl(info.info.flags);
            } else {
                msg.mutation.message.body.item.flags = info.info.flags;
            }
            msg.mutation.message.body.item.expiration = htonl(info.info.exptime);
            msg.mutation.message.body.tap.enginespecific_length = htons(nengine);
            msg.mutation.message.body.tap.ttl = ttl;
            msg.mutation.message.body.tap.flags = htons(tap_flags);
            memcpy(c->wcurr, msg.mutation.bytes, sizeof(msg.mutation.bytes));

            add_iov(c, c->wcurr, sizeof(msg.mutation.bytes));
            c->wcurr += sizeof(msg.mutation.bytes);
            c->wbytes += sizeof(msg.mutation.bytes);

            if (nengine > 0) {
                memcpy(c->wcurr, engine, nengine);
                add_iov(c, c->wcurr, nengine);
                c->wcurr += nengine;
                c->wbytes += nengine;
            }

            add_iov(c, info.info.key, info.info.nkey);
            if ((tap_flags & TAP_FLAG_NO_VALUE) == 0) {
                if (inflate) {
                    void *buf = malloc(inflated_length);
                    void *body = info.info.value[0].iov_base;
                    size_t bodylen = info.info.value[0].iov_len;
                    if (snappy_uncompress(body, bodylen,
                                          buf, &inflated_length) == SNAPPY_OK) {
                        c->temp_alloc_list[c->temp_alloc_left++] = buf;

                        add_iov(c, buf, inflated_length);
                    } else {
                        free(buf);
                        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                        "%d: FATAL: failed to inflate object. shutitng down connection", c->sfd);
                        conn_set_state(c, conn_closing);
                        return;
                    }
                } else {
                    int xx;
                    for (xx = 0; xx < info.info.nvalue; ++xx) {
                        add_iov(c, info.info.value[xx].iov_base,
                                info.info.value[xx].iov_len);
                    }
                }
            }

            break;
        case TAP_DELETION:
            /* This is a delete */
            if (!settings.engine.v1->get_item_info(settings.engine.v0, c, it,
                                                   (void*)&info)) {
                settings.engine.v1->release(settings.engine.v0, c, it);
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                "%d: Failed to get item info\n", c->sfd);
                break;
            }
            send_data = true;
            c->ilist[c->ileft++] = it;
            msg.delete.message.header.request.opcode = PROTOCOL_BINARY_CMD_TAP_DELETE;
            msg.delete.message.header.request.cas = htonll(info.info.cas);
            msg.delete.message.header.request.keylen = htons(info.info.nkey);

            bodylen = 8 + info.info.nkey + nengine;
            if ((tap_flags & TAP_FLAG_NO_VALUE) == 0) {
                bodylen += info.info.nbytes;
            }
            msg.delete.message.header.request.bodylen = htonl(bodylen);

            memcpy(c->wcurr, msg.delete.bytes, sizeof(msg.delete.bytes));
            add_iov(c, c->wcurr, sizeof(msg.delete.bytes));
            c->wcurr += sizeof(msg.delete.bytes);
            c->wbytes += sizeof(msg.delete.bytes);

            if (nengine > 0) {
                memcpy(c->wcurr, engine, nengine);
                add_iov(c, c->wcurr, nengine);
                c->wcurr += nengine;
                c->wbytes += nengine;
            }

            add_iov(c, info.info.key, info.info.nkey);
            if ((tap_flags & TAP_FLAG_NO_VALUE) == 0) {
                int xx;
                for (xx = 0; xx < info.info.nvalue; ++xx) {
                    add_iov(c, info.info.value[xx].iov_base,
                            info.info.value[xx].iov_len);
                }
            }

            cb_mutex_enter(&tap_stats.mutex);
            tap_stats.sent.delete++;
            cb_mutex_exit(&tap_stats.mutex);
            break;

        case TAP_DISCONNECT:
            disconnect = true;
            more_data = false;
            break;
        case TAP_VBUCKET_SET:
        case TAP_FLUSH:
        case TAP_OPAQUE:
            send_data = true;

            if (event == TAP_OPAQUE) {
                msg.flush.message.header.request.opcode = PROTOCOL_BINARY_CMD_TAP_OPAQUE;
                cb_mutex_enter(&tap_stats.mutex);
                tap_stats.sent.opaque++;
                cb_mutex_exit(&tap_stats.mutex);

            } else if (event == TAP_FLUSH) {
                msg.flush.message.header.request.opcode = PROTOCOL_BINARY_CMD_TAP_FLUSH;
                cb_mutex_enter(&tap_stats.mutex);
                tap_stats.sent.flush++;
                cb_mutex_exit(&tap_stats.mutex);
            } else if (event == TAP_VBUCKET_SET) {
                msg.flush.message.header.request.opcode = PROTOCOL_BINARY_CMD_TAP_VBUCKET_SET;
                msg.flush.message.body.tap.flags = htons(tap_flags);
                cb_mutex_enter(&tap_stats.mutex);
                tap_stats.sent.vbucket_set++;
                cb_mutex_exit(&tap_stats.mutex);
            }

            msg.flush.message.header.request.bodylen = htonl(8 + nengine);
            memcpy(c->wcurr, msg.flush.bytes, sizeof(msg.flush.bytes));
            add_iov(c, c->wcurr, sizeof(msg.flush.bytes));
            c->wcurr += sizeof(msg.flush.bytes);
            c->wbytes += sizeof(msg.flush.bytes);
            if (nengine > 0) {
                memcpy(c->wcurr, engine, nengine);
                add_iov(c, c->wcurr, nengine);
                c->wcurr += nengine;
                c->wbytes += nengine;
            }
            break;
        default:
            abort();
        }
    } while (more_data);

    c->ewouldblock = false;
    if (send_data) {
        conn_set_state(c, conn_mwrite);
        if (disconnect) {
            c->write_and_go = conn_closing;
        } else {
            c->write_and_go = conn_ship_log;
        }
    } else {
        if (disconnect) {
            conn_set_state(c, conn_closing);
        } else {
            /* No more items to ship to the slave at this time.. suspend.. */
            if (settings.verbose > 1) {
                settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                                "%d: No more items in tap log.. waiting\n",
                                                c->sfd);
            }
            c->ewouldblock = true;
        }
    }
}

static ENGINE_ERROR_CODE default_unknown_command(EXTENSION_BINARY_PROTOCOL_DESCRIPTOR *descriptor,
                                                 ENGINE_HANDLE* handle,
                                                 const void* cookie,
                                                 protocol_binary_request_header *request,
                                                 ADD_RESPONSE response)
{
    const conn *c = (void*)cookie;

    if (!c->supports_datatype && request->request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        if (response(NULL, 0, NULL, 0, NULL, 0, PROTOCOL_BINARY_RAW_BYTES,
                     PROTOCOL_BINARY_RESPONSE_EINVAL, 0, cookie)) {
            return ENGINE_SUCCESS;
        } else {
            return ENGINE_DISCONNECT;
        }
    } else {
        return settings.engine.v1->unknown_command(handle, cookie,
                                                   request, response);
    }
}

struct request_lookup {
    EXTENSION_BINARY_PROTOCOL_DESCRIPTOR *descriptor;
    BINARY_COMMAND_CALLBACK callback;
};

static struct request_lookup request_handlers[0x100];

typedef void (*RESPONSE_HANDLER)(conn*);
/**
 * A map between the response packets op-code and the function to handle
 * the response message.
 */
static RESPONSE_HANDLER response_handlers[0x100];

static void setup_binary_lookup_cmd(EXTENSION_BINARY_PROTOCOL_DESCRIPTOR *descriptor,
                                    uint8_t cmd,
                                    BINARY_COMMAND_CALLBACK new_handler) {
    request_handlers[cmd].descriptor = descriptor;
    request_handlers[cmd].callback = new_handler;
}

static void process_bin_unknown_packet(conn *c) {
    void *packet = c->rcurr - (c->binary_header.request.bodylen +
                               sizeof(c->binary_header));
    ENGINE_ERROR_CODE ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;

    if (ret == ENGINE_SUCCESS) {
        struct request_lookup *rq = request_handlers + c->binary_header.request.opcode;
        ret = rq->callback(rq->descriptor, settings.engine.v0, c, packet,
                           binary_response_handler);
    }

    switch (ret) {
    case ENGINE_SUCCESS:
        if (c->dynamic_buffer.buffer != NULL) {
            write_and_free(c, c->dynamic_buffer.buffer, c->dynamic_buffer.offset);
            c->dynamic_buffer.buffer = NULL;
        } else {
            conn_set_state(c, conn_new_cmd);
        }
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        break;
    case ENGINE_DISCONNECT:
        conn_set_state(c, conn_closing);
        break;
    default:
        /* Release the dynamic buffer.. it may be partial.. */
        free(c->dynamic_buffer.buffer);
        c->dynamic_buffer.buffer = NULL;
        write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
    }
}

static void cbsasl_refresh_main(void *c)
{
    int rv = cbsasl_server_refresh();
    if (rv == SASL_OK) {
        notify_io_complete(c, ENGINE_SUCCESS);
    } else {
        notify_io_complete(c, ENGINE_EINVAL);
    }
}

static ENGINE_ERROR_CODE refresh_cbsasl(conn *c)
{
    cb_thread_t tid;
    int err;

    err = cb_create_thread(&tid, cbsasl_refresh_main, c, 1);
    if (err != 0) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "Failed to create cbsasl db "
                                        "update thread: %s",
                                        strerror(err));
        return ENGINE_DISCONNECT;
    }

    return ENGINE_EWOULDBLOCK;
}

#if 0
static void ssl_certs_refresh_main(void *c)
{
    /* Update the internal certificates */

    notify_io_complete(c, ENGINE_SUCCESS);
}
#endif
static ENGINE_ERROR_CODE refresh_ssl_certs(conn *c)
{
#if 0
    cb_thread_t tid;
    int err;

    err = cb_create_thread(&tid, ssl_certs_refresh_main, c, 1);
    if (err != 0) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "Failed to create ssl_certificate "
                                        "update thread: %s",
                                        strerror(err));
        return ENGINE_DISCONNECT;
    }

    return ENGINE_EWOULDBLOCK;
#endif
    return ENGINE_SUCCESS;
}

static void process_bin_tap_connect(conn *c) {
    TAP_ITERATOR iterator;
    char *packet = (c->rcurr - (c->binary_header.request.bodylen +
                                sizeof(c->binary_header)));
    protocol_binary_request_tap_connect *req = (void*)packet;
    const char *key = packet + sizeof(req->bytes);
    const char *data = key + c->binary_header.request.keylen;
    uint32_t flags = 0;
    size_t ndata = c->binary_header.request.bodylen -
        c->binary_header.request.extlen -
        c->binary_header.request.keylen;

    if (c->binary_header.request.extlen == 4) {
        flags = ntohl(req->message.body.flags);

        if (flags & TAP_CONNECT_FLAG_BACKFILL) {
            /* the userdata has to be at least 8 bytes! */
            if (ndata < 8) {
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                "%d: ERROR: Invalid tap connect message\n",
                                                c->sfd);
                conn_set_state(c, conn_closing);
                return ;
            }
        }
    } else {
        data -= 4;
        key -= 4;
    }

    if (settings.verbose && c->binary_header.request.keylen > 0) {
        char buffer[1024];
        int len = c->binary_header.request.keylen;
        if (len >= sizeof(buffer)) {
            len = sizeof(buffer) - 1;
        }
        memcpy(buffer, key, len);
        buffer[len] = '\0';
        settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                        "%d: Trying to connect with named tap connection: <%s>\n",
                                        c->sfd, buffer);
    }

    iterator = settings.engine.v1->get_tap_iterator(
        settings.engine.v0, c, key, c->binary_header.request.keylen,
        flags, data, ndata);

    if (iterator == NULL) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "%d: FATAL: The engine does not support tap\n",
                                        c->sfd);
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
        c->write_and_go = conn_closing;
    } else {
        c->max_reqs_per_event = settings.reqs_per_event_high_priority;
        c->tap_iterator = iterator;
        c->which = EV_WRITE;
        conn_set_state(c, conn_ship_log);
    }
}

static void process_bin_tap_packet(tap_event_t event, conn *c) {
    char *packet;
    protocol_binary_request_tap_no_extras *tap;
    uint16_t nengine;
    uint16_t tap_flags;
    uint32_t seqno;
    uint8_t ttl;
    char *engine_specific;
    char *key;
    uint16_t nkey;
    char *data;
    uint32_t flags;
    uint32_t exptime;
    uint32_t ndata;
    ENGINE_ERROR_CODE ret;

    cb_assert(c != NULL);
    packet = (c->rcurr - (c->binary_header.request.bodylen +
                                sizeof(c->binary_header)));
    tap = (void*)packet;
    nengine = ntohs(tap->message.body.tap.enginespecific_length);
    tap_flags = ntohs(tap->message.body.tap.flags);
    seqno = ntohl(tap->message.header.request.opaque);
    ttl = tap->message.body.tap.ttl;
    engine_specific = packet + sizeof(tap->bytes);
    key = engine_specific + nengine;
    nkey = c->binary_header.request.keylen;
    data = key + nkey;
    flags = 0;
    exptime = 0;
    ndata = c->binary_header.request.bodylen - nengine - nkey - 8;
    ret = c->aiostat;

    if (ttl == 0) {
        ret = ENGINE_EINVAL;
    } else {
        if (event == TAP_MUTATION || event == TAP_CHECKPOINT_START ||
            event == TAP_CHECKPOINT_END) {
            protocol_binary_request_tap_mutation *mutation = (void*)tap;

            /* engine_specific data in protocol_binary_request_tap_mutation is */
            /* at a different offset than protocol_binary_request_tap_no_extras */
            engine_specific = packet + sizeof(mutation->bytes);

            flags = mutation->message.body.item.flags;
            if ((tap_flags & TAP_FLAG_NETWORK_BYTE_ORDER) == 0) {
                flags = ntohl(flags);
            }

            exptime = ntohl(mutation->message.body.item.expiration);
            key += 8;
            data += 8;
            ndata -= 8;
        }

        if (ret == ENGINE_SUCCESS) {
            uint8_t datatype = c->binary_header.request.datatype;
            if (event == TAP_MUTATION && !c->supports_datatype) {
                if (checkUTF8JSON((void*)data, ndata)) {
                    datatype = PROTOCOL_BINARY_DATATYPE_JSON;
                }
            }

            ret = settings.engine.v1->tap_notify(settings.engine.v0, c,
                                                 engine_specific, nengine,
                                                 ttl - 1, tap_flags,
                                                 event, seqno,
                                                 key, nkey,
                                                 flags, exptime,
                                                 ntohll(tap->message.header.request.cas),
                                                 datatype,
                                                 data, ndata,
                                                 c->binary_header.request.vbucket);
        }
    }

    switch (ret) {
    case ENGINE_DISCONNECT:
        conn_set_state(c, conn_closing);
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        break;
    default:
        if ((tap_flags & TAP_FLAG_ACK) || (ret != ENGINE_SUCCESS)) {
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        } else {
            conn_set_state(c, conn_new_cmd);
        }
    }
}

static void process_bin_tap_ack(conn *c) {
    char *packet;
    protocol_binary_response_no_extras *rsp;
    uint32_t seqno;
    uint16_t status;
    char *key;
    ENGINE_ERROR_CODE ret = ENGINE_DISCONNECT;

    cb_assert(c != NULL);
    packet = (c->rcurr - (c->binary_header.request.bodylen + sizeof(c->binary_header)));
    rsp = (void*)packet;
    seqno = ntohl(rsp->message.header.response.opaque);
    status = ntohs(rsp->message.header.response.status);
    key = packet + sizeof(rsp->bytes);

    if (settings.engine.v1->tap_notify != NULL) {
        ret = settings.engine.v1->tap_notify(settings.engine.v0, c, NULL, 0, 0, status,
                                             TAP_ACK, seqno, key,
                                             c->binary_header.request.keylen, 0, 0,
                                             0, c->binary_header.request.datatype, NULL,
                                             0, 0);
    }

    if (ret == ENGINE_DISCONNECT) {
        conn_set_state(c, conn_closing);
    } else {
        conn_set_state(c, conn_ship_log);
    }
}

/**
 * We received a noop response.. just ignore it
 */
static void process_bin_noop_response(conn *c) {
    cb_assert(c != NULL);
    conn_set_state(c, conn_new_cmd);
}

/*******************************************************************************
 **                             DCP MESSAGE PRODUCERS                         **
 ******************************************************************************/
static ENGINE_ERROR_CODE dcp_message_get_failover_log(const void *cookie,
                                                      uint32_t opaque,
                                                      uint16_t vbucket)
{
    protocol_binary_request_dcp_get_failover_log packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_stream_req(const void *cookie,
                                                uint32_t opaque,
                                                uint16_t vbucket,
                                                uint32_t flags,
                                                uint64_t start_seqno,
                                                uint64_t end_seqno,
                                                uint64_t vbucket_uuid,
                                                uint64_t snap_start_seqno,
                                                uint64_t snap_end_seqno)
{
    protocol_binary_request_dcp_stream_req packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_STREAM_REQ;
    packet.message.header.request.extlen = 48;
    packet.message.header.request.bodylen = htonl(48);
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);

    packet.message.body.flags = ntohl(flags);
    packet.message.body.start_seqno = ntohll(start_seqno);
    packet.message.body.end_seqno = ntohll(end_seqno);
    packet.message.body.vbucket_uuid = ntohll(vbucket_uuid);
    packet.message.body.snap_start_seqno = ntohll(snap_start_seqno);
    packet.message.body.snap_end_seqno = ntohll(snap_end_seqno);

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_add_stream_response(const void *cookie,
                                                         uint32_t opaque,
                                                         uint32_t dialogopaque,
                                                         uint8_t status)
{
    protocol_binary_response_dcp_add_stream packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.response.magic =  (uint8_t)PROTOCOL_BINARY_RES;
    packet.message.header.response.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_ADD_STREAM;
    packet.message.header.response.extlen = 4;
    packet.message.header.response.status = htons(status);
    packet.message.header.response.bodylen = htonl(4);
    packet.message.header.response.opaque = opaque;
    packet.message.body.opaque = ntohl(dialogopaque);

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_marker_response(const void *cookie,
                                                     uint32_t opaque,
                                                     uint8_t status)
{
    protocol_binary_response_dcp_snapshot_marker packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.response.magic =  (uint8_t)PROTOCOL_BINARY_RES;
    packet.message.header.response.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER;
    packet.message.header.response.extlen = 0;
    packet.message.header.response.status = htons(status);
    packet.message.header.response.bodylen = 0;
    packet.message.header.response.opaque = opaque;

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_set_vbucket_state_response(const void *cookie,
                                                                uint32_t opaque,
                                                                uint8_t status)
{
    protocol_binary_response_dcp_set_vbucket_state packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.response.magic =  (uint8_t)PROTOCOL_BINARY_RES;
    packet.message.header.response.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE;
    packet.message.header.response.extlen = 0;
    packet.message.header.response.status = htons(status);
    packet.message.header.response.bodylen = 0;
    packet.message.header.response.opaque = opaque;

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_stream_end(const void *cookie,
                                                uint32_t opaque,
                                                uint16_t vbucket,
                                                uint32_t flags)
{
    protocol_binary_request_dcp_stream_end packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_STREAM_END;
    packet.message.header.request.extlen = 4;
    packet.message.header.request.bodylen = htonl(4);
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.body.flags = ntohl(flags);

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_marker(const void *cookie,
                                            uint32_t opaque,
                                            uint16_t vbucket,
                                            uint64_t start_seqno,
                                            uint64_t end_seqno,
                                            uint32_t flags)
{
    protocol_binary_request_dcp_snapshot_marker packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.header.request.extlen = 20;
    packet.message.header.request.bodylen = htonl(20);
    packet.message.body.start_seqno = htonll(start_seqno);
    packet.message.body.end_seqno = htonll(end_seqno);
    packet.message.body.flags = htonl(flags);

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_mutation(const void* cookie,
                                              uint32_t opaque,
                                              item *it,
                                              uint16_t vbucket,
                                              uint64_t by_seqno,
                                              uint64_t rev_seqno,
                                              uint32_t lock_time,
                                              const void *meta,
                                              uint16_t nmeta,
                                              uint8_t nru)
{
    conn *c = (void*)cookie;
    item_info_holder info;
    protocol_binary_request_dcp_mutation packet;
    int xx;

    if (c->wbytes + sizeof(packet.bytes) + nmeta >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(&info, 0, sizeof(info));
    info.info.nvalue = IOV_MAX;

    if (!settings.engine.v1->get_item_info(settings.engine.v0, c, it,
                                           (void*)&info)) {
        settings.engine.v1->release(settings.engine.v0, c, it);
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "%d: Failed to get item info\n", c->sfd);
        return ENGINE_FAILED;
    }

    memset(packet.bytes, 0, sizeof(packet));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_MUTATION;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.header.request.cas = htonll(info.info.cas);
    packet.message.header.request.keylen = htons(info.info.nkey);
    packet.message.header.request.extlen = 31;
    packet.message.header.request.bodylen = ntohl(31 + info.info.nkey + info.info.nbytes + nmeta);
    packet.message.header.request.datatype = info.info.datatype;
    packet.message.body.by_seqno = htonll(by_seqno);
    packet.message.body.rev_seqno = htonll(rev_seqno);
    packet.message.body.lock_time = htonl(lock_time);
    packet.message.body.flags = info.info.flags;
    packet.message.body.expiration = htonl(info.info.exptime);
    packet.message.body.nmeta = htons(nmeta);
    packet.message.body.nru = nru;

    c->ilist[c->ileft++] = it;

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);
    add_iov(c, info.info.key, info.info.nkey);
    for (xx = 0; xx < info.info.nvalue; ++xx) {
        add_iov(c, info.info.value[xx].iov_base, info.info.value[xx].iov_len);
    }

    memcpy(c->wcurr, meta, nmeta);
    add_iov(c, c->wcurr, nmeta);
    c->wcurr += nmeta;
    c->wbytes += nmeta;

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_deletion(const void* cookie,
                                              uint32_t opaque,
                                              const void *key,
                                              uint16_t nkey,
                                              uint64_t cas,
                                              uint16_t vbucket,
                                              uint64_t by_seqno,
                                              uint64_t rev_seqno,
                                              const void *meta,
                                              uint16_t nmeta)
{
    conn *c = (void*)cookie;
    protocol_binary_request_dcp_deletion packet;
    if (c->wbytes + sizeof(packet.bytes) + nkey + nmeta >= c->wsize) {
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_DELETION;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.header.request.cas = htonll(cas);
    packet.message.header.request.keylen = htons(nkey);
    packet.message.header.request.extlen = 18;
    packet.message.header.request.bodylen = ntohl(18 + nkey + nmeta);
    packet.message.body.by_seqno = htonll(by_seqno);
    packet.message.body.rev_seqno = htonll(rev_seqno);
    packet.message.body.nmeta = htons(nmeta);

    add_iov(c, c->wcurr, sizeof(packet.bytes) + nkey + nmeta);
    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);
    memcpy(c->wcurr, key, nkey);
    c->wcurr += nkey;
    c->wbytes += nkey;
    memcpy(c->wcurr, meta, nmeta);
    c->wcurr += nmeta;
    c->wbytes += nmeta;

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_expiration(const void* cookie,
                                                uint32_t opaque,
                                                const void *key,
                                                uint16_t nkey,
                                                uint64_t cas,
                                                uint16_t vbucket,
                                                uint64_t by_seqno,
                                                uint64_t rev_seqno,
                                                const void *meta,
                                                uint16_t nmeta)
{
    conn *c = (void*)cookie;
    protocol_binary_request_dcp_deletion packet;

    if (c->wbytes + sizeof(packet.bytes) + nkey + nmeta >= c->wsize) {
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_EXPIRATION;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.header.request.cas = htonll(cas);
    packet.message.header.request.keylen = htons(nkey);
    packet.message.header.request.extlen = 18;
    packet.message.header.request.bodylen = ntohl(18 + nkey + nmeta);
    packet.message.body.by_seqno = htonll(by_seqno);
    packet.message.body.rev_seqno = htonll(rev_seqno);
    packet.message.body.nmeta = htons(nmeta);

    add_iov(c, c->wcurr, sizeof(packet.bytes) + nkey + nmeta);
    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);
    memcpy(c->wcurr, key, nkey);
    c->wcurr += nkey;
    c->wbytes += nkey;
    memcpy(c->wcurr, meta, nmeta);
    c->wcurr += nmeta;
    c->wbytes += nmeta;

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_flush(const void* cookie,
                                           uint32_t opaque,
                                           uint16_t vbucket)
{
    protocol_binary_request_dcp_flush packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_FLUSH;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_set_vbucket_state(const void* cookie,
                                                       uint32_t opaque,
                                                       uint16_t vbucket,
                                                       vbucket_state_t state)
{
    protocol_binary_request_dcp_set_vbucket_state packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE;
    packet.message.header.request.extlen = 1;
    packet.message.header.request.bodylen = htonl(1);
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);

    switch (state) {
    case vbucket_state_active:
        packet.message.body.state = 0x01;
        break;
    case vbucket_state_pending:
        packet.message.body.state = 0x02;
        break;
    case vbucket_state_replica:
        packet.message.body.state = 0x03;
        break;
    case vbucket_state_dead:
        packet.message.body.state = 0x04;
        break;
    default:
        return ENGINE_EINVAL;
    }

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_noop(const void* cookie,
                                          uint32_t opaque)
{
    protocol_binary_request_dcp_noop packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_NOOP;
    packet.message.header.request.opaque = opaque;

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_buffer_acknowledgement(const void* cookie,
                                                            uint32_t opaque,
                                                            uint16_t vbucket,
                                                            uint32_t buffer_bytes)
{
    protocol_binary_request_dcp_buffer_acknowledgement packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT;
    packet.message.header.request.extlen = 4;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.header.request.bodylen = ntohl(4);
    packet.message.body.buffer_bytes = ntohl(buffer_bytes);

    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    add_iov(c, c->wcurr, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_control(const void* cookie,
                                             uint32_t opaque,
                                             const void *key,
                                             uint16_t nkey,
                                             const void *value,
                                             uint32_t nvalue)
{
    protocol_binary_request_dcp_control packet;
    conn *c = (void*)cookie;

    if (c->wbytes + sizeof(packet.bytes) + nkey + nvalue >= c->wsize) {
        /* We don't have room in the buffer */
        return ENGINE_E2BIG;
    }

    memset(packet.bytes, 0, sizeof(packet.bytes));
    packet.message.header.request.magic =  (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_CONTROL;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.keylen = ntohs(nkey);
    packet.message.header.request.bodylen = ntohl(nvalue + nkey);

    add_iov(c, c->wcurr, sizeof(packet.bytes) + nkey + nvalue);
    memcpy(c->wcurr, packet.bytes, sizeof(packet.bytes));
    c->wcurr += sizeof(packet.bytes);
    c->wbytes += sizeof(packet.bytes);

    memcpy(c->wcurr, key, nkey);
    c->wcurr += nkey;
    c->wbytes += nkey;

    memcpy(c->wcurr, value, nvalue);
    c->wcurr += nvalue;
    c->wbytes += nvalue;

    return ENGINE_SUCCESS;
}

static void ship_dcp_log(conn *c) {
    static struct dcp_message_producers producers = {
        dcp_message_get_failover_log,
        dcp_message_stream_req,
        dcp_message_add_stream_response,
        dcp_message_marker_response,
        dcp_message_set_vbucket_state_response,
        dcp_message_stream_end,
        dcp_message_marker,
        dcp_message_mutation,
        dcp_message_deletion,
        dcp_message_expiration,
        dcp_message_flush,
        dcp_message_set_vbucket_state,
        dcp_message_noop,
        dcp_message_buffer_acknowledgement,
        dcp_message_control
    };
    ENGINE_ERROR_CODE ret;

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
        if (settings.verbose) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                            "%d: Failed to create output headers. Shutting down DCP connection\n", c->sfd);
        }
        conn_set_state(c, conn_closing);
        return ;
    }

    c->wbytes = 0;
    c->wcurr = c->wbuf;
    c->icurr = c->ilist;

    c->ewouldblock = false;
    ret = settings.engine.v1->dcp.step(settings.engine.v0, c, &producers);
    if (ret == ENGINE_SUCCESS) {
        /* the engine don't have more data to send at this moment */
        c->ewouldblock = true;
    } else if (ret == ENGINE_WANT_MORE) {
        /* The engine got more data it wants to send */
        ret = ENGINE_SUCCESS;
    }

    if (ret == ENGINE_SUCCESS) {
        conn_set_state(c, conn_mwrite);
        c->write_and_go = conn_ship_log;
    } else {
        conn_set_state(c, conn_closing);
    }
}

/******************************************************************************
 *                        TAP packet executors                                *
 ******************************************************************************/
static void tap_connect_executor(conn *c, void *packet)
{
    cb_mutex_enter(&tap_stats.mutex);
    tap_stats.received.connect++;
    cb_mutex_exit(&tap_stats.mutex);
    conn_set_state(c, conn_setup_tap_stream);
}

static void tap_mutation_executor(conn *c, void *packet)
{
    cb_mutex_enter(&tap_stats.mutex);
    tap_stats.received.mutation++;
    cb_mutex_exit(&tap_stats.mutex);
    process_bin_tap_packet(TAP_MUTATION, c);
}

static void tap_delete_executor(conn *c, void *packet)
{
    cb_mutex_enter(&tap_stats.mutex);
    tap_stats.received.delete++;
    cb_mutex_exit(&tap_stats.mutex);
    process_bin_tap_packet(TAP_DELETION, c);
}

static void tap_flush_executor(conn *c, void *packet)
{
    cb_mutex_enter(&tap_stats.mutex);
    tap_stats.received.flush++;
    cb_mutex_exit(&tap_stats.mutex);
    process_bin_tap_packet(TAP_FLUSH, c);
}

static void tap_opaque_executor(conn *c, void *packet)
{
    cb_mutex_enter(&tap_stats.mutex);
    tap_stats.received.opaque++;
    cb_mutex_exit(&tap_stats.mutex);
    process_bin_tap_packet(TAP_OPAQUE, c);
}

static void tap_vbucket_set_executor(conn *c, void *packet)
{
    cb_mutex_enter(&tap_stats.mutex);
    tap_stats.received.vbucket_set++;
    cb_mutex_exit(&tap_stats.mutex);
    process_bin_tap_packet(TAP_VBUCKET_SET, c);
}

static void tap_checkpoint_start_executor(conn *c, void *packet)
{
    cb_mutex_enter(&tap_stats.mutex);
    tap_stats.received.checkpoint_start++;
    cb_mutex_exit(&tap_stats.mutex);
    process_bin_tap_packet(TAP_CHECKPOINT_START, c);
}

static void tap_checkpoint_end_executor(conn *c, void *packet)
{
    cb_mutex_enter(&tap_stats.mutex);
    tap_stats.received.checkpoint_end++;
    cb_mutex_exit(&tap_stats.mutex);
    process_bin_tap_packet(TAP_CHECKPOINT_END, c);
}

/*******************************************************************************
 *                        DCP packet validators                                *
 ******************************************************************************/
static int dcp_open_validator(void *packet)
{
    protocol_binary_request_dcp_open *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 8 ||
        req->message.header.request.keylen == 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        /* INCORRECT FORMAT */
        return -1;
    }

    return 0;
}

static int dcp_add_stream_validator(void *packet)
{
    protocol_binary_request_dcp_add_stream *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != htonl(4) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        /* INCORRECT FORMAT */
        return -1;
    }

    return 0;
}

static int dcp_close_stream_validator(void *packet)
{
    protocol_binary_request_dcp_close_stream *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        /* INCORRECT FORMAT */
        return -1;
    }

    return 0;
}

static int dcp_get_failover_log_validator(void *packet)
{
    protocol_binary_request_dcp_get_failover_log *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int dcp_stream_req_validator(void *packet)
{
    protocol_binary_request_dcp_stream_req *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 5*sizeof(uint64_t) + 2*sizeof(uint32_t) ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        /* INCORRECT FORMAT */
        return -1;
    }
    return 0;
}

static int dcp_stream_end_validator(void *packet)
{
    protocol_binary_request_dcp_stream_end *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != htonl(4) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int dcp_snapshot_marker_validator(void *packet)
{
    protocol_binary_request_dcp_snapshot_marker *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 20 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != htonl(20) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int dcp_mutation_validator(void *packet)
{
    protocol_binary_request_dcp_mutation *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != (2*sizeof(uint64_t) + 3 * sizeof(uint32_t) + sizeof(uint16_t)) + sizeof(uint8_t) ||
        req->message.header.request.keylen == 0 ||
        req->message.header.request.bodylen == 0) {
        return -1;
    }

    return 0;
}

static int dcp_deletion_validator(void *packet)
{
    protocol_binary_request_dcp_deletion *req = packet;
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t bodylen = ntohl(req->message.header.request.bodylen) - klen;
    bodylen -= req->message.header.request.extlen;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != (2*sizeof(uint64_t) + sizeof(uint16_t)) ||
        req->message.header.request.keylen == 0 ||
        bodylen != 0) {
        return -1;
    }

    return 0;
}

static int dcp_expiration_validator(void *packet)
{
    protocol_binary_request_dcp_deletion *req = packet;
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t bodylen = ntohl(req->message.header.request.bodylen) - klen;
    bodylen -= req->message.header.request.extlen;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != (2*sizeof(uint64_t) + sizeof(uint16_t)) ||
        req->message.header.request.keylen == 0 ||
        bodylen != 0) {
        return -1;
    }

    return 0;
}

static int dcp_flush_validator(void *packet)
{
    protocol_binary_request_dcp_flush *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int dcp_set_vbucket_state_validator(void *packet)
{
    protocol_binary_request_dcp_set_vbucket_state *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 1 ||
        req->message.header.request.keylen != 0 ||
        ntohl(req->message.header.request.bodylen) != 1 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    if (req->message.body.state < 1 || req->message.body.state > 4) {
        return -1;
    }

    return 0;
}

static int dcp_noop_validator(void *packet)
{
    protocol_binary_request_dcp_noop *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int dcp_buffer_acknowledgement_validator(void *packet)
{
    protocol_binary_request_dcp_buffer_acknowledgement *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != ntohl(4) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int dcp_control_validator(void *packet)
{
    protocol_binary_request_dcp_control *req = packet;
    uint16_t nkey = ntohs(req->message.header.request.keylen);
    uint32_t nval = ntohl(req->message.header.request.bodylen) - nkey;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 || nkey == 0 || nval == 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int isasl_refresh_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int ssl_certs_refresh_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int verbosity_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 4 ||
        req->message.header.request.keylen != 0 ||
        ntohl(req->message.header.request.bodylen) != 4 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int hello_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    uint32_t len = ntohl(req->message.header.request.bodylen);
    len -= ntohs(req->message.header.request.keylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 || (len % 2) != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int version_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int quit_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int sasl_list_mech_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int noop_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int flush_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    uint8_t extlen = req->message.header.request.extlen;
    uint32_t bodylen = ntohl(req->message.header.request.bodylen);

    if (extlen != 0 && extlen != 4) {
        return -1;
    }

    if (bodylen != extlen) {
        return -1;
    }

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int get_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        klen == 0 || klen != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int delete_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        klen == 0 || klen != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int stat_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 || klen != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int arithmetic_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);
    uint8_t extlen = req->message.header.request.extlen;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        extlen != 20 || klen == 0 || (klen + extlen) != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int get_cmd_timer_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t blen = ntohl(req->message.header.request.bodylen);
    uint8_t extlen = req->message.header.request.extlen;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        extlen != 1 || klen != 0 || (klen + extlen) != blen ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int set_ctrl_token_validator(void *packet)
{
    protocol_binary_request_set_ctrl_token *req = packet;

    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != sizeof(uint64_t) ||
        req->message.header.request.keylen != 0 ||
        ntohl(req->message.header.request.bodylen) != sizeof(uint64_t) ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES ||
        req->message.body.new_cas == 0) {
        return -1;
    }

    return 0;
}

static int get_ctrl_token_validator(void *packet)
{
    protocol_binary_request_no_extras *req = packet;
    if (req->message.header.request.magic != PROTOCOL_BINARY_REQ ||
        req->message.header.request.extlen != 0 ||
        req->message.header.request.keylen != 0 ||
        req->message.header.request.bodylen != 0 ||
        req->message.header.request.datatype != PROTOCOL_BINARY_RAW_BYTES) {
        return -1;
    }

    return 0;
}

static int tap_validator(void *packet)
{
    protocol_binary_request_tap_no_extras *req = packet;
    uint32_t bodylen = ntohl(req->message.header.request.bodylen);
    uint16_t enginelen = ntohs(req->message.body.tap.enginespecific_length);
    if (enginelen > bodylen) {
        return -1;
    }
    return 0;
}

/*******************************************************************************
 *                         DCP packet executors                                *
 ******************************************************************************/
static void dcp_open_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_open *req = (void*)packet;

    if (settings.engine.v1->dcp.open == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;
        c->supports_datatype = true;

        if (ret == ENGINE_SUCCESS) {
            ret = settings.engine.v1->dcp.open(settings.engine.v0, c,
                                               req->message.header.request.opaque,
                                               ntohl(req->message.body.seqno),
                                               ntohl(req->message.body.flags),
                                               (void*)(req->bytes + sizeof(req->bytes)),
                                               ntohs(req->message.header.request.keylen));
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS, 0);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_add_stream_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_add_stream *req = (void*)packet;

    if (settings.engine.v1->dcp.add_stream == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            ret = settings.engine.v1->dcp.add_stream(settings.engine.v0, c,
                                                     req->message.header.request.opaque,
                                                     ntohs(req->message.header.request.vbucket),
                                                     ntohl(req->message.body.flags));
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            c->dcp = 1;
            conn_set_state(c, conn_ship_log);
            break;
        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_close_stream_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_close_stream *req = (void*)packet;

    if (settings.engine.v1->dcp.close_stream == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            uint16_t vbucket = ntohs(req->message.header.request.vbucket);
            uint32_t opaque = ntohl(req->message.header.request.opaque);
            ret = settings.engine.v1->dcp.close_stream(settings.engine.v0, c,
                                                       opaque, vbucket);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS, 0);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

/** Callback from the engine adding the response */
static ENGINE_ERROR_CODE add_failover_log(vbucket_failover_t*entries,
                                          size_t nentries,
                                          const void *cookie)
{
    ENGINE_ERROR_CODE ret;
    size_t ii;
    for (ii = 0; ii < nentries; ++ii) {
        entries[ii].uuid = htonll(entries[ii].uuid);
        entries[ii].seqno = htonll(entries[ii].seqno);
    }

    if (binary_response_handler(NULL, 0, NULL, 0, entries,
                                (uint32_t)(nentries * sizeof(vbucket_failover_t)), 0,
                                PROTOCOL_BINARY_RESPONSE_SUCCESS, 0,
                                (void*)cookie)) {
        ret = ENGINE_SUCCESS;
    } else {
        ret = ENGINE_ENOMEM;
    }

    for (ii = 0; ii < nentries; ++ii) {
        entries[ii].uuid = htonll(entries[ii].uuid);
        entries[ii].seqno = htonll(entries[ii].seqno);
    }

    return ret;
}

static void dcp_get_failover_log_executor(conn *c, void *packet) {
    protocol_binary_request_dcp_get_failover_log *req = (void*)packet;

    if (settings.engine.v1->dcp.get_failover_log == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            ret = settings.engine.v1->dcp.get_failover_log(settings.engine.v0, c,
                                                           req->message.header.request.opaque,
                                                           ntohs(req->message.header.request.vbucket),
                                                           add_failover_log);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            if (c->dynamic_buffer.buffer != NULL) {
                write_and_free(c, c->dynamic_buffer.buffer,
                               c->dynamic_buffer.offset);
                c->dynamic_buffer.buffer = NULL;
            } else {
                write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS, 0);
            }
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_stream_req_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_stream_req *req = (void*)packet;

    if (settings.engine.v1->dcp.stream_req == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        uint32_t flags = ntohl(req->message.body.flags);
        uint64_t start_seqno = ntohll(req->message.body.start_seqno);
        uint64_t end_seqno = ntohll(req->message.body.end_seqno);
        uint64_t vbucket_uuid = ntohll(req->message.body.vbucket_uuid);
        uint64_t snap_start_seqno = ntohll(req->message.body.snap_start_seqno);
        uint64_t snap_end_seqno = ntohll(req->message.body.snap_end_seqno);
        uint64_t rollback_seqno;

        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        cb_assert(ret != ENGINE_ROLLBACK);

        if (ret == ENGINE_SUCCESS) {
            ret = settings.engine.v1->dcp.stream_req(settings.engine.v0, c,
                                                     flags,
                                                     c->binary_header.request.opaque,
                                                     c->binary_header.request.vbucket,
                                                     start_seqno, end_seqno,
                                                     vbucket_uuid,
                                                     snap_start_seqno,
                                                     snap_end_seqno,
                                                     &rollback_seqno,
                                                     add_failover_log);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            c->dcp = 1;
            c->max_reqs_per_event = settings.reqs_per_event_med_priority;
            if (c->dynamic_buffer.buffer != NULL) {
                write_and_free(c, c->dynamic_buffer.buffer,
                               c->dynamic_buffer.offset);
                c->dynamic_buffer.buffer = NULL;
            } else {
                write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS, 0);
            }
            break;

        case ENGINE_ROLLBACK:
            rollback_seqno = htonll(rollback_seqno);
            if (binary_response_handler(NULL, 0, NULL, 0, &rollback_seqno,
                                        sizeof(rollback_seqno), 0,
                                        PROTOCOL_BINARY_RESPONSE_ROLLBACK, 0,
                                        c)) {
                write_and_free(c, c->dynamic_buffer.buffer,
                               c->dynamic_buffer.offset);
                c->dynamic_buffer.buffer = NULL;
            } else {
                write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, 0);
            }
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_stream_end_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_stream_end *req = (void*)packet;

    if (settings.engine.v1->dcp.stream_end == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            ret = settings.engine.v1->dcp.stream_end(settings.engine.v0, c,
                                                     req->message.header.request.opaque,
                                                     ntohs(req->message.header.request.vbucket),
                                                     ntohl(req->message.body.flags));
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            conn_set_state(c, conn_ship_log);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_snapshot_marker_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_snapshot_marker *req = (void*)packet;

    if (settings.engine.v1->dcp.snapshot_marker == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        uint16_t vbucket = ntohs(req->message.header.request.vbucket);
        uint32_t opaque = req->message.header.request.opaque;
        uint32_t flags = ntohl(req->message.body.flags);
        uint64_t start_seqno = ntohll(req->message.body.start_seqno);
        uint64_t end_seqno = ntohll(req->message.body.end_seqno);

        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            ret = settings.engine.v1->dcp.snapshot_marker(settings.engine.v0, c,
                                                          opaque, vbucket,
                                                          start_seqno,
                                                          end_seqno, flags);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            conn_set_state(c, conn_ship_log);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_mutation_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_mutation *req = (void*)packet;

    if (settings.engine.v1->dcp.mutation == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            char *key = (char*)packet + sizeof(req->bytes);
            uint16_t nkey = ntohs(req->message.header.request.keylen);
            void *value = key + nkey;
            uint64_t cas = ntohll(req->message.header.request.cas);
            uint16_t vbucket = ntohs(req->message.header.request.vbucket);
            uint32_t flags = req->message.body.flags;
            uint8_t datatype = req->message.header.request.datatype;
            uint64_t by_seqno = ntohll(req->message.body.by_seqno);
            uint64_t rev_seqno = ntohll(req->message.body.rev_seqno);
            uint32_t expiration = ntohl(req->message.body.expiration);
            uint32_t lock_time = ntohl(req->message.body.lock_time);
            uint16_t nmeta = ntohs(req->message.body.nmeta);
            uint32_t nvalue = ntohl(req->message.header.request.bodylen) - nkey
                - req->message.header.request.extlen - nmeta;

            ret = settings.engine.v1->dcp.mutation(settings.engine.v0, c,
                                                   req->message.header.request.opaque,
                                                   key, nkey, value, nvalue, cas, vbucket,
                                                   flags, datatype, by_seqno, rev_seqno,
                                                   expiration, lock_time,
                                                   (char*)value + nvalue, nmeta,
                                                   req->message.body.nru);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            conn_set_state(c, conn_new_cmd);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_deletion_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_deletion *req = (void*)packet;

    if (settings.engine.v1->dcp.deletion == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            char *key = (char*)packet + sizeof(req->bytes);
            uint16_t nkey = ntohs(req->message.header.request.keylen);
            uint64_t cas = ntohll(req->message.header.request.cas);
            uint16_t vbucket = ntohs(req->message.header.request.vbucket);
            uint64_t by_seqno = ntohll(req->message.body.by_seqno);
            uint64_t rev_seqno = ntohll(req->message.body.rev_seqno);
            uint16_t nmeta = ntohs(req->message.body.nmeta);

            ret = settings.engine.v1->dcp.deletion(settings.engine.v0, c,
                                                   req->message.header.request.opaque,
                                                   key, nkey, cas, vbucket,
                                                   by_seqno, rev_seqno, key + nkey, nmeta);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            conn_set_state(c, conn_new_cmd);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_expiration_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_expiration *req = (void*)packet;

    if (settings.engine.v1->dcp.expiration == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            char *key = (char*)packet + sizeof(req->bytes);
            uint16_t nkey = ntohs(req->message.header.request.keylen);
            uint64_t cas = ntohll(req->message.header.request.cas);
            uint16_t vbucket = ntohs(req->message.header.request.vbucket);
            uint64_t by_seqno = ntohll(req->message.body.by_seqno);
            uint64_t rev_seqno = ntohll(req->message.body.rev_seqno);
            uint16_t nmeta = ntohs(req->message.body.nmeta);

            ret = settings.engine.v1->dcp.expiration(settings.engine.v0, c,
                                                     req->message.header.request.opaque,
                                                     key, nkey, cas, vbucket,
                                                     by_seqno, rev_seqno, key + nkey, nmeta);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            conn_set_state(c, conn_new_cmd);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_flush_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_flush *req = (void*)packet;

    if (settings.engine.v1->dcp.flush == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            ret = settings.engine.v1->dcp.flush(settings.engine.v0, c,
                                                req->message.header.request.opaque,
                                                ntohs(req->message.header.request.vbucket));
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            conn_set_state(c, conn_new_cmd);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_set_vbucket_state_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_set_vbucket_state *req = (void*)packet;

    if (settings.engine.v1->dcp.set_vbucket_state== NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            vbucket_state_t state = (vbucket_state_t)req->message.body.state;
            ret = settings.engine.v1->dcp.set_vbucket_state(settings.engine.v0, c,
                                                            c->binary_header.request.opaque,
                                                            c->binary_header.request.vbucket,
                                                            state);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            conn_set_state(c, conn_ship_log);
            break;
        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            conn_set_state(c, conn_closing);
            break;
        }
    }
}

static void dcp_noop_executor(conn *c, void *packet)
{
    if (settings.engine.v1->dcp.noop == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            ret = settings.engine.v1->dcp.noop(settings.engine.v0, c,
                                               c->binary_header.request.opaque);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS, 0);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_buffer_acknowledgement_executor(conn *c, void *packet)
{
    protocol_binary_request_dcp_buffer_acknowledgement *req = (void*)packet;

    if (settings.engine.v1->dcp.buffer_acknowledgement == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            uint32_t bbytes;
            memcpy(&bbytes, &req->message.body.buffer_bytes, 4);
            ret = settings.engine.v1->dcp.buffer_acknowledgement(settings.engine.v0, c,
                                                                 c->binary_header.request.opaque,
                                                                 c->binary_header.request.vbucket,
                                                                 ntohl(bbytes));
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            conn_set_state(c, conn_new_cmd);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void dcp_control_executor(conn *c, void *packet)
{
    if (settings.engine.v1->dcp.control == NULL) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        ENGINE_ERROR_CODE ret = c->aiostat;
        c->aiostat = ENGINE_SUCCESS;
        c->ewouldblock = false;

        if (ret == ENGINE_SUCCESS) {
            protocol_binary_request_dcp_control *req = (void*)packet;
            const uint8_t *key = req->bytes + sizeof(req->bytes);
            uint16_t nkey = ntohs(req->message.header.request.keylen);
            const uint8_t *value = key + nkey;
            uint32_t nvalue = ntohl(req->message.header.request.bodylen) - nkey;
            ret = settings.engine.v1->dcp.control(settings.engine.v0, c,
                                                  c->binary_header.request.opaque,
                                                  key, nkey, value, nvalue);
        }

        switch (ret) {
        case ENGINE_SUCCESS:
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS, 0);
            break;

        case ENGINE_DISCONNECT:
            conn_set_state(c, conn_closing);
            break;

        case ENGINE_EWOULDBLOCK:
            c->ewouldblock = true;
            break;

        default:
            write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
        }
    }
}

static void isasl_refresh_executor(conn *c, void *packet)
{
    ENGINE_ERROR_CODE ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;

    if (ret == ENGINE_SUCCESS) {
        ret = refresh_cbsasl(c);
    }

    switch (ret) {
    case ENGINE_SUCCESS:
        write_bin_response(c, NULL, 0, 0, 0);
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        conn_set_state(c, conn_refresh_cbsasl);
        break;
    case ENGINE_DISCONNECT:
        conn_set_state(c, conn_closing);
        break;
    default:
        write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
    }
}

static void ssl_certs_refresh_executor(conn *c, void *packet)
{
    ENGINE_ERROR_CODE ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;

    if (ret == ENGINE_SUCCESS) {
        ret = refresh_ssl_certs(c);
    }

    switch (ret) {
    case ENGINE_SUCCESS:
        write_bin_response(c, NULL, 0, 0, 0);
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        conn_set_state(c, conn_refresh_ssl_certs);
        break;
    case ENGINE_DISCONNECT:
        conn_set_state(c, conn_closing);
        break;
    default:
        write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
    }
}

static void verbosity_executor(conn *c, void *packet)
{
    protocol_binary_request_verbosity *req = packet;
    uint32_t level = (uint32_t)ntohl(req->message.body.level);
    if (level > MAX_VERBOSITY_LEVEL) {
        level = MAX_VERBOSITY_LEVEL;
    }
    settings.verbose = (int)level;
    perform_callbacks(ON_LOG_LEVEL, NULL, NULL);
    write_bin_response(c, NULL, 0, 0, 0);
}

static void process_hello_packet_executor(conn *c, void *packet) {
    protocol_binary_request_hello *req = packet;
    char log_buffer[512];
    int offset = snprintf(log_buffer, sizeof(log_buffer), "HELO ");
    char *key = (char*)packet + sizeof(*req);
    uint16_t klen = ntohs(req->message.header.request.keylen);
    uint32_t total = (ntohl(req->message.header.request.bodylen) - klen) / 2;
    uint32_t ii;
    char *curr = key + klen;
    uint16_t out[2]; /* We're currently only supporting two features */
    int jj = 0;
#if 0
    int added_tls = 0;
#endif
    memset((char*)out, 0, sizeof(out));

    /*
     * Disable all features the hello packet may enable, so that
     * the client can toggle features on/off during a connection
     */
    c->supports_datatype = false;

    if (klen) {
        if (klen > 256) {
            klen = 256;
        }
        log_buffer[offset++] = '[';
        memcpy(log_buffer + offset, key, klen);
        offset += klen;
        log_buffer[offset++] = ']';
        log_buffer[offset++] = ' ';
    }

    for (ii = 0; ii < total; ++ii) {
        uint16_t in;
        /* to avoid alignment */
        memcpy(&in, curr, 2);
        curr += 2;
        switch (ntohs(in)) {
        case PROTOCOL_BINARY_FEATURE_TLS:
#if 0
            /* Not implemented */
            if (added_tls == 0) {
                out[jj++] = htons(PROTOCOL_BINARY_FEATURE_TLS);
                added_sls++;
            }
#endif

            break;
        case PROTOCOL_BINARY_FEATURE_DATATYPE:
            if (settings.datatype && !c->supports_datatype) {
                offset += snprintf(log_buffer + offset,
                                   sizeof(log_buffer) - offset,
                                   "datatype ");
                out[jj++] = htons(PROTOCOL_BINARY_FEATURE_DATATYPE);
                c->supports_datatype = true;
            }
            break;
        }
    }

    if (jj == 0) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS, 0);
    } else {
        binary_response_handler(NULL, 0, NULL, 0, out, 2 * jj,
                                PROTOCOL_BINARY_RAW_BYTES,
                                PROTOCOL_BINARY_RESPONSE_SUCCESS,
                                0, c);
        write_and_free(c, c->dynamic_buffer.buffer,
                       c->dynamic_buffer.offset);
        c->dynamic_buffer.buffer = NULL;
    }

    log_buffer[offset++] = '\0';
    settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                    "%d: %s", c->sfd, log_buffer);
}

static void version_executor(conn *c, void *packet)
{
    write_bin_response(c, get_server_version(), 0, 0,
                       (uint32_t)strlen(get_server_version()));
}

static void quit_executor(conn *c, void *packet)
{
    write_bin_response(c, NULL, 0, 0, 0);
    c->write_and_go = conn_closing;
}

static void quitq_executor(conn *c, void *packet)
{
    conn_set_state(c, conn_closing);
}

static void sasl_list_mech_executor(conn *c, void *packet)
{
    const char *result_string = NULL;
    unsigned int string_length = 0;

    if (cbsasl_list_mechs(&result_string, &string_length) != SASL_OK) {
        /* Perhaps there's a better error for this... */
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "%d: Failed to list SASL mechanisms.\n",
                                        c->sfd);
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, 0);
        return;
    }
    write_bin_response(c, (char*)result_string, 0, 0, string_length);
}

static void noop_executor(conn *c, void *packet)
{
    write_bin_response(c, NULL, 0, 0, 0);
}

static void flush_executor(conn *c, void *packet)
{
    ENGINE_ERROR_CODE ret;
    time_t exptime = 0;
    protocol_binary_request_flush* req = packet;

    if (c->cmd == PROTOCOL_BINARY_CMD_FLUSHQ) {
        c->noreply = true;
    }

    if (c->binary_header.request.extlen == sizeof(req->message.body)) {
        exptime = ntohl(req->message.body.expiration);
    }

    if (settings.verbose > 1) {
        settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                        "%d: flush %ld", c->sfd,
                                        (long)exptime);
    }

    ret = settings.engine.v1->flush(settings.engine.v0, c, exptime);

    if (ret == ENGINE_SUCCESS) {
        write_bin_response(c, NULL, 0, 0, 0);
    } else if (ret == ENGINE_ENOTSUP) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
    } else {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
    }
    STATS_NOKEY(c, cmd_flush);
}

static void get_executor(conn *c, void *packet)
{
    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_GETQ:
        c->cmd = PROTOCOL_BINARY_CMD_GET;
        c->noreply = true;
        break;
    case PROTOCOL_BINARY_CMD_GET:
        c->noreply = false;
        break;
    case PROTOCOL_BINARY_CMD_GETKQ:
        c->cmd = PROTOCOL_BINARY_CMD_GETK;
        c->noreply = true;
        break;
    case PROTOCOL_BINARY_CMD_GETK:
        c->noreply = false;
        break;
    default:
        abort();
    }

    process_bin_get(c);
}

static void process_bin_delete(conn *c);
static void delete_executor(conn *c, void *packet)
{
    if (c->cmd == PROTOCOL_BINARY_CMD_DELETEQ) {
        c->noreply = true;
    }

    process_bin_delete(c);
}

static void stat_executor(conn *c, void *packet)
{
    char *subcommand = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;
    ENGINE_ERROR_CODE ret;

    if (settings.verbose > 1) {
        char buffer[1024];
        if (key_to_printable_buffer(buffer, sizeof(buffer), c->sfd, true,
                                    "STATS", subcommand, nkey) != -1) {
            settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c, "%s\n",
                                            buffer);
        }
    }

    ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;

    if (ret == ENGINE_SUCCESS) {
        if (nkey == 0) {
            /* request all statistics */
            ret = settings.engine.v1->get_stats(settings.engine.v0, c, NULL, 0, append_stats);
            if (ret == ENGINE_SUCCESS) {
                server_stats(&append_stats, c, false);
            }
        } else if (strncmp(subcommand, "reset", 5) == 0) {
            stats_reset(c);
            settings.engine.v1->reset_stats(settings.engine.v0, c);
        } else if (strncmp(subcommand, "settings", 8) == 0) {
            process_stat_settings(&append_stats, c);
        } else if (strncmp(subcommand, "cachedump", 9) == 0) {
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
            return;
        } else if (strncmp(subcommand, "detail", 6) == 0) {
            char *subcmd_pos = subcommand + 6;
            if (settings.allow_detailed) {
                if (strncmp(subcmd_pos, " dump", 5) == 0) {
                    int len;
                    char *dump_buf = stats_prefix_dump(&len);
                    if (dump_buf == NULL || len <= 0) {
                        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, 0);
                        return ;
                    } else {
                        append_stats("detailed", (uint16_t)strlen("detailed"), dump_buf, len, c);
                        free(dump_buf);
                    }
                } else if (strncmp(subcmd_pos, " on", 3) == 0) {
                    settings.detail_enabled = 1;
                } else if (strncmp(subcmd_pos, " off", 4) == 0) {
                    settings.detail_enabled = 0;
                } else {
                    write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, 0);
                    return;
                }
            } else {
                write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, 0);
                return;
            }
        } else if (strncmp(subcommand, "aggregate", 9) == 0) {
            server_stats(&append_stats, c, true);
        } else if (strncmp(subcommand, "connections", 11) == 0) {
            connection_stats(&append_stats, c);
        } else {
            ret = settings.engine.v1->get_stats(settings.engine.v0, c,
                                                subcommand, (int)nkey,
                                                append_stats);
        }
    }

    switch (ret) {
    case ENGINE_SUCCESS:
        append_stats(NULL, 0, NULL, 0, c);
        write_and_free(c, c->dynamic_buffer.buffer, c->dynamic_buffer.offset);
        c->dynamic_buffer.buffer = NULL;
        break;
    case ENGINE_ENOMEM:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, 0);
        break;
    case ENGINE_TMPFAIL:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ETMPFAIL, 0);
        break;
    case ENGINE_KEY_ENOENT:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, 0);
        break;
    case ENGINE_NOT_MY_VBUCKET:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET, 0);
        break;
    case ENGINE_DISCONNECT:
        c->state = conn_closing;
        break;
    case ENGINE_ENOTSUP:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        break;
    default:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
    }
}

static void arithmetic_executor(conn *c, void *packet)
{
    protocol_binary_response_incr* rsp = (protocol_binary_response_incr*)c->wbuf;
    protocol_binary_request_incr* req = binary_get_request(c);
    ENGINE_ERROR_CODE ret;
    uint64_t delta;
    uint64_t initial;
    rel_time_t expiration;
    char *key;
    size_t nkey;
    bool incr;

    cb_assert(c != NULL);
    cb_assert(c->wsize >= sizeof(*rsp));


    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_INCREMENTQ:
        c->cmd = PROTOCOL_BINARY_CMD_INCREMENT;
        c->noreply = true;
        break;
    case PROTOCOL_BINARY_CMD_INCREMENT:
        c->noreply = false;
        break;
    case PROTOCOL_BINARY_CMD_DECREMENTQ:
        c->cmd = PROTOCOL_BINARY_CMD_DECREMENT;
        c->noreply = true;
        break;
    case PROTOCOL_BINARY_CMD_DECREMENT:
        c->noreply = false;
        break;
    default:
        abort();
    }

    if (req->message.header.request.cas != 0) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
        return;
    }

    /* fix byteorder in the request */
    delta = ntohll(req->message.body.delta);
    initial = ntohll(req->message.body.initial);
    expiration = ntohl(req->message.body.expiration);
    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;
    incr = (c->cmd == PROTOCOL_BINARY_CMD_INCREMENT ||
            c->cmd == PROTOCOL_BINARY_CMD_INCREMENTQ);

    if (settings.verbose > 1) {
        char buffer[1024];
        ssize_t nw;
        nw = key_to_printable_buffer(buffer, sizeof(buffer), c->sfd, true,
                                     incr ? "INCR" : "DECR", key, nkey);
        if (nw != -1) {
            if (snprintf(buffer + nw, sizeof(buffer) - nw,
                         " %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
                         delta, initial, (uint64_t)expiration) != -1) {
                settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c, "%s",
                                                buffer);
            }
        }
    }

    ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    if (ret == ENGINE_SUCCESS) {
        ret = settings.engine.v1->arithmetic(settings.engine.v0,
                                             c, key, (int)nkey, incr,
                                             req->message.body.expiration != 0xffffffff,
                                             delta, initial, expiration,
                                             &c->cas,
                                             c->binary_header.request.datatype,
                                             &rsp->message.body.value,
                                             c->binary_header.request.vbucket);
    }

    switch (ret) {
    case ENGINE_SUCCESS:
        rsp->message.body.value = htonll(rsp->message.body.value);
        write_bin_response(c, &rsp->message.body, 0, 0,
                           sizeof (rsp->message.body.value));
        if (incr) {
            STATS_INCR(c, incr_hits, key, nkey);
        } else {
            STATS_INCR(c, decr_hits, key, nkey);
        }
        break;
    case ENGINE_KEY_EEXISTS:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, 0);
        break;
    case ENGINE_KEY_ENOENT:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, 0);
        if (c->cmd == PROTOCOL_BINARY_CMD_INCREMENT) {
            STATS_INCR(c, incr_misses, key, nkey);
        } else {
            STATS_INCR(c, decr_misses, key, nkey);
        }
        break;
    case ENGINE_ENOMEM:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, 0);
        break;
    case ENGINE_TMPFAIL:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ETMPFAIL, 0);
        break;
    case ENGINE_EINVAL:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL, 0);
        break;
    case ENGINE_NOT_STORED:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_STORED, 0);
        break;
    case ENGINE_DISCONNECT:
        c->state = conn_closing;
        break;
    case ENGINE_ENOTSUP:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
        break;
    case ENGINE_NOT_MY_VBUCKET:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET, 0);
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        break;
    default:
        abort();
    }
}

static void get_cmd_timer_executor(conn *c, void *packet)
{
    protocol_binary_request_get_cmd_timer *req = packet;

    generate_timings(req->message.body.opcode, c);
    write_and_free(c, c->dynamic_buffer.buffer, c->dynamic_buffer.offset);
    c->dynamic_buffer.buffer = NULL;
}

static void set_ctrl_token_executor(conn *c, void *packet)
{
    protocol_binary_request_set_ctrl_token *req = packet;
    uint64_t old_cas = ntohll(req->message.header.request.cas);
    uint16_t ret = PROTOCOL_BINARY_RESPONSE_SUCCESS;

    if (cookie_is_admin(c)) {
        cb_mutex_enter(&(session_cas.mutex));
        if (session_cas.ctr > 0) {
            ret = PROTOCOL_BINARY_RESPONSE_EBUSY;
        } else {
            if (old_cas == session_cas.value || old_cas == 0) {
                session_cas.value = ntohll(req->message.body.new_cas);
            } else {
                ret = PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
            }
        }

        binary_response_handler(NULL, 0, NULL, 0, NULL, 0,
                                PROTOCOL_BINARY_RAW_BYTES,
                                ret, session_cas.value, c);
        cb_mutex_exit(&(session_cas.mutex));

        write_and_free(c, c->dynamic_buffer.buffer, c->dynamic_buffer.offset);
        c->dynamic_buffer.buffer = NULL;
    } else {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EACCESS, 0);
    }
}

static void get_ctrl_token_executor(conn *c, void *packet)
{
    if (cookie_is_admin(c)) {
        cb_mutex_enter(&(session_cas.mutex));
        binary_response_handler(NULL, 0, NULL, 0, NULL, 0,
                                PROTOCOL_BINARY_RAW_BYTES,
                                PROTOCOL_BINARY_RESPONSE_SUCCESS,
                                session_cas.value, c);
        cb_mutex_exit(&(session_cas.mutex));
        write_and_free(c, c->dynamic_buffer.buffer, c->dynamic_buffer.offset);
        c->dynamic_buffer.buffer = NULL;
    } else {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EACCESS, 0);
    }
}

static void ioctl_get_executor(conn *c, void *packet)
{
    /* Currently no ioctl GET subcommands supported. */
    write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
}

static void ioctl_set_executor(conn *c, void *packet)
{
    protocol_binary_request_ioctl_set *req = packet;

    size_t keylen = ntohs(req->message.header.request.keylen);

    if (keylen == 0 || keylen > KEY_MAX_LENGTH) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
        return;
    }

    const char* key = (const char*)(req->bytes + sizeof(req->bytes));
    const char* value = key + keylen;
    (void)value; /* Value currently unused. */

    if (strncmp("release_free_memory", key, keylen) == 0 &&
        keylen == strlen("release_free_memory")) {
        mc_release_free_memory();
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                "%d: IOCTL_SET: release_free_memory called\n", c->sfd);
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS, 0);
    } else {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
    }
}

static void not_supported_executor(conn *c, void *packet)
{
    write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED, 0);
}


typedef int (*bin_package_validate)(void *packet);
typedef void (*bin_package_execute)(conn *c, void *packet);

bin_package_validate validators[0x100];
bin_package_execute executors[0x100];

static void setup_bin_packet_handlers(void) {
    validators[PROTOCOL_BINARY_CMD_DCP_OPEN] = dcp_open_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_ADD_STREAM] = dcp_add_stream_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_CLOSE_STREAM] = dcp_close_stream_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER] = dcp_snapshot_marker_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_DELETION] = dcp_deletion_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_EXPIRATION] = dcp_expiration_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_FLUSH] = dcp_flush_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG] = dcp_get_failover_log_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_MUTATION] = dcp_mutation_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE] = dcp_set_vbucket_state_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_NOOP] = dcp_noop_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT] = dcp_buffer_acknowledgement_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_CONTROL] = dcp_control_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_STREAM_END] = dcp_stream_end_validator;
    validators[PROTOCOL_BINARY_CMD_DCP_STREAM_REQ] = dcp_stream_req_validator;
    validators[PROTOCOL_BINARY_CMD_ISASL_REFRESH] = isasl_refresh_validator;
    validators[PROTOCOL_BINARY_CMD_SSL_CERTS_REFRESH] = ssl_certs_refresh_validator;
    validators[PROTOCOL_BINARY_CMD_VERBOSITY] = verbosity_validator;
    validators[PROTOCOL_BINARY_CMD_HELLO] = hello_validator;
    validators[PROTOCOL_BINARY_CMD_VERSION] = version_validator;
    validators[PROTOCOL_BINARY_CMD_QUIT] = quit_validator;
    validators[PROTOCOL_BINARY_CMD_QUITQ] = quit_validator;
    validators[PROTOCOL_BINARY_CMD_SASL_LIST_MECHS] = sasl_list_mech_validator;
    validators[PROTOCOL_BINARY_CMD_NOOP] = noop_validator;
    validators[PROTOCOL_BINARY_CMD_FLUSH] = flush_validator;
    validators[PROTOCOL_BINARY_CMD_FLUSHQ] = flush_validator;
    validators[PROTOCOL_BINARY_CMD_GET] = get_validator;
    validators[PROTOCOL_BINARY_CMD_GETQ] = get_validator;
    validators[PROTOCOL_BINARY_CMD_GETK] = get_validator;
    validators[PROTOCOL_BINARY_CMD_GETKQ] = get_validator;
    validators[PROTOCOL_BINARY_CMD_DELETE] = delete_validator;
    validators[PROTOCOL_BINARY_CMD_DELETEQ] = delete_validator;
    validators[PROTOCOL_BINARY_CMD_STAT] = stat_validator;
    validators[PROTOCOL_BINARY_CMD_INCREMENT] = arithmetic_validator;
    validators[PROTOCOL_BINARY_CMD_INCREMENTQ] = arithmetic_validator;
    validators[PROTOCOL_BINARY_CMD_DECREMENT] = arithmetic_validator;
    validators[PROTOCOL_BINARY_CMD_DECREMENTQ] = arithmetic_validator;
    validators[PROTOCOL_BINARY_CMD_GET_CMD_TIMER] = get_cmd_timer_validator;
    validators[PROTOCOL_BINARY_CMD_SET_CTRL_TOKEN] = set_ctrl_token_validator;
    validators[PROTOCOL_BINARY_CMD_GET_CTRL_TOKEN] = get_ctrl_token_validator;
    validators[PROTOCOL_BINARY_CMD_IOCTL_GET] = get_validator;
    validators[PROTOCOL_BINARY_CMD_TAP_MUTATION] = tap_validator;
    validators[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_END] = tap_validator;
    validators[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_START] = tap_validator;
    validators[PROTOCOL_BINARY_CMD_TAP_DELETE] = tap_validator;
    validators[PROTOCOL_BINARY_CMD_TAP_FLUSH] = tap_validator;
    validators[PROTOCOL_BINARY_CMD_TAP_OPAQUE] = tap_validator;
    validators[PROTOCOL_BINARY_CMD_TAP_VBUCKET_SET] = tap_validator;

    executors[PROTOCOL_BINARY_CMD_DCP_OPEN] = dcp_open_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_ADD_STREAM] = dcp_add_stream_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_CLOSE_STREAM] = dcp_close_stream_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER] = dcp_snapshot_marker_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_END] = tap_checkpoint_end_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_START] = tap_checkpoint_start_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_CONNECT] = tap_connect_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_DELETE] = tap_delete_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_FLUSH] = tap_flush_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_MUTATION] = tap_mutation_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_OPAQUE] = tap_opaque_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_VBUCKET_SET] = tap_vbucket_set_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_DELETION] = dcp_deletion_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_EXPIRATION] = dcp_expiration_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_FLUSH] = dcp_flush_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG] = dcp_get_failover_log_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_MUTATION] = dcp_mutation_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE] = dcp_set_vbucket_state_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_NOOP] = dcp_noop_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT] = dcp_buffer_acknowledgement_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_CONTROL] = dcp_control_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_STREAM_END] = dcp_stream_end_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_STREAM_REQ] = dcp_stream_req_executor;
    executors[PROTOCOL_BINARY_CMD_ISASL_REFRESH] = isasl_refresh_executor;
    executors[PROTOCOL_BINARY_CMD_SSL_CERTS_REFRESH] = ssl_certs_refresh_executor;
    executors[PROTOCOL_BINARY_CMD_VERBOSITY] = verbosity_executor;
    executors[PROTOCOL_BINARY_CMD_HELLO] = process_hello_packet_executor;
    executors[PROTOCOL_BINARY_CMD_VERSION] = version_executor;
    executors[PROTOCOL_BINARY_CMD_QUIT] = quit_executor;
    executors[PROTOCOL_BINARY_CMD_QUITQ] = quitq_executor;
    executors[PROTOCOL_BINARY_CMD_SASL_LIST_MECHS] = sasl_list_mech_executor;
    executors[PROTOCOL_BINARY_CMD_NOOP] = noop_executor;
    executors[PROTOCOL_BINARY_CMD_FLUSH] = flush_executor;
    executors[PROTOCOL_BINARY_CMD_FLUSHQ] = flush_executor;
    executors[PROTOCOL_BINARY_CMD_GET] = get_executor;
    executors[PROTOCOL_BINARY_CMD_GETQ] = get_executor;
    executors[PROTOCOL_BINARY_CMD_GETK] = get_executor;
    executors[PROTOCOL_BINARY_CMD_GETKQ] = get_executor;
    executors[PROTOCOL_BINARY_CMD_DELETE] = delete_executor;
    executors[PROTOCOL_BINARY_CMD_DELETEQ] = delete_executor;
    executors[PROTOCOL_BINARY_CMD_STAT] = stat_executor;
    executors[PROTOCOL_BINARY_CMD_INCREMENT] = arithmetic_executor;
    executors[PROTOCOL_BINARY_CMD_INCREMENTQ] = arithmetic_executor;
    executors[PROTOCOL_BINARY_CMD_DECREMENT] = arithmetic_executor;
    executors[PROTOCOL_BINARY_CMD_DECREMENTQ] = arithmetic_executor;
    executors[PROTOCOL_BINARY_CMD_GET_CMD_TIMER] = get_cmd_timer_executor;
    executors[PROTOCOL_BINARY_CMD_SET_CTRL_TOKEN] = set_ctrl_token_executor;
    executors[PROTOCOL_BINARY_CMD_GET_CTRL_TOKEN] = get_ctrl_token_executor;
    executors[PROTOCOL_BINARY_CMD_IOCTL_GET] = ioctl_get_executor;
    executors[PROTOCOL_BINARY_CMD_IOCTL_SET] = ioctl_set_executor;
}

static void setup_not_supported_handlers(void) {
    if (settings.engine.v1->get_tap_iterator == NULL) {
        executors[PROTOCOL_BINARY_CMD_TAP_CONNECT] = not_supported_executor;
    }

    if (settings.engine.v1->tap_notify == NULL) {
        executors[PROTOCOL_BINARY_CMD_TAP_MUTATION] = not_supported_executor;
        executors[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_START] = not_supported_executor;
        executors[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_END] = not_supported_executor;
        executors[PROTOCOL_BINARY_CMD_TAP_DELETE] = not_supported_executor;
        executors[PROTOCOL_BINARY_CMD_TAP_FLUSH] = not_supported_executor;
        executors[PROTOCOL_BINARY_CMD_TAP_OPAQUE] = not_supported_executor;
        executors[PROTOCOL_BINARY_CMD_TAP_VBUCKET_SET] = not_supported_executor;
    }
}

static int invalid_datatype(conn *c) {
    switch (c->binary_header.request.datatype) {
    case PROTOCOL_BINARY_RAW_BYTES:
        return 0;

    case PROTOCOL_BINARY_DATATYPE_JSON:
    case PROTOCOL_BINARY_DATATYPE_COMPRESSED:
    case PROTOCOL_BINARY_DATATYPE_COMPRESSED_JSON:
        if (c->supports_datatype) {
            return 0;
        }
        /* FALLTHROUGH */
    default:
        return 1;
    }
}

static void process_bin_packet(conn *c) {

    char *packet = (c->rcurr - (c->binary_header.request.bodylen +
                                sizeof(c->binary_header)));

    uint8_t opcode = c->binary_header.request.opcode;

    bin_package_validate validator = validators[opcode];
    bin_package_execute executor = executors[opcode];

    if (validator != NULL && validator(packet) != 0) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
    } else if (executor != NULL) {
        executor(c, packet);
    } else {
        process_bin_unknown_packet(c);
    }
}

static void dispatch_bin_command(conn *c) {
    int protocol_error = 0;

    int extlen = c->binary_header.request.extlen;
    uint16_t keylen = c->binary_header.request.keylen;
    uint32_t bodylen = c->binary_header.request.bodylen;

    if (settings.require_sasl && !authenticated(c)) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, 0);
        c->write_and_go = conn_closing;
        return;
    }

    if (invalid_datatype(c)) {
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
        c->write_and_go = conn_closing;
        return;
    }

    if (c->start == 0) {
        c->start = gethrtime();
    }

    MEMCACHED_PROCESS_COMMAND_START(c->sfd, c->rcurr, c->rbytes);

    /* binprot supports 16bit keys, but internals are still 8bit */
    if (keylen > KEY_MAX_LENGTH) {
        handle_binary_protocol_error(c);
        return;
    }

    if (executors[c->cmd] != NULL) {
        c->noreply = false;
        bin_read_chunk(c, bin_reading_packet, c->binary_header.request.bodylen);
        return;
    }

    c->noreply = true;

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SETQ:
        c->cmd = PROTOCOL_BINARY_CMD_SET;
        break;
    case PROTOCOL_BINARY_CMD_ADDQ:
        c->cmd = PROTOCOL_BINARY_CMD_ADD;
        break;
    case PROTOCOL_BINARY_CMD_REPLACEQ:
        c->cmd = PROTOCOL_BINARY_CMD_REPLACE;
        break;
    case PROTOCOL_BINARY_CMD_APPENDQ:
        c->cmd = PROTOCOL_BINARY_CMD_APPEND;
        break;
    case PROTOCOL_BINARY_CMD_PREPENDQ:
        c->cmd = PROTOCOL_BINARY_CMD_PREPEND;
        break;
    default:
        c->noreply = false;
    }

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SET: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_ADD: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_REPLACE:
        if (extlen == 8 && keylen != 0 && bodylen >= (uint32_t)(keylen + 8)) {
            bin_read_key(c, bin_reading_set_header, 8);
        } else {
            protocol_error = 1;
        }

        break;
    case PROTOCOL_BINARY_CMD_APPEND:
    case PROTOCOL_BINARY_CMD_PREPEND:
        if (keylen > 0 && extlen == 0) {
            bin_read_key(c, bin_reading_set_header, 0);
        } else {
            protocol_error = 1;
        }
        break;

    case PROTOCOL_BINARY_CMD_SASL_AUTH:
    case PROTOCOL_BINARY_CMD_SASL_STEP:
        if (extlen == 0 && keylen != 0) {
            bin_read_key(c, bin_reading_sasl_auth, 0);
        } else {
            protocol_error = 1;
        }
        break;

    default:
        if (settings.engine.v1->unknown_command == NULL) {
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND,
                             bodylen);
        } else {
            bin_read_chunk(c, bin_reading_packet, c->binary_header.request.bodylen);
        }
    }

    if (protocol_error) {
        handle_binary_protocol_error(c);
    }
}

static void process_bin_update(conn *c) {
    char *key;
    uint16_t nkey;
    uint32_t vlen;
    item *it;
    protocol_binary_request_set* req = binary_get_request(c);
    ENGINE_ERROR_CODE ret;
    item_info_holder info;
    rel_time_t expiration;

    cb_assert(c != NULL);
    memset(&info, 0, sizeof(info));
    info.info.nvalue = 1;
    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;

    /* fix byteorder in the request */
    req->message.body.flags = req->message.body.flags;
    expiration = ntohl(req->message.body.expiration);

    vlen = c->binary_header.request.bodylen - (nkey + c->binary_header.request.extlen);

    if (settings.verbose > 1) {
        size_t nw;
        char buffer[1024];
        const char *prefix;
        if (c->cmd == PROTOCOL_BINARY_CMD_ADD) {
            prefix = "ADD";
        } else if (c->cmd == PROTOCOL_BINARY_CMD_SET) {
            prefix = "SET";
        } else {
            prefix = "REPLACE";
        }

        nw = key_to_printable_buffer(buffer, sizeof(buffer), c->sfd, true,
                                     prefix, key, nkey);

        if (nw != -1) {
            if (snprintf(buffer + nw, sizeof(buffer) - nw,
                         " Value len is %d\n", vlen)) {
                settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c, "%s",
                                                buffer);
            }
        }
    }

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;

    if (ret == ENGINE_SUCCESS) {
        ret = settings.engine.v1->allocate(settings.engine.v0, c,
                                           &it, key, nkey,
                                           vlen,
                                           req->message.body.flags,
                                           expiration,
                                           c->binary_header.request.datatype);
        if (ret == ENGINE_SUCCESS && !settings.engine.v1->get_item_info(settings.engine.v0,
                                                                        c, it,
                                                                        (void*)&info)) {
            settings.engine.v1->release(settings.engine.v0, c, it);
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINTERNAL, 0);
            return;
        }
    }

    switch (ret) {
    case ENGINE_SUCCESS:
        item_set_cas(c, it, c->binary_header.request.cas);

        switch (c->cmd) {
        case PROTOCOL_BINARY_CMD_ADD:
            c->store_op = OPERATION_ADD;
            break;
        case PROTOCOL_BINARY_CMD_SET:
            if (c->binary_header.request.cas != 0) {
                c->store_op = OPERATION_CAS;
            } else {
                c->store_op = OPERATION_SET;
            }
            break;
        case PROTOCOL_BINARY_CMD_REPLACE:
            if (c->binary_header.request.cas != 0) {
                c->store_op = OPERATION_CAS;
            } else {
                c->store_op = OPERATION_REPLACE;
            }
            break;
        default:
            cb_assert(0);
        }

        c->item = it;
        c->ritem = info.info.value[0].iov_base;
        c->rlbytes = vlen;
        conn_set_state(c, conn_nread);
        c->substate = bin_read_set_value;
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        break;
    case ENGINE_DISCONNECT:
        c->state = conn_closing;
        break;
    default:
        if (ret == ENGINE_E2BIG) {
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_E2BIG, vlen);
        } else {
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, vlen);
        }

        /* swallow the data line */
        c->write_and_go = conn_swallow;
    }
}

static void process_bin_append_prepend(conn *c) {
    ENGINE_ERROR_CODE ret;
    char *key;
    int nkey;
    int vlen;
    item *it;
    item_info_holder info;
    memset(&info, 0, sizeof(info));
    info.info.nvalue = 1;

    cb_assert(c != NULL);

    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;
    vlen = c->binary_header.request.bodylen - nkey;

    if (settings.verbose > 1) {
        settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                        "Value len is %d\n", vlen);
    }

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;

    if (ret == ENGINE_SUCCESS) {
        ret = settings.engine.v1->allocate(settings.engine.v0, c,
                                           &it, key, nkey,
                                           vlen, 0, 0,
                                           c->binary_header.request.datatype);
        if (ret == ENGINE_SUCCESS && !settings.engine.v1->get_item_info(settings.engine.v0,
                                                                        c, it,
                                                                        (void*)&info)) {
            settings.engine.v1->release(settings.engine.v0, c, it);
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINTERNAL, 0);
            return;
        }
    }

    switch (ret) {
    case ENGINE_SUCCESS:
        item_set_cas(c, it, c->binary_header.request.cas);

        switch (c->cmd) {
        case PROTOCOL_BINARY_CMD_APPEND:
            c->store_op = OPERATION_APPEND;
            break;
        case PROTOCOL_BINARY_CMD_PREPEND:
            c->store_op = OPERATION_PREPEND;
            break;
        default:
            cb_assert(0);
        }

        c->item = it;
        c->ritem = info.info.value[0].iov_base;
        c->rlbytes = vlen;
        conn_set_state(c, conn_nread);
        c->substate = bin_read_set_value;
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        break;
    case ENGINE_DISCONNECT:
        c->state = conn_closing;
        break;
    default:
        if (ret == ENGINE_E2BIG) {
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_E2BIG, vlen);
        } else {
            write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, vlen);
        }
        /* swallow the data line */
        c->write_and_go = conn_swallow;
    }
}

static void process_bin_delete(conn *c) {
    ENGINE_ERROR_CODE ret;
    protocol_binary_request_delete* req = binary_get_request(c);
    char* key = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;
    uint64_t cas = ntohll(req->message.header.request.cas);
    item_info_holder info;
    memset(&info, 0, sizeof(info));

    info.info.nvalue = 1;

    cb_assert(c != NULL);

    if (settings.verbose > 1) {
        char buffer[1024];
        if (key_to_printable_buffer(buffer, sizeof(buffer), c->sfd, true,
                                    "DELETE", key, nkey) != -1) {
            settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c, "%s\n",
                                            buffer);
        }
    }

    ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;

    if (ret == ENGINE_SUCCESS) {
        if (settings.detail_enabled) {
            stats_prefix_record_delete(key, nkey);
        }
        ret = settings.engine.v1->remove(settings.engine.v0, c, key, nkey,
                                         &cas, c->binary_header.request.vbucket);
    }

    /* For some reason the SLAB_INCR tries to access this... */
    switch (ret) {
    case ENGINE_SUCCESS:
        c->cas = cas;
        write_bin_response(c, NULL, 0, 0, 0);
        SLAB_INCR(c, delete_hits, key, nkey);
        break;
    case ENGINE_KEY_EEXISTS:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, 0);
        break;
    case ENGINE_KEY_ENOENT:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, 0);
        STATS_INCR(c, delete_misses, key, nkey);
        break;
    case ENGINE_NOT_MY_VBUCKET:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET, 0);
        break;
    case ENGINE_TMPFAIL:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_ETMPFAIL, 0);
        break;
    case ENGINE_EWOULDBLOCK:
        c->ewouldblock = true;
        break;
    default:
        write_bin_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL, 0);
    }
}

static void complete_nread(conn *c) {
    cb_assert(c != NULL);
    cb_assert(c->cmd >= 0);

    switch(c->substate) {
    case bin_reading_set_header:
        if (c->cmd == PROTOCOL_BINARY_CMD_APPEND ||
                c->cmd == PROTOCOL_BINARY_CMD_PREPEND) {
            process_bin_append_prepend(c);
        } else {
            process_bin_update(c);
        }
        break;
    case bin_read_set_value:
        complete_update_bin(c);
        break;
    case bin_reading_sasl_auth:
        process_bin_sasl_auth(c);
        break;
    case bin_reading_sasl_auth_data:
        process_bin_complete_sasl_auth(c);
        break;
    case bin_reading_packet:
        if (c->binary_header.request.magic == PROTOCOL_BINARY_RES) {
            RESPONSE_HANDLER handler;
            handler = response_handlers[c->binary_header.request.opcode];
            if (handler) {
                handler(c);
            } else {
                settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                       "%d: ERROR: Unsupported response packet received: %u\n",
                        c->sfd, (unsigned int)c->binary_header.request.opcode);
                conn_set_state(c, conn_closing);
            }
        } else {
            process_bin_packet(c);
        }
        break;
    default:
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                "Not handling substate %d\n", c->substate);
        abort();
    }
}

static void reset_cmd_handler(conn *c) {
    c->sbytes = 0;
    c->cmd = -1;
    c->substate = bin_no_state;
    if(c->item != NULL) {
        settings.engine.v1->release(settings.engine.v0, c, c->item);
        c->item = NULL;
    }
    conn_shrink(c);
    if (c->rbytes > 0) {
        conn_set_state(c, conn_parse_cmd);
    } else {
        conn_set_state(c, conn_waiting);
    }
}

/* set up a connection to write a buffer then free it, used for stats */
static void write_and_free(conn *c, char *buf, size_t bytes) {
    if (buf) {
        c->write_and_free = buf;
        c->wcurr = buf;
        c->wbytes = (uint32_t)bytes;
        conn_set_state(c, conn_write);
        c->write_and_go = conn_new_cmd;
    } else {
        conn_set_state(c, conn_closing);
    }
}

void append_stat(const char *name, ADD_STAT add_stats, conn *c,
                 const char *fmt, ...) {
    char val_str[STAT_VAL_LEN];
    int vlen;
    va_list ap;

    cb_assert(name);
    cb_assert(add_stats);
    cb_assert(c);
    cb_assert(fmt);

    va_start(ap, fmt);
    vlen = vsnprintf(val_str, sizeof(val_str) - 1, fmt, ap);
    va_end(ap);

    add_stats(name, (uint16_t)strlen(name), val_str, vlen, c);
}

static void aggregate_callback(void *in, void *out) {
    threadlocal_stats_aggregate(in, out);
}

/* return server specific stats only */
static void server_stats(ADD_STAT add_stats, conn *c, bool aggregate) {
#ifdef WIN32
    long pid = GetCurrentProcessId();
#else
    struct rusage usage;
    long pid = (long)getpid();
#endif
    struct slab_stats slab_stats;
    char stat_key[1024];
    int i;
    struct tap_stats ts;
    rel_time_t now = mc_time_get_current_time();

    struct thread_stats thread_stats;
    threadlocal_stats_clear(&thread_stats);

    if (aggregate && settings.engine.v1->aggregate_stats != NULL) {
        settings.engine.v1->aggregate_stats(settings.engine.v0,
                                            (const void *)c,
                                            aggregate_callback,
                                            &thread_stats);
    } else {
        threadlocal_stats_aggregate(get_independent_stats(c),
                                    &thread_stats);
    }

    slab_stats_aggregate(&thread_stats, &slab_stats);

#ifndef WIN32
    getrusage(RUSAGE_SELF, &usage);
#endif

    STATS_LOCK();

    APPEND_STAT("pid", "%lu", pid);
    APPEND_STAT("uptime", "%u", now);
    APPEND_STAT("time", "%ld", mc_time_convert_to_abs_time(now));
    APPEND_STAT("version", "%s", get_server_version());
    APPEND_STAT("memcached_version", "%s", MEMCACHED_VERSION);
    APPEND_STAT("libevent", "%s", event_get_version());
    APPEND_STAT("pointer_size", "%d", (int)(8 * sizeof(void *)));

#ifndef WIN32
    append_stat("rusage_user", add_stats, c, "%ld.%06ld",
                (long)usage.ru_utime.tv_sec,
                (long)usage.ru_utime.tv_usec);
    append_stat("rusage_system", add_stats, c, "%ld.%06ld",
                (long)usage.ru_stime.tv_sec,
                (long)usage.ru_stime.tv_usec);
#endif

    APPEND_STAT("daemon_connections", "%u", stats.daemon_conns);
    APPEND_STAT("curr_connections", "%u", stats.curr_conns);
    for (i = 0; i < settings.num_interfaces; ++i) {
        sprintf(stat_key, "%s", "max_conns_on_port_");
        sprintf(stat_key + strlen(stat_key), "%d", stats.listening_ports[i].port);
        APPEND_STAT(stat_key, "%d", stats.listening_ports[i].maxconns);
        sprintf(stat_key, "%s", "curr_conns_on_port_");
        sprintf(stat_key + strlen(stat_key), "%d", stats.listening_ports[i].port);
        APPEND_STAT(stat_key, "%d", stats.listening_ports[i].curr_conns);
    }
    APPEND_STAT("total_connections", "%u", stats.total_conns);
    APPEND_STAT("connection_structures", "%u", stats.conn_structs);
    APPEND_STAT("cmd_get", "%"PRIu64, thread_stats.cmd_get);
    APPEND_STAT("cmd_set", "%"PRIu64, slab_stats.cmd_set);
    APPEND_STAT("cmd_flush", "%"PRIu64, thread_stats.cmd_flush);
    APPEND_STAT("auth_cmds", "%"PRIu64, thread_stats.auth_cmds);
    APPEND_STAT("auth_errors", "%"PRIu64, thread_stats.auth_errors);
    APPEND_STAT("get_hits", "%"PRIu64, slab_stats.get_hits);
    APPEND_STAT("get_misses", "%"PRIu64, thread_stats.get_misses);
    APPEND_STAT("delete_misses", "%"PRIu64, thread_stats.delete_misses);
    APPEND_STAT("delete_hits", "%"PRIu64, slab_stats.delete_hits);
    APPEND_STAT("incr_misses", "%"PRIu64, thread_stats.incr_misses);
    APPEND_STAT("incr_hits", "%"PRIu64, thread_stats.incr_hits);
    APPEND_STAT("decr_misses", "%"PRIu64, thread_stats.decr_misses);
    APPEND_STAT("decr_hits", "%"PRIu64, thread_stats.decr_hits);
    APPEND_STAT("cas_misses", "%"PRIu64, thread_stats.cas_misses);
    APPEND_STAT("cas_hits", "%"PRIu64, slab_stats.cas_hits);
    APPEND_STAT("cas_badval", "%"PRIu64, slab_stats.cas_badval);
    APPEND_STAT("bytes_read", "%"PRIu64, thread_stats.bytes_read);
    APPEND_STAT("bytes_written", "%"PRIu64, thread_stats.bytes_written);
    APPEND_STAT("accepting_conns", "%u",  is_listen_disabled() ? 0 : 1);
    APPEND_STAT("listen_disabled_num", "%"PRIu64, get_listen_disabled_num());
    APPEND_STAT("rejected_conns", "%" PRIu64, (uint64_t)stats.rejected_conns);
    APPEND_STAT("threads", "%d", settings.num_threads);
    APPEND_STAT("conn_yields", "%" PRIu64, (uint64_t)thread_stats.conn_yields);
    STATS_UNLOCK();

    /*
     * Add tap stats (only if non-zero)
     */
    cb_mutex_enter(&tap_stats.mutex);
    ts = tap_stats;
    cb_mutex_exit(&tap_stats.mutex);

    if (ts.sent.connect) {
        APPEND_STAT("tap_connect_sent", "%"PRIu64, ts.sent.connect);
    }
    if (ts.sent.mutation) {
        APPEND_STAT("tap_mutation_sent", "%"PRIu64, ts.sent.mutation);
    }
    if (ts.sent.checkpoint_start) {
        APPEND_STAT("tap_checkpoint_start_sent", "%"PRIu64, ts.sent.checkpoint_start);
    }
    if (ts.sent.checkpoint_end) {
        APPEND_STAT("tap_checkpoint_end_sent", "%"PRIu64, ts.sent.checkpoint_end);
    }
    if (ts.sent.delete) {
        APPEND_STAT("tap_delete_sent", "%"PRIu64, ts.sent.delete);
    }
    if (ts.sent.flush) {
        APPEND_STAT("tap_flush_sent", "%"PRIu64, ts.sent.flush);
    }
    if (ts.sent.opaque) {
        APPEND_STAT("tap_opaque_sent", "%"PRIu64, ts.sent.opaque);
    }
    if (ts.sent.vbucket_set) {
        APPEND_STAT("tap_vbucket_set_sent", "%"PRIu64,
                    ts.sent.vbucket_set);
    }
    if (ts.received.connect) {
        APPEND_STAT("tap_connect_received", "%"PRIu64, ts.received.connect);
    }
    if (ts.received.mutation) {
        APPEND_STAT("tap_mutation_received", "%"PRIu64, ts.received.mutation);
    }
    if (ts.received.checkpoint_start) {
        APPEND_STAT("tap_checkpoint_start_received", "%"PRIu64, ts.received.checkpoint_start);
    }
    if (ts.received.checkpoint_end) {
        APPEND_STAT("tap_checkpoint_end_received", "%"PRIu64, ts.received.checkpoint_end);
    }
    if (ts.received.delete) {
        APPEND_STAT("tap_delete_received", "%"PRIu64, ts.received.delete);
    }
    if (ts.received.flush) {
        APPEND_STAT("tap_flush_received", "%"PRIu64, ts.received.flush);
    }
    if (ts.received.opaque) {
        APPEND_STAT("tap_opaque_received", "%"PRIu64, ts.received.opaque);
    }
    if (ts.received.vbucket_set) {
        APPEND_STAT("tap_vbucket_set_received", "%"PRIu64,
                    ts.received.vbucket_set);
    }
}

static void process_stat_settings(ADD_STAT add_stats, void *c) {
    int ii;
    cb_assert(add_stats);
    APPEND_STAT("maxconns", "%d", settings.maxconns);

    for (ii = 0; ii < settings.num_interfaces; ++ii) {
        char interface[1024];
        int offset;
        if (settings.interfaces[ii].host == NULL) {
            offset = sprintf(interface, "interface-*:%u", settings.interfaces[ii].port);
        } else {
            offset = snprintf(interface, sizeof(interface), "interface-%s:%u",
                              settings.interfaces[ii].host,
                              settings.interfaces[ii].port);
        }

        snprintf(interface + offset, sizeof(interface) - offset, "-maxconn");
        APPEND_STAT(interface, "%u", settings.interfaces[ii].maxconn);
        snprintf(interface + offset, sizeof(interface) - offset, "-backlog");
        APPEND_STAT(interface, "%u", settings.interfaces[ii].backlog);
        snprintf(interface + offset, sizeof(interface) - offset, "-ipv4");
        APPEND_STAT(interface, "%s", settings.interfaces[ii].ipv4 ?
                    "true" : "false");
        snprintf(interface + offset, sizeof(interface) - offset, "-ipv6");
        APPEND_STAT(interface, "%s", settings.interfaces[ii].ipv6 ?
                    "true" : "false");

        snprintf(interface + offset, sizeof(interface) - offset,
                 "-tcp_nodelay");
        APPEND_STAT(interface, "%s", settings.interfaces[ii].tcp_nodelay ?
                    "true" : "false");

        if (settings.interfaces[ii].ssl.key) {
            snprintf(interface + offset, sizeof(interface) - offset,
                     "-ssl-pkey");
            APPEND_STAT(interface, "%s", settings.interfaces[ii].ssl.key);
            snprintf(interface + offset, sizeof(interface) - offset,
                     "-ssl-cert");
            APPEND_STAT(interface, "%s", settings.interfaces[ii].ssl.cert);
        } else {
            snprintf(interface + offset, sizeof(interface) - offset,
                     "-ssl");
            APPEND_STAT(interface, "%s", "false");
        }
    }

    APPEND_STAT("verbosity", "%d", settings.verbose);
    APPEND_STAT("num_threads", "%d", settings.num_threads);
    APPEND_STAT("stat_key_prefix", "%c", settings.prefix_delimiter);
    APPEND_STAT("detail_enabled", "%s",
                settings.detail_enabled ? "yes" : "no");
    APPEND_STAT("allow_detailed", "%s",
                settings.allow_detailed ? "yes" : "no");
    APPEND_STAT("reqs_per_event_high_priority", "%d",
                settings.reqs_per_event_high_priority);
    APPEND_STAT("reqs_per_event_med_priority", "%d",
                settings.reqs_per_event_med_priority);
    APPEND_STAT("reqs_per_event_low_priority", "%d",
                settings.reqs_per_event_low_priority);
    APPEND_STAT("reqs_per_event_def_priority", "%d",
                settings.default_reqs_per_event);
    APPEND_STAT("auth_enabled_sasl", "%s", "yes");

    APPEND_STAT("auth_sasl_engine", "%s", "cbsasl");
    APPEND_STAT("auth_required_sasl", "%s", settings.require_sasl ? "yes" : "no");
    {
        EXTENSION_DAEMON_DESCRIPTOR *ptr;
        for (ptr = settings.extensions.daemons; ptr != NULL; ptr = ptr->next) {
            APPEND_STAT("extension", "%s", ptr->get_name());
        }
    }

    APPEND_STAT("logger", "%s", settings.extensions.logger->get_name());
    {
        EXTENSION_BINARY_PROTOCOL_DESCRIPTOR *ptr;
        for (ptr = settings.extensions.binary; ptr != NULL; ptr = ptr->next) {
            APPEND_STAT("binary_extension", "%s", ptr->get_name());
        }
    }

    if (settings.config) {
        add_stats("config", (uint16_t)strlen("config"),
                  settings.config, strlen(settings.config), c);
    }
}

/*
 * if we have a complete line in the buffer, process it.
 */
static int try_read_command(conn *c) {
    cb_assert(c != NULL);
    cb_assert(c->rcurr <= (c->rbuf + c->rsize));
    cb_assert(c->rbytes > 0);

    /* Do we have the complete packet header? */
    if (c->rbytes < sizeof(c->binary_header)) {
        /* need more data! */
        return 0;
    } else {
#ifdef NEED_ALIGN
        if (((long)(c->rcurr)) % 8 != 0) {
            /* must realign input buffer */
            memmove(c->rbuf, c->rcurr, c->rbytes);
            c->rcurr = c->rbuf;
            if (settings.verbose > 1) {
                settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                                "%d: Realign input buffer\n", c->sfd);
            }
        }
#endif
        protocol_binary_request_header* req;
        req = (protocol_binary_request_header*)c->rcurr;

        if (settings.verbose > 1) {
            /* Dump the packet before we convert it to host order */
            char buffer[1024];
            ssize_t nw;
            nw = bytes_to_output_string(buffer, sizeof(buffer), c->sfd,
                                        true, "Read binary protocol data:",
                                        (const char*)req->bytes,
                                        sizeof(req->bytes));
            if (nw != -1) {
                settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                                "%s", buffer);
            }
        }

        c->binary_header = *req;
        c->binary_header.request.keylen = ntohs(req->request.keylen);
        c->binary_header.request.bodylen = ntohl(req->request.bodylen);
        c->binary_header.request.vbucket = ntohs(req->request.vbucket);
        c->binary_header.request.cas = ntohll(req->request.cas);

        if (c->binary_header.request.magic != PROTOCOL_BINARY_REQ &&
            !(c->binary_header.request.magic == PROTOCOL_BINARY_RES &&
              response_handlers[c->binary_header.request.opcode])) {
            if (settings.verbose) {
                if (c->binary_header.request.magic != PROTOCOL_BINARY_RES) {
                    settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                                    "%d: Invalid magic:  %x\n",
                                                    c->sfd,
                                                    c->binary_header.request.magic);
                } else {
                    settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                                    "%d: ERROR: Unsupported response packet received: %u\n",
                                                    c->sfd, (unsigned int)c->binary_header.request.opcode);

                }
            }
            conn_set_state(c, conn_closing);
            return -1;
        }

        c->msgcurr = 0;
        c->msgused = 0;
        c->iovused = 0;
        if (add_msghdr(c) != 0) {
            conn_set_state(c, conn_closing);
            return -1;
        }

        c->cmd = c->binary_header.request.opcode;
        c->keylen = c->binary_header.request.keylen;
        c->opaque = c->binary_header.request.opaque;
        /* clear the returned cas value */
        c->cas = 0;

        dispatch_bin_command(c);

        c->rbytes -= sizeof(c->binary_header);
        c->rcurr += sizeof(c->binary_header);
    }

    return 1;
}

static void drain_bio_send_pipe(conn *c) {
    int n;
    bool stop = false;

    do {
        if (c->ssl.out.current < c->ssl.out.total) {
#ifdef WIN32
            DWORD error;
#else
            int error;
#endif
            n = send(c->sfd, c->ssl.out.buffer + c->ssl.out.current,
                     c->ssl.out.total - c->ssl.out.current, 0);
            if (n > 0) {
                c->ssl.out.current += n;
                if (c->ssl.out.current == c->ssl.out.total) {
                    c->ssl.out.current = c->ssl.out.total = 0;
                }
            } else {
                if (n == -1) {
#ifdef WIN32
                    error = WSAGetLastError();
#else
                    error = errno;
#endif
                    if (!is_blocking(error)) {
                        c->ssl.error = true;
                    }
                }
                return ;
            }
        }

        if (c->ssl.out.total == 0) {
            n = BIO_read(c->ssl.network, c->ssl.out.buffer, c->ssl.out.buffsz);
            if (n > 0) {
                c->ssl.out.total = n;
            } else {
                stop = true;
            }
        }
    } while (!stop);
}

static void drain_bio_recv_pipe(conn *c) {
    int n;
    bool stop = false;

    stop = false;
    do {
        if (c->ssl.in.current < c->ssl.in.total) {
            n = BIO_write(c->ssl.network, c->ssl.in.buffer + c->ssl.in.current,
                          c->ssl.in.total - c->ssl.in.current);
            if (n > 0) {
                c->ssl.in.current += n;
                if (c->ssl.in.current == c->ssl.in.total) {
                    c->ssl.in.current = c->ssl.in.total = 0;
                }
            } else {
                /* Our input BIO is full, no need to grab more data from
                 * the network at this time..
                 */
                return ;
            }
        }

        if (c->ssl.in.total < c->ssl.in.buffsz) {
#ifdef WIN32
            DWORD error;
#else
            int error;
#endif
            n = recv(c->sfd, c->ssl.in.buffer + c->ssl.in.total,
                     c->ssl.in.buffsz - c->ssl.in.total, 0);
            if (n > 0) {
                c->ssl.in.total += n;
            } else {
                stop = true;
                if (n == 0) {
                    c->ssl.error = true; /* read end shutdown */
                } else {
#ifdef WIN32
                    error = WSAGetLastError();
#else
                    error = errno;
#endif
                    if (!is_blocking(error)) {
                        c->ssl.error = true;
                    }
                }
            }
        }
    } while (!stop);
}

static int do_ssl_pre_connection(conn *c) {
    int r = SSL_accept(c->ssl.client);
    if (r == 1) {
        drain_bio_send_pipe(c);
        c->ssl.connected = true;
    } else {
        if (SSL_get_error(c->ssl.client, r) == SSL_ERROR_WANT_READ) {
            drain_bio_send_pipe(c);
            set_ewouldblock();
            return -1;
        } else {
            char *errmsg = malloc(8*1024);
            if (errmsg) {
                int offset = sprintf(errmsg,
                                     "SSL_accept() returned %d with error %d\n",
                                     r, SSL_get_error(c->ssl.client, r));

                ERR_error_string_n(ERR_get_error(), errmsg + offset,
                                   8192 - offset);

                settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                "%d: ERROR: %s",
                                                c->sfd, errmsg);
                free(errmsg);
            }
            set_econnreset();
            return -1;
        }
    }

    return 0;
}

static int do_ssl_read(conn *c, char *dest, size_t nbytes) {
    int ret = 0;

    while (ret < nbytes) {
        int n;
        drain_bio_recv_pipe(c);
        if (c->ssl.error) {
            set_econnreset();
            return -1;
        }
        n = SSL_read(c->ssl.client, dest + ret, nbytes - ret);
        if (n > 0) {
            ret += n;
        } else {
            /* n < 0 and n == 0 require a check of SSL error*/
            int error = SSL_get_error(c->ssl.client, n);

            switch (error) {
            case SSL_ERROR_WANT_READ:
                /*
                 * Drain the buffers and retry if we've got data in
                 * our input buffers
                 */
                if (c->ssl.in.current < c->ssl.in.total) {
                    /* our recv buf has data feed the BIO */
                    drain_bio_recv_pipe(c);
                } else if (ret > 0) {
                    /* nothing in our recv buf, return what we have */
                    return ret;
                } else {
                    set_ewouldblock();
                    return -1;
                }
                break;

            default:
                /*
                 * @todo I don't know how to gracefully recover from this
                 * let's just shut down the connection
                 */
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                "%d: ERROR: SSL_read returned -1 with error %d",
                                                c->sfd, error);
                set_econnreset();
                return -1;
            }
        }
    }

    return ret;
}

static int do_data_recv(conn *c, void *dest, size_t nbytes) {
    int res;
    if (c->ssl.enabled) {
        drain_bio_recv_pipe(c);

        if (c->ssl.error) {
            set_econnreset();
            return -1;
        }

        if (!c->ssl.connected) {
            res = do_ssl_pre_connection(c);
            if (res == -1) {
                return -1;
            }
        }

        /* The SSL negotiation might be complete at this time */
        if (c->ssl.connected) {
            res = do_ssl_read(c, dest, nbytes);
        }
    } else {
        res = recv(c->sfd, dest, nbytes, 0);
    }

    return res;
}

static int do_ssl_write(conn *c, char *dest, size_t nbytes) {
    int ret = 0;

    int chunksize = settings.bio_drain_buffer_sz;

    while (ret < nbytes) {
        int n;
        int chunk;

        drain_bio_send_pipe(c);
        if (c->ssl.error) {
            set_econnreset();
            return -1;
        }

        chunk = nbytes - ret;
        if (chunk > chunksize) {
            chunk = chunksize;
        }

        n = SSL_write(c->ssl.client, dest + ret, chunk);
        if (n > 0) {
            ret += n;
        } else {
            if (ret > 0) {
                /* We've sent some data.. let the caller have them */
                return ret;
            }

            if (n < 0) {
                int error = SSL_get_error(c->ssl.client, n);
                switch (error) {
                case SSL_ERROR_WANT_WRITE:
                    set_ewouldblock();
                    return -1;

                default:
                    /*
                     * @todo I don't know how to gracefully recover from this
                     * let's just shut down the connection
                     */
                    settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                    "%d: ERROR: SSL_write returned -1 with error %d",
                                                    c->sfd, error);
                    set_econnreset();
                    return -1;
                }
            }
        }
    }

    return ret;
}


static int do_data_sendmsg(conn *c, struct msghdr *m) {
    int res;
    if (c->ssl.enabled) {
        int ii;
        res = 0;
        for (ii = 0; ii < m->msg_iovlen; ++ii) {
            int n = do_ssl_write(c,
                                 m->msg_iov[ii].iov_base,
                                 m->msg_iov[ii].iov_len);
            if (n > 0) {
                res += n;
            } else {
                return res > 0 ? res : -1;
            }
        }

        /* @todo figure out how to drain the rest of the data if we
         * failed to send all of it...
         */
        drain_bio_send_pipe(c);
        return res;
    } else {
        res = sendmsg(c->sfd, m, 0);
    }

    return res;
}

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 *
 * To protect us from someone flooding a connection with bogus data causing
 * the connection to eat up all available memory, break out and start looking
 * at the data I've got after a number of reallocs...
 *
 * @return enum try_read_result
 */
static enum try_read_result try_read_network(conn *c) {
    enum try_read_result gotdata = READ_NO_DATA_RECEIVED;
    int res;
    int num_allocs = 0;
    cb_assert(c != NULL);

    if (c->rcurr != c->rbuf) {
        if (c->rbytes != 0) /* otherwise there's nothing to copy */
            memmove(c->rbuf, c->rcurr, c->rbytes);
        c->rcurr = c->rbuf;
    }

    while (1) {
        int avail;
#ifdef WIN32
        DWORD error;
#else
        int error;
#endif

        if (c->rbytes >= c->rsize) {
            char *new_rbuf;

            if (num_allocs == 4) {
                return gotdata;
            }
            ++num_allocs;
            new_rbuf = realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf) {
                if (settings.verbose > 0) {
                    settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                                    "Couldn't realloc input buffer\n");
                }
                c->rbytes = 0; /* ignore what we read */
                conn_set_state(c, conn_closing);
                return READ_MEMORY_ERROR;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        avail = c->rsize - c->rbytes;
        res = do_data_recv(c, c->rbuf + c->rbytes, avail);
        if (res > 0) {
            STATS_ADD(c, bytes_read, res);
            gotdata = READ_DATA_RECEIVED;
            c->rbytes += res;
            if (res == avail) {
                continue;
            } else {
                break;
            }
        }
        if (res == 0) {
            return READ_ERROR;
        }
        if (res == -1) {
#ifdef WIN32
            error = WSAGetLastError();
#else
            error = errno;
#endif

            if (is_blocking(error)) {
                break;
            }
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                            "%d Closing connection due to read error: %s",
                                            c->sfd,
                                            strerror(errno));
            return READ_ERROR;
        }
    }
    return gotdata;
}

bool register_event(conn *c, struct timeval *timeout) {
    cb_assert(!c->registered_in_libevent);
    cb_assert(c->sfd != INVALID_SOCKET);

    if (event_add(&c->event, timeout) == -1) {
        log_system_error(EXTENSION_LOG_WARNING,
                         NULL,
                         "Failed to add connection to libevent: %s");
        return false;
    }

    c->registered_in_libevent = true;

    return true;
}

bool unregister_event(conn *c) {
    cb_assert(c->registered_in_libevent);
    cb_assert(c->sfd != INVALID_SOCKET);

    if (event_del(&c->event) == -1) {
        return false;
    }

    c->registered_in_libevent = false;

    return true;
}

bool update_event(conn *c, const int new_flags) {
    struct event_base *base;

    cb_assert(c != NULL);
    base = c->event.ev_base;

    if (c->ssl.enabled && c->ssl.connected && (new_flags & EV_READ)) {
        /*
         * If we want more data and we have SSL, that data might be inside
         * SSL's internal buffers rather than inside the socket buffer. In
         * that case signal an EV_READ event without actually polling the
         * socket.
         */
        char dummy;
        /* SSL_pending() will not work here despite the name */
        int rv = SSL_peek(c->ssl.client, &dummy, 1);
        if (rv > 0) {
            /* signal a call to the handler */
            event_active(&c->event, EV_READ, 0);
            return true;
        }
    }

    if (c->ev_flags == new_flags) {
        return true;
    }

    settings.extensions.logger->log(EXTENSION_LOG_DEBUG, NULL,
                                    "Updated event for %d to read=%s, write=%s\n",
                                    c->sfd, (new_flags & EV_READ ? "yes" : "no"),
                                    (new_flags & EV_WRITE ? "yes" : "no"));

    if (!unregister_event(c)) {
        return false;
    }

    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;

    return register_event(c, NULL);
}

/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static enum transmit_result transmit(conn *c) {
    cb_assert(c != NULL);

    while (c->msgcurr < c->msgused &&
           c->msglist[c->msgcurr].msg_iovlen == 0) {
        /* Finished writing the current msg; advance to the next. */
        c->msgcurr++;
    }

    if (c->msgcurr < c->msgused) {
#ifdef WIN32
        DWORD error;
#else
        int error;
#endif
        ssize_t res;
        struct msghdr *m = &c->msglist[c->msgcurr];

        res = do_data_sendmsg(c, m);
#ifdef WIN32
        error = WSAGetLastError();
#else
        error = errno;
#endif
        if (res > 0) {
            STATS_ADD(c, bytes_written, res);

            /* We've written some of the data. Remove the completed
               iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len) {
                res -= (ssize_t)m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry;
               adjust it so the next write will do the rest. */
            if (res > 0) {
                m->msg_iov->iov_base = (void*)((unsigned char*)m->msg_iov->iov_base + res);
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }

        if (res == -1 && is_blocking(error)) {
            if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                if (settings.verbose > 0) {
                    settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                            "Couldn't update event\n");
                }
                conn_set_state(c, conn_closing);
                return TRANSMIT_HARD_ERROR;
            }
            return TRANSMIT_SOFT_ERROR;
        }
        /* if res == 0 or res == -1 and error is not EAGAIN or EWOULDBLOCK,
           we have a real error, on which we close the connection */
        if (settings.verbose > 0) {
            if (res == -1) {
                log_socket_error(EXTENSION_LOG_WARNING, c,
                                 "Failed to write, and not due to blocking: %s");
            } else {
                int ii;
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                "%d - sendmsg returned 0\n",
                                                c->sfd);
                for (ii = 0; ii < m->msg_iovlen; ++ii) {
                    settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                                    "\t%d - %zu\n",
                                                    c->sfd, m->msg_iov[ii].iov_len);
                }

            }
        }

        conn_set_state(c, conn_closing);
        return TRANSMIT_HARD_ERROR;
    } else {
        if (c->ssl.enabled) {
            drain_bio_send_pipe(c);
            if (c->ssl.out.total) {
                if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                    if (settings.verbose > 0) {
                        settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                                        "Couldn't update event");
                    }
                    conn_set_state(c, conn_closing);
                    return TRANSMIT_HARD_ERROR;
                }
                return TRANSMIT_SOFT_ERROR;
            }
        }

        return TRANSMIT_COMPLETE;
    }
}

bool conn_listening(conn *c)
{
    SOCKET sfd;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    int curr_conns;
    int port_conns;
    struct listening_port *port_instance;

    if ((sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen)) == -1) {
#ifdef WIN32
        DWORD error = WSAGetLastError();
#else
        int error = errno;
#endif

        if (is_emfile(error)) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                            "Too many open files\n");
            disable_listen();
        } else if (!is_blocking(error)) {
            log_socket_error(EXTENSION_LOG_WARNING, c,
                             "Failed to accept new client: %s");
        }

        return false;
    }

    STATS_LOCK();
    curr_conns = ++stats.curr_conns;
    port_instance = get_listening_port_instance(c->parent_port);
    cb_assert(port_instance);
    port_conns = ++port_instance->curr_conns;
    STATS_UNLOCK();

    if (curr_conns >= settings.maxconns || port_conns >= port_instance->maxconns) {
        STATS_LOCK();
        ++stats.rejected_conns;
        --port_instance->curr_conns;
        STATS_UNLOCK();

        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "Too many open connections\n");

        safe_close(sfd);
        return false;
    }

    if (evutil_make_socket_nonblocking(sfd) == -1) {
        STATS_LOCK();
        --port_instance->curr_conns;
        STATS_UNLOCK();
        safe_close(sfd);
        return false;
    }

    dispatch_conn_new(sfd, c->parent_port, conn_new_cmd, EV_READ | EV_PERSIST,
                      DATA_BUFFER_SIZE);

    return false;
}

/**
 * Ship tap log to the other end. This state differs with all other states
 * in the way that it support full duplex dialog. We're listening to both read
 * and write events from libevent most of the time. If a read event occurs we
 * switch to the conn_read state to read and execute the input message (that would
 * be an ack message from the other side). If a write event occurs we continue to
 * send tap log to the other end.
 * @param c the tap connection to drive
 * @return true if we should continue to process work for this connection, false
 *              if we should start processing events for other connections.
 */
bool conn_ship_log(conn *c) {
    bool cont = false;
    short mask = EV_READ | EV_PERSIST | EV_WRITE;

    if (c->sfd == INVALID_SOCKET) {
        return false;
    }

    if (c->which & EV_READ || c->rbytes > 0) {
        if (c->rbytes > 0) {
            if (try_read_command(c) == 0) {
                conn_set_state(c, conn_read);
            }
        } else {
            conn_set_state(c, conn_read);
        }

        /* we're going to process something.. let's proceed */
        cont = true;

        /* We have a finite number of messages in the input queue */
        /* so let's process all of them instead of backing off after */
        /* reading a subset of them. */
        /* Why? Because we've got every time we're calling ship_tap_log */
        /* we try to send a chunk of items.. This means that if we end */
        /* up in a situation where we're receiving a burst of nack messages */
        /* we'll only process a subset of messages in our input queue, */
        /* and it will slowly grow.. */
        c->nevents = c->max_reqs_per_event;
    } else if (c->which & EV_WRITE) {
        --c->nevents;
        if (c->nevents >= 0) {
            c->ewouldblock = false;
            if (c->dcp) {
                ship_dcp_log(c);
            } else {
                ship_tap_log(c);
            }
            if (c->ewouldblock) {
                mask = EV_READ | EV_PERSIST;
            } else {
                cont = true;
            }
        }
    }

    if (!update_event(c, mask)) {
        if (settings.verbose > 0) {
            settings.extensions.logger->log(EXTENSION_LOG_INFO,
                                            c, "Couldn't update event\n");
        }
        conn_set_state(c, conn_closing);
    }

    return cont;
}

bool conn_waiting(conn *c) {
    if (!update_event(c, EV_READ | EV_PERSIST)) {
        if (settings.verbose > 0) {
            settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                            "Couldn't update event\n");
        }
        conn_set_state(c, conn_closing);
        return true;
    }
    conn_set_state(c, conn_read);
    return false;
}

bool conn_read(conn *c) {
    int res = try_read_network(c);
    switch (res) {
    case READ_NO_DATA_RECEIVED:
        conn_set_state(c, conn_waiting);
        break;
    case READ_DATA_RECEIVED:
        conn_set_state(c, conn_parse_cmd);
        break;
    case READ_ERROR:
        conn_set_state(c, conn_closing);
        break;
    case READ_MEMORY_ERROR: /* Failed to allocate more memory */
        /* State already set by try_read_network */
        break;
    }

    return true;
}

bool conn_parse_cmd(conn *c) {
    if (try_read_command(c) == 0) {
        /* wee need more data! */
        conn_set_state(c, conn_waiting);
    }

    return !c->ewouldblock;
}

bool conn_new_cmd(conn *c) {
    c->start = 0;
    --c->nevents;

    /*
     * In order to ensure that all clients will be served each
     * connection will only process a certain number of operations
     * before they will back off.
     */
    if (c->nevents >= 0) {
        reset_cmd_handler(c);
    } else {
        STATS_NOKEY(c, conn_yields);

        /*
         * If we've got data in the input buffer we might get "stuck"
         * if we're waiting for a read event. Why? because we might
         * already have all of the data for the next command in the
         * userspace buffer so the client is idle waiting for the
         * response to arrive. Lets set up a _write_ notification,
         * since that'll most likely be true really soon.
         */
        int block = (c->rbytes > 0);

        if (c->ssl.enabled) {
            char dummy;
            block |= SSL_peek(c->ssl.client, &dummy, 1);
        }

        /*
         * DCP and TAP connections is different from normal
         * connections in the way that they may not even get data from
         * the other end so that they'll _have_ to wait for a write event.
         */
        block |= c->dcp || (c->tap_iterator != NULL);

        if (block) {
            if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                settings.extensions.logger->log(EXTENSION_LOG_WARNING,
                                                c, "Couldn't update event");
                conn_set_state(c, conn_closing);
                return true;
            }
        }
        return false;
    }

    return true;
}

bool conn_swallow(conn *c) {
    ssize_t res;
#ifdef WIN32
    DWORD error;
#else
    int error;
#endif
    /* we are reading sbytes and throwing them away */
    if (c->sbytes == 0) {
        conn_set_state(c, conn_new_cmd);
        return true;
    }

    /* first check if we have leftovers in the conn_read buffer */
    if (c->rbytes > 0) {
        uint32_t tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
        c->sbytes -= tocopy;
        c->rcurr += tocopy;
        c->rbytes -= tocopy;
        return true;
    }

    /*  now try reading from the socket */
    res = do_data_recv(c, c->rbuf, c->rsize > c->sbytes ? c->sbytes : c->rsize);
#ifdef WIN32
    error = WSAGetLastError();
#else
    error = errno;
#endif
    if (res > 0) {
        STATS_ADD(c, bytes_read, res);
        c->sbytes -= res;
        return true;
    }
    if (res == 0) { /* end of stream */
        conn_set_state(c, conn_closing);
        return true;
    }
    if (res == -1 && is_blocking(error)) {
        if (!update_event(c, EV_READ | EV_PERSIST)) {
            if (settings.verbose > 0) {
                settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                                "Couldn't update event\n");
            }
            conn_set_state(c, conn_closing);
            return true;
        }
        return false;
    }

    /* otherwise we have a real error, on which we close the connection */
    if (!is_closed_conn(error)) {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "%d Failed to read, and not due to blocking (%%s)",
                 (int)c->sfd);

        log_socket_error(EXTENSION_LOG_INFO, c, msg);
    }

    conn_set_state(c, conn_closing);

    return true;
}

bool conn_nread(conn *c) {
    ssize_t res;
#ifdef WIN32
    DWORD error;
#else
    int error;
#endif

    if (c->rlbytes == 0) {
        bool block = c->ewouldblock = false;
        complete_nread(c);
        if (c->ewouldblock) {
            unregister_event(c);
            block = true;
        }
        return !block;
    }
    /* first check if we have leftovers in the conn_read buffer */
    if (c->rbytes > 0) {
        uint32_t tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
        if (c->ritem != c->rcurr) {
            memmove(c->ritem, c->rcurr, tocopy);
        }
        c->ritem += tocopy;
        c->rlbytes -= tocopy;
        c->rcurr += tocopy;
        c->rbytes -= tocopy;
        if (c->rlbytes == 0) {
            return true;
        }
    }

    /*  now try reading from the socket */
    res = do_data_recv(c, c->ritem, c->rlbytes);
#ifdef WIN32
    error = WSAGetLastError();
#else
    error = errno;
#endif
    if (res > 0) {
        STATS_ADD(c, bytes_read, res);
        if (c->rcurr == c->ritem) {
            c->rcurr += res;
        }
        c->ritem += res;
        c->rlbytes -= res;
        return true;
    }
    if (res == 0) { /* end of stream */
        conn_set_state(c, conn_closing);
        return true;
    }

    if (res == -1 && is_blocking(error)) {
        if (!update_event(c, EV_READ | EV_PERSIST)) {
            if (settings.verbose > 0) {
                settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                                "Couldn't update event\n");
            }
            conn_set_state(c, conn_closing);
            return true;
        }
        return false;
    }

    /* otherwise we have a real error, on which we close the connection */
    if (!is_closed_conn(error)) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, c,
                                        "%d Failed to read, and not due to blocking:\n"
                                        "errno: %d %s \n"
                                        "rcurr=%lx ritem=%lx rbuf=%lx rlbytes=%d rsize=%d\n",
                                        c->sfd, errno, strerror(errno),
                                        (long)c->rcurr, (long)c->ritem, (long)c->rbuf,
                                        (int)c->rlbytes, (int)c->rsize);
    }
    conn_set_state(c, conn_closing);
    return true;
}

bool conn_write(conn *c) {
    /*
     * We want to write out a simple response. If we haven't already,
     * assemble it into a msgbuf list (this will be a single-entry
     * list for TCP).
     */
    if (c->iovused == 0) {
        if (add_iov(c, c->wcurr, c->wbytes) != 0) {
            if (settings.verbose > 0) {
                settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                                "Couldn't build response\n");
            }
            conn_set_state(c, conn_closing);
            return true;
        }
    }

    return conn_mwrite(c);
}

bool conn_mwrite(conn *c) {
    switch (transmit(c)) {
    case TRANSMIT_COMPLETE:
        if (c->state == conn_mwrite) {
            while (c->ileft > 0) {
                item *it = *(c->icurr);
                settings.engine.v1->release(settings.engine.v0, c, it);
                c->icurr++;
                c->ileft--;
            }
            while (c->temp_alloc_left > 0) {
                char *temp_alloc_ = *(c->temp_alloc_curr);
                free(temp_alloc_);
                c->temp_alloc_curr++;
                c->temp_alloc_left--;
            }
            /* XXX:  I don't know why this wasn't the general case */
            conn_set_state(c, c->write_and_go);
        } else if (c->state == conn_write) {
            if (c->write_and_free) {
                free(c->write_and_free);
                c->write_and_free = 0;
            }
            conn_set_state(c, c->write_and_go);
        } else {
            if (settings.verbose > 0) {
                settings.extensions.logger->log(EXTENSION_LOG_INFO, c,
                                                "Unexpected state %d\n", c->state);
            }
            conn_set_state(c, conn_closing);
        }
        break;

    case TRANSMIT_INCOMPLETE:
    case TRANSMIT_HARD_ERROR:
        break;                   /* Continue in state machine. */

    case TRANSMIT_SOFT_ERROR:
        return false;
    }

    return true;
}

bool conn_pending_close(conn *c) {
    cb_assert(c->sfd == INVALID_SOCKET);
    settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                    "Awaiting clients to release the cookie (pending close for %p)",
                                    (void*)c);
    /*
     * tell the tap connection that we're disconnecting it now,
     * but give it a grace period
     */
    perform_callbacks(ON_DISCONNECT, NULL, c);

    if (c->refcount > 1) {
        return false;
    }

    conn_set_state(c, conn_immediate_close);
    return true;
}

bool conn_immediate_close(conn *c) {
    struct listening_port *port_instance;
    cb_assert(c->sfd == INVALID_SOCKET);
    settings.extensions.logger->log(EXTENSION_LOG_DETAIL, c,
                                    "Releasing connection %p",
                                    c);

    STATS_LOCK();
    port_instance = get_listening_port_instance(c->parent_port);
    cb_assert(port_instance);
    --port_instance->curr_conns;
    STATS_UNLOCK();

    perform_callbacks(ON_DISCONNECT, NULL, c);
    conn_close(c);

    return false;
}

bool conn_closing(conn *c) {
    /* We don't want any network notifications anymore.. */
    unregister_event(c);
    safe_close(c->sfd);
    c->sfd = INVALID_SOCKET;

    /* engine::release any allocated state */
    conn_cleanup_engine_allocations(c);

    if (c->refcount > 1 || c->ewouldblock) {
        conn_set_state(c, conn_pending_close);
    } else {
        conn_set_state(c, conn_immediate_close);
    }
    return true;
}

bool conn_setup_tap_stream(conn *c) {
    process_bin_tap_connect(c);
    return true;
}

bool conn_refresh_cbsasl(conn *c) {
    ENGINE_ERROR_CODE ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;

    cb_assert(ret != ENGINE_EWOULDBLOCK);

    switch (ret) {
    case ENGINE_SUCCESS:
        write_bin_response(c, NULL, 0, 0, 0);
        break;
    case ENGINE_DISCONNECT:
        conn_set_state(c, conn_closing);
        break;
    default:
        write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
    }

    return true;
}

bool conn_refresh_ssl_certs(conn *c) {
    ENGINE_ERROR_CODE ret = c->aiostat;
    c->aiostat = ENGINE_SUCCESS;
    c->ewouldblock = false;

    cb_assert(ret != ENGINE_EWOULDBLOCK);

    switch (ret) {
    case ENGINE_SUCCESS:
        write_bin_response(c, NULL, 0, 0, 0);
        break;
    case ENGINE_DISCONNECT:
        conn_set_state(c, conn_closing);
        break;
    default:
        write_bin_packet(c, engine_error_2_protocol_error(ret), 0);
    }

    return true;
}

void event_handler(evutil_socket_t fd, short which, void *arg) {
    conn *c = arg;
    LIBEVENT_THREAD *thr;

    cb_assert(c != NULL);

    if (memcached_shutdown) {
        event_base_loopbreak(c->event.ev_base);
        return ;
    }

    thr = c->thread;
    if (!is_listen_thread()) {
        cb_assert(thr);
        LOCK_THREAD(thr);
        /*
         * Remove the list from the list of pending io's (in case the
         * object was scheduled to run in the dispatcher before the
         * callback for the worker thread is executed.
         */
        c->thread->pending_io = list_remove(c->thread->pending_io, c);
    }

    c->which = which;

    /* sanity */
    cb_assert(fd == c->sfd);
    perform_callbacks(ON_SWITCH_CONN, c, c);
    c->nevents = c->max_reqs_per_event;

    do {
        if (settings.verbose) {
            settings.extensions.logger->log(EXTENSION_LOG_DEBUG, c,
                                            "%d - Running task: (%s)\n",
                                            c->sfd, state_text(c->state));
        }
    } while (c->state(c));

    if (thr) {
        UNLOCK_THREAD(thr);
    }
}

static void dispatch_event_handler(evutil_socket_t fd, short which, void *arg) {
    char buffer[80];
    ssize_t nr = recv(fd, buffer, sizeof(buffer), 0);

    if (nr != -1 && is_listen_disabled()) {
        bool enable = false;
        cb_mutex_enter(&listen_state.mutex);
        listen_state.count -= nr;
        if (listen_state.count <= 0) {
            enable = true;
            listen_state.disabled = false;
        }
        cb_mutex_exit(&listen_state.mutex);
        if (enable) {
            conn *next;
            for (next = listen_conn; next; next = next->next) {
                int backlog = 1024;
                int ii;
                update_event(next, EV_READ | EV_PERSIST);
                for (ii = 0; ii < settings.num_interfaces; ++ii) {
                    if (next->parent_port == settings.interfaces[ii].port) {
                        backlog = settings.interfaces[ii].backlog;
                        break;
                    }
                }

                if (listen(next->sfd, backlog) != 0) {
                    settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                                    "listen() failed",
                                                    strerror(errno));
                }
            }
        }
    }
}

/*
 * Sets a socket's send buffer size to the maximum allowed by the system.
 */
static void maximize_sndbuf(const SOCKET sfd) {
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;

    /* Start with the default size. */
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&old_size, &intsize) != 0) {
        if (settings.verbose > 0) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                            "getsockopt(SO_SNDBUF): %s",
                                            strerror(errno));
        }

        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size;
    max = MAX_SENDBUF_SIZE;

    while (min <= max) {
        avg = ((unsigned int)(min + max)) / 2;
        if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0) {
            last_good = avg;
            min = avg + 1;
        } else {
            max = avg - 1;
        }
    }

    if (settings.verbose > 1) {
        settings.extensions.logger->log(EXTENSION_LOG_DEBUG, NULL,
                 "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
    }
}

static SOCKET new_socket(struct addrinfo *ai) {
    SOCKET sfd;

    sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sfd == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    if (evutil_make_socket_nonblocking(sfd) == -1) {
        safe_close(sfd);
        return INVALID_SOCKET;
    }

    maximize_sndbuf(sfd);

    return sfd;
}

/**
 * Create a socket and bind it to a specific port number
 * @param interface the interface to bind to
 * @param port the port number to bind to
 * @param portnumber_file A filepointer to write the port numbers to
 *        when they are successfully added to the list of ports we
 *        listen on.
 */
static int server_socket(struct interface *interf, FILE *portnumber_file) {
    SOCKET sfd;
    struct linger ling = {0, 0};
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints;
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;
    int flags =1;
    char *host = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_socktype = SOCK_STREAM;

    if (interf->ipv4 && interf->ipv6) {
        hints.ai_family = AF_UNSPEC;
    } else if (interf->ipv4) {
        hints.ai_family = AF_INET;
    } else if (interf->ipv6) {
        hints.ai_family = AF_INET6;
    }

    snprintf(port_buf, sizeof(port_buf), "%u", (unsigned int)interf->port);

    if (interf->host) {
        if (strlen(interf->host) > 0 && strcmp(interf->host, "*") != 0) {
            host = interf->host;
        }
    }
    error = getaddrinfo(host, port_buf, &hints, &ai);
    if (error != 0) {
#ifdef WIN32
        log_errcode_error(EXTENSION_LOG_WARNING, NULL,
                          "getaddrinfo(): %s", error);
#else
        if (error != EAI_SYSTEM) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                     "getaddrinfo(): %s", gai_strerror(error));
        } else {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                     "getaddrinfo(): %s", strerror(error));
        }
#endif
        return 1;
    }

    for (next= ai; next; next= next->ai_next) {
        struct listening_port *port_instance;
        conn *listen_conn_add;
        if ((sfd = new_socket(next)) == INVALID_SOCKET) {
            /* getaddrinfo can return "junk" addresses,
             * we make sure at least one works before erroring.
             */
            continue;
        }

#ifdef IPV6_V6ONLY
        if (next->ai_family == AF_INET6) {
            error = setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &flags, sizeof(flags));
            if (error != 0) {
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                                "setsockopt(IPV6_V6ONLY): %s",
                                                strerror(errno));
                safe_close(sfd);
                continue;
            }
        }
#endif

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
        error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
        if (error != 0) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                            "setsockopt(SO_KEEPALIVE): %s",
                                            strerror(errno));
        }

        error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
        if (error != 0) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                            "setsockopt(SO_LINGER): %s",
                                            strerror(errno));
        }

        if (interf->tcp_nodelay) {
            error = setsockopt(sfd, IPPROTO_TCP,
                               TCP_NODELAY, (void *)&flags, sizeof(flags));
            if (error != 0) {
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                                "setsockopt(TCP_NODELAY): %s",
                                                strerror(errno));
            }
        }

        if (bind(sfd, next->ai_addr, (socklen_t)next->ai_addrlen) == SOCKET_ERROR) {
#ifdef WIN32
            DWORD error = WSAGetLastError();
#else
            int error = errno;
#endif
            if (!is_addrinuse(error)) {
                log_errcode_error(EXTENSION_LOG_WARNING, NULL,
                                  "bind(): %s", error);
                safe_close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            safe_close(sfd);
            continue;
        } else {
            success++;
            if (listen(sfd, interf->backlog) == SOCKET_ERROR) {
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                                "listen(): %s",
                                                strerror(errno));
                safe_close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            if (portnumber_file != NULL &&
                (next->ai_addr->sa_family == AF_INET ||
                 next->ai_addr->sa_family == AF_INET6)) {
                union {
                    struct sockaddr_in in;
                    struct sockaddr_in6 in6;
                } my_sockaddr;
                socklen_t len = sizeof(my_sockaddr);
                if (getsockname(sfd, (struct sockaddr*)&my_sockaddr, &len)==0) {
                    if (next->ai_addr->sa_family == AF_INET) {
                        fprintf(portnumber_file, "%s INET: %u\n", "TCP",
                                ntohs(my_sockaddr.in.sin_port));
                    } else {
                        fprintf(portnumber_file, "%s INET6: %u\n", "TCP",
                                ntohs(my_sockaddr.in6.sin6_port));
                    }
                }
            }
        }

        if (!(listen_conn_add = conn_new(sfd, interf->port, conn_listening,
                                         EV_READ | EV_PERSIST, 1,
                                         main_base, NULL))) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                            "failed to create listening connection\n");
            exit(EXIT_FAILURE);
        }
        listen_conn_add->next = listen_conn;
        listen_conn = listen_conn_add;
        STATS_LOCK();
        ++stats.curr_conns;
        ++stats.daemon_conns;
        port_instance = get_listening_port_instance(interf->port);
        cb_assert(port_instance);
        ++port_instance->curr_conns;
        STATS_UNLOCK();
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

static int server_sockets(FILE *portnumber_file) {
    int ret = 0;
    int ii = 0;

    for (ii = 0; ii < settings.num_interfaces; ++ii) {
        stats.listening_ports[ii].port = settings.interfaces[ii].port;
        stats.listening_ports[ii].maxconns = settings.interfaces[ii].maxconn;
        ret |= server_socket(settings.interfaces + ii, portnumber_file);
    }

    return ret;
}





#ifndef WIN32
static void save_pid(const char *pid_file) {
    FILE *fp;

    if (access(pid_file, F_OK) == 0) {
        if ((fp = fopen(pid_file, "r")) != NULL) {
            char buffer[1024];
            if (fgets(buffer, sizeof(buffer), fp) != NULL) {
                unsigned int pid;
                if (safe_strtoul(buffer, &pid) && kill((pid_t)pid, 0) == 0) {
                    settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                               "WARNING: The pid file contained the following (running) pid: %u\n", pid);
                }
            }
            fclose(fp);
        }
    }

    if ((fp = fopen(pid_file, "w")) == NULL) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                 "Could not open the pid file %s for writing: %s\n",
                 pid_file, strerror(errno));
        return;
    }

    fprintf(fp,"%ld\n", (long)getpid());
    if (fclose(fp) == -1) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                "Could not close the pid file %s: %s\n",
                pid_file, strerror(errno));
    }
}

static void remove_pidfile(const char *pid_file) {
    if (pid_file != NULL) {
        if (unlink(pid_file) != 0) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Could not remove the pid file %s: %s\n",
                    pid_file, strerror(errno));
        }
    }
}
#endif

#ifndef WIN32

#ifndef HAVE_SIGIGNORE
static int sigignore(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;

    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(sig, &sa, 0) == -1) {
        return -1;
    }
    return 0;
}
#endif /* !HAVE_SIGIGNORE */

static void sigterm_handler(int sig) {
    cb_assert(sig == SIGTERM || sig == SIGINT);
    memcached_shutdown = 1;
}
#endif

static int install_sigterm_handler(void) {
#ifndef WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;

    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGTERM, &sa, 0) == -1 ||
        sigaction(SIGINT, &sa, 0) == -1) {
        return -1;
    }
#endif

    return 0;
}

const char* get_server_version(void) {
    if (strlen(PRODUCT_VERSION) == 0) {
        return "unknown";
    } else {
        return PRODUCT_VERSION;
    }
}

static void store_engine_specific(const void *cookie,
                                  void *engine_data) {
    conn *c = (conn*)cookie;
    c->engine_storage = engine_data;
}

static void *get_engine_specific(const void *cookie) {
    conn *c = (conn*)cookie;
    return c->engine_storage;
}

static bool is_datatype_supported(const void *cookie) {
    conn *c = (conn*)cookie;
    return c->supports_datatype;
}

static uint8_t get_opcode_if_ewouldblock_set(const void *cookie) {
    conn *c = (conn*)cookie;
    uint8_t opcode = PROTOCOL_BINARY_CMD_INVALID;
    if (c->ewouldblock) {
        opcode = c->binary_header.request.opcode;
    }
    return opcode;
}

static bool validate_session_cas(const uint64_t cas) {
    bool ret = true;
    cb_mutex_enter(&(session_cas.mutex));
    if (cas != 0) {
        if (session_cas.value != cas) {
            ret = false;
        } else {
            session_cas.ctr++;
        }
    } else {
        session_cas.ctr++;
    }
    cb_mutex_exit(&(session_cas.mutex));
    return ret;
}

static void decrement_session_ctr() {
    cb_mutex_enter(&(session_cas.mutex));
    cb_assert(session_cas.ctr != 0);
    session_cas.ctr--;
    cb_mutex_exit(&(session_cas.mutex));
}

static SOCKET get_socket_fd(const void *cookie) {
    conn *c = (conn *)cookie;
    return c->sfd;
}

static ENGINE_ERROR_CODE reserve_cookie(const void *cookie) {
    conn *c = (conn *)cookie;
    ++c->refcount;
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE release_cookie(const void *cookie) {
    conn *c = (conn *)cookie;
    int notify;
    LIBEVENT_THREAD *thr;

    cb_assert(c);
    thr = c->thread;
    cb_assert(thr);
    LOCK_THREAD(thr);
    --c->refcount;

    /* Releasing the refererence to the object may cause it to change
     * state. (NOTE: the release call shall never be called from the
     * worker threads), so should put the connection in the pool of
     * pending IO and have the system retry the operation for the
     * connection
     */
    notify = add_conn_to_pending_io_list(c);
    UNLOCK_THREAD(thr);

    /* kick the thread in the butt */
    if (notify) {
        notify_thread(thr);
    }

    return ENGINE_SUCCESS;
}

static void cookie_set_admin(const void *cookie) {
    cb_assert(cookie);
    ((conn *)cookie)->admin = true;
}

static bool cookie_is_admin(const void *cookie) {
    if (settings.disable_admin) {
        return true;
    }
    cb_assert(cookie);
    return ((conn *)cookie)->admin;
}

static void cookie_set_priority(const void* cookie, CONN_PRIORITY priority) {
    conn* c = (conn*)cookie;
    switch (priority) {
    case CONN_PRIORITY_HIGH:
        c->max_reqs_per_event = settings.reqs_per_event_high_priority;
        break;
    case CONN_PRIORITY_MED:
        c->max_reqs_per_event = settings.reqs_per_event_med_priority;
        break;
    case CONN_PRIORITY_LOW:
        c->max_reqs_per_event = settings.reqs_per_event_low_priority;
        break;
    default:
        abort();
    }
}

static int num_independent_stats(void) {
    return settings.num_threads + 1;
}

static void *new_independent_stats(void) {
    int nrecords = num_independent_stats();
    struct thread_stats *ts = calloc(nrecords, sizeof(struct thread_stats));
    int ii;
    for (ii = 0; ii < nrecords; ii++) {
        cb_mutex_initialize(&ts[ii].mutex);
    }
    return ts;
}

static void release_independent_stats(void *stats) {
    int nrecords = num_independent_stats();
    struct thread_stats *ts = stats;
    int ii;
    for (ii = 0; ii < nrecords; ii++) {
        cb_mutex_destroy(&ts[ii].mutex);
    }
    free(ts);
}

static struct thread_stats* get_independent_stats(conn *c) {
    struct thread_stats *independent_stats;
    if (settings.engine.v1->get_stats_struct != NULL) {
        independent_stats = settings.engine.v1->get_stats_struct(settings.engine.v0, (const void *)c);
        if (independent_stats == NULL) {
            independent_stats = default_independent_stats;
        }
    } else {
        independent_stats = default_independent_stats;
    }
    return independent_stats;
}

static struct thread_stats *get_thread_stats(conn *c) {
    struct thread_stats *independent_stats;
    cb_assert(c->thread->index < num_independent_stats());
    independent_stats = get_independent_stats(c);
    return &independent_stats[c->thread->index];
}

static void register_callback(ENGINE_HANDLE *eh,
                              ENGINE_EVENT_TYPE type,
                              EVENT_CALLBACK cb, const void *cb_data) {
    struct engine_event_handler *h =
        calloc(sizeof(struct engine_event_handler), 1);

    cb_assert(h);
    h->cb = cb;
    h->cb_data = cb_data;
    h->next = engine_event_handlers[type];
    engine_event_handlers[type] = h;
}

static void count_eviction(const void *cookie, const void *key, const int nkey) {
    (void)cookie;
    (void)key;
    (void)nkey;
}

/**
 * To make it easy for engine implementors that doesn't want to care about
 * writing their own incr/decr code, they can just set the arithmetic function
 * to NULL and use this implementation. It is not efficient, due to the fact
 * that it does multiple calls through the interface (get and then cas store).
 * If you don't care, feel free to use it..
 */
static ENGINE_ERROR_CODE internal_arithmetic(ENGINE_HANDLE* handle,
                                             const void* cookie,
                                             const void* key,
                                             const int nkey,
                                             const bool increment,
                                             const bool create,
                                             const uint64_t delta,
                                             const uint64_t initial,
                                             const rel_time_t exptime,
                                             uint64_t *cas,
                                             uint8_t datatype,
                                             uint64_t *result,
                                             uint16_t vbucket)
{
    ENGINE_HANDLE_V1 *e = (ENGINE_HANDLE_V1*)handle;
    item *it = NULL;
    ENGINE_ERROR_CODE ret;

    ret = e->get(handle, cookie, &it, key, nkey, vbucket);

    if (ret == ENGINE_SUCCESS) {
        size_t nb;
        item *nit;
        char value[80];
        uint64_t val;
        item_info_holder info;
        item_info_holder i2;
        memset(&info, 0, sizeof(info));
        memset(&i2, 0, sizeof(i2));

        info.info.nvalue = 1;

        if (!e->get_item_info(handle, cookie, it, (void*)&info)) {
            e->release(handle, cookie, it);
            return ENGINE_FAILED;
        }

        if (info.info.value[0].iov_len > (sizeof(value) - 1)) {
            e->release(handle, cookie, it);
            return ENGINE_EINVAL;
        }

        memcpy(value, info.info.value[0].iov_base, info.info.value[0].iov_len);
        value[info.info.value[0].iov_len] = '\0';

        if (!safe_strtoull(value, &val)) {
            e->release(handle, cookie, it);
            return ENGINE_EINVAL;
        }

        if (increment) {
            val += delta;
        } else {
            if (delta > val) {
                val = 0;
            } else {
                val -= delta;
            }
        }

        nb = snprintf(value, sizeof(value), "%"PRIu64, val);
        *result = val;
        nit = NULL;
        if (e->allocate(handle, cookie, &nit, key,
                        nkey, nb, info.info.flags, info.info.exptime,
                        datatype) != ENGINE_SUCCESS) {
            e->release(handle, cookie, it);
            return ENGINE_ENOMEM;
        }

        i2.info.nvalue = 1;
        if (!e->get_item_info(handle, cookie, nit, (void*)&i2)) {
            e->release(handle, cookie, it);
            e->release(handle, cookie, nit);
            return ENGINE_FAILED;
        }

        memcpy(i2.info.value[0].iov_base, value, nb);
        e->item_set_cas(handle, cookie, nit, info.info.cas);
        ret = e->store(handle, cookie, nit, cas, OPERATION_CAS, vbucket);
        e->release(handle, cookie, it);
        e->release(handle, cookie, nit);
    } else if (ret == ENGINE_KEY_ENOENT && create) {
        char value[80];
        size_t nb = snprintf(value, sizeof(value), "%"PRIu64"\r\n", initial);
        item_info_holder info;
        memset(&info, 0, sizeof(info));
        info.info.nvalue = 1;

        *result = initial;
        if (e->allocate(handle, cookie, &it, key, nkey, nb, 0, exptime,
                        datatype) != ENGINE_SUCCESS) {
            e->release(handle, cookie, it);
            return ENGINE_ENOMEM;
        }

        if (!e->get_item_info(handle, cookie, it, (void*)&info)) {
            e->release(handle, cookie, it);
            return ENGINE_FAILED;
        }

        memcpy(info.info.value[0].iov_base, value, nb);
        ret = e->store(handle, cookie, it, cas, OPERATION_CAS, vbucket);
        e->release(handle, cookie, it);
    }

    /* We had a race condition.. just call ourself recursively to retry */
    if (ret == ENGINE_KEY_EEXISTS) {
        return internal_arithmetic(handle, cookie, key, nkey, increment, create, delta,
                                   initial, exptime, cas, datatype, result, vbucket);
    }

    return ret;
}

/**
 * Register an extension if it's not already registered
 *
 * @param type the type of the extension to register
 * @param extension the extension to register
 * @return true if success, false otherwise
 */
static bool register_extension(extension_type_t type, void *extension)
{
    if (extension == NULL) {
        return false;
    }

    switch (type) {
    case EXTENSION_DAEMON:
        {
            EXTENSION_DAEMON_DESCRIPTOR *ptr;
            for (ptr = settings.extensions.daemons; ptr != NULL; ptr = ptr->next) {
                if (ptr == extension) {
                    return false;
                }
            }
            ((EXTENSION_DAEMON_DESCRIPTOR *)(extension))->next = settings.extensions.daemons;
            settings.extensions.daemons = extension;
        }
        return true;
    case EXTENSION_LOGGER:
        settings.extensions.logger = extension;
        return true;

    case EXTENSION_BINARY_PROTOCOL:
        if (settings.extensions.binary != NULL) {
            EXTENSION_BINARY_PROTOCOL_DESCRIPTOR *last;
            for (last = settings.extensions.binary; last->next != NULL;
                 last = last->next) {
                if (last == extension) {
                    return false;
                }
            }
            if (last == extension) {
                return false;
            }
            last->next = extension;
            last->next->next = NULL;
        } else {
            settings.extensions.binary = extension;
            settings.extensions.binary->next = NULL;
        }

        ((EXTENSION_BINARY_PROTOCOL_DESCRIPTOR*)extension)->setup(setup_binary_lookup_cmd);
        return true;

    default:
        return false;
    }
}

/**
 * Unregister an extension
 *
 * @param type the type of the extension to remove
 * @param extension the extension to remove
 */
static void unregister_extension(extension_type_t type, void *extension)
{
    switch (type) {
    case EXTENSION_DAEMON:
        {
            EXTENSION_DAEMON_DESCRIPTOR *prev = NULL;
            EXTENSION_DAEMON_DESCRIPTOR *ptr = settings.extensions.daemons;

            while (ptr != NULL && ptr != extension) {
                prev = ptr;
                ptr = ptr->next;
            }

            if (ptr != NULL && prev != NULL) {
                prev->next = ptr->next;
            }

            if (ptr != NULL && settings.extensions.daemons == ptr) {
                settings.extensions.daemons = ptr->next;
            }
        }
        break;
    case EXTENSION_LOGGER:
        if (settings.extensions.logger == extension) {
            if (get_stderr_logger() == extension) {
                settings.extensions.logger = get_null_logger();
            } else {
                settings.extensions.logger = get_stderr_logger();
            }
        }
        break;
    case EXTENSION_BINARY_PROTOCOL:
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                        "You can't unregister a binary command handler!");
        abort();
        break;

    default:
        ;
    }

}

/**
 * Get the named extension
 */
static void* get_extension(extension_type_t type)
{
    switch (type) {
    case EXTENSION_DAEMON:
        return settings.extensions.daemons;

    case EXTENSION_LOGGER:
        return settings.extensions.logger;

    case EXTENSION_BINARY_PROTOCOL:
        return settings.extensions.binary;

    default:
        return NULL;
    }
}

static void shutdown_server(void) {
    memcached_shutdown = 1;
}

static EXTENSION_LOGGER_DESCRIPTOR* get_logger(void)
{
    return settings.extensions.logger;
}

static EXTENSION_LOG_LEVEL get_log_level(void)
{
    EXTENSION_LOG_LEVEL ret;
    switch (settings.verbose) {
    case 0: ret = EXTENSION_LOG_WARNING; break;
    case 1: ret = EXTENSION_LOG_INFO; break;
    case 2: ret = EXTENSION_LOG_DEBUG; break;
    default:
        ret = EXTENSION_LOG_DETAIL;
    }
    return ret;
}

static void set_log_level(EXTENSION_LOG_LEVEL severity)
{
    switch (severity) {
    case EXTENSION_LOG_WARNING: settings.verbose = 0; break;
    case EXTENSION_LOG_INFO: settings.verbose = 1; break;
    case EXTENSION_LOG_DEBUG: settings.verbose = 2; break;
    default:
        settings.verbose = 3;
    }
}

static void get_config_append_stats(const char *key, const uint16_t klen,
                                    const char *val, const uint32_t vlen,
                                    const void *cookie)
{
    char *pos;
    size_t nbytes;

    if (klen == 0  || vlen == 0) {
        return ;
    }

    pos = (char*)cookie;
    nbytes = strlen(pos);

    if ((nbytes + klen + vlen + 3) > 1024) {
        /* Not enough size in the buffer.. */
        return;
    }

    memcpy(pos + nbytes, key, klen);
    nbytes += klen;
    pos[nbytes] = '=';
    ++nbytes;
    memcpy(pos + nbytes, val, vlen);
    nbytes += vlen;
    memcpy(pos + nbytes, ";", 2);
}

static bool get_config(struct config_item items[]) {
    char config[1024];
    int rval;

    config[0] = '\0';
    process_stat_settings(get_config_append_stats, config);
    rval = parse_config(config, items, NULL);
    return rval >= 0;
}

/**
 * Callback the engines may call to get the public server interface
 * @return pointer to a structure containing the interface. The client should
 *         know the layout and perform the proper casts.
 */
static SERVER_HANDLE_V1 *get_server_api(void)
{
    static int init;
    static SERVER_CORE_API core_api;
    static SERVER_COOKIE_API server_cookie_api;
    static SERVER_STAT_API server_stat_api;
    static SERVER_LOG_API server_log_api;
    static SERVER_EXTENSION_API extension_api;
    static SERVER_CALLBACK_API callback_api;
    static ALLOCATOR_HOOKS_API hooks_api;
    static SERVER_HANDLE_V1 rv;

    if (!init) {
        init = 1;
        core_api.server_version = get_server_version;
        core_api.hash = hash;
        core_api.realtime = mc_time_convert_to_real_time;
        core_api.abstime = mc_time_convert_to_abs_time;
        core_api.get_current_time = mc_time_get_current_time;
        core_api.parse_config = parse_config;
        core_api.shutdown = shutdown_server;
        core_api.get_config = get_config;

        server_cookie_api.get_auth_data = get_auth_data;
        server_cookie_api.store_engine_specific = store_engine_specific;
        server_cookie_api.get_engine_specific = get_engine_specific;
        server_cookie_api.is_datatype_supported = is_datatype_supported;
        server_cookie_api.get_opcode_if_ewouldblock_set = get_opcode_if_ewouldblock_set;
        server_cookie_api.validate_session_cas = validate_session_cas;
        server_cookie_api.decrement_session_ctr = decrement_session_ctr;
        server_cookie_api.get_socket_fd = get_socket_fd;
        server_cookie_api.notify_io_complete = notify_io_complete;
        server_cookie_api.reserve = reserve_cookie;
        server_cookie_api.release = release_cookie;
        server_cookie_api.set_admin = cookie_set_admin;
        server_cookie_api.is_admin = cookie_is_admin;
        server_cookie_api.set_priority = cookie_set_priority;

        server_stat_api.new_stats = new_independent_stats;
        server_stat_api.release_stats = release_independent_stats;
        server_stat_api.evicting = count_eviction;

        server_log_api.get_logger = get_logger;
        server_log_api.get_level = get_log_level;
        server_log_api.set_level = set_log_level;

        extension_api.register_extension = register_extension;
        extension_api.unregister_extension = unregister_extension;
        extension_api.get_extension = get_extension;

        callback_api.register_callback = register_callback;
        callback_api.perform_callbacks = perform_callbacks;

        hooks_api.add_new_hook = mc_add_new_hook;
        hooks_api.remove_new_hook = mc_remove_new_hook;
        hooks_api.add_delete_hook = mc_add_delete_hook;
        hooks_api.remove_delete_hook = mc_remove_delete_hook;
        hooks_api.get_extra_stats_size = mc_get_extra_stats_size;
        hooks_api.get_allocator_stats = mc_get_allocator_stats;
        hooks_api.get_allocation_size = mc_get_allocation_size;
        hooks_api.get_detailed_stats = mc_get_detailed_stats;
        hooks_api.release_free_memory = mc_release_free_memory;

        rv.interface = 1;
        rv.core = &core_api;
        rv.stat = &server_stat_api;
        rv.extension = &extension_api;
        rv.callback = &callback_api;
        rv.log = &server_log_api;
        rv.cookie = &server_cookie_api;
        rv.alloc_hooks = &hooks_api;
    }

    if (rv.engine == NULL) {
        rv.engine = settings.engine.v0;
    }

    return &rv;
}

static void process_bin_dcp_response(conn *c) {
    char *packet;
    ENGINE_ERROR_CODE ret = ENGINE_DISCONNECT;

    c->supports_datatype = true;
    packet = (c->rcurr - (c->binary_header.request.bodylen + sizeof(c->binary_header)));
    if (settings.engine.v1->dcp.response_handler != NULL) {
        ret = settings.engine.v1->dcp.response_handler(settings.engine.v0, c,
                                                       (void*)packet);
    }

    if (ret == ENGINE_DISCONNECT) {
        conn_set_state(c, conn_closing);
    } else {
        conn_set_state(c, conn_ship_log);
    }
}


static void initialize_binary_lookup_map(void) {
    int ii;
    for (ii = 0; ii < 0x100; ++ii) {
        request_handlers[ii].descriptor = NULL;
        request_handlers[ii].callback = default_unknown_command;
    }

    response_handlers[PROTOCOL_BINARY_CMD_NOOP] = process_bin_noop_response;
    response_handlers[PROTOCOL_BINARY_CMD_TAP_MUTATION] = process_bin_tap_ack;
    response_handlers[PROTOCOL_BINARY_CMD_TAP_DELETE] = process_bin_tap_ack;
    response_handlers[PROTOCOL_BINARY_CMD_TAP_FLUSH] = process_bin_tap_ack;
    response_handlers[PROTOCOL_BINARY_CMD_TAP_OPAQUE] = process_bin_tap_ack;
    response_handlers[PROTOCOL_BINARY_CMD_TAP_VBUCKET_SET] = process_bin_tap_ack;
    response_handlers[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_START] = process_bin_tap_ack;
    response_handlers[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_END] = process_bin_tap_ack;

    response_handlers[PROTOCOL_BINARY_CMD_DCP_OPEN] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_ADD_STREAM] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_CLOSE_STREAM] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_STREAM_REQ] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_STREAM_END] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_MUTATION] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_DELETION] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_EXPIRATION] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_FLUSH] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_NOOP] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_CONTROL] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_RESERVED4] = process_bin_dcp_response;
}

/**
 * Load a shared object and initialize all the extensions in there.
 *
 * @param soname the name of the shared object (may not be NULL)
 * @param config optional configuration parameters
 * @return true if success, false otherwise
 */
bool load_extension(const char *soname, const char *config) {
    cb_dlhandle_t handle;
    void *symbol;
    EXTENSION_ERROR_CODE error;
    union my_hack {
        MEMCACHED_EXTENSIONS_INITIALIZE initialize;
        void* voidptr;
    } funky;
    char *error_msg;

    if (soname == NULL) {
        return false;
    }

    handle = cb_dlopen(soname, &error_msg);
    if (handle == NULL) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                        "Failed to open library \"%s\": %s\n",
                                        soname, error_msg);
        free(error_msg);
        return false;
    }

    symbol = cb_dlsym(handle, "memcached_extensions_initialize", &error_msg);
    if (symbol == NULL) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                        "Could not find symbol \"memcached_extensions_initialize\" in %s: %s\n",
                                        soname, error_msg);
        free(error_msg);
        return false;
    }
    funky.voidptr = symbol;

    error = (*funky.initialize)(config, get_server_api);
    if (error != EXTENSION_SUCCESS) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                "Failed to initalize extensions from %s. Error code: %d\n",
                soname, error);
        cb_dlclose(handle);
        return false;
    }

    if (settings.verbose > 0) {
        settings.extensions.logger->log(EXTENSION_LOG_INFO, NULL,
                "Loaded extensions from: %s\n", soname);
    }

    return true;
}

/**
 * Do basic sanity check of the runtime environment
 * @return true if no errors found, false if we can't use this env
 */
static bool sanitycheck(void) {
    /* One of our biggest problems is old and bogus libevents */
    const char *ever = event_get_version();
    if (ever != NULL) {
        if (strncmp(ever, "1.", 2) == 0) {
            /* Require at least 1.3 (that's still a couple of years old) */
            if ((ever[2] == '1' || ever[2] == '2') && !isdigit(ever[3])) {
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                        "You are using libevent %s.\nPlease upgrade to"
                        " a more recent version (1.3 or newer)\n",
                        event_get_version());
                return false;
            }
        }
    }

    return true;
}

/**
 * Log a socket error message.
 *
 * @param severity the severity to put in the log
 * @param cookie cookie representing the client
 * @param prefix What to put as a prefix (MUST INCLUDE
 *               the %s for where the string should go)
 */
void log_socket_error(EXTENSION_LOG_LEVEL severity,
                      const void* cookie,
                      const char* prefix)
{
#ifdef WIN32
    log_errcode_error(severity, cookie, prefix,
                      WSAGetLastError());
#else
    log_errcode_error(severity, cookie, prefix, errno);
#endif
}

/**
 * Log a system error message.
 *
 * @param severity the severity to put in the log
 * @param cookie cookie representing the client
 * @param prefix What to put as a prefix (MUST INCLUDE
 *               the %s for where the string should go)
 */
void log_system_error(EXTENSION_LOG_LEVEL severity,
                      const void* cookie,
                      const char* prefix)
{
#ifdef WIN32
    log_errcode_error(severity, cookie, prefix,
                      GetLastError());
#else
    log_errcode_error(severity, cookie, prefix, errno);
#endif
}

#ifdef WIN32
void log_errcode_error(EXTENSION_LOG_LEVEL severity,
                       const void* cookie,
                       const char* prefix, DWORD err) {
    LPVOID error_msg;

    if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL, err, 0,
                      (LPTSTR)&error_msg, 0, NULL) != 0) {
        settings.extensions.logger->log(severity, cookie,
                                        prefix, error_msg);
        LocalFree(error_msg);
    } else {
        settings.extensions.logger->log(severity, cookie,
                                        prefix, "unknown error");
    }
}
#else
void log_errcode_error(EXTENSION_LOG_LEVEL severity,
                       const void* cookie,
                       const char* prefix, int err) {
    settings.extensions.logger->log(severity,
                                    cookie,
                                    prefix,
                                    strerror(err));
}
#endif

#ifdef WIN32
static void parent_monitor_thread(void *arg) {
    HANDLE parent = arg;
    WaitForSingleObject(parent, INFINITE);
    ExitProcess(EXIT_FAILURE);
}

static void setup_parent_monitor(void) {
    char *env = getenv("MEMCACHED_PARENT_MONITOR");
    if (env != NULL) {
        HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, atoi(env));
        if (handle == INVALID_HANDLE_VALUE) {
            log_system_error(EXTENSION_LOG_WARNING, NULL,
                "Failed to open parent process: %s");
            exit(EXIT_FAILURE);
        }
        cb_create_thread(NULL, parent_monitor_thread, handle, 1);
    }
}

static void set_max_filehandles(void) {
    /* EMPTY */
}

#else
static void parent_monitor_thread(void *arg) {
    pid_t pid = atoi(arg);
    while (true) {
        sleep(1);
        if (kill(pid, 0) == -1 && errno == ESRCH) {
            _exit(1);
        }
    }
}

static void setup_parent_monitor(void) {
    char *env = getenv("MEMCACHED_PARENT_MONITOR");
    if (env != NULL) {
        cb_thread_t t;
        if (cb_create_thread(&t, parent_monitor_thread, env, 1) != 0) {
            log_system_error(EXTENSION_LOG_WARNING, NULL,
                "Failed to open parent process: %s");
            exit(EXIT_FAILURE);
        }
    }
}

static void set_max_filehandles(void) {
    struct rlimit rlim;

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                "failed to getrlimit number of files\n");
        exit(EX_OSERR);
    } else {
        int maxfiles = settings.maxconns + (3 * (settings.num_threads + 2));
        int syslimit = rlim.rlim_cur;
        if (rlim.rlim_cur < maxfiles) {
            rlim.rlim_cur = maxfiles;
        }
        if (rlim.rlim_max < rlim.rlim_cur) {
            rlim.rlim_max = rlim.rlim_cur;
        }
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            const char *fmt;
            int req;
            fmt = "WARNING: maxconns cannot be set to (%d) connections due to "
                "system\nresouce restrictions. Increase the number of file "
                "descriptors allowed\nto the memcached user process or start "
                "memcached as root (remember\nto use the -u parameter).\n"
                "The maximum number of connections is set to %d.\n";
            req = settings.maxconns;
            settings.maxconns = syslimit - (3 * (settings.num_threads + 2));
            if (settings.maxconns < 0) {
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                         "failed to set rlimit for open files. Try starting as"
                         " root or requesting smaller maxconns value.\n");
                exit(EX_OSERR);
            }
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                            fmt, req, settings.maxconns);
        }
    }
}

#endif

static cb_mutex_t *openssl_lock_cs;

static unsigned long get_thread_id(void) {
    return (unsigned long)cb_thread_self();
}

static void openssl_locking_callback(int mode, int type, char *file, int line)
{
    if (mode & CRYPTO_LOCK) {
        cb_mutex_enter(&(openssl_lock_cs[type]));
    } else {
        cb_mutex_exit(&(openssl_lock_cs[type]));
    }
}

static void initialize_openssl(void) {
    int ii;

    CRYPTO_malloc_init();
    SSL_library_init();
    SSL_load_error_strings();
    ERR_load_BIO_strings();
    OpenSSL_add_all_algorithms();

    openssl_lock_cs = calloc(CRYPTO_num_locks(), sizeof(cb_mutex_t));
    for (ii = 0; ii < CRYPTO_num_locks(); ii++) {
        cb_mutex_initialize(&(openssl_lock_cs[ii]));
    }

    CRYPTO_set_id_callback((unsigned long (*)())get_thread_id);
    CRYPTO_set_locking_callback((void (*)())openssl_locking_callback);
}

static void calculate_maxconns(void) {
    int ii;
    settings.maxconns = 0;
    for (ii = 0; ii < settings.num_interfaces; ++ii) {
        settings.maxconns += settings.interfaces[ii].maxconn;
    }
}

int main (int argc, char **argv) {
    ENGINE_HANDLE *engine_handle = NULL;

    // MB-14649 log() crash on windows on some CPU's
#ifdef _WIN64
    _set_FMA3_enable (0);
#endif

    initialize_openssl();

    initialize_timings();

    /* Initialize global variables */
    cb_mutex_initialize(&listen_state.mutex);
    cb_mutex_initialize(&connections.mutex);
    cb_mutex_initialize(&tap_stats.mutex);
    cb_mutex_initialize(&stats_lock);
    cb_mutex_initialize(&session_cas.mutex);

    session_cas.value = 0xdeadbeef;
    session_cas.ctr = 0;

    /* Initialize the socket subsystem */
    cb_initialize_sockets();

    init_alloc_hooks();

    /* init settings */
    settings_init();

    initialize_binary_lookup_map();

    setup_bin_packet_handlers();

    if (memcached_initialize_stderr_logger(get_server_api) != EXTENSION_SUCCESS) {
        fprintf(stderr, "Failed to initialize log system\n");
        return EX_OSERR;
    }

    if (!sanitycheck()) {
        return EX_OSERR;
    }

    {
        // MB-13642 Allow the user to specify the SSL cipher list
        //    If someone wants to use SSL we should try to be "secure
        //    by default", and only allow for using strong ciphers.
        //    Users that may want to use a less secure cipher list
        //    should be allowed to do so by setting an environment
        //    variable (since there is no place in the UI to do
        //    so currently). Whenever ns_server allows for specifying
        //    the SSL cipher list in the UI, it will be stored
        //    in memcached.json and override these settings.
        const char *env = getenv("COUCHBASE_SSL_CIPHER_LIST");
        if (env == NULL) {
            set_ssl_cipher_list("HIGH");
        } else {
            set_ssl_cipher_list(env);
        }
    }

    /* Parse command line arguments */
    parse_arguments(argc, argv);

    set_max_filehandles();

    if (install_sigterm_handler() != 0) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                                        "Failed to install SIGTERM handler\n");
        exit(EXIT_FAILURE);
    }

    /* Aggregate the maximum number of connections */
    calculate_maxconns();

    /* allocate the connection array */
    initialize_connections();

    cbsasl_server_init();

    /* initialize main thread libevent instance */
    main_base = event_base_new();

    /* Load the storage engine */
    if (!load_engine(settings.engine_module,
                     get_server_api,settings.extensions.logger,
                     &engine_handle)) {
        /* Error already reported */
        exit(EXIT_FAILURE);
    }

    if (!init_engine(engine_handle,
                     settings.engine_config,
                     settings.extensions.logger)) {
        return false;
    }

    if (settings.verbose > 0) {
        log_engine_details(engine_handle,settings.extensions.logger);
    }
    settings.engine.v1 = (ENGINE_HANDLE_V1 *) engine_handle;

    if (settings.engine.v1->arithmetic == NULL) {
        settings.engine.v1->arithmetic = internal_arithmetic;
    }

    setup_not_supported_handlers();

    /* initialize other stuff */
    stats_init();

    default_independent_stats = new_independent_stats();

#ifndef WIN32
    /* daemonize if requested */
    /* if we want to ensure our ability to dump core, don't chdir to / */
    if (settings.daemonize) {
        if (sigignore(SIGHUP) == -1) {
            settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                    "Failed to ignore SIGHUP: ", strerror(errno));
        }
        if (daemonize(1, settings.verbose) == -1) {
             settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                    "failed to daemon() in order to daemonize\n");
            exit(EXIT_FAILURE);
        }
    }

    /*
     * ignore SIGPIPE signals; we can use errno == EPIPE if we
     * need that information
     */
    if (sigignore(SIGPIPE) == -1) {
        settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                "failed to ignore SIGPIPE; sigaction");
        exit(EX_OSERR);
    }
#endif

    /* start up worker threads if MT mode */
    thread_init(settings.num_threads, main_base, dispatch_event_handler);

    /* Initialise memcached time keeping */
    mc_time_init(main_base);

    /* create the listening socket, bind it, and init */
    {
        const char *portnumber_filename = getenv("MEMCACHED_PORT_FILENAME");
        char temp_portnumber_filename[PATH_MAX];
        FILE *portnumber_file = NULL;

        if (portnumber_filename != NULL) {
            snprintf(temp_portnumber_filename,
                     sizeof(temp_portnumber_filename),
                     "%s.lck", portnumber_filename);

            portnumber_file = fopen(temp_portnumber_filename, "a");
            if (portnumber_file == NULL) {
                settings.extensions.logger->log(EXTENSION_LOG_WARNING, NULL,
                        "Failed to open \"%s\": %s\n",
                        temp_portnumber_filename, strerror(errno));
            }
        }

        if (server_sockets(portnumber_file)) {
            exit(EX_OSERR);
        }

        if (portnumber_file) {
            fclose(portnumber_file);
            rename(temp_portnumber_filename, portnumber_filename);
        }
    }

#ifndef WIN32
    if (settings.pid_file != NULL) {
        save_pid(settings.pid_file);
    }
#endif

    /* Drop privileges no longer needed */
    drop_privileges();

    /* Optional parent monitor */
    setup_parent_monitor();

    if (!memcached_shutdown) {
        /* enter the event loop */
        event_base_loop(main_base, 0);
    }

    if (settings.verbose) {
        settings.extensions.logger->log(EXTENSION_LOG_INFO, NULL,
                                        "Initiating shutdown\n");
    }
    threads_shutdown();

    settings.engine.v1->destroy(settings.engine.v0, false);

    threads_cleanup();

    /* remove the PID file if we're a daemon */
#ifndef WIN32
    if (settings.daemonize)
        remove_pidfile(settings.pid_file);
#endif

    /* Free the memory used by listening_port structure */
    if (stats.listening_ports) {
        free(stats.listening_ports);
    }

    event_base_free(main_base);
    release_independent_stats(default_independent_stats);
    destroy_connections();

    if (get_alloc_hooks_type() == none) {
        unload_engine();
    }

    free(settings.config);

    return EXIT_SUCCESS;
}
