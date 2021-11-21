#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "dict.h"
#include "xxhash.h"

#define GROWTH_FACTOR (2)
#define MAX_LOAD_FACTOR (0.80)

//int first_av;
//struct elt elt_list[INITIAL_SIZE];
//struct elt *pelt_list[INITIAL_SIZE];

//struct dict *d=NULL;
//struct elt *elt_list=NULL;
//int *ht_table=NULL;

// Totoal size = sizeof(dict) + sizeof(struct elt *)*INITIAL_SIZE + sizeof(struct elt)*INITIAL_SIZE
// memory region three parts. 1) dict ( sizeof(dict) ) 2) pelt_list[] ( sizeof(struct elt *)*INITIAL_SIZE ) 3) elt_list ( sizeof(struct elt)*INITIAL_SIZE )

void Init_Pointers(Dict d, struct elt ** p_elt_list, int ** p_ht_table)
{
//    ht_table = (int *)((void *)d + sizeof(struct dict));
//	  elt_list = (struct elt *)((void *)d + sizeof(struct dict) + sizeof(int)*(d->size));
    *p_ht_table = (int *)((void *)d + sizeof(struct dict));
	*p_elt_list = (struct elt *)((void *)d + sizeof(struct dict) + sizeof(int)*(d->size));
}

void DictCreate(Dict d, int nSize, struct elt ** p_elt_list, int ** p_ht_table)
{
  int i;

	if(d == NULL)	{
		printf("d = NULL.\nThe memory for hash table is not allocated.\nQuit\n");
		exit(1);
	}

  if(nSize) {
	  if(nSize) {
	    d->size = nSize;
	  }
	  else {
	    d->size = INITIAL_SIZE;
	  }
	  d->n = 0;
  }
  Init_Pointers(d, p_elt_list, p_ht_table);

//    ht_table = (int *)((void *)d + sizeof(dict));
//	elt_list = (struct elt *)((void *)d + sizeof(struct dict) + sizeof(int)*INITIAL_SIZE);

  if(nSize)	{
	  for(i = 0; i < d->size; i++) (*p_ht_table)[i] = -1;
	  d->first_av = 0;
	  for(i=0; i<d->size; i++)	{
		  (*p_elt_list)[i].next = i+1;
	  }
	  (*p_elt_list)[d->size - 1].next = -1;	// the end
  }
}
/*
static void grow(Dict d)
{
    Dict d2;            // new dictionary we'll create 
    struct dict swap;   // temporary structure for brain transplant
    int i;
    struct elt *e;

    d2 = internalDictCreate(d->size * GROWTH_FACTOR);

    for(i = 0; i < d->size; i++) {
        for(e = d->table[i]; e != 0; e = e->next) {
            //  note: this recopies everything 
            //  a more efficient implementation would
            // patch out the strdups inside DictInsert
            // to avoid this problem 
            DictInsert(d2, e->key, e->value);
        }
    }

    // the hideous part
    // We'll swap the guts of d and d2
    // then call DictDestroy on d2
    swap = *d;
    *d = *d2;
    *d2 = swap;

    DictDestroy(d2);
}
*/

// insert a new key-value pair into an existing dictionary 
int DictInsert(Dict d, const char *key, const int value, struct elt ** p_elt_list, int ** p_ht_table)
{
    struct elt *e;
    unsigned long long h;
	int first_av_Save, nLen;

    assert(key);

	first_av_Save = d->first_av;
	e = &( (*p_elt_list)[d->first_av]);	// first available unit
	strcpy(e->key, key);
    e->value = value;

	d->first_av = e->next;	// pointing to the next available unit

 	nLen = strlen(key);
	if(nLen >= MAX_NAME_LEN)	{
		printf("ERROR in DictInsert: Hash table key length is not long enough. Overflowed.\n");
	}
   h = XXH64(key, nLen, 0) % d->size;

    e->next = (*p_ht_table)[h];
    (*p_ht_table)[h] = first_av_Save;
    d->n++;

    /* grow table if there is not enough room */
    if(d->n >= (d->size * MAX_LOAD_FACTOR) ) {
		printf("Hash table is FULL.\nQuit.\n");
		exit(1);
//        grow(d);
    }
	return first_av_Save;	// the unit saving the data
}

int DictInsertAuto(Dict d, const char *key, struct elt ** p_elt_list, int ** p_ht_table)
{
    struct elt *e;
    unsigned long long h;
	int first_av_Save;

    assert(key);

	first_av_Save = d->first_av;
	e = &( (*p_elt_list)[d->first_av]);	// first available unit
	strcpy(e->key, key);
    e->value = first_av_Save;

	d->first_av = e->next;	// pointing to the next available unit

    h = XXH64(key, strlen(key), 0) % d->size;

    e->next = (*p_ht_table)[h];
    (*p_ht_table)[h] = first_av_Save;
    d->n++;

    /* grow table if there is not enough room */
    if(d->n >= (d->size * MAX_LOAD_FACTOR) ) {
		printf("Hash table is FULL.\nQuit.\n");
		exit(1);
//        grow(d);
    }
	return first_av_Save;	// the unit saving the data
}
/* return the most recently inserted value associated with a key */
/* or 0 if no matching key is present */
int DictSearch(Dict d, const char *key, struct elt ** p_elt_list, int ** p_ht_table, unsigned long long *fn_hash)
{
	int idx;
	struct elt *e;
	
	if(d->n == 0) return (-1);
	
	*fn_hash = XXH64(key, strlen(key), 0);
	idx = (*p_ht_table)[(*fn_hash)% d->size];
	if(idx == -1)	{
		return (-1);
	}
	
	e = &( (*p_elt_list)[idx] );
    while(1) {
        if(!strcmp(e->key, key)) {
            return e->value;
        }
		else	{
			idx = e->next;
			if(idx == -1)	{	// end
				return (-1);
			}
			e = &( (*p_elt_list)[idx] );
		}
    }
	
    return -1;
}


// delete the most recently inserted record with the given key 
// if there is no such record, has no effect 
void DictDelete(Dict d, const char *key, struct elt ** p_elt_list, int ** p_ht_table)
{
    int idx, next;
    unsigned long long h;

	h = XXH64(key, strlen(key), 0) % d->size;
	idx = (*p_ht_table)[h];
	
	if(!strcmp((*p_elt_list)[idx].key, key)) {	// found as the first element
		(*p_ht_table)[h] = (*p_elt_list)[idx].next;
		(*p_elt_list)[idx].next = d->first_av;
		d->first_av = idx;		// put back as the beginning of the free space
		d->n--;
		return;
	}

	next = (*p_elt_list)[idx].next;
    for(; next != -1; next = (*p_elt_list)[idx].next) {
        if(!strcmp((*p_elt_list)[next].key, key)) {
  		  (*p_elt_list)[idx].next = (*p_elt_list)[next].next;
		  (*p_elt_list)[next].next = d->first_av;
		  d->first_av = next;		
		  d->n--;
		  return;
        }
        idx = next;
    }
}


