#include <stdio.h>
#include <hiredis.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "redis.h"
#include "aux.h"
#include "call.h"
#include "log.h"





#define redisCommandNR(a...) (int)({ void *__tmp; __tmp = redisCommand(a); if (__tmp) freeReplyObject(__tmp); __tmp ? 0 : -1;})





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




static void redis_consume(struct redis *r, int count) {
	redisReply *rp;

	while (count-- > 0) {
		redisGetReply(r->ctx, (void **) &rp);
		freeReplyObject(rp);
	}
}




static int redis_connect(struct redis *r, int wait) {
	struct timeval tv;
	redisReply *rp;
	char *s;

	if (r->ctx)
		redisFree(r->ctx);
	r->ctx = NULL;

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	r->ctx = redisConnectWithTimeout(r->host, r->port, tv);

	if (!r->ctx)
		goto err;
	if (r->ctx->err)
		goto err2;

	if (redisCommandNR(r->ctx, "PING"))
		goto err2;

	if (redisCommandNR(r->ctx, "SELECT %i", r->db))
		goto err2;

	while (wait-- >= 0) {
		mylog(LOG_INFO, "Asking Redis whether it's master or slave...\n");
		rp = redisCommand(r->ctx, "INFO");
		if (!rp)
			goto err2;

		s = strstr(rp->str, "role:");
		if (!s)
			goto err3;
		if (!memcmp(s, "role:master", 9))
			goto done;
		else if (!memcmp(s, "role:slave", 8))
			goto next;
		else
			goto err3;

next:
		freeReplyObject(rp);
		mylog(LOG_INFO, "Connected to Redis, but it's in slave mode\n");
		sleep(1);
	}

	goto err2;

done:
	freeReplyObject(rp);
	mylog(LOG_INFO, "Connected to Redis\n");
	return 0;

err3:
	freeReplyObject(rp);
err2:
	if (r->ctx->err)
		mylog(LOG_ERR, "Redis error: %s\n", r->ctx->errstr);
	redisFree(r->ctx);
	r->ctx = NULL;
err:
	mylog(LOG_ERR, "Failed to connect to master Redis database\n");
	return -1;
}




struct redis *redis_new(u_int32_t ip, u_int16_t port, int db) {
	struct redis *r;

	r = malloc(sizeof(*r));
	ZERO(*r);

	sprintf(r->host, IPF, IPP(ip));
	r->port = port;
	r->db = db;

	if (redis_connect(r, 10))
		goto err;

	return r;

err:
	free(r);
	return NULL;
}




static void redis_delete_uuid(char *uuid, struct callmaster *m) {
	struct redis *r = m->redis;
	redisReply *rp, *rp2;
	int i, count = 0;

	if (!r)
		return;

	rp = redisCommand(r->ctx, "LRANGE %s-streams 0 -1", uuid);
	if (!rp)
		return;
	if (rp->type != REDIS_REPLY_ARRAY) {
		freeReplyObject(rp);
		return;
	}

	for (i = 0; i < rp->elements; i++) {
		rp2 = rp->element[i];
		if (rp2->type != REDIS_REPLY_STRING)
			continue;

		redisAppendCommand(r->ctx, "DEL %s:0 %s:1", rp2->str, rp2->str);
		count++;
	}

	redisAppendCommand(r->ctx, "DEL %s-streams %s", uuid, uuid);
	redisAppendCommand(r->ctx, "SREM calls %s", uuid);
	count += 2;

	redis_consume(r, count);
	freeReplyObject(rp);
}




int redis_restore(struct callmaster *m) {
	struct redis *r = m->redis;
	redisReply *rp, *rp2, *rp3, *rp4, *rp5, *rp6;
	GQueue q = G_QUEUE_INIT;
	int i, j, k, l;

	rp = redisCommand(r->ctx, "SMEMBERS calls");
	if (!rp || rp->type != REDIS_REPLY_ARRAY) {
		mylog(LOG_ERR, "Could not retrieve call list from Redis: %s\n", r->ctx->errstr);
		if (rp)
			freeReplyObject(rp);
		goto err;
	}

	for (i = 0; i < rp->elements; i++) {
		rp2 = rp->element[i];
		if (rp2->type != REDIS_REPLY_STRING)
			continue;

		rp3 = redisCommand(r->ctx, "HMGET %s callid created", rp2->str);

		if (!rp3)
			goto del;
		if (rp3->type != REDIS_REPLY_ARRAY)
			goto del2;
		if (rp3->elements != 2)
			goto del2;
		for (j = 0; j < rp3->elements; j++) {
			if (rp3->element[j]->type != REDIS_REPLY_STRING)
				goto del2;
		}

		rp4 = redisCommand(r->ctx, "LRANGE %s-streams 0 -1", rp2->str);
		if (!rp4)
			goto del2;
		if (rp4->type != REDIS_REPLY_ARRAY)
			goto del3;

		for (j = 0; j < rp4->elements; j++) {
			rp5 = rp4->element[j];
			if (rp5->type != REDIS_REPLY_STRING)
				continue;
			for (k = 0; k < 2; k++) {
				rp6 = redisCommand(r->ctx, "HMGET %s:%i ip port localport kernel filled confirmed tag", rp5->str, k);
				if (!rp6)
					goto del4;
				if (rp6->type != REDIS_REPLY_ARRAY)
					goto del5;
				if (rp6->elements != 7)
					goto del5;
				for (l = 0; l < rp6->elements; l++) {
					if (rp6->element[l]->type != REDIS_REPLY_STRING)
						goto del5;
				}
				g_queue_push_tail(&q, rp6);
			}
		}

		call_restore(m, rp2->str, rp3->element, q.head);

		if (q.head)
			g_list_foreach(q.head, (GFunc) freeReplyObject, NULL);
		g_queue_clear(&q);
		freeReplyObject(rp4);
		freeReplyObject(rp3);

		continue;

del5:
		freeReplyObject(rp6);
del4:
		if (q.head)
			g_list_foreach(q.head, (GFunc) freeReplyObject, NULL);
		g_queue_clear(&q);
del3:
		freeReplyObject(rp4);
del2:
		freeReplyObject(rp3);
del:
		mylog(LOG_WARNING, "Could not restore call with GUID %s from Redis DB due to incomplete data\n", rp2->str);
		redis_delete_uuid(rp2->str, m);
	}

	freeReplyObject(rp);

	return 0;

err:
	return -1;
}




void redis_update(struct call *c) {
	struct callmaster *cm = c->callmaster;
	struct redis *r = cm->redis;
	char uuid[37];
	GList *l;
	struct callstream *cs;
	int i, count = 0;
	struct peer *p;
	redisReply *oldstreams;

	if (!r)
		return;

	if (!c->redis_uuid[0])
		uuid_str_generate(c->redis_uuid);

	redis_check_type(r, c->redis_uuid, NULL, "hash");
	oldstreams = redisCommand(r->ctx, "LRANGE %s-streams 0 -1", c->redis_uuid);

	redisAppendCommand(r->ctx, "HMSET %s callid %s created %i", c->redis_uuid, c->callid, c->created);
	redisAppendCommand(r->ctx, "DEL %s-streams-temp", c->redis_uuid);
	count += 2;

	for (l = c->callstreams->head; l; l = l->next) {
		cs = l->data;
		uuid_str_generate(uuid);

		for (i = 0; i < 2; i++) {
			p = &cs->peers[i];

			redisAppendCommand(r->ctx, "DEL %s:%i", uuid, i);
			redisAppendCommand(r->ctx, "HMSET %s:%i ip " IPF " port %i localport %i kernel %i filled %i confirmed %i tag %s", uuid, i, IPP(p->rtps[0].peer.ip), p->rtps[0].peer.port, p->rtps[0].localport, p->kernelized, p->filled, p->confirmed, p->tag);
			redisAppendCommand(r->ctx, "EXPIRE %s:%i 86400", uuid, i);
			count += 3;
		}

		redisAppendCommand(r->ctx, "RPUSH %s-streams-temp %s", c->redis_uuid, uuid);
		count++;
	}

	redisAppendCommand(r->ctx, "RENAME %s-streams-temp %s-streams", c->redis_uuid, c->redis_uuid);
	redisAppendCommand(r->ctx, "EXPIRE %s-streams 86400", c->redis_uuid);
	redisAppendCommand(r->ctx, "EXPIRE %s 86400", c->redis_uuid);
	redisAppendCommand(r->ctx, "SADD calls %s", c->redis_uuid);
	count += 4;

	if (oldstreams) {
		if (oldstreams->type == REDIS_REPLY_ARRAY) {
			for (i = 0; i < oldstreams->elements; i++) {
				if (oldstreams->element[0]->type == REDIS_REPLY_STRING) {
					redisAppendCommand(r->ctx, "DEL %s:0 %s:1", oldstreams->element[0]->str, oldstreams->element[0]->str);
					count++;
				}
			}
		}
		freeReplyObject(oldstreams);
	}

	redis_consume(r, count);
}





void redis_delete(struct call *c) {
	redis_delete_uuid(c->redis_uuid, c->callmaster);
}