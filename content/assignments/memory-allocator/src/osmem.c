// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include "helpers.h"

struct block_meta *heap_start;
struct block_meta *prefix;
char first_brk = 1;
FILE* log_file;

// Coalesce all blocks that are free after the given block
void coalesce_next(struct block_meta *start, size_t max_size_to_expand)
{
	struct block_meta *header = start;
	struct block_meta *next = header->next;

	while (next != NULL) {
		if (next->status == STATUS_FREE) {
			header->size += next->size + BLOCK_META_SIZE;
			header->next = next->next;
			next = header->next;
			if (header->size >= max_size_to_expand)
				return;
		} else
			return;
	}
}

// Coalesce all blocks that are free
void coalesce_all_free()
{
	struct block_meta *header = prefix;

	while (header != NULL) {
		if (header->status == STATUS_FREE)
			coalesce_next(header, LONG_MAX);
		header = header->next;
	}
}

// Find the first free block that fits the requested size
struct block_meta *find_fit(struct block_meta **last, size_t size)
{
	coalesce_all_free();
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
				header->size += next->size + BLOCK_META_SIZE;
				header->next = next->next;
				continue;
			}
			// Found large enough block
			if (header->size >= ALIGN(size)) {
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

// Split the block into two blocks, one with the requested size and one with the remaining size
void split(struct block_meta *header, size_t size)
{
	size_t blk_size = ALIGN(size + BLOCK_META_SIZE);
	struct block_meta *new_header = (struct block_meta *)((char *)header + blk_size);

	new_header->size = header->size - size - BLOCK_META_SIZE;
	new_header->status = STATUS_FREE;
	new_header->next = header->next;
	header->next = new_header;
}

// Allocate a new block
void alloc(struct block_meta **header, struct block_meta *last, size_t size, size_t threshold)
{
	size_t blk_size = ALIGN(size + BLOCK_META_SIZE);

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
	(*header)->size = ALIGN(size);
	(*header)->next = NULL;
}

// Same as a malloc, but with a threshold parameter for using mmap
// This is used because calloc uses a different threshold
void *malloc_helper(size_t size, size_t threshold)
{
	// Alloc heap_start if it's the first time allocating
	if (!heap_start) {
		alloc(&heap_start, NULL, size, threshold);
		if (prefix)
			heap_start->next = prefix;
		prefix = heap_start;
		return (void *)((char *)heap_start + BLOCK_META_SIZE);
	}

	struct block_meta *header;
	size_t alligned_size = ALIGN(size);
	struct block_meta *last = heap_start;

	// Find a free block that fits the requested size
	header = find_fit(&last, size);
	if (header) {
		// Split the block if the remaining size is large enough to fit a block_meta struct and 1 byte
		size_t diff = header->size - alligned_size;

		if (diff >= ALIGN(1 + BLOCK_META_SIZE)) {
			split(header, alligned_size);
			header->size = alligned_size;
		}
		header->status = STATUS_ALLOC;
	} else {
		// If last block is free, extend it, otherwise allocate a new block
		if (last->status == STATUS_FREE) {
			size_t extra_size = alligned_size - last->size;

			sbrk(extra_size);
			header = last;
			header->size = alligned_size;
			header->status = STATUS_ALLOC;
		} else {
			alloc(&header, last, size, threshold);
		}
	}
	return (void *)((char *)header + BLOCK_META_SIZE);
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
	coalesce_all_free();
	if (prev_status == STATUS_MAPPED) {
		if (header == heap_start)
			prefix = heap_start->next;
		int result = munmap(header, header->size + BLOCK_META_SIZE);

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

// Check if the block needs to be changed from sbrk to mmap or vice versa
char changes_alloc_type(struct block_meta *header, size_t size)
{
	size_t blk_size = ALIGN(size + BLOCK_META_SIZE);

	if (header->status == STATUS_MAPPED && blk_size < MMAP_THRESHOLD)
		return 1;
	if (header->status == STATUS_ALLOC && blk_size >= MMAP_THRESHOLD)
		return 1;
	return 0;
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
	size_t blk_size = ALIGN(size + BLOCK_META_SIZE);
	size_t alligned_size = ALIGN(size);

	// If the new size is smaller than the old size, we might be able to split the block
	if (old_size >= alligned_size) {
		// Check if the block doesn't need to change allocation type
		if (changes_alloc_type(header, size) == 0) {
			if (old_size - alligned_size >= ALIGN(1 + BLOCK_META_SIZE)) {
				split(header, alligned_size);
				header->size = alligned_size;
			}
			return ptr;
		}
		if (old_size == alligned_size)
			return ptr;
	} else {
		// Check if block is last block to do expanding
		if (header->next == NULL && header->status != STATUS_ALLOC && blk_size < MMAP_THRESHOLD) {
			size_t extra_size = alligned_size - old_size;

			sbrk(extra_size);
			header->size = alligned_size;
			return ptr;
		}
		// Try to coalesce the block with the next ones
		coalesce_next(header, alligned_size);
		if (header->size >= alligned_size) {
			// Check if the block doesn't need to change allocation type
			if (changes_alloc_type(header, size) == 0) {
				if (header->size - alligned_size >= ALIGN(1 + BLOCK_META_SIZE)) {
					split(header, alligned_size);
					header->size = alligned_size;
				}
				return ptr;
			}
		}
	}
	
	// If the block was not coalesced or expanded, allocate a new block and copy the data
	void *new_ptr = os_malloc(size);

	DIE(new_ptr == NULL, "os_malloc failed");
	size_t lowest = old_size < alligned_size ? old_size : alligned_size;
	memcpy(new_ptr, ptr, lowest);

	os_free(ptr);
	return new_ptr;
}
