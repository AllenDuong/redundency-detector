#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <iostream>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bitset>

#include "unistd.h"

#include "packet.h"
#include "config.h"

#include "murmur3/murmur3.h"

/* Generates the report */
#define REPORT \
    { printf("\n\tBytes Processed:\t%ld\n\tTotal Cache Hits:\t%d\n\tRedundancy Detected:\t%ld%%\n", \
        totalBytesProcessed, hits, (totalRedundantBytes * 100) / totalBytesProcessed);} \

#define BLOOM_FILTER_SIZE 510000000
#define BUFFER_SIZE 15
#define N_HASHES 4
#define STRIDE 64
#define BLOOM_RATIO 3

// Global mutex / condition variables and file pointer
typedef struct _thread__arg {
    FILE * fp;
    pthread_mutex_t * mutex;
    pthread_cond_t  * empty;
    pthread_cond_t  * fill;
} thread_args;

/* =================== Globals ===================== */

int sharedBufferIndex = 0;
int hits = 0;

long int totalBytesProcessed = 0;
long int totalRedundantBytes = 0;
bool combo = false;
int comboBytes = 0;
long int dataWordsProcessed = 0;

bool doneReading = false;

packet * sharedBuffer[BUFFER_SIZE] = { NULL };
std::bitset<BLOOM_FILTER_SIZE> bloomFilter;

int level = 1;

/* Debug stuff */
#ifdef DEBUG_ALL
long int maxDataInMemory = 0;
long int dataInMemory = 0;
long int numPackets = 0;
#endif

/* ================================================== */

void freePacket(packet * p) {
#ifdef DEBUG_ALL
    dataInMemory -= sizeof(packet);
#endif
    free(p);
}

void addPacketToBuffer(packet * p) {
    sharedBuffer[sharedBufferIndex++] = p;
    totalBytesProcessed += p->size;
#ifdef DEBUG_ALL
    dataInMemory += sizeof(packet);
    numPackets += 1;
    if (dataInMemory > maxDataInMemory)
        maxDataInMemory = dataInMemory;
#endif
}

bool hashAndAdd(unsigned char * data, int size) {
    /* Hashes data and adds it to the bloom filter? */
    dataWordsProcessed += N_HASHES;
    if (dataWordsProcessed > (BLOOM_FILTER_SIZE / BLOOM_RATIO)) {
        // If we are in the danger zone of false positives, blow it away :(
        dataWordsProcessed = 0;
#ifdef DEBUG_ALL
        puts("resetting the bloom filter");
#endif
        bloomFilter.reset();
    }
    unsigned long djb2 = djb2Hash(data, size) % BLOOM_FILTER_SIZE;
    unsigned char murmur[128];
    MurmurHash3_x64_128(data, size, 33, murmur);
    size_t hash = 0;
    // By default assume that it is redundant
    bool redundant = true;
    for (int i = 1; i < N_HASHES + 1; i ++) {
        // The more dank this is, the better this is gonna be!
        hash = (int) murmur[i] + ((int) murmur[i+1] * 33)  + ((int) murmur[i+2] * 5381)  + djb2 * i;
        hash = hash % BLOOM_FILTER_SIZE;
        // If the bloom filter comes up with ANY 0s, then we KNOW that this is
        // NOT redundant
        if (bloomFilter[hash] == 0) {
            redundant = false;
        }

        // After the above check, we set it to 1, thereby "adding" it to the
        // bloom filter
        bloomFilter[hash] = 1;
#ifdef DEBUG_ALL
        printf("[HASH] %lu\n", hash);
#endif
    }
#ifdef DEBUG_ALL
    printf("[MURMUR] %d\n", (int) murmur[0]);
#endif

    if (redundant) {
        if (!combo){
            comboBytes = size;
            combo = true;
        } else
            comboBytes += 1;
    }

    return redundant;
}

void checkAndAddToBloomFilter(packet * p) {
    // Checks and adds the packet to the bloom filter
    unsigned char data[STRIDE];
    int dataOffset = 0; // Where in the data array we are
    if (level == 2) {
        // Use a sliding window of size STRIDE
        while ((dataOffset + STRIDE) < p->size) {
            memcpy(data, p->data + dataOffset, STRIDE);
            // If there wasn't a match, but we are currently processing a previous match,
            // flush the data out and reset it
            if(!hashAndAdd(data, STRIDE)) {
                if (combo) {
                    combo = false;
                    totalRedundantBytes += comboBytes;
                    comboBytes = 0;
                    hits ++;
                }
            }
            dataOffset ++;
        }
    } else if (level == 1) {
        // Hash over the entire data part of the packet
        hashAndAdd(p->data, p->size);
    }

    // At the end of the processing, if we were in the middle of processing a
    // chunk of redundant data, flush that out
    if (combo) {
        combo = false;
        totalRedundantBytes += comboBytes;
        comboBytes = 0;
        hits ++;
    }
}

void * consumerThread(void * arg) {
    /* Consumer thread */

    thread_args * args = (thread_args *) arg;

    while (!doneReading || sharedBufferIndex > 0) {
        pthread_mutex_lock(args->mutex);
        // Spin
        while (!doneReading && sharedBufferIndex == 0) {
            pthread_cond_wait(args->fill, args->mutex);
        }
        // Grabs a packet off the buffer, adds to bloom filter and checks and
        // the same time, and then frees the packet
        if (sharedBufferIndex > 0) {
            packet * p = sharedBuffer[--sharedBufferIndex];
            assert(p != NULL);
            checkAndAddToBloomFilter(p);
            freePacket(p);
        }
        pthread_cond_signal(args->empty);
        pthread_mutex_unlock(args->mutex);
    }
    return 0;
}

void * producerThread(void * arg) {
    /* Producer thread - loops through the file and adds stuff to the buffer */
    thread_args * args = (thread_args *) arg;

    // Loop through the file and parse the packets
    while (feof(args->fp) == 0) {
        // Parses out the packets from the file pointer
        packet * p = parsePacket(args->fp);
        pthread_mutex_lock(args->mutex);
        // Wait while the buffer is full...
        while (sharedBufferIndex == BUFFER_SIZE)
            pthread_cond_wait(args->empty, args->mutex);

        // Pushes to the buffer
        if (p != NULL)
            addPacketToBuffer(p);

        pthread_cond_signal(args->fill);
        pthread_mutex_unlock(args->mutex);
    }
    // Set this to true to signal the consumer threads to start cleaning up the
    // buffer
    doneReading = true;
    // Make sure to wake up all sleeping consumers
    pthread_cond_broadcast(args->fill);
    return 0;
}

/* Prints out a help message if the -h flag, or incorrect flags are given */
void help(char *progname, int status) {
    printf("usage: %s [-l LEVEL] [-t THREADS] file ...\n", progname);
    printf("\t -l LEVEL: set level of redundancy detection (default = 1)\n");
    printf("\t -t THREADS: # of threads to run (default = 2)\n");
    printf("\tMax threads: 10\n");
    printf("\tLevel 1: Detect redundancy on a whole packet payload basis using a hash function across the packet payload.\n");
    printf("\tLevel 2: Detect redundancy on sub-packet windows (minimum of 64 bytes).\n");
    exit(status);
}


void analyzeFile(FILE * fp, int numThreads) {
    /* Producer that loops through the input file and fills a queue of packets */

    /*
     * Since we might be analyzing multiple files, we want to re-initialize the
     * global variables to zero
     */
    hits = 0;
    sharedBufferIndex = 0;
    doneReading = false;
    comboBytes = 0;
    combo = false;
#ifdef DEBUG_ALL
    numPackets = 0;
#endif

    /* Condition variables and lock */
    pthread_cond_t empty  = PTHREAD_COND_INITIALIZER;
    pthread_cond_t fill   = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    /* Jump through the global header of the file */
    check(fseek(fp, 24, SEEK_SET));

    // Initializes a struct to hold arguments for the producer
    thread_args * threadArgs = (thread_args *) malloc(sizeof(thread_args));

    if (threadArgs == NULL) ERROR;

    threadArgs->fp = fp;
    threadArgs->mutex = &mutex;
    threadArgs->empty = &empty;
    threadArgs->fill  = &fill;

    // Creates the producer thread
    pthread_t producer;

    if (pthread_create(&producer, NULL, producerThread, threadArgs) < 0) {
        // If the thread creation fails...
        // Make sure to free the args struct
        free(threadArgs);
        ERROR;
    }
    pthread_t consumers[numThreads];

    // Make all the consumers
    for (size_t i = 0; i < (size_t) numThreads; i ++) {
        if (pthread_create(&consumers[i], NULL, consumerThread, threadArgs) < 0) {
            free(threadArgs);
            ERROR;
        }
    }

    // Wait for producer thread
    if (pthread_join(producer, NULL) < 0)
        ERROR;

    for (size_t i = 0; i < (size_t) numThreads; i ++) {
        if (pthread_join(consumers[i], NULL) < 0) ERROR;
    }

    /* Frees the argument struct and generates the report */
    free(threadArgs);

#ifdef DEBUG_ALL
    printf("%lu\n", bloomFilter.count());
    printf("%lu\n", bloomFilter.size());
    /* printf("%lu\n", sizeof(bloomFilter)); */
    /* For development */
    float dataInMemory = (float) maxDataInMemory + sizeof(char) * BLOOM_FILTER_SIZE;
    fprintf(stderr, "%ld packets processed\n", numPackets);
    fprintf(stderr, "%.2f MB max used for storage\n", dataInMemory / 1000000.0f);
#endif
}

bool isNumber(char * optarg) {
    /* Ensures that the optional argument (c-string) is a number) */
    char c;
    while ((c = *optarg++)) {
        if (!isdigit(c)) return false;
    }
    return true;
}

int main(int argc, char * argv[]) {
    // Default the number of consumer threads to 1
    int numThreads = 1;
    int c;

    if(argc == 1) help(argv[0], 1);

    // process command line arguments
    while((c = getopt(argc, argv, "hl:t:")) != -1){
        switch(c){
            case 'h':
                help(argv[0], 0);
            case 'l':
                if (!isNumber(optarg)) help(argv[0], 1);
                level = atoi(optarg);
                if (level != 1 && level != 2){
                    fprintf(stderr, "[ERROR] Invalid level specified\n");
                    help(argv[0], 1);
                }
                break;
            case 't':
                if (!isNumber(optarg)) help(argv[0], 1);
                numThreads = atoi(optarg);
                if (numThreads < 0 || numThreads > 9) {
                    fprintf(stderr, "[ERROR] Invalid number of threads\n");
                    help(argv[0], 1);
                }
                break;
            default:
                help(argv[0], 1);
                break;
        }
    }

    // process files remaining in command line arguments
    int fileN = 1;
    int numOfFiles = argc - optind;
    printf("Using 1 producer thread, %d consumer threads, in level %d mode\n\n", numThreads, level);
    for (size_t i = optind; i < (size_t) argc; i++){
        FILE * inputFile = fopen(argv[i], "r");
        printf("Processing File %d of %d\n", fileN++, numOfFiles);
        if (inputFile == NULL) ERROR;
        // Get the packet data from the file
        analyzeFile(inputFile, numThreads);

        /* Cleanup */
        fclose(inputFile);
    }
    REPORT;

    return EXIT_SUCCESS;
}
