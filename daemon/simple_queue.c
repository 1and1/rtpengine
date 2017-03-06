#include "simple_queue.h"
#include "aux.h"

inline int sq_alloc(SQueue *q, int size) {
	if (!q)
		return -1;

	q->intArray = (int*)malloc(size * sizeof(int));
	q->front = 0;
	q->rear = -1;
	q->itemCount = 0;
	q->fullSize = size;
	return 0;
}

inline int sq_shuffle(SQueue *q, int port_min, int port_max) {
    int i;

    if (!q)
        return -1;

    if (port_max - port_min + 1 > q->fullSize)
        return -1;

    srand(time(0));
    for (i=0; i<port_max - port_min + 1; i++) {
        q->intArray[i] = port_min + (rand() % (i+1));
    }

    q->front = 0;
    q->rear = q->fullSize-1;
    q->itemCount = q->fullSize;
}

inline int sq_clear(SQueue *q) {

    if (!q)
        return -1;

    q->front = q->itemCount = 0;
    q->rear = -1;
    memset(q->intArray, 0, q->fullSize * sizeof(int));
    return 0;
}

inline void sq_free(SQueue *q, int size) {
	if (!q)
		return;

	free(q->intArray);
}

inline int sq_peek(SQueue *q) {
   return q->intArray[q->front];
}

inline int isEmpty(SQueue *q) {
   return q->itemCount == 0;
}

inline int sq_is_full(SQueue *q) {
   return q->itemCount == q->fullSize;
}

inline int sq_size(SQueue *q) {
   return q->itemCount;
}

inline void sq_push_tail(SQueue *q, int data) {
   if(!sq_is_full(q)) {

      if(q->rear == q->fullSize-1) {
         q->rear = -1;
      }

      q->intArray[++q->rear] = data;
      q->itemCount++;
   }
}

inline int sq_pop_head(SQueue *q) {
   int data = -1;

   if(!isEmpty(q)) {
       data = q->intArray[q->front++];

       if(q->front == q->fullSize) {
          q->front = 0;
       }

       q->itemCount--;
   }
   return data;

}

/* rebuild queue from bit array */
/* shuffling algo is not yet implemented */
/* src_ba: 1 means port is in use */
//TBD check volatile
void sq_rebuild_from_ba(SQueue *dst_q, volatile unsigned int *src_ba, unsigned int start_port, unsigned int stop_port) {
    unsigned int port;
    if (NULL == dst_q)
        return;


    sq_clear(dst_q);

    for (port=start_port; port<=stop_port; port++) {
        if (!bit_array_isset(src_ba, port)) {
            sq_push_tail(dst_q, port);
        }

    }
}
