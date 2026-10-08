# Linux VFS Notes (super_block / inode / dentry / file)

> Source tree refs: ./include/linux/fs.h, ./include/linux/dcache.h, ./fs/inode.c, ./fs/dcache.c, ./fs/super.c, ./fs/file_table.c, ./fs/namei.c, ./fs/libfs.c
> Field names change between kernel versions (verify against your tree). Notable changes:
> - `i_mutex` -> `i_rwsem` (4.7)
> - timestamps moved to accessor helpers (6.6+)
> - `mnt_idmap` arg added to many inode ops (6.3+)
> - `f_count` -> `f_ref` (newer kernels)
> - mount API moved to `fs_context` (5.2+)

---

## 0. Big picture

| Object | Struct | Represents | Lives where |
|--------|--------|------------|-------------|
| Superblock | `struct super_block` | one mounted filesystem instance | memory (+ on-disk copy) |
| Inode | `struct inode` | one file/dir/symlink/device (metadata, no name) | memory (+ on-disk copy) |
| Dentry | `struct dentry` | one path component (name -> inode link), cache | memory only |
| File | `struct file` | one *open* instance of a file (position, flags) | memory only |
| Mount | `struct vfsmount` / `struct mount` | where an fs is attached in the tree | memory only |

Chain from a process:

```
task_struct->files (files_struct)
   -> fdtable->fd[n]         // fd number indexes this array
      -> struct file         // f_pos, f_flags, f_op
         -> f_path.dentry    // name + tree position
            -> d_inode       // struct inode (metadata)
               -> i_sb       // struct super_block (the fs)
               -> i_mapping  // address_space (page cache)
         -> f_path.mnt       // struct vfsmount
```

Key point:
- many `file` -> one `dentry` (same file opened many times)
- many `dentry` -> one `inode` (hard links, or aliases)
- many `inode` -> one `super_block`

---

## 1. struct super_block

One per mounted filesystem instance (not per filesystem type).

### 1.1 Common fields

| Field | Type | Meaning |
|-------|------|---------|
| `s_dev` | `dev_t` | device identifier |
| `s_blocksize` | `unsigned long` | block size in bytes |
| `s_blocksize_bits` | `unsigned char` | log2(block size) |
| `s_maxbytes` | `loff_t` | max file size this fs supports |
| `s_type` | `struct file_system_type *` | the fs type (ext4, tmpfs...) |
| `s_op` | `const struct super_operations *` | superblock method table |
| `s_flags` | `unsigned long` | mount flags (`SB_RDONLY`, `SB_NOSUID`, ...) |
| `s_magic` | `unsigned long` | fs magic number (what `statfs` reports as `f_type`) |
| `s_root` | `struct dentry *` | root dentry of this fs |
| `s_umount` | `struct rw_semaphore` | held for write during unmount/remount, read when using the sb |
| `s_count` | `int` | passive refcount (struct itself stays allocated) |
| `s_active` | `atomic_t` | active refcount (fs is alive, mounted) |
| `s_fs_info` | `void *` | fs-private data (e.g. `ext4_sb_info`) |
| `s_inodes` | `struct list_head` | all inodes of this sb |
| `s_inode_list_lock` | `spinlock_t` | protects `s_inodes` |
| `s_bdev` | `struct block_device *` | backing block device (NULL for tmpfs/procfs) |
| `s_id` | `char[32]` | name of the device/fs ("sda1") |
| `s_uuid` | `uuid_t` | filesystem UUID |
| `s_time_gran` | `u32` | timestamp granularity in ns |
| `s_xattr` | `const struct xattr_handler **` | xattr handlers |
| `s_export_op` | `const struct export_operations *` | NFS export hooks (file handles) |
| `s_dentry_lru` / `s_inode_lru` | `struct list_lru` | LRU lists for shrinkers |
| `s_vfs_rename_mutex` | `struct mutex` | serializes cross-directory renames |

### 1.2 struct super_operations (key hooks)

| Op | Purpose |
|----|---------|
| `alloc_inode` | allocate fs-specific inode (embed `struct inode` in your own struct) |
| `free_inode` | free it (called after RCU grace period) |
| `destroy_inode` | tear down fs-specific stuff before free |
| `dirty_inode` | inode marked dirty (fs may journal it) |
| `write_inode` | write inode metadata to disk |
| `evict_inode` | last ref gone / nlink==0: truncate pages, free blocks, `clear_inode()` |
| `put_super` | unmount: release sb private resources |
| `sync_fs` | flush fs metadata to disk |
| `statfs` | fill `struct kstatfs` (used by `df`) |
| `show_options` | print mount options to /proc/self/mountinfo |
| `freeze_fs` / `unfreeze_fs` | freeze for snapshots |

### 1.3 Superblock / mount APIs

| API | Explanation |
|-----|-------------|
| `register_filesystem(fst)` | add a `file_system_type` so `mount -t` can find it (see /proc/filesystems) |
| `unregister_filesystem(fst)` | remove it (module exit) |
| `get_tree_bdev(fc, fill_super)` | mount a block-device-backed fs (finds/creates the sb, calls your `fill_super`) |
| `get_tree_nodev(fc, fill_super)` | mount an fs without a device (tmpfs-style) |
| `get_tree_single(fc, fill_super)` | single-instance fs (e.g. debugfs-like) |
| `kill_block_super(sb)` | `kill_sb` for bdev fs: shutdown + release bdev |
| `kill_anon_super(sb)` | `kill_sb` for nodev fs |
| `kill_litter_super(sb)` | same, plus drops dentries pinned by the fs (ramfs-style) |
| `generic_shutdown_super(sb)` | core teardown: evict inodes, `put_super` |
| `sb_set_blocksize(sb, size)` | set block size, returns 0 on failure |
| `sb_min_blocksize(sb, size)` | set the smallest usable blocksize >= device logical block size |
| `sb_bread(sb, blk)` | read a block via buffer cache -> `struct buffer_head *`; release with `brelse()` |
| `d_make_root(inode)` | create root dentry from root inode (puts the inode on failure); assign to `sb->s_root` |
| `sb_rdonly(sb)` | true if mounted read-only |
| `sb_start_write(sb)` / `sb_end_write(sb)` | freeze-protection around writes |
| `sync_filesystem(sb)` | flush everything on this sb |
| `iterate_supers(fn, arg)` | call `fn` for every active sb |

### 1.4 Typical mount flow (new mount API)

```
mount(2) / fsopen+fsconfig+fsmount
  -> init_fs_context()           // fs allocates fs_context, sets ops
  -> fc->ops->get_tree()         // usually get_tree_bdev(fc, myfs_fill_super)
       -> myfs_fill_super(sb, fc)
            - sb->s_magic / s_op / s_blocksize / s_fs_info
            - read on-disk sb (sb_bread)
            - root = myfs_iget(sb, ROOT_INO)
            - sb->s_root = d_make_root(root)
```

---

## 2. struct inode

Everything about a file *except its name* and its data. One inode = one file object; its `i_ino` is unique within one sb.

### 2.1 Common fields

| Field | Type | Meaning |
|-------|------|---------|
| `i_ino` | `unsigned long` | inode number (unique per sb) |
| `i_mode` | `umode_t` | file type + permission bits (`S_IFREG`, `S_IFDIR`, 0644...) |
| `i_uid` / `i_gid` | `kuid_t` / `kgid_t` | owner / group (kernel ids; use `i_uid_read()` for raw value) |
| `i_nlink` | `unsigned int` | hard link count (on-disk links) |
| `i_size` | `loff_t` | file size in bytes (use `i_size_read()` / `i_size_write()`) |
| `i_blocks` | `blkcnt_t` | number of 512-byte sectors allocated |
| `i_blkbits` | `u8` | log2(block size) |
| `i_rdev` | `dev_t` | device number if char/block special file |
| `i_atime` / `i_mtime` / `i_ctime` | timestamps | access / modify / metadata-change time (use accessor helpers on new kernels) |
| `i_sb` | `struct super_block *` | owning superblock |
| `i_op` | `const struct inode_operations *` | inode method table (dir ops, getattr, ...) |
| `i_fop` | `const struct file_operations *` | default file ops given to `struct file` on open |
| `i_mapping` | `struct address_space *` | page cache mapping (usually `&inode->i_data`) |
| `i_data` | `struct address_space` | the embedded page cache object |
| `i_rwsem` | `struct rw_semaphore` | inode lock (was `i_mutex`); serializes dir modifications, file writes |
| `i_lock` | `spinlock_t` | protects `i_state`, `i_count`, some other fields |
| `i_count` | `atomic_t` | in-memory reference count (`iget`/`iput`) |
| `i_state` | `unsigned long` | `I_NEW`, `I_DIRTY_SYNC`, `I_DIRTY_DATASYNC`, `I_DIRTY_PAGES`, `I_FREEING`, `I_CLEAR` |
| `i_flags` | `unsigned int` | `S_IMMUTABLE`, `S_APPEND`, `S_NOATIME`, `S_DAX`... |
| `i_hash` | `struct hlist_node` | link in global inode hash (lookup by sb + ino) |
| `i_sb_list` | `struct list_head` | link in `sb->s_inodes` |
| `i_lru` | `struct list_head` | link in the sb inode LRU (unused inodes) |
| `i_dentry` | `struct hlist_head` | list of dentries pointing at this inode (aliases) |
| `i_generation` | `u32` | generation number (NFS file handles) |
| `i_version` | `atomic64_t` | change counter (NFS, IMA) |
| `i_private` | `void *` | fs private pointer |
| `i_security` | `void *` | LSM blob (SELinux/AppArmor) |
| `i_writecount` | `atomic_t` | writers (negative = deny write, e.g. executing) |
| `i_pipe` / `i_cdev` / `i_link` | union | type-specific data (pipe, char device, fast symlink target) |

### 2.2 Notes

- Real filesystems embed `struct inode` inside their own struct:
```c
  struct myfs_inode_info {
      u32 my_block_ptrs[12];
      struct inode vfs_inode;   // embedded
  };
  #define MYFS_I(i) container_of(i, struct myfs_inode_info, vfs_inode)
```
- `i_nlink` (on-disk links) and `i_count` (in-memory refs) are different things. Inode is freed on disk when nlink == 0 AND no open refs remain. This is why you can `unlink()` an open file and keep reading it.
- `S_ISREG(mode)`, `S_ISDIR(mode)`, `S_ISLNK(mode)`, `S_ISCHR`, `S_ISBLK`, `S_ISFIFO`, `S_ISSOCK` test `i_mode`.
- Inode locking rule: VFS takes the *parent directory's* `i_rwsem` exclusively for create/mkdir/unlink/rmdir/rename/link, and shared for lookup and readdir (`iterate_shared`).

### 2.3 struct inode_operations (key hooks)

| Op | Purpose |
|----|---------|
| `lookup` | find name in dir: look up child, call `d_splice_alias()` (returns dentry or NULL) |
| `create` | create regular file |
| `mkdir` / `rmdir` | create / remove dir |
| `link` / `unlink` | hard link / remove name |
| `symlink` | create symlink |
| `mknod` | create device node / fifo / socket |
| `rename` | rename (flags: `RENAME_NOREPLACE`, `RENAME_EXCHANGE`, `RENAME_WHITEOUT`) |
| `get_link` | resolve symlink target (`readlink`, path walk) |
| `permission` | custom permission check (else `generic_permission`) |
| `getattr` | fill `struct kstat` for `stat(2)` |
| `setattr` | handle `chmod`/`chown`/`truncate`/`utimes` |
| `listxattr` | list extended attributes |
| `atomic_open` | lookup + create + open in one call (network fs) |
| `tmpfile` | `O_TMPFILE` support |
| `fileattr_get` / `fileattr_set` | `FS_IOC_GETFLAGS`-style attrs (chattr/lsattr) |

### 2.4 Inode APIs

| API | Explanation |
|-----|-------------|
| `new_inode(sb)` | allocate a fresh inode on this sb (for creating new files); `i_ino` not set |
| `iget_locked(sb, ino)` | find inode in cache or allocate new one; if returned inode has `I_NEW`, fs must read it from disk then call `unlock_new_inode()` |
| `iget5_locked(sb, hashval, test, set, data)` | same but with custom match (when `ino` alone is not unique) |
| `unlock_new_inode(inode)` | clear `I_NEW`, wake up waiters |
| `iget_failed(inode)` | failure path for a half-initialized `I_NEW` inode |
| `iput(inode)` | drop a reference; on last put -> `evict` (or goes to LRU if still linked) |
| `ihold(inode)` | take an extra ref when you already hold one |
| `igrab(inode)` | take a ref if inode isn't being freed (can return NULL) |
| `inode_init_owner(idmap, inode, dir, mode)` | set uid/gid/mode on a new inode (handles setgid dirs) |
| `inode_init_once(inode)` | one-time init in slab constructor |
| `insert_inode_hash(inode)` / `remove_inode_hash(inode)` | add/remove from the inode hash |
| `mark_inode_dirty(inode)` | inode needs writeback (calls `->dirty_inode`) |
| `mark_inode_dirty_sync(inode)` | dirty, but not necessary for fdatasync |
| `i_size_read(inode)` / `i_size_write(inode, n)` | safe size access (seqcount on 32-bit); caller holds `i_rwsem` for write |
| `inc_nlink` / `drop_nlink` / `clear_nlink` / `set_nlink` | adjust `i_nlink` (marks dirty by caller) |
| `inode_lock(inode)` / `inode_unlock(inode)` | exclusive `i_rwsem` |
| `inode_lock_shared(inode)` / `inode_unlock_shared(inode)` | shared `i_rwsem` |
| `inode_trylock(inode)` | non-blocking exclusive |
| `current_time(inode)` | current time truncated to the fs granularity |
| `generic_permission(idmap, inode, mask)` | standard POSIX mode-bit permission check |
| `inode_permission(idmap, inode, mask)` | full check (mode + LSM + immutable) |
| `generic_fillattr(idmap, req_mask, inode, stat)` | fill `kstat` from inode fields |
| `setattr_prepare()` / `setattr_copy()` | validate / apply `iattr` in `->setattr` |
| `init_special_inode(inode, mode, rdev)` | set up char/block/fifo/socket inode |
| `clear_inode(inode)` | mark `I_CLEAR`, call from `->evict_inode` |
| `truncate_inode_pages_final(mapping)` | drop all page cache pages in `->evict_inode` |
| `generic_delete_inode(inode)` | `drop_inode` that always evicts (no LRU caching) |
| `make_bad_inode(inode)` / `is_bad_inode(inode)` | mark a corrupt inode; all ops return `-EIO` |

---

## 3. struct dentry (dcache)

Maps a **name** to an **inode**, and encodes the tree structure. Purely an in-memory cache (no on-disk form), makes path lookup fast.

### 3.1 Common fields

| Field | Type | Meaning |
|-------|------|---------|
| `d_name` | `struct qstr` | the name (`.name`, `.len`, `.hash`) |
| `d_iname` | `unsigned char[]` | inline storage for short names (avoids a kmalloc) |
| `d_inode` | `struct inode *` | associated inode; **NULL = negative dentry** |
| `d_parent` | `struct dentry *` | parent dentry (root points to itself) |
| `d_sb` | `struct super_block *` | owning sb |
| `d_op` | `const struct dentry_operations *` | dentry method table |
| `d_flags` | `unsigned int` | type + state flags (`DCACHE_*`) |
| `d_lock` (in `d_lockref`) | `spinlock_t` | protects the dentry |
| `d_lockref` | `struct lockref` | spinlock + refcount combined (lockless fast-path inc/dec) |
| `d_seq` | `seqcount_spinlock_t` | seq counter for RCU path walk |
| `d_hash` | `struct hlist_bl_node` | link in the global dentry hash (parent + name -> dentry) |
| `d_child` | `struct hlist_node` | link in the parent's `d_children` list |
| `d_subdirs` / `d_children` | `list_head` / `hlist_head` | children of this dentry |
| `d_u.d_alias` | `hlist_node` | link in `inode->i_dentry` |
| `d_u.d_rcu` | `rcu_head` | for RCU freeing |
| `d_lru` | `struct list_head` | LRU list (unused dentries kept for reuse) |
| `d_fsdata` | `void *` | fs private data |

### 3.2 States

| State | Condition | Meaning |
|-------|-----------|---------|
| In use | refcount > 0, positive | open file or cwd points to it |
| Unused | refcount == 0, positive | cached on LRU, can be reclaimed |
| Negative | `d_inode == NULL` | "this name does NOT exist" (caches failed lookups) |
| Unhashed | not in dcache hash | e.g. after `d_drop()`, won't be found by new lookups |

### 3.3 struct dentry_operations (all optional)

| Op | Purpose |
|----|---------|
| `d_revalidate` | is the cached dentry still valid? (network fs: server may have changed it) |
| `d_hash` | custom name hashing (e.g. case-insensitive fs) |
| `d_compare` | custom name comparison |
| `d_delete` | on last `dput`: return 1 to drop instead of caching |
| `d_init` | on alloc: set up `d_fsdata` |
| `d_release` | on free: release `d_fsdata` |
| `d_iput` | replace default "iput the inode" when dentry dies |
| `d_dname` | generate name on demand (e.g. pipefs `pipe:[1234]`) |
| `d_automount` | automount triggers (NFS referrals) |

### 3.4 Dentry APIs

| API | Explanation |
|-----|-------------|
| `d_inode(dentry)` | get the inode associated with a dentry (may be NULL if negative) |
| `d_inode_rcu(dentry)` | same, for RCU-walk context (uses `READ_ONCE`) |
| `d_really_is_positive(dentry)` / `d_really_is_negative(dentry)` | has an inode / doesn't |
| `d_is_dir(dentry)` / `d_is_reg(dentry)` / `d_is_symlink(dentry)` | type test from `d_flags` (no inode deref) |
| `d_alloc(parent, name)` | allocate a new child dentry (unhashed, no inode) |
| `d_alloc_name(parent, name)` | same with a C string |
| `d_alloc_parallel()` | alloc with in-lookup tracking (used in VFS lookup path) |
| `d_instantiate(dentry, inode)` | attach inode to dentry (does not hash it); used in `->create`/`->mkdir` |
| `d_add(dentry, inode)` | `d_instantiate` + `d_rehash`; inode may be NULL -> negative dentry |
| `d_splice_alias(inode, dentry)` | use at the end of `->lookup`: attach inode, handles NULL (negative) and directory aliases; returns replacement dentry or NULL |
| `d_make_root(inode)` | root dentry from root inode (for `s_root`) |
| `d_obtain_alias(inode)` | get/create a (possibly disconnected) dentry for an inode (NFS export handle decoding) |
| `d_find_alias(inode)` | get a refcounted alias dentry of an inode, or NULL |
| `d_lookup(parent, name)` | find child in dcache (does not call the fs) |
| `d_hash_and_lookup(dir, name)` | hash the name via `d_op->d_hash` then `d_lookup` |
| `dget(dentry)` | take a reference |
| `dput(dentry)` | drop a reference (last put -> LRU or free); NEVER leak these |
| `dget_parent(dentry)` | safely get a ref to the parent |
| `d_drop(dentry)` | unhash (no new lookups find it) |
| `d_delete(dentry)` | called by unlink-ish paths: makes dentry negative if unused elsewhere, else unhashes |
| `d_move(dentry, target)` | move/rename in dcache |
| `d_invalidate(dentry)` | invalidate a dentry and its subtree |
| `d_path(path, buf, len)` | build the absolute path string from a `struct path` (fills from the end of buf) |
| `dentry_path_raw(dentry, buf, len)` | path relative to the fs root (no mount awareness) |
| `shrink_dcache_sb(sb)` | prune all unused dentries of an sb |
| `d_set_d_op(dentry, ops)` | set dentry ops (usually done via `sb->s_d_op`) |
| `simple_lookup()` | libfs `->lookup` for in-memory fs: adds a negative dentry |
| `kern_path(name, flags, &path)` | resolve a path string from kernel code (release with `path_put`) |

---

## 4. struct file

One per `open()` (also per `dup`-shared reference: `dup()`/`fork()` share the same struct file; separate `open()` calls make separate ones). Holds per-open state.

### 4.1 Common fields

| Field | Type | Meaning |
|-------|------|---------|
| `f_path` | `struct path` | `{ .mnt = vfsmount*, .dentry = dentry* }`: where this file is |
| `f_inode` | `struct inode *` | cached `d_inode(f_path.dentry)` |
| `f_op` | `const struct file_operations *` | copied from `inode->i_fop` at open |
| `f_mapping` | `struct address_space *` | page cache (= `inode->i_mapping`) |
| `f_pos` | `loff_t` | current file offset |
| `f_pos_lock` | `struct mutex` | serializes `f_pos` updates when shared |
| `f_flags` | `unsigned int` | `O_RDONLY`, `O_APPEND`, `O_NONBLOCK`, `O_DIRECT`... |
| `f_mode` | `fmode_t` | `FMODE_READ`, `FMODE_WRITE`, `FMODE_LSEEK`, `FMODE_ATOMIC_POS`... |
| `f_count` / `f_ref` | `atomic_long_t` / `file_ref_t` | reference count |
| `f_owner` | `struct fown_struct` | `SIGIO` owner (`fcntl F_SETOWN`) |
| `f_cred` | `const struct cred *` | credentials at open time |
| `f_ra` | `struct file_ra_state` | readahead state |
| `private_data` | `void *` | **driver/fs private** pointer (set in `->open`, free in `->release`) |
| `f_security` | `void *` | LSM blob |

### 4.2 struct file_operations (key hooks)

| Op | Purpose |
|----|---------|
| `owner` | `THIS_MODULE` (pins the module while file is open) |
| `llseek` | `lseek(2)` |
| `read` / `write` | classic read/write on a user buffer |
| `read_iter` / `write_iter` | modern iov_iter-based I/O (preferred for regular fs) |
| `iterate_shared` | `readdir`/`getdents`: fill entries using `dir_emit()` |
| `poll` | `select`/`poll`/`epoll` readiness |
| `unlocked_ioctl` | `ioctl(2)` (no BKL) |
| `compat_ioctl` | 32-bit process on 64-bit kernel |
| `mmap` | `mmap(2)` |
| `open` | called on open (alloc `private_data`) |
| `flush` | called on every `close()` of an fd |
| `release` | called when the last ref to the struct file goes away |
| `fsync` | `fsync(2)` / `fdatasync(2)` |
| `fasync` | `O_ASYNC` / `SIGIO` setup |
| `splice_read` / `splice_write` | splice/sendfile |
| `fallocate` | `fallocate(2)` |
| `get_unmapped_area` | pick address for `mmap` |

### 4.3 File APIs

| API | Explanation |
|-----|-------------|
| `fget(fd)` | fd -> `struct file *` with a ref (NULL if invalid); pair with `fput` |
| `fdget(fd)` / `fdput(f)` | lighter version, skips the atomic ref when the fd table isn't shared |
| `get_file(file)` | take an extra ref |
| `fput(file)` | drop ref; last put runs `->release` (deferred to task work for user tasks) |
| `get_unused_fd_flags(flags)` | reserve an fd number |
| `fd_install(fd, file)` | publish file at that fd (after this, userspace can see it) |
| `put_unused_fd(fd)` | give back a reserved fd on error |
| `alloc_file_pseudo()` | create anonymous file with no real path (pipes, eventfd...) |
| `anon_inode_getfile()` / `anon_inode_getfd()` | create a file backed by the anon inode (epoll, timerfd style) |
| `file_inode(file)` | `file->f_inode` |
| `file_dentry(file)` | the real dentry (overlayfs-aware) |
| `file_path(file, buf, len)` | path string for an open file |
| `filp_open(name, flags, mode)` | open a file from kernel code; close with `filp_close(f, NULL)` |
| `kernel_read(file, buf, count, &pos)` / `kernel_write(...)` | read/write using kernel buffers |
| `vfs_read` / `vfs_write` | VFS entry points (do permission/security checks) |
| `generic_file_llseek` | standard `llseek` for fs with `s_maxbytes` |
| `no_llseek` / `noop_llseek` / `default_llseek` | not seekable / ignore / default |
| `generic_file_read_iter` / `generic_file_write_iter` | page-cache based I/O helpers |
| `generic_file_mmap` / `generic_file_splice_read` | page-cache `mmap` / splice helpers |
| `simple_open` | `->open` that just copies `inode->i_private` to `file->private_data` |
| `nonseekable_open` / `stream_open` | mark file as non-seekable / stream |
| `dir_emit(ctx, name, len, ino, type)` | add one entry in `->iterate_shared`; returns false when buffer full |
| `dir_emit_dots(file, ctx)` | emit `.` and `..` |
| `copy_to_user` / `copy_from_user` | move data across user/kernel boundary in read/write ops |

`struct dir_context` = `{ .actor, .pos }`. `ctx->pos` is the dir cookie, update it as you emit.

---

## 5. Lifetime / refcount cheat sheet

| Object | Get | Put | Freed when |
|--------|-----|-----|------------|
| super_block | `s_active++` (`atomic_inc`), `s_count++` | `deactivate_super()`, `put_super()` | no active refs -> `kill_sb`; no passive refs -> struct freed |
| inode | `iget_locked` / `igrab` / `ihold` | `iput` | count 0 and (nlink 0 or pruned by LRU) -> `evict` |
| dentry | `dget` / `d_lookup` | `dput` | count 0 -> LRU; shrinker later frees |
| file | `fget` / `get_file` / `fdget` | `fput` / `fdput` | count 0 -> `__fput` -> `->release` -> `dput` + `mntput` |

Important ownership: `struct file` holds refs on its dentry (`dput`) and mount (`mntput`). A positive dentry holds a ref on its inode.

---

## 6. Locking cheat sheet

| Lock | Type | Protects |
|------|------|----------|
| `sb->s_umount` | rwsem | mount/umount/remount vs users of the sb |
| `inode->i_rwsem` | rwsem | dir content changes (excl), lookup/readdir (shared), file writes (excl) |
| `inode->i_lock` | spinlock | `i_state`, `i_count`, small fields |
| `dentry->d_lock` | spinlock | dentry fields, d_flags, children list |
| `rename_lock` | seqlock | global rename consistency (RCU path walk retry) |
| `sb->s_vfs_rename_mutex` | mutex | cross-directory rename ordering |
| `sb->s_inode_list_lock` | spinlock | `s_inodes` list |
| `inode_hash_lock` | spinlock | global inode hash |
| `file->f_pos_lock` | mutex | `f_pos` when file is shared |
| `mapping->i_mmap_rwsem` | rwsem | VMAs mapping this file |

Lock order (rough): `s_umount` -> parent `i_rwsem` -> child `i_rwsem` -> `i_lock` -> `d_lock`. Parent before child, and for two non-ancestor dirs the order is by address, via `lock_rename()`.

---

## 7. Path lookup in one screen

Example `open("/home/a/f.txt")`:

```
do_sys_open -> do_filp_open -> path_openat -> link_path_walk
  for each component:
     1. hash the name
     2. __d_lookup_rcu()        // RCU-walk: lockless dcache hit, uses d_seq to validate
     3. hit  -> check permission (inode_permission), follow mount/symlink, continue
        miss -> fall back to ref-walk, d_alloc_parallel(),
                parent->i_op->lookup(dir, dentry, flags)   // fs reads disk
                -> d_splice_alias(inode, dentry)           // positive or negative
  last component: do_open -> vfs_open -> file->f_op->open
  allocate struct file, fd_install()
```

- **RCU-walk**: no refcounts, no locks, fast; bails out (retry) on any conflict.
- **Ref-walk**: takes `dget`/`d_lock`, slower, can sleep.
- **Negative dentry**: second lookup of a missing file never reaches the fs.

---

## 8. Related objects worth knowing

| Object | Notes |
|--------|-------|
| `struct address_space` | page cache of one inode (`a_ops`, `i_pages` xarray, `host` inode, `nrpages`) |
| `struct address_space_operations` | `read_folio`, `writepages`, `dirty_folio`, `write_begin`/`write_end`, `bmap`, `direct_IO` |
| `struct path` | `{ struct vfsmount *mnt; struct dentry *dentry; }`, always handled as a pair; `path_get()` / `path_put()` |
| `struct vfsmount` | public part of a mount: `mnt_root`, `mnt_sb`, `mnt_flags` |
| `struct mount` | private part (fs/mount.h): `mnt_parent`, `mnt_mountpoint`, `mnt_devname`, `mnt_ns` |
| `struct file_system_type` | `name`, `init_fs_context`, `kill_sb`, `fs_flags` (`FS_REQUIRES_DEV`...), `owner` |
| `struct fs_context` | new mount API: carries options before the sb exists (`fs_context_operations`: `parse_param`, `get_tree`, `reconfigure`, `free`) |
| `struct qstr` | counted string: `{ hash, len, name }` |
| `struct kstat` | what `stat(2)` returns, filled by `->getattr` |
| `struct iattr` | what `setattr` receives (`ia_valid` bitmask: `ATTR_MODE`, `ATTR_SIZE`...) |
| `struct nameidata` | internal state of a path walk (fs/namei.c) |
| `struct mnt_idmap` | idmapped mounts: translates uid/gid per mount; passed to many inode ops |

---

## 9. Minimal skeleton of a toy filesystem

```c
static struct file_system_type myfs_type = {
    .owner            = THIS_MODULE,
    .name             = "myfs",
    .init_fs_context  = myfs_init_fs_context,
    .kill_sb          = kill_litter_super,   // or kill_block_super
};

static int myfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct inode *root;

    sb->s_magic    = MYFS_MAGIC;
    sb->s_op       = &myfs_sops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;

    root = new_inode(sb);
    if (!root)
        return -ENOMEM;
    root->i_ino  = 1;
    root->i_mode = S_IFDIR | 0755;
    root->i_op   = &simple_dir_inode_operations;   // from libfs
    root->i_fop  = &simple_dir_operations;         // from libfs

    sb->s_root = d_make_root(root);                // consumes root on failure
    if (!sb->s_root)
        return -ENOMEM;
    return 0;
}

static int __init myfs_init(void)  { return register_filesystem(&myfs_type); }
static void __exit myfs_exit(void) { unregister_filesystem(&myfs_type); }
```

---

## 10. Debug / inspect

| Where / tool | What it shows |
|--------------|---------------|
| /proc/filesystems | registered fs types (`nodev` = no backing device) |
| /proc/self/mountinfo | mounts, mount IDs, parent IDs, fs type, options |
| /proc/sys/fs/dentry-state | dentry count, unused, negative, etc. |
| /proc/sys/fs/inode-nr | allocated / free inodes |
| /proc/sys/fs/file-nr | open files: allocated / unused / max |
| /proc/slabinfo (or `slabtop`) | `dentry`, `inode_cache`, `ext4_inode_cache` slab usage |
| /proc/sys/vm/drop_caches | `echo 2 > /proc/sys/vm/drop_caches` drops dentries+inodes; `1` page cache; `3` both |
| /sys/kernel/debug/tracing/ | tracepoints / ftrace (`events/filelock/`, `events/writeback/`, kprobes on `d_lookup`, `iget_locked`) |
| `stat ./file` | shows inode number, links, blocks |
| `ls -i ./` | inode numbers in current dir |
| `debugfs`, `dumpe2fs` | inspect ext4 on-disk sb/inodes |
| crash / drgn / gdb | walk `task->files->fdt->fd[n]->f_path.dentry->d_inode` live |

---

## 11. Quick mental recap

- **super_block** = the mounted filesystem (config + root + inode list).
- **inode** = the file itself (metadata + pointer to data). No name.
- **dentry** = a name in the tree (cache), points to an inode; can be negative.
- **file** = one open handle (offset + flags + ops), points to dentry.
- `i_nlink` counts names on disk, `i_count` counts memory users, `d_lockref` counts dentry users, `f_count` counts fd/file users.
- `->lookup` fills the dcache, `->open` fills `private_data`, `->evict_inode` frees disk resources.
- When in doubt: grep for how `ramfs` (./fs/ramfs/) or `minix` (./fs/minix/) does it; they are the smallest readable examples.