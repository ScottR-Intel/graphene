/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * Implementation of system calls "unlink", "unlinkat", "mkdir", "mkdirat", "rmdir", "umask",
 * "chmod", "fchmod", "fchmodat", "rename", "renameat" and "sendfile".
 */

#include <asm/mman.h>
#include <errno.h>
#include <linux/fcntl.h>

#include "pal.h"
#include "pal_error.h"
#include "shim_fs.h"
#include "shim_handle.h"
#include "shim_internal.h"
#include "shim_lock.h"
#include "shim_process.h"
#include "shim_table.h"
#include "shim_utils.h"
#include "stat.h"

/* The kernel would look up the parent directory, and remove the child from the inode. But we are
 * working with the PAL, so we open the file, truncate and close it. */
long shim_do_unlink(const char* file) {
    return shim_do_unlinkat(AT_FDCWD, file, 0);
}

long shim_do_unlinkat(int dfd, const char* pathname, int flag) {
    if (!is_user_string_readable(pathname))
        return -EFAULT;

    if (flag & ~AT_REMOVEDIR)
        return -EINVAL;

    struct shim_dentry* dir = NULL;
    struct shim_dentry* dent = NULL;
    int ret = 0;

    if (*pathname != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    if ((ret = path_lookupat(dir, pathname, LOOKUP_NO_FOLLOW, &dent)) < 0)
        goto out;

    if (!dent->parent) {
        ret = -EACCES;
        goto out;
    }

    if (flag & AT_REMOVEDIR) {
        if (!(dent->state & DENTRY_ISDIRECTORY)) {
            ret = -ENOTDIR;
            goto out;
        }
    } else {
        if (dent->state & DENTRY_ISDIRECTORY) {
            ret = -EISDIR;
            goto out;
        }
    }

    if (dent->fs && dent->fs->d_ops && dent->fs->d_ops->unlink) {
        if ((ret = dent->fs->d_ops->unlink(dent->parent, dent)) < 0) {
            goto out;
        }
    } else {
        dent->state |= DENTRY_PERSIST;
    }

    if (flag & AT_REMOVEDIR)
        dent->state &= ~DENTRY_ISDIRECTORY;

    dent->state |= DENTRY_NEGATIVE;
out:
    if (dir)
        put_dentry(dir);
    if (dent) {
        put_dentry(dent);
    }
    return ret;
}

long shim_do_mkdir(const char* pathname, int mode) {
    return shim_do_mkdirat(AT_FDCWD, pathname, mode);
}

long shim_do_mkdirat(int dfd, const char* pathname, int mode) {
    if (!is_user_string_readable(pathname))
        return -EFAULT;

    struct shim_dentry* dir = NULL;
    int ret = 0;

    if (*pathname != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    ret = open_namei(NULL, dir, pathname, O_CREAT | O_EXCL | O_DIRECTORY, mode, NULL);

    if (dir)
        put_dentry(dir);
    return ret;
}

long shim_do_rmdir(const char* pathname) {
    int ret = 0;
    struct shim_dentry* dent = NULL;

    if (!is_user_string_readable(pathname))
        return -EFAULT;

    if ((ret = path_lookupat(/*start=*/NULL, pathname, LOOKUP_NO_FOLLOW | LOOKUP_DIRECTORY,
                             &dent)) < 0)
        return ret;

    if (!dent->parent) {
        ret = -EACCES;
        goto out;
    }

    if (!(dent->state & DENTRY_ISDIRECTORY)) {
        ret = -ENOTDIR;
        goto out;
    }

    if (dent->fs && dent->fs->d_ops && dent->fs->d_ops->unlink) {
        if ((ret = dent->fs->d_ops->unlink(dent->parent, dent)) < 0)
            goto out;
    } else {
        dent->state |= DENTRY_PERSIST;
    }

    dent->state &= ~DENTRY_ISDIRECTORY;
    dent->state |= DENTRY_NEGATIVE;
out:
    put_dentry(dent);
    return ret;
}

long shim_do_umask(mode_t mask) {
    lock(&g_process.fs_lock);
    mode_t old = g_process.umask;
    g_process.umask = mask & 0777;
    unlock(&g_process.fs_lock);
    return old;
}

long shim_do_chmod(const char* path, mode_t mode) {
    return shim_do_fchmodat(AT_FDCWD, path, mode);
}

long shim_do_fchmodat(int dfd, const char* filename, mode_t mode) {
    if (!is_user_string_readable(filename))
        return -EFAULT;

    /* This isn't documented, but that's what Linux does. */
    mode &= 07777;

    struct shim_dentry* dir = NULL;
    struct shim_dentry* dent = NULL;
    int ret = 0;

    if (*filename != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    if ((ret = path_lookupat(dir, filename, LOOKUP_FOLLOW, &dent)) < 0)
        goto out;

    if (dent->fs && dent->fs->d_ops && dent->fs->d_ops->chmod) {
        if ((ret = dent->fs->d_ops->chmod(dent, mode)) < 0)
            goto out_dent;
    } else {
        dent->state |= DENTRY_PERSIST;
    }

    dent->perm = mode;
out_dent:
    put_dentry(dent);
out:
    if (dir)
        put_dentry(dir);
    return ret;
}

long shim_do_fchmod(int fd, mode_t mode) {
    struct shim_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    /* This isn't documented, but that's what Linux does. */
    mode &= 07777;

    struct shim_dentry* dent = hdl->dentry;
    int ret = 0;

    if (!dent) {
        ret = -EINVAL;
        goto out;
    }

    if (dent->fs && dent->fs->d_ops && dent->fs->d_ops->chmod) {
        if ((ret = dent->fs->d_ops->chmod(dent, mode)) < 0)
            goto out;
    } else {
        dent->state |= DENTRY_PERSIST;
    }

    dent->perm = mode;
out:
    put_handle(hdl);
    return ret;
}

long shim_do_chown(const char* path, uid_t uid, gid_t gid) {
    return shim_do_fchownat(AT_FDCWD, path, uid, gid, 0);
}

long shim_do_fchownat(int dfd, const char* filename, uid_t uid, gid_t gid, int flags) {
    __UNUSED(flags);
    __UNUSED(uid);
    __UNUSED(gid);

    if (!is_user_string_readable(filename))
        return -EFAULT;

    struct shim_dentry* dir = NULL;
    struct shim_dentry* dent = NULL;
    int ret = 0;

    if (*filename != '/' && (ret = get_dirfd_dentry(dfd, &dir)) < 0)
        return ret;

    if ((ret = path_lookupat(dir, filename, LOOKUP_FOLLOW, &dent)) < 0)
        goto out;

    /* XXX: do nothing now */
    put_dentry(dent);
out:
    if (dir)
        put_dentry(dir);
    return ret;
}

long shim_do_fchown(int fd, uid_t uid, gid_t gid) {
    __UNUSED(uid);
    __UNUSED(gid);

    struct shim_handle* hdl = get_fd_handle(fd, NULL, NULL);
    if (!hdl)
        return -EBADF;

    /* XXX: do nothing now */
    return 0;
}

#define MAP_SIZE (ALLOC_ALIGNMENT * 256)  /* mmap/memcpy in 1MB chunks for sendfile() */
#define BUF_SIZE 2048                     /* read/write in 2KB chunks for sendfile() */

/* TODO: The below implementation needs to be refactored: (1) remove offseto, it is always zero;
 *       (2) simplify handling of non-blocking handles, (3) instead of relying on PAL to mmap
 *       into a new address and free on every iteration of the copy loop, pre-allocate VMA and
 *       use it, (4) do not use stack-allocated buffer for read/write logic, (5) use a switch
 *       statement to distinguish between "map input", "map output", "map both", "map none" */
static ssize_t handle_copy(struct shim_handle* hdli, off_t* offseti, struct shim_handle* hdlo,
                           off_t* offseto, ssize_t count) {
    struct shim_fs* fsi = hdli->fs;
    struct shim_fs* fso = hdlo->fs;

    if (!count)
        return 0;

    if (!fsi || !fsi->fs_ops || !fso || !fso->fs_ops)
        return -EACCES;

    bool do_mapi  = fsi->fs_ops->mmap != NULL;
    bool do_mapo  = fso->fs_ops->mmap != NULL;
    bool do_marki = false;
    bool do_marko = false;
    int offi = 0, offo = 0;

    if (offseti) {
        if (!fsi->fs_ops->seek)
            return -EACCES;
        offi = *offseti;
        fsi->fs_ops->seek(hdli, offi, SEEK_SET);
    } else {
        if (!fsi->fs_ops->seek || (offi = fsi->fs_ops->seek(hdli, 0, SEEK_CUR)) < 0)
            do_mapi = false;
    }

    if (offseto) {
        if (!fso->fs_ops->seek)
            return -EACCES;
        offo = *offseto;
        fso->fs_ops->seek(hdlo, offo, SEEK_SET);
    } else {
        if (!fso->fs_ops->seek || (offo = fso->fs_ops->seek(hdlo, 0, SEEK_CUR)) < 0)
            do_mapo = false;
    }

    if (do_mapi) {
        int size;
        if (fsi->fs_ops->poll && (size = fsi->fs_ops->poll(hdli, FS_POLL_SZ)) >= 0) {
            if (count == -1 || count > size - offi)
                count = size - offi;

            if (!count)
                return 0;
        } else {
            do_mapi = false;
        }
    }

    if (do_mapo && count > 0)
        do {
            int size;
            if (!fso->fs_ops->poll || (size = fso->fs_ops->poll(hdlo, FS_POLL_SZ)) < 0) {
                do_mapo = false;
                break;
            }

            if (offo + count < size)
                break;

            if (!fso->fs_ops->truncate || fso->fs_ops->truncate(hdlo, offo + count) < 0) {
                do_mapo = false;
                break;
            }
        } while (0);

    void* bufi = NULL;
    void* bufo = NULL;
    int bytes    = 0;
    int bufsize  = MAP_SIZE;
    int copysize = 0;

    if (!do_mapi && (hdli->flags & O_NONBLOCK) && fsi->fs_ops->setflags) {
        int ret = fsi->fs_ops->setflags(hdli, 0);
        if (!ret) {
            log_debug("mark handle %s as blocking\n", qstrgetstr(&hdli->uri));
            do_marki = true;
        }
    }

    if (!do_mapo && (hdlo->flags & O_NONBLOCK) && fso->fs_ops->setflags) {
        int ret = fso->fs_ops->setflags(hdlo, 0);
        if (!ret) {
            log_debug("mark handle %s as blocking\n", qstrgetstr(&hdlo->uri));
            do_marko = true;
        }
    }

    assert(count);
    do {
        int boffi = 0, boffo = 0;
        int expectsize = bufsize;

        if (count > 0 && bufsize > count - bytes)
            expectsize = bufsize = count - bytes;

        if (do_mapi && !bufi) {
            boffi = offi - ALLOC_ALIGN_DOWN(offi);

            if (fsi->fs_ops->mmap(hdli, &bufi, ALLOC_ALIGN_UP(bufsize + boffi), PROT_READ, MAP_FILE,
                                  offi - boffi) < 0) {
                do_mapi = false;
                boffi = 0;
                if ((hdli->flags & O_NONBLOCK) && fsi->fs_ops->setflags) {
                    int ret = fsi->fs_ops->setflags(hdli, 0);
                    if (!ret) {
                        log_debug("mark handle %s as blocking\n", qstrgetstr(&hdli->uri));
                        do_marki = true;
                    }
                }
                if (fsi->fs_ops->seek)
                    offi = fsi->fs_ops->seek(hdli, offi, SEEK_SET);
            }
        }

        if (do_mapo && !bufo) {
            boffo = offo - ALLOC_ALIGN_DOWN(offo);

            if (fso->fs_ops->mmap(hdlo, &bufo, ALLOC_ALIGN_UP(bufsize + boffo), PROT_WRITE,
                                  MAP_FILE, offo - boffo) < 0) {
                do_mapo = false;
                boffo = 0;
                if ((hdlo->flags & O_NONBLOCK) && fso->fs_ops->setflags) {
                    int ret = fso->fs_ops->setflags(hdlo, 0);
                    if (!ret) {
                        log_debug("mark handle %s as blocking\n", qstrgetstr(&hdlo->uri));
                        do_marko = true;
                    }
                }
                if (fso->fs_ops->seek)
                    offo = fso->fs_ops->seek(hdlo, offo, SEEK_SET);
            }
        }

        if (do_mapi && do_mapo) {
            copysize = count - bytes > bufsize ? bufsize : count - bytes;
            memcpy(bufo + boffo, bufi + boffi, copysize);
            /* XXX: ??? Where is vma bookkeeping? Hans, get ze flammenwerfer... */
            DkVirtualMemoryFree(bufi, ALLOC_ALIGN_UP(bufsize + boffi));
            bufi = NULL;
            if (fso->fs_ops->flush) {
                /* SGX Protected Files propagate mmapped changes only on flush/close, so perform
                 * explicit flush before freeing PF's mmapped region `bufo` */
                fso->fs_ops->flush(hdlo);
            }
            /* XXX: ??? Where is vma bookkeeping? Hans, get ze flammenwerfer... */
            DkVirtualMemoryFree(bufo, ALLOC_ALIGN_UP(bufsize + boffo));
            bufo = NULL;
        } else if (do_mapo) {
            copysize = fsi->fs_ops->read(hdli, bufo + boffo, bufsize);
            if (fso->fs_ops->flush) {
                /* SGX Protected Files propagate mmapped changes only on flush/close, so perform
                 * explicit flush before freeing PF's mmapped region `bufo` */
                fso->fs_ops->flush(hdlo);
            }
            /* XXX: ??? Where is vma bookkeeping? Hans, get ze flammenwerfer... */
            DkVirtualMemoryFree(bufo, ALLOC_ALIGN_UP(bufsize + boffo));
            bufo = NULL;
            if (copysize < 0)
                break;
        } else if (do_mapi) {
            copysize = fso->fs_ops->write(hdlo, bufi + boffi, bufsize);
            /* XXX: ??? Where is vma bookkeeping? Hans, get ze flammenwerfer... */
            DkVirtualMemoryFree(bufi, ALLOC_ALIGN_UP(bufsize + boffi));
            bufi = NULL;
            if (copysize < 0)
                break;
        } else {
            if (!bufi)
                bufi = __alloca((bufsize = (bufsize > BUF_SIZE) ? BUF_SIZE : bufsize));

            copysize = fsi->fs_ops->read(hdli, bufi, bufsize);

            if (copysize <= 0)
                break;

            expectsize = copysize;
            copysize = fso->fs_ops->write(hdlo, bufi, expectsize);
            if (copysize < 0)
                break;
        }

        log_debug("copy %d bytes\n", copysize);
        bytes += copysize;
        offi += copysize;
        offo += copysize;
        if (copysize < expectsize)
            break;
    } while (bytes < count);

    if (copysize < 0 || (count > 0 && bytes < count)) {
        int ret = copysize < 0 ? copysize : -EAGAIN;

        if (bytes) {
            if (fsi->fs_ops->seek)
                fsi->fs_ops->seek(hdli, offi - bytes, SEEK_SET);
            if (fso->fs_ops->seek)
                fso->fs_ops->seek(hdlo, offo - bytes, SEEK_SET);
        }

        return ret;
    }

    if (do_marki && (hdli->flags & O_NONBLOCK)) {
        log_debug("mark handle %s as nonblocking\n", qstrgetstr(&hdli->uri));
        fsi->fs_ops->setflags(hdli, O_NONBLOCK);
    }

    if (do_marko && (hdlo->flags & O_NONBLOCK)) {
        log_debug("mark handle %s as nonblocking\n", qstrgetstr(&hdlo->uri));
        fso->fs_ops->setflags(hdlo, O_NONBLOCK);
    }

    if (do_mapi) {
        if (fsi->fs_ops->seek)
            fsi->fs_ops->seek(hdli, offi, SEEK_SET);
    }

    if (offseti)
        *offseti = offi;

    if (do_mapo) {
        if (fso->fs_ops->seek)
            fso->fs_ops->seek(hdlo, offo, SEEK_SET);
    }

    if (offseto)
        *offseto = offo;

    return bytes;
}

static int do_rename(struct shim_dentry* old_dent, struct shim_dentry* new_dent) {
    if ((old_dent->type != S_IFREG) ||
            (!(new_dent->state & DENTRY_NEGATIVE) && (new_dent->type != S_IFREG))) {
        /* Current implementation of fs does not allow for renaming anything but regular files */
        return -ENOSYS;
    }

    if (old_dent->fs != new_dent->fs) {
        /* Disallow cross mount renames */
        return -EXDEV;
    }

    if (!old_dent->fs || !old_dent->fs->d_ops || !old_dent->fs->d_ops->rename) {
        return -EPERM;
    }

    if (old_dent->state & DENTRY_ISDIRECTORY) {
        if (!(new_dent->state & DENTRY_NEGATIVE)) {
            if (!(new_dent->state & DENTRY_ISDIRECTORY)) {
                return -ENOTDIR;
            }
            if (new_dent->nchildren > 0) {
                return -ENOTEMPTY;
            }
        } else {
            /* destination is a negative dentry and needs to be marked as a directory, since source
             * is a directory */
            new_dent->state |= DENTRY_ISDIRECTORY;
        }
    } else if (new_dent->state & DENTRY_ISDIRECTORY) {
        return -EISDIR;
    }

    if (dentry_is_ancestor(old_dent, new_dent) || dentry_is_ancestor(new_dent, old_dent)) {
        return -EINVAL;
    }

    /* TODO: Add appropriate checks for hardlinks once they get implemented. */

    int ret = old_dent->fs->d_ops->rename(old_dent, new_dent);
    if (!ret) {
        old_dent->state |= DENTRY_NEGATIVE;
        new_dent->state &= ~DENTRY_NEGATIVE;
    }

    return ret;
}

long shim_do_rename(const char* oldpath, const char* newpath) {
    return shim_do_renameat(AT_FDCWD, oldpath, AT_FDCWD, newpath);
}

long shim_do_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath) {
    struct shim_dentry* old_dir_dent = NULL;
    struct shim_dentry* old_dent     = NULL;
    struct shim_dentry* new_dir_dent = NULL;
    struct shim_dentry* new_dent     = NULL;
    int ret = 0;

    if (!is_user_string_readable(oldpath) || !is_user_string_readable(newpath)) {
        return -EFAULT;
    }

    if (*oldpath != '/' && (ret = get_dirfd_dentry(olddirfd, &old_dir_dent)) < 0) {
        goto out;
    }

    if ((ret = path_lookupat(old_dir_dent, oldpath, LOOKUP_NO_FOLLOW, &old_dent)) < 0) {
        goto out;
    }

    if (old_dent->state & DENTRY_NEGATIVE) {
        ret = -ENOENT;
        goto out;
    }

    if (*newpath != '/' && (ret = get_dirfd_dentry(newdirfd, &new_dir_dent)) < 0) {
        goto out;
    }

    ret = path_lookupat(new_dir_dent, newpath, LOOKUP_NO_FOLLOW | LOOKUP_CREATE, &new_dent);
    if (ret < 0)
        goto out;

    // Both dentries should have a ref count of at least 2 at this point
    assert(REF_GET(old_dent->ref_count) >= 2);
    assert(REF_GET(new_dent->ref_count) >= 2);

    ret = do_rename(old_dent, new_dent);

out:
    if (old_dir_dent)
        put_dentry(old_dir_dent);
    if (old_dent)
        put_dentry(old_dent);
    if (new_dir_dent)
        put_dentry(new_dir_dent);
    if (new_dent)
        put_dentry(new_dent);
    return ret;
}

long shim_do_sendfile(int ofd, int ifd, off_t* offset, size_t count) {
    struct shim_handle* hdli = get_fd_handle(ifd, NULL, NULL);
    if (!hdli)
        return -EBADF;

    struct shim_handle* hdlo = get_fd_handle(ofd, NULL, NULL);
    if (!hdlo) {
        put_handle(hdli);
        return -EBADF;
    }

    int ret = -EINVAL;
    if (hdlo->flags & O_APPEND) {
        /* Linux errors out if output fd has the O_APPEND flag set; comply with this behavior */
        goto out;
    }

    off_t old_offset = 0;
    ret = -EACCES;

    if (offset) {
        if (!hdli->fs || !hdli->fs->fs_ops || !hdli->fs->fs_ops->seek)
            goto out;

        old_offset = hdli->fs->fs_ops->seek(hdli, 0, SEEK_CUR);
        if (old_offset < 0) {
            ret = old_offset;
            goto out;
        }
    }

    ret = handle_copy(hdli, offset, hdlo, NULL, count);

    if (ret >= 0 && offset)
        hdli->fs->fs_ops->seek(hdli, old_offset, SEEK_SET);

out:
    put_handle(hdli);
    put_handle(hdlo);
    return ret;
}

long shim_do_chroot(const char* filename) {
    int ret = 0;
    struct shim_dentry* dent = NULL;
    if ((ret = path_lookupat(/*start=*/NULL, filename, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &dent)) < 0)
        goto out;

    if (!dent) {
        ret = -ENOENT;
        goto out;
    }

    lock(&g_process.fs_lock);
    put_dentry(g_process.root);
    g_process.root = dent;
    unlock(&g_process.fs_lock);
out:
    return ret;
}
