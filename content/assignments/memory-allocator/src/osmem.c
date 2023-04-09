// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include "helpers.h"
#include <stdio.h>

FILE* log_file;
struct block_meta *heap_start;

struct block_meta *find_fit(struct block_meta **last, size_t size)
{
	struct block_meta *header = heap_start;
	struct block_meta *next = NULL;
	while (header != NULL)
	{
		fprintf(log_file, "os_calloc()\n");
		fflush(log_file);
		fprintf(log_file, "os_calloc(%d)\n", header->status);
		fflush(log_file);
		if (header->status == STATUS_FREE)
		{
			if (header->size >= size)
			{
				return header;
			}
			next = header->next;
			
			if (next != NULL && next->status == STATUS_FREE)
			{
				header->size += next->size;
				header->next = next->next;
				continue;
			}
		}
		*last = header;
		header = header->next;
	}
	return NULL;
}

void split(struct block_meta *header, size_t blk_size)
{
	struct block_meta *new_header = (struct block_meta *)((char *)header + blk_size);
	new_header->size = header->size - blk_size;
	new_header->status = STATUS_FREE;
	new_header->next = header->next;
	header->next = new_header;
}

void alloc(struct block_meta** header, struct block_meta* last, size_t blk_size, int heap_start, size_t threshold)
{
	if (blk_size < threshold)
	{
		if (heap_start)
		{
			(*header) = (struct block_meta *)sbrk(MMAP_THRESHOLD);
		}
		else
		{
			(*header) = (struct block_meta *)sbrk(blk_size);
		}
		DIE(header == MAP_FAILED, "sbrk failed");
		(*header)->status = STATUS_ALLOC;
		/*if (heap_start)
		{
			split((*header), blk_size);
		}*/
	}
	else
	{
		(*header) = (struct block_meta *)mmap(NULL, blk_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		DIE(header == MAP_FAILED, "mmap failed");
		(*header)->status = STATUS_MAPPED;
	}
	if (last)
	{
		last->next = *header;
	}
	(*header)->size = blk_size;
	(*header)->next = NULL;
}

void *malloc_helper(size_t size, size_t threshold)
{
	size_t blk_size = ALIGN(size + BLOCK_META_SIZE);
	if (!heap_start)
	{
		alloc(&heap_start, NULL, blk_size, 1, threshold);
		return (void *)((char *)heap_start + sizeof(struct block_meta));
	}

	struct block_meta *header;
	blk_size = blk_size < BLOCK_META_SIZE ? BLOCK_META_SIZE : blk_size;

	struct block_meta *last = heap_start;
	header = find_fit(&last, blk_size);
	
	if (header)
	{
		size_t diff = header->size - blk_size;
		if (diff >= ALIGN(1 + BLOCK_META_SIZE))
		{
			split(header, blk_size);
			header->size = blk_size;
		}
		header->status = STATUS_ALLOC;
	}
	else
	{
		if (last->status == STATUS_FREE)
		{
			size_t extra_size = blk_size - last->size;
			sbrk(extra_size);
			header = last;
			header->size = blk_size;
			header->status = STATUS_ALLOC;
		}
		else
		{
			alloc(&header, last, blk_size, 0, threshold);
		}
	}
	return (void *)((char *)header + sizeof(struct block_meta));
}

void *os_malloc(size_t size)
{
	if (size == 0)
	{
		return NULL;
	}
	return malloc_helper(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	if (ptr == NULL)
	{
		return;
	}
	
	struct block_meta *header = (struct block_meta *)((char *)ptr - BLOCK_META_SIZE);
	int prev_status = header->status;
	header->status = STATUS_FREE;
	
	if (prev_status == STATUS_MAPPED)
	{
		int result = munmap(header, header->size);
		DIE(result == -1, "munmap failed");
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (!log_file)
	{
		log_file = fopen("log.txt", "w");
		DIE(log_file == NULL, "fopen failed");
	}
	if (nmemb == 0 || size == 0)
	{
		return NULL;
	}
	
	size_t total_size = nmemb * size;

	long sz = sysconf(_SC_PAGE_SIZE);
	DIE(sz == -1, "sysconf failed");
	void *ptr = malloc_helper(total_size, sz);
	DIE(ptr == NULL, "os_malloc failed");

	memset(ptr, 0, total_size);

	return ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	if (ptr == NULL)
	{
		return os_malloc(size);
	}

	struct block_meta *header = (struct block_meta *)((char *)ptr - BLOCK_META_SIZE);
	size_t old_size = header->size;
	size_t new_size = ALIGN(size + BLOCK_META_SIZE);

	if (old_size >= new_size)
	{
		return ptr;
	}
	else
	{
		void *new_ptr = os_malloc(size);
		DIE(new_ptr == NULL, "os_malloc failed");
		memcpy(new_ptr, ptr, old_size - BLOCK_META_SIZE);
		os_free(ptr);
		return new_ptr;
	}
}
