// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include "helpers.h"

struct block_meta *heap_start;
struct block_meta *prefix;
char first_brk = 1;

void coalesce_starting_with(struct block_meta *start, char has_max_size, size_t max_size_to_expand)
{
	struct block_meta *header = start;
	struct block_meta *next = NULL;

	while (header != NULL) {
		next = header->next;
		if (next != NULL && next->status == STATUS_FREE) {
			header->size += next->size;
			header->next = next->next;
			if (has_max_size && header->size >= max_size_to_expand)
				break;
			continue;
		}
		header = header->next;
	}
}

struct block_meta *find_fit(struct block_meta **last, size_t size)
{
	struct block_meta *header = prefix;
	struct block_meta *next = NULL;
	size_t min_size = LONG_MAX;
	struct block_meta *min_header = NULL;

	// Find the first free block that fits the requested size
	while (header != NULL) {
		if (header->status == STATUS_FREE) {
			next = header->next;
			// Merge with the next block if it is free
			if (next != NULL && next->status == STATUS_FREE) {
				header->size += next->size;
				header->next = next->next;
				continue;
			}
			// Found large enough block
			if (header->size >= size) {
				// Update the minimum size and header
				if (header->size < min_size) {
					min_size = header->size;
					min_header = header;
				}
			}
		}
		*last = header;
		header = header->next;
	}
	return min_header;
}

void split(struct block_meta *header, size_t blk_size)
{
	struct block_meta *new_header = (struct block_meta *)((char *)header + blk_size);

	new_header->size = header->size - blk_size;
	new_header->status = STATUS_FREE;
	new_header->next = header->next;
	header->next = new_header;
}

void alloc(struct block_meta **header, struct block_meta *last, size_t blk_size, size_t threshold)
{
	// Alloc with sbrk if the size is smaller than the threshold, otherwise use mmap
	if (blk_size < threshold) {
		// If it's the first time allocating with sbrk, allocate MMAP_THRESHOLD size
		if (first_brk) {
			(*header) = (struct block_meta *)sbrk(MMAP_THRESHOLD);
			first_brk = 0;
		} else
			(*header) = (struct block_meta *)sbrk(blk_size);
		DIE(*header == MAP_FAILED, "sbrk failed");
		(*header)->status = STATUS_ALLOC;
	} else {
		(*header) = (struct block_meta *)mmap(NULL, blk_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		DIE(*header == MAP_FAILED, "mmap failed");
		(*header)->status = STATUS_MAPPED;
	}
	if (last)
		last->next = *header;
	(*header)->size = blk_size;
	(*header)->next = NULL;
}

void *malloc_helper(size_t size, size_t threshold)
{
	size_t blk_size = ALIGN(size + BLOCK_META_SIZE);

	// Alloc heap_start if it's the first time allocating
	if (!heap_start) {
		alloc(&heap_start, NULL, blk_size, threshold);
		heap_start->next = prefix;
		prefix = heap_start;
		return (void *)((char *)heap_start + sizeof(struct block_meta));
	}
	struct block_meta *header;

	// blk_size must be at least BLOCK_META_SIZE to fit the block_meta struct
	blk_size = blk_size < BLOCK_META_SIZE ? BLOCK_META_SIZE : blk_size;

	struct block_meta *last = heap_start;

	// Find a free block that fits the requested size
	header = find_fit(&last, blk_size);
	if (header) {
		// Split the block if the remaining size is large enough to fit a block_meta struct and 1 byte
		size_t diff = header->size - blk_size;

		if (diff >= ALIGN(1 + BLOCK_META_SIZE)) {
			split(header, blk_size);
			header->size = blk_size;
		}
		header->status = STATUS_ALLOC;
	} else {
		// If last block is free, extend it, otherwise allocate a new block
		if (last->status == STATUS_FREE) {
			size_t extra_size = blk_size - last->size;

			sbrk(extra_size);
			header = last;
			header->size = blk_size;
			header->status = STATUS_ALLOC;
		} else {
			alloc(&header, last, blk_size, threshold);
		}
	}
	return (void *)((char *)header + sizeof(struct block_meta));
}

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;
	return malloc_helper(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	if (ptr == NULL)
		return;
	struct block_meta *header = (struct block_meta *)((char *)ptr - BLOCK_META_SIZE);
	int prev_status = header->status;

	header->status = STATUS_FREE;
	if (prev_status == STATUS_MAPPED) {
		if (header == heap_start)
			prefix = heap_start->next;
		int result = munmap(header, header->size);

		DIE(result == -1, "munmap failed");
		if (header == heap_start)
			heap_start = NULL;
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;
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
		return os_malloc(size);
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}
	struct block_meta *header = (struct block_meta *)((char *)ptr - BLOCK_META_SIZE);
	
	if (header->status == STATUS_FREE)
		return NULL;
	size_t old_size = header->size;
	size_t new_size = ALIGN(size + BLOCK_META_SIZE);

	// If the new size is smaller than the old size, we might be able to split the block
	if (old_size >= new_size) {
		// Check if the block was allocated with mmap and if the new size should be allocated with sbrk
		if (header->status != STATUS_MAPPED || new_size >= MMAP_THRESHOLD) {
			if (old_size - new_size >= ALIGN(1 + BLOCK_META_SIZE)) {
				split(header, new_size);
				header->size = new_size;
			}
			return ptr;
		}
	}
	// Check if block is last block to do expanding
	if (header->next == NULL && header->status == STATUS_ALLOC && new_size < MMAP_THRESHOLD) {
		size_t extra_size = new_size - old_size;

		sbrk(extra_size);
		header->size = new_size;
		return ptr;
	}
	coalesce_starting_with(header, 1, new_size);
	if (header->size >= new_size) {
		if (header->status != STATUS_MAPPED || new_size >= MMAP_THRESHOLD) {
			if (header->size - new_size >= ALIGN(1 + BLOCK_META_SIZE)) {
				split(header, new_size);
				header->size = new_size;
			}
			return ptr;
		}
	}
	// If the block was not coalesced, allocate a new block and copy the data
	void *new_ptr = os_malloc(new_size);

	DIE(new_ptr == NULL, "os_malloc failed");
	size_t lowest = old_size < new_size ? old_size : new_size;
	memcpy(new_ptr, ptr, lowest - BLOCK_META_SIZE);

	os_free(ptr);
	return new_ptr;
}
