#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/types.h>
#include <curl/curl.h>

#include "zutil.h"
#include "lab_png.h"
#include "crc.h"

#define NUM_STRIPS        50
#define NUM_SERVERS       3
#define MAX_SEGMENT_SIZE (1024 * 30)
#define SHM_KEY          IPC_PRIVATE

const char *servers[NUM_SERVERS] = {
    "http://ece252-1.uwaterloo.ca:2530/image?img=%d&part=%d",
    "http://ece252-2.uwaterloo.ca:2530/image?img=%d&part=%d",
    "http://ece252-3.uwaterloo.ca:2530/image?img=%d&part=%d"
};

typedef struct {
    // storage for each compressed strip
    int    received[NUM_STRIPS];
    size_t sizes[NUM_STRIPS];
    unsigned char data[NUM_STRIPS][MAX_SEGMENT_SIZE];
    // bounded‐buffer queue of strip IDs
    int    buf[NUM_STRIPS];
    int    in, out, count;
    // next strip to download
    int    next_id;
} shared_buf_t;

// named semaphores
sem_t *mutex, *empty, *full;

// cURL accumulator
typedef struct {
    unsigned char *buf;
    size_t         sz;
    int            frag_id;
} curl_data_t;

// parse "X-Ece252-Fragment: M"
static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    curl_data_t *cd = ud;
    size_t total = size * nmemb;
    if (!strncasecmp(ptr, "X-Ece252-Fragment:", 18)) {
        cd->frag_id = atoi(ptr + 18);
    }
    return total;
}

// write body into cd->buf
static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    curl_data_t *cd = ud;
    size_t total = size * nmemb;
    if (cd->sz + total > MAX_SEGMENT_SIZE) return 0;
    memcpy(cd->buf + cd->sz, ptr, total);
    cd->sz += total;
    return total;
}

// producer: fetch strips 0..49, store, enqueue
void producer(shared_buf_t *shm, int B, int imgN) {
    CURL *curl = curl_easy_init();
    if (!curl) exit(EXIT_FAILURE);
    srand(getpid());

    while (1) {
        sem_wait(mutex);
        if (shm->next_id >= NUM_STRIPS) {
            sem_post(mutex);
            break;
        }
        int id = shm->next_id++;
        sem_post(mutex);

        // download strip 'id'
        curl_data_t cd = {
            .buf     = malloc(MAX_SEGMENT_SIZE),
            .sz      = 0,
            .frag_id = -1
        };
        char url[256];
        const char *srv = servers[rand() % NUM_SERVERS];
        snprintf(url, sizeof(url), srv, imgN, id);

        curl_easy_setopt(curl, CURLOPT_URL,            url);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA,     &cd);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &cd);
        curl_easy_perform(curl);

        // store compressed data
        memcpy(shm->data[id], cd.buf, cd.sz);
        shm->sizes[id]    = cd.sz;
        shm->received[id] = 1;
        free(cd.buf);

        // enqueue id
        sem_wait(empty);
        sem_wait(mutex);
        shm->buf[shm->in] = id;
        shm->in = (shm->in + 1) % B;
        shm->count++;
        sem_post(mutex);
        sem_post(full);
    }

    curl_easy_cleanup(curl);
    exit(EXIT_SUCCESS);
}

// consumer: dequeue IDs, exit on id<0
void consumer(shared_buf_t *shm, int B, int delay_ms) {
    while (1) {
        sem_wait(full);
        sem_wait(mutex);
        int id = shm->buf[shm->out];
        shm->out = (shm->out + 1) % B;
        shm->count--;
        sem_post(mutex);
        sem_post(empty);

        if (id < 0) break;            // poison pill → exit
        usleep(delay_ms * 1000);
    }
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <B> <P> <C> <X> <N>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int B = atoi(argv[1]),
        P = atoi(argv[2]),
        C = atoi(argv[3]),
        X = atoi(argv[4]),
        N = atoi(argv[5]);
    if (B<1||B>NUM_STRIPS||P<1||C<1||X<0||N<1||N>3) {
        fprintf(stderr, "Invalid arguments\n");
        return EXIT_FAILURE;
    }

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    curl_global_init(CURL_GLOBAL_ALL);

    // allocate shared memory
    int shmid = shmget(SHM_KEY, sizeof(shared_buf_t),
                      IPC_CREAT | 0600);
    shared_buf_t *shm = shmat(shmid, NULL, 0);
    memset(shm, 0, sizeof(*shm));

    // named semaphores
    sem_unlink("/paster_mutex");
    sem_unlink("/paster_empty");
    sem_unlink("/paster_full");
    mutex = sem_open("/paster_mutex", O_CREAT, 0666, 1);
    empty = sem_open("/paster_empty", O_CREAT, 0666, B);
    full  = sem_open("/paster_full",  O_CREAT, 0666, 0);

    // fork producers, record PIDs
    pid_t pP[P];
    for (int i = 0; i < P; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
        if (pid == 0) producer(shm, B, N);
        pP[i] = pid;
    }

    // fork consumers, record PIDs
    pid_t pC[C];
    for (int i = 0; i < C; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
        if (pid == 0) consumer(shm, B, X);
        pC[i] = pid;
    }

    // wait for all producers
    for (int i = 0; i < P; i++) {
        waitpid(pP[i], NULL, 0);
    }

    // inject C poison pills (-1) to wake consumers
    for (int i = 0; i < C; i++) {
        sem_wait(empty);
        sem_wait(mutex);
        shm->buf[shm->in] = -1;
        shm->in = (shm->in + 1) % B;
        shm->count++;
        sem_post(mutex);
        sem_post(full);
    }

    // wait for all consumers
    for (int i = 0; i < C; i++) {
        waitpid(pC[i], NULL, 0);
    }

    // assemble and save PNG
    const unsigned char *data_ptrs[NUM_STRIPS];
    size_t              sizes[NUM_STRIPS];
    for (int i = 0; i < NUM_STRIPS; i++) {
        data_ptrs[i] = shm->data[i];
        sizes[i]     = shm->sizes[i];
    }
    if (save_png_from_memstrips(data_ptrs, sizes, "all.png"))
        fprintf(stderr, "Error writing all.png\n");

    gettimeofday(&t1, NULL);
    double elapsed = (t1.tv_sec  - t0.tv_sec)
                   + (t1.tv_usec - t0.tv_usec)/1e6;
    printf("paster2 execution time: %.3f seconds\n", elapsed);

    // cleanup
    sem_close(mutex);
    sem_close(empty);
    sem_close(full);
    sem_unlink("/paster_mutex");
    sem_unlink("/paster_empty");
    sem_unlink("/paster_full");
    shmdt(shm);
    shmctl(shmid, IPC_RMID, NULL);
    curl_global_cleanup();

    return EXIT_SUCCESS;
}
