#include <upipe/uqueue.h>

#include <upipe-pthread/umutex_pthread.h>
#include <upump-ev/upump_ev.h>

#include <pthread.h>
#include <unistd.h>
#include <dvbcsa/dvbcsa.h>

#define UPUMP_POOL          255
#define UPUMP_BLOCK_POOL    0

static uint8_t length = 255;
static int count = 256;
static bool output = false;
static int push = -1; /* all */
static int pop = -1; /* all */

static void upump_pop_cb(struct upump *upump)
{
    struct uqueue *uqueue = upump_get_opaque(upump, struct uqueue *);
    const char *m;
    int c = 0;
    while ((m = uqueue_pop(uqueue, const char *))) {
        if (output)
            fprintf(stderr, "pop %s\n", m);
        c++;
        if (pop >= 0 && c >= pop)
            break;
    }
}

static void upump_mgr_clean(void *arg)
{
    upump_mgr_release(arg);
}

static void upump_clean(void *arg)
{
    upump_free(arg);
}

static void *start(void *arg)
{
    struct uqueue *uqueue = arg;

    assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL));

    struct upump_mgr *upump_mgr =
        upump_ev_mgr_alloc_loop(UPUMP_POOL, UPUMP_BLOCK_POOL);
    assert(upump_mgr);
    pthread_cleanup_push(upump_mgr_clean, upump_mgr);

    struct upump *upump_pop =
        uqueue_upump_alloc_pop(uqueue, upump_mgr,
                               upump_pop_cb, uqueue,
                               NULL);
    upump_start(upump_pop);
    pthread_cleanup_push(upump_clean, upump_pop);

    assert(!pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL));

    upump_mgr_run(upump_mgr, NULL);

    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    return NULL;
}

static void upump_push_cb(struct upump *upump)
{
    struct uqueue *uqueue = upump_get_opaque(upump, struct uqueue *);
    static const char *msg[] = {
        "msg 1",
        "msg 2",
        "msg 3",
        "msg 4",
        "msg 5",
    };
    static unsigned i = 0;
    const char *m;
    bool pushed = false;
    int c = 0;
    do {
        m = msg[i % UBASE_ARRAY_SIZE(msg)];
        if ((pushed = uqueue_push(uqueue, (void *)m))) {
            if (output)
                fprintf(stderr, "push %s\n", m);
            i++;
            c++;
            if (count > 0)
                count--;
            if (!count) {
                upump_stop(upump);
                break;
            }
            if (push >= 0 && c >= push)
                break;
        }
    } while (pushed);
}

static void upump_signal_term_cb(struct upump *upump)
{
    struct upump *upump_push = upump_get_opaque(upump, struct upump *);
    upump_stop(upump_push);
}

int main(int argc, char *argv[])
{
    bool use_mutex = false;
    int c;

    do {
        c = getopt(argc, argv, "mc:vi:o:l:");
        switch (c) {
            case 'm':
                use_mutex = true;
                break;
            case 'c':
                count = atoi(optarg);
                break;
            case 'v':
                output = true;
                break;
            case 'i':
                push = atoi(optarg);
                break;
            case 'o':
                pop = atoi(optarg);
                break;
            case 'l':
                length = atoi(optarg);
                break;
            case -1:
                break;
            default:
                exit(-1);
        }
    } while (c != -1);

    struct uqueue_elt extra_mutex[length];
    uint8_t extra[uqueue_sizeof(length)];
    struct uqueue uqueue;
    uqueue_init(&uqueue, length, extra);

    pthread_t thread;
    pthread_create(&thread, NULL, start, &uqueue);

    struct upump_mgr *upump_mgr =
        upump_ev_mgr_alloc_default(UPUMP_POOL, UPUMP_BLOCK_POOL);
    assert(upump_mgr);

    struct upump *upump_push =
        uqueue_upump_alloc_push(&uqueue, upump_mgr,
                                upump_push_cb, &uqueue, NULL);
    assert(upump_push);
    upump_start(upump_push);

    struct upump *upump_sigterm =
        upump_alloc_signal(upump_mgr, upump_signal_term_cb, upump_push,
                           NULL, SIGTERM);
    upump_set_status(upump_sigterm, false);
    upump_start(upump_sigterm);

    struct upump *upump_sigint =
        upump_alloc_signal(upump_mgr, upump_signal_term_cb, upump_push,
                           NULL, SIGINT);
    upump_set_status(upump_sigint, false);
    upump_start(upump_sigint);

    upump_mgr_run(upump_mgr, NULL);

    pthread_cancel(thread);
    pthread_join(thread, NULL);

    upump_free(upump_sigterm);
    upump_free(upump_sigint);
    upump_free(upump_push);
    upump_mgr_release(upump_mgr);
    uqueue_clean(&uqueue);
    return 0;
}
