# Simplified-OS-Kernel

This project creates an extension of an operating system focusing on expanding user-level capabilities and kernel features. This is supposed to be a simplified version of an OS kernel, which still holds a lot of the functionalities like system calls, preemption, inter-process communication, virtual memory, signal handling, etc.

## Physical Address Space

| Address Range                   | Description         |
|---------------------------------|---------------------|
| 0x00000000 - kConfig.memSize    | DRAM                |
| kConfig.ioAPIC - ... + 0x000    | the IOPIC device    |
| kConfig.localAPIC - ... + 0x000 | the LAPIC device    |


## Virtual Address Space

| Address Range                    | Description                          |
|----------------------------------|--------------------------------------|
| 0x00001000 - kConfig.memSize     | identity mapped kernel, read/write   |
| 0x80000000 - 0xF0000000          | per-process, on-demand, user RW      |
| 0xF0000000 - 0xF0001000          | shared between all user processes    |
| kConfig.ioAPIC - ... + 0x1000    | identity mapped kernel, read/write   |
| kConfig.localAPIC - ... + 0x1000 | identity mapped kernel, read/write   |
| *everything else*                | no valid mapping                     |

User code should not be able to access anything outside the user
range (0x80000000 - 0xF0001000). Any attempt to do so should
force the faulting process to exit and store the faulting
address at (0xF0000800)

## System Calls

- [999] int join() 
    ... block until the last forked child exits
    ... the returned value is the exit code for the child
    ... returns -1 if no children
    ... a process that gets an unhanded page fault is 
        forced to exit with code 139
    ... keeps a stack of children, each call to join
        pops a child off of that stack

- [1001] unsigned int sem(unsigned int n)
    ... create an in-kernel semaphore initialized to 'n'
    ... returns its in-kernel address (cast to int) [bad idea]
    ... returns 0 on error

- [1002] void up(unsigned int)
    ... the argument must be a value returned by a call to "sem"
    ... calls up on the corresponding semaphore

- [1003] void down(unsigned int)
    ... the argument must be a value returned by a call to "sem"
    ... calls down on the corresponding semaphore and blocks
        the calling process until the down succeeds

- [1004] simple_signal(void (*handler)(int, unsigned int))

    * a simplification of the Unix signal system call.

    * gives a user process a chance to handle its own exceptions by
      registering a user space handler

    * the only exception we handle is 'segmentation violation' as above

- [1005] void* simple_mmap(void* addr, unsigned size, int fd, unsigned offset)

    * a simplification of the Unix mmap system call
    * creates a lazily loaded, zero-filled region in the calling process 
      virtual address space
    * returns the mapped virtual address on success
    * returns 0 on failure
    * fails if any of the following is true:
        . addr is not page aligned
        . size is not page aligned
        . the desired region is not fully accessible in user mode
        . the desired region intersects with an existing mapping
    * the mapped region will be of the given 'size'
    * if 'addr' is 0, does a first-fit search in the user accessible portion
      of the process virtual address space. Returns the allocated virtual
      address
    * if 'addr' is not zero, the mapped region starts at the given 'addr'
    * if offset is not page aligned, fail and return 0
    * if fd does not refer to an valid file descriptor, fail and return 0
    * create a private virtual mapping but fill it with the
      contents of the file starting at 'offset'
    * if size > (len(file)-offset) then fill the rest with zero

- [1006] void sigreturn()

    * called from a signal handler to resume normal execution of the process
    * does nothing if called from outside a signal handler

- [1007] int sem_close(int sem)

    * return -1 if sem doesn't refer to a valid semaphore; one that was
      either created by the calling process of inherited from a parent
    * marks the semaphore as being invalid for the calling process. Other
      processes can continue to use it
    * should garbage collect a semaphore when all processes that have access
      to it either exit or close it

- [1008] int simple_munmap(void* addr)

    * a simplification of Unix munmap
    * marks the region containing 'addr' as invalid and frees its resources
    * fails if any of the following is true:
        - addr is outside the process private range (0x80000000-0xF0000000)
        - addr is not in the middle of a previously mapped region
    * return 0 on success and -1 on failure

- signal handlers don't have to call sigreturn() directly. A handler that
  returns behaves as if it called sigreturn().

### System Calls -  File descriptors

    * A process can have up to 10 open file descriptor (0 .. 9)
    * file descriptors are inherited on fork and preserved across exec
    * kernelInit initializes the file descriptors for the first process
      as follows:

         - 0: all read/write attempts fail (return -1)
         - 1: reads fail, writes go to the terminal
         - 2: reads fail, writes go to the terminal

- [1020] void chdir(char* path)

    * changes the current working directory of the calling process to 'path'
    * path doesn't have to be valid
    * CWD is initialized to '/'
    * path may be relative to the current CWD

- [1021] int open(char* path)

    * path could either be absolute or relative
    * opens the file at the given path and returns the lowest available
      descriptor
    * all errors returns -1 (path not valid, path not found, no available
      file descriptors)
    * it is legal to open a directory

- [1022] int close(fd)

    * closes the given file descriptor
    * returns 0 on success and -1 on failure
    * fails if the file descriptor doesn't refer to an open file
    * closing a mmapped file descriptor does not invalidate the mapping

- [1023] int len(fd)

    * returns the length (in bytes) of the file referred to by fd
    * return -1 on failure
    * fails if the file descriptor doesn't refer to a regular file
      (normally this would be implemented in terms of lseek, so it
       would fail on non-seekable files -- pipes, directories, etc)

- [1024] int n = read(fd, void* buffer, unsigned count)

    * reads up to 'count' bytes from a file into buffer
    * failures are indicated by n == -1
        - buffer is not a valid user-accessible virtual address
        - fd doesn't refer to a readable file descriptor
        - directories are not readable
    * n == 0 iff at end of file or count == 0
    * otherwise 0 < n <= count
    * file descriptors store an internal offset; a second call
      to read will read bytes starting just after the end of the
      previously read bytes.

- [1025] (or [1]) int n = write(fd, void* buffer, unsigned count)

    * writes up to 'count' bytes from buffer to the indicated file
    * failures are indicated by n == -1
        - buffer is not a valid user-accessible virtual address
        - fd doesn't refer to a writable file descriptor
        - files on the ext2 filesystem are not currently writable
        - directories are not writable
    * n == 0 iff count == 0
    * otherwise, 0 < n <= count

- [1026] int rc = pipe(int* write_fd, int* read_fd)

    * creates an in-kernel bounded buffer of size 100 bytes
    * returns 0 on success and -1 on failure
    * allocates two files descriptors, the lowest available (write_fd < read_fd)
    * writes the two descriptors into the pointers passed to the syscall
        * writing to *write_fd stores bytes in the bounded buffer
        * reading from *read_fd retrieves bytes from the bounded buffer
        * reading from *write_fd should fail
        * writing to *read_fd should fail

- [1027] int kill(unsigned v)

    * sends signal #2 to the youngest child
    * if the child has a handler then 'v' is passed as the 'arg'
    * if the child doesn't have a handler, then it is forced to
      exit with status 'v'

    * kill does not block
    * returns 0 on success and -1 on failure
    * suceeds if the child exists and is alive at the point kill was called
    * fails if there are no remaining children, or if the youngest child has
      already exited
    * does not modify the stack of children

- [1028] int dup(int fd)

    * allocates a new file descriptor, the lowest available
    * makes the new file descriptor point to the same file/pipe/etc
      as the provided fd
    * the internal offset used by read is shared between duped file descriptors
    * returns 0 on success and -1 on failure
    * fails if:
        * there are no available file descriptors
        * if fd is not a valid file descriptor

## File Structure

- kernel/          contains the kernel files

- <test>.dir/      the contents of the root disk

- <test>.dir/sbin
    init.c         ... init source
    libc.c/libc.h  ... minimal libc implementation
    sys.S/sys.h    ... user-side system calls
    init           ... the ELF init file packaged in t0.img

## Testing

### To run tests

    make -s clean test

### To run one test

    make -s t0.test

### To run by hand

    ./run_qemu t0

### To attach with gdb

    ./debug_qemu t0