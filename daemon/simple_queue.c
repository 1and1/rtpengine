
#include "simple_queue.h"
#include "aux.h"

inline int sq_alloc(simple_queue_t *q, int capacity) {
	if (!q)
		return -1;
	q->array = (int*) malloc(capacity * sizeof(int));
	q->front = 0;
	q->rear = capacity-1;
	q->item_count = 0;
	q->capacity = capacity;
	return 0;
}

inline int sq_init(simple_queue_t *q, int port_min, int port_max) {
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

inline int sq_shuffle(simple_queue_t *q) {
	int i, pos1, pos2, tmp;

	if (!q)
		return -1;
	srand(time(0));
	for (i = q->rear/2; i >= 1; i--) {
		pos1 = 2*(rand() % (i + 1));
		pos2 = i*2;
		tmp = q->array[pos1];
		q->array[pos1] = q->array[pos2];
		q->array[pos2] = tmp;

		pos1++; pos2++;
		tmp = q->array[pos1];
		q->array[pos1] = q->array[pos2];
		q->array[pos2] = tmp;
	}
}

inline int sq_clear(simple_queue_t *q) {
	if (!q)
		return -1;
	q->front = q->item_count = 0;
	q->rear = -1;
	memset(q->array, 0, q->capacity * sizeof(int));
	return 0;
}

inline void sq_free(simple_queue_t *q, int size) {
	if (q) free(q->array);
}

inline int sq_peek(simple_queue_t *q) {
	return q->array[q->front];
}

inline int is_empty(simple_queue_t *q) {
	return q->item_count == 0;
}

inline int sq_is_full(simple_queue_t *q) {
	return q->item_count == q->capacity;
}

inline int sq_size(simple_queue_t *q) {
	return q->item_count;
}

inline void sq_push_tail(simple_queue_t *q, int data) {
	if (!sq_is_full(q)) {
		q->rear = (q->rear == q->capacity-1) ? 0 : q->rear+1;
		q->array[q->rear] = data;
		q->item_count++;
	}
}

inline int sq_pop_head(simple_queue_t *q) {
	int data = -1;
	if (!is_empty(q)) {
		data = q->array[q->front];
		q->front = (q->front == q->capacity-1) ? 0 : q->front+1;
		q->item_count--;
	}
	return data;
}

/* rebuild queue from bit array
 * src_ba: 1 means port is in use
 * because of the way ports are allocated, we want to have odd number of ports
 * available, and as such we look for even start port, odd stop port */

void sq_rebuild_from_ba(simple_queue_t *dst_q, volatile unsigned int *src_ba,
		unsigned int start_port, unsigned int stop_port) {
	unsigned int port, even_start_port, odd_stop_port;

	even_start_port = start_port;
	odd_stop_port = stop_port;

	if (start_port % 2 == 1)
		even_start_port = start_port + 1;
	if (stop_port % 2 == 0)
		odd_stop_port = stop_port - 1;

	/* TODO: be more verbose around checks here */
	if ((NULL == dst_q) || (even_start_port > odd_stop_port))
		return;

	sq_clear(dst_q);
	for (port = even_start_port; port <= odd_stop_port; port++) {
		if (!bit_array_isset(src_ba, port)) {
			sq_push_tail(dst_q, port);
		}
	}
	sq_shuffle(dst_q);
}
