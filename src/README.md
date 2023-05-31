# Memory Allocator

## Helpers

- **coallesce_next**

    Coalesce the next block with the current block if the next block is free. Continue coalescing until the next block is not free, the end of the heap is reached, or the maximum coalesced memory size is reached.

    This is used to try and expand the current block to fit a larger allocation when reallocating. It also is used in **coallesce_all_free**.

- **coallesce_all_free**

    Coalesce all free blocks in the heap. This is used in **os_free**. This fixes some bugs when freeing a block that is not the last block in the heap that was allocated with **mmap**.

- **find_fit**

    Calls **coallesce_all_free**. Finds the free block that is large enough to fit the requested size and is closest to the required size. Also coalesces blocks on the way.
    
    Returns *null* if no block is found. It also sets *last* to the block before the block that was found. If no block was found, *last* is set to the last block in the heap.

- **split**

    Splits the block into two blocks. The first block has the requested size and the second block has the remaining size. The second block is marked as free.

- **alloc**

    Allocates a block using *brk* or mmp based on the threshold. If the block is larger than the threshold, it is allocated using mmap. Otherwise, it is allocated using *brk*. The block is added to the linked list.
    
    If it's the first time allocating with *brk*, it alloc *MMAP_THRESHOLD* bytes.

- **prefix**

    Is used to fix a bug when freeing the heap_start that was allocated with *mmap*. It stores the block after heap_start, so when heaep_start is allocated again it can be added properly to the linked list.

- **changes_alloc_type**

    Checks if the block, if reallocated with the received size would be allocated with a different method. If it would be allocated with a different method, it returns 1. Otherwise, it returns 0.

## Functions

- **malloc_helper**

    This is the main malloc function. It is used by malloc and calloc. Unlike malloc it also takes *threshold* as a parameter, because calloc and malloc have different thresholds for *brk* and *mmap*.

    It checks wheter the heap_start is allocating. If not, it allocates it.

    If it was allocated, then it searches for the best fit free block. If enough space is left, it is split into two blocks. If not, the whole block is allocated.

    If no block was found, it checks if the last block is free. If it is, it tries to expand the last block to fit the requested size. Otherwise it allocates a new block.

- **os_malloc**

    Returns if size is 0. Otherwise, it calls **malloc_helper** with the requested size and the *MMAP_THRESHOLD*.

- **os_free**

    Gets the header of the pointer and sets the block to free. Coalesces all free blocks. If the block was allocated with *mmap*, it deallocates it with *munmap*. It also deallocates the header. If the heap_start was the block deallocated, it sets up prefix.

- **os_calloc**

    Returns if either argument is 0. Otherwise, it calls **malloc_helper** with the requested size and *_SC_PAGE_SIZE*. Memsets the payload to 0.

- **os_realloc**

    If ptr is null, returns **os_malloc**. If size is 0, calls **os_free** and returns null. If the block is free, it returns null.

    Check if the new size is lower than or equal to the old size. If it is and the block doesn't change allocation type, check if it can be split. If it can, split it. Otherwise, return the pointer. If the sizes are the same, just return the pointer.

    Else, check if the block is the last and can be expanded to fit the new size. If it can, return the pointer. Otherwise, try to coalesce the blocks after the current block. If it can be coalesced, check if the size is lower than or equal to the new block size. If it is and the block doesn't change allocation type, check if it can be split. If it can, split it. Otherwise, return the pointer.

    If none of the options above worked, call malloc with the new size, copy the old payload to the new payload, and free the old block. Return the new pointer.