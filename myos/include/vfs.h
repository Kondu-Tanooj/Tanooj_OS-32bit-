#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_NAME_MAX   32
#define VFS_MAX_FILES  64    /* max files + dirs in the whole fs */

typedef enum {
    VFS_FILE = 1,
    VFS_DIR  = 2,
} vfs_type_t;

typedef struct vfs_node {
    char             name[VFS_NAME_MAX];
    vfs_type_t       type;
    uint32_t         size;       /* bytes of content (files only)   */
    char            *data;       /* heap-allocated content           */
    struct vfs_node *parent;     /* parent directory                 */
    struct vfs_node *children[VFS_MAX_FILES]; /* directory entries   */
    int              child_count;
} vfs_node_t;

void        vfs_init(void);
vfs_node_t *vfs_get_root(void);
vfs_node_t *vfs_get_cwd(void);
void        vfs_set_cwd(vfs_node_t *dir);

/* Operations */
vfs_node_t *vfs_mkdir(vfs_node_t *parent, const char *name);
vfs_node_t *vfs_create(vfs_node_t *parent, const char *name);
int         vfs_write(vfs_node_t *file, const char *data, uint32_t len);
int         vfs_append(vfs_node_t *file, const char *data, uint32_t len);
vfs_node_t *vfs_find(vfs_node_t *dir, const char *name);
vfs_node_t *vfs_resolve(const char *path); /* absolute or relative path */
int         vfs_delete(vfs_node_t *node);
void        vfs_print_tree(vfs_node_t *node, int depth);

#endif
