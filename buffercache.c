/* 
Implement a Buffer Cache that allows concurrent read/write access to buffers in the cache.
A buffer is a chunk of memory that is associated with a unique buffer id.
The cache size is bounded and should grow dynamically to use memory efficiently.

Clients can add buffers to the cache via the addToCache API.
The acquireBuffer* APIs allow clients to access the buffers that are in the cache.
*/


/* 
								Design Assumptions :

	1) When a buffer in cache is being accessed to be written to or read from, no new buffers can be added in Cache.
	   But just after required buffer is accessed, the control is returned, thus enabling addition of a buffer.
	   I considered adding a buffer in cache as a write to the cache (as it may evict a buffer which is being accessed for reading or writing 
	   by some process) and read and write access of the buffer from the cache as a read operation on the cache as it does not change the structure of the cache.
	   I use the readers-writers solution so that there is no change in structure of the cache, when we are reading from/writing to the buffer.

	2) The client uses aquireBufferForRead only for reading buffer, and aquireBufferForWrite only for modifying buffer, 
	   as the readers' part of the solution is implemented in aquireBufferForRead and the writers' part of the solution is 
	   implemented in aquireBufferForWrite and there is no way (without changing function prototype) to ensure that the client is restricted to modify when given
	   read access over the buffer. We may change the prototype of aquireBufferForRead to "const void* aquireBufferForRead(int id)", to ensure the user doesn't 
	   modify the buffer returned.

	3) The design supports multiple readers and one writer at a time over any buffer. Preference has been given to the readers, 
	   when both reader or writer can enter the critical section.
*/

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

/* Maximum number of buffers in the cache */
// #define MAX_BUFFERS (1<<30)
#define MAX_BUFFERS 2
#define MOD_HASH (1<<8)
#define BUFFER_CACHE_KEY 11011010

// Each node in the doubly linked list of the LRU cache.
typedef struct BufferNode {
	int bufferId;
	char readOrWriteFlag;
	int semWrtId, semMutexId;
	int readCount;
	void* bufferAddr;
	struct BufferNode *parentNode, *nextNode;
} bufferNode;

// Each node in the hash Map of the LRU cache, storing pointers to bufferNode.
typedef struct HashNode {
	int bufferId;
	bufferNode *bufferCacheAddr;
	struct HashNode* nextNode;
} hashNode;

// HashMap of size MOD_HASH, each bucket can contain many hashNodes with same hash values.
typedef struct HashMap {
	hashNode* buckets[MOD_HASH];
} hashMap;

// Struct for the LRU cache, containing 1 write semaphore, 1 mutex semaphore, 
// number of buffers currently in LRU cache, pointers to top and bottom bufferNodes, one hashMap.
// top of cache implies recently used bufferNode and bottom of cache implies bufferNode in
// LRU cache used before every other bufferNode. The bufferNodes are kept as a doubly linked list.
typedef struct BufferLRUCache {
	int numBuffersInCache;
	int readCount;
	int semWrtId, semMutexId;
	hashMap hashBufferAddrs;
	bufferNode* topBufferNode;
	bufferNode* bottomBufferNode;
} bufferLRUCache;

// This function is used to initialize shared memory for the LRU cache, semaphores and 
// initialize variables of the LRU cache.
void initializeCache()
{
	int bufferCacheId = shmget(BUFFER_CACHE_KEY , sizeof(bufferLRUCache), IPC_CREAT | IPC_EXCL | 0666);
    if (bufferCacheId < 0) {
        perror("shmget");
        return;
    }

    bufferLRUCache* bufferCache = (bufferLRUCache *)shmat(bufferCacheId, NULL, 0);
    if (bufferCache == (bufferLRUCache *) -1) {
        perror("shmat bufferCache");
        return;
    }

    bufferCache->numBuffersInCache = 0;
    bufferCache->topBufferNode = NULL;
    bufferCache->bottomBufferNode = NULL;
    bufferCache->readCount = 0;
    bufferCache->semMutexId = 1;
    bufferCache->semWrtId = 2;

    int mutexIdSem;
    if ((mutexIdSem = semget(bufferCache->semMutexId, 1, IPC_CREAT | IPC_EXCL | 0666)) == -1) {
		perror("semget"); 
		exit(1); 
	}
	semctl(mutexIdSem, 0, SETVAL, 1);

    int wrtIdSem;
    if ((wrtIdSem = semget(bufferCache->semWrtId, 1, IPC_CREAT | IPC_EXCL | 0666)) == -1) {
		perror("semget"); 
		exit(1); 
	}
	semctl(wrtIdSem, 0, SETVAL, 1);
	int i;
    for (i = 0;i < MOD_HASH; i++) {
    	bufferCache->hashBufferAddrs.buckets[i] = NULL;
    }
}

// function to release shared memory of LRU cache and semaphores used by it.
void releaseCache()
{
	int bufferCacheId = shmget(BUFFER_CACHE_KEY , sizeof(bufferLRUCache), 0666);
    if (bufferCacheId < 0) {
        perror("shmget");
    }

	if(shmctl(bufferCacheId,IPC_RMID,0) < 0) {
		perror("shmctl remove bufferCache");
	}

	int mutexIdSem;
    if ((mutexIdSem = semget(1, 1, 0666)) == -1) {
		perror("semget"); 
		exit(1); 
	}
	semctl(mutexIdSem, 0, SETVAL, 1);

    int wrtIdSem;
    if ((wrtIdSem = semget(2, 1, 0666)) == -1) {
		perror("semget"); 
		exit(1); 
	}
	semctl(wrtIdSem, 0, SETVAL, 1);
}

// This function corresponds to signal function of a semaphore.
void signal(int semId) {
    struct sembuf sop;
    sop.sem_num = 0;
    sop.sem_op = 1;
    sop.sem_flg = 0;
    semop(semId, &sop, 1);
}

// This function corresponds to wait function of a semaphore.
void wait(int semId) {
    struct sembuf sop;
    sop.sem_num = 0;
    sop.sem_op = -1;
    sop.sem_flg = 0;
    semop(semId, &sop, 1);
}


/* Adds a new buffer to the Buffer Cache.
 * The number of buffers in the cache must always be bounded by MAX_BUFFERS.
 * A suitable victim buffer must be evicted if the number of buffers exceeds MAX_BUFFERS.
 *
 * Parameters:
 * id - Buffer id. This function is a no-op if a duplicate id already exists in the cache.
 * buffer - Memory address of a buffer.
 *
 * Returns - NONE
 */
void addToCache(int id, void *buffer)
{
  //TODO: implementation of addToCache
	// Buffer Cache implemented as a shared memory to allow all processes to access it.
	int bufferCacheId = shmget(BUFFER_CACHE_KEY , sizeof(bufferLRUCache), 0666);
    if (bufferCacheId < 0) {
        perror("shmget bufferCache");
        return;
    }

    bufferLRUCache* bufferCache = (bufferLRUCache *)shmat(bufferCacheId, NULL, 0);
    if (bufferCache == (bufferLRUCache *) -1) {
        perror("shmat bufferCache");
        return;
    }

    // using readers-writers problem solution with preference to readers for accessing LRUCache.
    int wrtIdSem;
    if ((wrtIdSem = semget(bufferCache->semWrtId, 1, 0666)) == -1) {
		perror("semget wrt bufferCache"); 
		exit(1); 
	}

    wait(wrtIdSem);			// wait to access write semaphore, to ensure no process is reading from LRU cache.

    // hash function used x = y % MOD_HASH, at most, total Buffers / MOD_HASH nodes with same hash value.
    int hashVal = (id % MOD_HASH);
    printf("id : %d, hashVal : %d added in cache\n", id, hashVal);
    fflush(stdout);
    hashNode* bucketPtr = bufferCache->hashBufferAddrs.buckets[hashVal];
    hashNode* parentPtr = NULL;
    // Check if buffer with same bufferId already present in hashMap of cache or not, if yes, return with no-op
    // else find the pointer to the last node in the hashMap corresponding to the hash of the buffer.
    while (bucketPtr != NULL) {
    	if (bucketPtr->bufferId == id) {
    		// buffer with duplicate bufferId found, perform no-op.
    		return;
    	}
    	parentPtr = bucketPtr;
    	bucketPtr = bucketPtr->nextNode;
    }

    // Initialize the buffer Node to be stored in cache.
    bufferNode* newBufferNode = (bufferNode *)malloc(sizeof(bufferNode));
    newBufferNode->bufferId = id;
    newBufferNode->bufferAddr = buffer;
    newBufferNode->semMutexId = 2 * id + 3;		// ensure the mutex Ids are unique. Mutex for read/write on buffer.
    newBufferNode->semWrtId = 2 * id + 4;		// ensure the wrt Ids are unique. Wrt semaphore to ensure many reads/one write on buffer.
    int mutexIdBuf;
    if ((mutexIdBuf = semget(newBufferNode->semMutexId, 1, IPC_CREAT | 0666)) == -1) {
		perror("semget mutex buf"); 
		exit(1); 
	}
	semctl(mutexIdBuf, 0, SETVAL, 1);			// mutex initialized to 1, to allow access to first requesting process.
    int wrtIdBuf;
    if ((wrtIdBuf = semget(newBufferNode->semWrtId, 1, IPC_CREAT | 0666)) == -1) {
		perror("semget wrt buf"); 
		exit(1); 
	}
	semctl(wrtIdBuf, 0, SETVAL, 1);				// wrt initialized to 1, to allow either read or write to first requesting process.	
    newBufferNode->readCount = 0;			// initialize readCount, keeping count of number of processes reading from buffer.

    // Case : no buffers in cache, initialize top ptr and bottom ptr to current node.
    if (bufferCache->numBuffersInCache == 0) {
    	newBufferNode->parentNode = NULL;
    	newBufferNode->nextNode = NULL;
    	bufferCache->topBufferNode = newBufferNode;
    	bufferCache->bottomBufferNode = newBufferNode;
    	bufferCache->numBuffersInCache += 1;
    }
    // Case : numBuffers inside cache < MAX_BUFFERS, insert in the top of the cache.
    else if (bufferCache->numBuffersInCache < MAX_BUFFERS) {
    	newBufferNode->parentNode = NULL;
    	newBufferNode->nextNode = bufferCache->topBufferNode;
    	bufferCache->topBufferNode->parentNode = newBufferNode;
    	bufferCache->topBufferNode = newBufferNode;
    	bufferCache->numBuffersInCache += 1;
    }
    // Case : numBuffers inside cache = MAX_BUFFERS, evict node at bottom of cache, and 
    // insert in the top of the cache.
    else {
    	// evict Node present in the bottom based on Least Recently Used strategy.
    	bufferNode *evictNode = bufferCache->bottomBufferNode;
    	// update bottom ptr of cache to parent of evictNode. update next ptr of parent node of evictNode.
    	if (evictNode != NULL) {
    		printf("Node evicted : %d\n", evictNode->bufferId);
    		fflush(stdout);	
    	}
    	bufferCache->bottomBufferNode = evictNode->parentNode;
    	evictNode->parentNode->nextNode = NULL;
    	int evictBufferId = evictNode->bufferId;
    	free(evictNode);
    	

    	// remove entry corresponding to evictNode from hashMap of the cache.
		hashNode *evictBucketPtr = bufferCache->hashBufferAddrs.buckets[(evictBufferId % MOD_HASH)];
		hashNode *evictParentPtr = NULL;
		while (evictBucketPtr != NULL) {
	    	// Find hash Node with same bufferId as evictNode. If parent is Null, then this node is in the front of hashBucket.
	    	// point the front of bucket to the next node of  hash Node corresponding to evictNode.
	    	// else, update the parentNode's next pointer to hash Node to be evicted's next pointer.
	    	if (evictBucketPtr->bufferId == evictBufferId) {
	    		if (evictParentPtr != NULL) {
	    			evictParentPtr->nextNode = evictBucketPtr->nextNode;
	    		}
	    		else {
	    			bufferCache->hashBufferAddrs.buckets[(evictBufferId % MOD_HASH)] = evictBucketPtr->nextNode;
	    		}
	    		free(evictBucketPtr);
	    		break;
	    	}
	    	// Keep Track of the parent Node.
	    	evictParentPtr = evictBucketPtr;
	    	evictBucketPtr = evictBucketPtr->nextNode;
	    }

    	// insert the new buffer in the top of the cache.
	    newBufferNode->parentNode = NULL;
    	newBufferNode->nextNode = bufferCache->topBufferNode;
    	bufferCache->topBufferNode->parentNode = newBufferNode;
    	bufferCache->topBufferNode = newBufferNode;

    }

    // Update hashMap of cache to add a hash of the new buffer in cache.
    hashNode* newHashNode = (hashNode *)malloc(sizeof(hashNode));
    newHashNode->bufferId = id;
    newHashNode->bufferCacheAddr = newBufferNode;
    newHashNode->nextNode = NULL;
    if (parentPtr == NULL) {
    	bufferCache->hashBufferAddrs.buckets[hashVal] = newHashNode;
    }
    else {
    	parentPtr->nextNode = newHashNode;
    }

    signal(wrtIdSem);

}

// Updates LRU Cache to bring the currentNode to the top of the Cache as it has been accessed now.
void updateLRUCache(bufferLRUCache* bufferCache, bufferNode* updatedBufferNode)
{

    // Case 1 : this node is at the top, so no requirement of any changes in the structure of LRU cache.
    if (bufferCache->topBufferNode == updatedBufferNode) {
    	return;
    }

    // Case 2 : this node is somewhere in the middle, so parent of this node cannot be Null.
    if (updatedBufferNode->nextNode != NULL) {
    	// next node's new parent is the parent of the currentNode.
    	updatedBufferNode->nextNode->parentNode = updatedBufferNode->parentNode;
    	// parent node's new next is the next of the currentNode.
    	updatedBufferNode->parentNode->nextNode = updatedBufferNode->nextNode;
    	
    }
    // Case 3 : this is the bottom node (LRU Cache has > 1 nodes, or else Case 1 would have been triggered), 
    // update bottom ptr of cache.
    else {
    	// parentNode is not NULL, as this is not the top node.
    	bufferCache->bottomBufferNode = updatedBufferNode->parentNode;
    	// parentNode becomes bottom Node, so nextNode of parent is Null.
    	updatedBufferNode->parentNode->nextNode = NULL;
    		
    }

    updatedBufferNode->parentNode = NULL;				// currentNode becomes top, so new parent becomes Null.
	updatedBufferNode->nextNode = bufferCache->topBufferNode;	// old top node becomes next node of currentNode.
	bufferCache->topBufferNode->parentNode = updatedBufferNode;	// new parent of old top node is currentNode.
	bufferCache->topBufferNode = updatedBufferNode;				// currentNode is pointed to by the top of LRU cache.

}

/* Get access to a buffer in the cache for read operations only.
 * If a client needs to modify the contents of the buffer, it should use acquireBufferForWrite instead.
 *
 * Parameters:
 * id - Buffer id.
 *
 * Returns: The buffer address for the matching id, if the id does not exist in the cache return NULL.
 */
void *acquireBufferForRead(int id)
{
  //TODO: implementation of acquireBufferForRead.
	// Open Buffer Cache stored as a shared memory.
	int bufferCacheId = shmget(BUFFER_CACHE_KEY , sizeof(bufferLRUCache), 0666);
    if (bufferCacheId < 0) {
        perror("shmget bufferCache");
        return;
    }

    bufferLRUCache* bufferCache = (bufferLRUCache *)shmat(bufferCacheId, NULL, 0);
    if (bufferCache == (bufferLRUCache *) -1) {
        perror("shmat bufferCache");
        return;
    }

    int mutexIdSem;
    if ((mutexIdSem = semget(bufferCache->semMutexId, 1, 0666)) == -1) {
		perror("semget mutex bufferCache"); 
		exit(1); 
	}

    int wrtIdSem;
    if ((wrtIdSem = semget(bufferCache->semWrtId, 1, 0666)) == -1) {
		perror("semget wrt bufferCache"); 
		exit(1); 
	}

	// using readers solution for readers-writers problem with preference to readers for LRU cache.
	// wait for being granted access to read from the LRU cache.
    wait(mutexIdSem);					// mutex used for accessing shared memory readCount.
    bufferCache->readCount += 1;
	if (bufferCache->readCount == 1) {
    	wait(wrtIdSem);					// wait for a process which is writing to the LRU cache (adding or changing the structure of the Cache).
    }
    signal(mutexIdSem);					// leave control of readCount.

    int hashVal = (id % MOD_HASH);
    hashNode* bucketPtr = bufferCache->hashBufferAddrs.buckets[hashVal];
    // Check if buffer with id present in hashMap of cache or not, if yes, return the buffer.
    // else return NULL.
    while (bucketPtr != NULL) {
    	if (bucketPtr->bufferId == id) {
			bufferNode *requiredBufferNode = bucketPtr->bufferCacheAddr;
			void *buffer = requiredBufferNode->bufferAddr;
		    updateLRUCache(bufferCache, requiredBufferNode);

			int mutexIdBuf;
		    if ((mutexIdBuf = semget(requiredBufferNode->semMutexId, 1, 0666)) == -1) {
				perror("semget mutex requiredBufferNode"); 
				exit(1); 
			}

		    int wrtIdBuf;
		    if ((wrtIdBuf = semget(requiredBufferNode->semWrtId, 1, 0666)) == -1) {
				perror("semget wrt requiredBufferNode"); 
				exit(1); 
			}

			// release control for allowing write operations on LRU cache. See Assumption 1 at the top of code.
		    wait(mutexIdSem);
		    bufferCache->readCount -= 1;
		    if (bufferCache->readCount == 0) {
		    	signal(wrtIdSem);
		    }
		    signal(mutexIdSem);

			// using readers solution for readers-writers problem with preference to readers for buffer access.
		    wait(mutexIdBuf);
		    if (requiredBufferNode->readCount == 0) {
		    	wait(wrtIdBuf);
		    }
		    requiredBufferNode->readCount += 1;
		    signal(mutexIdBuf);

		    return buffer;

    	}
    	bucketPtr = bucketPtr->nextNode;
    }

    // release read access of Cache semaphores when buffer with id not found.
    wait(mutexIdSem);
    bufferCache->readCount -= 1;
    if (bufferCache->readCount == 0) {
    	signal(wrtIdSem);
    }
    signal(mutexIdSem);
    return NULL;

}

/* Get access to a buffer in the cache to modify the buffer's contents.
 *
 * Parameters:
 * id - Buffer id.
 *
 * Returns: The buffer address for the matching id, if the id does not exist in the cache return NULL.
 */

void *acquireBufferForWrite(int id)
{
  //TODO: implementation of acquireBufferForWrite
	// Open Buffer Cache stored as a shared memory.
	int bufferCacheId = shmget(BUFFER_CACHE_KEY , sizeof(bufferLRUCache), 0666);
    if (bufferCacheId < 0) {
        perror("shmget bufferCache");
        return;
    }

    bufferLRUCache* bufferCache = (bufferLRUCache *)shmat(bufferCacheId, NULL, 0);
    if (bufferCache == (bufferLRUCache *) -1) {
        perror("shmat bufferCache");
        return;
    }

    int mutexIdSem;
    if ((mutexIdSem = semget(bufferCache->semMutexId, 1, 0666)) == -1) {
		perror("semget mutex bufferCache"); 
		exit(1); 
	}

    int wrtIdSem;
    if ((wrtIdSem = semget(bufferCache->semWrtId, 1, 0666)) == -1) {
		perror("semget wrt bufferCache"); 
		exit(1); 
	}

	// using readers solution for readers-writers problem with preference to readers.
	// wait for being granted access to read from the LRU cache.
    wait(mutexIdSem);					// mutex used for accessing shared memory readCount.
    bufferCache->readCount += 1;
    if (bufferCache->readCount == 1) {
    	wait(wrtIdSem);					// wait for a process which is writing to the LRU cache (adding or changing the structure of the Cache).
    }
    signal(mutexIdSem);					// leave control of readCount.

    int hashVal = (id % MOD_HASH);
    hashNode* bucketPtr = bufferCache->hashBufferAddrs.buckets[hashVal];
    // Check if buffer with id present in hashMap of cache or not, if yes, return the buffer.
    // else return NULL.
    while (bucketPtr != NULL) {
    	if (bucketPtr->bufferId == id) {
			bufferNode *requiredBufferNode = bucketPtr->bufferCacheAddr;
			void *buffer = requiredBufferNode->bufferAddr;
		    updateLRUCache(bufferCache, requiredBufferNode);
			int wrtIdBuf;
		    if ((wrtIdBuf = semget(requiredBufferNode->semWrtId, 1, 0666)) == -1) {
				perror("semget wrt requiredBufferNode"); 
				exit(1); 
			}

			// release control for allowing write operations on LRU cache. See Assumption 1 at the top of code.
		    wait(mutexIdSem);
		    bufferCache->readCount -= 1;
		    if (bufferCache->readCount == 0) {
		    	signal(wrtIdSem);
		    }
		    signal(mutexIdSem);

		    // using writers solution for readers-writers problem with preference to readers.
		    wait(wrtIdBuf);

		    return buffer;

    	}
    	bucketPtr = bucketPtr->nextNode;
    }

    // release read access of Cache semaphores when buffer with id not found.
    wait(mutexIdSem);
    bufferCache->readCount -= 1;
    if (bufferCache->readCount == 0) {
    	signal(wrtIdSem);
    }
    signal(mutexIdSem);
    return NULL;

}

/* Release a buffer that was previously acquired.
 *
 * Parameters:
 * id - Buffer id.
 *
 * Returns: NONE
 */
void releaseBuffer(int id)
{
  //TODO: implementation of releaseBuffer
	// Open Buffer Cache stored as a shared memory.
	int bufferCacheId = shmget(BUFFER_CACHE_KEY , sizeof(bufferLRUCache), 0666);
    if (bufferCacheId < 0) {
        perror("shmget bufferCache");
        return;
    }

    bufferLRUCache* bufferCache = (bufferLRUCache *)shmat(bufferCacheId, NULL, 0);
    if (bufferCache == (bufferLRUCache *) -1) {
        perror("shmat bufferCache");
        return;
    }

    int mutexIdSem;
    if ((mutexIdSem = semget(bufferCache->semMutexId, 1, 0666)) == -1) {
		perror("semget mutex bufferCache"); 
		exit(1); 
	}

    int wrtIdSem;
    if ((wrtIdSem = semget(bufferCache->semWrtId, 1, 0666)) == -1) {
		perror("semget wrt bufferCache"); 
		exit(1); 
	}

	// using readers solution for readers-writers problem with preference to readers.
	// wait for being granted access to read from the LRU cache.
    wait(mutexIdSem);					// mutex used for accessing shared memory readCount.
    bufferCache->readCount += 1;
    if (bufferCache->readCount == 1) {
    	wait(wrtIdSem);					// wait for a process which is writing to the LRU cache (adding or changing the structure of the Cache).
    }
    signal(mutexIdSem);					// leave control of readCount.

    int hashVal = (id % MOD_HASH);
    hashNode* bucketPtr = bufferCache->hashBufferAddrs.buckets[hashVal];
    // Check if buffer with id present in hashMap of cache or not, if yes, return the buffer.
    // else return NULL.
    while (bucketPtr != NULL) {
    	if (bucketPtr->bufferId == id) {
			bufferNode *requiredBufferNode = bucketPtr->bufferCacheAddr;
			
			int wrtIdBuf;
		    if ((wrtIdBuf = semget(requiredBufferNode->semWrtId, 1, 0666)) == -1) {
				perror("semget wrt requiredBufferNode"); 
				exit(1); 
			}

			int mutexIdBuf;
		    if ((mutexIdBuf = semget(requiredBufferNode->semMutexId, 1, 0666)) == -1) {
				perror("semget mutex requiredBufferNode"); 
				exit(1); 
			}

			// release control for allowing write operations on LRU cache. See Assumption 1 at the top of code.
		    wait(mutexIdSem);
		    bufferCache->readCount -= 1;
		    if (bufferCache->readCount == 0) {
		    	signal(wrtIdSem);
		    }
		    signal(mutexIdSem);

		    // Buffer Accessed for write, that is why readCount was 0. Other readers either wait on mutex or wrt.
		    // and readCount incremented only when 1st reader has access of wrt which is currently accessed by the writer.
		    if (requiredBufferNode->readCount == 0) {
		    	signal(wrtIdBuf);
		    }
		    // Buffer Accessed for read, that is why readCount was non-zero, reduce readCount by 1, and 
		    // signal writer if it is last reader.
		    else {
		    	wait(mutexIdBuf);
			    requiredBufferNode->readCount -= 1;
			    if (requiredBufferNode->readCount == 0) {
			    	signal(wrtIdBuf);
			    }
		    	signal(mutexIdBuf);
			}
			return;
    	}
    	bucketPtr = bucketPtr->nextNode;
    }

    // release read access of Cache semaphores when buffer with id not found.
    wait(mutexIdSem);
	bufferCache->readCount -= 1;
	if (bufferCache->readCount == 0) {
		signal(wrtIdSem);
	}
	signal(mutexIdSem);
}

int main() {
	// releaseCache();
	initializeCache(); // uncomment this, as IPC_EXCL used to initialize LRU cache.
	char s[] = "abcde";
	addToCache(1, s);
	addToCache(2, s);
	addToCache(3, s);
	char *b = (char *)acquireBufferForRead(3);
	printf("%s\n", b);
	fflush(stdout);
	releaseBuffer(3);
	char *p = (char *)acquireBufferForRead(2);
	printf("Value in 2 after read : %s\n", p);
	fflush(stdout);
	releaseBuffer(2);
	char *c = (char *)acquireBufferForWrite(2);
	c[0] = 'g';
	releaseBuffer(2);
	char *d = (char *)acquireBufferForRead(2);
	printf("Value in 2 after write : %s\n", d);
	fflush(stdout);
	releaseBuffer(2);
	addToCache(4, s);
	addToCache(1, s);
	return 0;
}