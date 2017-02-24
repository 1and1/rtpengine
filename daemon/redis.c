#include "redis.h"

#include <stdio.h>
#include <hiredis/hiredis.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <glib.h>
#include <stdarg.h>
#include <ctype.h>
#include <glib.h>
#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libevent.h>
#include <event2/thread.h>

#include "compat.h"
#include "aux.h"
#include "call.h"
#include "log.h"
#include "log_funcs.h"
#include "str.h"
#include "crypto.h"
#include "dtls.h"
#include "recording.h"
#include "rtplib.h"
#include "str.h"

#include <stdlib.h>
#include <glib-object.h>
#include <json-glib/json-glib.h>

INLINE redisReply *redis_expect(int type, redisReply *r) {
	if (!r)
		return NULL;
	if (r->type != type) {
		freeReplyObject(r);
		return NULL;
	}
	return r;
}

#if __YCM

/* format checking in YCM editor */

INLINE void redis_pipe(struct redis *r, const char *fmt, ...)
	__attribute__((format(printf,2,3)));
INLINE redisReply *redis_get(struct redis *r, int type, const char *fmt, ...)
	__attribute__((format(printf,3,4)));
static int redisCommandNR(redisContext *r, const char *fmt, ...)
	__attribute__((format(printf,2,3)));

#define PB "%.*s"
#define STR(x) (int) (x)->len, (x)->s
#define STR_R(x) (int) (x)->len, (x)->str
#define S_LEN(s,l) (int) (l), (s)

#else

#define PB "%b"
#define STR(x) (x)->s, (size_t) (x)->len
#define STR_R(x) (x)->str, (size_t) (x)->len
#define S_LEN(s,l) (s), (size_t) (l)

#endif

#define REDIS_FMT(x) (x)->len, (x)->str

static void redis_restore_call(struct redis *r, struct callmaster *m, const str *id, enum call_type);
static int redis_check_conn(struct redis *r);
static void json_restore_call(struct redis *r, struct callmaster *m, const str *id, enum call_type type);

static void redis_pipe(struct redis *r, const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	redisvAppendCommand(r->ctx, fmt, ap);
	va_end(ap);
	r->pipeline++;
}
static redisReply *redis_get(struct redis *r, int type, const char *fmt, ...) {
	va_list ap;
	redisReply *ret;

	va_start(ap, fmt);
	ret = redis_expect(type, redisvCommand(r->ctx, fmt, ap));
	va_end(ap);

	return ret;
}
static int redisCommandNR(redisContext *r, const char *fmt, ...) {
	va_list ap;
	redisReply *ret;
	int i = 0;

	va_start(ap, fmt);
	ret = redisvCommand(r, fmt, ap);
	va_end(ap);

	if (!ret)
		return -1;

	if (ret->type == REDIS_REPLY_ERROR) {
		i = -1;
		ilog(LOG_WARNING, "Redis returned error to command '%s': %s", fmt, ret->str);
	}

	freeReplyObject(ret);
	return i;
}


/* called with r->lock held */
static int redis_check_type(struct redis *r, char *key, char *suffix, char *type) {
	redisReply *rp;

	rp = redisCommand(r->ctx, "TYPE %s%s", key, suffix ? : "");
	if (!rp)
		return -1;
	if (rp->type != REDIS_REPLY_STATUS) {
		freeReplyObject(rp);
		return -1;
	}
	if (strcmp(rp->str, type) && strcmp(rp->str, "none"))
		redisCommandNR(r->ctx, "DEL %s%s", key, suffix ? : "");
	freeReplyObject(rp);
	return 0;
}


/* called with r->lock held */
static void redis_consume(struct redis *r) {
	redisReply *rp;

	while (r->pipeline) {
		if (redisGetReply(r->ctx, (void **) &rp) == REDIS_OK)
			freeReplyObject(rp);
		r->pipeline--;
	}
}


/* called with r->lock held if necessary */
static int redis_connect(struct redis *r, int wait) {
	struct timeval tv;
	redisReply *rp;
	char *s;

	if (r->ctx)
		redisFree(r->ctx);
	r->ctx = NULL;

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	r->ctx = redisConnectWithTimeout(r->host, r->endpoint.port, tv);

	if (!r->ctx)
		goto err;
	if (r->ctx->err)
		goto err2;

	if (r->auth) {
		if (redisCommandNR(r->ctx, "AUTH %s", r->auth))
			goto err2;
	}
	else {
		if (redisCommandNR(r->ctx, "PING"))
			goto err2;
	}

	if (redisCommandNR(r->ctx, "SELECT %i", r->db))
		goto err2;

	while (wait-- >= 0) {
		ilog(LOG_INFO, "Asking Redis whether it's master or slave...");
		rp = redisCommand(r->ctx, "INFO");
		if (!rp) {
			goto err2;
		}

		s = strstr(rp->str, "role:");
		if (!s) {
			goto err3;
		}

		if (!memcmp(s, "role:master", 9)) {
			if (r->role == MASTER_REDIS_ROLE || r->role == ANY_REDIS_ROLE) {
				ilog(LOG_INFO, "Connected to Redis %s in master mode", 
					endpoint_print_buf(&r->endpoint));
				goto done;
			} else if (r->role == SLAVE_REDIS_ROLE) {
				ilog(LOG_INFO, "Connected to Redis %s in master mode, but wanted mode is slave; retrying...",
					endpoint_print_buf(&r->endpoint));
				goto next;
			}
		} else if (!memcmp(s, "role:slave", 8)) {
			if (r->role == SLAVE_REDIS_ROLE || r->role == ANY_REDIS_ROLE) {
				ilog(LOG_INFO, "Connected to Redis %s in slave mode",
					endpoint_print_buf(&r->endpoint));
				goto done;
			} else if (r->role == MASTER_REDIS_ROLE) {
				ilog(LOG_INFO, "Connected to Redis %s in slave mode, but wanted mode is master; retrying...",
					endpoint_print_buf(&r->endpoint));
				goto next;
			}
		} else {
			goto err3;
		}

next:
		freeReplyObject(rp);
		usleep(1000000);
	}

	goto err2;

done:
	freeReplyObject(rp);
	redis_check_type(r, "calls", NULL, "set");
	return 0;

err3:
	freeReplyObject(rp);
err2:
	if (r->ctx->err) {
		rlog(LOG_ERR, "Failed to connect to Redis %s, error: %s",
			endpoint_print_buf(&r->endpoint), r->ctx->errstr);
		return -1;
	}
err:
	rlog(LOG_ERR, "Failed to connect to Redis %s",
		endpoint_print_buf(&r->endpoint));
	return -1;
}


void on_redis_notification(redisAsyncContext *actx, void *reply, void *privdata) {

	struct callmaster *cm = privdata;
	struct redis *r = 0;
	struct call *c = NULL;
	str callid;
	str keyspace_id;

	// sanity checks
	if (!cm) {
		rlog(LOG_ERROR, "Struct callmaster is NULL on on_redis_notification");
		return;
	}

	if (!cm->conf.redis_notify) {
		rlog(LOG_ERROR, "A redis notification has been received but no redis_notify database found");
		return;
	}

	r = cm->conf.redis_notify;

	mutex_lock(&r->lock);

	redisReply *rr = (redisReply*)reply;

	if (reply == NULL || rr->type != REDIS_REPLY_ARRAY)
		goto err;

	for (int j = 0; j < rr->elements; j++) {
		rlog(LOG_DEBUG, "Redis-Notify: %u) %s\n", j, rr->element[j]->str);
	}

	if (rr->elements != 4)
		goto err;

	// format: __keyspace@<db>__:<key>
	str_init_len(&keyspace_id, rr->element[2]->str, rr->element[2]->len);

	if (str_shift_cmp(&keyspace_id, "__keyspace@"))
		goto err;

	// extract <db>
	char *endp;
	r->db = strtoul(keyspace_id.s, &endp, 10);
	if (endp == keyspace_id.s || *endp != '_')
		goto err;
	if (str_shift(&keyspace_id, endp - keyspace_id.s + 3))
		goto err;
	if (keyspace_id.s[-1] != ':')
		goto err;
	// now at <key>

	if (cm->conf.redis_multikey) {
		if (str_shift_cmp(&keyspace_id, "notifier-")) {
			rlog(LOG_ERROR,"Redis-Notifier: The prefix 'notifier-' to determine the redis key has not been found in the redis notification !\n");
			goto err;
		}
	} else {
		if (str_shift_cmp(&keyspace_id, "json-")) {
			rlog(LOG_ERROR,"Redis-Notifier: The prefix 'json-' to determine the redis key has not been found in the redis notification !\n");
			goto err;
		}
	}

	callid = keyspace_id;

	// select the right db for restoring the call
	if (redisCommandNR(r->ctx, "SELECT %i", r->db)) {
		if (r->ctx->err)
			rlog(LOG_ERROR, "Redis error: %s", r->ctx->errstr);
		redisFree(r->ctx);
		r->ctx = NULL;
		goto err;
	}

	if (cm->conf.redis_multikey && strncmp(rr->element[3]->str,"sadd",4)==0)
		redis_restore_call(r, cm, &callid, CT_FOREIGN_CALL);

	if (!cm->conf.redis_multikey && strncmp(rr->element[3]->str,"set",3)==0) {
		c = call_get(&callid, cm);
		if (c) {
			rwlock_unlock_w(&c->master_lock);
			if (IS_FOREIGN_CALL(c))
				call_destroy(c);
			else {
				rlog(LOG_WARN, "Redis-Notifier: Ignoring SET received for OWN call: %s\n", rr->element[2]->str);
				goto err;
			}
		}
		json_restore_call(r, cm, &callid, CT_FOREIGN_CALL);
	}

	if (strncmp(rr->element[3]->str,"del",3)==0) {
		c = call_get(&callid, cm);
		if (!c) {
			rlog(LOG_NOTICE, "Redis-Notifier: DEL did not find call with callid: %s\n", rr->element[2]->str);
			goto err;
		}
		rwlock_unlock_w(&c->master_lock);
		if (!IS_FOREIGN_CALL(c)) {
			rlog(LOG_WARN, "Redis-Notifier: Ignoring DEL received for an OWN call: %s\n", rr->element[2]->str);
			goto err;
		}
		call_destroy(c);
	}

err:
	if (c) {
		// because of call_get(..)
		obj_put(c);
		log_info_clear();
	}

	mutex_unlock(&r->lock);
}

void redis_async_context_disconnect(const redisAsyncContext *redis_notify_async_context, int status) {
	if (status == REDIS_ERR) {
		if (redis_notify_async_context->errstr) {
			rlog(LOG_ERROR, "redis_async_context_disconnect error %d on context free: %s",
				redis_notify_async_context->err, redis_notify_async_context->errstr);
		} else {
			rlog(LOG_ERROR, "redis_async_context_disconnect error %d on context free: no errstr",
				redis_notify_async_context->err);
		}
	} else if (status == REDIS_OK) {
		rlog(LOG_ERROR, "redis_async_context_disconnect initiated by user");
	} else {
		rlog(LOG_ERROR, "redis_async_context_disconnect invalid status code %d", status);
	}
}

int redis_async_context_alloc(struct callmaster *cm) {
	struct redis *r = 0;

	// sanity checks
	if (!cm) {
		rlog(LOG_ERROR, "Struct callmaster is NULL on context free");
		return -1;
	}

	if (!cm->conf.redis_notify) {
		rlog(LOG_INFO, "redis_notify database is NULL.");
		return -1;
	}

	// get redis_notify database
	r = cm->conf.redis_notify;
	rlog(LOG_INFO, "Use Redis %s for notifications", endpoint_print_buf(&r->endpoint));

	// alloc async context
	cm->conf.redis_notify_async_context = redisAsyncConnect(r->host, r->endpoint.port);
	if (cm->conf.redis_notify_async_context && cm->conf.redis_notify_async_context->ev.data != NULL )
	    rlog(LOG_ERROR, "redis_async_context_alloc() ev.data is NOT NULL after connect");

	if (!cm->conf.redis_notify_async_context) {
		rlog(LOG_ERROR, "redis_notify_async_context can't create new");
		return -1;
	}
	if (cm->conf.redis_notify_async_context->err) {
		rlog(LOG_ERROR, "redis_notify_async_context can't create new error: %s", cm->conf.redis_notify_async_context->errstr);
		return -1;
	}

	if (cm->conf.redis_notify_async_context && cm->conf.redis_notify_async_context->ev.data != NULL )
        rlog(LOG_ERROR, "redis_async_context_alloc() ev.data is NOT NULL somewhere in between");


	if (redisAsyncSetDisconnectCallback(cm->conf.redis_notify_async_context, redis_async_context_disconnect) != REDIS_OK) {
		rlog(LOG_ERROR, "redis_notify_async_context can't set disconnect callback");
		return -1;
	}

	if (cm->conf.redis_notify_async_context && cm->conf.redis_notify_async_context->ev.data != NULL )
	    rlog(LOG_ERROR, "redis_async_context_alloc() ev.data is NOT NULL afterasync context");

	return 0;
}

int redis_notify_event_base_action(struct callmaster *cm, enum event_base_action action) {
	// sanity checks
	if (!cm) {
		rlog(LOG_ERROR, "Struct callmaster is NULL on event base action %d", action);
		return -1;
	}

	if (!cm->conf.redis_notify_event_base && action!=EVENT_BASE_ALLOC) {
		rlog(LOG_ERROR, "redis_notify_event_base is NULL on event base action %d", action);
		return -1;
	}

	// exec event base action
	switch (action) {
		case EVENT_BASE_ALLOC:
			cm->conf.redis_notify_event_base = event_base_new();
			if (!cm->conf.redis_notify_event_base) {
				rlog(LOG_ERROR, "Fail alloc redis_notify_event_base");
				return -1;
			} else {
				rlog(LOG_DEBUG, "Success alloc redis_notify_event_base");
			}
			break;

		case EVENT_BASE_FREE:
			event_base_free(cm->conf.redis_notify_event_base);
			rlog(LOG_DEBUG, "Success free redis_notify_event_base");
			break;

		case EVENT_BASE_LOOPBREAK:
			if (event_base_loopbreak(cm->conf.redis_notify_event_base)) {
				rlog(LOG_ERROR, "Fail loopbreak redis_notify_event_base");
				return -1;
			} else {
				rlog(LOG_DEBUG, "Success loopbreak redis_notify_event_base");
			}
			break;

		default:
			rlog(LOG_ERROR, "No event base action found: %d", action);
			return -1;
	}

	return 0;
}

int redis_notify_subscribe_action(struct callmaster *cm, enum subscribe_action action, int keyspace) {
	// sanity checks
	if (!cm) {
		rlog(LOG_ERROR, "Struct callmaster is NULL on subscribe action");
		return -1;
	}

	if (!cm->conf.redis_notify_async_context) {
		rlog(LOG_ERROR, "redis_notify_async_context is NULL on subscribe action");
		return -1;
	}

	if (cm->conf.redis_notify_async_context->err) {
		rlog(LOG_ERROR, "redis_notify_async_context error on subscribe action: %s", cm->conf.redis_notify_async_context->errstr);
		return -1;
	}

	if (cm->conf.redis_multikey) {
		switch (action) {
		case SUBSCRIBE_KEYSPACE:
			if (redisAsyncCommand(cm->conf.redis_notify_async_context, on_redis_notification, (void*)cm, "psubscribe __keyspace@%i__:notifier-*", keyspace) != REDIS_OK) {
				rlog(LOG_ERROR, "Fail redisAsyncCommand on SUBSCRIBE_KEYSPACE");
				return -1;
			}
			break;
		case UNSUBSCRIBE_KEYSPACE:
			if (redisAsyncCommand(cm->conf.redis_notify_async_context, on_redis_notification, (void*)cm, "punsubscribe __keyspace@%i__:notifier-*", keyspace) != REDIS_OK) {
				rlog(LOG_ERROR, "Fail redisAsyncCommand on UNSUBSCRIBE_KEYSPACE");
				return -1;
			}
			break;
		case UNSUBSCRIBE_ALL:
			if (redisAsyncCommand(cm->conf.redis_notify_async_context, on_redis_notification, (void *) cm, "punsubscribe") != REDIS_OK) {
				rlog(LOG_ERROR, "Fail redisAsyncCommand on UNSUBSCRIBE_ALL");
				return -1;
			}
			break;
		default:
			rlog(LOG_ERROR, "No subscribe action found: %d", action);
			return -1;
		}
	} else {
		switch (action) {
		case SUBSCRIBE_KEYSPACE:
			if (redisAsyncCommand(cm->conf.redis_notify_async_context, on_redis_notification, (void*)cm, "psubscribe __keyspace@%i__:json-*", keyspace) != REDIS_OK) {
				rlog(LOG_ERROR, "Fail redisAsyncCommand on JSON SUBSCRIBE_KEYSPACE");
				return -1;
			}
			break;
		case UNSUBSCRIBE_KEYSPACE:
			if (redisAsyncCommand(cm->conf.redis_notify_async_context, on_redis_notification, (void*)cm, "punsubscribe __keyspace@%i__:json-*", keyspace) != REDIS_OK) {
				rlog(LOG_ERROR, "Fail redisAsyncCommand on JSON UNSUBSCRIBE_KEYSPACE");
				return -1;
			}
			break;
		case UNSUBSCRIBE_ALL:
			if (redisAsyncCommand(cm->conf.redis_notify_async_context, on_redis_notification, (void *) cm, "punsubscribe") != REDIS_OK) {
				rlog(LOG_ERROR, "Fail redisAsyncCommand on JSON UNSUBSCRIBE_ALL");
				return -1;
			}
			break;
		default:
			rlog(LOG_ERROR, "No subscribe action found: %d", action);
			return -1;
		}
	}

	return 0;
}

static int redis_notify(struct callmaster *cm) {
	struct redis *r = 0;
	GList *l;

	// sanity checks
	if (!cm) {
		rlog(LOG_ERROR, "Struct callmaster is NULL on redis_notify()");
		return -1;
	}

	if (!cm->conf.redis_notify) {
		rlog(LOG_ERROR, "redis_notify database is NULL on redis_notify()");
		return -1;
	}

	if (!cm->conf.redis_notify_async_context) {
		rlog(LOG_ERROR, "redis_notify_async_context is NULL on redis_notify()");
		return -1;
	}

	if (!cm->conf.redis_notify_event_base) {
		rlog(LOG_ERROR, "redis_notify_event_base is NULL on redis_notify()");
		return -1;
	}

	// get redis_notify database
	r = cm->conf.redis_notify;
	rlog(LOG_INFO, "Use Redis %s to subscribe to notifications", endpoint_print_buf(&r->endpoint));

	// attach event base
	if (redisLibeventAttach(cm->conf.redis_notify_async_context, cm->conf.redis_notify_event_base) == REDIS_ERR) {
		if (cm->conf.redis_notify_async_context->err) {
			rlog(LOG_ERROR, "redis_notify_async_context can't attach event base error: %s", cm->conf.redis_notify_async_context->errstr);
		} else {
			rlog(LOG_ERROR, "redis_notify_async_context can't attach event base");

		}
		return -1;
	}

	// subscribe to the values in the configured keyspaces
	rwlock_lock_r(&cm->conf.config_lock);
	for (l = cm->conf.redis_subscribed_keyspaces->head; l; l = l->next) {
		redis_notify_subscribe_action(cm, SUBSCRIBE_KEYSPACE, GPOINTER_TO_UINT(l->data));
	}
	rwlock_unlock_r(&cm->conf.config_lock);

	// dispatch event base => thread blocks here
	if (event_base_dispatch(cm->conf.redis_notify_event_base) < 0) {
		rlog(LOG_ERROR, "Fail event_base_dispatch()");
		return -1;
	}

	return 0;
}

void redis_notify_loop(void *d) {
	int seconds = 1, redis_notify_return = 0;
	time_t next_run = g_now.tv_sec;
	struct callmaster *cm = (struct callmaster *)d;
	struct redis *r;

	// sanity checks
	if (!cm) {
		ilog(LOG_ERROR, "NULL callmaster");
		return ;
	}

	r = cm->conf.redis_notify;
	if (!r) {
		rlog(LOG_ERROR, "Don't use Redis notifications. See --redis-notifications parameter.");
		return ;
	}

	// init libevent for pthread usage
	if (evthread_use_pthreads() < 0) {
		ilog(LOG_ERROR, "evthread_use_pthreads failed");
		return ;
	}

	// alloc redis async context 
	if (redis_async_context_alloc(cm) < 0) {
		return ;
	}

	// alloc event base
	if (redis_notify_event_base_action(cm, EVENT_BASE_ALLOC) < 0) {
		return ;
	}

	// initial redis_notify
	if (redis_check_conn(r) == REDIS_STATE_CONNECTED) {
		redis_notify_return = redis_notify(cm);
		ilog(LOG_ERROR, "redis_notify_loop() initial redis_notify return=%d", redis_notify_return);
	}

	// loop redis_notify => in case of lost connection
	while (!g_shutdown) {
		gettimeofday(&g_now, NULL);
		if (g_now.tv_sec < next_run) {
			usleep(100000);
			continue;
		}

		next_run = g_now.tv_sec + seconds;

		if (redis_check_conn(r) == REDIS_STATE_RECONNECTED || redis_notify_return < 0) {
			// alloc new redis async context upon redis breakdown
			if (redis_async_context_alloc(cm) < 0) {
			    ilog(LOG_ERROR, "redis_notify_loop() redis_async_context_alloc <<<<< 0 ");
				continue;
			}

			// prepare notifications
			redis_notify_return = redis_notify(cm);
		}
	}

	// unsubscribe notifications
	redis_notify_subscribe_action(cm, UNSUBSCRIBE_ALL, 0);

	// free async context
	redisAsyncDisconnect(cm->conf.redis_notify_async_context);

	// free event base
	redis_notify_event_base_action(cm, EVENT_BASE_FREE);
}

struct redis *redis_new(const endpoint_t *ep, int db, const char *auth, enum redis_role role, int no_redis_required) {
	struct redis *r;

	r = g_slice_alloc0(sizeof(*r));

	r->endpoint = *ep;
	sockaddr_print(&ep->address, r->host, sizeof(r->host));
	r->db = db;
	r->auth = auth;
	r->role = role;
	r->state = REDIS_STATE_DISCONNECTED;
	r->no_redis_required = no_redis_required;
	mutex_init(&r->lock);

	if (redis_connect(r, 10)) {
		if (r->no_redis_required) {
			rlog(LOG_WARN, "Starting with no initial connection to Redis %s !",
				endpoint_print_buf(&r->endpoint));
			return r;
		}
		goto err;
	}

	// redis is connected
	rlog(LOG_INFO, "Established initial connection to Redis %s",
		endpoint_print_buf(&r->endpoint));
	r->state = REDIS_STATE_CONNECTED;
	return r;

err:
	mutex_destroy(&r->lock);
	g_slice_free1(sizeof(*r), r);
	return NULL;
}


static void redis_close(struct redis *r) {
	if (r->ctx)
		redisFree(r->ctx);
	mutex_destroy(&r->lock);
	g_slice_free1(sizeof(*r), r);
}


/* must be called with r->lock held */
static int redis_check_conn(struct redis *r) {
	// try redis connection
	if (redisCommandNR(r->ctx, "PING") == 0) {
		// redis is connected
		return REDIS_STATE_CONNECTED;
	}

	// redis is disconnected
	if (r->state == REDIS_STATE_CONNECTED) {
		rlog(LOG_ERR, "Lost connection to Redis %s",
			endpoint_print_buf(&r->endpoint));
		r->state = REDIS_STATE_DISCONNECTED;
	}

	// try redis reconnect => will free current r->ctx
	if (redis_connect(r, 1)) {
		// redis is disconnected
		return REDIS_STATE_DISCONNECTED;
	}

	// redis is connected
	if (r->state == REDIS_STATE_DISCONNECTED) {
		rlog(LOG_INFO, "RE-Established connection to Redis %s",
			endpoint_print_buf(&r->endpoint));
		r->state = REDIS_STATE_CONNECTED;
	}

	// redis is re-connected
	return REDIS_STATE_RECONNECTED;
}



static void redis_delete_list(struct redis *r, const str *callid, const char *prefix, GQueue *q) {
	unsigned int i;

	for (i = 0; i < g_queue_get_length(q); i++)
		redis_pipe(r, "DEL %s-"PB"-%u", prefix, STR(callid), i);
}

/* called with r->lock held and c->master_lock held */
static void redis_delete_call_json(struct call *c, struct redis *r) {
	redis_pipe(r, "DEL json-"PB"", STR(&c->callid));
	redis_consume(r);
}

/* called with r->lock held and c->master_lock held */
static void redis_delete_call(struct call *c, struct redis *r) {
	redis_pipe(r, "DEL notifier-"PB"", STR(&c->callid));
	redis_pipe(r, "SREM calls "PB"", STR(&c->callid));
	redis_pipe(r, "DEL call-"PB"", STR(&c->callid));
	redis_delete_list(r, &c->callid, "sfd", &c->stream_fds);
	redis_delete_list(r, &c->callid, "stream", &c->streams);
	redis_delete_list(r, &c->callid, "stream_sfds", &c->streams);
	redis_delete_list(r, &c->callid, "tag", &c->monologues);
	redis_delete_list(r, &c->callid, "other_tags", &c->monologues);
	redis_delete_list(r, &c->callid, "medias", &c->monologues);
	redis_delete_list(r, &c->callid, "media", &c->medias);
	redis_delete_list(r, &c->callid, "streams", &c->medias);
	redis_delete_list(r, &c->callid, "maps", &c->medias);
	redis_delete_list(r, &c->callid, "payload_types", &c->medias);
	redis_delete_list(r, &c->callid, "map", &c->endpoint_maps);
	redis_delete_list(r, &c->callid, "map_sfds", &c->endpoint_maps);

	redis_consume(r);
}

INLINE void json_builder_add_string_value_uri_enc(JsonBuilder *builder, const char* tmp, int len) {
	char enc[len * 3 + 1];
	str_uri_encode_len(enc, tmp, len);
	json_builder_add_string_value(builder,enc);
}
INLINE char* json_reader_get_string_value_uri_enc(JsonReader *root_reader, int *lenp) {
	const char *str = json_reader_get_string_value(root_reader);
	char *ret;
	int len = str_uri_decode_len(&ret, str, strlen(str));
	if (lenp)
		*lenp = len;
	return ret; // must be free'd
}

// stolen from libhiredis
/* Create a reply object */
INLINE redisReply *createReplyObject(int type) {
    redisReply *r = calloc(1,sizeof(*r));

    if (r == NULL)
        return NULL;

    r->type = type;
    return r;
}

static int json_get_hash(struct redis_hash *out, struct call* c,
		const char *key,
		unsigned int id)
{
	static unsigned int MAXKEYLENGTH = 512;
	char key_concatted[MAXKEYLENGTH];
	int rc=0;

	if (!c)
		goto err;

	if (id == -1) {
		rc = snprintf(key_concatted, MAXKEYLENGTH, "%s",key);
	} else {
		rc = snprintf(key_concatted, MAXKEYLENGTH, "%s-%u",key,id);
	}
	if (rc>=MAXKEYLENGTH)
		rlog(LOG_ERROR,"Json key too long.");


	if (!json_reader_read_member(c->root_reader, key_concatted)) {
		rlog(LOG_ERROR, "Could not read json member: %s",key_concatted);
		goto err;
	}

	out->ht = g_hash_table_new_full(g_str_hash, g_str_equal, free, freeReplyObject);
	if (!out->ht)
		goto err;
	out->rr = 0;

	gchar **members = json_reader_list_members(c->root_reader);
	gchar **orig_members = members;

	for (int i=0; i < json_reader_count_members (c->root_reader); ++i) {

		if (!json_reader_read_member(c->root_reader, *members)) {
			rlog(LOG_ERROR, "Could not read json member: %s",*members);
			goto err3;
		}
		out->rr = createReplyObject(REDIS_REPLY_STRING);
		if (!out->rr) {
			rlog(LOG_ERROR, "Could not create redis reply object (out of memory?)");
			goto err3;
		}
		out->rr->str = json_reader_get_string_value_uri_enc(c->root_reader, &out->rr->len);

		char* tmp = strdup(*members);

		if (g_hash_table_insert_check(out->ht, tmp, out->rr) != TRUE) {
			goto err3;
		}

		json_reader_end_member(c->root_reader);

		++members;
	} // for
	g_strfreev(orig_members);
	json_reader_end_member (c->root_reader);

	return 0;

err3:
	g_strfreev(members);
	if (out->rr)
		freeReplyObject(out->rr);
	g_hash_table_destroy(out->ht);
err:
	return -1;
}


static int redis_get_hash(struct redis_hash *out, struct redis *r, const char *key, const str *which,
		unsigned int id)
{
	redisReply *k, *v;
	int i;

	out->ht = g_hash_table_new(g_str_hash, g_str_equal);
	if (!out->ht)
		goto err;
	if (id == -1)
		out->rr = redis_get(r, REDIS_REPLY_ARRAY, "HGETALL %s-"PB"", key, STR(which));
	else
		out->rr = redis_get(r, REDIS_REPLY_ARRAY, "HGETALL %s-"PB"-%u", key, STR(which), id);
	if (!out->rr)
		goto err2;

	for (i = 1; i < out->rr->elements; i += 2) {
		k = out->rr->element[i - 1];
		v = out->rr->element[i];
		if (k->type != REDIS_REPLY_STRING || v->type != REDIS_REPLY_STRING)
			continue;

		if (g_hash_table_insert_check(out->ht, k->str, v) != TRUE)
			goto err3;
	}

	return 0;

err3:
	if (out->rr)
		freeReplyObject(out->rr);
err2:
	g_hash_table_destroy(out->ht);
err:
	return -1;
}


static void redis_destroy_hash(struct redis_hash *rh) {

	if (rh->rr)
		freeReplyObject(rh->rr);
	g_hash_table_destroy(rh->ht);
}
static void redis_destroy_list(struct redis_list *rl) {
	unsigned int i;

	for (i = 0; i < rl->len; i++) {
		redis_destroy_hash(&rl->rh[i]);
	}
	free(rl->rh);
	free(rl->ptrs);
}

static void json_destroy_hash(struct redis_hash *rh) {
        g_hash_table_destroy(rh->ht);
}

static void json_destroy_list(struct redis_list *rl) {
        unsigned int i;

        for (i = 0; i < rl->len; i++) {
                json_destroy_hash(&rl->rh[i]);
        }
        free(rl->rh);
        free(rl->ptrs);
}

static int redis_hash_get_str(str *out, const struct redis_hash *h, const char *k) {
	redisReply *r;

	r = g_hash_table_lookup(h->ht, k);
	if (!r) {
		out->s = NULL;
		out->len = 0;
		return -1;
	}
	out->s = r->str;
	out->len = r->len;
	return 0;
}

/* we can do this because this happens during startup in a single thread */
static atomic64 strtoa64(const char *c, char **endp, int base) {
	u_int64_t u;
	atomic64 ret;

	u = strtoull(c, endp, base);
	atomic64_set_na(&ret, u);
	return ret;
}

define_get_int_type(time_t, time_t, strtoull);
define_get_int_type(int, int, strtol);
define_get_int_type(unsigned, unsigned int, strtol);
//define_get_int_type(u16, u_int16_t, strtol);
define_get_int_type(u64, u_int64_t, strtoull);
define_get_int_type(a64, atomic64, strtoa64);

define_get_type_format(str, str);
define_get_type_format(int, int);
//define_get_type_format(unsigned, unsigned int);
//define_get_type_format(u16, u_int16_t);
//define_get_type_format(u64, u_int64_t);
define_get_type_format(a64, atomic64);

static int redis_hash_get_c_buf_fn(unsigned char *out, size_t len, const struct redis_hash *h,
		const char *k, ...)
{
	va_list ap;
	str s;
	int ret;

	va_start(ap, k);
	ret = redis_hash_get_str_v(&s, h, k, ap);
	va_end(ap);
	if (ret)
		return -1;
	if (s.len > len)
		return -1;

	memcpy(out, s.s, s.len);

	return 0;
}

#define redis_hash_get_c_buf_f(o, h, f...) \
		redis_hash_get_c_buf_fn(o, sizeof(o), h, f)

static int redis_hash_get_bool_flag(const struct redis_hash *h, const char *k) {
	int i;

	if (redis_hash_get_int(&i, h, k))
		return 0;
	if (i)
		return -1;
	return 0;
}

static int redis_hash_get_endpoint(struct endpoint *out, const struct redis_hash *h, const char *k) {
	str s;

	if (redis_hash_get_str(&s, h, k))
		return -1;
	if (endpoint_parse_any(out, s.s))
		return -1;

	return 0;
}
static int redis_hash_get_stats(struct stats *out, const struct redis_hash *h, const char *k) {
	if (redis_hash_get_a64_f(&out->packets, h, "%s-packets", k))
		return -1;
	if (redis_hash_get_a64_f(&out->bytes, h, "%s-bytes", k))
		return -1;
	if (redis_hash_get_a64_f(&out->errors, h, "%s-errors", k))
		return -1;
	return 0;
}
static void *redis_list_get_idx_ptr(struct redis_list *list, unsigned int idx) {
	if (idx >= list->len)
		return NULL;
	return list->ptrs[idx];
}
static void *redis_list_get_ptr(struct redis_list *list, struct redis_hash *rh, const char *key) {
	unsigned int idx;
	if (redis_hash_get_unsigned(&idx, rh, key))
		return NULL;
	return redis_list_get_idx_ptr(list, idx);
}

static int json_build_list_cb(GQueue *q, struct call *c, const char *key,
		unsigned int idx, struct redis_list *list,
		int (*cb)(str *, GQueue *, struct redis_list *, void *), void *ptr)
{
	str s;
	char key_concatted[256];

	snprintf(key_concatted, 256, "%s-%u", key, idx);

	if (!json_reader_read_member(c->root_reader, key_concatted))
		rlog(LOG_ERROR,"Key in json not found:%s",key_concatted);
	for (int jidx=0; jidx < json_reader_count_elements(c->root_reader); ++jidx) {
		if (!json_reader_read_element(c->root_reader,jidx))
			rlog(LOG_ERROR,"Element in array not found.");
		char *strp = s.s = json_reader_get_string_value_uri_enc(c->root_reader, &s.len);
		if (!strp)
			rlog(LOG_ERROR,"String in json not found.");
		if (cb(&s, q, list, ptr)) {
			free(s.s);
			return -1;
		}
		free(strp);
		json_reader_end_element(c->root_reader);
	}
	json_reader_end_member (c->root_reader);

	return 0;
}


static int redis_build_list_cb(GQueue *q, struct redis *r, const char *key, const str *callid,
		unsigned int idx, struct redis_list *list,
		int (*cb)(str *, GQueue *, struct redis_list *, void *), void *ptr)
{
	redisReply *rr;
	int i;
	str s;

	rr = redis_get(r, REDIS_REPLY_ARRAY, "LRANGE %s-"PB"-%u 0 -1", key, STR(callid), idx);
	if (!rr)
		return -1;

	for (i = 0; i < rr->elements; i++) {
		if (rr->element[i]->type != REDIS_REPLY_STRING) {
			freeReplyObject(rr);
			return -1;
		}
		str_init_len(&s, rr->element[i]->str, rr->element[i]->len);
		if (cb(&s, q, list, ptr)) {
			freeReplyObject(rr);
			return -1;
		}
	}

	freeReplyObject(rr);
	return 0;
}
static int rbl_cb_simple(str *s, GQueue *q, struct redis_list *list, void *ptr) {
	int j;
	j = str_to_i(s, 0);
	g_queue_push_tail(q, redis_list_get_idx_ptr(list, (unsigned) j));
	return 0;
}

static int json_build_list(GQueue *q, struct call *c, const char *key, const str *callid,
		unsigned int idx, struct redis_list *list)
{
	return json_build_list_cb(q, c, key, idx, list, rbl_cb_simple, NULL);
}

static int redis_build_list(GQueue *q, struct redis *r, const char *key, const str *callid,
		unsigned int idx, struct redis_list *list)
{
	return redis_build_list_cb(q, r, key, callid, idx, list, rbl_cb_simple, NULL);
}

static int json_get_list_hash(struct redis_list *out, struct call* c,
		const char *key,
		const struct redis_hash *rh, const char *rh_num_key)
{
	unsigned int i;

	if (redis_hash_get_unsigned(&out->len, rh, rh_num_key))
		return -1;
	out->rh = malloc(sizeof(*out->rh) * out->len);
	if (!out->rh)
		return -1;
	out->ptrs = malloc(sizeof(*out->ptrs) * out->len);
	if (!out->ptrs)
		goto err1;

	for (i = 0; i < out->len; i++) {
		if (json_get_hash(&out->rh[i], c, key, i))
			goto err2;
	}

	return 0;

err2:
	free(out->ptrs);
	while (i) {
		i--;
		json_destroy_hash(&out->rh[i]);
	}
err1:
	free(out->rh);
	return -1;
}

static int redis_get_list_hash(struct redis_list *out, struct redis *r, const char *key, const str *id,
		const struct redis_hash *rh, const char *rh_num_key)
{
	unsigned int i;

	if (redis_hash_get_unsigned(&out->len, rh, rh_num_key))
		return -1;
	out->rh = malloc(sizeof(*out->rh) * out->len);
	if (!out->rh)
		return -1;
	out->ptrs = malloc(sizeof(*out->ptrs) * out->len);
	if (!out->ptrs)
		goto err1;

	for (i = 0; i < out->len; i++) {
		if (redis_get_hash(&out->rh[i], r, key, id, i))
			goto err2;
	}

	return 0;

err2:
	free(out->ptrs);
	while (i) {
		i--;
		redis_destroy_hash(&out->rh[i]);
	}
err1:
	free(out->rh);
	return -1;
}




/* can return 1, 0 or -1 */
static int redis_hash_get_crypto_params(struct crypto_params *out, const struct redis_hash *h, const char *k) {
	str s;
	int i;
	const char *err;

	if (redis_hash_get_str_f(&s, h, "%s-crypto_suite", k))
		return 1;
	out->crypto_suite = crypto_find_suite(&s);
	err = "crypto suite not known";
	if (!out->crypto_suite)
		goto err;

	err = "master_key";
	if (redis_hash_get_c_buf_f(out->master_key, h, "%s-master_key", k))
		goto err;
	err = "master_salt";
	if (redis_hash_get_c_buf_f(out->master_salt, h, "%s-master_salt", k))
		goto err;

	if (!redis_hash_get_str_f(&s, h, "%s-mki", k)) {
		err = "mki too long";
		if (s.len > 255)
			return -1;
		out->mki = malloc(s.len);
		memcpy(out->mki, s.s, s.len);
		out->mki_len = s.len;
	}

	if (!redis_hash_get_int_f(&i, h, "%s-unenc-srtp", k))
		out->session_params.unencrypted_srtp = i;
	if (!redis_hash_get_int_f(&i, h, "%s-unenc-srtcp", k))
		out->session_params.unencrypted_srtcp = i;
	if (!redis_hash_get_int_f(&i, h, "%s-unauth-srtp", k))
		out->session_params.unauthenticated_srtp = i;

	return 0;

err:
	rlog(LOG_ERR, "Crypto params error: %s", err);
	return -1;
}
static int redis_hash_get_crypto_context(struct crypto_context *out, const struct redis_hash *h) {
	int ret;

	ret = redis_hash_get_crypto_params(&out->params, h, "");
	if (ret == 1)
		return 0;
	else if (ret)
		return -1;

	if (redis_hash_get_u64(&out->last_index, h, "last_index"))
		return -1;
	redis_hash_get_unsigned(&out->ssrc, h, "ssrc");

	return 0;
}

static int redis_sfds(struct call *c, struct redis_list *sfds) {
	unsigned int i;
	str family, intf_name;
	struct redis_hash *rh;
	sockfamily_t *fam;
	struct logical_intf *lif;
	struct local_intf *loc;
	GQueue q = G_QUEUE_INIT;
	unsigned int loc_uid;
	struct stream_fd *sfd;
	socket_t *sock;
	int port;
	const char *err;

	for (i = 0; i < sfds->len; i++) {
		rh = &sfds->rh[i];

		err = "'localport' key not present";
		if (redis_hash_get_int(&port, rh, "localport"))
			goto err;
		err = "'pref_family' key not present";
		if (redis_hash_get_str(&family, rh, "pref_family"))
			goto err;
		err = "'logical_intf' key not present";
		if (redis_hash_get_str(&intf_name, rh, "logical_intf"))
			goto err;
		err = "'local_intf_uid' key not present";
		if (redis_hash_get_unsigned(&loc_uid, rh, "local_intf_uid"))
			goto err;

		err = "socket family not known";
		fam = get_socket_family_rfc(&family);
		if (!fam)
			goto err;
		err = "logical interface not known";
		lif = get_logical_interface(&intf_name, fam, 0);
		if (!lif)
			goto err;
		err = "not enough local interfaces";
		loc = g_queue_peek_nth(&lif->list, loc_uid);
		if (!loc)
			goto err;

		err = "failed to open ports";
		if (__get_consecutive_ports(&q, 1, port, loc->spec))
			goto err;
		err = "no port returned";
		sock = g_queue_pop_head(&q);
		if (!sock)
			goto err;
		sfd = stream_fd_new(sock, c, loc);
		// XXX tos
		err = "failed to config crypto context";
		if (redis_hash_get_crypto_context(&sfd->crypto, rh))
			goto err;

		sfds->ptrs[i] = sfd;
	}
	return 0;

err:
	rlog(LOG_ERR, "Error creating sfd: %s", err);
	return -1;
}

static int redis_streams(struct call *c, struct redis_list *streams) {
	unsigned int i;
	struct redis_hash *rh;
	struct packet_stream *ps;

	for (i = 0; i < streams->len; i++) {
		rh = &streams->rh[i];

		ps = __packet_stream_new(c);
		if (!ps)
			return -1;

		atomic64_set_na(&ps->last_packet, time(NULL));
		if (redis_hash_get_unsigned((unsigned int *) &ps->ps_flags, rh, "ps_flags"))
			return -1;
		if (redis_hash_get_unsigned((unsigned int *) &ps->component, rh, "component"))
			return -1;
		if (redis_hash_get_endpoint(&ps->endpoint, rh, "endpoint"))
			return -1;
		if (redis_hash_get_endpoint(&ps->advertised_endpoint, rh, "advertised_endpoint"))
			return -1;
		if (redis_hash_get_stats(&ps->stats, rh, "stats"))
			return -1;
		if (redis_hash_get_crypto_context(&ps->crypto, rh))
			return -1;

		streams->ptrs[i] = ps;

		PS_CLEAR(ps, KERNELIZED);
	}
	return 0;
}

static int redis_tags(struct call *c, struct redis_list *tags) {
	unsigned int i;
	struct redis_hash *rh;
	struct call_monologue *ml;
	str s;

	for (i = 0; i < tags->len; i++) {
		rh = &tags->rh[i];

		ml = __monologue_create(c);
		if (!ml)
			return -1;

		if (redis_hash_get_time_t(&ml->created, rh, "created"))
			return -1;
		if (!redis_hash_get_str(&s, rh, "tag"))
			__monologue_tag(ml, &s);
		if (!redis_hash_get_str(&s, rh, "via-branch"))
			__monologue_viabranch(ml, &s);
		redis_hash_get_time_t(&ml->deleted, rh, "deleted");

		tags->ptrs[i] = ml;
	}

	return 0;
}

static int rbl_cb_plts(str *s, GQueue *q, struct redis_list *list, void *ptr) {
	struct rtp_payload_type *pt;
	str ptype, enc, clock, parms;
	struct call_media *med = ptr;
	struct call *call = med->call;

	if (str_token(&ptype, s, '/'))
		return -1;
	if (str_token(&enc, s, '/'))
		return -1;
	if (str_token(&clock, s, '/'))
		return -1;
	parms = *s;

	// from call.c
	// XXX remove all the duplicate code
	pt = g_slice_alloc0(sizeof(*pt));
	pt->payload_type = str_to_ui(&ptype, 0);
	call_str_cpy(call, &pt->encoding, &enc);
	pt->clock_rate = str_to_ui(&clock, 0);
	call_str_cpy(call, &pt->encoding_parameters, &parms);
	g_hash_table_replace(med->rtp_payload_types, &pt->payload_type, pt);
	return 0;
}
static int json_medias(struct redis *r, struct call *c, struct redis_list *medias) {
	unsigned int i;
	struct redis_hash *rh;
	struct call_media *med;
	str s;

	for (i = 0; i < medias->len; i++) {
		rh = &medias->rh[i];

		/* from call.c:__get_media() */
		med = uid_slice_alloc0(med, &c->medias);
		med->call = c;
		med->rtp_payload_types = g_hash_table_new_full(g_int_hash, g_int_equal, NULL,
				__payload_type_free);

		if (redis_hash_get_unsigned(&med->index, rh, "index"))
			return -1;
		if (redis_hash_get_str(&s, rh, "type"))
			return -1;
		call_str_cpy(c, &med->type, &s);

		if (redis_hash_get_str(&s, rh, "protocol"))
			return -1;
		med->protocol = transport_protocol(&s);

		if (redis_hash_get_str(&s, rh, "desired_family"))
			return -1;
		med->desired_family = get_socket_family_rfc(&s);

		if (redis_hash_get_str(&s, rh, "logical_intf")
				|| !(med->logical_intf = get_logical_interface(&s, med->desired_family, 0)))
		{
			rlog(LOG_ERR, "unable to find specified local interface");
			med->logical_intf = get_logical_interface(NULL, med->desired_family, 0);
		}

		if (redis_hash_get_unsigned(&med->sdes_in.tag, rh, "sdes_in_tag"))
			return -1;
		if (redis_hash_get_unsigned(&med->sdes_out.tag, rh, "sdes_out_tag"))
			return -1;
		if (redis_hash_get_unsigned((unsigned int *) &med->media_flags, rh,
					"media_flags"))
			return -1;
		if (redis_hash_get_crypto_params(&med->sdes_in.params, rh, "sdes_in") < 0)
			return -1;
		if (redis_hash_get_crypto_params(&med->sdes_out.params, rh, "sdes_out") < 0)
			return -1;

		json_build_list_cb(NULL, c, "payload_types", i, NULL, rbl_cb_plts, med);
		/* XXX dtls */

		medias->ptrs[i] = med;
	}

	return 0;
}
static int redis_medias(struct redis *r, struct call *c, struct redis_list *medias) {
	unsigned int i;
	struct redis_hash *rh;
	struct call_media *med;
	str s;

	for (i = 0; i < medias->len; i++) {
		rh = &medias->rh[i];

		/* from call.c:__get_media() */
		med = uid_slice_alloc0(med, &c->medias);
		med->call = c;
		med->rtp_payload_types = g_hash_table_new_full(g_int_hash, g_int_equal, NULL,
				__payload_type_free);

		if (redis_hash_get_unsigned(&med->index, rh, "index"))
			return -1;
		if (redis_hash_get_str(&s, rh, "type"))
			return -1;
		call_str_cpy(c, &med->type, &s);

		if (redis_hash_get_str(&s, rh, "protocol"))
			return -1;
		med->protocol = transport_protocol(&s);

		if (redis_hash_get_str(&s, rh, "desired_family"))
			return -1;
		med->desired_family = get_socket_family_rfc(&s);

		if (redis_hash_get_str(&s, rh, "logical_intf")
				|| !(med->logical_intf = get_logical_interface(&s, med->desired_family, 0)))
		{
			rlog(LOG_ERR, "unable to find specified local interface");
			med->logical_intf = get_logical_interface(NULL, med->desired_family, 0);
		}

		if (redis_hash_get_unsigned(&med->sdes_in.tag, rh, "sdes_in_tag"))
			return -1;
		if (redis_hash_get_unsigned(&med->sdes_out.tag, rh, "sdes_out_tag"))
			return -1;
		if (redis_hash_get_unsigned((unsigned int *) &med->media_flags, rh,
					"media_flags"))
			return -1;
		if (redis_hash_get_crypto_params(&med->sdes_in.params, rh, "sdes_in") < 0)
			return -1;
		if (redis_hash_get_crypto_params(&med->sdes_out.params, rh, "sdes_out") < 0)
			return -1;

		redis_build_list_cb(NULL, r, "payload_types", &c->callid, i, NULL, rbl_cb_plts, med);
		/* XXX dtls */

		medias->ptrs[i] = med;
	}

	return 0;
}

static int redis_maps(struct call *c, struct redis_list *maps) {
	unsigned int i;
	struct redis_hash *rh;
	struct endpoint_map *em;
	str s, t;
	sockfamily_t *fam;

	for (i = 0; i < maps->len; i++) {
		rh = &maps->rh[i];

		/* from call.c:__get_endpoint_map() */
		em = uid_slice_alloc0(em, &c->endpoint_maps);
		g_queue_init(&em->intf_sfds);

		em->wildcard = redis_hash_get_bool_flag(rh, "wildcard");
		if (redis_hash_get_unsigned(&em->num_ports, rh, "num_ports"))
			return -1;
		if (redis_hash_get_str(&t, rh, "intf_preferred_family"))
			return -1;
		fam = get_socket_family_rfc(&t);
		if (!fam)
			return -1;
		if (redis_hash_get_str(&s, rh, "logical_intf")
				|| !(em->logical_intf = get_logical_interface(&s, fam, 0)))
		{
			rlog(LOG_ERR, "unable to find specified local interface");
			em->logical_intf = get_logical_interface(NULL, fam, 0);
		}
		if (redis_hash_get_endpoint(&em->endpoint, rh, "endpoint"))
			return -1;

		maps->ptrs[i] = em;
	}

	return 0;
}

static int redis_link_sfds(struct redis_list *sfds, struct redis_list *streams) {
	unsigned int i;
	struct stream_fd *sfd;

	for (i = 0; i < sfds->len; i++) {
		sfd = sfds->ptrs[i];

		sfd->stream = redis_list_get_ptr(streams, &sfds->rh[i], "stream");
		if (!sfd->stream)
			return -1;
	}

	return 0;
}

static int json_link_tags(struct call *c, struct redis_list *tags, struct redis_list *medias)
{
	unsigned int i;
	struct call_monologue *ml, *other_ml;
	GQueue q = G_QUEUE_INIT;
	GList *l;

	for (i = 0; i < tags->len; i++) {
		ml = tags->ptrs[i];

		ml->active_dialogue = redis_list_get_ptr(tags, &tags->rh[i], "active");

		if (json_build_list(&q, c, "other_tags", &c->callid, i, tags))
			return -1;
		for (l = q.head; l; l = l->next) {
			other_ml = l->data;
			if (!other_ml)
			    return -1;
			g_hash_table_insert(ml->other_tags, &other_ml->tag, other_ml);
		}
		g_queue_clear(&q);

		if (json_build_list(&ml->medias, c, "medias", &c->callid, i, medias))
			return -1;
	}

	return 0;
}

static int redis_link_tags(struct redis *r, struct call *c, struct redis_list *tags, struct redis_list *medias)
{
	unsigned int i;
	struct call_monologue *ml, *other_ml;
	GQueue q = G_QUEUE_INIT;
	GList *l;

	for (i = 0; i < tags->len; i++) {
		ml = tags->ptrs[i];

		ml->active_dialogue = redis_list_get_ptr(tags, &tags->rh[i], "active");

		if (redis_build_list(&q, r, "other_tags", &c->callid, i, tags))
			return -1;
		for (l = q.head; l; l = l->next) {
			other_ml = l->data;
			if (!other_ml)
			    return -1;
			g_hash_table_insert(ml->other_tags, &other_ml->tag, other_ml);
		}
		g_queue_clear(&q);

		if (redis_build_list(&ml->medias, r, "medias", &c->callid, i, medias))
			return -1;
	}

	return 0;
}

static int json_link_streams(struct call *c, struct redis_list *streams,
		struct redis_list *sfds, struct redis_list *medias)
{
	unsigned int i;
	struct packet_stream *ps;

	for (i = 0; i < streams->len; i++) {
		ps = streams->ptrs[i];

		ps->media = redis_list_get_ptr(medias, &streams->rh[i], "media");
		ps->selected_sfd = redis_list_get_ptr(sfds, &streams->rh[i], "sfd");
		ps->rtp_sink = redis_list_get_ptr(streams, &streams->rh[i], "rtp_sink");
		ps->rtcp_sink = redis_list_get_ptr(streams, &streams->rh[i], "rtcp_sink");
		ps->rtcp_sibling = redis_list_get_ptr(streams, &streams->rh[i], "rtcp_sibling");

		if (json_build_list(&ps->sfds, c, "stream_sfds", &c->callid, i, sfds))
			return -1;

		if (ps->media)
			__rtp_stats_update(ps->rtp_stats, ps->media->rtp_payload_types);
	}

	return 0;
}

static int redis_link_streams(struct redis *r, struct call *c, struct redis_list *streams,
		struct redis_list *sfds, struct redis_list *medias)
{
	unsigned int i;
	struct packet_stream *ps;

	for (i = 0; i < streams->len; i++) {
		ps = streams->ptrs[i];

		ps->media = redis_list_get_ptr(medias, &streams->rh[i], "media");
		ps->selected_sfd = redis_list_get_ptr(sfds, &streams->rh[i], "sfd");
		ps->rtp_sink = redis_list_get_ptr(streams, &streams->rh[i], "rtp_sink");
		ps->rtcp_sink = redis_list_get_ptr(streams, &streams->rh[i], "rtcp_sink");
		ps->rtcp_sibling = redis_list_get_ptr(streams, &streams->rh[i], "rtcp_sibling");

		if (redis_build_list(&ps->sfds, r, "stream_sfds", &c->callid, i, sfds))
			return -1;

		if (ps->media)
			__rtp_stats_update(ps->rtp_stats, ps->media->rtp_payload_types);
	}

	return 0;
}

static int json_link_medias(struct redis *r, struct call *c, struct redis_list *medias,
		struct redis_list *streams, struct redis_list *maps, struct redis_list *tags)
{
	unsigned int i;
	struct call_media *med;

	for (i = 0; i < medias->len; i++) {
		med = medias->ptrs[i];

		med->monologue = redis_list_get_ptr(tags, &medias->rh[i], "tag");
		if (!med->monologue)
			return -1;
		if (json_build_list(&med->streams, c, "streams", &c->callid, i, streams))
			return -1;
		if (json_build_list(&med->endpoint_maps, c, "maps", &c->callid, i, maps))
			return -1;
	}
	return 0;
}

static int redis_link_medias(struct redis *r, struct call *c, struct redis_list *medias,
		struct redis_list *streams, struct redis_list *maps, struct redis_list *tags)
{
	unsigned int i;
	struct call_media *med;

	for (i = 0; i < medias->len; i++) {
		med = medias->ptrs[i];

		med->monologue = redis_list_get_ptr(tags, &medias->rh[i], "tag");
		if (!med->monologue)
			return -1;
		if (redis_build_list(&med->streams, r, "streams", &c->callid, i, streams))
			return -1;
		if (redis_build_list(&med->endpoint_maps, r, "maps", &c->callid, i, maps))
			return -1;
	}
	return 0;
}

static int rbl_cb_intf_sfds(str *s, GQueue *q, struct redis_list *list, void *ptr) {
	int i;
	struct intf_list *il;
	struct endpoint_map *em;
	void *sfd;

	if (!strncmp(s->s, "loc-", 4)) {
		il = g_slice_alloc0(sizeof(*il));
		em = ptr;
		i = atoi(s->s+4);
		il->local_intf = g_queue_peek_nth((GQueue*) &em->logical_intf->list, i);
		if (!il->local_intf)
			return -1;
		g_queue_push_tail(q, il);
		return 0;
	}

	il = g_queue_peek_tail(q);
	if (!il)
		return -1;

	sfd = redis_list_get_idx_ptr(list, atoi(s->s));
	if (G_UNLIKELY(!sfd))
	    return -1;

	g_queue_push_tail(&il->list, sfd);
	return 0;
}

static int json_link_maps(struct redis *r, struct call *c, struct redis_list *maps,
		struct redis_list *sfds)
{
	unsigned int i;
	struct endpoint_map *em;

	for (i = 0; i < maps->len; i++) {
		em = maps->ptrs[i];

		if (json_build_list_cb(&em->intf_sfds, c, "map_sfds", em->unique_id, sfds,
				rbl_cb_intf_sfds, em))
			return -1;
	}
	return 0;
}

static int redis_link_maps(struct redis *r, struct call *c, struct redis_list *maps,
		struct redis_list *sfds)
{
	unsigned int i;
	struct endpoint_map *em;

	for (i = 0; i < maps->len; i++) {
		em = maps->ptrs[i];

		if (redis_build_list_cb(&em->intf_sfds, r, "map_sfds", &c->callid, em->unique_id, sfds,
					rbl_cb_intf_sfds, em))
			return -1;
	}
	return 0;
}

static void json_restore_call(struct redis *r, struct callmaster *m, const str *callid, enum call_type type) {
	redisReply* rr_jsonStr;
	struct redis_hash call;
	struct redis_list tags, sfds, streams, medias, maps;
	struct call *c = NULL;
	const char *err = 0;
	int i;
	str s;
	JsonReader *root_reader =0;
	JsonParser *parser =0;

	rr_jsonStr = redis_get(r, REDIS_REPLY_STRING, "GET json-" PB, STR(callid));
	if (!rr_jsonStr) {
		rlog(LOG_ERR, "Could not retrieve json data from redis for callid: " STR_FORMAT,
				STR_FMT(callid));
		goto err1;
	}

	parser = json_parser_new();
	if (!json_parser_load_from_data (parser, rr_jsonStr->str, -1, NULL)) {
		rlog(LOG_DEBUG, "Could not parse json data !");
		goto err1;
	}
	root_reader = json_reader_new (json_parser_get_root (parser));
	if (!root_reader) {
		rlog(LOG_DEBUG, "Could not read json data !");
		goto err1;
	}

	c = call_get_or_create(callid, m, type);
	err = "failed to create call struct";
	if (!c)
		goto err1;

	c->root_reader = root_reader; // attach the json to the call in order to restore data from there
	
	err = "call already exists";
	if (c->last_signal)
		goto err2;
	err = "'call' data incomplete";
	if (json_get_hash(&call, c, "json", -1))
		goto err2;
	err = "'tags' incomplete";
	if (json_get_list_hash(&tags, c, "tag", &call, "num_tags"))
		goto err3;
	err = "'sfds' incomplete";
	if (json_get_list_hash(&sfds, c, "sfd", &call, "num_sfds"))
		goto err4;
	err = "'streams' incomplete";
	if (json_get_list_hash(&streams, c, "stream", &call, "num_streams"))
		goto err5;
	err = "'medias' incomplete";
	if (json_get_list_hash(&medias, c, "media", &call, "num_medias"))
		goto err6;
	err = "'maps' incomplete";
	if (json_get_list_hash(&maps, c, "map", &call, "num_maps"))
		goto err7;

	err = "missing 'created' timestamp";
	if (redis_hash_get_time_t(&c->created, &call, "created"))
		goto err8;
	err = "missing 'last signal' timestamp";
	if (redis_hash_get_time_t(&c->last_signal, &call, "last_signal"))
		goto err8;
	if (redis_hash_get_int(&i, &call, "tos"))
		c->tos = 184;
	else
		c->tos = i;
	redis_hash_get_time_t(&c->deleted, &call, "deleted");
	redis_hash_get_time_t(&c->ml_deleted, &call, "ml_deleted");
	if (!redis_hash_get_str(&s, &call, "created_from"))
		c->created_from = call_strdup(c, s.s);
	if (!redis_hash_get_str(&s, &call, "created_from_addr"))
		sockaddr_parse_any_str(&c->created_from_addr, &s);

	err = "missing 'redis_hosted_db' value";
	if (redis_hash_get_unsigned((unsigned int *) &c->redis_hosted_db, &call, "redis_hosted_db"))
		goto err8;

	err = "failed to create sfds";
	if (redis_sfds(c, &sfds))
		goto err8;
	err = "failed to create streams";
	if (redis_streams(c, &streams))
		goto err8;
	err = "failed to create tags";
	if (redis_tags(c, &tags))
		goto err8;
	err = "failed to create medias";
	if (json_medias(r, c, &medias))
		goto err8;
	err = "failed to create maps";
	if (redis_maps(c, &maps))
		goto err8;

	err = "failed to link sfds";
	if (redis_link_sfds(&sfds, &streams))
		goto err8;
	err = "failed to link streams";
	if (json_link_streams(c, &streams, &sfds, &medias))
		goto err8;
	err = "failed to link tags";
	if (json_link_tags(c, &tags, &medias))
		goto err8;
	err = "failed to link medias";
	if (json_link_medias(r, c, &medias, &streams, &maps, &tags))
		goto err8;
	err = "failed to link maps";
	if (json_link_maps(r, c, &maps, &sfds))
		goto err8;

	err = NULL;

err8:
	json_destroy_list(&maps);
err7:
	json_destroy_list(&medias);
err6:
	json_destroy_list(&streams);
err5:
	json_destroy_list(&sfds);
err4:
	json_destroy_list(&tags);
err3:
	json_destroy_hash(&call);
err2:
	rwlock_unlock_w(&c->master_lock);
err1:
	if (root_reader)
		g_object_unref (root_reader);
	if (parser)
		g_object_unref (parser);
	if (rr_jsonStr)
		freeReplyObject(rr_jsonStr);	
	log_info_clear();
	if (err) {
		rlog(LOG_WARNING, "Failed to restore call ID '" STR_FORMAT "' from Redis: %s", STR_FMT(callid),
				err);
		if (c) 
			call_destroy(c);
		else
			redisCommandNR(m->conf.redis_write->ctx, "DEL json-" PB, STR(callid));
	}
	if (c)
		obj_put(c);
}

static void redis_restore_recording(struct call *c, struct redis_hash *call) {
	str s;

	// presence of this key determines whether we were recording at all
	if (redis_hash_get_str(&s, call, "recording_meta_prefix"))
		return;

	recording_start(c, s.s);

	if (!redis_hash_get_str(&s, call, "recording_metadata"))
		call_str_cpy(c, &c->recording->metadata, &s);
}


static void redis_restore_call(struct redis *r, struct callmaster *m, const str *id, enum call_type type) {
	struct redis_hash call;
	struct redis_list tags, sfds, streams, medias, maps;
	struct call *c = NULL;
	str s;
	const char *err;
	int i;

	c = call_get_or_create(id, m, type);
	err = "failed to create call struct";
	if (!c)
		goto err1;
	err = "call already exists";
	if (c->last_signal)
		goto err2;
	err = "'call' data incomplete";
	if (redis_get_hash(&call, r, "call", id, -1))
		goto err2;
	err = "'tags' incomplete";
	if (redis_get_list_hash(&tags, r, "tag", id, &call, "num_tags"))
		goto err3;
	err = "'sfds' incomplete";
	if (redis_get_list_hash(&sfds, r, "sfd", id, &call, "num_sfds"))
		goto err4;
	err = "'streams' incomplete";
	if (redis_get_list_hash(&streams, r, "stream", id, &call, "num_streams"))
		goto err5;
	err = "'medias' incomplete";
	if (redis_get_list_hash(&medias, r, "media", id, &call, "num_medias"))
		goto err6;
	err = "'maps' incomplete";
	if (redis_get_list_hash(&maps, r, "map", id, &call, "num_maps"))
		goto err7;

	err = "missing 'created' timestamp";
	if (redis_hash_get_time_t(&c->created, &call, "created"))
		goto err8;
	err = "missing 'last signal' timestamp";
	if (redis_hash_get_time_t(&c->last_signal, &call, "last_signal"))
		goto err8;
	if (redis_hash_get_int(&i, &call, "tos"))
		c->tos = 184;
	else
		c->tos = i;
	redis_hash_get_time_t(&c->deleted, &call, "deleted");
	redis_hash_get_time_t(&c->ml_deleted, &call, "ml_deleted");
	if (!redis_hash_get_str(&s, &call, "created_from"))
		c->created_from = call_strdup(c, s.s);
	if (!redis_hash_get_str(&s, &call, "created_from_addr"))
		sockaddr_parse_any_str(&c->created_from_addr, &s);

	err = "missing 'redis_hosted_db' value";
	if (redis_hash_get_unsigned((unsigned int *) &c->redis_hosted_db, &call, "redis_hosted_db"))
		goto err8;

	err = "failed to create sfds";
	if (redis_sfds(c, &sfds))
		goto err8;
	err = "failed to create streams";
	if (redis_streams(c, &streams))
		goto err8;
	err = "failed to create tags";
	if (redis_tags(c, &tags))
		goto err8;
	err = "failed to create medias";
	if (redis_medias(r, c, &medias))
		goto err8;
	err = "failed to create maps";
	if (redis_maps(c, &maps))
		goto err8;

	err = "failed to link sfds";
	if (redis_link_sfds(&sfds, &streams))
		goto err8;
	err = "failed to link streams";
	if (redis_link_streams(r, c, &streams, &sfds, &medias))
		goto err8;
	err = "failed to link tags";
	if (redis_link_tags(r, c, &tags, &medias))
		goto err8;
	err = "failed to link medias";
	if (redis_link_medias(r, c, &medias, &streams, &maps, &tags))
		goto err8;
	err = "failed to link maps";
	if (redis_link_maps(r, c, &maps, &sfds))
		goto err8;

	redis_restore_recording(c, &call);

	err = NULL;

err8:
	redis_destroy_list(&maps);
err7:
	redis_destroy_list(&medias);
err6:
	redis_destroy_list(&streams);
err5:
	redis_destroy_list(&sfds);
err4:
	redis_destroy_list(&tags);
err3:
	redis_destroy_hash(&call);
err2:	
	rwlock_unlock_w(&c->master_lock);
err1:
	log_info_clear();
	if (err) {
		rlog(LOG_WARNING, "Failed to restore call ID '" STR_FORMAT "' from Redis: %s", STR_FMT(id), err);
		if (c)
			call_destroy(c);
		else
			redisCommandNR(m->conf.redis_write->ctx, "SREM calls "PB"", STR(id));
	}

	if (c)
		obj_put(c);
}



struct thread_ctx {
	struct callmaster *m;
	GQueue r_q;
	mutex_t r_m;
};

static void restore_thread(void *call_p, void *ctx_p) {
	struct thread_ctx *ctx = ctx_p;
	redisReply *call = call_p;
	struct redis *r;
	str callid;

	rlog(LOG_DEBUG, "Processing call ID '%.*s' from Redis", REDIS_FMT(call));

	mutex_lock(&ctx->r_m);
	r = g_queue_pop_head(&ctx->r_q);
	mutex_unlock(&ctx->r_m);

	str_init_len(&callid, call->str, call->len);

	if (ctx->m->conf.redis_multikey) {
		redis_restore_call(r, ctx->m, &callid, CT_OWN_CALL);
	} else {
		if (str_shift_cmp(&callid, "json-") == 0)
			json_restore_call(r, ctx->m, &callid, CT_OWN_CALL);
	}

	mutex_lock(&ctx->r_m);
	g_queue_push_tail(&ctx->r_q, r);
	mutex_unlock(&ctx->r_m);
}

int redis_restore(struct callmaster *m, struct redis *r) {
	redisReply *calls = NULL, *call;
	int i, ret = -1;
	GThreadPool *gtp;
	struct thread_ctx ctx;

	if (!r)
		return 0;

	log_level |= LOG_FLAG_RESTORE;

	rlog(LOG_DEBUG, "Restoring calls from Redis...");

	mutex_lock(&r->lock);
	if (redis_check_conn(r) == REDIS_STATE_DISCONNECTED) {
		mutex_unlock(&r->lock);
		ret = 0;
		goto err;
	}
	mutex_unlock(&r->lock);

	if (m->conf.redis_multikey) {
		calls = redis_get(r, REDIS_REPLY_ARRAY, "SMEMBERS calls");
	} else {
		calls = redis_get(r, REDIS_REPLY_ARRAY, "KEYS json-*");
	}

	if (!calls) {
		rlog(LOG_ERR, "Could not retrieve call list from Redis: %s", r->ctx->errstr);
		goto err;
	}

	ctx.m = m;
	mutex_init(&ctx.r_m);
	g_queue_init(&ctx.r_q);
	for (i = 0; i < m->conf.redis_num_threads; i++)
		g_queue_push_tail(&ctx.r_q, redis_new(&r->endpoint, r->db, r->auth, r->role, r->no_redis_required));
	gtp = g_thread_pool_new(restore_thread, &ctx, m->conf.redis_num_threads, TRUE, NULL);

	for (i = 0; i < calls->elements; i++) {
		call = calls->element[i];
		if (call->type != REDIS_REPLY_STRING)
			continue;

		g_thread_pool_push(gtp, call, NULL);
	}

	g_thread_pool_free(gtp, FALSE, TRUE);

	while ((r = g_queue_pop_head(&ctx.r_q)))
		redis_close(r);
	ret = 0;

	freeReplyObject(calls);

err:
	log_level &= ~LOG_FLAG_RESTORE;
	return ret;
}

#define JSON_ADD_STRING(f...) do { \
		int len = snprintf(tmp,sizeof(tmp), f); \
		json_builder_add_string_value_uri_enc(builder, tmp, len); \
	} while (0)
#define JSON_SET_NSTRING(a,b,c,d) do { \
		snprintf(tmp,sizeof(tmp), a,b); \
		json_builder_set_member_name(builder, tmp); \
		JSON_ADD_STRING(c, d); \
	} while (0)
#define JSON_SET_NSTRING_CSTR(a,b,d) JSON_SET_NSTRING_LEN(a, b, strlen(d), d)
#define JSON_SET_NSTRING_LEN(a,b,l,d) do { \
		snprintf(tmp,sizeof(tmp), a,b); \
		json_builder_set_member_name(builder, tmp); \
		json_builder_add_string_value_uri_enc(builder, d, l); \
	} while (0)
#define JSON_SET_SIMPLE(a,c,d) do { \
		json_builder_set_member_name(builder, a); \
		JSON_ADD_STRING(c, d); \
	} while (0)
#define JSON_SET_SIMPLE_LEN(a,l,d) do { \
		json_builder_set_member_name(builder, a); \
		json_builder_add_string_value_uri_enc(builder, d, l); \
	} while (0)
#define JSON_SET_SIMPLE_CSTR(a,d) JSON_SET_SIMPLE_LEN(a, strlen(d), d)
#define JSON_SET_SIMPLE_STR(a,d) JSON_SET_SIMPLE_LEN(a, (d)->len, (d)->s)

static int json_update_crypto_params(JsonBuilder *builder, const char *pref,
		unsigned int unique_id,
		const char *key, const struct crypto_params *p)
{
	char tmp[2048];

	if (!p->crypto_suite)
		return -1;

	JSON_SET_NSTRING_CSTR("%s-crypto_suite",key,p->crypto_suite->name);
	JSON_SET_NSTRING_LEN("%s-master_key",key, sizeof(p->master_key), (char *) p->master_key);
	JSON_SET_NSTRING_LEN("%s-master_salt",key, sizeof(p->master_salt), (char *) p->master_salt);

	JSON_SET_NSTRING("%s-unenc-srtp",key,"%i",p->session_params.unencrypted_srtp);
	JSON_SET_NSTRING("%s-unenc-srtcp",key,"%i",p->session_params.unencrypted_srtcp);
	JSON_SET_NSTRING("%s-unauth-srtp",key,"%i",p->session_params.unauthenticated_srtp);

	if (p->mki) {
		JSON_SET_NSTRING_LEN("%s-mki",key, p->mki_len, (char *) p->mki);
	}

	return 0;
}

static int redis_update_crypto_params(struct redis *r, const char *pref, const str *callid,
		unsigned int unique_id,
		const char *key, const struct crypto_params *p)
{
	if (!p->crypto_suite)
		return -1;
	redis_pipe(r, "HMSET %s-"PB"-%u %s-crypto_suite %s %s-master_key "PB" %s-master_salt "PB" "
			"%s-unenc-srtp %i %s-unenc-srtcp %i %s-unauth-srtp %i",
		pref, STR(callid), unique_id,
		key, p->crypto_suite->name,
		key, S_LEN(p->master_key, sizeof(p->master_key)),
		key, S_LEN(p->master_salt, sizeof(p->master_salt)),
		key, p->session_params.unencrypted_srtp,
		key, p->session_params.unencrypted_srtcp,
		key, p->session_params.unauthenticated_srtp);
	if (p->mki)
		redis_pipe(r, "HMSET %s-"PB"-%u %s-mki "PB"",
			pref, STR(callid), unique_id,
			key,
			S_LEN(p->mki, p->mki_len));

	return 0;
}

static void json_update_crypto_context(JsonBuilder *builder, const char *pref,
		unsigned int unique_id,
		const struct crypto_context *c)
{
	char tmp[2048];

	if (json_update_crypto_params(builder, pref, unique_id, "", &c->params))
		return;

	JSON_SET_SIMPLE("last_index","%lu",c->last_index);
	JSON_SET_SIMPLE("ssrc","%u",(unsigned) c->ssrc);

}

static void redis_update_crypto_context(struct redis *r, const char *pref, const str *callid,
		unsigned int unique_id,
		const struct crypto_context *c)
{
	if (redis_update_crypto_params(r, pref, callid, unique_id, "", &c->params))
		return;
	redis_pipe(r, "HMSET %s-"PB"-%u last_index "UINT64F" ssrc %u",
		pref, STR(callid), unique_id,
		c->last_index, (unsigned) c->ssrc);
}
static void redis_update_endpoint(struct redis *r, const char *pref, const str *callid,
		unsigned int unique_id,
		const char *key, const struct endpoint *e)
{
	redis_pipe(r, "HMSET %s-"PB"-%u %s %s",
		pref, STR(callid), unique_id,
		key, endpoint_print_buf(e));
}
static void redis_update_stats(struct redis *r, const char *pref, const str *callid,
		unsigned int unique_id,
		const char *key, const struct stats *s)
{
	redis_pipe(r, "HMSET %s-"PB"-%u %s-packets "UINT64F" %s-bytes "UINT64F" %s-errors "UINT64F"",
		pref, STR(callid), unique_id,
		key, atomic64_get(&s->packets), key, atomic64_get(&s->bytes),
		key, atomic64_get(&s->errors));
}

static void json_update_dtls_fingerprint(JsonBuilder *builder, const char *pref,
		unsigned int unique_id,
		const struct dtls_fingerprint *f)
{
	if (!f->hash_func)
		return;

	JSON_SET_SIMPLE_CSTR("hash_func",f->hash_func->name);
	JSON_SET_SIMPLE_LEN("fingerprint", sizeof(f->digest), (char *) f->digest);
}

static void redis_update_dtls_fingerprint(struct redis *r, const char *pref, const str *callid,
		unsigned int unique_id,
		const struct dtls_fingerprint *f)
{
	if (!f->hash_func)
		return;
	redis_pipe(r, "HMSET %s-"PB"-%u hash_func %s fingerprint "PB"",
		pref, STR(callid), unique_id,
		f->hash_func->name,
		S_LEN(f->digest, sizeof(f->digest)));
}
/**
 * encodes the few (k,v) pairs for one call under one json structure
 */

char* redis_encode_json(struct call *c) {

	GList *l=0,*k=0, *m=0, *n=0;
	struct endpoint_map *ep;
	struct call_media *media;
	struct rtp_payload_type *pt;
	struct stream_fd *sfd;
	struct packet_stream *ps;
	struct intf_list *il;
	struct call_monologue *ml, *ml2;
	JsonBuilder *builder = json_builder_new ();

	char tmp[2048];

	json_builder_begin_object (builder);
	{
		json_builder_set_member_name(builder, "json");

		json_builder_begin_object (builder);

		{
			JSON_SET_SIMPLE("created","%ld",(long int) c->created);
			JSON_SET_SIMPLE("last_signal","%ld",(long int) c->last_signal);
			JSON_SET_SIMPLE("tos","%u",(int) c->tos);
			JSON_SET_SIMPLE("deleted","%ld",(long int) c->deleted);
			JSON_SET_SIMPLE("num_sfds","%u",g_queue_get_length(&c->stream_fds));
			JSON_SET_SIMPLE("num_streams","%u",g_queue_get_length(&c->streams));
			JSON_SET_SIMPLE("num_medias","%u",g_queue_get_length(&c->medias));
			JSON_SET_SIMPLE("num_tags","%u",g_queue_get_length(&c->monologues));
			JSON_SET_SIMPLE("num_maps","%u",g_queue_get_length(&c->endpoint_maps));
			JSON_SET_SIMPLE("ml_deleted","%ld",(long int) c->ml_deleted);
			JSON_SET_SIMPLE_CSTR("created_from",c->created_from);
			JSON_SET_SIMPLE_CSTR("created_from_addr",sockaddr_print_buf(&c->created_from_addr));
			JSON_SET_SIMPLE("redis_hosted_db","%u",c->redis_hosted_db);

		}

		json_builder_end_object (builder);

		for (l = c->stream_fds.head; l; l = l->next) {
			sfd = l->data;

			snprintf(tmp, sizeof(tmp), "sfd-%u", sfd->unique_id);
			json_builder_set_member_name(builder, tmp);

			json_builder_begin_object (builder);

			{
				JSON_SET_SIMPLE_CSTR("pref_family",sfd->local_intf->logical->preferred_family->rfc_name);
				JSON_SET_SIMPLE("localport","%u",sfd->socket.local.port);
				JSON_SET_SIMPLE_STR("logical_intf",&sfd->local_intf->logical->name);
				JSON_SET_SIMPLE("local_intf_uid","%u",sfd->local_intf->unique_id);
				JSON_SET_SIMPLE("stream","%u",sfd->stream->unique_id);

				json_update_crypto_context(builder, "sfd", sfd->unique_id, &sfd->crypto);

			}
			json_builder_end_object (builder);

		} // --- for

		for (l = c->streams.head; l; l = l->next) {
			ps = l->data;

			mutex_lock(&ps->in_lock);
			mutex_lock(&ps->out_lock);

			snprintf(tmp, sizeof(tmp), "stream-%u", ps->unique_id);
			json_builder_set_member_name(builder, tmp);

			json_builder_begin_object (builder);

			{
				JSON_SET_SIMPLE("media","%u",ps->media->unique_id);
				JSON_SET_SIMPLE("sfd","%u",ps->selected_sfd ? ps->selected_sfd->unique_id : -1);
				JSON_SET_SIMPLE("rtp_sink","%u",ps->rtp_sink ? ps->rtp_sink->unique_id : -1);
				JSON_SET_SIMPLE("rtcp_sink","%u",ps->rtcp_sink ? ps->rtcp_sink->unique_id : -1);
				JSON_SET_SIMPLE("rtcp_sibling","%u",ps->rtcp_sibling ? ps->rtcp_sibling->unique_id : -1);
				JSON_SET_SIMPLE("last_packet",UINT64F,atomic64_get(&ps->last_packet));
				JSON_SET_SIMPLE("ps_flags","%u",ps->ps_flags);
				JSON_SET_SIMPLE("component","%u",ps->component);
				JSON_SET_SIMPLE_CSTR("endpoint",endpoint_print_buf(&ps->endpoint));
				JSON_SET_SIMPLE_CSTR("advertised_endpoint",endpoint_print_buf(&ps->advertised_endpoint));
				JSON_SET_SIMPLE("stats-packets","%ld",atomic64_get(&ps->stats.packets));
				JSON_SET_SIMPLE("stats-bytes","%ld",atomic64_get(&ps->stats.bytes));
				JSON_SET_SIMPLE("stats-errors","%ld",atomic64_get(&ps->stats.errors));

				json_update_crypto_context(builder, "stream", ps->unique_id, &ps->crypto);

			}

			json_builder_end_object (builder);

			// stream_sfds was here before
			mutex_unlock(&ps->in_lock);
			mutex_unlock(&ps->out_lock);

		} // --- for streams.head


		for (l = c->streams.head; l; l = l->next) {
			ps = l->data;

			mutex_lock(&ps->in_lock);
			mutex_lock(&ps->out_lock);

			snprintf(tmp, sizeof(tmp), "stream_sfds-%u", ps->unique_id);
			json_builder_set_member_name(builder, tmp);
			json_builder_begin_array (builder);
			for (k = ps->sfds.head; k; k = k->next) {
				sfd = k->data;
				JSON_ADD_STRING("%u",sfd->unique_id);
			}
			json_builder_end_array (builder);

			mutex_unlock(&ps->in_lock);
			mutex_unlock(&ps->out_lock);
		}


		for (l = c->monologues.head; l; l = l->next) {
			ml = l->data;

			snprintf(tmp, sizeof(tmp), "tag-%u", ml->unique_id);
			json_builder_set_member_name(builder, tmp);

			json_builder_begin_object (builder);
			{

				JSON_SET_SIMPLE("created","%llu",(long long unsigned) ml->created);
				JSON_SET_SIMPLE("active","%u",ml->active_dialogue ? ml->active_dialogue->unique_id : -1);
				JSON_SET_SIMPLE("deleted","%llu",(long long unsigned) ml->deleted);

				if (ml->tag.s) {
					JSON_SET_SIMPLE_STR("tag",&ml->tag);
				}
				if (ml->viabranch.s) {
					JSON_SET_SIMPLE_STR("via-branch",&ml->viabranch);
				}
			}
			json_builder_end_object (builder);

			// other_tags and medias- was here before

		} // --- for monologues.head

		for (l = c->monologues.head; l; l = l->next) {
			ml = l->data;
			// -- we do it again here since the jsonbuilder is linear straight forward
			k = g_hash_table_get_values(ml->other_tags);
			snprintf(tmp, sizeof(tmp), "other_tags-%u", ml->unique_id);
			json_builder_set_member_name(builder, tmp);
			json_builder_begin_array (builder);
			for (m = k; m; m = m->next) {
				ml2 = m->data;
				JSON_ADD_STRING("%u",ml2->unique_id);
			}
			json_builder_end_array (builder);

			g_list_free(k);

			snprintf(tmp, sizeof(tmp), "medias-%u", ml->unique_id);
			json_builder_set_member_name(builder, tmp);
			json_builder_begin_array (builder);
			for (k = ml->medias.head; k; k = k->next) {
				media = k->data;
				JSON_ADD_STRING("%u",media->unique_id);
			}
			json_builder_end_array (builder);
		}


		for (l = c->medias.head; l; l = l->next) {
			media = l->data;

			snprintf(tmp, sizeof(tmp), "media-%u", media->unique_id);
			json_builder_set_member_name(builder, tmp);

			json_builder_begin_object (builder);
			{
				JSON_SET_SIMPLE("tag","%u",media->monologue->unique_id);
				JSON_SET_SIMPLE("index","%u",media->index);
				JSON_SET_SIMPLE_STR("type",&media->type);
				JSON_SET_SIMPLE_CSTR("protocol",media->protocol ? media->protocol->name : "");
				JSON_SET_SIMPLE_CSTR("desired_family",media->desired_family ? media->desired_family->rfc_name : "");
				JSON_SET_SIMPLE("sdes_in_tag","%u",media->sdes_in.tag);
				JSON_SET_SIMPLE("sdes_out_tag","%u",media->sdes_out.tag);
				JSON_SET_SIMPLE_STR("logical_intf",&media->logical_intf->name);
				JSON_SET_SIMPLE("media_flags","%u",media->media_flags);

				json_update_crypto_params(builder, "media", media->unique_id, "sdes_in",
						&media->sdes_in.params);
				json_update_crypto_params(builder, "media", media->unique_id, "sdes_out",
						&media->sdes_out.params);
				json_update_dtls_fingerprint(builder, "media", media->unique_id, &media->fingerprint);

				// streams and maps- and payload_types- was here before

			}
			json_builder_end_object (builder);

		} // --- for medias.head

		// -- we do it again here since the jsonbuilder is linear straight forward
		// XXX can this be moved into the above json object?
		for (l = c->medias.head; l; l = l->next) {
			media = l->data;

			snprintf(tmp, sizeof(tmp), "streams-%u", media->unique_id);
			json_builder_set_member_name(builder, tmp);
			json_builder_begin_array (builder);
			for (m = media->streams.head; m; m = m->next) {
				ps = m->data;
				JSON_ADD_STRING("%u",ps->unique_id);
			}
			json_builder_end_array (builder);

			snprintf(tmp, sizeof(tmp), "maps-%u", media->unique_id);
			json_builder_set_member_name(builder, tmp);
			json_builder_begin_array (builder);
			for (m = media->endpoint_maps.head; m; m = m->next) {
				ep = m->data;
				JSON_ADD_STRING("%u",ep->unique_id);
			}
			json_builder_end_array (builder);

			k = g_hash_table_get_values(media->rtp_payload_types);
			snprintf(tmp, sizeof(tmp), "payload_types-%u", media->unique_id);
			json_builder_set_member_name(builder, tmp);
			json_builder_begin_array (builder);
			for (m = k; m; m = m->next) {
				pt = m->data;
				JSON_ADD_STRING("%u/" STR_FORMAT "/%u/" STR_FORMAT, 
						pt->payload_type, STR_FMT(&pt->encoding),
						pt->clock_rate, STR_FMT(&pt->encoding_parameters));
			}
			json_builder_end_array (builder);

			g_list_free(k);

		}

		for (l = c->endpoint_maps.head; l; l = l->next) {
			ep = l->data;

			snprintf(tmp, sizeof(tmp), "map-%u", ep->unique_id);
			json_builder_set_member_name(builder, tmp);

			json_builder_begin_object (builder);
			{
				JSON_SET_SIMPLE("wildcard","%i",ep->wildcard);
				JSON_SET_SIMPLE("num_ports","%u",ep->num_ports);
				JSON_SET_SIMPLE_CSTR("intf_preferred_family",ep->logical_intf->preferred_family->rfc_name);
				JSON_SET_SIMPLE_STR("logical_intf",&ep->logical_intf->name);
				JSON_SET_SIMPLE_CSTR("endpoint",endpoint_print_buf(&ep->endpoint));

			}
			json_builder_end_object (builder);

		} // --- for c->endpoint_maps.head

		// -- we do it again here since the jsonbuilder is linear straight forward
		for (l = c->endpoint_maps.head; l; l = l->next) {
			ep = l->data;

			snprintf(tmp, sizeof(tmp), "map_sfds-%u", ep->unique_id);
			json_builder_set_member_name(builder, tmp);
			json_builder_begin_array (builder);
			for (m = ep->intf_sfds.head; m; m = m->next) {
				il = m->data;
				JSON_ADD_STRING("loc-%u",il->local_intf->unique_id);
				for (n = il->list.head; n; n = n->next) {
					sfd = n->data;
					JSON_ADD_STRING("%u",sfd->unique_id);
				}
			}
			json_builder_end_array (builder);
		}
	}
	json_builder_end_object (builder);

	JsonGenerator *gen = json_generator_new ();
	JsonNode * root = json_builder_get_root (builder);
	json_generator_set_root (gen, root);
	char* result = json_generator_to_data (gen, NULL);

	json_node_free (root);
	g_object_unref (gen);
	g_object_unref (builder);

	return result;

}


void redis_update_onekey(struct call *c, struct redis *r) {
	unsigned int redis_expires_s;

	if (!r)
		return;

	mutex_lock(&r->lock);
	if (redis_check_conn(r) == REDIS_STATE_DISCONNECTED) {
		mutex_unlock(&r->lock);
		return ;
	}

	rwlock_lock_r(&c->master_lock);

	redis_expires_s = c->callmaster->conf.redis_expires_secs;

	c->redis_hosted_db = r->db;
	if (redisCommandNR(r->ctx, "SELECT %i", c->redis_hosted_db)) {
		rlog(LOG_ERR, " >>>>>>>>>>>>>>>>> Redis error.");
		goto err;
	}

	char* result = redis_encode_json(c);
	if (!result)
		goto err;

	redis_pipe(r, "SET json-"PB" %s", STR(&c->callid), result);
	redis_pipe(r, "EXPIRE json-"PB" %i", STR(&c->callid), redis_expires_s);

	redis_consume(r);

	if (result)
		free(result);
	mutex_unlock(&r->lock);
	rwlock_unlock_r(&c->master_lock);

	return;
err:

	mutex_unlock(&r->lock);
	rwlock_unlock_r(&c->master_lock);
	if (r->ctx->err)
		rlog(LOG_ERR, "Redis error: %s", r->ctx->errstr);
	redisFree(r->ctx);
	r->ctx = NULL;

}

static void redis_update_recording(struct redis *r, struct call *c) {
	struct recording *rec;

	if (!(rec = c->recording))
		return;

	redis_pipe(r, "HMSET call-"PB" recording_metadata "PB" recording_meta_prefix %s ",
		STR(&c->callid),
		STR(&rec->metadata), rec->meta_prefix);
}


/*
 * Redis data structure:
 *
 * SET: calls %s %s %s ...
 *
 * HASH: call-$callid num_sfds %u num_streams %u num_medias %u num_tags %u num_maps %u
 * 
 * HASH: sfd-$callid-$num stream %u
 * 
 * HASH: stream-$callid-$num media %u sfd %u rtp_sink %u rtcp_sink %u rtcp_sibling %u
 * LIST: stream_sfds-$callid-$num %u %u ...
 * 
 * HASH: tag-$callid-$num
 * LIST: other_tags-$callid-$num %u %u ...
 * LIST: medias-$callid-$num %u %u ...
 * 
 * HASH: media-$callid-$num tag %u
 * LIST: streams-$callid-$num %u %u ...
 * LIST: maps-$callid-$num %u %u ...
 * 
 * HASH: map-$callid-$num
 * LIST: map_sfds-$callid-$num %u %u ...
 */

/* must be called lock-free */
void redis_update(struct call *c, struct redis *r) {
	GList *l, *n, *k, *m;
	struct call_monologue *ml, *ml2;

	struct call_media *media;
	struct packet_stream *ps;
	struct stream_fd *sfd;
	struct intf_list *il;
	struct endpoint_map *ep;
	struct rtp_payload_type *pt;
	unsigned int redis_expires_s;

	if (!r)
		return;

	mutex_lock(&r->lock);
	if (redis_check_conn(r) == REDIS_STATE_DISCONNECTED) {
		mutex_unlock(&r->lock);
		return ;
	}

	rwlock_lock_r(&c->master_lock);

	redis_expires_s = c->callmaster->conf.redis_expires_secs;

	c->redis_hosted_db = r->db;
	if (redisCommandNR(r->ctx, "SELECT %i", c->redis_hosted_db)) {
		rlog(LOG_ERR, " >>>>>>>>>>>>>>>>> Redis error.");
		goto err;
	}

	redis_pipe(r, "DEL notifier-"PB"", STR(&c->callid));
	redis_pipe(r, "SREM calls "PB"", STR(&c->callid));
	redis_pipe(r, "DEL call-"PB"", STR(&c->callid));
	redis_pipe(r, "HMSET call-"PB" created %llu last_signal %llu tos %i deleted %llu "
			"num_sfds %u num_streams %u num_medias %u num_tags %u "
			"num_maps %u "
			"ml_deleted %llu created_from %s created_from_addr %s redis_hosted_db %u",
		STR(&c->callid), (long long unsigned) c->created, (long long unsigned) c->last_signal,
		(int) c->tos, (long long unsigned) c->deleted,
		g_queue_get_length(&c->stream_fds), g_queue_get_length(&c->streams),
		g_queue_get_length(&c->medias), g_queue_get_length(&c->monologues),
		g_queue_get_length(&c->endpoint_maps),
		(long long unsigned) c->ml_deleted,
		c->created_from, sockaddr_print_buf(&c->created_from_addr),
		c->redis_hosted_db);
	/* XXX DTLS cert?? */

	redis_update_recording(r, c);

	redis_pipe(r, "DEL sfd-"PB"-0", STR(&c->callid));

	for (l = c->stream_fds.head; l; l = l->next) {
		sfd = l->data;

		redis_pipe(r, "HMSET sfd-"PB"-%u pref_family %s localport %u logical_intf "PB" "
			"local_intf_uid %u "
			"stream %u",
			STR(&c->callid), sfd->unique_id,
			sfd->local_intf->logical->preferred_family->rfc_name,
			sfd->socket.local.port,
			STR(&sfd->local_intf->logical->name),
			sfd->local_intf->unique_id,
			sfd->stream->unique_id);
		redis_update_crypto_context(r, "sfd", &c->callid, sfd->unique_id, &sfd->crypto);
		/* XXX DTLS?? */
		redis_pipe(r, "EXPIRE sfd-"PB"-%u %u", STR(&c->callid), sfd->unique_id, redis_expires_s);

		redis_pipe(r, "DEL sfd-"PB"-%u", STR(&c->callid), sfd->unique_id + 1);
	}

	redis_pipe(r, "DEL stream-"PB"-0 stream_sfds-"PB"-0", STR(&c->callid), STR(&c->callid));

	for (l = c->streams.head; l; l = l->next) {
		ps = l->data;

		mutex_lock(&ps->in_lock);
		mutex_lock(&ps->out_lock);

		redis_pipe(r, "HMSET stream-"PB"-%u media %u sfd %u rtp_sink %u "
			"rtcp_sink %u rtcp_sibling %u last_packet "UINT64F" "
			"ps_flags %u component %u",
			STR(&c->callid), ps->unique_id,
			ps->media->unique_id,
			ps->selected_sfd ? ps->selected_sfd->unique_id : -1,
			ps->rtp_sink ? ps->rtp_sink->unique_id : -1,
			ps->rtcp_sink ? ps->rtcp_sink->unique_id : -1,
			ps->rtcp_sibling ? ps->rtcp_sibling->unique_id : -1,
			atomic64_get(&ps->last_packet),
			ps->ps_flags,
			ps->component);
		redis_update_endpoint(r, "stream", &c->callid, ps->unique_id, "endpoint", &ps->endpoint);
		redis_update_endpoint(r, "stream", &c->callid, ps->unique_id, "advertised_endpoint",
				&ps->advertised_endpoint);
		redis_update_stats(r, "stream", &c->callid, ps->unique_id, "stats", &ps->stats);
		redis_update_crypto_context(r, "stream", &c->callid, ps->unique_id, &ps->crypto);
		/* XXX DTLS?? */

		for (k = ps->sfds.head; k; k = k->next) {
			sfd = k->data;
			redis_pipe(r, "RPUSH stream_sfds-"PB"-%u %u",
				STR(&c->callid), ps->unique_id,
				sfd->unique_id);
		}

		mutex_unlock(&ps->in_lock);
		mutex_unlock(&ps->out_lock);

		redis_pipe(r, "EXPIRE stream-"PB"-%u %u", STR(&c->callid), ps->unique_id, redis_expires_s);
		redis_pipe(r, "EXPIRE stream_sfds-"PB"-%u %u", STR(&c->callid), ps->unique_id, redis_expires_s);

		redis_pipe(r, "DEL stream-"PB"-%u stream_sfds-"PB"-%u",
				STR(&c->callid), ps->unique_id + 1,
				STR(&c->callid), ps->unique_id + 1);
	}

	redis_pipe(r, "DEL tag-"PB"-0 other_tags-"PB"-0 medias-"PB"-0",
			STR(&c->callid), STR(&c->callid), STR(&c->callid));

	for (l = c->monologues.head; l; l = l->next) {
		ml = l->data;

		redis_pipe(r, "HMSET tag-"PB"-%u created %llu active %u deleted %llu",
			STR(&c->callid), ml->unique_id,
			(long long unsigned) ml->created,
			ml->active_dialogue ? ml->active_dialogue->unique_id : -1,
			(long long unsigned) ml->deleted);
		if (ml->tag.s)
			redis_pipe(r, "HMSET tag-"PB"-%u tag "PB"",
				STR(&c->callid), ml->unique_id,
				STR(&ml->tag));
		if (ml->viabranch.s)
			redis_pipe(r, "HMSET tag-"PB"-%u via-branch "PB"",
				STR(&c->callid), ml->unique_id,
				STR(&ml->viabranch));

		k = g_hash_table_get_values(ml->other_tags);
		for (m = k; m; m = m->next) {
			ml2 = m->data;
			redis_pipe(r, "RPUSH other_tags-"PB"-%u %u",
				STR(&c->callid), ml->unique_id,
				ml2->unique_id);
		}
		g_list_free(k);

		for (k = ml->medias.head; k; k = k->next) {
			media = k->data;
			redis_pipe(r, "RPUSH medias-"PB"-%u %u",
				STR(&c->callid), ml->unique_id,
				media->unique_id);
		}

		redis_pipe(r, "EXPIRE tag-"PB"-%u %u", STR(&c->callid), ml->unique_id, redis_expires_s);
		redis_pipe(r, "EXPIRE other_tags-"PB"-%u %u", STR(&c->callid), ml->unique_id, redis_expires_s);
		redis_pipe(r, "EXPIRE medias-"PB"-%u %u", STR(&c->callid), ml->unique_id, redis_expires_s);

		redis_pipe(r, "DEL tag-"PB"-%u other_tags-"PB"-%u medias-"PB"-%u",
				STR(&c->callid), ml->unique_id + 1,
				STR(&c->callid), ml->unique_id + 1,
				STR(&c->callid), ml->unique_id + 1);
	}

	redis_pipe(r, "DEL media-"PB"-0 streams-"PB"-0 maps-"PB"-0 payload_types-"PB"-0",
			STR(&c->callid), STR(&c->callid), STR(&c->callid), STR(&c->callid));

	for (l = c->medias.head; l; l = l->next) {
		media = l->data;

		redis_pipe(r, "HMSET media-"PB"-%u "
			"tag %u "
			"index %u "
			"type "PB" protocol %s desired_family %s "
			"sdes_in_tag %u sdes_out_tag %u logical_intf "PB" "
			"media_flags %u",
			STR(&c->callid), media->unique_id,
			media->monologue->unique_id,
			media->index,
			STR(&media->type), media->protocol ? media->protocol->name : "",
			media->desired_family ? media->desired_family->rfc_name : "",
			media->sdes_in.tag, media->sdes_out.tag,
			STR(&media->logical_intf->name),
			media->media_flags);
		redis_update_crypto_params(r, "media", &c->callid, media->unique_id, "sdes_in",
				&media->sdes_in.params);
		redis_update_crypto_params(r, "media", &c->callid, media->unique_id, "sdes_out",
				&media->sdes_out.params);
		redis_update_dtls_fingerprint(r, "media", &c->callid, media->unique_id, &media->fingerprint);

		for (m = media->streams.head; m; m = m->next) {
			ps = m->data;
			redis_pipe(r, "RPUSH streams-"PB"-%u %u",
				STR(&c->callid), media->unique_id,
				ps->unique_id);
		}

		for (m = media->endpoint_maps.head; m; m = m->next) {
			ep = m->data;
			redis_pipe(r, "RPUSH maps-"PB"-%u %u",
				STR(&c->callid), media->unique_id,
				ep->unique_id);
		}

		k = g_hash_table_get_values(media->rtp_payload_types);
		for (m = k; m; m = m->next) {
			pt = m->data;
			redis_pipe(r, "RPUSH payload_types-"PB"-%u %u/"PB"/%u/"PB"",
				STR(&c->callid), media->unique_id,
				pt->payload_type, STR(&pt->encoding),
				pt->clock_rate, STR(&pt->encoding_parameters));
		}
		g_list_free(k);

		redis_pipe(r, "EXPIRE media-"PB"-%u %u", STR(&c->callid), media->unique_id, redis_expires_s);
		redis_pipe(r, "EXPIRE streams-"PB"-%u %u", STR(&c->callid), media->unique_id, redis_expires_s);
		redis_pipe(r, "EXPIRE maps-"PB"-%u %u", STR(&c->callid), media->unique_id, redis_expires_s);
		redis_pipe(r, "EXPIRE payload_types-"PB"-%u %u", STR(&c->callid), media->unique_id, redis_expires_s);

		redis_pipe(r, "DEL media-"PB"-%u streams-"PB"-%u maps-"PB"-%u payload_types-"PB"-%u",
				STR(&c->callid), media->unique_id + 1,
				STR(&c->callid), media->unique_id + 1,
				STR(&c->callid), media->unique_id + 1,
				STR(&c->callid), media->unique_id + 1);
	}

	redis_pipe(r, "DEL map-"PB"-0 map_sfds-"PB"-0",
			STR(&c->callid), STR(&c->callid));

	for (l = c->endpoint_maps.head; l; l = l->next) {
		ep = l->data;

		redis_pipe(r, "HMSET map-"PB"-%u wildcard %i num_ports %u intf_preferred_family %s "
			"logical_intf "PB"",
			STR(&c->callid), ep->unique_id,
			ep->wildcard,
			ep->num_ports,
			ep->logical_intf->preferred_family->rfc_name,
			STR(&ep->logical_intf->name));
		redis_update_endpoint(r, "map", &c->callid, ep->unique_id, "endpoint", &ep->endpoint);

		for (m = ep->intf_sfds.head; m; m = m->next) {
			il = m->data;

			redis_pipe(r, "RPUSH map_sfds-"PB"-%u loc-%u",
				STR(&c->callid), ep->unique_id,
				il->local_intf->unique_id);

			for (n = il->list.head; n; n = n->next) {
				sfd = n->data;

				redis_pipe(r, "RPUSH map_sfds-"PB"-%u %u",
					STR(&c->callid), ep->unique_id,
					sfd->unique_id);
			}

		}

		redis_pipe(r, "EXPIRE map-"PB"-%u %u", STR(&c->callid), ep->unique_id, redis_expires_s);
		redis_pipe(r, "EXPIRE map_sfds-"PB"-%u %u", STR(&c->callid), ep->unique_id, redis_expires_s);

		redis_pipe(r, "DEL map-"PB"-%u map_sfds-"PB"-%u",
				STR(&c->callid), ep->unique_id + 1,
				STR(&c->callid), ep->unique_id + 1);
	}

	redis_pipe(r, "EXPIRE call-"PB" %u", STR(&c->callid), redis_expires_s);
	redis_pipe(r, "SADD calls "PB"", STR(&c->callid));
	redis_pipe(r, "SADD notifier-"PB" "PB"", STR(&c->callid), STR(&c->callid));
	redis_pipe(r, "EXPIRE notifier-"PB" %u", STR(&c->callid), redis_expires_s);

	redis_consume(r);

	mutex_unlock(&r->lock);
	rwlock_unlock_r(&c->master_lock);

	return;
err:

	mutex_unlock(&r->lock);
	rwlock_unlock_r(&c->master_lock);
	if (r->ctx->err)
		rlog(LOG_ERR, "Redis error: %s", r->ctx->errstr);
	redisFree(r->ctx);
	r->ctx = NULL;
}

/* must be called lock-free */
void redis_delete(struct call *c, struct redis *r) {
	if (!r)
		return;

	mutex_lock(&r->lock);
	if (redis_check_conn(r) == REDIS_STATE_DISCONNECTED) {
		mutex_unlock(&r->lock);
		return ;
	}
	rwlock_lock_r(&c->master_lock);

	if (redisCommandNR(r->ctx, "SELECT %i", c->redis_hosted_db))
		goto err;

	if (c->callmaster->conf.redis_multikey) {
		redis_delete_call(c, r);
	} else {
		redis_delete_call_json(c, r);
	}

	rwlock_unlock_r(&c->master_lock);
	mutex_unlock(&r->lock);
	return;

err:
	rwlock_unlock_r(&c->master_lock);
	mutex_unlock(&r->lock);

	if (r->ctx->err)
		rlog(LOG_ERR, "Redis error: %s", r->ctx->errstr);
	redisFree(r->ctx);
	r->ctx = NULL;
}





void redis_wipe(struct redis *r) {
	if (!r)
		return;

	mutex_lock(&r->lock);
	if (redis_check_conn(r) == REDIS_STATE_DISCONNECTED) {
		mutex_unlock(&r->lock);
		return ;
	}
	redisCommandNR(r->ctx, "DEL calls");
	mutex_unlock(&r->lock);
}
