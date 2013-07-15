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
static long totalBytesRequested = 0;
static int numAllocations = 0;
static int gcCount = 0;
static long totalBytesRecovered = 0;
static int totalBlocksRecovered = 0;
static int searchCount = 0;

static void *maxAddr = NULL;    // used by SafeMalloc, etc
static void *minAddr = NULL;


/* Allocate the Java heap and initialize the free list */
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
 * Gets the heap pointer from a block pointer
 */
uint8_t* heapPointerFromBlockPointer(uint8_t* blockPointer) {

    return blockPointer + BLOCK_SIZE_SIZE;
}

/*
 * Gets the block size from a block pointer
 */
uint32_t blockSizeFromBlockPointer(uint8_t* blockPointer) {

	return *((uint32_t*)blockPointer);
}

/*
 * Check whether a pointer actually points to a value in the heap
 */
int pointsToHeapObject(uint8_t* possibleHeapPointer) {

	uint8_t* blockPointer = HeapStart;
	uint8_t* heapPointer;

	while (blockPointer < HeapEnd) {

		heapPointer = heapPointerFromBlockPointer(blockPointer);

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
	HeapPointer heapPointer = MAKE_HEAP_REFERENCE((uint8_t*)blockPtr + sizeof(blockPtr->size));
	printf("returning %p (min is %p and max is %p) - is that a heap pointer? %s\n",
			newBlockPtr,
			HeapStart,
			HeapEnd,
			(isHeapPointer(heapPointer)? "yes" : "no"));
    }
    blockPtr->offsetToNextBlock = 0;  /* remove this info from the returned block */
    totalBytesRequested += minSizeNeeded;
    numAllocations++;
    return (uint8_t*)blockPtr + sizeof(blockPtr->size);
}


/* When garbage collection is implemented, this function should never
   be called from outside the current file.
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
    if (blockSize < MINBLOCKSIZE || (p1 + blockSize) >= HeapEnd || (blockSize & 3) != 0) {
        fprintf(stderr, "bad call to MyHeapFree -- invalid block\n");
        exit(1);
    }
    /* link the block into the free list at the front */
    blockPtr = (FreeStorageBlock*)p1;
    blockPtr->offsetToNextBlock = offsetToFirstBlock;
    offsetToFirstBlock = p1 - HeapStart;
}

/*
 * The number of items in the JVM stack
 */
int jvmStackHeight() {

	return (int)(JVM_Top - JVM_Stack);
}

uint32_t* blockSizePtrFromHeapPtr(HeapPointer heapPointer) {

	// Uh, let's see
	// Move back from heap pointer the size of, uh, the size field
	// then return that pointer interpreted as a pointer to the size field
	return (uint32_t*)((uint8_t*)REAL_HEAP_POINTER(heapPointer) - sizeof(uint32_t));
}

/*
 * Checks if the mark bit is set
 */
int markBitIsSet(HeapPointer heapPointer) {

	return *blockSizePtrFromHeapPtr(heapPointer) & MARK_SIZE_BIT;
}

/*
 * Sets the mark bit
 */
void setMarkBit(HeapPointer heapPointer) {

	*blockSizePtrFromHeapPtr(heapPointer) |= MARK_SIZE_BIT;
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

				ArrayOfRef array = *((ArrayOfRef*)REAL_HEAP_POINTER(heapPointer));
				int index = 0;
				for (index = 0; index < array.size; ++index) {

					mark(array.elements[index]);
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

				ClassInstance instance = *((ClassInstance*)REAL_HEAP_POINTER(heapPointer));

				// Mark the thing's class
				markClassType(instance.thisClass);

				// Then mark all of the instance's references
				int index = 0;
				for (index = 0; index < instance.thisClass->numInstanceFields; ++index) {

					// If any look like they might be references
					HeapPointer heapPointer = instance.instField[index].pval;
					if (isHeapPointer(heapPointer)) {

						// mark them
						mark(heapPointer);
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
 * Sweeps marked blocks of heap into Free List
 */
void sweep() {
    FreeStorageBlock* heapPointer = HeapStart;
    while (heapPointer < HeapEnd) {
        if (heapPointer->size & MARK_SIZE_BIT == 0) {
            MyHeapFree(heapPointer);    
        }
        else {
            heapPointer->size & ~MARK_SIZE_BIT;
        }
        // heapPointer = heapPointer + size + offset + data
        heapPointer = heapPointer + sizeof(uint32_t) + sizeof(int32_t) + heapPointer->size;
    }
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


/* This implements garbage collection.
   It should be called when
   (a) MyAlloc cannot satisfy a request for a block of memory, or
   (b) when invoked by the call System.gc() in the Java program.
*/
void gc() {
    gcCount++;

    DataItem* stackPointer = JVM_Stack + 1; // skip the bottom of the stack (deadbeef)
    while (stackPointer <= JVM_Top) {

	    printf("stack pointer is %p, stack start is %p, top is %p\n", stackPointer, JVM_Stack, JVM_Top);
	    HeapPointer heapPointer = stackPointer->pval;
	    int wasHeapPointer = isHeapPointer(heapPointer);

	    printf("heapPointer is %p and HeapStart is %p and HeapEnd is %p\n",
			    REAL_HEAP_POINTER(heapPointer), HeapStart, HeapEnd);

	    printDataItem(stackPointer);

	    printf("%p is %sa heap pointer\n",
			    REAL_HEAP_POINTER(heapPointer),
			    (wasHeapPointer? "":"not "));

	    if (wasHeapPointer) {

		    char kind[5];
		    readKind(heapPointer, kind);
		    printf("Kind is %s\n", kind);
		    mark(heapPointer);
	    }
	    
	    ++stackPointer;
    }
    sweep();
}


/* Report on heap memory usage */
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


static void *trackHeapArea( void *p ) {
    if (p > maxAddr)
        maxAddr = p;
    if (p < minAddr)
        minAddr = p;
    return p;
}


void *SafeMalloc( int size ) {
    return SafeCalloc(1,size);
}


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


char *SafeStrdup( char *s ) {
    char *r;
    int len;

    len = (s == NULL)? 0 : strlen(s);
    r = SafeMalloc(len+1);
    if (len > 0)
        strcpy(r,s);
    return r;
}


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
