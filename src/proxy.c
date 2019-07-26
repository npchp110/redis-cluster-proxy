/*
 * Copyright (C) 2019  Giuseppe Fabio Nicotra <artix2 at gmail dot com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "proxy.h"
#include "config.h"
#include "logger.h"
#include "zmalloc.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define DEFAULT_PORT            7777
#define DEFAULT_MAX_CLIENTS     10000
#define MAX_THREADS             500
#define DEFAULT_THREADS         8
#define DEFAULT_TCP_KEEPALIVE   300
#define DEFAULT_TCP_BACKLOG     511
#define QUERY_OFFSETS_MIN_SIZE  10
#define EL_INSTALL_HANDLER_FAIL 9999
#define REQ_STATUS_UNKNOWN      -1
#define PARSE_STATUS_INCOMPLETE -1
#define UNDEFINED_SLOT          -1
#define PARSE_STATUS_ERROR      0
#define PARSE_STATUS_OK         1

#define MAX_ACCEPTS             1000
#define NET_IP_STR_LEN          46

#define THREAD_IO_READ          0
#define THREAD_IO_WRITE         1

#define QUEUE_TYPE_SENDING      1
#define QUEUE_TYPE_PENDING      2

#define UNUSED(V) ((void) V)

#define getCluster(c) \
    (c->cluster ? c->cluster : proxy.threads[c->thread_id]->cluster)
#define enqueueRequestToSend(req) (enqueueRequest(req, QUEUE_TYPE_SENDING))
#define dequeueRequestToSend(req) (dequeueRequest(req, QUEUE_TYPE_SENDING))
#define enqueuePendingRequest(req) (enqueueRequest(req, QUEUE_TYPE_PENDING))
#define dequeuePendingRequest(req) (dequeueRequest(req, QUEUE_TYPE_PENDING))
#define getFirstRequestToSend(node, isempty) \
    (getFirstQueuedRequest(node->connection->requests_to_send,\
     isempty))
#define getFirstRequestPending(node, isempty) \
    (getFirstQueuedRequest(node->connection->requests_pending,\
     isempty))

typedef struct proxyThread {
    int thread_id;
    int io[2];
    pthread_t thread;
    redisCluster *cluster;
    aeEventLoop *loop;
    list *clients;
    list *pending_messages;
    uint64_t next_client_id;
    sds msgbuffer;
} proxyThread;

redisClusterProxy proxy;
redisClusterProxyConfig config;

/* Forward declarations. */

static proxyThread *createProxyThread(int index);
static void freeProxyThread(proxyThread *thread);
static void *execProxyThread(void *ptr);
static client *createClient(int fd, char *ip);
static void freeClient(client *c);
static clientRequest *createRequest(client *c);
void readQuery(aeEventLoop *el, int fd, void *privdata, int mask);
static int writeToClient(client *c);
static int writeToCluster(aeEventLoop *el, int fd, clientRequest *req);
static void writeToClusterHandler(aeEventLoop *el, int fd, void *privdata,
                                  int mask);
static void readClusterReply(aeEventLoop *el, int fd, void *privdata, int mask);
static clusterNode *getRequestNode(clientRequest *req, sds *err);
static clientRequest *handleNextRequestToCluster(clusterNode *node);
static clientRequest *getFirstQueuedRequest(list *queue, int *is_empty);
static int enqueueRequest(clientRequest *req, int queue_type);
static void dequeueRequest(clientRequest *req, int queue_type);
static int sendMessageToThread(proxyThread *thread, sds buf);
static int installIOHandler(aeEventLoop *el, int fd, int mask, aeFileProc *proc,
                            void *data, int retried);
static int disableMultiplexingForClient(client *c);

/* Hiredis helpers */

int processItem(redisReader *r);

/* This function does the same things as redisReaderGetReply, but
 * it does not trim the reader's buffer, in order to let the proxy's
 * read handler to get the full reply's buffer. Consuming and trimming
 * ther reader's buffer is up to the proxy. */

static int __hiredisReadReplyFromBuffer(redisReader *r, void **reply) {
    /* Default target pointer to NULL. */
    if (reply != NULL)
        *reply = NULL;

    /* Return early when this reader is in an erroneous state. */
    if (r->err)
        return REDIS_ERR;

    /* When the buffer is empty, there will never be a reply. */
    if (r->len == 0)
        return REDIS_OK;

    /* Set first item to process when the stack is empty. */
    if (r->ridx == -1) {
        r->rstack[0].type = -1;
        r->rstack[0].elements = -1;
        r->rstack[0].idx = -1;
        r->rstack[0].obj = NULL;
        r->rstack[0].parent = NULL;
        r->rstack[0].privdata = r->privdata;
        r->ridx = 0;
    }

    /* Process items in reply. */
    while (r->ridx >= 0)
        if (processItem(r) != REDIS_OK)
            break;

    /* Return ASAP when an error occurred. */
    if (r->err)
        return REDIS_ERR;

    /* Emit a reply when there is one. */
    if (r->ridx == -1) {
        if (reply != NULL)
            *reply = r->reply;
        r->reply = NULL;
    }
    return REDIS_OK;
}

/* Custom Commands */

static sds proxySubCommandConfig(clientRequest *r, sds option, sds value,
                                 sds *err)
{
    void *opt = NULL;
    sds reply = NULL;
    int ok = 0;
    int is_int = 0, is_float = 0, is_string = 0, read_only = 0;
    UNUSED(is_float);
    UNUSED(is_string);
    if (strcmp("log-level", option) == 0) {
        opt = &(config.loglevel);
    } else if (strcmp("threads", option) == 0) {
        is_int = 1;
        opt = &(config.num_threads);
        read_only = 1;
    } else if (strcmp("max-clients", option) == 0) {
        is_int = 1;
        opt = &(config.maxclients);
        read_only = 1;
    } else if (strcmp("tcpkeepalive", option) == 0) {
        is_int = 1;
        opt = &(config.tcpkeepalive);
        read_only = 1;
    } else if (strcmp("tcp-backlog", option) == 0) {
        is_int = 1;
        opt = &(config.tcp_backlog);
        read_only = 1;
    } else if (strcmp("daemonize", option) == 0) {
        is_int = 1;
        opt = &(config.daemonize);
        read_only = 1;
    } else if (strcmp("dump-queries", option) == 0) {
        is_int = 1;
        opt = &(config.dump_queries);
    } else if (strcmp("dump-buffer", option) == 0) {
        is_int = 1;
        opt = &(config.dump_buffer);
    } else if (strcmp("dump-replies", option) == 0) {
        is_int = 1;
        opt = &(config.dump_queues);
    }
    if (opt == NULL) {
        if (err) *err = sdsnew("Invalid config option");
        return NULL;
    }
    if (opt == &(config.loglevel)) {
        if (value != NULL) {
            int i, level = -1;
            for (i = 0; i < LOGLEVEL_ERROR; i++) {
                if (!strcasecmp(value, redisProxyLogLevels[i])) {
                    level = i;
                    break;
                }
            }
            if (level == -1) {
                *err = sdsnew("Invalid log-level");
                return NULL;
            }
            config.loglevel = level;
            ok = 1;
        } else {
            reply = sdsnew(redisProxyLogLevels[config.loglevel]);
        }
    } else {
        if (value == NULL) {
            if (!initReplyArray(r->client)) {
                *err = sdsnew("Out of memory");
                return NULL;
            }
            addReplyString(r->client, option, r->id);
            if (is_int) addReplyInt(r->client, *((int *) opt), r->id);
            addReplyArray(r->client, r->id);
        } else {
            if (read_only) *err = sdsnew("This config option is read-only");
            else {
                if (is_int) {
                    *((int *) opt) = atoi(value);
                    ok = 1;
                }
            }
        }
    }
    if (reply == NULL && ok) reply = sdsnew("OK");
    return reply;
}

int proxyCommand(void *r) {
    clientRequest *req = r;
    sds subcmd = NULL, err = NULL;
    if (req->argc < 2) {
        err = sdsnew("Wrong number of arguments for command PROXY");
        goto final;
    }
    assert(req->offsets_size >= 2);
    int buflen = sdslen(req->buffer);
    int offset = req->offsets[1];
    int len = req->lengths[1];
    char *p = req->buffer + offset, *end = req->buffer + buflen;
    assert(p < end);
    assert((p + len) < end);
    subcmd = sdsnewlen(p, len);
    if (strcasecmp("config", subcmd) == 0) {
        if (req->argc < 3) {
            err = sdsnew("Missing PROXY CONFIG action (GET | SET)");
            goto final;
        }
        assert(req->offsets_size >= 3);
        offset = req->offsets[2];
        len = req->lengths[2];
        p = req->buffer + offset, end = req->buffer + buflen;
        assert(p < end);
        assert((p + len) < end);
        sds action = sdsnewlen(p, len), value = NULL;
        int is_set_action = 0;
        if (strcasecmp("get", action) == 0 || strcasecmp("set", action) == 0) {
            is_set_action = (action[0] == 's' || action[0] == 'S');
        } else {
            err = sdsnew("Unsupported PROXY CONFIG action ");
            err = sdscatfmt(err, "'%S'", action);
            sdsfree(action);
            goto final;
        }
        sdsfree(action);
        if (req->argc < 4)
            err = sdsnew("Missing config option name");
        else if (is_set_action && req->argc < 5)
            err = sdsnew("Missing config option value");
        if (err != NULL) goto final;
        assert(req->offsets_size >= 4);
        offset = req->offsets[3];
        len = req->lengths[3];
        p = req->buffer + offset, end = req->buffer + buflen;
        assert(p < end);
        assert((p + len) < end);
        sds option = sdsnewlen(p, len);
        if (is_set_action) {
            assert(req->offsets_size >= 5);
            offset = req->offsets[4];
            len = req->lengths[4];
            p = req->buffer + offset, end = req->buffer + buflen;
            assert(p < end);
            assert((p + len) < end);
            value = sdsnewlen(p, len);
        }
        sds reply = proxySubCommandConfig(req, option, value, &err);
        if (reply != NULL) {
            addReplyString(req->client, reply, req->id);
            sdsfree(reply);
        }
        sdsfree(option);
        if (value != NULL) sdsfree(value);
    } else if (strcasecmp("multiplexing", subcmd) == 0) {
        assert(req->offsets_size >= 3);
        offset = req->offsets[2];
        len = req->lengths[2];
        p = req->buffer + offset, end = req->buffer + buflen;
        assert(p < end);
        assert((p + len) < end);
        sds action = sdsnewlen(p, len);
        if (strcasecmp("status", action) == 0) {
            char *status = (req->client->cluster != NULL ? "off" : "on");
            addReplyString(req->client, status, req->id);
        } else if (strcasecmp("off", action) == 0) {
            if (disableMultiplexingForClient(req->client))
                addReplyString(req->client, "OK", req->id);
            else
                err = sdsnew("Failed to disable multiplexing");
        } else
            err = sdsnew("Unsupported action. Supported actions: status|off");
        sdsfree(action);
    } else if (strcasecmp("ping", subcmd) == 0) {
        addReplyString(req->client, "PONG", req->id);
    } else {
        err = sdsnew("Unsupported subcommand ");
        err = sdscatfmt(err, "'%S' for command PROXY", subcmd);
    }
final:
    if (err != NULL) {
        addReplyError(req->client, err, req->id);
        sdsfree(err);
    }
    if (subcmd != NULL) sdsfree(subcmd);
    freeRequest(req, 1);
    return PROXY_COMMAND_HANDLED;
}

/* Proxy functions */

static void dumpQueue(clusterNode *node, int thread_id, int type) {
    redisClusterConnection *conn = node->connection;
    if (conn == NULL) return;
    list *queue = NULL;
    if (type == QUEUE_TYPE_PENDING) queue = conn->requests_pending;
    else if (type == QUEUE_TYPE_SENDING) queue = conn->requests_to_send;
    if (queue == NULL) return;
    sds msg = sdsnew("Node ");
    msg = sdscatprintf(msg, "%s:%d[thread %d] -> %s",
                       node->ip, node->port, thread_id,
                       (type == QUEUE_TYPE_PENDING ? "requests pending: [" :
                                                     "requests to send: ["));
    listIter li;
    listNode *ln;
    listRewind(queue, &li);
    int i = 0;
    while ((ln = listNext(&li))) {
        if (i++ > 0) msg = sdscat(msg, ", ");
        clientRequest *req = ln->value;
        if (req == NULL) msg = sdscat(msg, "NULL");
        else msg = sdscatprintf(msg, "%llu:%llu", req->client->id, req->id);
    }
    msg = sdscat(msg, "]\n");
    proxyLogDebug(msg);
    sdsfree(msg);
}

redisCommandDef *getRedisCommand(sds name) {
    redisCommandDef *cmd = NULL;
    raxIterator iter;
    raxStart(&iter, proxy.commands);
    if (raxSeek(&iter, "=", (unsigned char*) name, sdslen(name)))
        if (raxNext(&iter)) cmd = (redisCommandDef *) iter.data;
    raxStop(&iter);
    return cmd;
}

static int parseAddress(char *address, char **ip, int *port, char **hostsocket)
{
    *ip = NULL;
    *hostsocket = NULL;
    *port = 0;
    int size = strlen(address);
    char *p = strchr(address, ':');
    if (!p) *hostsocket = address;
    else {
        if (p == address) *ip = "localhost";
        else {
            *p = '\0';
            *ip = address;
        }
        if (p - address != size) *port = atoi(++p);
        if (!*port) return 0;
    }
    return 1;
}

static void printHelp(void) {
    fprintf(stderr, "Usage: redis-cluster-proxy [OPTIONS] "
        "cluster_host:cluster_port\n"
        "  -p, --port <port>    Port (default: %d)\n"
        "  --max-clients <n>    Max clients (default: %d)\n"
        "  --threads <n>        Thread number (default: %d, max: %d)\n"
        "  --tcpkeepalive       TCP Keep Alive (default: %d)\n"
        "  --tcp-backlog        TCP Backlog (default: %d)\n"
        "  --daemonize          Execute the proxy in background\n"
        "  --disable-multiplexing <opt> When should multiplexing disabled\n"
        "                               (never|auto|always) (default: auto)\n"
        "  -a, --auth <passw>   Authentication password\n"
        "  --disable-colors     Disable colorized output\n"
        "  --log-level <level>  Minimum log level: (default: info)\n"
        "                       (debug|info|success|warning|error)\n"
        "  --dump-queries       Dump query args (only for log-level "
                                "'debug') \n"
        "  --dump-buffer        Dump query buffer (only for log-level "
                                "'debug') \n"
        "  --dump-queues        Dump request queues (only for log-level "
                                "'debug') \n"
        "  -h, --help         Print this help\n",
        DEFAULT_PORT, DEFAULT_MAX_CLIENTS, DEFAULT_THREADS, MAX_THREADS,
        DEFAULT_TCP_KEEPALIVE, DEFAULT_TCP_BACKLOG);
}

static int parseOptions(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        int lastarg = (i == (argc - 1));
        char *arg = argv[i];
        if ((!strcmp("-p", arg) || !strcmp("--port", arg)) && !lastarg)
            config.port = atoi(argv[++i]);
        else if ((!strcmp(argv[i],"-a") || !strcmp("--auth", arg))&& !lastarg)
            config.auth = argv[++i];
        else if (!strcmp("--disable-colors", arg))
            config.use_colors = 0;
        else if (!strcmp("--daemonize", arg))
            config.daemonize = 1;
        else if (!strcmp("--maxclients", arg) && !lastarg)
            config.maxclients = atoi(argv[++i]);
        else if (!strcmp("--tcpkeepalive", arg) && !lastarg)
            config.tcpkeepalive = atoi(argv[++i]);
        else if (!strcmp("--tcp-backlog", arg) && !lastarg)
            config.tcp_backlog = atoi(argv[++i]);
        else if (!strcmp("--dump-queries", arg))
            config.dump_queries = 1;
        else if (!strcmp("--dump-buffer", arg))
            config.dump_buffer = 1;
        else if (!strcmp("--dump-queues", arg))
            config.dump_queues = 1;
        else if (!strcmp("--threads", arg) && !lastarg) {
            config.num_threads = atoi(argv[++i]);
            if (config.num_threads > MAX_THREADS) {
                fprintf(stderr, "Warning: maximum threads allowed: %d\n",
                                MAX_THREADS);
                config.num_threads = MAX_THREADS;
            } else if (config.num_threads < 1) config.num_threads = 1;
        } else if (!strcmp("--log-level", arg) && !lastarg) {
            char *level_name = argv[++i];
            int j = 0, level = -1;
            for (; j < LOGLEVEL_ERROR; j++) {
                if (!strcasecmp(level_name, redisProxyLogLevels[j])) {
                    level = j;
                    break;
                }
            }
            if (level < 0) {
                fprintf(stderr, "Invalid log level '%s', valid levels:\n", arg);
                for (j = 0; j < LOGLEVEL_ERROR; j++) {
                    if (j > 0) fprintf(stderr, ", ");
                    fprintf(stderr, "%s", redisProxyLogLevels[j]);
                }
                fprintf(stderr, "\n");
                exit(1);
            }
            config.loglevel = level;
        } else if (!strcmp("--disable-multiplexing", arg) && !lastarg) {
            char *val = argv[++i];
            if (!strcasecmp("never", val))
                config.disable_multiplexing = CFG_DISABLE_MULTIPLEXING_NEVER;
            else if (!strcasecmp("always", val))
                config.disable_multiplexing = CFG_DISABLE_MULTIPLEXING_ALWAYS;
            else if (!strcasecmp("auto", val))
                config.disable_multiplexing = CFG_DISABLE_MULTIPLEXING_AUTO;
            else {
                fprintf(stderr, "Invalid option for --disable-multiplexing, "
                        "valid options are:\nnever|auto|always\n");
                exit(1);
            }
        } else if (!strcmp("--help", arg)) {
            printHelp();
            exit(0);
        } else {
            if (*arg == '-') goto invalid;
            break;
        }
    }
    return i;
invalid:
    fprintf(stderr, "Invalid option '%s' or invalid number of option "
                    "arguments\n\n", argv[i]);
    printHelp();
    exit(1);
}

/* This function will try to raise the max number of open files accordingly to
 * the configured max number of clients. It also reserves a number of file
 * descriptors (proxy.min_reserved_fds) for thread pipes, listen sockets, and
 * so on.
 *
 * If it will not be possible to set the limit accordingly to the configured
 * max number of clients, the function will do the reverse setting
 * comfig.maxclients to the value that we can actually handle. */
void adjustOpenFilesLimit(void) {
    rlim_t maxfiles = config.maxclients + proxy.min_reserved_fds;
    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE,&limit) == -1) {
        proxyLogWarn("Unable to obtain the current NOFILE limit (%s), "
                     "assuming 1024 and setting the max clients configuration "
                     "accordingly.\n", strerror(errno));
        config.maxclients = 1024 - proxy.min_reserved_fds;
    } else {
        rlim_t oldlimit = limit.rlim_cur;

        /* Set the max number of files if the current limit is not enough
         * for our needs. */
        if (oldlimit < maxfiles) {
            rlim_t bestlimit;
            int setrlimit_error = 0;

            /* Try to set the file limit to match 'maxfiles' or at least
             * to the higher value supported less than maxfiles. */
            bestlimit = maxfiles;
            while(bestlimit > oldlimit) {
                rlim_t decr_step = 16;

                limit.rlim_cur = bestlimit;
                limit.rlim_max = bestlimit;
                if (setrlimit(RLIMIT_NOFILE,&limit) != -1) break;
                setrlimit_error = errno;

                /* We failed to set file limit to 'bestlimit'. Try with a
                 * smaller limit decrementing by a few FDs per iteration. */
                if (bestlimit < decr_step) break;
                bestlimit -= decr_step;
            }

            /* Assume that the limit we get initially is still valid if
             * our last try was even lower. */
            if (bestlimit < oldlimit) bestlimit = oldlimit;

            if (bestlimit < maxfiles) {
                unsigned int old_maxclients = config.maxclients;
                config.maxclients = bestlimit - proxy.min_reserved_fds;
                /* maxclients is unsigned so may overflow: in order
                 * to check if maxclients is now logically less than 1
                 * we test indirectly via bestlimit. */
                if (bestlimit <= (rlim_t) proxy.min_reserved_fds) {
                    proxyLogWarn("Your current 'ulimit -n' "
                        "of %llu is not enough for the server to start. "
                        "Please increase your open file limit to at least "
                        "%llu. Exiting.\n",
                        (unsigned long long) oldlimit,
                        (unsigned long long) maxfiles);
                    exit(1);
                }
                proxyLogWarn("You requested maxclients of %d "
                    "requiring at least %llu max file descriptors.\n",
                    old_maxclients,
                    (unsigned long long) maxfiles);
                proxyLogWarn("Server can't set maximum open files "
                    "to %llu because of OS error: %s.\n",
                    (unsigned long long) maxfiles, strerror(setrlimit_error));
                proxyLogWarn("Current maximum open files is %llu. "
                    "maxclients has been reduced to %d to compensate for "
                    "low ulimit. "
                    "If you need higher maxclients increase 'ulimit -n'.\n",
                    (unsigned long long) bestlimit, config.maxclients);
            } else {
                proxyLogInfo("Increased maximum number of open files "
                    "to %llu (it was originally set to %llu).\n",
                    (unsigned long long) maxfiles,
                    (unsigned long long) oldlimit);
            }
        }
    }
}

/* Check that server.tcp_backlog can be actually enforced in Linux according
 * to the value of /proc/sys/net/core/somaxconn, or warn about it. */
static void checkTcpBacklogSettings(void) {
#ifdef HAVE_PROC_SOMAXCONN
    FILE *fp = fopen("/proc/sys/net/core/somaxconn","r");
    char buf[1024];
    if (!fp) return;
    if (fgets(buf,sizeof(buf),fp) != NULL) {
        int somaxconn = atoi(buf);
        if (somaxconn > 0 && somaxconn < proxy.tcp_backlog) {
            proxyLogWarn("The TCP backlog setting of %d cannot be enforced "
			 "because /proc/sys/net/core/somaxconn is set to the "
			 "lower value of %d.\n", proxy.tcp_backlog, somaxconn);
        }
    }
    fclose(fp);
#endif
}

static void initConfig(void) {
    config.port = DEFAULT_PORT;
    config.tcpkeepalive = DEFAULT_TCP_KEEPALIVE;
    config.maxclients = DEFAULT_MAX_CLIENTS;
    config.num_threads = DEFAULT_THREADS;
    config.tcp_backlog = DEFAULT_TCP_BACKLOG;
    config.daemonize = 0;
    config.loglevel = LOGLEVEL_INFO;
    config.use_colors = 0;
    config.dump_queries = 0;
    config.dump_buffer = 0;
    config.dump_queues = 0;
    config.auth = NULL;
}

static void initProxy(void) {
    int i;
    proxy.numclients = 0;
    proxy.min_reserved_fds = 10 + (config.num_threads * 3) +
                             (proxy.fd_count * 2);
    adjustOpenFilesLimit();
    /* Populate commands table. */
    proxy.commands = raxNew();
    int command_count = sizeof(redisCommandTable) / sizeof(redisCommandDef);
    for (i = 0; i < command_count; i++) {
        redisCommandDef *cmd = redisCommandTable + i;
        raxInsert(proxy.commands, (unsigned char*) cmd->name,
                  strlen(cmd->name), cmd, NULL);
    }
    proxy.main_loop = aeCreateEventLoop(proxy.min_reserved_fds);
    proxy.threads = zmalloc(config.num_threads *
                            sizeof(proxyThread *));
    if (proxy.threads == NULL) {
        fprintf(stderr, "FATAL: failed to allocate memory for threads.\n");
        exit(1);
    }
    proxyLogInfo("Starting %d threads...\n", config.num_threads);
    for (i = 0; i < config.num_threads; i++) {
        proxyLogDebug("Creating thread %d...\n", i);
        proxy.threads[i] = createProxyThread(i);
        if (proxy.threads[i] == NULL) {
            fprintf(stderr, "FATAL: failed to create thread %d.\n", i);
            exit(1);
        }
        pthread_t *t = &(proxy.threads[i]->thread);
        if (pthread_create(t, NULL, execProxyThread, proxy.threads[i])){
            fprintf(stderr, "FATAL: Failed to start thread %d.\n", i);
            exit(1);
        }
    }
    proxyLogInfo("All thread(s) started!\n");
}

static void releaseProxy(void) {
    int i;
    if (proxy.main_loop != NULL) {
        aeStop(proxy.main_loop);
        aeDeleteEventLoop(proxy.main_loop);
    }
    if (proxy.threads != NULL) {
        for (i = 0; i < config.num_threads; i++) {
            proxyThread *thread =  proxy.threads[i];
            if (thread) freeProxyThread(thread);
            proxy.threads[i] = NULL;
        }
        zfree(proxy.threads);
    }
    if (proxy.commands)
        raxFree(proxy.commands);
}

void writeHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);
    client *c = privdata;
    assert(c != NULL);
    writeToClient(c);
}

static void writeRepliesToClients(struct aeEventLoop *el) {
    proxyThread *thread = el->privdata;
    assert(thread != NULL);
    if (thread->clients == NULL) return;
    listIter li;
    listNode *ln;
    listRewind(thread->clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = ln->value;
        if (!writeToClient(c)) continue;
        if (c->written > 0 && c->written < sdslen(c->obuf)) {
            if (aeCreateFileEvent(el, c->fd, AE_WRITABLE, writeHandler, c) ==
                AE_OK) {
                c->has_write_handler = 1;
            } else {
                c->has_write_handler = 0;
                proxyLogDebug("Failed to create write handler for client.\n");
            }
        }
    }
}

/* This function gets called every time threads' lopps are entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors. */
void beforeThreadSleep(struct aeEventLoop *eventLoop) {
    proxyThread *thread = eventLoop->privdata;
    writeRepliesToClients(eventLoop);
    listIter li;
    listNode *ln;
    listRewind(thread->cluster->nodes, &li);
    while ((ln = listNext(&li))) {
        clusterNode *node = ln->value;
        handleNextRequestToCluster(node);
    }
    /* TODO: Also flush private clusters' requests. */
}

static int processThreadPipeBufferForNewClients(proxyThread *thread) {
    client *c = NULL;
    int buflen = sdslen(thread->msgbuffer);
    int msgsize = sizeof(c);
    int processed = 0, count = buflen / msgsize, i;
    for (i = 0; i < count; i++) {
        char *p = thread->msgbuffer + (i * msgsize);
        client **pc = (void*) p;
        c = (client *) *pc;
        aeEventLoop *el = thread->loop;
        listAddNodeTail(thread->clients, c);
        proxyLogDebug("Client %llu added to thread %d\n",
                      c->id, c->thread_id);
        errno = 0;
        if (!installIOHandler(el, c->fd, AE_READABLE, readQuery, c, 0)) {
            proxyLogErr("ERROR: Failed to create read query handler for "
                        "client %s\n", c->ip);
            errno = EL_INSTALL_HANDLER_FAIL;
            freeClient(c);
            processed++;
            continue;
        }
        c->status = CLIENT_STATUS_LINKED;
        processed++;
    }
    if (processed > 0)
        sdsrange(thread->msgbuffer, processed * msgsize, -1);
    return processed;
}

static void readThreadPipe(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);
    proxyThread *thread = privdata;
    int processed = 0, nread = 0;
    char buf[4096];
    nread = read(fd, buf, sizeof(buf));
    if (nread == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            proxyLogDebug("Error reading from thread pipe: %s\n",
                          strerror(errno));
            return;
        }
    }
    thread->msgbuffer = sdscatlen(thread->msgbuffer, buf, nread);
    processed = processThreadPipeBufferForNewClients(thread);
}

static void printClusterConfiguration(redisCluster *cluster) {
    if (config.loglevel == LOGLEVEL_DEBUG) {
        int j;
        clusterNode *last_n = NULL;
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            clusterNode *n = searchNodeBySlot(cluster, j);
            if (n == NULL) {
                proxyLogErr("NULL node for slot %d\n", j);
                break;
            }
            if (n != last_n) {
                last_n = n;
                proxyLogDebug("Slot %d -> node %d\n", j, n->port);
            }
        }
    }
    int master_count = 0, replica_count = 0;
    listIter li;
    listNode *ln;
    listRewind(cluster->nodes, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterNode *n = ln->value;
        if (n->is_replica) replica_count++;
        else master_count++;
    }
    printf("Cluster has %d masters and %d replica(s)\n", master_count,
           replica_count);
}

static proxyThread *createProxyThread(int index) {
    int is_first = (index == 0);
    proxyThread *thread = zmalloc(sizeof(*thread));
    if (thread == NULL) return NULL;
    if (pipe(thread->io) == -1) {
        proxyLogErr("ERROR: failed to open pipe for thread!\n");
        zfree(thread);
        return NULL;
    }
    thread->thread_id = index;
    thread->next_client_id = 0;
    thread->cluster = createCluster(index);
    if (thread->cluster == NULL) {
        proxyLogErr("ERROR: failed to allocate cluster for thread: %d\n",
                    index);
        freeProxyThread(thread);
        return NULL;
    }
    if (is_first) printf("Fetching cluster configuration...\n");
    if (!fetchClusterConfiguration(thread->cluster, config.entry_node_host,
                                   config.entry_node_port,
                                   config.entry_node_socket)) {
        proxyLogErr("ERROR: Failed to fetch cluster configuration!\n");
        return NULL;
    }
    if (is_first) printClusterConfiguration(thread->cluster);
    thread->clients = listCreate();
    if (thread->clients == NULL) {
        freeProxyThread(thread);
        return NULL;
    }
    thread->pending_messages = listCreate();
    if (thread->pending_messages == NULL) {
        freeProxyThread(thread);
        return NULL;
    }
    listSetFreeMethod(thread->pending_messages, zfree);
    int loopsize = proxy.min_reserved_fds +
                   listLength(thread->cluster->nodes) +
                   (config.maxclients / config.num_threads) + 1;
    thread->loop = aeCreateEventLoop(loopsize);
    if (thread->loop == NULL) {
        freeProxyThread(thread);
        return NULL;
    }
    thread->loop->privdata = thread;
    aeSetBeforeSleepProc(thread->loop, beforeThreadSleep);
    /*aeCreateTimeEvent(thread->loop, 1, proxyThreadCron, NULL,NULL);*/
    if (aeCreateFileEvent(thread->loop, thread->io[THREAD_IO_READ],
                          AE_READABLE, readThreadPipe, thread) == AE_ERR) {
        freeProxyThread(thread);
        return NULL;
    }
    thread->msgbuffer = sdsempty();
    return thread;
}

static void handlePendingAwakeMessages(aeEventLoop *el, int fd, void *privdata,
                                       int mask)
{
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);
    proxyThread *thread = privdata;
    listIter li;
    listNode *ln;
    listRewind(thread->pending_messages, &li);
    while ((ln = listNext(&li))) {
        sds msg = ln->value;
        int sent = sendMessageToThread(thread, msg);
        if (sent == -1) continue;
        else {
            listDelNode(thread->pending_messages, ln);
            if (!sent) proxyLogErr("Failed to send message to thread!\n");
        }
    }
}

static int sendMessageToThread(proxyThread *thread, sds buf) {
    int fd = thread->io[THREAD_IO_WRITE];
    int nwritten = 0, totwritten = 0, buflen = sdslen(buf);
    if (buflen == 0) return 0;
    while (totwritten < buflen) {
        nwritten = write(fd, buf + nwritten, sdslen(buf));
        if (nwritten == -1) {
            if (errno == EAGAIN) {
                goto install_write_handler;
            }
            sdsfree(buf);
            return 0;
        } else if (nwritten == 0) break;
        totwritten += nwritten;
    }
    if (totwritten == buflen) {
        sdsfree(buf);
        aeDeleteFileEvent(thread->loop, fd, AE_WRITABLE);
    } else goto install_write_handler;
    return 1;
install_write_handler:
    sdsrange(buf, totwritten, -1);
    listAddNodeTail(thread->pending_messages, buf);
    if (aeCreateFileEvent(thread->loop, fd, AE_WRITABLE,
        handlePendingAwakeMessages, thread) == AE_ERR)
    {
        proxyLogDebug("Failed to create thread awake write handler!\n");
        listNode *ln = listSearchKey(thread->pending_messages, buf);
        if (ln != NULL) listDelNode(thread->pending_messages, ln);
        sdsfree(buf);
    }
    return -1;
}

static int awakeThreadForNewClient(proxyThread *thread, client *c) {
    sds buf = sdsnewlen(&c, sizeof(c));
    return sendMessageToThread(thread, buf);
}

static void freeProxyThread(proxyThread *thread) {
    if (thread->loop != NULL) aeDeleteEventLoop(thread->loop);
    if (thread->clients != NULL) {
        listIter li;
        listNode *ln;
        listRewind(thread->clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = ln->value;
            freeClient(c);
        }
        listRelease(thread->clients);
        thread->clients = NULL;
    }
    if (thread->pending_messages != NULL) {
        listRelease(thread->pending_messages);
    }
    if (thread->io[0]) close(thread->io[0]);
    if (thread->io[1]) close(thread->io[1]);
    if (thread->cluster != NULL) freeCluster(thread->cluster);
    zfree(thread);
}

static client *createClient(int fd, char *ip) {
    client *c = zcalloc(sizeof(*c));
    if (c == NULL) {
        proxyLogErr("Failed to allocate memory for client: %s\n", ip);
        close(fd);
        return NULL;
    }
    c->requests_to_process = listCreate();
    if (c->requests_to_process == NULL) {
        freeClient(c);
        return NULL;
    }
    c->unordered_replies = raxNew();
    if (c->unordered_replies == NULL) {
        freeClient(c);
        return NULL;
    }
    c->status = CLIENT_STATUS_NONE;
    c->fd = fd;
    c->ip = sdsnew(ip);
    c->obuf = sdsempty();
    c->reply_array = NULL;
    c->current_request = NULL;
    c->cluster = NULL;
    anetNonBlock(NULL, fd);
    anetEnableTcpNoDelay(NULL, fd);
    if (config.tcpkeepalive)
        anetKeepAlive(NULL, fd, config.tcpkeepalive);
    /* TODO: select thread with less clients */
    uint64_t numclients = proxy.numclients++;
    c->thread_id = (numclients % config.num_threads);
    c->id = proxy.threads[c->thread_id]->next_client_id++;
    if (proxy.threads[c->thread_id]->next_client_id == UINT64_MAX)
        proxy.threads[c->thread_id]->next_client_id = 0;
    c->next_request_id = 0;
    c->min_reply_id = 0;
    c->requests_with_write_handler = 0;
    c->pending_multiplex_requests = 0;
    if (config.disable_multiplexing == CFG_DISABLE_MULTIPLEXING_ALWAYS) {
        if (!disableMultiplexingForClient(c)) {
            freeClient(c);
            return NULL;
        }
    }
    return c;
}

/* Disable multiplexing on the specified client by creating a private
 * slots map (radix tree) on the client itself. The slots map will be
 * a duplicate of the shared map (proxy.cluster->slots_map), and every
 * node in the map will be a duplicate of the shared map's corresponding
 * node, but the duplicated node will have the cluster member set to
 * NULL, so that it will have just one connection (redisClusterConnection *).
 * This connection will give client a private socket to the node and private
 * request queues. */
static int disableMultiplexingForClient(client *c) {
    if (c->cluster != NULL) return 1;
    proxyLogDebug("Disabling multiplexing for client %llu\n", c->id);
    proxyThread *thread = proxy.threads[c->thread_id];
    c->cluster = duplicateCluster(thread->cluster);
    if (c->cluster == NULL) return 0;
    c->cluster->owner = c;
    listIter li;
    listNode *ln;
    listRewind(c->cluster->nodes, &li);
    while ((ln = listNext(&li))) {
        clusterNode *node = (clusterNode *) ln->value;
        clusterNode *source = node->duplicated_from;
        assert(source != NULL);
        redisClusterConnection *conn = source->connection;

        /* Move requests from shared connection to private connection. */
        listIter rli;
        listNode *rln;
        listRewind(conn->requests_to_send, &rli);
        while ((rln = listNext(&rli))) {
            clientRequest *req = rln->value;
            if (req->client != c) continue;
            if (req->has_write_handler) {
                c->pending_multiplex_requests++;
                continue;
            }
            /* Replace request node with duplicated node owned by the client */
            req->node = node;
            listAddNodeTail(node->connection->requests_to_send, req);
            listDelNode(conn->requests_to_send, ln);
            req->owned_by_client = 1;
        }
        if (c->pending_multiplex_requests == 0) {
            listRewind(conn->requests_pending, &rli);
            while ((rln = listNext(&rli))) {
                clientRequest *req = rln->value;
                if (req->client == c) c->pending_multiplex_requests++;
            }
        }
        if (c->pending_multiplex_requests == 0)
            handleNextRequestToCluster(node);
    }
    return 1;
}

static void unlinkClient(client *c) {
    if (c->fd) {
        aeEventLoop *el = getClientLoop(c);
        if (el != NULL) {
            aeDeleteFileEvent(el, c->fd, AE_READABLE);
            aeDeleteFileEvent(el, c->fd, AE_WRITABLE);
            close(c->fd);
        }
    }
    c->status = CLIENT_STATUS_UNLINKED;
}

static void freeAllClientRequests(client *c) {
    listIter li;
    listNode *ln;
    redisCluster *cluster = getCluster(c);
    listRewind(cluster->nodes, &li);
    while ((ln = listNext(&li))) {
        clusterNode *node = ln->value;
        redisClusterConnection *conn = node->connection;
        if (!conn) continue;
        listIter nli;
        listNode *nln;
        listRewind(conn->requests_to_send, &nli);
        while ((nln = listNext(&nli))) {
            clientRequest *req = nln->value;
            if (req == NULL) {
                /*listDelNode(conn->requests_to_send, nln);*/
                continue;
            }
            if (req->client != c) continue;
            if (c->status == CLIENT_STATUS_UNLINKED && req->has_write_handler)
                continue;
            listDelNode(conn->requests_to_send, nln);
            freeRequest(req, 0);
        }
        listRewind(conn->requests_pending, &nli);
        while ((nln = listNext(&nli))) {
            clientRequest *req = nln->value;
            if (req == NULL) continue;
            if (req->client != c) continue;
            /* We cannot delete the request's list node from the queue, since
             * this would break the processing order of the replies, so we
             * we create a NULL placeholder (a 'ghost request') by simply
             * setting the list node's value to NULL. */
            nln->value = NULL;
            freeRequest(req, 0);
        }
        if (config.dump_queues)
            dumpQueue(node, c->thread_id, QUEUE_TYPE_PENDING);
    }
}

static void freeClient(client *c) {
    if (c->status != CLIENT_STATUS_UNLINKED) unlinkClient(c);
    /* If the client still has requests handled by write handlers, it's not
     * possibile to free it soon, as those requests would be truncated and
     * they could break all other following requests in a multiplexing
     * context. */
    if (c->requests_with_write_handler > 0) return;
    proxyLogDebug("Free client %llu\n", c->id);
    int thread_id = c->thread_id;
    proxyThread *thread = proxy.threads[thread_id];
    assert(thread != NULL);
    listNode *ln = listSearchKey(thread->clients, c);
    if (ln != NULL) listDelNode(thread->clients, ln);
    if (c->ip != NULL) sdsfree(c->ip);
    if (c->obuf != NULL) sdsfree(c->obuf);
    if (c->reply_array != NULL) listRelease(c->reply_array);
    if (c->current_request) freeRequest(c->current_request, 1);
    listIter li;
    listRewind(c->requests_to_process, &li);
    while ((ln = listNext(&li))) {
        clientRequest *req = ln->value;
        freeRequest(req, 0);
    }
    listRelease(c->requests_to_process);
    freeAllClientRequests(c);
    if (c->unordered_replies)
        raxFreeWithCallback(c->unordered_replies, (void (*)(void*))sdsfree);
    if (c->cluster != NULL) freeCluster(c->cluster);
    zfree(c);
    proxy.numclients--;
}

static int writeToClient(client *c) {
    int success = 1, buflen = sdslen(c->obuf), nwritten = 0;
    if (buflen == 0) return 1;
    while (c->written < (size_t) buflen) {
        nwritten = write(c->fd, c->obuf + c->written, buflen - c->written);
        if (nwritten <= 0) break;
        c->written += nwritten;
    }
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            proxyLogDebug("Error writing to client: %s", strerror(errno));
            freeClient(c);
            return 0;
        }
    }
    /* The whole buffer has been written, so reset everything. */
    if (c->written == (size_t) buflen) {
        sdsclear(c->obuf);
        c->written = 0;
        if (c->has_write_handler) {
            proxyThread *thread = proxy.threads[c->thread_id];
            assert(thread != NULL);
            aeEventLoop *el = thread->loop;
            assert(el != NULL);
            aeDeleteFileEvent(el, c->fd, AE_WRITABLE);
            c->has_write_handler = 0;
        }
    }
    return success;
}

static void writeToClusterHandler(aeEventLoop *el, int fd, void *privdata,
                                  int mask)
{
    UNUSED(mask);
    clusterNode *node = privdata;
    redisContext *ctx = getClusterNodeContext(node);
    if (ctx == NULL) return;
    clientRequest *req = getFirstRequestToSend(node, NULL);
    if (ctx->err) {
        proxyLogErr("Failed to connect to node %s:%d\n", node->ip, node->port);
        if (req != NULL) {
            sds err = sdsnew("Cluster node disconnected: ");
            err = sdscatprintf(err, "%s:%d", node->ip, node->port);
            addReplyError(req->client, err, req->id);
            sdsfree(err);
            freeRequest(req, 1);
        }
        return;
    }
    if (!node->connection->has_read_handler) {
        if (!installIOHandler(el, ctx->fd, AE_READABLE, readClusterReply,
                              node, 0))
        {
            proxyLogErr("Failed to create read reply handler for node %s:%d\n",
                          node->ip, node->port);
            if (req != NULL) {
                addReplyError(req->client, "Failed to read from cluster",
                              req->id);
                freeRequest(req, 1);
            }
            return;
        } else  {
            node->connection->has_read_handler = 1;
            proxyLogDebug("Read reply handler installed "
                          "for node %s:%d\n", node->ip, node->port);
        }
    }
    node->connection->connected = 1;
    if (req == NULL) return;
    writeToCluster(el, fd, req);
}


static int writeToCluster(aeEventLoop *el, int fd, clientRequest *req) {
    size_t buflen = sdslen(req->buffer);
    int nwritten = 0;
    while (req->written < buflen) {
        nwritten = write(fd, req->buffer + req->written, buflen - req->written);
        if (nwritten <= 0) break;
        req->written += nwritten;
    }
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            proxyLogDebug("Error writing to cluster: %s\n", strerror(errno));
            if (errno == EPIPE) {
                clusterNodeDisconnect(req->node);
            } else {
                addReplyError(req->client, "Error writing to cluster", req->id);
                freeRequest(req, 1);
            }
            return 0;
        }
    }
    int success = 1;
    /* The whole query has been written, so create the read handler and
     * move the request from requests_to_send to requests_pending. */
    if (req->written == buflen) {
        client *c = req->client;
        clusterNode *node = req->node;
        int thread_id = c->thread_id;
        proxyLogDebug("Request %llu:%llu written to node %s:%d, adding it to "
                      "pending requests\n", c->id, req->id,
                      node->ip, node->port);
        aeDeleteFileEvent(el, fd, AE_WRITABLE);
        if (req->has_write_handler) {
            req->has_write_handler = 0;
            c->requests_with_write_handler--;
        }
        dequeueRequestToSend(req);
        if (c->status == CLIENT_STATUS_UNLINKED) {
            /* Client has been disconnected, so we'll enqueue a NULL pointer
             * (a 'ghost rquest') to pending requests, so that the reply will
             * be just skipped during reply buffer processing. Without using
             * this NULL placeholder, the reply buffer processing order would
             * be broken. After enqueuing the ghost request, we can finally
             * free and the request itself and try to free the client
             * completely. */
            list *pending_queue =
                node->connection->requests_pending;
            listAddNodeTail(pending_queue, NULL);
            freeRequest(req, 1);
            freeClient(c);
        } else if (!enqueuePendingRequest(req)) {
            proxyLogDebug("Could not enqueue pending request %llu:%llu\n",
                          req->client->id, req->id);
            addReplyError(req->client, "Could not enqueue request", req->id);
            freeRequest(req, 1);
            return 0;
        }
        if (config.dump_queues) dumpQueue(node, thread_id, QUEUE_TYPE_PENDING);
        proxyLogDebug("Still have %d request(s) to send\n",
                      listLength(node->connection->requests_to_send));
        /* Try to send the next available request to send, if one. */
        handleNextRequestToCluster(node);
    }
    return success;
}

/* This should be called every time a node connection is closed (ie. because
 * the connection has been closed by the proxy itself or because the node
 * instance went down.
 * This functions does the following:
 *   - Delete event loop's file events related to the node's socket
 *   - Check for requests that were still writing to the node's socket and
 *     dequeue and free them (after repying with an error to their client).
 *   - Check for requests that were still waiting to read replies from the
 *     node's socket, dequeue and free them (after repying with an error to
 *     their client). */
void onClusterNodeDisconnection(clusterNode *node) {
    redisClusterConnection *connection = node->connection;
    if (connection == NULL) return;
    redisContext *ctx = connection->context;
    if (ctx != NULL && ctx->fd >= 0) {
        int thread_id = node->cluster->thread_id;
        aeEventLoop *el = proxy.threads[thread_id]->loop;
        aeDeleteFileEvent(el, ctx->fd, AE_WRITABLE | AE_READABLE);
        sds err = sdsnew("Cluster node disconnected: ");
        err = sdscatprintf(err, "%s:%d", node->ip, node->port);
        listIter li;
        listNode *ln;
        /* If there are requests currently writing to the node, we must reply
         * to their client with a "node disconnected" error, free them and then
         * dequeue them. */
        listRewind(connection->requests_to_send, &li);
        while ((ln = listNext(&li))) {
            clientRequest *req = ln->value;
            if (req == NULL) continue;
            assert(req->node == node);
            /* If the request has a write handler installed, it means that
             * it could not have completely written its buffer to the node.
             * In this case, the client shpuld receive the reply error and
             * the request itself should be dequeued and freed. */
            if (req->has_write_handler) {
                req->has_write_handler = 0;
                req->client->requests_with_write_handler--;
                addReplyError(req->client, err, req->id);
                dequeueRequestToSend(req);
                freeRequest(req, 0);
            }
        }
        /* If there are pending requests that are reading or waiting to read
         * from the node, we must reply to their client with a
         * "node disconnected" error, free them and dequeue them. */
        listRewind(connection->requests_pending, &li);
        while ((ln = listNext(&li))) {
            clientRequest *req = ln->value;
            if (req == NULL) continue;
            assert(req->node == node);
            addReplyError(req->client, err, req->id);
            dequeuePendingRequest(req);
            freeRequest(req, 0);
        }
        sdsfree(err);
    }
}

/* TODO: implement also UNIX socket listener */
static int listen(void) {
    int fd_idx = 0;
    /* Try to use both IPv6 and IPv4 */
    proxy.fds[fd_idx] = anetTcp6Server(proxy.neterr, config.port, NULL,
                                       proxy.tcp_backlog);
    if (proxy.fds[fd_idx] != ANET_ERR)
        anetNonBlock(NULL, proxy.fds[fd_idx++]);
    else if (errno == EAFNOSUPPORT)
        proxyLogWarn("Not listening to IPv6: unsupported\n");

    proxy.fds[fd_idx] = anetTcpServer(proxy.neterr, config.port, NULL,
                                      proxy.tcp_backlog);
    if (proxy.fds[fd_idx] != ANET_ERR)
        anetNonBlock(NULL, proxy.fds[fd_idx++]);
    else if (errno == EAFNOSUPPORT)
        proxyLogWarn("Not listening to IPv4: unsupported\n");
    proxy.fd_count = fd_idx;
    return fd_idx;
}

static int requestMakeRoomForArgs(clientRequest *req, int argc) {
    if (argc >= req->offsets_size) {
        int new_size = argc + QUERY_OFFSETS_MIN_SIZE;
        size_t sz = new_size * sizeof(int);
        req->offsets = zrealloc(req->offsets, sz);
        req->lengths = zrealloc(req->lengths, sz);
        if (req->offsets == NULL || req->lengths == NULL) {
            proxyLogErr("Failed to reallocate request "
                        "offsets\n");
            return 0;
        }
        req->offsets_size = new_size;
    }
    return 1;
}

static int parseRequest(clientRequest *req) {
    int status = req->parsing_status, lf_len = 2, len, i;
    if (status != PARSE_STATUS_INCOMPLETE) return status;
    if (config.dump_buffer) {
        proxyLogDebug("Request %llu:%llu buffer:\n%s\n",
                      req->client->id, req->id, req->buffer);
    }
    int buflen = sdslen(req->buffer);
    char *p = req->buffer + req->query_offset, *nl = NULL;
    sds line = NULL;
    /* New request, so request type must be determinded. */
    if (req->is_multibulk == REQ_STATUS_UNKNOWN) {
        if (*p == '*') req->is_multibulk = 1;
        else req->is_multibulk = 0;
    }
    if (req->is_multibulk) {
        while (req->query_offset < buflen) {
            if (*p == '*') {
                if (req->num_commands > 0) {/*TODO: make it configuable */
                    /* Multiple commands, split into multiple requests */
                    proxyLogDebug("Multiple commands %d, "
                                  "splitting request...\n",
                                  req->num_commands);
                    client *c = req->client;
                    /* Truncate current request buffer */
                    req->query_offset = p - req->buffer;
                    sds newbuf = sdsnewlen(p, buflen - req->query_offset);
                    sds reqbuf = sdsnewlen(req->buffer, req->query_offset);
                    if (req->buffer) sdsfree(req->buffer);
                    req->buffer = reqbuf;
                    req->num_commands = 1;
                    req->pending_bulks = 0;
                    clientRequest *new = createRequest(c);
                    new->buffer = sdscat(new->buffer, newbuf);
                    sdsfree(newbuf);
                    c->current_request = new;
                    buflen = req->query_offset;
                    listAddNodeTail(c->requests_to_process, new);
                    break;
                } else {
                    req->num_commands++;
                    req->query_offset++;
                    p++;
                    req->pending_bulks = REQ_STATUS_UNKNOWN;
                    req->current_bulk_length = REQ_STATUS_UNKNOWN;
                }
            }
            if (req->query_offset >= buflen) {
                status = PARSE_STATUS_INCOMPLETE;
                goto cleanup;
            }
            long long lc = req->pending_bulks;
            if (lc == REQ_STATUS_UNKNOWN) {
                nl = strchr(p, '\r');
                if (nl == NULL) {
                    status = PARSE_STATUS_INCOMPLETE;
                    goto cleanup;
                }
                int len = nl - p;
                if (line != NULL) sdsfree(line);
                line = sdsnewlen(p, len);
                lc = atoll(line);
                if (lc < 0) lc = 0;
                req->query_offset += (len + 2);
                req->pending_bulks = lc;
                if (req->query_offset >= buflen) {
                    status = PARSE_STATUS_INCOMPLETE;
                    goto cleanup;
                }
                p = req->buffer + req->query_offset;
            }
            for (i = 0; i < lc; i++) {
                int arglen = req->current_bulk_length;
                if (arglen == REQ_STATUS_UNKNOWN) {
                    if (*p != '$') {
                        proxyLogErr("Failed to parse multibulk query: '$' not "
                                    "found!\n");
                        status = PARSE_STATUS_ERROR;
                        goto cleanup;
                    }
                    if ((req->query_offset + 1) >= buflen) {
                        status = PARSE_STATUS_INCOMPLETE;
                        goto cleanup;
                    }
                    nl = strchr(++p, '\r');
                    if (nl == NULL) {
                        status = PARSE_STATUS_INCOMPLETE;
                        goto cleanup;
                    }
                    len = nl - p;
                    if (line != NULL) sdsfree(line);
                    line = sdsnewlen(p, len);
                    arglen = atoi(line);
                    if (arglen < 0) arglen = 0;
                    req->current_bulk_length = arglen;
                    req->query_offset += (len + 3);
                    if (req->query_offset >= buflen) {
                        status = PARSE_STATUS_INCOMPLETE;
                        goto cleanup;
                    }
                    p = req->buffer + req->query_offset;
                }
                if (arglen > 0) {
                    int newargc = req->argc + 1;
                    if (!requestMakeRoomForArgs(req, newargc)) {
                        status = PARSE_STATUS_ERROR;
                        goto cleanup;
                    }
                    nl = strchr(p, '\r');
                    if (nl == NULL) {
                        status = PARSE_STATUS_INCOMPLETE;
                        goto cleanup;
                    }
                    int endarg = req->query_offset + arglen;
                    if (endarg >= buflen || *(req->buffer+endarg) != '\r') {
                        status = PARSE_STATUS_INCOMPLETE;
                        goto cleanup;
                    }
                    int idx = req->argc++;
                    req->offsets[idx] = p - req->buffer;
                    req->lengths[idx] = arglen;
                    if (config.dump_queries) {
                        sds tk = sdsnewlen(p, arglen);
                        proxyLogDebug("Req. %llu:%llu ARGV[%d]: '%s'\n",
                                      req->client->id, req->id, idx, tk);
                        sdsfree(tk);
                    }
                    req->pending_bulks--;
                    req->current_bulk_length = REQ_STATUS_UNKNOWN;
                    req->query_offset = endarg + 2;
                    p = req->buffer + req->query_offset;
                }
            }
        }
    } else {
        nl = strchr(p, '\n');
        if (nl == NULL) {
            status = PARSE_STATUS_INCOMPLETE;
            goto cleanup;
        }
        lf_len = 1;
        if (nl != p && *(nl - 1) == '\r') {
            lf_len++;
            nl--;
        }
        int qrylen = nl - p;
        int remaining = qrylen;
        while (remaining > 0) {
            int idx = req->argc++;
            if (!requestMakeRoomForArgs(req, idx)) {
                status = PARSE_STATUS_ERROR;
                goto cleanup;
            }
            char *sep = strchr(p, ' ');
            if (!sep) sep = nl;
            req->offsets[idx] = p - req->buffer;
            req->lengths[idx] = sep - p;
            p = sep + 1;
            remaining = nl - p;
        }
        status = PARSE_STATUS_OK;
    }
cleanup:
    if (req->query_offset > buflen) req->query_offset = buflen;
    int remaining = buflen - req->query_offset;
    if (status == PARSE_STATUS_INCOMPLETE) {
        if (req->is_multibulk && req->pending_bulks <= 0 && remaining == 0)
            status = PARSE_STATUS_OK;
    }
    req->parsing_status = status;
    if (line != NULL) sdsfree(line);
    return status;
}

static sds getRequestCommand(clientRequest *req) {
    if (req->argc == 0) return NULL;
    assert(req->buffer != NULL);
    int start = req->offsets[0], len = req->lengths[0],
        buflen = sdslen(req->buffer);
    assert(start < buflen);
    assert((start + len)  < buflen);
    sds cmd = sdsnewlen(req->buffer + start, len);
    sdstolower(cmd);
    return cmd;
}

static clusterNode *getRequestNode(clientRequest *req, sds *err) {
    clusterNode *node = NULL;
    redisCluster *cluster = getCluster(req->client);
    int slot = UNDEFINED_SLOT;
    if (req->argc == 1) {
        /*TODO: temporary behaviour */
        node = getFirstMappedNode(cluster);
        req->node = node;
        return node;
    }
    int first_key = req->command->first_key,
        last_key = req->command->last_key,
        key_step = req->command->key_step, i;
    if (first_key == 0) return NULL;
    else if (first_key >= req->argc) first_key = req->argc - 1;
    if (last_key < 0 || last_key >= req->argc) last_key = req->argc - 1;
    if (last_key < first_key) last_key = first_key;
    if (key_step < 1) key_step = 1;
    for (i = first_key; i <= last_key; i += key_step) {
        char *key = req->buffer + req->offsets[i];
        clusterNode *n = getNodeByKey(cluster, key, req->lengths[i], &slot);
        if (n == NULL) break;
        if (node == NULL) node = n;
        else {
            if (node != n) {
                if (err != NULL) {
                    if (*err != NULL) sdsfree(*err);
                    *err = sdsnew("Queries with keys belonging to "
                                  "different nodes are not supported");
                }
                node = NULL;
                break;
            }
        }
    }
    req->node = node;
    req->slot = slot;
    return node;
}

void freeRequest(clientRequest *req, int delete_from_lists) {
    if (req == NULL) return;
    proxyLogDebug("Free Request %llu:%llu\n", req->client->id, req->id);
    if (req->has_write_handler && req->written > 0) {
        proxyLogDebug("Request %llu:%llu is still writting, cannot free "
                      "it now...\n", req->client->id, req->id);
        return;
    }
    if (req->buffer != NULL) sdsfree(req->buffer);
    if (req->offsets != NULL) zfree(req->offsets);
    if (req->lengths != NULL) zfree(req->lengths);
    if (req->client->current_request == req)
        req->client->current_request = NULL;
    redisContext *ctx = NULL;
    if (req->node) ctx = getClusterNodeContext(req->node);
    aeEventLoop *el = getClientLoop(req->client);
    if (ctx != NULL && req->has_write_handler)
        aeDeleteFileEvent(el, ctx->fd, AE_WRITABLE);
    if (delete_from_lists && req->node != NULL) {
        redisClusterConnection *conn = req->node->connection;
        assert(conn != NULL);
        listNode *ln = listSearchKey(conn->requests_to_send, req);
        if (ln) listDelNode(conn->requests_to_send, ln);
        ln = listSearchKey(conn->requests_pending, req);
        if (ln) listDelNode(conn->requests_pending, ln);
        ln = listSearchKey(req->client->requests_to_process, req);
        /* We cannot delete the request's list node from the requests_pending
         * queue, since this would break the reply processing order. So we just
         * set its value to NULL. The resulting NULL placeholder (we can call
         * it a 'ghost request') will be simply skipped during reply buffer
         * processing. */
        if (ln) ln->value = NULL;
        if (config.dump_queues)
            dumpQueue(req->node, req->client->thread_id, QUEUE_TYPE_PENDING);
    }
    zfree(req);
}

void freeRequestList(list *request_list) {
    if (request_list == NULL) return;
    listIter li;
    listNode *ln;
    listRewind(request_list, &li);
    while ((ln = listNext(&li))) {
        clientRequest *req = ln->value;
        freeRequest(req, 0);
    }
    listRelease(request_list);
}

/* Return the first queued request from thr specified queue. It can return
 * NULL if the queue is empty or if the first list node is just a NULL
 * placeholder (a 'ghost request') used in place of a request created by a
 * freed client (ie. a disconnected client). The is_empty pointer can be
 * used to check if the queue is actually empty or if the first node is a
 * NULL placeholder for a ghost request. */
static clientRequest *getFirstQueuedRequest(list *queue, int *is_empty) {
    if (is_empty != NULL) *is_empty = 0;
    listNode *ln = listFirst(queue);
    if (ln == NULL) {
        if (is_empty != NULL) *is_empty = 1;
        return NULL;
    }
    return (clientRequest *) ln->value;
}

static redisClusterConnection *getRequestConnection(clientRequest *req) {
    clusterNode *node = req->node;
    if (node == NULL) return NULL;
    return node->connection;
}

static int enqueueRequest(clientRequest *req, int queue_type) {
    redisClusterConnection *conn = getRequestConnection(req);
    if (conn == NULL) return 0;
    list *queue = NULL;
    if (queue_type == QUEUE_TYPE_SENDING)
        queue = listAddNodeTail(conn->requests_to_send, req);
    else if (queue_type == QUEUE_TYPE_PENDING)
        queue = listAddNodeTail(conn->requests_pending, req);
    return (queue != NULL);
}

static void dequeueRequest(clientRequest *req, int queue_type) {
    redisClusterConnection *conn = getRequestConnection(req);
    if (conn == NULL) return;
    list *queue = NULL;
    if (queue_type == QUEUE_TYPE_SENDING)
        queue = conn->requests_to_send;
    else if (queue_type == QUEUE_TYPE_PENDING)
        queue = conn->requests_pending;
    if (queue == NULL) return;
    listNode *ln = listSearchKey(queue, req);
    if (ln != NULL)
        listDelNode(queue, ln);
}

static clientRequest *createRequest(client *c) {
    clientRequest *req = zcalloc(sizeof(*req));
    if (req == NULL) goto alloc_failure;
    req->client = c;
    req->buffer = sdsempty();
    if (req->buffer ==  NULL) goto alloc_failure;
    req->query_offset = 0;
    req->is_multibulk = REQ_STATUS_UNKNOWN;
    req->pending_bulks = REQ_STATUS_UNKNOWN;
    req->current_bulk_length = REQ_STATUS_UNKNOWN;
    req->parsing_status = PARSE_STATUS_INCOMPLETE;
    req->has_write_handler = 0;
    req->owned_by_client = 0;
    req->written = 0;
    size_t offsets_size = sizeof(int) * QUERY_OFFSETS_MIN_SIZE;
    req->offsets = zmalloc(offsets_size);
    req->lengths = zmalloc(offsets_size);
    if (!req->offsets || !req->lengths) goto alloc_failure;
    req->argc = 0;
    req->offsets_size = QUERY_OFFSETS_MIN_SIZE;
    req->command = NULL;
    req->node = NULL;
    req->slot = UNDEFINED_SLOT;
    c->current_request = req;
    req->id = c->next_request_id++;
    /* Avoid overflow */
    /* TODO: this could lead to issues in reply sequencing */
    if (c->next_request_id >= UINT64_MAX)
        c->next_request_id = 0;
    req->owned_by_client = (c->cluster != NULL);
    proxyLogDebug("Created Request %llu:%llu\n", req->client->id, req->id);
    return req;
alloc_failure:
    proxyLogErr("ERROR: Failed to allocate request!\n");
    if (!req) return NULL;
    freeRequest(req, 1);
    return NULL;
}

/* Try to call aeCreateFileEvent, and if ERANGE error has been issued, try to
 * resize event loop's setsize to fd + proxy.min_reserved_fds.
 * The 'retried' argument is used to ensure that resizing will be tried only
 * once. */
static int installIOHandler(aeEventLoop *el, int fd, int mask, aeFileProc *proc,
                            void *data, int retried)
{
    if (aeCreateFileEvent(el, fd, mask, proc, data) != AE_ERR) {
        return 1;
    } else {
        if (!retried && errno == ERANGE) {
            int newsize = fd + proxy.min_reserved_fds;
            if (aeResizeSetSize(el, newsize) == AE_ERR)
                return 0;
            return installIOHandler(el, fd, mask, proc, data, 1);
        } else if (errno != ERANGE) {
            proxyLogErr("Could not create read handler: %s\n", strerror(errno));
        }
        return 0;
    }
}

/* Fetch the cluster node connection (redisContext *) related to the
 * request and try to connect to it if not already connected.
 * Then install the write handler on the request.
 * Return 1 if the request already has a write handler or if the
 * write handler has been correctly installed.
 * Return 0 if the connection to the cluster node is missing and cannot be
 * established or if the write handler installation fails. */
static int sendRequestToCluster(clientRequest *req, sds *errmsg)
{
    if (errmsg != NULL) *errmsg = NULL;
    if (req->has_write_handler) return 1;
    assert(req->node != NULL);
    aeEventLoop *el = getClientLoop(req->client);
    redisContext *ctx = getClusterNodeContext(req->node);
    if (ctx == NULL) {
        if ((ctx = clusterNodeConnect(req->node)) == NULL) {
            sds err = sdsnew("Could not connect to node ");
            err = sdscatfmt(err, "%s:%u", req->node->ip, req->node->port);
            addReplyError(req->client, err, req->id);
            proxyLogDebug("%s\n", err);
            if (errmsg != NULL) {
                /* Remember to free the string outside this function*/
                *errmsg = err;
            } else sdsfree(err);
            freeRequest(req, 1);
            return 0;
        }
        /* Install the write handler since the connection to the cluster node
         * is asynchronous. */
        if (aeCreateFileEvent(el, ctx->fd, AE_WRITABLE,
                              writeToClusterHandler, req->node) == AE_ERR) {
            addReplyError(req->client, "Failed to write to cluster\n", req->id);
            proxyLogErr("Failed to create write handler for request\n");
            freeRequest(req, 1);
            return 0;
        }
        req->has_write_handler = 1;
        req->client->requests_with_write_handler++;
        proxyLogDebug("Write handler installed into request %llu:%llu for "
                      "node %s:%d\n", req->client->id, req->id,
                      req->node->ip, req->node->port);
        return 1;
    } else if (!isClusterNodeConnected(req->node)) {
        return 1;
    }
    redisClusterConnection *conn = req->node->connection;
    assert(conn != NULL);
    if (!conn->has_read_handler) {
        if (!installIOHandler(el, ctx->fd, AE_READABLE, readClusterReply,
                              req->node, 0))
        {
            proxyLogErr("Failed to create read reply handler for node %s:%d\n",
                          req->node->ip, req->node->port);
            addReplyError(req->client, "Failed to read from cluster\n",
                          req->id);
            freeRequest(req, 1);
            return 0;
        } else  {
            conn->has_read_handler = 1;
            proxyLogDebug("Read reply handler installed "
                          "for node %s:%d\n", req->node->ip, req->node->port);
        }
    }
    if (!writeToCluster(el, ctx->fd, req)) return 0;
    int sent = (req->written == sdslen(req->buffer));
    if (!sent) {
        if (aeCreateFileEvent(el, ctx->fd, AE_WRITABLE,
                              writeToClusterHandler, req->node) == AE_ERR) {
            addReplyError(req->client, "Failed to write to cluster\n", req->id);
            proxyLogErr("Failed to create write handler for request\n");
            freeRequest(req, 1);
            return 0;
        }
        req->has_write_handler = 1;
        req->client->requests_with_write_handler++;
        proxyLogDebug("Write handler installed into request %llu:%llu for "
                      "node %s:%d\n", req->client->id, req->id,
                      req->node->ip, req->node->port);
    }
    return 1;
}

/* Try to send the next request in requests_to_send list, by calling
 * sendRequestToCluster. If an error occurs (sendRequestToCluster returns 0)
 * keep cycling the queue until sendRequestsToCluster returns 1.
 * Return the handled request, if any. */

static clientRequest *handleNextRequestToCluster(clusterNode *node) {
    clientRequest *req = getFirstRequestToSend(node, NULL);
    if (req == NULL) return NULL;
    while (!sendRequestToCluster(req, NULL)) {
        req = getFirstRequestToSend(node, NULL);
        if (req == NULL) break;
    }
    return req;
}

/* Check whether a client with private connection (multiplexing disabled) still
 * has pending requests in the multiplexed context. After all peinding requests
 * have been consumed, start sending requests to the private connection. */
static void checkForMultiplexingRequestsToBeConsumed(clientRequest *req) {
    if (req->client->cluster != NULL && !req->owned_by_client) {
        if (--req->client->pending_multiplex_requests <= 0) {
            listIter li;
            listNode *ln;
            listRewind(req->client->cluster->nodes, &li);
            while ((ln = listNext(&li))) {
                clusterNode *node = ln->value;
                handleNextRequestToCluster(node);
            }
        }
    }
}

static int processRequest(clientRequest *req) {
    int status = parseRequest(req);
    if (status == PARSE_STATUS_ERROR) return 0;
    else if (status == PARSE_STATUS_INCOMPLETE) return 1;
    client *c = req->client;
    if (req == c->current_request) c->current_request = NULL;
    if (req->id < c->min_reply_id) c->min_reply_id = req->id;
    proxyLogDebug("Processing request %llu:%llu\n", c->id, req->id);
    sds command_name = NULL;
    sds errmsg = NULL;
    if (req->argc == 0) {
        proxyLogDebug("Request with zero arguments\n");
        errmsg = sdsnew("Invalid request");
        goto invalid_request;
    }
    /* Multi command requests are currently unsupported. */
    if (req->num_commands > 1) {
        errmsg = sdsnew("Multi-command requests are not currently supported");
        proxyLogDebug("Multi-command requests are not currently supported\n");
        goto invalid_request;
    }
    command_name = getRequestCommand(req);
    if (command_name == NULL) {
        proxyLogDebug("Missing command name\n");
        errmsg = sdsnew("Invalid request");
        goto invalid_request;
    }
    redisCommandDef *cmd = getRedisCommand(command_name);
    /* Unsupported commands:
     * - Commands not defined in redisCommandTable
     * - Commands explictly having unsupported to 1
     * - Commands without explicit first_key offset */
    if (cmd == NULL || cmd->unsupported ||
        (!cmd->handle && cmd->arity != 1 && !cmd->first_key)){
        errmsg = sdsnew("Unsupported command: ");
        errmsg = sdscatfmt(errmsg, "'%s'", command_name);
        proxyLogDebug("%s\n", errmsg);
        goto invalid_request;
    }
    req->command = cmd;
    if (cmd->handle && cmd->handle(req) == PROXY_COMMAND_HANDLED) {
        if (command_name) sdsfree(command_name);
        return 1;
    }
    clusterNode *node = getRequestNode(req, &errmsg);
    if (node == NULL) {
        if (errmsg == NULL)
            errmsg = sdsnew("Failed to get node for query");
        proxyLogDebug("%s %llu:%llu\n", errmsg, c->id, req->id);
        goto invalid_request;
    }
    if (!enqueueRequestToSend(req)) goto invalid_request;
    handleNextRequestToCluster(req->node);
    if (command_name) sdsfree(command_name);
    return 1;
invalid_request:
    if (command_name) sdsfree(command_name);
    if (errmsg != NULL) {
        addReplyError(c, (char *) errmsg, req->id);
        sdsfree(errmsg);
        freeRequest(req, 1);
        return 1;
    }
    freeRequest(req, 1);
    return 0;
}

void readQuery(aeEventLoop *el, int fd, void *privdata, int mask){
    UNUSED(el);
    UNUSED(mask);
    client *c = (client *) privdata;
    int nread, readlen = (1024*16);
    clientRequest *req = c->current_request;
    if (req == NULL) {
        req = createRequest(c);
        if (req == NULL) {
            proxyLogErr("Failed to create request\n");
            freeClient(c);
            return;
        }
    }
    size_t iblen = sdslen(req->buffer);
    req->buffer = sdsMakeRoomFor(req->buffer, readlen);
    nread = read(fd, req->buffer + iblen, readlen);
    if (nread == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            proxyLogDebug("Error reading from client %s: %s\n", c->ip,
                          strerror(errno));
            unlinkClient(c); /* TODO: Free? */
            return;
        }
    } else if (nread == 0) {
        proxyLogDebug("Client %llu from %s closed connection\n", c->id, c->ip);
        freeClient(c);
        return;
    }
    sdsIncrLen(req->buffer, nread);
    /*TODO: support max query buffer length */
    if (!processRequest(req)) freeClient(c);
    else {
        while (listLength(c->requests_to_process) > 0) {
            listNode *ln = listFirst(c->requests_to_process);
            req = ln->value;
            if (!processRequest(req)) freeClient(c);
            else {
                if (req->parsing_status == PARSE_STATUS_INCOMPLETE) break;
                else {
                    listDelNode(c->requests_to_process, ln);
                }
            }
        }
    }
}

static void acceptHandler(int fd, char *ip) {
    client *c = createClient(fd, ip);
    if (c == NULL) return;
    proxyLogDebug("Client %llu connected from %s\n", c->id, ip);
    proxyThread *thread = proxy.threads[c->thread_id];
    assert(thread != NULL);
    if (!awakeThreadForNewClient(thread, c)) {
        /* TODO: append client to a list of pending clients to be handled
         * by a beforeSleep (which should call awakeThread again*/
    }
}

void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask)
{
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);
    int client_port, client_fd, max = MAX_ACCEPTS;
    char client_ip[NET_IP_STR_LEN];
    while (max--) {
        client_fd = anetTcpAccept(proxy.neterr, fd, client_ip,
                                  sizeof(client_ip), &client_port);
        if (client_fd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                proxyLogWarn("Accepting client connection: %s\n",
                             proxy.neterr);
            return;
        }
        proxyLogDebug("Accepted connection from %s:%d\n", client_ip,
                      client_port);
        acceptHandler(client_fd, client_ip);
    }
}

static int processClusterReplyBuffer(redisContext *ctx, clusterNode *node,
                                     int thread_id)
{
    char *errmsg = NULL;
    void *_reply = NULL;
    redisReply *reply = NULL;
    int replies = 0;
    while (ctx->reader->len > 0) {
        int ok =
            (__hiredisReadReplyFromBuffer(ctx->reader, &_reply) == REDIS_OK);
        if (!ok) {
            proxyLogErr("Error: %s\n", ctx->errstr);
            errmsg = "Failed to get reply";
        }
        reply = (redisReply *) _reply;
        /* Reply not yet available, just return */
        if (ok && reply == NULL) break;
        replies++;
        clientRequest *req = getFirstRequestPending(node, NULL);
        /* If request is NULL, it's a ghost request that is a NULL
         * placeholder in place of a request created by a freed client
         * (ie. a disconnected client). In this case, just dequeue the list
         * node containing the NULL placeholder and directly skip to
         * 'consume_buffer' in order to process the remaining reply buffer. */
        if (req == NULL) {
            list *queue = node->connection->requests_pending;
            /* It should never happen that the request is NULL because of an
             * empty queue while we still have reply buffer to process */
            assert(listLength(queue) > 0);
            listNode *ln = listFirst(queue);
            listDelNode(queue, ln);
            goto consume_buffer;
        }
        proxyLogDebug("Reply read complete for request %llu:%llu, %s%s\n",
                      req->client->id, req->id, errmsg ? " ERR: " : "OK!",
                      errmsg ? errmsg : "");
        dequeuePendingRequest(req);
        if (errmsg != NULL) addReplyError(req->client, errmsg, req->id);
        else {
            proxyLogDebug("Writing reply for request %llu:%llu to client "
                          "buffer...\n", req->client->id, req->id);
            char *obuf = ctx->reader->buf;
            /*size_t len = ctx->reader->len;*/
            size_t len = ctx->reader->pos;
            if (len > ctx->reader->len) len = ctx->reader->len;
            if (config.dump_buffer) {
                sds rstr = sdsnewlen(obuf, len);
                proxyLogDebug("\nReply for request %llu:%llu:\n%s\n",
                              req->client->id, req->id, rstr);
                sdsfree(rstr);
            }
            addReplyRaw(req->client, obuf, len, req->id);
        }
consume_buffer:
        if (config.dump_queues) dumpQueue(node, thread_id, QUEUE_TYPE_PENDING);
        /* Consume reader buffer */
        sdsrange(ctx->reader->buf, ctx->reader->pos, -1);
        ctx->reader->pos = 0;
        ctx->reader->len = sdslen(ctx->reader->buf);
        freeReplyObject(reply);
        if (req) freeRequest(req, 1);
        if (!ok) break;
    }
    return replies;
}

static void readClusterReply(aeEventLoop *el, int fd,
                             void *privdata, int mask)
{
    UNUSED(mask);
    UNUSED(fd);
    proxyThread *thread = el->privdata;
    int thread_id = thread->thread_id;
    clusterNode *node = privdata;
    clientRequest *req = getFirstRequestPending(node, NULL);
    redisContext *ctx = getClusterNodeContext(node);
    list *queue = node->connection->requests_pending;
    sds errmsg = NULL;
    proxyLogDebug("Reading reply from %s:%d on thread %d...\n",
                  node->ip, node->port, thread_id);
    int success = (redisBufferRead(ctx) == REDIS_OK), replies = 0,
                  node_disconnected = 0;
    if (!success) {
        proxyLogDebug("Failed redisBufferRead from %s:%d on thread %d\n",
                      node->ip, node->port, thread_id);
        int err = ctx->err;
        node_disconnected = (err & (REDIS_ERR_IO | REDIS_ERR_EOF));
        if (node_disconnected) errmsg = sdsnew("Cluster node disconnected: ");
        else {
            proxyLogErr("Error from node %s:%d: %s\n", node->ip, node->port,
                        ctx->errstr);
            errmsg = sdsnew("Failed to read reply from ");
        }
        errmsg = sdscatfmt(errmsg, "%s:%u", node->ip, node->port);
        /* An error occurred, so dequeue the request. If the request is not
         * NULL, send an error reply to the client and then free the requests
         * itself. If the node is down (node_disconnected), call
         * clusterNodeDisconnect in order to close the socket, close the file
         * event handlers and close all the other requests waiting for a
         * reply on the same node's socket. */
        if (req != NULL) {
            dequeuePendingRequest(req);
            addReplyError(req->client, errmsg, req->id);
            client *c = req->client;
            if (c->cluster && c->pending_multiplex_requests > 0)
                checkForMultiplexingRequestsToBeConsumed(req);
            freeRequest(req, 1);
        } else {
            listNode *first = listFirst(queue);
            if (first) listDelNode(queue, first);
        }
        if (node_disconnected) {
            proxyLogDebug(errmsg);
            clusterNodeDisconnect(node);
        }
        sdsfree(errmsg);
        /* Exit, since an error occurred. */
        return;
    } else replies = processClusterReplyBuffer(ctx, node, thread_id);
    if (errmsg != NULL) sdsfree(errmsg);
}

static void *execProxyThread(void *ptr) {
    proxyThread *thread = (proxyThread *) ptr;
    /* proxyLogDebug("Starting thread %d...\n", thread->thread_id); */
    aeMain(thread->loop);
    return NULL;
}

void daemonize(void) {
    int fd;

    if (fork() != 0) exit(0); /* parent exits */
    setsid(); /* create a new session */

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. */
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

int main(int argc, char **argv) {
    int exit_status = 0, i;
    signal(SIGPIPE, SIG_IGN);
    printf("Redis Cluster Proxy v%s\n", REDIS_CLUSTER_PROXY_VERSION);
    initConfig();
    int parsed_opts = parseOptions(argc, argv);
    if (parsed_opts >= argc) {
        fprintf(stderr, "Missing cluster address.\n\n");
        printHelp();
        return 1;
    }
    proxy.tcp_backlog = config.tcp_backlog;
    checkTcpBacklogSettings();
    config.cluster_address = argv[parsed_opts];
    printf("Cluster Address: %s\n", config.cluster_address);
    if (!parseAddress(config.cluster_address, &config.entry_node_host,
                      &config.entry_node_port, &config.entry_node_socket)) {
        fprintf(stderr, "Invalid address '%s'\n", config.cluster_address);
        return 1;
    }
    if (!listen()) {
        proxyLogErr("Failed to listen on port %d\n", config.port);
        exit_status = 1;
        goto cleanup;
    }
    printf("Listening on port %d\n", config.port);
    if (config.daemonize) daemonize();
    initProxy();
    for (i = 0; i < proxy.fd_count; i++) {
        if (aeCreateFileEvent(proxy.main_loop, proxy.fds[i], AE_READABLE,
                              acceptTcpHandler, NULL) == AE_ERR) {
            proxyLogErr("FATAL: Failed to create TCP accept handlers, "
                        "aborting...\n");
            exit_status = 1;
            goto cleanup;
        }
    }
    aeMain(proxy.main_loop);
cleanup:
    releaseProxy();
    return exit_status;
}
