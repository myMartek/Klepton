// The guest's semaphores under load, in both orders (KL_SEM_CLAIM_FIRST): no token
// is lost or doubled across sem_wait, sem_trywait and sem_timedwait, and a producer
// that posts only when the value reads 0 never leaves its consumer asleep.
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int klb_sem_init(int *s, int pshared, unsigned value);
int klb_sem_post(int *s);
int klb_sem_wait(int *s);
int klb_sem_trywait(int *s);
int klb_sem_timedwait(int *s, const struct timespec *ts);
int klb_sem_getvalue(int *s, int *out);

enum { TAKERS = 8, POSTERS = 4, TOKENS = 100000, ITEMS = 300000 };

static int tokens, wake;
static atomic_int taken, posted, finished, queued, handled, stop;

static void *try_taker(void *arg) {
    long timed = (long)arg;
    while (!atomic_load(&finished)) {
        int r;
        if (timed) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 200000;
            if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
            r = klb_sem_timedwait(&tokens, &t);
        } else {
            r = klb_sem_trywait(&tokens);
        }
        if (r == 0) atomic_fetch_add(&taken, 1);
    }
    return NULL;
}

static void *blocking_taker(void *arg) {
    (void)arg;
    for (;;) {
        klb_sem_wait(&tokens);
        if (atomic_load(&finished)) return NULL;
        atomic_fetch_add(&taken, 1);
    }
}

static void *poster(void *arg) {
    (void)arg;
    for (int i = 0; i < TOKENS; i++) {
        klb_sem_post(&tokens);
        atomic_fetch_add(&posted, 1);
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    for (;;) {
        klb_sem_wait(&wake);
        if (atomic_load(&stop)) return NULL;
        while (atomic_load(&queued) > 0) {
            atomic_fetch_sub(&queued, 1);
            atomic_fetch_add(&handled, 1);
        }
    }
}

int main(void) {
    alarm(120);
    klb_sem_init(&tokens, 0, 0);
    pthread_t takers[TAKERS], posters[POSTERS];
    for (long i = 0; i < TAKERS; i++)
        pthread_create(&takers[i], NULL, i % 4 == 0 ? blocking_taker : try_taker, (void *)(i % 2));
    for (int i = 0; i < POSTERS; i++) pthread_create(&posters[i], NULL, poster, NULL);
    for (int i = 0; i < POSTERS; i++) pthread_join(posters[i], NULL);
    while (atomic_load(&taken) < atomic_load(&posted)) usleep(1000);
    usleep(20000);
    int left = -1, got = atomic_load(&taken), sent = atomic_load(&posted);
    klb_sem_getvalue(&tokens, &left);
    atomic_store(&finished, 1);
    for (int i = 0; i < TAKERS; i++) klb_sem_post(&tokens);
    for (int i = 0; i < TAKERS; i++) pthread_join(takers[i], NULL);
    if (got != sent || left != 0) {
        printf("FAIL: %d posted, %d taken, %d left\n", sent, got, left);
        return 1;
    }

    klb_sem_init(&wake, 0, 0);
    pthread_t c;
    pthread_create(&c, NULL, consumer, NULL);
    int lost = 0;
    for (int i = 1; i <= ITEMS; i++) {
        atomic_fetch_add(&queued, 1);
        int value = 1;
        klb_sem_getvalue(&wake, &value);
        if (value == 0) klb_sem_post(&wake);
        if (i % 50 == 0) {
            int spins = 0;
            while (atomic_load(&handled) < i && spins++ < 200000)
                if (spins % 64 == 0) sched_yield();
            if (atomic_load(&handled) < i) { lost++; klb_sem_post(&wake); }
        }
    }
    atomic_store(&stop, 1);
    klb_sem_post(&wake);
    pthread_join(c, NULL);
    if (lost) {
        printf("FAIL: %d lost wakes in %d items\n", lost, ITEMS);
        return 1;
    }
    printf("PASS: %d tokens across wait, trywait and timedwait; %d items without a lost wake\n", got, ITEMS);
    return 0;
}
