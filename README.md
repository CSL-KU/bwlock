BWLOCK: memory bandwidth control API 

Heechul Yun <heechul.yun@ku.edu>
Waqar Ali   <wali@ku.edu>

In multicore platforms, memory intensive code sections of a program can be significantly 
delayed by other memory intensive programs running in parallel due to memory bandwidth contention. 
BWLOCK provides explicit ways to mitigate memory bandwidth contention to protect performance of 
real-time applications without any special hardware support. 

Preparation
===========

Patch the kernel (v4.0) as follows. 

```
   $ patch -p1 < bwlock-4.0.patch 
```

Configure the kernel to enable BWLOCK.

```
   CONFIG_BWLOCK=y
```

Install & boot the BWLOCK enabled kernel. 

Build & install the bwlock kernel module
```
   $ make
```

This will generate the "exe" directory containing the bandwidth lock kernel module. Go into this directory and install the kernel module using the following command:

```
   $ cd exe
   $ sudo insmod bwlockmod.ko 
```
In order to remove the generated files, do the following:

```
   $ make clean
```

Usage
==========

## fine-grain locking

To support fine-grain bw locking, you need to modify the program to use 
bw_lock()/bw_unlock(). 

```
  #include "bwlock.h"
  // register the current process (syscall)
  bwlock_register (getpid());
  
  // request the bwlock (library call, not syscall)
  bw_lock()
  
  // memory performance critical section
  
  // release the bwlock (library call, not syscall)
  bw_unlock()
  
  // unregister the current process (syscall)
  bwlock_unregister();

```

Publication
=============

Heechul Yun, Waqar Ali, Santosh Gondi, Siddhartha Biswas. "Protecting Memory-Performance Critical Sections in Soft Real-Time Applications," IEEE Transactions on Computer Brief, 2017 [pdf](http://ittc.ku.edu/~heechul/papers/bwlock-tc2017.pdf).



