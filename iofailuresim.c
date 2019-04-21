#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

const static int kMaxFd = 1024;

struct Buf {
    void* data;
    size_t len;
};

// For each file descriptor, holds a buffer of written but unsynced data.
// Then when the process is killed, unsynced writes will be dropped, which
// simulates system crash behavior.
static struct Buf* fd_to_buf = NULL;

// Lock needed to handle concurrent writes and syncs.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int crash_failure_one_in = -1;
static int sync_failure_one_in = -1;
static bool crash_after_sync_failure = false;
static int num_syncs_until_crash = -1;

void maybe_init() {
    pthread_mutex_lock(&lock);
    if (fd_to_buf != NULL) {
        pthread_mutex_unlock(&lock);
        return;
    }
    fd_to_buf = (struct Buf*)malloc(kMaxFd * sizeof(struct Buf));
    assert(fd_to_buf != NULL);
    memset(fd_to_buf, 0, kMaxFd * sizeof(struct Buf));

    char* getenv_res;
    if ((getenv_res = getenv("CRASH_FAILURE_ONE_IN")) != NULL) {
        crash_failure_one_in = atoi(getenv_res);
    }
    if ((getenv_res = getenv("SYNC_FAILURE_ONE_IN")) != NULL) {
        sync_failure_one_in = atoi(getenv_res);
    }
    if ((getenv_res = getenv("CRASH_AFTER_SYNC_FAILURE")) != NULL) {
        if (atoi(getenv_res) == 1) {
            crash_after_sync_failure = true;
        }
    }
    srandom(time(NULL));
    pthread_mutex_unlock(&lock);
}

// Forwards to the C standard library's `write()`
ssize_t libc_write(int fd, const void* buf, size_t count) {
    static ssize_t (*_libc_write)(int fd, const void *buf, size_t count) = NULL;
    pthread_mutex_lock(&lock);
    if (_libc_write == NULL) {
        _libc_write = dlsym(RTLD_NEXT, "write");
    }
    pthread_mutex_unlock(&lock);
    return _libc_write(fd, buf, count);
}

// Intercepts calls to `write()` and, for regular files, buffers the data in
// `fd_to_buf[fd]`.
ssize_t write(int fd, const void *buf, size_t count) {
    maybe_init();
    assert(fd < kMaxFd);
    if (count == 0 || buf == NULL) {
        return 0;
    }

    // Non-regular file could be socket, etc. We need to let those writes pass
    // through immediately in order for the database to function.
    struct stat statbuf;
    memset(&statbuf, 0, sizeof(struct stat));
    int status = fstat(fd, &statbuf);
    assert(status == 0);
    if (!S_ISREG(statbuf.st_mode)) {
        return libc_write(fd, buf, count);
    }

    // For regular file, append to the buffer of written but unsynced data.
    pthread_mutex_lock(&lock);
    size_t old_len = fd_to_buf[fd].len;
    fd_to_buf[fd].len += count;
    fd_to_buf[fd].data = realloc(fd_to_buf[fd].data, fd_to_buf[fd].len);
    assert(fd_to_buf[fd].data != NULL);
    memcpy(fd_to_buf[fd].data + old_len, buf, count);
    pthread_mutex_unlock(&lock);
    return count;
}

// We are using process crash to simulate system crash for tests and don't
// expect these tests to face actual system crashes. So for "syncing" it is
// sufficient to make a `write()` syscall since that'll push data into page
// cache, allowing it to survive a process crash.
int fsync(int fd) {
    maybe_init();
    pthread_mutex_lock(&lock);
    if (num_syncs_until_crash > 0) {
        --num_syncs_until_crash;
        if (num_syncs_until_crash == 0) {
            kill(getpid(), SIGKILL);
        }
    }
    if (fd_to_buf[fd].data == NULL) {
        pthread_mutex_unlock(&lock);
        return 0;
    }

    struct Buf old_buf = fd_to_buf[fd];
    fd_to_buf[fd].data = NULL;
    fd_to_buf[fd].len = 0;

    int ret;
    if (crash_failure_one_in > 0 && random() % crash_failure_one_in == 0) {
        kill(getpid(), SIGKILL);
    } else if (sync_failure_one_in > 0 && random() % sync_failure_one_in == 0) {
        if (num_syncs_until_crash == -1) {
            // This was the first failure. Start the countdown.
            num_syncs_until_crash = 10;
        }
        pthread_mutex_unlock(&lock);
        ret = -1;
    } else {
        // It should be fine to buffer new writes while we're syncing old ones.
        pthread_mutex_unlock(&lock);
        while (old_buf.len > 0) {
            ssize_t ret = libc_write(fd, old_buf.data, old_buf.len);
            assert(ret > 0);
            old_buf.len -= (size_t)ret;
        }
        ret = 0;
    }
    free(old_buf.data);
    old_buf.data = NULL;
    return ret;
}

// Currently we only buffer data writes so `fdatasync()` and `fsync()` can
// behave the same.
int fdatasync(int fd) {
    fsync(fd);
}
