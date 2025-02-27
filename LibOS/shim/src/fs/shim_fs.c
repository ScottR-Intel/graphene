/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University */

/*
 * This file contains code for creating filesystems in library OS.
 */

#include <linux/fcntl.h>

#include "api.h"
#include "list.h"
#include "pal.h"
#include "pal_error.h"
#include "shim_checkpoint.h"
#include "shim_fs.h"
#include "shim_internal.h"
#include "shim_lock.h"
#include "shim_process.h"
#include "shim_utils.h"
#include "toml.h"

struct shim_fs* builtin_fs[] = {
    &chroot_builtin_fs,
    &proc_builtin_fs,
    &dev_builtin_fs,
    &sys_builtin_fs,
    &tmp_builtin_fs,
    &pipe_builtin_fs,
    &fifo_builtin_fs,
    &socket_builtin_fs,
    &epoll_builtin_fs,
    &eventfd_builtin_fs,
};

static struct shim_lock mount_mgr_lock;

#define SYSTEM_LOCK()   lock(&mount_mgr_lock)
#define SYSTEM_UNLOCK() unlock(&mount_mgr_lock)
#define SYSTEM_LOCKED() locked(&mount_mgr_lock)

#define MOUNT_MGR_ALLOC 64

#define OBJ_TYPE struct shim_mount
#include "memmgr.h"

static MEM_MGR mount_mgr = NULL;
DEFINE_LISTP(shim_mount);
/* Links to mount->list */
static LISTP_TYPE(shim_mount) mount_list;
static struct shim_lock mount_list_lock;

int init_fs(void) {
    mount_mgr = create_mem_mgr(init_align_up(MOUNT_MGR_ALLOC));
    if (!mount_mgr)
        return -ENOMEM;

    if (!create_lock(&mount_mgr_lock) || !create_lock(&mount_list_lock)) {
        destroy_mem_mgr(mount_mgr);
        return -ENOMEM;
    }
    return 0;
}

static struct shim_mount* alloc_mount(void) {
    return get_mem_obj_from_mgr_enlarge(mount_mgr, size_align_up(MOUNT_MGR_ALLOC));
}

static bool mount_migrated = false;

static int __mount_root(struct shim_dentry** root) {
    int ret = 0;
    char* fs_root_type = NULL;
    char* fs_root_uri  = NULL;

    assert(g_manifest_root);

    ret = toml_string_in(g_manifest_root, "fs.root.type", &fs_root_type);
    if (ret < 0) {
        log_error("Cannot parse 'fs.root.type' (the value must be put in double quotes!)\n");
        ret = -EINVAL;
        goto out;
    }

    ret = toml_string_in(g_manifest_root, "fs.root.uri", &fs_root_uri);
    if (ret < 0) {
        log_error("Cannot parse 'fs.root.uri' (the value must be put in double quotes!)\n");
        ret = -EINVAL;
        goto out;
    }

    if (fs_root_type && fs_root_uri) {
        log_debug("Mounting root as %s filesystem: from %s to /\n", fs_root_type, fs_root_uri);
        if ((ret = mount_fs(fs_root_type, fs_root_uri, "/", NULL, root, 0)) < 0) {
            log_error("Mounting root filesystem failed (%d)\n", ret);
            goto out;
        }
    } else {
        log_debug("Mounting root as chroot filesystem: from file:. to /\n");
        if ((ret = mount_fs("chroot", URI_PREFIX_FILE, "/", NULL, root, 0)) < 0) {
            log_error("Mounting root filesystem failed (%d)\n", ret);
            goto out;
        }
    }

    ret = 0;
out:
    free(fs_root_type);
    free(fs_root_uri);
    return ret;
}

static int __mount_sys(struct shim_dentry* root) {
    int ret;

    log_debug("Mounting special proc filesystem: /proc\n");
    if ((ret = mount_fs("proc", NULL, "/proc", root, NULL, 0)) < 0) {
        log_error("Mounting /proc filesystem failed (%d)\n", ret);
        return ret;
    }

    log_debug("Mounting special dev filesystem: /dev\n");
    struct shim_dentry* dev_dent = NULL;
    if ((ret = mount_fs("dev", NULL, "/dev", root, &dev_dent, 0)) < 0) {
        log_error("Mounting dev filesystem failed (%d)\n", ret);
        return ret;
    }

    log_debug("Mounting terminal device /dev/tty under /dev\n");
    if ((ret = mount_fs("chroot", URI_PREFIX_DEV "tty", "/dev/tty", dev_dent, NULL, 0)) < 0) {
        log_error("Mounting terminal device /dev/tty failed (%d)\n", ret);
        return ret;
    }

    log_debug("Mounting special sys filesystem: /sys\n");

    if ((ret = mount_fs("sys", NULL, "/sys", root, NULL, 0)) < 0) {
        log_error("Mounting sys filesystem failed (%d)\n", ret);
        return ret;
    }

    return 0;
}

static int __mount_one_other(toml_table_t* mount) {
    assert(mount);

    int ret;
    const char* key = toml_table_key(mount);

    toml_raw_t mount_type_raw = toml_raw_in(mount, "type");
    if (!mount_type_raw) {
        log_error("Cannot find 'fs.mount.%s.type'\n", key);
        return -EINVAL;
    }

    toml_raw_t mount_path_raw = toml_raw_in(mount, "path");
    if (!mount_path_raw) {
        log_error("Cannot find 'fs.mount.%s.path'\n", key);
        return -EINVAL;
    }

    toml_raw_t mount_uri_raw = toml_raw_in(mount, "uri");
    if (!mount_uri_raw) {
        log_error("Cannot find 'fs.mount.%s.uri'\n", key);
        return -EINVAL;
    }

    char* mount_type = NULL;
    char* mount_path = NULL;
    char* mount_uri  = NULL;

    ret = toml_rtos(mount_type_raw, &mount_type);
    if (ret < 0) {
        log_error("Cannot parse 'fs.mount.%s.type' (the value must be put in double quotes!)\n",
                  key);
        ret = -EINVAL;
        goto out;
    }

    ret = toml_rtos(mount_path_raw, &mount_path);
    if (ret < 0) {
        log_error("Cannot parse 'fs.mount.%s.path' (the value must be put in double quotes!)\n",
                  key);
        ret = -EINVAL;
        goto out;
    }

    ret = toml_rtos(mount_uri_raw, &mount_uri);
    if (ret < 0) {
        log_error("Cannot parse 'fs.mount.%s.uri' (the value must be put in double quotes!)\n",
                  key);
        ret = -EINVAL;
        goto out;
    }

    log_debug("Mounting as %s filesystem: from %s to %s\n", mount_type, mount_uri, mount_path);

    if (!strcmp(mount_path, "/")) {
        log_error(
            "Root mount / already exists, verify that there are no duplicate mounts in manifest\n"
            "(note that root / is automatically mounted in Graphene and can be changed via "
            "'fs.root' manifest entry).\n");
        ret = -EEXIST;
        goto out;
    }

    if (!strcmp(mount_path, ".") || !strcmp(mount_path, "..")) {
        log_error("Mount points '.' and '..' are not allowed, remove them from manifest.\n");
        ret = -EINVAL;
        goto out;
    }

    if ((ret = mount_fs(mount_type, mount_uri, mount_path, NULL, NULL, 1)) < 0) {
        log_error("Mounting %s on %s (type=%s) failed (%d)\n", mount_uri, mount_path, mount_type,
                  -ret);
        goto out;
    }

    ret = 0;
out:
    free(mount_type);
    free(mount_path);
    free(mount_uri);
    return ret;
}

static int __mount_others(void) {
    int ret = 0;

    assert(g_manifest_root);
    toml_table_t* manifest_fs = toml_table_in(g_manifest_root, "fs");
    if (!manifest_fs)
        return 0;

    toml_table_t* manifest_fs_mounts = toml_table_in(manifest_fs, "mount");
    if (!manifest_fs_mounts)
        return 0;

    ssize_t mounts_cnt = toml_table_ntab(manifest_fs_mounts);
    if (mounts_cnt <= 0)
        return 0;

    /* *** Warning: A _very_ ugly hack below (hopefully only temporary) ***
     *
     * Currently we don't use proper TOML syntax for declaring mountpoints, instead, we use a syntax
     * which resembles the pre-TOML one used in Graphene. As a result, the entries are not ordered,
     * but Graphene actually relies on the specific mounting order (e.g. you can't mount /lib/asdf
     * first and then /lib, but the other way around works). The problem is, that TOML structure is
     * just a dictionary, so the order of keys is not preserved.
     *
     * The correct solution is to change the manifest syntax for mounts, but this will be a huge,
     * breaking change. For now, just to fix the issue, we use an ugly heuristic - we apply mounts
     * sorted by the path length, which in most cases should result in a proper mount order.
     *
     * We do this in O(n^2) because we don't have a sort function, but that shouldn't be an issue -
     * usually there are around 5 mountpoints with ~30 chars in paths, so it should still be quite
     * fast.
     *
     * Corresponding issue: https://github.com/oscarlab/graphene/issues/2214.
     */
    const char** keys = malloc(mounts_cnt * sizeof(*keys));
    size_t* lengths = malloc(mounts_cnt * sizeof(*lengths));
    size_t longest = 0;
    for (ssize_t i = 0; i < mounts_cnt; i++) {
        keys[i] = toml_key_in(manifest_fs_mounts, i);
        assert(keys[i]);

        toml_table_t* mount = toml_table_in(manifest_fs_mounts, keys[i]);
        assert(mount);
        char* mount_path;
        ret = toml_string_in(mount, "path", &mount_path);
        if (ret < 0 || !mount_path) {
            if (!ret)
                ret = -ENOENT;
            goto out;
        }
        lengths[i] = strlen(mount_path);
        longest = MAX(longest, lengths[i]);
        free(mount_path);
    }

    for (size_t i = 0; i <= longest; i++) {
        for (ssize_t j = 0; j < mounts_cnt; j++) {
            if (lengths[j] != i)
                continue;
            toml_table_t* mount = toml_table_in(manifest_fs_mounts, keys[j]);
            ret = __mount_one_other(mount);
            if (ret < 0)
                goto out;
        }
    }
out:
    free(keys);
    free(lengths);
    return ret;
}

int init_mount_root(void) {
    if (mount_migrated)
        return 0;

    int ret;
    struct shim_dentry* root = NULL;

    if ((ret = __mount_root(&root)) < 0)
        return ret;

    if ((ret = __mount_sys(root)) < 0)
        return ret;

    return 0;
}

int init_mount(void) {
    if (mount_migrated)
        return 0;

    int ret;

    if ((ret = __mount_others()) < 0)
        return ret;

    assert(g_manifest_root);

    char* fs_start_dir = NULL;
    ret = toml_string_in(g_manifest_root, "fs.start_dir", &fs_start_dir);
    if (ret < 0) {
        log_error("Can't parse 'fs.start_dir' (note that the value must be put in double quotes)!"
                  "\n");
        return ret;
    }

    if (fs_start_dir) {
        struct shim_dentry* dent = NULL;
        ret = path_lookupat(/*start=*/NULL, fs_start_dir, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &dent);
        free(fs_start_dir);
        if (ret < 0) {
            log_error("Invalid 'fs.start_dir' in manifest.\n");
            return ret;
        }
        lock(&g_process.fs_lock);
        put_dentry(g_process.cwd);
        g_process.cwd = dent;
        unlock(&g_process.fs_lock);
    }
    /* Otherwise `cwd` is already initialized. */

    return 0;
}

struct shim_fs* find_fs(const char* name) {
    for (size_t i = 0; i < ARRAY_SIZE(builtin_fs); i++) {
        struct shim_fs* fs = builtin_fs[i];
        if (!strncmp(fs->name, name, sizeof(fs->name)))
            return fs;
    }

    return NULL;
}

static int __mount_fs(struct shim_mount* mount, struct shim_dentry* dent) {
    assert(locked(&g_dcache_lock));

    int ret = 0;

    get_dentry(dent);
    mount->mount_point = dent;
    dent->mounted = mount;

    /* TODO: use `mount->root` as actual filesystem root (see comment for `struct shim_mount`) */
    mount->root = NULL;

    if ((ret = __del_dentry_tree(dent)) < 0)
        return ret;

    lock(&mount_list_lock);
    get_mount(mount);
    LISTP_ADD_TAIL(mount, &mount_list, list);
    unlock(&mount_list_lock);

    do {
        struct shim_dentry* parent = dent->parent;

        if (dent->state & DENTRY_SYNTHETIC) {
            put_dentry(dent);
            break;
        }

        dent->state |= DENTRY_SYNTHETIC;
        if (parent)
            get_dentry(parent);
        put_dentry(dent);
        dent = parent;
    } while (dent);

    return 0;
}

/* Extracts the last component of the `path`. If there's none, `*last_elem_len` is set to 0 and
 * `*last_elem` is set to NULL. */
static void find_last_component(const char* path, const char** last_comp, size_t* last_comp_len) {
    *last_comp = NULL;
    size_t last_len = 0;
    size_t path_len = strlen(path);
    if (path_len == 0)
        goto out;

    // Drop any trailing slashes.
    const char* last = path + path_len - 1;
    while (last > path && *last == '/')
        last--;
    if (*last == '/')
        goto out;

    // Skip the last component.
    last_len = 1;
    while (last > path && *(last - 1) != '/') {
        last--;
        last_len++;
    }
    *last_comp = last;
out:
    *last_comp_len = last_len;
}

/* Parent is optional, but helpful.
 * dentp (optional) memoizes the dentry of the newly-mounted FS, on success.
 *
 * The make_ancestor flag creates pseudo-dentries for any missing paths (passed to _path_lookupat).
 * This is only intended for use to connect mounts specified in the manifest when an intervening
 * path is missing.
 */
int mount_fs(const char* type, const char* uri, const char* mount_point, struct shim_dentry* parent,
             struct shim_dentry** dentp, bool make_ancestor) {
    int ret = 0;
    struct shim_fs* fs = find_fs(type);

    int lookup_flags = LOOKUP_NO_FOLLOW;
    if (make_ancestor)
        lookup_flags |= LOOKUP_MAKE_SYNTHETIC;

    if (!fs || !fs->fs_ops || !fs->fs_ops->mount) {
        ret = -ENODEV;
        goto out;
    }

    /* Split the mount point into the prefix and atom */
    size_t mount_point_len = strlen(mount_point);
    if (mount_point_len == 0) {
        ret = -EINVAL;
        goto out;
    }
    const char* last;
    size_t last_len;
    find_last_component(mount_point, &last, &last_len);

    bool need_parent_put = false;
    lock(&g_dcache_lock);

    if (!parent) {
        // See if we are not at the root mount
        if (last_len > 0) {
            // Look up the parent
            size_t parent_len = last - mount_point;
            char* parent_path = __alloca(parent_len + 1);
            memcpy(parent_path, mount_point, parent_len);
            parent_path[parent_len] = 0;
            if ((ret = _path_lookupat(g_dentry_root, parent_path, lookup_flags, &parent)) < 0) {
                log_error("Path lookup failed %d\n", ret);
                goto out_with_unlock;
            }
            need_parent_put = true;
        }
    }

    struct shim_mount* mount = alloc_mount();
    void* mount_data         = NULL;

    /* call fs-specific mount to allocate mount_data */
    if ((ret = fs->fs_ops->mount(uri, &mount_data)) < 0)
        goto out_with_unlock;

    size_t uri_len = uri ? strlen(uri) : 0;
    qstrsetstr(&mount->path, mount_point, mount_point_len);
    qstrsetstr(&mount->uri, uri, uri_len);
    mount->fs = fs;
    mount->data = mount_data;

    /* Get the negative dentry from the cache, if one exists */
    struct shim_dentry* dent;
    struct shim_dentry* dent2;
    /* Special case the root */
    if (last_len == 0)
        dent = g_dentry_root;
    else {
        dent = lookup_dcache(parent, last, last_len);

        if (!dent) {
            dent = get_new_dentry(mount, parent, last, last_len);
        }
    }

    if (dent != g_dentry_root && dent->state & DENTRY_VALID) {
        log_error("Mount %s already exists, verify that there are no duplicate mounts in manifest\n"
                  "(note that /proc and /dev are automatically mounted in Graphene).\n",
                  mount_point);
        ret = -EEXIST;
        goto out_with_unlock;
    }

    /*Now go ahead and do a lookup so the dentry is valid */
    dent->state |= DENTRY_MOUNTPOINT;
    if ((ret = _path_lookupat(g_dentry_root, mount_point, lookup_flags, &dent2)) < 0) {
        dent->state &= ~DENTRY_MOUNTPOINT;
        goto out_with_unlock;
    }

    assert(dent == dent2);

    /* We want the net impact of mounting to increment the ref count on the
     * entry (until the unmount).  But we shouldn't also hold the reference on
     * dent from the validation step.  Drop it here */
    put_dentry(dent2);

    ret = __mount_fs(mount, dent);

    // If we made it this far and the dentry is still negative, clear
    // the negative flag from the denry.
    if (!ret && (dent->state & DENTRY_NEGATIVE))
        dent->state &= ~DENTRY_NEGATIVE;

    /* Set the file system at the mount point properly */
    dent->mount = mount;
    dent->fs = mount->fs;

    if (dentp && !ret) {
        *dentp = dent;
    } else {
        put_dentry(dent);
    }

out_with_unlock:
    unlock(&g_dcache_lock);
    if (need_parent_put) {
        put_dentry(parent);
    }
out:
    return ret;
}

/*
 * XXX: These two functions are useless - `mount` is not freed even if refcount reaches 0.
 * Unfortunately Graphene is not keeping track of this refcount correctly, so we cannot free
 * the object. Fixing this would require revising whole filesystem implementation - but this code
 * is, uhm, not the best achievement of humankind and probably requires a complete rewrite.
 */
void get_mount(struct shim_mount* mount) {
    __UNUSED(mount);
    // REF_INC(mount->ref_count);
}

void put_mount(struct shim_mount* mount) {
    __UNUSED(mount);
    // REF_DEC(mount->ref_count);
}

int walk_mounts(int (*walk)(struct shim_mount* mount, void* arg), void* arg) {
    struct shim_mount* mount;
    struct shim_mount* n;
    int ret = 0;
    int nsrched = 0;

    lock(&mount_list_lock);

    LISTP_FOR_EACH_ENTRY_SAFE(mount, n, &mount_list, list) {
        if ((ret = (*walk)(mount, arg)) < 0)
            break;

        if (ret > 0)
            nsrched++;
    }

    unlock(&mount_list_lock);
    return ret < 0 ? ret : (nsrched ? 0 : -ESRCH);
}

struct shim_mount* find_mount_from_uri(const char* uri) {
    struct shim_mount* mount;
    struct shim_mount* found = NULL;
    size_t longest_path = 0;

    lock(&mount_list_lock);
    LISTP_FOR_EACH_ENTRY(mount, &mount_list, list) {
        if (qstrempty(&mount->uri))
            continue;

        if (!memcmp(qstrgetstr(&mount->uri), uri, mount->uri.len)) {
            if (mount->path.len > longest_path) {
                longest_path = mount->path.len;
                found = mount;
            }
        }
    }

    if (found)
        get_mount(found);

    unlock(&mount_list_lock);
    return found;
}

/*
 * Note that checkpointing the `shim_fs` structure copies it, instead of using a pointer to
 * corresponding global object on the remote side. This does not waste too much memory (because each
 * global object is only copied once), but it means that `shim_fs` objects cannot be compared by
 * pointer.
 */
BEGIN_CP_FUNC(fs) {
    __UNUSED(size);
    assert(size == sizeof(struct shim_fs));

    struct shim_fs* fs = (struct shim_fs*)obj;
    struct shim_fs* new_fs = NULL;

    size_t off = GET_FROM_CP_MAP(obj);

    if (!off) {
        off = ADD_CP_OFFSET(sizeof(struct shim_fs));
        ADD_TO_CP_MAP(obj, off);

        new_fs = (struct shim_fs*)(base + off);

        memcpy(new_fs->name, fs->name, sizeof(new_fs->name));
        new_fs->fs_ops = NULL;
        new_fs->d_ops = NULL;

        ADD_CP_FUNC_ENTRY(off);
    } else {
        new_fs = (struct shim_fs*)(base + off);
    }

    if (objp)
        *objp = (void*)new_fs;
}
END_CP_FUNC(fs)

BEGIN_RS_FUNC(fs) {
    __UNUSED(offset);
    __UNUSED(rebase);
    struct shim_fs* fs = (void*)(base + GET_CP_FUNC_ENTRY());

    struct shim_fs* builtin_fs = find_fs(fs->name);
    if (!builtin_fs)
        return -EINVAL;

    fs->fs_ops = builtin_fs->fs_ops;
    fs->d_ops = builtin_fs->d_ops;
}
END_RS_FUNC(fs)

BEGIN_CP_FUNC(mount) {
    __UNUSED(size);
    assert(size == sizeof(struct shim_mount));

    struct shim_mount* mount     = (struct shim_mount*)obj;
    struct shim_mount* new_mount = NULL;

    size_t off = GET_FROM_CP_MAP(obj);

    if (!off) {
        off = ADD_CP_OFFSET(sizeof(struct shim_mount));
        ADD_TO_CP_MAP(obj, off);

        mount->cpdata = NULL;
        if (mount->fs->fs_ops && mount->fs->fs_ops->checkpoint) {
            void* cpdata = NULL;
            int bytes = mount->fs->fs_ops->checkpoint(&cpdata, mount->data);
            if (bytes > 0) {
                mount->cpdata = cpdata;
                mount->cpsize = bytes;
            }
        }

        new_mount  = (struct shim_mount*)(base + off);
        *new_mount = *mount;

        DO_CP(fs, mount->fs, &new_mount->fs);

        if (mount->cpdata) {
            size_t cp_off = ADD_CP_OFFSET(mount->cpsize);
            memcpy((char*)base + cp_off, mount->cpdata, mount->cpsize);
            new_mount->cpdata = (char*)base + cp_off;
        }

        new_mount->data        = NULL;
        new_mount->mount_point = NULL;
        new_mount->root        = NULL;
        INIT_LIST_HEAD(new_mount, list);
        REF_SET(new_mount->ref_count, 0);

        DO_CP_IN_MEMBER(qstr, new_mount, path);
        DO_CP_IN_MEMBER(qstr, new_mount, uri);

        if (mount->mount_point)
            DO_CP_MEMBER(dentry, mount, new_mount, mount_point);

        if (mount->root)
            DO_CP_MEMBER(dentry, mount, new_mount, root);

        ADD_CP_FUNC_ENTRY(off);
    } else {
        new_mount = (struct shim_mount*)(base + off);
    }

    if (objp)
        *objp = (void*)new_mount;
}
END_CP_FUNC(mount)

BEGIN_RS_FUNC(mount) {
    __UNUSED(offset);
    struct shim_mount* mount = (void*)(base + GET_CP_FUNC_ENTRY());

    CP_REBASE(mount->cpdata);
    CP_REBASE(mount->list);
    CP_REBASE(mount->mount_point);
    CP_REBASE(mount->root);

    if (mount->mount_point) {
        get_dentry(mount->mount_point);
    }

    if (mount->root) {
        get_dentry(mount->root);
    }

    CP_REBASE(mount->fs);
    if (mount->fs->fs_ops && mount->fs->fs_ops->migrate && mount->cpdata) {
        void* mount_data = NULL;
        if (mount->fs->fs_ops->migrate(mount->cpdata, &mount_data) == 0)
            mount->data = mount_data;
        mount->cpdata = NULL;
    }

    LISTP_ADD_TAIL(mount, &mount_list, list);

    if (!qstrempty(&mount->path)) {
        DEBUG_RS("type=%s,uri=%s,path=%s", mount->type, qstrgetstr(&mount->uri),
                 qstrgetstr(&mount->path));
    } else {
        DEBUG_RS("type=%s,uri=%s", mount->type, qstrgetstr(&mount->uri));
    }
}
END_RS_FUNC(mount)

BEGIN_CP_FUNC(all_mounts) {
    __UNUSED(obj);
    __UNUSED(size);
    __UNUSED(objp);
    struct shim_mount* mount;
    lock(&mount_list_lock);
    LISTP_FOR_EACH_ENTRY(mount, &mount_list, list) {
        DO_CP(mount, mount, NULL);
    }
    unlock(&mount_list_lock);

    /* add an empty entry to mark as migrated */
    ADD_CP_FUNC_ENTRY(0UL);
}
END_CP_FUNC(all_mounts)

BEGIN_RS_FUNC(all_mounts) {
    __UNUSED(entry);
    __UNUSED(base);
    __UNUSED(offset);
    __UNUSED(rebase);
    /* to prevent file system from being mount again */
    mount_migrated = true;
}
END_RS_FUNC(all_mounts)
