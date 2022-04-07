#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <err.h>
#include <sched.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include <linux/io_uring.h>
#include <liburing.h>

/* Try to keep the queue size to under one page as internally its stored in
 * the kernel as a contiguously ordered page. Basically the bigger you make it
 * the higher order it becomes and the less likely you'll have the contiguous
 * pages to support it, despite not hitting any user limits.
 * Really, its just preventing an ENOMEM here by keeping the queue size as
 * order 0.
 * Ring size internally is rougly 24 bytes per entry plus overheads I haven't
 * accounted for.
 */
#define QUEUE_SIZE 256

static volatile int pending = 0;
static volatile int total_files = 0;

/* Probe uring and check if action is supported inside
 * the kernel */
static void probe_uring(
    struct io_uring *ring)
{
  struct io_uring_probe *pb = {0};

  pb = io_uring_get_probe_ring(ring);

  /* Can we perform IO uring unlink in this kernel ? */
  if (!io_uring_opcode_supported(pb, IORING_OP_UNLINKAT)) {
    free(pb);
    errno = ENOTSUP;
    err(EXIT_FAILURE, "Unable to configure uring");
  }

  free(pb);
}


/* Place a unlink call for the specified file/directory on the ring */
static int submit_unlink_request(
    int dfd,
    const char *fname,
    struct io_uring *ring)
{
  char *fname_cpy = strdup(fname);
  struct io_uring_sqe *sqe = NULL;

  /* Fetch a free submission entry off the ring */
  sqe = io_uring_get_sqe(ring);
  if (!sqe)
    /* Submission queue full */
    return 0;

  pending++;
  /* Format the unlink call for submission */
  io_uring_prep_rw(IORING_OP_UNLINKAT, sqe, dfd, fname_cpy, 0, 0);
  sqe->unlink_flags = 0;

  /* Set the data to just be a filename copy. Useful for debugging
   * at a later point */
  io_uring_sqe_set_data(sqe, fname_cpy);

  return 1;
}


/* Submit the pending queue, then consume the queue
 * clearing up room on the completion queue */
static void consume_queue(
    struct io_uring *ring)
{
  char *fn;
  int i = 0, bad = 0;
  int rc;
  struct io_uring_cqe **cqes = NULL;

  if (pending < 0)
    abort();

  cqes = calloc(pending, sizeof(struct io_uring_cqe *));
  if (!cqes)
    err(EXIT_FAILURE, "Cannot find memory for CQE pointers");

  /* Fetch submitted entries from the queue (this is a async call) */
  io_uring_submit(ring);

  /* We can immediately take a peek to see if we've anything completed */
  rc = io_uring_peek_batch_cqe(ring, cqes, pending);

  /* Iterate the list of completed entries. Check nothing crazy happened */
  for (i=0; i < rc; i++) {
    /* This returns the filename we set earlier */
    fn = io_uring_cqe_get_data(cqes[i]);

    /* Check the error code of the unlink calls */
    if (cqes[i]->res < 0) {
      errno = -cqes[i]->res;
      warn("Unlinking entry %s failed", fn);
      bad++;
    }

    /* Clear up our CQE */
    free(fn);
    io_uring_cqe_seen(ring, cqes[i]);
  }

  pending -= rc + bad;
  total_files += rc - bad;
}

int main(
    const int argc,
    const char **argv)
{
  struct io_uring ring = {0};
  struct stat st = {0};
  DIR *target = NULL;
  int dfd;
  struct dirent *fn;

  /* Check initial arguments passed make sense */
  if (argc < 2)
    errx(EXIT_FAILURE, "Must pass a directory to remove files from.");

  /* Check path validity */
  if (lstat(argv[1], &st) < 0)
    err(EXIT_FAILURE, "Cannot access target directory");

  /* Sanity check */
  if (strcmp(argv[1], ".") == 0)
    errx(EXIT_FAILURE, "Wont remove from the current working directory");

  if (!S_ISDIR(st.st_mode)) 
    errx(EXIT_FAILURE, "Path specified must be a directory");

  /* Open the directory */
  target = opendir(argv[1]);
  if (!target)
    err(EXIT_FAILURE, "Opening the directory failed");
  dfd = dirfd(target);

  /* Create the initial uring for handling the file removals */
  if (io_uring_queue_init(QUEUE_SIZE, &ring, 0) < 0)
    err(EXIT_FAILURE, "Cannot initialize URING");

  /* Check the action is supported */
  probe_uring(&ring);

  /* So as of writing this code, GETDENTS doesn't have URING support.
   * but checking the kernel mailing list indicates its in progress.
   * For now, we'll just do laymans readdir(). These days theres no 
   * actual difference between it and making the getdents() call ourselves.
   */
  while (fn = readdir(target)) {
    if (fn->d_type != DT_REG)
      /* Pay no attention to non-files */
      continue;

    /* Add to the queue until its full, try to consume it
     * once its full. 
     */
    while (!submit_unlink_request(dfd, fn->d_name, &ring)) {
      /* When the queue becomes full, consume queued entries */
      consume_queue(&ring);
      /* This yield is here to give the uring a chance to 
       * complete pending requests */
      sched_yield();
      continue;
    }
  }

  /* Out of files in directory to list. Just clear the queue */
  while (pending) {
    consume_queue(&ring);
    sched_yield();
  }

  printf("Total files: %d\n", total_files);

  io_uring_queue_exit(&ring);
  closedir(target);
  exit(0);
}
