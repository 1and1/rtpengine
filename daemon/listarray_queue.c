#include <stdio.h>
#include "listarray_queue.h"


//is list empty
inline int laqIsEmpty(LAQueue *q) {
   return (0 == q->itemCount);
}

node* findElem(LAQueue *q, int data) {
    node* pElem = getmem(q, data);
    //pElem->next == NULL && pElem->prev == NULL => elem is not part of queue
    if (pElem && !pElem->next && !pElem->prev)
        return NULL;
    return pElem;
}

/* to be called as qAlloc(LAQueue *q, MAX_PORTS) */
int laqAlloc(LAQueue *q, int size) {
    if (!q)
        return -1;

    q->nodeArray = (node*)malloc(size * sizeof(node));
    q->itemCount = 0;
    q->fullSize = size;
}

int laqFree(LAQueue *q) {
    if (!q)
        return -1;

    free(q->nodeArray);
}

int laqShuffle(LAQueue *q, int port_min, int port_max) {
    int i;

    if (!q)
        return -1;

    for (i=port_min; i<=port_max; i++) {
        //q->nodeArray[i].data = i;
        laqEnqueue(q, i);
    }
}

int laqIsFull(LAQueue *q) {
   return q->itemCount == q->fullSize;
}

int laqSize(LAQueue *q) {
   return q->itemCount;
}

struct node* getmem(LAQueue *q, int data) {
    return q->nodeArray+data;
}

// this is push
//insert link at the last location
void laqEnqueue(LAQueue *q, int data) {

   //create a link
   struct node *link = (struct node*) getmem(q, data);
   link->data = data;

   if(laqIsEmpty(q)) {
      //make it the last link
      q->head = link;
      q->last = link;
   } else {
      //make link a new last link
      q->last->next = link;

      //mark old last node as prev of new link
      link->prev = q->last;
   }

   //point last to new last node
   q->last = link;
   q->itemCount++;
}

//delete first item
struct node* laqDequeue(LAQueue *q) {
   //save reference to first link
   struct node *tempLink = q->head;

   if (NULL == q->head) {
       fprintf(stderr, "laqDequeue(): empty queue");
       return NULL;
   }

   tempLink->next = tempLink->prev = NULL;

   //if only one link
   if(q->head->next == NULL){
      q->last = NULL;
   } else {
      q->head->next->prev = NULL;
   }

   q->head = q->head->next;
   q->itemCount--;
   //return the deleted link
   return tempLink;
}

//delete link at the last location
struct node* laqDeleteLast(LAQueue *q) {
   //save reference to last link
   struct node *tempLink = q->last;

   //if only one link
   if(q->head->next == NULL) {
      q->head = NULL;
   } else {
      q->last->prev->next = NULL;
   }

   q->last = q->last->prev;
   q->itemCount--;
   //return the deleted link
   return tempLink;
}

//delete a link with given data
struct node* laqDelete(LAQueue *q, int data) {

   struct node* current = findElem(q, data);
   if (NULL == current) return NULL;

   //found a match, update the link
   if(current == q->head) {
      //change first to point to next link
      q->head = q->head->next;
   } else {
      //bypass the current link
      current->prev->next = current->next;
   }

   if(current == q->last) {
      //change last to point to prev link
      q->last = current->prev;
   } else {
      current->next->prev = current->prev;
   }
   q->itemCount--;
   return current;
}
