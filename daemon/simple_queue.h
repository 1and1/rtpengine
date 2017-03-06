#ifndef _SIMPLE_QUEUE_H_
#define _SIMPLE_QUEUE_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct SQueue {
	int *intArray;
	int fullSize;
	int front;
	int rear;
	int itemCount;
} SQueue;

int sq_alloc(SQueue *q, int size);
int sq_shuffle(SQueue *q, int port_min, int port_max);
void sq_free(SQueue *q, int size);
int sq_peek(SQueue *q);
int sq_is_empty(SQueue *q);
int sq_is_full(SQueue *q);
int sq_size(SQueue *q);
void sq_push_tail(SQueue *q, int data);
int sq_pop_head(SQueue *q);
int sq_clear(SQueue *q);
void sq_rebuild_from_ba(SQueue *dst_q, volatile unsigned int *src_ba,
	unsigned int start_port, unsigned int stop_port);

#endif
