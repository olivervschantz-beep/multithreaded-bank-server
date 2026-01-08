#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>

#define SOCK_PATH "/tmp/threadbank.sock"
#define MAX_LINE 512

/* Number of service desks (threads). */
#define NUM_DESKS 3

/* Maximum number of waiting clients in one desk queue. */
#define QUEUE_CAPACITY 64

/* Number of bank accounts (testbench uses accounts 0..19). */
#define NUM_ACCOUNTS 20

/* Filenames for data and logging. */
#define ACCOUNT_FILE "accounts.dat"
#define LOG_FILE "bank.log"


/* -------------------- Types and global variables -------------------- */


/* Bounded circular queue used for each desk's waiting clients. */
typedef struct {
    int fds[QUEUE_CAPACITY];     /* Client socket file descriptors. */
    int head;                    /* Index of first element. */
    int tail;                    /* Index one past last element. */
    int count;                   /* Number of elements in the queue. */
    int busy;                    /* 1 if this desk is currently serving a client, 0 if idle */
    pthread_mutex_t mutex;       /* Protects access to the queue. */
    pthread_cond_t cond;         /* Signals when queue becomes non-empty. */
} client_queue_t;

/* One queue per service desk. */
static client_queue_t desks[NUM_DESKS];

/* Account data + read–write locks */
static long accounts[NUM_ACCOUNTS];
static pthread_rwlock_t account_locks[NUM_ACCOUNTS];

/* Logging state (shared by all threads). */
static FILE *logf = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Signal/shutdown control. */
static volatile sig_atomic_t shutting_down = 0;
static int listen_fd_global = -1;



/* -------------------- Low-level helper functions -------------------- */

/*
 * Read a single line from fd into buf.
 * - Reads up to maxlen-1 characters.
 * - Stops when it sees '\n' or EOF.
 * - Always terminates buf with '\0' if at least one character was read.
 *
 * Returns:
 *   >0  number of bytes read
 *    0  EOF with no data
 *   -1  error (read() failed with something else than EINTR)
 */
static ssize_t read_line(int fd, char *buf, size_t maxlen)
{
    size_t pos = 0;
    while (pos + 1 < maxlen) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 0) {
            /* EOF */
            break;
        } else if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf[pos++] = c;
        if (c == '\n') {
            break;
        }
    }
    if (pos == 0) {
        return 0; /* nothing read (EOF) */
    }
    buf[pos] = '\0';
    return (ssize_t)pos;
}


/* Print an error message using perror() and terminate the server. */
static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}


/* -------------------- Logging helpers -------------------- */


/* Open the log file in append mode. The server continues even if this fails. */
static void log_open(void)
{
    logf = fopen(LOG_FILE, "a");
    if (!logf) {
        perror("fopen log file");
        /* Not fatal: server can run without logging if this fails. */
    } else {
        fprintf(stderr, "Logging to %s\n", LOG_FILE);
    }
}

/* Flush and close the log file if it is open. */
static void log_close(void)
{
    if (!logf) return;

    fflush(logf);
    int fd = fileno(logf);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(logf);
    logf = NULL;
}

/*
 * Thread-safe write to the log file.
 * Uses a mutex so that log lines from different threads do not interleave.
 */
static void log_write(const char *fmt, ...)
{
    if (!logf) return;

    pthread_mutex_lock(&log_mutex);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);

    fflush(logf);  /* ensurethe line is written to disk buffers */
    pthread_mutex_unlock(&log_mutex);
}

/* -------------------- Signal handling -------------------- */

/*
 * Signal handler for SIGINT and SIGTERM.
 * - Sets the global shutting_down flag.
 * - Closes the listening socket, which will cause accept() to fail
 *   and let the main loop exit.
 *
 * Only async-signal-safe operations are performed here.
 */
static void handle_signal(int sig)
{
    (void)sig; /* unused */

    shutting_down = 1;

    /* Closing the listen socket will make accept() fail and let main exit the loop. */
    if (listen_fd_global >= 0) {
        close(listen_fd_global);
        listen_fd_global = -1;
    }
}

/* -------------------- Account helpers and RW locks -------------------- */

/* Check that an account index is inside the valid range 0..NUM_ACCOUNTS-1. */
static int account_valid(int acc)
{
    return acc >= 0 && acc < NUM_ACCOUNTS;
}

/* Initialize all account balances to zero and set up their read–write locks. */
static void accounts_init(void)
{
    for (int i = 0; i < NUM_ACCOUNTS; i++) {
        accounts[i] = 0; /* start with zero balance */
        if (pthread_rwlock_init(&account_locks[i], NULL) != 0) {
            die("pthread_rwlock_init");
        }
    }
}

/*
 * Load account balances from a text file ACCOUNT_FILE.
 * Format: one long integer per line, NUM_ACCOUNTS lines.
 * If the file doesn't exist, keep zero balances and print a message.
 */
static void accounts_load_from_file(void)
{
    FILE *f = fopen(ACCOUNT_FILE, "r");
    if (!f) {
        fprintf(stderr, "No %s found, starting with zero balances\n", ACCOUNT_FILE);
        return;
    }

    fprintf(stderr, "Loading accounts from %s\n", ACCOUNT_FILE);
    for (int i = 0; i < NUM_ACCOUNTS; i++) {
        long val;
        if (fscanf(f, "%ld", &val) != 1) {
            fprintf(stderr, "Short or invalid %s, resetting all balances to zero\n", ACCOUNT_FILE);
            for (int j = 0; j < NUM_ACCOUNTS; j++) {
                accounts[j] = 0;
            }
            fclose(f);
            return;
        }
        accounts[i] = val;
    }

    fclose(f);
    fprintf(stderr, "Loaded %d accounts from %s\n", NUM_ACCOUNTS, ACCOUNT_FILE);
}

/*
 * Save account balances to disk atomically.
 * Writes to a temporary file "accounts.tmp" first, flushes and fsyncs it,
 * then renames it to ACCOUNT_FILE.
 *
 * Returns 0 on success, -1 on error.
 */
static int accounts_save_to_file(void)
{
    const char *tmpname = "accounts.tmp";
    FILE *f = fopen(tmpname, "w");
    if (!f) {
        perror("fopen accounts.tmp");
        return -1;
    }

    for (int i = 0; i < NUM_ACCOUNTS; i++) {
        if (fprintf(f, "%ld\n", accounts[i]) < 0) {
            perror("fprintf accounts.tmp");
            fclose(f);
            return -1;
        }
    }

    if (fflush(f) != 0) {
        perror("fflush accounts.tmp");
        fclose(f);
        return -1;
    }

    int fd = fileno(f);
    if (fd >= 0 && fsync(fd) != 0) {
        /* fsync errors are not fatal but we log them */
        perror("fsync accounts.tmp");
    }

    fclose(f);

    if (rename(tmpname, ACCOUNT_FILE) != 0) {
        perror("rename accounts.tmp -> accounts.dat");
        return -1;
    }

    fprintf(stderr, "Saved %d accounts to %s\n", NUM_ACCOUNTS, ACCOUNT_FILE);
    return 0;
}

/*
 * Read the current balance of one account.
 * Returns 0 on success and places balance in *balance.
 * Returns -1 if account index is invalid or locking fails.
 */
static int account_get_balance(int acc, long *balance)
{
    if (!account_valid(acc)) return -1;
    if (pthread_rwlock_rdlock(&account_locks[acc]) != 0) {
        return -1;
    }
    *balance = accounts[acc];
    pthread_rwlock_unlock(&account_locks[acc]);
    return 0;
}

/*
 * Withdraw "amount" from account "acc".
 *
 * Returns:
 *   0   on success
 *  -1   if invalid account or other error
 *  -2   if insufficient funds
 */
static int account_withdraw(int acc, long amount)
{
    if (!account_valid(acc) || amount < 0) return -1;
    if (pthread_rwlock_wrlock(&account_locks[acc]) != 0) {
        return -1;
    }
    int rc = 0;
    if (accounts[acc] < amount) {
        rc = -2; /* insufficient funds */
    } else {
        accounts[acc] -= amount;
    }
    pthread_rwlock_unlock(&account_locks[acc]);
    return rc;
}


/*
 * Deposit "amount" into account "acc".
 *
 * Returns:
 *   0   on success
 *  -1   if invalid account or other error
 */
static int account_deposit(int acc, long amount)
{
    if (!account_valid(acc) || amount < 0) return -1;
    if (pthread_rwlock_wrlock(&account_locks[acc]) != 0) {
        return -1;
    }
    accounts[acc] += amount;
    pthread_rwlock_unlock(&account_locks[acc]);
    return 0;
}

/*
 * Transfer "amount" from account "from" to account "to".
 *
 * Returns:
 *   0   on success
 *  -1   if invalid accounts or other error
 *  -2   if insufficient funds in "from"
 *
 * To avoid deadlocks when two threads transfer between the same pair of
 * accounts in opposite directions, we always lock the lower-index account
 * first and the higher-index account second.
 */
static int account_transfer(int from, int to, long amount)
{
    if (amount < 0) return -1;
    if (!account_valid(from) || !account_valid(to)) return -1;

    if (from == to) {
        /* No-op transfer, succeeds without locking to avoid weirdness. */
        return 0;
    }

    int first = (from < to) ? from : to;
    int second = (from < to) ? to : from;

    if (pthread_rwlock_wrlock(&account_locks[first]) != 0) {
        return -1;
    }
    if (pthread_rwlock_wrlock(&account_locks[second]) != 0) {
        pthread_rwlock_unlock(&account_locks[first]);
        return -1;
    }

    int rc = 0;
    if (accounts[from] < amount) {
        rc = -2; /* insufficient funds */
    } else {
        accounts[from] -= amount;
        accounts[to] += amount;
    }

    pthread_rwlock_unlock(&account_locks[second]);
    pthread_rwlock_unlock(&account_locks[first]);

    return rc;
}

/* -------------------- Queue helpers -------------------- */

/* Initialize an empty client queue with its mutex and condition variable. */
static void queue_init(client_queue_t *q)
{
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->busy  = 0;
    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        die("pthread_mutex_init");
    }
    if (pthread_cond_init(&q->cond, NULL) != 0) {
        die("pthread_cond_init");
    }
}

/*
 * Push a new client fd into the queue.
 * If the queue is full, drop the client (close fd) and print a message.
 */
static void queue_push(client_queue_t *q, int fd)
{
    pthread_mutex_lock(&q->mutex);

    if (q->count >= QUEUE_CAPACITY) {
        /* Should not happen, but be safe. */
        fprintf(stderr, "Queue full, dropping client\n");
        close(fd);
        pthread_mutex_unlock(&q->mutex);
        return;
    }

    q->fds[q->tail] = fd;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}


/*
 * Pop the next client fd from the queue.
 * If the queue is empty, waits on the condition variable.
 *
 * The special value fd = -1 is used as a "shutdown sentinel" to tell
 * a desk thread to exit its loop.
 */
static int queue_pop(client_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    int fd = q->fds[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;

    if (fd >= 0) {
        q->busy = 1;
    }

    pthread_mutex_unlock(&q->mutex);
    return fd;
}

/* -------------------- Command handling -------------------- */


/*
 * Parse one client command line (without interpreting 'q' or 'x').
 * Fills "resp" with the server's response line (ending with '\n').
 * The response always starts with "ok:" or "fail:".
 */
static void format_response(const char *line, char *resp, size_t resp_size)
{
    /* line contains the command from client, including newline */
    if (line[0] == 'l') { /* list balance */
        int acc = -1;
        if (sscanf(line, "l %d", &acc) != 1 || !account_valid(acc)) {
            snprintf(resp, resp_size, "fail: bad account\n");
            return;
        }
        long bal = 0;
        if (account_get_balance(acc, &bal) != 0) {
            snprintf(resp, resp_size, "fail: internal error\n");
        } else {
            snprintf(resp, resp_size, "ok: balance %ld\n", bal);
        }
    } else if (line[0] == 'w') { /* withdraw */
        int acc = -1;
        long amount = 0;
        if (sscanf(line, "w %d %ld", &acc, &amount) != 2 || !account_valid(acc)) {
            snprintf(resp, resp_size, "fail: bad withdraw command\n");
            return;
        }
        int rc = account_withdraw(acc, amount);
        if (rc == 0) {
            long bal;
            account_get_balance(acc, &bal);
            snprintf(resp, resp_size, "ok: withdrew %ld, balance %ld\n", amount, bal);
            log_write("W %d %ld %ld\n", acc, amount, bal);
        } else if (rc == -2) {
            snprintf(resp, resp_size, "fail: insufficient funds\n");
        } else {
            snprintf(resp, resp_size, "fail: internal error\n");
        }
    } else if (line[0] == 'd') { /* deposit */
        int acc = -1;
        long amount = 0;
        if (sscanf(line, "d %d %ld", &acc, &amount) != 2 || !account_valid(acc)) {
            snprintf(resp, resp_size, "fail: bad deposit command\n");
            return;
        }
        int rc = account_deposit(acc, amount);
        if (rc == 0) {
            long bal;
            account_get_balance(acc, &bal);
            snprintf(resp, resp_size, "ok: deposited %ld, balance %ld\n", amount, bal);
            log_write("D %d %ld %ld\n", acc, amount, bal);
        } else {
            snprintf(resp, resp_size, "fail: internal error\n");
        }
    } else if (line[0] == 't') { /* transfer */
        int from = -1, to = -1;
        long amount = 0;
        if (sscanf(line, "t %d %d %ld", &from, &to, &amount) != 3 ||
            !account_valid(from) || !account_valid(to)) {
            snprintf(resp, resp_size, "fail: bad transfer command\n");
            return;
        }
        int rc = account_transfer(from, to, amount);
        if (rc == 0) {
            long bal_from, bal_to;
            account_get_balance(from, &bal_from);
            account_get_balance(to, &bal_to);
            snprintf(resp, resp_size,
                     "ok: transferred %ld from %d (bal %ld) to %d (bal %ld)\n",
                     amount, from, bal_from, to, bal_to);
            log_write("T %d %d %ld %ld %ld\n",
                      from, to, amount, bal_from, bal_to);
        } else if (rc == -2) {
            snprintf(resp, resp_size, "fail: insufficient funds\n");
        } else {
            snprintf(resp, resp_size, "fail: internal error\n");
        }
    } else {
        /* Anyother command (not q or x because they handled separately) */
        snprintf(resp, resp_size, "fail: unknown command\n");
    }
}

/*
 * Handle one client over its socket:
 * - Send "READY\n" when the client reaches a "desk".
 * - Then read commands line by line.
 * - 'x' is an internal admin command: save accounts, close log, exit server.
 * - 'q' tells the client session to end.
 * - Other commands are processed by format_response().
 */
static void handle_client(int client_fd)
{
    const char *ready_msg = "READY\n";
    if (write(client_fd, ready_msg, strlen(ready_msg)) < 0) {
        perror("write READY");
        return;
    }

    char line[MAX_LINE];
    char resp[MAX_LINE];

    for (;;) {
        ssize_t n = read_line(client_fd, line, sizeof(line));
        if (n <= 0) {
            fprintf(stderr, "Client disconnected\n");
            break;
        }

        if (line[0] == 'x') {
            accounts_save_to_file();
            log_close();
            const char *msg = "ok: saved accounts and server exiting\n";
            write(client_fd, msg, strlen(msg));
            _exit(0); /* terminate whole server process immediately */
    }


        if (line[0] == 'q') {
            const char *bye = "ok: bye\n";
            if (write(client_fd, bye, strlen(bye)) < 0) {
                perror("write bye");
            }
            break;
        }

        format_response(line, resp, sizeof(resp));
        if (write(client_fd, resp, strlen(resp)) < 0) {
            perror("write response");
            break;
        }
    }
}

/* -------------------- Desk threads and scheduling -------------------- */


/*
 * Thread function for each service desk.
 * It repeatedly:
 *   - Pops a client fd from its queue.
 *   - If fd == -1, exits (shutdown sentinel).
 *   - Otherwise, serves the client and closes the fd.
 */
static void *desk_thread_main(void *arg)
{
    long desk_id = (long)arg;

    fprintf(stderr, "Desk %ld thread started\n", desk_id);

    for (;;) {
        int client_fd = queue_pop(&desks[desk_id]);
        if (client_fd < 0) {
            /* Sentinel: time to shut down this desk thread */
            pthread_mutex_lock(&desks[desk_id].mutex);
            desks[desk_id].busy = 0;
            pthread_mutex_unlock(&desks[desk_id].mutex);

            fprintf(stderr, "Desk %ld shutting down\n", desk_id);
            break;
        }
        handle_client(client_fd);
        close(client_fd);
        /* Mark this desk as idle again. */
        pthread_mutex_lock(&desks[desk_id].mutex);
        desks[desk_id].busy = 0;
        pthread_mutex_unlock(&desks[desk_id].mutex);
    }

    return NULL;
}


/*
 * Choose the desk with the shortest queue (fewest waiting clients).
 * This keeps the workload roughly balanced.
 */
static int choose_desk(void)
{
    int best = 0;
    int best_load;

    pthread_mutex_lock(&desks[0].mutex);
    best_load = desks[0].count + desks[0].busy;
    pthread_mutex_unlock(&desks[0].mutex);

    for (int i = 1; i < NUM_DESKS; i++) {
        pthread_mutex_lock(&desks[i].mutex);
        int load = desks[i].count + desks[i].busy;
        pthread_mutex_unlock(&desks[i].mutex);
        if (load < best_load) {
            best = i;
            best_load = load;
        }
    }
    return best;
}


/* -------------------- Main program -------------------- */
int main(void)
{
    int listen_fd = -1;
    struct sockaddr_un addr;

    /* Initialize  accounts and load exsisting balances from disk */
    accounts_init();
    accounts_load_from_file();

    /* Open log file */
    log_open();

    /* Initialize  queues and desk threads */
    for (int i = 0; i < NUM_DESKS; i++) {
        queue_init(&desks[i]);
    }

    pthread_t threads[NUM_DESKS];
    for (int i = 0; i < NUM_DESKS; i++) {
        if (pthread_create(&threads[i], NULL, desk_thread_main, (void *)(long)i) != 0) {
            die("pthread_create");
        }
    }

    /* Create listening socket */
    if ((listen_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        die("socket");
    }
    listen_fd_global = listen_fd;

    /* Install signal handlers for SIGINT and SIGTERM */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("sigaction SIGINT");
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("sigaction SIGTERM");
    }

    /* Bind and listen on the socket path. */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    /* Remove any old socket file */
    unlink(SOCK_PATH);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        die("bind");
    }

    if (listen(listen_fd, 16) < 0) {
        die("listen");
    }

    fprintf(stderr, "ThreadBank server listening on %s with %d desks\n",
            SOCK_PATH, NUM_DESKS);

    /* Main loop:  accept clients and assign each to the shortest desk queue. */
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                if (shutting_down) {
                    /* Interrupted because of signal and we are shutting down */
                    break;
                }
                continue;
            }
            if (shutting_down && (errno == EBADF || errno == EINVAL)) {
                /* listen_fd was closed by the signal handler */
                break;
            }
            die("accept");
        }

        if (shutting_down) {
            /* We got a client just as we are shutting down: close it immediately */
            close(client_fd);
            break;
        }

        int desk_id = choose_desk();
        fprintf(stderr, "New client assigned to desk %d\n", desk_id);
        queue_push(&desks[desk_id], client_fd);
    }

    fprintf(stderr, "Server shutting down, signaling desk threads...\n");

    /* Wake up desk threads so they can exit. */
    for (int i = 0; i < NUM_DESKS; i++) {
        queue_push(&desks[i], -1);
    }

    /* Wait for all desks to finish. */
    for (int i = 0; i < NUM_DESKS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Save accounts and close log on clean shutdown. */
    accounts_save_to_file();
    log_close();

    if (listen_fd >= 0) {
        close(listen_fd);
    }
    unlink(SOCK_PATH);
    return 0;
}

