#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "qthread.h"

static qlib_t *qlib=NULL;

static void *qthread_shepherd(void *arg);
static void qthread_wrapper(void *arg);
static int qthread_lock_cmp(const void *p1, const void *p2, const void *conf);
static qthread_lock_t *qthread_lock_locate(void *a);

/* the qthread_shepherd() is the pthread responsible for actually
 * executing the work units
 */

static void *qthread_shepherd(void *arg)
{
    qthread_shepherd_t *me = (qthread_shepherd_t *)arg;         /* rcm -- not used */
    ucontext_t my_context;
    qthread_t *t;
    int done=0;
    
    qthread_debug("qthread_shepherd(%p): forked\n", me);

    while(!done) {
        t = qthread_dequeue(me->ready);

        qthread_debug("qthread_shepherd(%p): dequeued thread id %d/state %d\n", me, t->thread_id, t->thread_state);

        if(t->thread_state == QTHREAD_STATE_TERM_SHEP)
            done=1;
        else {
            assert((t->thread_state == QTHREAD_STATE_NEW) || 
                   (t->thread_state == QTHREAD_STATE_RUNNING));
            
            assert(t->f != NULL);

            /* note: there's a good argument that the following should
             * be: (*t->f)(t), however the state management would be
             * more complex 
             */

            t->shepherd = me->kthread_index;
            qthread_exec(t, &my_context);
            if(t->thread_state == QTHREAD_STATE_YIELDED) {     /* reschedule it */
                t->thread_state = QTHREAD_STATE_RUNNING;
                qthread_enqueue(qlib->kthreads[t->shepherd].ready, t);
            }

            if(t->thread_state == QTHREAD_STATE_BLOCKED) {     /* put it in the blocked queue */
                qthread_enqueue((qthread_queue_t *)t->queue, t);
                assert(pthread_mutex_unlock(&qlib->lock_lock) == 0);
            }

            if(t->thread_state == QTHREAD_STATE_TERMINATED) {
                qthread_debug("qthread_shepherd(): thread %p is in state terminated.\n", t);
                t->thread_state = QTHREAD_STATE_DONE;
            }
        }
    }

    qthread_debug("qthread_shepherd(%p): finished\n", me);
}

int qthread_init(int nkthreads)
{
    int i, r;

    qthread_debug("qthread_init(): began.\n");

    if((qlib = (qlib_t *)malloc(sizeof(qlib_t))) == NULL) {
        perror("qthread_init()");
        abort();
    }

    /* initialize the FEB-like locking structures */

    if((qlib->locks = rbinit(qthread_lock_cmp, NULL)) == NULL) {
        perror("qthread_init()");
        abort();
    }

    assert(pthread_mutex_init(&qlib->lock_lock, NULL)==0);

    /* initialize the kernel threads and scheduler */
    qlib->nkthreads = nkthreads;
    if((qlib->kthreads = (qthread_shepherd_t *)
        malloc(sizeof(qthread_shepherd_t)*nkthreads)) == NULL) {
        perror("qthread_init()");
        abort();
    }

    qlib->stack_size = QTHREAD_DEFAULT_STACK_SIZE;
    qlib->sched_kthread = 0;
    assert(pthread_mutex_init(&qlib->sched_kthread_lock, NULL) == 0);

    for(i=0; i<nkthreads; i++) {
        qlib->kthreads[i].kthread_index = i;
        qlib->kthreads[i].ready = qthread_queue_new();

        qthread_debug("qthread_init(): forking thread %p\n", &qlib->kthreads[i]);

        if((r = pthread_create(&qlib->kthreads[i].kthread, NULL, 
                               qthread_shepherd, &qlib->kthreads[i])) != 0) {
            fprintf(stderr, "qthread_init: pthread_create() failed (%d)\n", r);
            abort();
        }
    }
    
    qthread_debug("qthread_init(): finished.\n");
}

void qthread_finalize(void)
{
    int i, r;
    qthread_t *t;

    assert(qlib != NULL);

    qthread_debug("qthread_finalize(): began.\n");

    /* rcm - probably need to put a "turn off the library flag" here, but,
     * the programmer can ensure that no further threads are forked for now
     */

    /* enqueue the termination thread sentinal */
    for(i=0; i<qlib->nkthreads; i++) {
        t = qthread_thread_new(NULL, NULL);
        t->thread_state = QTHREAD_STATE_TERM_SHEP;
        qthread_enqueue(qlib->kthreads[i].ready, t);
    }

    /* wait for each thread to drain it's queue! */
    for(i=0; i<qlib->nkthreads; i++) {
        if((r = pthread_join(qlib->kthreads[i].kthread, NULL)) != 0) {
            fprintf(stderr, "qthread_finalize: pthread_join() failed (%d)\n",r);
            abort();
        }
    }

    assert(pthread_mutex_lock(&qlib->lock_lock) == 0);
    rbdestroy(qlib->locks);
    assert(pthread_mutex_unlock(&qlib->lock_lock) == 0);
    pthread_mutex_destroy(&qlib->lock_lock);

    assert(pthread_mutex_destroy(&qlib->sched_kthread_lock) == 0);

    free(qlib->kthreads);
    free(qlib);
    qlib = NULL;

    qthread_debug("qthread_finalize(): finished.\n");
}


/* functions to manage thread stack allocation/deallocation */

qthread_t *qthread_thread_new(void (*f)(), void *arg)
{
    qthread_t *t;

    /* rcm - note: in the future we REALLY want to reuse thread and 
     * stack allocations! 
     */

    if((t = (qthread_t *)malloc(sizeof(qthread_t))) == NULL) {
        perror("qthread_thread_new()");
        abort();
    }

    if(((t->context) = (ucontext_t *)malloc(sizeof(ucontext_t))) == NULL) {
        perror("qthread_thread_new()");
        abort();
    }

    t->thread_state = QTHREAD_STATE_NEW;
    t->f = f;
    t->arg = arg;
    t->stack = NULL;
    t->queue = NULL;

    return(t);
}

void qthread_thread_free(qthread_t *t)
{
    /* rcm - note: in the future we REALLY want to reuse thread and 
     * stack allocations! 
     */

    assert(t != NULL);

    free(t->context);
    qthread_stack_free(t);
 
    free(t);
}

void qthread_stack_new(qthread_t *t, unsigned stack_size)
{
    /* rcm - note: in the future we REALLY want to reuse thread and 
     * stack allocations! 
     */

    if((t->stack = (void *)malloc(stack_size)) == NULL) {
        perror("qthread_thread_new()");
        abort();
    }
}

void qthread_stack_free(qthread_t *t)
{
    /* rcm - note: in the future we REALLY want to reuse thread and 
     * stack allocations! 
     */
    if(t->stack != NULL)
        free(t->stack);
}

/* functions to manage the thread queues */

qthread_queue_t *qthread_queue_new()
{
    qthread_queue_t *q;

    if((q = (qthread_queue_t *)malloc(sizeof(qthread_queue_t))) == NULL) {
        perror("qthread_queue_new()");
        abort();
    }

    q->head = NULL;
    q->tail = NULL;
    assert(pthread_mutex_init(&q->lock, NULL) == 0);
    assert(pthread_cond_init(&q->notempty, NULL) == 0);
    return(q);
}

void qthread_queue_free(qthread_queue_t *q)
{
    assert((q->head == NULL) && (q->tail == NULL));
    assert(pthread_mutex_destroy(&q->lock) == 0);
    assert(pthread_cond_destroy(&q->notempty) == 0);
    free(q);
}

void qthread_enqueue(qthread_queue_t *q, qthread_t *t)
{
    assert(t != NULL);
    assert(q != NULL);

    qthread_debug("qthread_enqueue(%p,%p): started\n", q, t);

    assert(pthread_mutex_lock(&q->lock) == 0);

    t->next = NULL;

    if((q->head == NULL) && (q->tail == NULL)) {
        q->head = t;
        q->tail = t;
        assert(pthread_cond_signal(&q->notempty) == 0);
    } else {
        q->tail->next = t;
        q->tail = t;
    }

    assert(pthread_mutex_unlock(&q->lock) == 0);
    qthread_debug("qthread_enqueue(%p,%p): finished\n", q, t);
}

qthread_t *qthread_dequeue(qthread_queue_t *q)
{
    qthread_t *t;

    qthread_debug("qthread_dequeue(%p,%p): started\n", q, t);

    assert(pthread_mutex_lock(&q->lock) == 0);

    while((q->head == NULL) && (q->tail == NULL)) {
        assert(pthread_cond_wait(&q->notempty, &q->lock) == 0);
    }

    assert(q->head != NULL);

    if(q->head != q->tail) {
        t = q->head;
        q->head = q->head->next;
        t->next = NULL;
    } else {
        t = q->head;
        q->head = q->head->next;
        t->next = NULL;
        q->tail = NULL;
    }

    assert(pthread_mutex_unlock(&q->lock) == 0);

    qthread_debug("qthread_dequeue(%p,%p): finished\n", q, t);
    return(t);
}

qthread_t *qthread_dequeue_nonblocking(qthread_queue_t *q)
{
    qthread_t *t;
    
    /* NOTE: it's up to the caller to lock/unlock the queue! */
    qthread_debug("qthread_dequeue_nonblocking(%p,%p): started\n", q, t);

    if((q->head == NULL) && (q->tail == NULL)) {
        return(NULL);           
    }

    if(q->head != q->tail) {
        t = q->head;
        q->head = q->head->next;
        t->next = NULL;
    } else {
        t = q->head;
        q->head = q->head->next;
        t->next = NULL;
        q->tail = NULL;
    }

    qthread_debug("qthread_dequeue_nonblocking(%p,%p): finished\n", q, t);
    return(t);
}

/* this function runs a thread until it completes or yields */

static void qthread_wrapper(void *arg)
{
    qthread_t *t = (qthread_t *)arg;

    qthread_debug("qthread_wrapper(): executing f=%p arg=%p.\n", t->f, t->arg);
    (t->f)(t);
    t->thread_state = QTHREAD_STATE_TERMINATED;

    qthread_debug("qthread_wrapper(): f=%p arg=%p completed.\n", t->f, t->arg);
}

void qthread_exec(qthread_t *t, ucontext_t *c)
{
    assert(t != NULL);
    assert(c != NULL);

    if(t->thread_state == QTHREAD_STATE_NEW) {
        qthread_debug("qthread_exec(%p): type is QTHREAD_THREAD_NEW!\n", t);
        t->thread_state = QTHREAD_STATE_RUNNING;

        getcontext(t->context);
        t->context->uc_stack.ss_sp = t->stack;
        t->context->uc_stack.ss_size = qlib->stack_size;
        t->context->uc_stack.ss_flags = 0;
        qthread_debug("qthread_exec(): context is {%p, %d, %p}\n", t->context->uc_stack.ss_sp,
                      t->context->uc_stack.ss_size, t->context->uc_link);
        
        makecontext(t->context, qthread_wrapper, 1, t);
    }

    t->context->uc_link = c;                               /* NULL pthread_exit() */
    t->return_context = c;

    qthread_debug("qthread_exec(%p): executing swapcontext()...\n", t);
    if(swapcontext(t->return_context, t->context) != 0) {
        perror("qthread_exec: swapcontext() failed");
        abort();
    }

    assert(t != NULL);
    assert(c != NULL);

    qthread_debug("qthread_exec(%p): finished\n", t);
}

/* this function yields thread t to the master kernel thread */

void qthread_yield(qthread_t *t)
{
    qthread_debug("qthread_yield(): thread %p yielding.\n", t);
    t->thread_state = QTHREAD_STATE_YIELDED;
    if(swapcontext(t->context, t->return_context) != 0) {
        perror("qthread_exec: swapcontext() failed");
        abort();
    }
    qthread_debug("qthread_yield(): thread %p resumed.\n", t);
}

/* fork a thread by putting it in somebody's work queue
 * NOTE: scheduling happens here
 */

qthread_t *qthread_fork(void (*f)(qthread_t *), void *arg)
{
    qthread_t *t;
    unsigned shep, tid;

    t = qthread_thread_new(f, arg);
    qthread_stack_new(t,qlib->stack_size);
    
    qthread_debug("qthread_fork(): forking thread %p with stack %p\n", t, t->stack);

    /* figure out which queue to put the thread into */
    tid = qthread_atomic_inc(&qlib->max_thread_id, 
                             &qlib->max_thread_id_lock, 1);
    t->thread_id = tid;

    shep = qthread_atomic_inc_mod(&qlib->sched_kthread, 
                                  &qlib->sched_kthread_lock, 1, 
                                  qlib->nkthreads);

    qthread_enqueue(qlib->kthreads[shep].ready, t);

    return(t);
}

void qthread_join(qthread_t *t)
{
    /* this is extremely inefficient! */
    while(t->thread_state != QTHREAD_STATE_DONE)
        ;

    qthread_thread_free(t);
    return;
}

/* functions to implement FEB locking/unlocking 
 *
 * NOTE: these are very inefficiently implemented, and for prototyping
 * purposes only
 */

static int qthread_lock_cmp(const void *p1, const void *p2, const void *conf)
{
    qthread_lock_t *l1, *l2;

    l1 = (qthread_lock_t *)p1;
    l2 = (qthread_lock_t *)p2;
    if(l1->address < l2->address)
        return(-1);
    if(l1->address > l2->address)
        return(1);
    return(0);
}

/* the lock data structure needs to be locked before calling this function */

static qthread_lock_t *qthread_lock_locate(void *a)
{
    static qthread_lock_t *l=NULL;
    qthread_lock_t *m;

    m = NULL;

    if(l == NULL) {
        if((l = (qthread_lock_t *)malloc(sizeof(qthread_lock_t))) == NULL) {
            perror("qthread_lock()");
            abort();
        }
        l->locked = 0;
        l->waiting = qthread_queue_new();
    }

    l->address = a;

    m = (qthread_lock_t *)rbsearch((void *)l, qlib->locks);

    assert(m != NULL);
    if(l == m)
        l = NULL;

    return (m);
}

void qthread_lock(qthread_t *t, void *a)
{
    qthread_lock_t *m;

    assert(pthread_mutex_lock(&qlib->lock_lock)==0);

    m = qthread_lock_locate(a);

    /* now that we know which lock to test, try it */
    if(!m->locked) {            /* just created the lock during the search, it's mine */
        m->owner = t->thread_id;
        m->locked = 1;

        assert(pthread_mutex_unlock(&qlib->lock_lock) == 0);

        qthread_debug("qthread_lock(%p, %p): returned (not locked)\n", t, a);
    } else {
        /* failure, dequeue this thread and yield 
         * NOTE: it's up to the master thread to enqueue this thread and 
         * unlock the master lock
         */
        t->thread_state = QTHREAD_STATE_BLOCKED;
        t->queue = m->waiting;

        if(swapcontext(t->context, t->return_context) != 0) {
            perror("qthread_exec: swapcontext() failed");
            abort();
        }

        /* once I return to this context, I own the lock! */
        qthread_debug("qthread_lock(%p, %p): returned (was locked)\n", t, a);
    }
}

void qthread_unlock(qthread_t *t, void *a)
{
    qthread_lock_t *m;
    qthread_t *u;

    assert(pthread_mutex_lock(&qlib->lock_lock) == 0);
    
    qthread_debug("qthread_unlock(%p, %p): started\n", t, a);

    m = qthread_lock_locate(a);
    if(m->locked == 0) {
        fprintf(stderr, "qthread_unlock(%p,%p): attempt to unlock an address "
                "that is not locked!\n");
        abort();
    }

    /* unlock the address... if anybody's waiting for it, give them the lock
     * and put them in a ready queue.  If not, delete the lock structure.
     */

    assert(pthread_mutex_lock(&m->waiting->lock) == 0);

    u = qthread_dequeue_nonblocking(m->waiting);
    if(u == NULL) {
        qthread_debug("qthread_unlock(%p,%p): deleting waiting queue\n",t,a);
        m->locked = 0;

        assert(pthread_mutex_unlock(&m->waiting->lock) == 0);

        qthread_queue_free(m->waiting);
        rbdelete(m, qlib->locks);
    } else {
        qthread_debug("qthread_unlock(%p,%p): pulling thread from queue (%p)\n",t,a,u);
        u->thread_state = QTHREAD_STATE_RUNNING;
        m->owner = u->thread_id;
        
        /* NOTE: because of the use of getcontext()/setcontext(), threads
         * return to the shepherd that setcontext()'d into them, so they
         * must remain in that queue.
         */
        qthread_enqueue(qlib->kthreads[u->shepherd].ready, u);
        
        assert(pthread_mutex_unlock(&m->waiting->lock) == 0);
    }

    assert(pthread_mutex_unlock(&qlib->lock_lock) == 0);

    qthread_debug("qthread_unlock(%p, %p): returned\n", t, a);
}
