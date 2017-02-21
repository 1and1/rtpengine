#include "simple_queue.h"
#include "aux.h"

inline int qAlloc(SQueue *q, int size) {
	if (!q)
		return -1;

	q->intArray = (int*)malloc(size * sizeof(int));
	q->front = 0;
	q->rear = -1;
	q->itemCount = 0;
	q->fullSize = size;
}

inline int qShuffle(SQueue *q, int port_min, int port_max) {
    int i;

    if (!q)
        return -1;

    for (i=0; i<q->fullSize; i++) {
        q->intArray[i] = port_min + i;
    }

    q->front = 0;
    q->rear = q->fullSize-1;
    q->itemCount = q->fullSize;

}

inline int qClear(SQueue *q) {
    int i;

    if (!q)
        return -1;

    q->front = q->rear = q->itemCount = 0;
    memset(q->intArray, 0, q->fullSize * sizeof(int));
}

inline  int qFree(SQueue *q, int size) {
	if (!q)
		return -1;

	free(q->intArray);
}

inline int peek(SQueue *q) {
   return q->intArray[q->front];
}

inline int isEmpty(SQueue *q) {
   return q->itemCount == 0;
}

inline int isFull(SQueue *q) {
   return q->itemCount == q->fullSize;
}

inline int size(SQueue *q) {
   return q->itemCount;
}

inline void insert(SQueue *q, int data) {

   if(!isFull(q)) {

      if(q->rear == q->fullSize-1) {
         q->rear = -1;
      }

      q->intArray[++q->rear] = data;
      q->itemCount++;
   }
}

inline int removeData(SQueue *q) {
   int data = q->intArray[q->front++];

   if(q->front == q->fullSize) {
      q->front = 0;
   }

   q->itemCount--;
   return data;
}

/* rebuild queue from bit array */
/* shuffling algo is not yet implemented */
/* src_ba: 1 means port is in use */
//TBD check volatile
void rebuild_queue(SQueue *dst_q, volatile unsigned int *src_ba, unsigned int start_port, unsigned int stop_port) {
    unsigned int port;
    if (NULL == dst_q)
        return;

    qClear(dst_q);

    for (port=start_port; port<=stop_port; port++) {
        if (!bit_array_isset(src_ba, port)) {
            insert(dst_q, port);
        }
    }
}
