

#include <stdlib.h>
#include "r4300.h"
#include "Recomp-Cache.h"

typedef struct _meta_node {
	unsigned int  addr;
	PowerPC_func* func;
	unsigned int  size;
} CacheMetaNode;

static int cacheSize;

#define HEAP_CHILD1(i) ((i<<1)+1)
#define HEAP_CHILD2(i) ((i<<1)+2)
#define HEAP_PARENT(i) ((i-1)>>2)

#define INITIAL_HEAP_SIZE (64)
static unsigned int heapSize = 0;
static unsigned int maxHeapSize = 0;
static CacheMetaNode** cacheHeap = NULL;

static void heapSwap(int i, int j){
	CacheMetaNode* t = cacheHeap[i];
	cacheHeap[i] = cacheHeap[j];
	cacheHeap[j] = t;
}

static void heapUp(int i){
	// While the given element is out of order
	while(i && cacheHeap[i]->func->lru < cacheHeap[HEAP_PARENT(i)]->func->lru){
		// Swap the child with its parent
		heapSwap(i, HEAP_PARENT(i));
		// Consider its new position
		i = HEAP_PARENT(i);
	}
}

static void heapDown(int i){
	// While the given element is out of order
	while(1){
		unsigned int lru = cacheHeap[i]->func->lru;
		CacheMetaNode* c1 = (HEAP_CHILD1(i) < heapSize) ?
		                     cacheHeap[HEAP_CHILD1(i)] : NULL;
		CacheMetaNode* c2 = (HEAP_CHILD2(i) < heapSize) ?
		                     cacheHeap[HEAP_CHILD2(i)] : NULL;
		// Check against the children, swapping with the min if parent isn't
		if(c1 && lru > c1->func->lru &&
		   (!c2 || c1->func->lru < c2->func->lru)){
			heapSwap(i, HEAP_CHILD1(i));
			i = HEAP_CHILD1(i);
		} else if(c2 && lru > c2->func->lru){
			heapSwap(i, HEAP_CHILD2(i));
			i = HEAP_CHILD2(i);
		} else break;
	}
}

static void heapify(void){
	int i;
	for(i=1; i<heapSize; ++i) heapUp(i);
}

static void heapPush(CacheMetaNode* node){
	if(heapSize == maxHeapSize){
		maxHeapSize = 3*maxHeapSize/2 + 10;
		cacheHeap = realloc(cacheHeap, maxHeapSize*sizeof(void*));
	}
	// Simply add it to the end of the heap
	// No need to heapUp since its the most recently used
	cacheHeap[heapSize++] = node;
}

static CacheMetaNode* heapPop(void){
	heapSwap(0, --heapSize);
	heapDown(0);
	return cacheHeap[heapSize];
}

static void unlink_func(PowerPC_func* func, unsigned int code_size){
	//start_section(UNLINK_SECTION);
	
	// Remove any incoming links to this func
	PowerPC_func_link_node* link, * next_link;
	for(link = func->links_in; link != NULL; link = next_link){
		next_link = link->next;
		
		GEN_BLR(*link->branch, 1); // Set the linking branch to blrl
		//DCFlushRange(link->branch, sizeof(PowerPC_instr));
		//ICInvalidateRange(link->branch, sizeof(PowerPC_instr));
		
		remove_func(&link->func->links_out, func);
		free(link);
	}
	func->links_in = NULL;
	
	// Remove any references to outgoing links from this func
	void remove_outgoing_links(PowerPC_func_node** node){
		if(!*node) return;
		if((*node)->left) remove_outgoing_links(&(*node)->left);
		if((*node)->right) remove_outgoing_links(&(*node)->right);

		// Remove any links this function has which point in the code
		PowerPC_func_link_node** link, ** next;
		for(link = &(*node)->function->links_in; *link != NULL; link = next){
			next = &(*link)->next;
			if((*link)->branch >= func->code &&
			   (*link)->branch < func->code + code_size/sizeof(PowerPC_instr)){
				PowerPC_func_link_node* tmp = (*link)->next;
				free(*link);
				*link = tmp;
				next = link;
			}
		}
		free(*node); // Free the PowerPC_func_node*
	}
	remove_outgoing_links(&func->links_out);
	func->links_out = NULL;
	
	//end_section(UNLINK_SECTION);
}

static void free_func(PowerPC_func* func, unsigned int addr,
                      unsigned int code_size){
	// Free the code associated with the func
	free(func->code);
	free(func->code_addr);
	// Remove any holes into this func
	PowerPC_func_hole_node* hole, * next;
	for(hole = func->holes; hole != NULL; hole = next){
		next = hole->next;
		free(hole);
	}
	
	// Remove any pointers to this code
	PowerPC_block* block = blocks[addr>>12];
	remove_func(&block->funcs, func);
	// Remove func links
	unlink_func(func, code_size);

	free(func);
}

static inline void update_lru(PowerPC_func* func){
	static unsigned int nextLRU = 0;
	/*if(func->lru != nextLRU-1)*/ func->lru = nextLRU++;
	
	if(!nextLRU){
		// Handle nextLRU overflows
		// By heap-sorting and assigning new LRUs
		heapify();
		// Since you can't do an in-place min-heap ascending-sort
		//   I have to create a new heap
		CacheMetaNode** newHeap = malloc(maxHeapSize * sizeof(CacheMetaNode*));
		int i, savedSize = heapSize;
		for(i=0; heapSize > 0; ++i){
			newHeap[i] = heapPop();
			newHeap[i]->func->lru = i;
		}
		free(cacheHeap);
		cacheHeap = newHeap;
		
		nextLRU = heapSize = savedSize;
	}
}

static void release(int minNeeded){
	// Frees alloc'ed blocks so that at least minNeeded bytes are available
	int toFree = minNeeded * 2; // Free 2x what is needed
	printf("RecompCache requires ~%dkB to be released\n", minNeeded/1024);
	// Restore the heap properties to pop the LRU
	heapify();
	// Release nodes' memory until we've freed enough
	while(toFree > 0 && cacheSize){
		// Pop the LRU to be freed
		CacheMetaNode* n = heapPop();
		// Free the function it contains
		free_func(n->func, n->addr, n->size);
		toFree    -= n->size;
		cacheSize -= n->size;
		// And the cache node itself
		free(n);
	}
}

void RecompCache_Alloc(unsigned int size, unsigned int address, PowerPC_func* func){
	CacheMetaNode* newBlock = malloc( sizeof(CacheMetaNode) );
	newBlock->addr = address;
	newBlock->size = size;
	newBlock->func = func;
	
	int num_instrs = (func->end_addr ? func->end_addr : 0x10000) - func->start_addr;
	if(cacheSize + size + num_instrs * sizeof(void*) > RECOMP_CACHE_SIZE)
		// Free up at least enough space for it to fit
		release(cacheSize + size + num_instrs * sizeof(void*) - RECOMP_CACHE_SIZE);
	
	// We have the necessary space for this alloc, so just call malloc
	cacheSize += size;
	newBlock->func->code = malloc(size);
	newBlock->func->code_addr = malloc(num_instrs * sizeof(void*));
	// Add it to the heap
	heapPush(newBlock);
	// Make this function the LRU
	update_lru(func);
}

void RecompCache_Realloc(PowerPC_func* func, unsigned int new_size){
	// I'm not worrying about maintaining the cache size here for now
	func->code = realloc(func->code, new_size);
	unsigned int old_size;
	int i;
	for(i=heapSize-1; i>=0; --i){
		if(func == cacheHeap[i]->func){
			old_size = cacheHeap[i]->size;
			cacheSize += new_size - old_size;
			cacheHeap[i]->size = new_size;
			break;
		}
	}
	
	// Remove any func links since the code has changed
	unlink_func(func, old_size);
}

void RecompCache_Free(unsigned int addr){
	int i;
	CacheMetaNode* n = NULL;
	// Find the corresponding node
	for(i=heapSize-1; i>=0; --i){
		if(addr == cacheHeap[i]->addr){
			n = cacheHeap[i];
			// Remove from the heap
			heapSwap(i, --heapSize);
			// Free n's func
			free_func(n->func, addr, n->size);
			cacheSize -= n->size;
			// Free the cache node
			free(n);
			break;
		}
	}
}

void RecompCache_Update(PowerPC_func* func){
	update_lru(func);
}

void RecompCache_Link(PowerPC_func* src_func, PowerPC_instr* src_instr,
                      PowerPC_func* dst_func, PowerPC_instr* dst_instr){
	//start_section(LINK_SECTION);
	
	// Setup book-keeping
	// Create the incoming link info
	PowerPC_func_link_node* fln = malloc(sizeof(PowerPC_func_link_node));
	while(!fln){
		release(sizeof(PowerPC_func_link_node));
		fln = malloc(sizeof(PowerPC_func_link_node));
	}
	fln->branch = src_instr;
	fln->func = src_func;
	fln->next = dst_func->links_in;
	dst_func->links_in = fln;
	// Create the outgoing link info
	insert_func(&src_func->links_out, dst_func);
	
	// Actually link the funcs
	GEN_B(*src_instr, (PowerPC_instr*)dst_instr-src_instr, 0, 0);
	//DCFlushRange(src_instr, sizeof(PowerPC_instr));
	//ICInvalidateRange(src_instr, sizeof(PowerPC_instr));
	
	//end_section(LINK_SECTION);
}

