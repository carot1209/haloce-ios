/* Darwin's spellings, for building the iOS host's runtime on Linux */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <sys/stat.h>
#include <stdint.h>
#define pthread_setname_np(name) pthread_setname_np(pthread_self(), (name))
#define st_atimespec st_atim
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
int pthread_threadid_np(void *thread, uint64_t *id);
