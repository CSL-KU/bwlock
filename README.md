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
   $ sudo insmod bwlockmod.ko 

```

Usage
==========

## fine-grain locking

To support fine-grain bw locking, you need to modify the program to use 
bw_lock()/bw_unlock(). 

```
  #include "bwlock.h"

  bwlock_register (pid);
  ...  
  bw_lock()
  ...
  bw_unlock()
  ...
  bwlock_unregister();

```

Publication
=============

Heechul Yun, Santosh Gondi, Siddhartha Biswas. "Protecting Memory-Performance Critical Sections in Soft Real-Time Applications," In submission. [(pdf)](http://www.ittc.ku.edu/~heechul/papers/bwlock-submitted.pdf)



