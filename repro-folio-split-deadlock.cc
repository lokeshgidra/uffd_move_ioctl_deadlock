#include <sys/types.h>
#include <stdio.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <time.h>
#include <poll.h>
#include <stdint.h>
#include <assert.h>

#include <atomic>

#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE); \
                        } while (0)
#define KB (1024)
#define MB (KB * 1024)
#define PAGE_SIZE (4 * KB)

int page_size;
int uffd;
unsigned long len = 4 * MB;
char * volatile to;
char * volatile from;
std::atomic<int> signal_threads;
std::atomic<int> completed;

void move_ioctl(char* dst, char* src) {
   struct uffdio_move uffdio_move;
  
   uffdio_move.src = (unsigned long)src;
   /* We need to handle page faults in units of pages(!).
      So, round faulting address down to page boundary */
   uffdio_move.dst = (unsigned long)dst;
   uffdio_move.len = page_size;
   uffdio_move.mode = 0;
   uffdio_move.move = 0;
retry:
   if (ioctl(uffd, UFFDIO_MOVE, &uffdio_move)) {
     if (errno == EEXIST || errno == ENOENT)
       return;
     if (errno == EAGAIN) {
       uffdio_move.move = 0;
       goto retry;
     }
     errExit("ioctl-UFFDIO_COPY");
   }
}

void sigbus_handler(int sig, siginfo_t* info, void* ctxt) {
   assert(sig == SIGBUS && ctxt); 
  
   unsigned long offset = ((char*)info->si_addr - to) & ~(page_size - 1);
   move_ioctl(to + offset, from + offset);
}

void* start_routine(void* arg) {
  assert(arg == NULL);
  while (true) {
    while (signal_threads.load() == 0);
    if (signal_threads.load() == 2)
      break;

    for (int i = 0; i < len; i += page_size) {
      if (to[i] != 'a')
        errExit("corruption");
    }
    completed++;
  }
  return NULL;
}

int main (int argc, char *argv[]) {
  pthread_t* thread_ids;

  if (argc < 4) {
    fprintf(stderr, "Usage: %s <0 no-THP|1 THP> <#threads> <#iters>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  page_size = sysconf(_SC_PAGE_SIZE);
  assert(page_size == PAGE_SIZE);
  int use_thp = strtoul(argv[1], NULL, 0);
  int num_threads = strtoul(argv[2], NULL, 0);
  int iters = strtoul(argv[3], NULL, 0);

  int thp_behavior = use_thp ? MADV_HUGEPAGE : MADV_NOHUGEPAGE;
  thread_ids = (pthread_t*)calloc(num_threads, sizeof(pthread_t));
  /* Create and enable userfaultfd object */
  uffd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
  if (uffd == -1)
    errExit("userfaultfd");

  {
    struct uffdio_api uffdio_api;
    uffdio_api.api = UFFD_API;
    uffdio_api.features = UFFD_FEATURE_SIGBUS;
    if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
      errExit("ioctl-UFFDIO_API");

    if ((uffdio_api.features & UFFD_FEATURE_MOVE) == 0)
      errExit("MOVE no available");
  }

  for (char* addr = (char*)0x23400000; ; addr += 2 * MB) {
    from = (char* volatile) mmap(addr, len, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (from == MAP_FAILED) {
      if (errno == EEXIST)
	continue;
      errExit("from-space mmap");
    }
    if (madvise(from, len, thp_behavior))
      errExit("madvise-(NO)HUGEPAGE");

    for (int i = 0; i < len; i += page_size) {
      from[i] = 'a';
    }
    break;
  }

  /* Create a private anonymous mapping. The memory will be
     demand-zero paged--that is, not yet allocated. When we
     actually touch the memory, it will be allocated via
     the userfaultfd. */
  to = (char* volatile) mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (to == MAP_FAILED)
    errExit("to-space mmap");
  if (madvise(to, len, MADV_NOHUGEPAGE))
    errExit("madvise-NOHUGEPAGE");

  printf("Address returned by mmap() = from:%p, to:%p\n", from, to);

  {
    struct sigaction act;
    memset(&act, '\0', sizeof(act));
    act.sa_flags = SA_SIGINFO | SA_RESTART;
    act.sa_sigaction = sigbus_handler;
    if (sigaction(SIGBUS, &act, NULL)) {
      errExit("sigaction-SIGBUS");
    }
  }

  {
    struct uffdio_register uffdio_register;
    /* Register the memory range of the mapping we just created for
       handling by the userfaultfd object. In mode, we request to track
       missing pages (i.e., pages that have not yet been faulted in). */
    uffdio_register.range.start = (unsigned long) to;
    uffdio_register.range.len = len;
    uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1)
      errExit("ioctl-UFFDIO_REGISTER");
  }

  signal_threads = 0;
  for (int i = 0; i < num_threads; i++) {
    int s = pthread_create(thread_ids + i, NULL, start_routine, NULL);
    if (s != 0) {
      errno = s;
      errExit("pthread_create");
    }
  }

  for (int i = 0; i < iters; i++) {
    completed = 0;
    if (use_thp && madvise(from, len, MADV_COLLAPSE))
      errExit("madvise-COLLAPSE");

    signal_threads = 1;

    for (int j = 0; j < len; j += page_size) {
      move_ioctl(to + j, from + j);
    }

    signal_threads = 0;
    while (completed.load() < num_threads);
    printf("iteration %d complated\n", i);
    // mremap to prepare for next iteration
    if (mremap(to, len, len, MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, from) == MAP_FAILED)
      errExit("mremap");

    if (madvise(from, len, thp_behavior))
      errExit("madvise-HUGEPAGE");
  }

  signal_threads = 2;

  for (int i = 0; i < num_threads; i++) {
    int s = pthread_join(thread_ids[i], NULL);
    if (s != 0) {
      errno = s;
      errExit("pthread_join");
    }
  }
}
