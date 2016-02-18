/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef MEMCACHED_H
#define MEMCACHED_H

/** \file
 * The main memcached header holding commonly used data
 * structures and function prototypes.
 */
#include <event.h>
#include <platform/platform.h>
#include <cbsasl/cbsasl.h>

#include <memcached/openssl.h>

#include <memcached/protocol_binary.h>
#include <memcached/engine.h>
#include <memcached/extension.h>

#include "cache.h"

/** Maximum length of a key. */
#define KEY_MAX_LENGTH 250

/** Size of an incr buf. */
#define INCR_MAX_STORAGE_LEN 24

#define DATA_BUFFER_SIZE 2048
#define UDP_MAX_PAYLOAD_SIZE 1400
#define MAX_SENDBUF_SIZE (256 * 1024 * 1024)

/** Initial size of list of items being returned by "get". */
#define ITEM_LIST_INITIAL 200

/** Initial size of list of temprary auto allocates  */
#define TEMP_ALLOC_LIST_INITIAL 20

/** Initial size of the sendmsg() scatter/gather array. */
#define IOV_LIST_INITIAL 400

/** Initial number of sendmsg() argument structures to allocate. */
#define MSG_LIST_INITIAL 10

/** High water marks for buffer shrinking */
#define READ_BUFFER_HIGHWAT 8192
#define IOV_LIST_HIGHWAT 600
#define MSG_LIST_HIGHWAT 100

/* Slab sizing definitions. */
#define POWER_SMALLEST 1
#define POWER_LARGEST  200
#define CHUNK_ALIGN_BYTES 8
#define DONT_PREALLOC_SLABS
#define MAX_NUMBER_OF_SLAB_CLASSES (POWER_LARGEST + 1)


#define STAT_KEY_LEN 128
#define STAT_VAL_LEN 128

#define DEFAULT_REQS_PER_EVENT     20
#define DEFAULT_REQS_PER_TAP_EVENT 50

/** Append a simple stat with a stat name, value format and value */
#define APPEND_STAT(name, fmt, val) \
    append_stat(name, add_stats, c, fmt, val);

/** Append an indexed stat with a stat name (with format), value format
    and value */
#define APPEND_NUM_FMT_STAT(name_fmt, num, name, fmt, val)          \
    klen = snprintf(key_str, STAT_KEY_LEN, name_fmt, num, name);    \
    vlen = snprintf(val_str, STAT_VAL_LEN, fmt, val);               \
    add_stats(key_str, klen, val_str, vlen, c);

/** Common APPEND_NUM_FMT_STAT format. */
#define APPEND_NUM_STAT(num, name, fmt, val) \
    APPEND_NUM_FMT_STAT("%d:%s", num, name, fmt, val)

enum bin_substates {
    bin_no_state,
    bin_reading_set_header,
    bin_reading_cas_header,
    bin_read_set_value,
    bin_reading_sasl_auth,
    bin_reading_sasl_auth_data,
    bin_reading_packet
};

/** Stats stored per slab (and per thread). */
struct slab_stats {
    uint64_t  cmd_set;
    uint64_t  get_hits;
    uint64_t  delete_hits;
    uint64_t  cas_hits;
    uint64_t  cas_badval;
};

/**
 * Stats stored per-thread.
 */
struct thread_stats {
    cb_mutex_t   mutex;
    uint64_t          cmd_get;
    uint64_t          get_misses;
    uint64_t          delete_misses;
    uint64_t          incr_misses;
    uint64_t          decr_misses;
    uint64_t          incr_hits;
    uint64_t          decr_hits;
    uint64_t          cas_misses;
    uint64_t          bytes_read;
    uint64_t          bytes_written;
    uint64_t          cmd_flush;
    uint64_t          conn_yields; /* # of yields for connections (-R option)*/
    uint64_t          auth_cmds;
    uint64_t          auth_errors;
    struct slab_stats slab_stats[MAX_NUMBER_OF_SLAB_CLASSES];
};

/**
 * Listening port.
 */
struct listening_port {
    int port;
    int curr_conns;
    int maxconns;
};

/**
 * Global stats.
 */
struct stats {
    cb_mutex_t mutex;
    unsigned int  daemon_conns; /* conns used by the server */
    unsigned int  curr_conns;
    unsigned int  total_conns;
    unsigned int  conn_structs;
    time_t        started;          /* when the process was started */
    uint64_t      rejected_conns; /* number of times I reject a client */
    struct listening_port *listening_ports;
};

#define MAX_VERBOSITY_LEVEL 2

struct interface {
    char *host;
    struct {
        const char *key;
        const char *cert;
    } ssl;
    int maxconn;
    int backlog;
    in_port_t port;
    bool ipv6;
    bool ipv4;
    bool tcp_nodelay;
};

/* When adding a setting, be sure to update process_stat_settings */
/**
 * Globally accessible settings as derived from the commandline.
 */
struct settings {
    int maxconns;
    struct interface *interfaces;
    int num_interfaces;
    int verbose;
    int num_threads;        /* number of worker (without dispatcher) libevent threads to run */
    char prefix_delimiter;  /* character that marks a key prefix (for stats) */
    bool detail_enabled;    /* nonzero if we're collecting detailed stats */
    bool allow_detailed;    /* detailed stats commands are allowed */

    /* Maximum number of io events to process based on the priority of the
       connection */
    int reqs_per_event_high_priority;
    int reqs_per_event_med_priority;
    int reqs_per_event_low_priority;
    int default_reqs_per_event;

    bool sasl;              /* SASL on/off */
    bool require_sasl;      /* require SASL auth */
    int topkeys;            /* Number of top keys to track */
    union {
        ENGINE_HANDLE *v0;
        ENGINE_HANDLE_V1 *v1;
    } engine;
    struct {
        EXTENSION_DAEMON_DESCRIPTOR *daemons;
        EXTENSION_LOGGER_DESCRIPTOR *logger;
        EXTENSION_BINARY_PROTOCOL_DESCRIPTOR *binary;
    } extensions;
    size_t bio_drain_buffer_sz;
    bool tcp_nodelay;
    char *engine_module;
    char *engine_config;
    char *pid_file;
    bool daemonize;
    char *config;      /* The configuration specified by -C (json) */
    char *admin;
    bool disable_admin;
    bool datatype;
};

struct engine_event_handler {
    EVENT_CALLBACK cb;
    const void *cb_data;
    struct engine_event_handler *next;
};

extern struct stats stats;
extern struct settings settings;

enum thread_type {
    GENERAL = 11,
    TAP = 13,
    DISPATCHER = 15
};

typedef struct {
    cb_thread_t thread_id;      /* unique ID of this thread */
    struct event_base *base;    /* libevent handle this thread uses */
    struct event notify_event;  /* listen event for notify pipe */
    SOCKET notify[2];           /* notification pipes */
    struct conn_queue *new_conn_queue; /* queue of new connections to handle */
    cb_mutex_t mutex;      /* Mutex to lock protect access to the pending_io */
    bool is_locked;
    struct conn *pending_io;    /* List of connection with pending async io ops */
    int index;                  /* index of this thread in the threads array */
    enum thread_type type;      /* Type of IO this thread processes */

    rel_time_t last_checked;
} LIBEVENT_THREAD;

#define LOCK_THREAD(t)                          \
    cb_mutex_enter(&t->mutex);                  \
    cb_assert(t->is_locked == false);              \
    t->is_locked = true;

#define UNLOCK_THREAD(t)                         \
    cb_assert(t->is_locked == true);                \
    t->is_locked = false;                        \
    cb_mutex_exit(&t->mutex);

extern void notify_thread(LIBEVENT_THREAD *thread);
extern void notify_dispatcher(void);
extern bool create_notification_pipe(LIBEVENT_THREAD *me);

typedef struct conn conn;
typedef bool (*STATE_FUNC)(conn *);

/**
 * The structure representing a connection into memcached.
 */
struct conn {
    SOCKET sfd;
    int max_reqs_per_event; /** The maximum requests we can process in a worker
                                thread timeslice */
    int nevents;
    bool admin;
    cbsasl_conn_t *sasl_conn;
    STATE_FUNC   state;
    enum bin_substates substate;
    bool   registered_in_libevent;
    struct event event;
    short  ev_flags;
    short  which;   /** which events were just triggered */
    char   *rbuf;   /** buffer to read commands into */
    char   *rcurr;  /** but if we parsed some already, this is where we stopped */
    uint32_t rsize;   /** total allocated size of rbuf */
    uint32_t rbytes;  /** how much data, starting from rcur, do we have unparsed */

    char   *wbuf;
    char   *wcurr;
    uint32_t wsize;
    uint32_t wbytes;
    /** which state to go into after finishing current write */
    STATE_FUNC   write_and_go;
    void   *write_and_free; /** free this memory after finishing writing */

    char   *ritem;  /** when we read in an item's value, it goes here */
    uint32_t rlbytes;

    /* data for the nread state */

    /**
     * item is used to hold an item structure created after reading the command
     * line of set/add/replace commands, but before we finished reading the actual
     * data. The data is read into ITEM_data(item) to avoid extra copying.
     */

    void   *item;     /* for commands set/add/replace  */
    ENGINE_STORE_OPERATION    store_op; /* which one is it: set/add/replace */


    /* data for the swallow state */
    uint32_t sbytes;    /* how many bytes to swallow */

    /* data for the mwrite state */
    struct iovec *iov;
    int    iovsize;   /* number of elements allocated in iov[] */
    int    iovused;   /* number of elements used in iov[] */

    struct msghdr *msglist;
    int    msgsize;   /* number of elements allocated in msglist[] */
    int    msgused;   /* number of elements used in msglist[] */
    int    msgcurr;   /* element in msglist[] being transmitted now */
    int    msgbytes;  /* number of bytes in current msg */

    item   **ilist;   /* list of items to write out */
    int    isize;
    item   **icurr;
    int    ileft;

    char   **temp_alloc_list;
    int    temp_alloc_size;
    char   **temp_alloc_curr;
    int    temp_alloc_left;

    struct sockaddr_storage request_addr; /* Who sent the most recent request */
    socklen_t request_addr_size;
    int    hdrsize;   /* number of headers' worth of space is allocated */

    bool   noreply;   /* True if the reply should not be sent. */
    /* current stats command */

    uint8_t refcount; /* number of references to the object */
    bool   supports_datatype;

    struct {
        char *buffer;
        size_t size;
        size_t offset;
    } dynamic_buffer;

    void *engine_storage;
    hrtime_t start;

    /* Binary protocol stuff */
    /* This is where the binary header goes */
    protocol_binary_request_header binary_header;
    uint64_t cas; /* the cas to return */
    short cmd; /* current command being processed */
    int opaque;
    int keylen;

    int list_state; /* bitmask of list state data for this connection */
    conn   *next;     /* Used for generating a list of conn structures */
    LIBEVENT_THREAD *thread; /* Pointer to the thread object serving this connection */

    ENGINE_ERROR_CODE aiostat;
    bool ewouldblock;
    TAP_ITERATOR tap_iterator;
    in_port_t parent_port; /* Listening port that creates this connection instance */

    int dcp;

    /* Clean up this at one point.. */
    struct {
        struct {
            char *buffer;
            int buffsz;
            int total;
            int current;
        } in, out;

        bool enabled;
        bool error;
        SSL_CTX *ctx;
        SSL *client;

        bool connected;
        BIO *application;
        BIO *network;
    } ssl;

    /* The revision number of the clustermap sent to the client */
    int clustermap_revno;
};

/* States for the connection list_state */
#define LIST_STATE_PROCESSING 1
#define LIST_STATE_REQ_PENDING_IO 2
#define LIST_STATE_REQ_PENDING_CLOSE 4

/*
 * Functions
 */
conn *conn_new(const SOCKET sfd, in_port_t parent_port,
               STATE_FUNC init_state, int event_flags,
               unsigned int read_buffer_size, struct event_base *base,
               struct timeval *timeout);
#ifndef WIN32
extern int daemonize(int nochdir, int noclose);
#endif

#include "stats.h"
#include "trace.h"
#include "hash.h"
#include <memcached/util.h>

/*
 * Functions to add / update the connection to libevent
 */
bool register_event(conn *c, struct timeval *timeout);
bool unregister_event(conn *c);
bool update_event(conn *c, const int new_flags);

/*
 * Functions such as the libevent-related calls that need to do cross-thread
 * communication in multithreaded mode (rather than actually doing the work
 * in the current thread) are called via "dispatch_" frontends, which are
 * also #define-d to directly call the underlying code in singlethreaded mode.
 */

void thread_init(int nthreads, struct event_base *main_base,
                 void (*dispatcher_callback)(evutil_socket_t, short, void *));
void threads_shutdown(void);
void threads_cleanup(void);

int  dispatch_event_add(int thread, conn *c);
void dispatch_conn_new(SOCKET sfd, int parent_port,
                       STATE_FUNC init_state, int event_flags,
                       int read_buffer_size);

/* Lock wrappers for cache functions that are called from main loop. */
void accept_new_conns(const bool do_accept);
conn *conn_from_freelist(void);
bool  conn_add_to_freelist(conn *c);
int   is_listen_thread(void);

void STATS_LOCK(void);
void STATS_UNLOCK(void);
void threadlocal_stats_clear(struct thread_stats *stats);
void threadlocal_stats_reset(struct thread_stats *thread_stats);
void threadlocal_stats_aggregate(struct thread_stats *thread_stats, struct thread_stats *stats);
void slab_stats_aggregate(struct thread_stats *stats, struct slab_stats *out);

/* Stat processing functions */
void append_stat(const char *name, ADD_STAT add_stats, conn *c,
                 const char *fmt, ...);

void notify_io_complete(const void *cookie, ENGINE_ERROR_CODE status);
void conn_set_state(conn *c, STATE_FUNC state);
const char *state_text(STATE_FUNC state);
void safe_close(SOCKET sfd);


/* Number of times this connection is in the given pending list */
int number_of_pending(conn *c, conn *pending);
bool has_cycle(conn *c);
bool list_contains(conn *h, conn *n);
conn *list_remove(conn *h, conn *n);
size_t list_to_array(conn **dest, size_t max_items, conn **l);
void enlist_conn(conn *c, conn **list);
void finalize_list(conn **list, size_t items);
bool set_socket_nonblocking(SOCKET sfd);

void conn_close(conn *c);
bool load_extension(const char *soname, const char *config);

int add_conn_to_pending_io_list(conn *c);

extern void drop_privileges(void);

/* connection state machine */
bool conn_listening(conn *c);
bool conn_new_cmd(conn *c);
bool conn_waiting(conn *c);
bool conn_read(conn *c);
bool conn_parse_cmd(conn *c);
bool conn_write(conn *c);
bool conn_nread(conn *c);
bool conn_swallow(conn *c);
bool conn_pending_close(conn *c);
bool conn_immediate_close(conn *c);
bool conn_closing(conn *c);
bool conn_mwrite(conn *c);
bool conn_ship_log(conn *c);
bool conn_setup_tap_stream(conn *c);
bool conn_refresh_cbsasl(conn *c);
bool conn_refresh_ssl_certs(conn *c);

void log_socket_error(EXTENSION_LOG_LEVEL severity,
                      const void* client_cookie,
                      const char* prefix);
#ifdef WIN32
void log_errcode_error(EXTENSION_LOG_LEVEL severity,
                          const void* client_cookie,
                          const char* prefix, DWORD err);
#else
void log_errcode_error(EXTENSION_LOG_LEVEL severity,
                          const void* client_cookie,
                          const char* prefix, int err);
#endif
void log_system_error(EXTENSION_LOG_LEVEL severity,
                      const void* cookie,
                      const char* prefix);


void read_config_file(const char *filename);
void perform_callbacks(ENGINE_EVENT_TYPE type,
                       const void *data,
                       const void *c);

const char* get_server_version(void);

#endif
