#ifndef _LISTARRAY_QUEUE_H_
#define _LISTARRAY_QUEUE_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct node {
   int data;
   struct node *next;
   struct node *prev;
} node;

typedef struct LAQueue {
    struct node *head;
    struct node *last;
    struct node *current;

    node *nodeArray;
    int fullSize;
    int itemCount;
} LAQueue;

//is list empty
int laqIsEmpty(LAQueue *q);

node* findElem(LAQueue *q, int data) ;

/* to be called as qAlloc(LAQueue *q, MAX_PORTS) */
int laqAlloc(LAQueue *q, int size);

int laqFree(LAQueue *q);

int laqShuffle(LAQueue *q, int port_min, int port_max);

int laqIsFull(LAQueue *q);

int laqSize(LAQueue *q);

struct node* getmem(LAQueue *q, int data);

//insert link at the last location
void laqEnqueue(LAQueue *q, int data);

//delete first item
struct node* laqDequeue(LAQueue *q);

//delete link at the last location
struct node* laqDeleteLast(LAQueue *q);

//delete a link with given data
struct node* laqDelete(LAQueue *q, int data);

#endif
