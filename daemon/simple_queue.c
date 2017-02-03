#include "simple_queue.h"


inline int qAlloc(SQueue *q, int size) {
	if (!q)
		return -1;

	q->intArray = (int*)malloc(size * sizeof(int));
	q->front = 0;
	q->rear = -1;
	q->itemCount = 0;
	q->fullSize = size;
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
