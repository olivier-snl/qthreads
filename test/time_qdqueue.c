#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <qthread/qthread.h>
#include <qthread/qloop.h>
#include <qthread/qdqueue.h>
#include <qthread/qtimer.h>
#include "argparsing.h"

#define ELEMENT_COUNT 10000
#define THREAD_COUNT 128

static aligned_t queuer(
    qthread_t * me,
    void *arg)
{
    qdqueue_t *q = (qdqueue_t *) arg;
    size_t i;

    for (i = 0; i < ELEMENT_COUNT; i++) {
	if (qdqueue_enqueue(me, q, (void *)me) != QTHREAD_SUCCESS) {
	    fprintf(stderr, "qdqueue_enqueue(q, %p) failed!\n", (void *)me);
	    exit(-2);
	}
    }
    return 0;
}

static aligned_t dequeuer(
    qthread_t * me,
    void *arg)
{
    qdqueue_t *q = (qdqueue_t *) arg;
    size_t i;

    for (i = 0; i < ELEMENT_COUNT; i++) {
	while (qdqueue_dequeue(me, q) == NULL) {
	    qthread_yield(me);
	}
    }
    return 0;
}

static void loop_queuer(
    qthread_t * me,
    const size_t startat,
    const size_t stopat,
    void *arg)
{
    size_t i;
    qdqueue_t *q = (qdqueue_t *) arg;

    for (i = startat; i < stopat; i++) {
	if (qdqueue_enqueue(me, q, (void *)me) != QTHREAD_SUCCESS) {
	    fprintf(stderr, "qdqueue_enqueue(q, %p) failed!\n", (void *)me);
	    exit(-2);
	}
    }
}

static void loop_dequeuer(
    qthread_t * me,
    const size_t startat,
    const size_t stopat,
    void *arg)
{
    size_t i;
    qdqueue_t *q = (qdqueue_t *) arg;

    for (i = startat; i < stopat; i++) {
	if (qdqueue_dequeue(me, q) == NULL) {
	    fprintf(stderr, "qdqueue_dequeue(q, %p) failed!\n", (void *)me);
	    exit(-2);
	}
    }
}

int main(
    int argc,
    char *argv[])
{
    qdqueue_t *q;
    qthread_t *me;
    size_t i;
    aligned_t *rets;
    qtimer_t timer = qtimer_create();

    assert(qthread_initialize() == QTHREAD_SUCCESS);

    CHECK_VERBOSE();
    me = qthread_self();

    if ((q = qdqueue_create()) == NULL) {
	fprintf(stderr, "qdqueue_create() failed!\n");
	exit(-1);
    }

    /* prime the pump */
    qt_loop_balance(0, THREAD_COUNT * ELEMENT_COUNT, loop_queuer, q);
    qt_loop_balance(0, THREAD_COUNT * ELEMENT_COUNT, loop_dequeuer, q);
    if (!qdqueue_empty(me, q)) {
	fprintf(stderr, "qdqueue not empty after priming!\n");
	exit(-2);
    }

    qtimer_start(timer);
    qt_loop_balance(0, THREAD_COUNT * ELEMENT_COUNT, loop_queuer, q);
    qtimer_stop(timer);
    printf("loop balance enqueue: %f secs\n", qtimer_secs(timer));
    qtimer_start(timer);
    qt_loop_balance(0, THREAD_COUNT * ELEMENT_COUNT, loop_dequeuer, q);
    qtimer_stop(timer);
    printf("loop balance dequeue: %f secs\n", qtimer_secs(timer));
    if (!qdqueue_empty(me, q)) {
	fprintf(stderr, "qdqueue not empty after loop balance test!\n");
	exit(-2);
    }

    rets = calloc(THREAD_COUNT, sizeof(aligned_t));
    assert(rets != NULL);
    qtimer_start(timer);
    for (i = 0; i < THREAD_COUNT; i++) {
	assert(qthread_fork(dequeuer, q, &(rets[i])) == QTHREAD_SUCCESS);
    }
    for (i = 0; i < THREAD_COUNT; i++) {
	assert(qthread_fork(queuer, q, NULL) == QTHREAD_SUCCESS);
    }
    for (i = 0; i < THREAD_COUNT; i++) {
	assert(qthread_readFF(me, NULL, &(rets[i])) == QTHREAD_SUCCESS);
    }
    qtimer_stop(timer);
    if (!qdqueue_empty(me, q)) {
	fprintf(stderr, "qdqueue not empty after threaded test!\n");
	exit(-2);
    }
    printf("threaded dq test: %f secs\n", qtimer_secs(timer));
    free(rets);

    if (qdqueue_destroy(me, q) != QTHREAD_SUCCESS) {
	fprintf(stderr, "qdqueue_destroy() failed!\n");
	exit(-2);
    }

    iprintf("success!\n");

    return 0;
}
