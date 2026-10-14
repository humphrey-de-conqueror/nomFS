# Linux Kernel Filesystem (VFS) Locking Notes

## 1. Core Concepts

A filesystem contains multiple objects with different synchronization requirements:

- `struct inode`: Represents a filesystem object and its metadata.
- `struct dentry`: Represents a name in the directory hierarchy.
- `struct super_block`: Represents a mounted filesystem instance.
- `struct file`: Represents an open file.
- `struct folio`: Represents one or more contiguous pages in the page cache.
- Filesystem-private structures: Store custom metadata and state.

There is no single lock that protects an entire inode, dentry, or filesystem. Each lock protects particular fields, operations, or invariants.

## 2. Inode Locking

### 2.1 `inode->i_rwsem`

Type: `struct rw_semaphore`

Purpose:
- Synchronizes inode operations, especially directory namespace operations.
- Supports shared and exclusive acquisition.
- Does not automatically protect every inode field.

Helpers:

- `inode_lock(inode)`: Acquire the inode lock exclusively.
- `inode_unlock(inode)`: Release the exclusive lock.
- `inode_lock_shared(inode)`: Acquire the inode lock for shared access.
- `inode_unlock_shared(inode)`: Release the shared lock.
- `inode_lock_nested(inode, subclass)`: Acquire exclusively with a lockdep nesting subclass.
- `inode_lock_shared_nested(inode, subclass)`: Acquire shared access with a lockdep nesting subclass.
- `inode_trylock(inode)`: Attempt exclusive acquisition without waiting.
- `inode_is_locked(inode)`: Check whether the inode's `i_rwsem` is locked.

Example:

    inode_lock(inode);

    /* Perform an operation protected by i_rwsem. */

    inode_unlock(inode);

### 2.2 `inode_lock_nested()`

Purpose:
- Acquires the same `i_rwsem` as `inode_lock()`.
- Supplies a lockdep subclass for lock-order analysis.
- Helps distinguish nested locking patterns.

Important:
- The subclass is not a different locking mode.
- It does not lock the entire inode.
- Use the subclass required by the relevant VFS operation.

### 2.3 `inode->i_lock`

Type: `spinlock_t`

Purpose:
- Protects particular internal inode fields and state.
- Suitable for short critical sections that cannot sleep.

Helpers:

- `spin_lock(&inode->i_lock)`
- `spin_unlock(&inode->i_lock)`
- `spin_lock_irqsave()` / `spin_unlock_irqrestore()`
- `spin_lock_nested()` / `spin_unlock()`

Example:

    spin_lock(&inode->i_lock);

    /* Modify fields protected by i_lock. */

    spin_unlock(&inode->i_lock);

Important:
- `i_lock` and `i_rwsem` are different locks.
- Do not sleep while holding a spinlock.
- Do not assume `i_lock` protects arbitrary inode fields.

## 3. Directory Locking

Directory operations can modify namespace relationships and involve multiple inodes. Correct lock ordering is essential to avoid deadlocks.

### 3.1 Common Helpers

- `inode_lock()`: Exclusively lock one inode.
- `inode_lock_nested()`: Exclusively lock an inode with a lockdep subclass.
- `inode_lock_shared()`: Acquire an inode lock for shared access.
- `lock_rename()`: Traditional VFS helper for rename locking in older kernel APIs.
- `unlock_rename()`: Release locks acquired by `lock_rename()`.

Newer kernel versions may use different rename-locking interfaces, including `start_renaming()`. Check the target kernel headers and documentation.

### 3.2 Why Rename Requires Special Locking

Consider:

    rename("/home/user/a.txt",
           "/home/user/docs/a.txt");

The operation changes two directories:
- Source parent: `/home/user/`
- Destination parent: `/home/user/docs/`

Concurrent operations must not corrupt directory relationships or create deadlocks.

The VFS rename protocol also handles directory ancestry and prevents invalid directory cycles.

Important:
- Do not lock parent directories in arbitrary order.
- Follow the VFS locking contract for the relevant inode operation.
- Do not assume every filesystem must manually call `lock_rename()`; the VFS may already establish the required locks.

## 4. Dentry Locking

### 4.1 `dentry->d_lock`

Type: `spinlock_t`, accessed through the dentry's lock infrastructure.

Purpose:
- Protects specific dentry fields and internal dentry state.
- Coordinates certain concurrent modifications to dentry metadata.

Helpers:

- `spin_lock(&dentry->d_lock)`
- `spin_unlock(&dentry->d_lock)`
- `spin_lock_nested()` for appropriate nested spinlock patterns.

Example:

    spin_lock(&dentry->d_lock);

    /* Access fields requiring d_lock protection. */

    spin_unlock(&dentry->d_lock);

Important:
- `d_lock` does not lock the entire directory.
- It does not prevent every filesystem operation involving the dentry.
- Prefer higher-level VFS interfaces over direct dentry locking.
- Verify the required protection for each field before accessing it.

### 4.2 Reference Counting Is Not Locking

- `dget(dentry)`: Acquire a dentry reference.
- `dput(dentry)`: Release a dentry reference.
- `d_lookup()`: Look up a dentry by name.

These are not interchangeable with `d_lock`.

Reference counting helps manage object lifetime. It does not, by itself, serialize concurrent modifications to the object's fields.

## 5. Filesystem-Private Data Locking

Use a mutex or spinlock when protecting custom filesystem state.

### 5.1 Mutex

Type: `struct mutex`

Use when:
- The critical section may sleep.
- The operation may block.
- The surrounding execution context permits sleeping.

Helpers:

- `mutex_lock()`
- `mutex_unlock()`
- `mutex_trylock()`
- `mutex_lock_interruptible()`

Example:

    struct mutex lock;

    /* Initialize before concurrent use. */
    mutex_init(&lock);

    mutex_lock(&lock);

    /* Modify protected state. */

    mutex_unlock(&lock);

### 5.2 Spinlock

Type: `spinlock_t`

Use when:
- The critical section must not sleep.
- The operation is short.
- The execution context requires atomic-context-safe synchronization.

Helpers:

- `spin_lock()`
- `spin_unlock()`
- `spin_lock_irqsave()`
- `spin_unlock_irqrestore()`
- `spin_lock_nested()`

Example:

    spinlock_t lock;

    spin_lock_init(&lock);

    spin_lock(&lock);

    /* Perform a short, non-blocking update. */

    spin_unlock(&lock);

Important:
- Initialize locks before concurrent access.
- Do not use mutex operations on a `spinlock_t`.
- Do not use spinlocks around operations that may sleep.
- Choose the lock based on execution context and data-protection requirements.

## 6. Other Synchronization Mechanisms

### 6.1 Read-Write Semaphore

Type: `struct rw_semaphore`

Allows multiple readers or one exclusive writer.

Helpers:
- `down_read()` / `up_read()`
- `down_write()` / `up_write()`
- `down_read_trylock()`
- `down_write_trylock()`

The inode's `i_rwsem` is an example of a read-write semaphore.

### 6.2 Read-Write Spinlock

Type: `rwlock_t`

Allows concurrent readers or an exclusive writer in short, non-sleeping critical sections.

Helpers:
- `read_lock()` / `read_unlock()`
- `write_lock()` / `write_unlock()`
- IRQ-safe variants when required.

### 6.3 Atomic Operations

Use for independent counters, flags, and supported atomic state transitions.

Examples:
- `atomic_read()`
- `atomic_set()`
- `atomic_inc()`
- `atomic_dec()`
- `atomic_cmpxchg()`

Atomic operations do not automatically protect related data or multi-step invariants.

### 6.4 Sequence Counters

Types:
- `seqcount_t`
- `seqlock_t`

Purpose:
- Allow optimistic readers to detect concurrent updates and retry.
- Useful for consistent snapshots of suitable data.

Sequence counters do not automatically manage pointer lifetime or replace all forms of locking.

### 6.5 RCU

Read-Copy-Update (RCU) supports efficient concurrent reads and controlled object reclamation.

Common helpers:
- `rcu_read_lock()` / `rcu_read_unlock()`
- `rcu_dereference()`
- `rcu_assign_pointer()`
- `synchronize_rcu()`

RCU protects specific read-side access and lifetime patterns. It is not a general-purpose mutex.

## 7. Page Cache and Folio Locking

A folio represents one or more contiguous pages in the page cache.

Common helpers:
- `folio_lock()`
- `folio_unlock()`
- `folio_trylock()`
- `folio_wait_locked()`

Purpose:
- Synchronize operations involving a particular folio.
- Coordinate certain page-cache and file-I/O operations.

Folio locking does not replace inode metadata locking or filesystem-private locking.

## 8. File Locks Versus Internal Kernel Locks

Advisory file locks coordinate access between processes according to file-locking semantics.

Examples:
- `flock()`
- `fcntl()` record locks

These are distinct from:
- Inode mutexes.
- Dentry spinlocks.
- Filesystem-private mutexes.
- Folio locks.

Application-level file locks do not automatically prevent internal filesystem data races.

## 9. Practical Selection Guide for `nomFS`

| Operation | Starting point |
|---|---|
| `.create` | Follow the VFS parent-directory locking contract. |
| `.mkdir` | Follow the VFS parent-directory locking contract. |
| `.mknod` | Follow the VFS contract for directory modification and inode creation. |
| `.unlink` | Follow the VFS unlink contract, including victim-inode locking where required. |
| `.rmdir` | Follow the VFS directory-removal contract. |
| `.rename` | Follow the target kernel's rename-locking protocol. |
| Update custom inode counters | Use atomics, a mutex, or a spinlock depending on the invariant and context. |
| Modify dentry internals | Follow the dentry-field locking requirements. |
| Read or write page-cache contents | Follow the applicable folio, page-cache, and filesystem I/O rules. |
| Update global filesystem state | Use a suitable filesystem-wide synchronization mechanism. |

## 10. Core Rules to Remember

1. Identify the exact data or invariant being protected.
2. Check whether the VFS already holds the required lock.
3. Use the documented lock ordering for operations involving multiple objects.
4. Never sleep while holding a spinlock.
5. Do not confuse object references with locks.
6. Do not assume locking one object automatically locks related objects.
7. Use lockdep subclasses only when required by the relevant locking pattern.
8. Check the documentation and source code for the exact kernel version being targeted.

## 11. Official References

- VFS locking: https://docs.kernel.org/filesystems/locking.html
- Directory locking: https://docs.kernel.org/filesystems/directory-locking.html
- Mutex design: https://docs.kernel.org/locking/mutex-design.html
- Lockdep design: https://docs.kernel.org/locking/lockdep-design.html
- Linux kernel source: `include/linux/fs.h`, `include/linux/dcache.h`, `include/linux/mutex.h`, `include/linux/spinlock.h`, and the relevant VFS implementation files.