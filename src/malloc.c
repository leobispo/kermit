#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <math.h>

#define MAX_MEMSIZE 32
#define NUM_OF_PROCESSORS 1


/*
 +---------------------------+
 |HEADER                     | <--+
 +---------------------------+    |
 |USER_MEMORY_START          |    |
 |                           |    |
 |...                        |    |
 |                           |    |
 |USER_MEMORY_END            |    |
 +---------------------------+    |
 |FOOTER - Pointer to HEADER | ---+
 +---------------------------+
 |HEADER                     | <--+
 +---------------------------+    |
 |USER_MEMORY_START          |    |
 |                           |    |
 |...                        |    |
 |                           |    |
 |USER_MEMORY_END            |    |
 +---------------------------+    |
 |FOOTER - Pointer to HEADER | ---+
 +---------------------------+
              .
              .
              .
 +---------------------------+
 |HEADER                     | <--+
 +---------------------------+    |
 |USER_MEMORY_START          |    |
 |                           |    |
 |...                        |    |
 |                           |    |
 |USER_MEMORY_END            |    |
 +---------------------------+    |
 |FOOTER - Pointer to HEADER | ---+
 +---------------------------+
*/

typedef struct malloc_chunk_footer;

typedef struct malloc_chunk_header{
	bool in_use;
	struct malloc_chunk_header *next;
	struct malloc_chunk_header *prev;
  struct malloc_chunk_footer *prev_footer;

  size_t user_mem_size;

}malloc_chunk_footer;

typedef struct malloc_chunk_footer{
  malloc_chunk_header *header;
}malloc_chunk_footer;


typedef struct malloc_chunk_bucket{
	malloc_chunk *first;
	malloc_chunk *last;
}malloc_chunk_bucket;

static struct malloc_chunk_bucket mem_buckets[MAX_MEMSIZE] = {0};

//organize the blocks of memory
void arrange_chunks(void *addr, int *bytes){


}

void *meuAlocaMem(int bytes){
	int mem_bucket;
	int i, j;
	void *addr;

  int chunk_size = sizeof(struct malloc_chunk_header) + bytes + sizeof(struct malloc_chunk_footer);

	//find the bucket to search space
	mem_bucket = ceil(log2(bytes)) - 1;
	printf("%d\n", mem_bucket);

	//search space in all buckets >= mem_bucket
	for (i=mem_bucket; i<MAX_MEMSIZE; ++i){
		if (mem_buckets[i].first != NULL){
			break;
		}
	}

	//if you didn't find any space
	if (mem_buckets[i].first == NULL){
		//get the pagesize
		int pagesize = sysconf(_SC_PAGE_SIZE);
		int num_pages, available_space;

		//calculate the number of pages you must allocate
		num_pages = ceil((float)bytes/(float)pagesize);

    //space in bytes
		available_space = pagesize*num_pages;
		addr = sbrk(available_space);

		struct malloc_chunk_header *chunk_header = (struct malloc_chunk_header *)
        addr;

    chunk_header->prev_footer   = NULL;
		chunk_header->in_use        = true;
		chunk_header->next          = NULL;
		chunk_header->prev          = NULL;
    chunk_header->user_mem_size = available_space - sizeof(struct malloc_chunk_header) - sizeof(struct malloc_chunk_footer);

    struct malloc_chunk_footer *chunk_footer =

		arrange_chunks(header->mem_end_address+1, &available_space);

		return header->mem_start_address;

  //there's memory enough to be used
	}else{




	}

}

void  meuLiberaMem ( void*  ptr ){


}

int main(int argc, char *argv[]) {
		void *addr = meuAlocaMem(50);

    /*printf("%d\n", sizeof(malloc_chunk));*/
}


