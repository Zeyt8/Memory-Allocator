// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include "helpers.h"

struct block_meta* heap_start;

struct block_meta* find_fit(size_t size)
{
 	struct block_meta* header = heap_start;
	struct block_meta* next = NULL;
 	while (header->next != NULL)
	{
 		if (header->status == STATUS_FREE)
		{
			if (header->size >= size)
			{
 				return header;
			}
			next = header->next;
			if (next->next != NULL && next->status == STATUS_FREE)
			{
				header->size += next->size;
				header->next = next->next;
				continue;
			}
		}
 		header = header->next;
 	}
 	return NULL;
}

void *os_malloc(size_t size)
{
	if (size == 0)
	{
		return NULL;
	}

	size_t blk_size = ALIGN(size + BLOCK_META_SIZE);
	struct block_meta *header = find_fit(blk_size);

	if (header && blk_size < header->size)
	{
		struct block_meta* new_header = (struct block_meta*)((char*)header + blk_size);
		new_header->size = header->size - blk_size;
		new_header->status = STATUS_FREE;
		new_header->next = header->next;
		header->next = new_header;
	}
	else
	{
		header = (struct block_meta*)sbrk(blk_size);
		DIE(header == MAP_FAILED, "sbrk failed");
	}
	header->status = STATUS_ALLOC;

	return (void*)((char*)header + 8);
}

void os_free(void *ptr)
{
	if (ptr == NULL)
	{
		return;
	}
	
	struct block_meta *header = (struct block_meta*)((char*)ptr - BLOCK_META_SIZE);
	header->status = STATUS_FREE;
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
	{
        return NULL;
    }

    size_t total_size = nmemb * size;

    void *ptr = os_malloc(total_size);
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

	struct block_meta* header = (struct block_meta*)((char*)ptr - BLOCK_META_SIZE);
	size_t old_size = header->size;
	size_t new_size = ALIGN(size + BLOCK_META_SIZE);
	void* new_ptr;

	if (old_size >= new_size)
	{
		return ptr;
	}
	else
	{
		new_ptr = os_malloc(size);
		DIE(new_ptr == NULL, "os_malloc failed");
		memcpy(new_ptr, ptr, old_size - BLOCK_META_SIZE);
		os_free(ptr);
		return new_ptr;
	}
}
