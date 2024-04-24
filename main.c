#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#define CACHE_SIZE 1024 * 4                                // 4kb cache atm
#define CACHE_LINE_SIZE 32                                 // 32 byte cache block size atm
#define NUM_LINES_PER_CACHE (CACHE_SIZE / CACHE_LINE_SIZE) // 4096/32 = 128 lines per cache
#define MAX_CACHE_LINE_ID (unsigned int)0x0FFF             // 3 digits for cache line ID, so mac ID is 4095. with 128 lines per cache and 4 nodes, thats 512 possible lines, with 16 nodes thats 2048 lines. so it works
#define MAX_TASK_CACHE_LINES 3                             // max number of data segements a task can have
#define MAX_TASK_INSTR 20                                  // max number of instructions a task can have
#define MAX_CACHE_LINE_ID (unsigned int)0x0FFF             // max ID for cache lines
#define MAX_TASKS_PER_NODE 3                               // max number of tasks per node, but currently it really is just the number of tasks in a node
#define STATUS_PRINT_DELAY 1                               // how long to wait in between each status print

typedef struct
{
    unsigned int id;
    unsigned int cpuCore;
} threadParam;

typedef struct
{
    unsigned char data[MAX_TASK_CACHE_LINES][CACHE_LINE_SIZE]; // 3 lines of cache
    unsigned int instrs[MAX_TASK_INSTR];
    int numDataSegments;
    int ids[MAX_TASK_CACHE_LINES];
    int nodes[MAX_TASK_CACHE_LINES]; // which node is the page with the corresponding id in
} task;

typedef struct
{
    unsigned char line[CACHE_LINE_SIZE];
    int id;
} cacheLine;

typedef struct
{
    cacheLine *data[NUM_LINES_PER_CACHE];
    unsigned char lineDetails[NUM_LINES_PER_CACHE]; // dirty bit
} cache;

unsigned char stopThreadsFlag = 0;
pthread_mutex_t stopThreadsMutex;
unsigned int num_threads;
pthread_t *threads;
cache *caches;
pthread_mutex_t *cachesMutex;
int cacheCounter = 0; // for round robin
pthread_mutex_t cacheCounterMutex;
unsigned char minCacheLineID[0x0FFF] = {0};
pthread_mutex_t minCacheLineIDMutex;

void exitHandler()
{
    printf("exit\n");
    pthread_mutex_lock(&stopThreadsMutex);
    stopThreadsFlag = 1;
    pthread_mutex_unlock(&stopThreadsMutex);

    for (unsigned int i = 0; i < num_threads; i++)
    {
        pthread_mutex_lock(&cachesMutex[i]);
        for (int j = 0; j < NUM_LINES_PER_CACHE; j++)
        {
            if (caches[i].data[j] != NULL)
            {
                free(caches[i].data[j]);
                caches[i].data[j] = NULL;
            }
        }
        pthread_mutex_unlock(&cachesMutex[i]);
        if (pthread_join(threads[i], NULL) != 0)
        {
            printf("Error joining thread %d.\n", i);
        }
        printf("stopped thread %d\n", i);
    }
    free(caches);
    free(cachesMutex);
    free(threads);

    exit(EXIT_SUCCESS);
}

void createTask(task *me)
{
    me->numDataSegments = 3;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < CACHE_LINE_SIZE; j++)
        {
            me->data[i][j] = 0; 
        }
    }

    unsigned int tempInstr[5] = {0X00000145, 0X00002120, 0X00004135, 0X0000A130, 0xFFFFFFFF};
    for (int i = 0; i < 5; i++)
    {
        me->instrs[i] = tempInstr[i];
    }
}

// only prints data segments atm
void printTask(task *me, char nodeID)
{
    for (int i = 0; i < me->numDataSegments; i++)
    {
        printf("Node %d data seg %d: ", nodeID, i);
        for (int j = 0; j < CACHE_LINE_SIZE; j++)
        {
            printf("%2X ", me->data[i][j]);
        }
        printf("\n");
    }
}

void printTaskDetails(task *me)
{
    printf("physical page ids         : ");
    for (int i = 0; i < me->numDataSegments; i++)
    {
        printf("%d, ", me->ids[i]);
    }
    printf("\n");
    printf("node ids of physical pages: ");
    for (int i = 0; i < me->numDataSegments; i++)
    {
        printf("%d, ", me->nodes[i]);
    }
    printf("\n");
}

void printCache(cache *me)
{
    for (int i = 0; i < NUM_LINES_PER_CACHE; i++)
    {
        if (me->data[i] != NULL)
        {
            printf("%d: ", i);
            for (int j = 0; j < CACHE_LINE_SIZE; j++)
            {
                printf("%2X ", me->data[i]->line[j]);
            }
            printf("   Flags: %2X\n", me->lineDetails[i]);
        }
    }
}

void loadTask(task *t)
{
    cacheLine *cacheLineHolder[MAX_TASK_CACHE_LINES] = {NULL};
    for (int i = 0; i < t->numDataSegments; i++)
    {
        cacheLineHolder[i] = (cacheLine *)malloc(sizeof(cacheLine) * 1);
    }

    /*
    minId should be a global varialbe init to 0, and should have a check to ensure that minId is less than the max possible ID
    */
    for (int i = 0; i < t->numDataSegments; i++)
    {
        memcpy(cacheLineHolder[i]->line, t->data[i], CACHE_LINE_SIZE);
    }

    int id = -1;
    for (int i = 0; i < t->numDataSegments; i++)
    {
        pthread_mutex_lock(&cacheCounterMutex);
        // printf("cache counter: %d\n", cacheCounter);
        id = cacheCounter;
        cacheCounter = (cacheCounter + 1) % ((int)num_threads);
        pthread_mutex_unlock(&cacheCounterMutex);

        unsigned char outOfMem = 1;
        for (int j = 0; j < NUM_LINES_PER_CACHE; j++)
        {
            if (caches[id].data[j] == NULL)
            {
                caches[id].data[j] = cacheLineHolder[i];
                t->nodes[i] = id;
                t->ids[i] = j;
                outOfMem = 0;
                break;
            }
        }
        if (outOfMem == 1)
        {
            printf("node %d has run out of memory\n", id);
        }
    }
}

void cacheInit(cache *me)
{
    for (int i = 0; i < NUM_LINES_PER_CACHE; i++)
    {
        me->data[i] = NULL;
        me->lineDetails[i] = 0;
    }
}

void printAllCaches()
{
    for (int i = 0; i < num_threads; i++)
    {
        pthread_mutex_lock(&cachesMutex[i]);
        printf("Node %d memory contents:\n", i);
        printCache(&caches[i]);
        pthread_mutex_unlock(&cachesMutex[i]);
    }
}

void initializeTasksInCache(task *tasks[MAX_TASKS_PER_NODE])
{
    for (int i = 0; i < MAX_TASKS_PER_NODE; i++)
    {
        tasks[i] = (task *)malloc(sizeof(task) * 1);
        createTask(tasks[i]);
        loadTask(tasks[i]);
    }
}

void killTask(task *t)
{

    // for (int i = 0; i < me->numDataSegments; i++) {
    //     int id = me->nodes[i];
    //     int page = me->ids[i];

    //     pthread_mutex_lock(&cachesMutex[id]);
    //     free(caches[id].data[page]);
    //     caches[id].data[page] = NULL;
    //     pthread_mutex_unlock(&cachesMutex[id]);

    // }
    for (int j = 0; j < t->numDataSegments; j++)
    {
        int id = t->nodes[j];
        pthread_mutex_lock(&cachesMutex[id]);
        if (caches[id].data[t->ids[j]] != NULL)
        {
            free(caches[id].data[t->ids[j]]);
            caches[id].data[t->ids[j]] = NULL;
            pthread_mutex_unlock(&cachesMutex[id]);
        }
        else
        {
            pthread_mutex_unlock(&cachesMutex[id]);
        }
    }

    free(t);
}

unsigned char fetch(int id, int page, int offset)
{
    unsigned char out = 0xFF;
    pthread_mutex_lock(&cachesMutex[id]);
    // printf("id = %d,     page = %d\n", id, page);
    out = caches[id].data[page]->line[offset];
    // for (int i = 0; i < NUM_LINES_PER_CACHE; i++)
    // {
    //     // if (caches[id].data[i]->id == page)
    //     // {
    //     //     printf("c\n");
    //     //     unsigned char out = caches[id].data[i]->line[offset];
    //     //     pthread_mutex_unlock(&cachesMutex[id]);
    //     //     printf("b\n");
    //     //     return out;
    //     // }
    // }
    pthread_mutex_unlock(&cachesMutex[id]);
    return out;
}
void writeMem(int id, int page, int offset, unsigned char newVal)
{
    pthread_mutex_lock(&cachesMutex[id]);

    caches[id].data[page]->line[offset] = newVal;

    // for (int i = 0; i < NUM_LINES_PER_CACHE; i++)
    // {
    //     if (caches[id].data[i]->id == page)
    //     {
    //         caches[id].data[i]->line[offset] = newVal;
    //         pthread_mutex_unlock(&cachesMutex[id]);
    //         return;
    //     }
    // }
    pthread_mutex_unlock(&cachesMutex[id]);
}

void executeTask(task *t)
{
    for (int i = 0; i < MAX_TASK_INSTR; i++)
    {
        if (t->instrs[i] == 0xFFFFFFFF)
        {
            printf("task done\n");
            break;
        }

        unsigned int opcode = (t->instrs[i] & 0x00000F00) >> 8;
        int page = (t->instrs[i] & 0xFFF00000) >> 20;
        int offset = (t->instrs[i] & 0x000FF000) >> 12;
        int constant = (t->instrs[i] & 0x000000FF) >> 0;

        // printf("running instr: %2X\n", t->instrs[i]);
        // printf("opcode: %X\n", opcode);
        // printf("page: %X\n", page);
        // printf("offset: %X\n", offset);

        // check here for remote. if t->nodes[page] != id

        // printf("node id for physical page: %d\n", t->nodes[page]);
        // printf("physical page id for segment: %d\n", t->ids[page]);
        // printf("offset: %X\n", offset);
        unsigned char ret = fetch(t->nodes[page], t->ids[page], offset);
        if (ret != 0xFF)
        {
            // printf("%X\n", ret);
        }
        else
        {
            printf("no page in cache\n");
            return;
        }
        unsigned char newVal;
        switch (opcode)
        {
        case 1: // add
            newVal = ret + (unsigned char)constant;
            break;
        case 2: // sub
            newVal = ret - (unsigned char)constant;
            break;
        case 3: // mult
            newVal = ret * (unsigned char)constant;
            break;
        case 4: // div
            newVal = ret / (unsigned char)constant;
            break;
        case 5: // mod
            newVal = ret % (unsigned char)constant;
            break;
        default:
            printf("unknown opcode");
            break;
        }
        writeMem(t->nodes[page], t->ids[page], offset, newVal);
        usleep(100);
    }
}

void *thread_function(void *arg)
{
    threadParam *tp = (threadParam *)arg;
    task *tasks[MAX_TASKS_PER_NODE];

    // pthread_setaffinity_np() and cpu_set_t is not portable and only works with GNU_SOURCE
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(tp->cpuCore, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
    {
        printf("pthread_setaffinity_np issue\n");
        pthread_exit(NULL);
    }
    printf("Thread %d bound to CPU core %d\n", (int)tp->id, tp->cpuCore);

    printf("Thread %d is running\n", (int)tp->id);
    sleep(2);

    initializeTasksInCache(tasks);

    // pthread_mutex_lock(&cachesMutex[(int)tp->id]);
    // printCache(&caches[(int)tp->id]);
    // pthread_mutex_unlock(&cachesMutex[(int)tp->id]);

    pthread_mutex_lock(&stopThreadsMutex);
    int i = 0;
    while (!stopThreadsFlag)
    {
        pthread_mutex_unlock(&stopThreadsMutex);
        // printTaskDetails(tasks[i]);
        executeTask(tasks[i]);
        sleep(1);

        killTask(tasks[i]);
        tasks[i] = (task *)malloc(sizeof(task));
        createTask(tasks[i]);
        loadTask(tasks[i]);

        i = (i + 1) % MAX_TASKS_PER_NODE;
        pthread_mutex_lock(&stopThreadsMutex);
    }
    pthread_mutex_unlock(&stopThreadsMutex);

    pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
    signal(SIGINT, exitHandler);
    if (argc != 2)
    {
        printf("how many nodes?\n");
        exit(0);
    }

    num_threads = atoi(argv[1]);
    if (!(num_threads == 4 || num_threads == 8 || num_threads == 16 || num_threads == 1))
    {
        printf("can only have 4, 8, or 16 nodes\n");
        exit(0);
    }

    if (pthread_mutex_init(&stopThreadsMutex, NULL) != 0)
    {
        printf("could not create mutex for stopThreadsFlag\n");
        exit(EXIT_FAILURE);
    }
    if (pthread_mutex_init(&minCacheLineIDMutex, NULL) != 0)
    {
        printf("could not create mutex for minCacheLineIDMutex\n");
        exit(EXIT_FAILURE);
    }
    if (pthread_mutex_init(&cacheCounterMutex, NULL) != 0)
    {
        printf("could not create mutex for cacheCounterMutex\n");
        exit(EXIT_FAILURE);
    }
    cachesMutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t) * num_threads);
    for (int i = 0; i < num_threads; i++)
    {
        if (pthread_mutex_init(&cachesMutex[i], NULL) != 0)
        {
            printf("could not create mutex for caches\n");
            exit(EXIT_FAILURE);
        }
    }
    printf("inited mutexes\n");

    threads = (pthread_t *)malloc(sizeof(pthread_t) * num_threads);
    caches = (cache *)malloc(sizeof(cache) * num_threads);

    // initialize all caches
    for (unsigned int i = 0; i < num_threads; i++)
    {
        cacheInit(&caches[i]);
    }

    for (unsigned int i = 0; i < num_threads; i++)
    {
        threadParam *tp = (threadParam *)malloc(sizeof(threadParam));
        if (tp == NULL)
        {
            printf("could allocate memory for thread\n");
            exit(EXIT_FAILURE);
        }
        tp->id = i;
        tp->cpuCore = i % 4;
        int pthreadRetVal = pthread_create(&threads[i], NULL, thread_function, (void *)tp);
        if (pthreadRetVal)
        {
            printf("could not create thread\n");
            exit(EXIT_FAILURE);
        }
    }

    sleep(1);
    unsigned long long iters = 0LL;
    while (1)
    {
        printf("\ntime elapsed: %llu\n", STATUS_PRINT_DELAY * iters);
        printAllCaches();
        sleep(STATUS_PRINT_DELAY);
        iters++;
    }

    exit(EXIT_SUCCESS);
}
