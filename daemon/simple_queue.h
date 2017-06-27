#ifndef _SIMPLE_QUEUE_H_
#define _SIMPLE_QUEUE_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct simple_queue {
	int *array;
	int capacity;
	int front;
	int rear;
	int item_count;
} simple_queue_t;

int  sq_alloc(simple_queue_t *q, int size);
int  sq_init(simple_queue_t *q, int port_min, int port_max);
int  sq_shuffle(simple_queue_t *q);
int  sq_peek(simple_queue_t *q);
int  sq_is_empty(simple_queue_t *q);
int  sq_is_full(simple_queue_t *q);
int  sq_size(simple_queue_t *q);
int  sq_pop_head(simple_queue_t *q);
int  sq_clear(simple_queue_t *q);
void sq_push_tail(simple_queue_t *q, int data);
void sq_free(simple_queue_t *q, int size);
void sq_rebuild_from_ba(simple_queue_t *dst_q, volatile unsigned int *src_ba,
						unsigned int start_port, unsigned int stop_port);

#endif
