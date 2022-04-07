dentls
=======

This is the software posed to answer the serverfault questtion: https://serverfault.com/questions/183821/rm-on-a-directory-with-millions-of-files/328305#328305

Its a good showcase to using both BPF tracing facilities on Linux and the newer IO_Uring subsystem.

io_uring is a pretty revolutionary model on Linux for performing asyncronous work on Linux. It lets you batch various syscalls in a manner not possible before and attempts to make many of the system calls asyncronous. Many syscalls, particularly those pertaining to IO yield the CPU in the caller which may not be ideal if there exists other work that can be done immediately.

# Purpose

This program mass-unlinks files to measure the performance of and answer the question fo IO performance for unlink in Linux.
As a useful utility, its really limited.

# Implementation

This uses IO_uring and thats the interested part of this program.
There is a submission phase and consumption phase.

Entries are read from the readdir() call and submitted to the ring until the ring is full.
The consumption phase processes the results, ultimately until there are no more directory entries left to consume.

Traditionally, one would have to use a niave loop to perform this task, when in a singular thread. (while (1) unlink(...); )
Whilst its possible to emulate this behaviour using pthreads (spawn a thread and a conditional, wait for a pthread_cond_signal or use a work queue) this particular implementation allows for internal kernel optimizations. Also -- submitting system calls itself is a yield point on Linux and you can cause your program to preempt.

IO_uring actually offers a completely unpremptible mechanisms by which a internal kernel thread will scan the ring buffer continually, so you never have to yeild using the syscall io_uring_enter. Very fancy!
