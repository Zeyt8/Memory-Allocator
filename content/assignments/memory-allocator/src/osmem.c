// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include "helpers.h"

struct block_meta *heap_start;

struct block_meta *find_fit(struct block_meta *last, size_t size)
{
	struct block_meta *header = heap_start;
	struct block_meta *next = NULL;
	while (header->next != NULL)
	{
		if (header->status == STATUS_FREE)
		{
			if (header->size >= size)
			{
				if (last)
				{
					last->next = header;
				}
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
		last = header;
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

	if (!heap_start)
	{
		if (blk_size < MMAP_THRESHOLD)
		{
			heap_start = (struct block_meta *)sbrk(MMAP_THRESHOLD);
			DIE(heap_start == MAP_FAILED, "sbrk failed");
			heap_start->status = STATUS_ALLOC;
		}
		else
		{
			heap_start = (struct block_meta *)mmap(NULL, blk_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			DIE(heap_start == MAP_FAILED, "mmap failed");
			heap_start->status = STATUS_MAPPED;
		}
		heap_start->size = blk_size;
		return (void *)((char *)heap_start + sizeof(struct block_meta));
	}

	struct block_meta *header;
	blk_size = blk_size < BLOCK_META_SIZE ? BLOCK_META_SIZE : blk_size;

	header = find_fit(heap_start, blk_size);
	if (header && blk_size < header->size)
	{
		struct block_meta *new_header = (struct block_meta *)((char *)header + blk_size);
		new_header->size = header->size - blk_size;
		new_header->status = STATUS_FREE;
		new_header->next = header->next;
		header->next = new_header;
	}
	else
	{
		if (blk_size < MMAP_THRESHOLD)
		{
			header = (struct block_meta *)sbrk(blk_size);
			DIE(header == MAP_FAILED, "sbrk failed");
			header->status = STATUS_ALLOC;
		}
		else
		{
			header = (struct block_meta *)mmap(NULL, blk_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			DIE(header == MAP_FAILED, "mmap failed");
			header->status = STATUS_MAPPED;
		}
		header->size = blk_size;
	}

	return (void *)((char *)header + sizeof(struct block_meta));
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

	struct block_meta *header = (struct block_meta *)((char *)ptr - BLOCK_META_SIZE);
	size_t old_size = header->size;
	size_t new_size = ALIGN(size + BLOCK_META_SIZE);
	void *new_ptr;

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
