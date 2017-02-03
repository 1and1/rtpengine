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

int qAlloc(SQueue *q, int size);
int qFree(SQueue *q, int size);
int peek(SQueue *q);
int isEmpty(SQueue *q);
int isFull(SQueue *q);
int size(SQueue *q);
void insert(SQueue *q, int data) ;
int removeData(SQueue *q) ;

#endif
