#include "simple_queue.h"
#include "aux.h"

INLINE int sq_alloc(simple_queue_t *q, int capacity) {
	if (!q)
		return -1;
	q->array = (int*) malloc(capacity * sizeof(int));
	q->front = 0;
	q->rear = capacity-1;
	q->item_count = 0;
	q->capacity = capacity;
	return 0;
}

INLINE int sq_init(simple_queue_t *q, int port_min, int port_max) {
	int i;

	if (!q)
		return -1;
	if (port_max - port_min + 1 > q->capacity)
		return -1;

	for (i = 0; i <= port_max - port_min; i++) {
		q->array[i] = port_min + i;
	}
	q->front = 0;
	q->rear = port_max - port_min;
	q->item_count = port_max - port_min + 1;
	return 0;
}

INLINE int sq_shuffle(simple_queue_t *q) {
	int i,pos, tmp;

	if (!q)
		return -1;
	srand(time(0));
	/* To shuffle an array a of n elements (indices 0..n-1) */
	for (i = q->rear; i >= 1; i--) {
		pos = rand() % (i + 1);
		tmp = q->array[pos];
		q->array[pos] = q->array[i];
		q->array[i] = tmp;
	}
}

INLINE int sq_clear(simple_queue_t *q) {
	if (!q)
		return -1;
	q->front = q->item_count = 0;
	q->rear = -1;
	memset(q->array, 0, q->capacity * sizeof(int));
	return 0;
}

INLINE void sq_free(simple_queue_t *q, int size) {
	if (q) free(q->array);
}

INLINE int sq_peek(simple_queue_t *q) {
	return q->array[q->front];
}

INLINE int is_empty(simple_queue_t *q) {
	return q->item_count == 0;
}

INLINE int sq_is_full(simple_queue_t *q) {
	return q->item_count == q->capacity;
}

INLINE int sq_size(simple_queue_t *q) {
	return q->item_count;
}

INLINE void sq_push_tail(simple_queue_t *q, int data) {
	if (!sq_is_full(q)) {
		q->rear = (q->rear == q->capacity-1) ? 0 : q->rear+1;
		q->array[q->rear] = data;
		q->item_count++;
	}
}

INLINE int sq_pop_head(simple_queue_t *q) {
	int data = -1;
	if (!is_empty(q)) {
		data = q->array[q->front];
		q->front = (q->front == q->capacity-1) ? 0 : q->front+1;
		q->item_count--;
	}
	return data;
}

/* rebuild queue from bit array
 * src_ba: 1 means port is in use */
//TBD check volatile
void sq_rebuild_from_ba(simple_queue_t *dst_q, volatile unsigned int *src_ba,
		unsigned int start_port, unsigned int stop_port) {
	unsigned int port;

	if (NULL == dst_q)
		return;
	sq_clear(dst_q);
	for (port = start_port; port <= stop_port; port++) {
		if (!bit_array_isset(src_ba, port)) {
			sq_push_tail(dst_q, port);
		}
	}
	sq_shuffle(dst_q);
}
