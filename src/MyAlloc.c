/*
 * 	Modified by:
 *		Tom Gibson and Colton Phillips
 */

/* MyAlloc.c */

/*
   All memory allocation and deallocation is performed in this module.
   
   There are two families of functions, one just for managing the Java heap
   and a second for other memory requests.  This second family of functions
   provides wrappers for standard C library functions but checks that memory
   is not exhausted and zeroes out any returned memory.
   
   Java Heap Management Functions:
   * InitMyAlloc  -- initializes the Java heap before execution starts
   * MyHeapAlloc  -- returns a block of memory from the Java heap
   * gc           -- the System.gc garbage collector
   * MyHeapFree   -- to be called only by gc()!!
   * PrintHeapUsageStatistics  -- does as the name suggests

   General Storage Functions:
   * SafeMalloc  -- used like malloc
   * SafeCalloc  -- used like calloc
   * SafeStrdup  -- used like strdup
   * SafeFree    -- used like free
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ClassFileFormat.h"
#include "TraceOptions.h"
#include "MyAlloc.h"
#include "jvm.h"

/* we will never allocate a block smaller than this */
#define MINBLOCKSIZE 12

#define BLOCK_SIZE_SIZE sizeof(uint32_t)
#define BLOCK_OFFSET_SIZE sizeof(int32_t)
#define MARK_SIZE_BIT 0x800000

typedef struct FreeStorageBlock {
    uint32_t size;  /* size in bytes of this block of storage */
    int32_t  offsetToNextBlock;
    uint8_t  restOfBlock[1];   /* the actual size has to be determined from the size field */
} FreeStorageBlock;

/* these three variables are externally visible */
uint8_t *HeapStart, *HeapEnd;
HeapPointer MaxHeapPtr;

static int offsetToFirstBlock = -1;
static long totalBytesRequested = 0;    // Little more every time we MyHeapAlloc
static int numAllocations = 0;          // How many times, in total
static int gcCount = 0;                 //                 in total
static long totalBytesRecovered = 0;
static int totalBlocksRecovered = 0;    
static int searchCount = 0;             // We gotta search for a block with enough space 

static void *maxAddr = NULL;    // used by SafeMalloc, etc
static void *minAddr = NULL;


/*
 * Allocate the Java heap and initialize the free list 
 */
void InitMyAlloc( int HeapSize ) {
    FreeStorageBlock *FreeBlock;

    HeapSize &= 0xfffffffc;   /* force to a multiple of 4 */
    HeapStart = calloc(1,HeapSize);
    if (HeapStart == NULL) {
        fprintf(stderr, "unable to allocate %d bytes for heap\n", HeapSize);
        exit(1);
    }
    HeapEnd = HeapStart + HeapSize;
    MaxHeapPtr = (HeapPointer)HeapSize;
    
    FreeBlock = (FreeStorageBlock*)HeapStart;
    FreeBlock->size = HeapSize;
    FreeBlock->offsetToNextBlock = -1;  /* marks end of list */
    offsetToFirstBlock = 0;
    
    // Used bu SafeMalloc, SafeCalloc, SafeFree below
    maxAddr = minAddr = malloc(4);  // minimal small request to get things started
}

/*
 * Grabs the kind of some heap pointer
 */
uint32_t getKind(HeapPointer pointer) {

	return *((uint32_t*)REAL_HEAP_POINTER(pointer));
}

/*
 * Reads the kind of some heap pointer into a string
 */
void readKind(HeapPointer pointer, char kind[5]) {

	uint32_t kindVal = getKind(pointer);
	sprintf(kind, "%c%c%c%c",
			(kindVal >> 24) & 0XFF,
			(kindVal >> 16) & 0XFF,
			(kindVal >> 8) & 0XFF,
			(kindVal >> 0) & 0XFF);
}

/*
 * Check whether some bit is non-zero
 * The bit parameter is 1-indexed and relative to the right of the value.
 */
int isNonzeroBit(int bit, uint8_t* value) {

	return ((uintptr_t)value & (1 << (bit - 1)));
}

/*
 * Gets the real heap pointer from a block pointer
 */
uint8_t* realHeapPointerFromBlockPointer(uint8_t* blockPointer) {

    return blockPointer + BLOCK_SIZE_SIZE;
}

/*
 * Gets the heap pointer from a block pointer
 */
HeapPointer heapPointerFromBlockPointer(uint8_t* blockPointer) {

	return MAKE_HEAP_REFERENCE(realHeapPointerFromBlockPointer(blockPointer));
}

/*
 * Gets size pointer from block pointer
 */
uint32_t* blockSizePointerFromBlockPointer(uint8_t* blockPointer) {
    return ((uint32_t*)blockPointer);
}

/*
 * Gets the block size from a block pointer
 */
uint32_t blockSizeFromBlockPointer(uint8_t* blockPointer) {

	return *blockSizePointerFromBlockPointer(blockPointer) & ~MARK_SIZE_BIT;
}


/*
 * Check whether a pointer actually points to a value in the heap
 */
int pointsToHeapObject(uint8_t* possibleHeapPointer) {

	uint8_t* blockPointer = HeapStart;
	uint8_t* heapPointer;

	while (blockPointer < HeapEnd) {

		heapPointer = realHeapPointerFromBlockPointer(blockPointer);

		// if they match, bingo, we found it
		if (heapPointer == possibleHeapPointer) {
			/*printf("hey, possibleHeapPointer %p found with a block size of %i!\n",
					possibleHeapPointer,
					blockSizeFromBlockPointer(blockPointer));*/
			return 1;
		}

		// if the heap pointer is past the pointer we're looking for,
		// we know we won't find a match
		if (heapPointer > possibleHeapPointer) {
			/*printf("whoa, not gunna find %p from %p (block size is %i)\n",
					possibleHeapPointer,
					heapPointer,
					blockSizeFromBlockPointer(blockPointer));*/
			return 0;
		}

		/*printf("Cool. Well, moving on to the next block, skipping block size of %i at %p\n",
				blockSizeFromBlockPointer(blockPointer),
				blockPointer);*/
		blockPointer += blockSizeFromBlockPointer(blockPointer);
	}

	/*printf("yikes. reached the end of the heap looking for %p\n", possibleHeapPointer);*/
	return 0;
}

/*
 * Check whether a value on the stack is a heap pointer
 */
int isHeapPointer(HeapPointer pointer) {

	uint8_t* realHeapPointer = (uint8_t*)REAL_HEAP_POINTER(pointer);

	// The first three bits should be zero
	if (isNonzeroBit(1, realHeapPointer) || isNonzeroBit(2, realHeapPointer) /*|| isNonzeroBit(3, pointer)*/) {
		/*printf("Bit check failed\n");*/
		return 0;
	}

	// The pointer should be in bounds
	if (realHeapPointer < HeapStart || realHeapPointer > HeapEnd) {
		/*printf("Heap bounds check failed\n");*/
		return 0;
	}

	// And then, that value should actually point to a heap object
	if (!pointsToHeapObject(realHeapPointer)) {
		/*printf("Heap object check failed\n");*/
		return 0;
	}

	return 1;
}

/* Returns a pointer to a block with at least size bytes available,
   and initialized to hold zeros.
   Notes:
   1. The result will always be a word-aligned address.
   2. The word of memory preceding the result address holds the
      size in bytes of the block of storage returned (including
      this size field).
   3. A block larger than that requested may be returned if the
      leftover portion would be too small to be useful.
   4. The size of the returned block is always a multiple of 4.
   5. The implementation of MyAlloc contains redundant tests to
      verify that the free list blocks contain plausible info.
*/
void *MyHeapAlloc( int size ) {
    /* we need size bytes plus more for the size field that precedes
       the block in memory, and we round up to a multiple of 4 */
    int offset, diff, blocksize;
    FreeStorageBlock *blockPtr, *prevBlockPtr, *newBlockPtr;
    int minSizeNeeded = (size + sizeof(blockPtr->size) + 3) & 0xfffffffc;

    if (tracingExecution & TRACE_HEAP)
        fprintf(stdout, "* heap allocation request of size %d (augmented to %d)\n",
            size, minSizeNeeded);
    blockPtr = prevBlockPtr = NULL;
    offset = offsetToFirstBlock;
    while(offset >= 0) {
        searchCount++;
        blockPtr = (FreeStorageBlock*)(HeapStart + offset);
        /* the following check should be quite unnecessary, but is
           a good idea to have while debugging */
        if ((offset&3) != 0 || (uint8_t*)blockPtr >= HeapEnd) {
            fprintf(stderr,
                "corrupted block in the free list -- bad next offset pointer\n");
            exit(1);
        }
        blocksize = blockPtr->size;
        /* the following check should be quite unnecessary, but is
           a good idea to have while debugging */
        if (blocksize < MINBLOCKSIZE || (blocksize&3) != 0) {
            fprintf(stderr,
                "corrupted block in the free list -- bad size field\n");
            exit(1);
        }
        diff = blocksize - minSizeNeeded;
        if (diff >= 0) break;
        offset = blockPtr->offsetToNextBlock;
        prevBlockPtr = blockPtr;
    }
    if (offset < 0) {
        static int gcAlreadyPerformed = 0;
        void *result;
        if (gcAlreadyPerformed) {
            /* we are in a recursive call to MyAlloc after a gc */
            fprintf(stderr,
                "\nHeap exhausted! Unable to allocate %d bytes\n", size);
            exit(1);
        }
        gc();
        gcAlreadyPerformed = 1;
        result = MyHeapAlloc(size);
        /* control never returns from the preceding call if the gc
           did not obtain enough storage */
        gcAlreadyPerformed = 0;
        return result;
    }
    /* we have a sufficiently large block of free storage, now determine
       if we will have a significant amount of storage left over after
       taking what we need */
    if (diff < MINBLOCKSIZE) {
        /* we will return the entire free block that we found, so
           remove the block from the free list  */
        if (prevBlockPtr == NULL)
            offsetToFirstBlock = blockPtr->offsetToNextBlock;
        else
            prevBlockPtr->offsetToNextBlock = blockPtr->offsetToNextBlock;
        if (tracingExecution & TRACE_HEAP)
            fprintf(stdout, "* free list block of size %d used\n", blocksize);
    } else {
        /* we split the free block that we found into two pieces;
           blockPtr refers to the piece we will return;
           newBlockPtr will refer to the remaining piece */
        blockPtr->size = minSizeNeeded;
        newBlockPtr = (FreeStorageBlock*)((uint8_t*)blockPtr + minSizeNeeded);
        /* replace the block in the free list with the leftover piece */
        if (prevBlockPtr == NULL)
            offsetToFirstBlock += minSizeNeeded;
        else
            prevBlockPtr->offsetToNextBlock += minSizeNeeded;
        newBlockPtr->size = diff;
        newBlockPtr->offsetToNextBlock = blockPtr->offsetToNextBlock;
        if (tracingExecution & TRACE_HEAP)
            fprintf(stdout, "* free list block of size %d split into %d + %d\n",
                diff+minSizeNeeded, minSizeNeeded, diff);

	// These, obviously, should always be heap pointers
	/*HeapPointer heapPointer = MAKE_HEAP_REFERENCE((uint8_t*)blockPtr + sizeof(blockPtr->size));
	printf("returning %p (min is %p and max is %p) - is that a heap pointer? %s\n",
			newBlockPtr,
			HeapStart,
			HeapEnd,
			(isHeapPointer(heapPointer)? "yes" : "no"));*/
    }
    blockPtr->offsetToNextBlock = 0;  /* remove this info from the returned block */
    totalBytesRequested += minSizeNeeded;
    numAllocations++;
    return (uint8_t*)blockPtr + sizeof(blockPtr->size);
}


/* When garbage collection is implemented, this function should never
   be called from outside the current file.
   // TODO How do you feel about embedding questions in CODE?
   // QUEST Couldn't you just make the dang function private instead of 
            asking the user to not call it externally? #CProgramming
   This implementation checks that p is plausible and that the block of
   memory referenced by p holds a plausible size field.
*/
static void MyHeapFree(void *p) {
    uint8_t *p1 = (uint8_t*)p;
    int blockSize;
    FreeStorageBlock *blockPtr;

    if (p1 < HeapStart || p1 >= HeapEnd || ((p1-HeapStart) & 3) != 0) {
        fprintf(stderr, "bad call to MyHeapFree -- bad pointer\n");
        exit(1);
    }
    /* step back over the size field */
    p1 -= sizeof(blockPtr->size);
    /* now check the size field for validity */
    blockSize = *(uint32_t*)p1;
    if (blockSize < MINBLOCKSIZE || (p1 + blockSize) > HeapEnd || (blockSize & 3) != 0) {
        exit(1);
    }
    /* link the block into the free list at the front */
    blockPtr = (FreeStorageBlock*)p1;
    blockPtr->offsetToNextBlock = offsetToFirstBlock;
    offsetToFirstBlock = p1 - HeapStart;

    totalBlocksRecovered += 1;
    totalBytesRecovered += blockSize;
}

/*
 * The number of items in the JVM stack
 */
int jvmStackHeight() {

	return (int)(JVM_Top - JVM_Stack);
}

/*
 * BlockSize pointer is just a little bit before a Heap Pointer.
 * This function returns that BlockSize Pointer
 */
uint32_t* blockSizePointerFromHeapPtr(HeapPointer heapPointer) {

	// Uh, let's see
	// Move back from heap pointer the size of, uh, the size field
	// then return that pointer interpreted as a pointer to the size field
	return (uint32_t*)((uint8_t*)REAL_HEAP_POINTER(heapPointer) - sizeof(uint32_t));
}

/*
 * BlockSize pointer is just a little bit before a Heap Pointer.
 * This function returns that BlockSize Pointer
 */
uint8_t* blockPointerFromHeapPointer(HeapPointer heapPointer) {

	// Uh, let's see
	// Move back from heap pointer the size of, uh, the size field
	// then return that pointer interpreted as a pointer to the size field
	return (uint8_t*)REAL_HEAP_POINTER(heapPointer) - sizeof(uint32_t);
}

/*
 * Gets the offset for a block
 */
int32_t getBlockOffset(uint8_t* block) {

	return ((FreeStorageBlock*)block)->offsetToNextBlock;
}

/*
 * Sets the offset for a block
 */
void setBlockOffset(uint8_t* block, int32_t offset) {

	((FreeStorageBlock*)block)->offsetToNextBlock = offset;
}

/*
 * Gets the pointer to the next block for some block
 * Note that no check is down to see if the block is free
 * Returns null if there is no next block
 */
uint8_t* getNextBlock(uint8_t* block) {

	int32_t offset = getBlockOffset(block);

	if (offset == -1)
		return 0;
	else
		return HeapStart + offset;
}

/*
 * Checks if the mark bit is set
 */
int markBitIsSet(HeapPointer heapPointer) {

	return *blockSizePointerFromHeapPtr(heapPointer) & MARK_SIZE_BIT;
}

/*
 * Sets the mark bit
 */
void setMarkBit(HeapPointer heapPointer) {

	*blockSizePointerFromHeapPtr(heapPointer) |= MARK_SIZE_BIT;
}

/*
 * Unsets the mark bit
 */
void unsetMarkBit(HeapPointer heapPointer) {

	*blockSizePointerFromHeapPtr(heapPointer) &= ~MARK_SIZE_BIT;
}


void mark(HeapPointer heapPointer);

/*
 * Marks a class type
 */
void markClassType(ClassType* class) {

	// if it's not an array type
	if (!class->isArrayType) {

		// go through all of its static fields
		// TODO check if class.cf.fields_count is actually static fields
		int index = 0;
		for (index = 0; index < class->cf->fields_count; ++index) {

			// And if any look like they might be references
			HeapPointer heapPointer = class->classField[index].pval;
			if (isHeapPointer(heapPointer)) {

				// mark them
				mark(heapPointer);
			}
		}

		// And then mark the parent class if it exists?
		if (class->parent) markClassType(class->parent);
	}
}

/*
 * Marks a block as live and recurses to other heap references.
 */
void mark(HeapPointer heapPointer) {

	// Okay. If that thing is not marked
	if (!markBitIsSet(heapPointer)) {

		setMarkBit(heapPointer);

		// and recurse
		switch(getKind(heapPointer)) {
			case CODE_ARRA: {

				ArrayOfRef *array = (ArrayOfRef*)REAL_HEAP_POINTER(heapPointer);
				int index = 0;
				for (index = 0; index < array->size; ++index) {

					mark(array->elements[index]);
				}

			} break;

			case CODE_ARRS: {

				// No further work for a simple array
			} break;

			case CODE_CLAS: {

				ClassType *class = ((ClassType*)REAL_HEAP_POINTER(heapPointer));
				markClassType(class);

			} break; 

			case CODE_INST: {

				ClassInstance *instance = ((ClassInstance*)REAL_HEAP_POINTER(heapPointer));

				if (instance->thisClass) {

					// Mark the thing's class
					markClassType(instance->thisClass);

					// Then mark all of the instance's references
					int index = 0;
					for (index = 0; index < instance->thisClass->numInstanceFields; ++index) {

						// If any look like they might be references
						HeapPointer heapPointer = instance->instField[index].pval;
						if (isHeapPointer(heapPointer)) {

							// mark them
							mark(heapPointer);
						}

					}
				}

			} break;

			case CODE_STRG: {

				// I dunno dude. Doesn't look like String has any heap references.
				// TODO Do we still need to get the String class?
			} break;

			case CODE_SBLD: {

				// Ditto StringBuilder.
				// TODO Do we still need to get the StringBuilder class?
			} break;
		}
	}
}

/*
 * Checks if a block pointer is in the free list
 */
int isInFreeList(uint8_t* block) {

	int offset;
	uint8_t* freeBlock;

	offset = offsetToFirstBlock;

	while (offset >= 0) {

		freeBlock = HeapStart + offset;

		if (freeBlock == block)
			return 1;

		offset = ((FreeStorageBlock*)freeBlock)->offsetToNextBlock;
	}

	return 0;
}

/*
 * Finds the previous block in the free list
 * returns null if there is no previous block
 */
uint8_t* findPreviousBlock(uint8_t* blockToFind) {

	uint8_t *previousBlock = HeapStart + offsetToFirstBlock,
		*nextBlock;

	while (previousBlock) {

		nextBlock = getNextBlock(previousBlock);

		if (nextBlock == blockToFind)
			return previousBlock;

		previousBlock = nextBlock;
	}

	return 0;
}

/*
 * Merges consecutive blocks in the free list.
 */
void mergeFreeList() {

	int previousBlockExists;
	int32_t offsetToNextBlock;
	uint8_t *firstBlock, *secondBlock, *previousBlock;

	// Okay. Let's see.
	firstBlock = HeapStart;
	while (firstBlock < HeapEnd) {

		// If the current block is free
		if (isInFreeList(firstBlock)) {

			secondBlock = firstBlock + blockSizeFromBlockPointer(firstBlock);

			// and the next block is free
			while (isInFreeList(secondBlock)) {

				// basically, we gotta take the second block outta the list
				// So, like, hook the previous block up to the next one
				// and the next one up to the previous one
				// Getting the next block is easy
				offsetToNextBlock = getBlockOffset(secondBlock);

				// But we may or may not have a previous block
				// since the block we're looking at could be the head of the list
				previousBlock = findPreviousBlock(secondBlock);
				previousBlockExists = (previousBlock != 0);

				// If the previous block exists, set it to the next block
				if (previousBlockExists) {

					setBlockOffset(previousBlock, offsetToNextBlock);
				}

				// Otherwise, set the next block to the head of the list
				else {

					offsetToFirstBlock = offsetToNextBlock;
				}
				
				// Add the second block to the first
				*blockSizePointerFromBlockPointer(firstBlock) += blockSizeFromBlockPointer(secondBlock);

				// And continue with the next block
				secondBlock = firstBlock + blockSizeFromBlockPointer(firstBlock);
			}
		}

		firstBlock += blockSizeFromBlockPointer(firstBlock);
	}
}

/* 
 * Sweeps marked blocks of heap into Free List 
 */ 
void sweep() { 
	uint8_t* blockPointer = HeapStart;
	HeapPointer heapPointer;

	while (blockPointer < HeapEnd) {
		heapPointer = heapPointerFromBlockPointer(blockPointer);

		char kind[5];
		readKind(heapPointer, kind);

		if (!markBitIsSet(heapPointer)) {

			// Make sure it's not in the free list
			// so we can avoid cycles
			if (!isInFreeList(blockPointer)) {

				MyHeapFree(REAL_HEAP_POINTER(heapPointer));
			}
		}
		else {

			unsetMarkBit(heapPointer);
		}

		blockPointer += blockSizeFromBlockPointer(blockPointer);
	}

	mergeFreeList();
}

/*
 * Prints a DataItem
 */
void printDataItem(DataItem* item) {

	printf("{i: %i, p: %p (0x%x)}\n",
			item->ival,
			REAL_HEAP_POINTER(item->pval),
			item->pval);
}

/*
 * Prints the marked heap
 */
void printMarkedHeap() {
	uint8_t* blockPointer = HeapStart;
	HeapPointer heapPointer;
	int i = 0;

	printf("\n--- printing marked heap (%p to %p) ---\n", HeapStart, HeapEnd);
	while (blockPointer < HeapEnd) {
		heapPointer = heapPointerFromBlockPointer(blockPointer);

		char kind[5];
		readKind(heapPointer, kind);

		printf("%i:\t%p (%i bytes)\t-\t%s - %s\n",
				i,
				blockPointer,
				blockSizeFromBlockPointer(blockPointer),
				kind,
				(markBitIsSet(heapPointer)? "marked" : "not marked"));

		blockPointer += blockSizeFromBlockPointer(blockPointer);
		++i;
	}
	printf("\n");

}

/*
 * Mark the things in the stack
 */
void markStack() {

	DataItem* stackPointer = JVM_Stack + 1; // skip the bottom of the stack (deadbeef)

	while (stackPointer <= JVM_Top) {

		HeapPointer heapPointer = stackPointer->pval;
		int wasHeapPointer = isHeapPointer(heapPointer);

		if (wasHeapPointer) {
			mark(heapPointer);
		}

		++stackPointer;
	}
}

/*
 * Mark the classes in the heap
 */
void markClasses() {

	uint8_t* blockPointer = HeapStart;
	HeapPointer heapPointer;

	while (blockPointer < HeapEnd) {

		heapPointer = heapPointerFromBlockPointer(blockPointer);

		if (getKind(heapPointer) == CODE_CLAS) {

			mark(heapPointer);
		}

		blockPointer += blockSizeFromBlockPointer(blockPointer);
	}

}

/* This implements garbage collection.
   It should be called when
   (a) MyAlloc cannot satisfy a request for a block of memory, or
   (b) when invoked by the call System.gc() in the Java program.
   */
void gc() {
	gcCount++;

	markStack();
	markClasses();
	mark(MAKE_HEAP_REFERENCE(Fake_System_Out)); // Uh, a special case I guess

	sweep();
}


/* 
 * Report on heap memory usage 
 */
void PrintHeapUsageStatistics() {
	printf("\nHeap Usage Statistics\n=====================\n\n");
	printf("  Number of blocks allocated = %d\n", numAllocations);
	if (numAllocations > 0) {
		float avgBlockSize = (float)totalBytesRequested / numAllocations;
		float avgSearch = (float)searchCount / numAllocations;
		printf("  Average size of allocated blocks = %.2f\n", avgBlockSize);
		printf("  Average number of blocks checked = %.2f\n", avgSearch);
	}
	printf("  Number of garbage collections = %d\n", gcCount);
	if (gcCount > 0) {
		float avgRecovery = (float)totalBytesRecovered / gcCount;
		printf("  Total storage reclaimed = %ld\n", totalBytesRecovered);
		printf("  Total number of blocks reclaimed = %d\n", totalBlocksRecovered);
		printf("  Average bytes recovered per gc = %.2f\n", avgRecovery);
	}
}

/*
 * Adjust minAddr and maxAddr to contain pointer p
 * QUEST - Is there a more apt word than 'contain' - Thought I saw something in FlashPunk...
 */
static void *trackHeapArea( void *p ) {
	if (p > maxAddr)
		maxAddr = p;
	if (p < minAddr)
		minAddr = p;
	return p;
}

/*
 * Allocate one chunk 'o memory of given size
 */
void *SafeMalloc( int size ) {
	return SafeCalloc(1,size);
}

/*
 * Wraps calloc(n,size) but tracks heap area as well
 */
void *SafeCalloc( int ncopies, int size ) {
	void *result;
	result = calloc(ncopies,size);
	if (result == NULL) {
		fprintf(stderr, "Fatal error: memory request cannot be satisfied\n");
		exit(1);
	}
	trackHeapArea(result);
	return result;    
}

/*
 * Copy a string into a newly created string
 */
char *SafeStrdup( char *s ) {
	char *r;
	int len;

	len = (s == NULL)? 0 : strlen(s);
	r = SafeMalloc(len+1);
	if (len > 0)
		strcpy(r,s);
	return r;
}

/*
 * Free byte-aligned non-NULL pointers, that are in range
 * QUEST: What would you call the MAGIC_NUMBER to replace 0x7 ?
 *          i.e. 0x7 is a concept, not the number 7
 */
void SafeFree( void *p ) {
	if (p == NULL || ((int)p & 0x7) != 0) {
		fprintf(stderr, "Fatal error: invalid parameter passed to SafeFree\n");
		fprintf(stderr, "    The address was NULL or misaligned\n");
		abort();
	}
	if (p >= minAddr && p <= maxAddr)
		free(p);
	else {
		fprintf(stderr, "Fatal error: invalid parameter passed to SafeFree\n");
		fprintf(stderr, "     The memory was not allocated by SafeMalloc\n");
		abort();
	}
}
