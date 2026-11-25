/*
 * vfs.c — TanoojOS in-memory virtual filesystem
 *
 * A simple tree of vfs_node_t structs allocated from the heap.
 * Directories hold pointers to children (files + subdirs).
 * Files hold a heap-allocated data buffer.
 *
 * Layout on boot:
 *   /               ← root directory
 *   ├── home/       ← home directory
 *   ├── etc/        ← config files
 *   └── readme.txt  ← welcome file
 */

#include "vfs.h"
#include "heap.h"
#include "terminal.h"
#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
   String helpers
   ----------------------------------------------------------------------- */
static int vfs_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void vfs_strcpy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int vfs_strlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

static void vfs_memcpy(char *dst, const char *src, int n) {
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

/* -----------------------------------------------------------------------
   State
   ----------------------------------------------------------------------- */
static vfs_node_t *root = NULL;
static vfs_node_t *cwd  = NULL;  /* current working directory */

/* -----------------------------------------------------------------------
   Node allocation
   ----------------------------------------------------------------------- */
static vfs_node_t *vfs_alloc_node(const char *name, vfs_type_t type) {
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;

    /* Zero the struct */
    char *p = (char *)node;
    for (int i = 0; i < (int)sizeof(vfs_node_t); i++) p[i] = 0;

    vfs_strcpy(node->name, name, VFS_NAME_MAX);
    node->type        = type;
    node->size        = 0;
    node->data        = NULL;
    node->parent      = NULL;
    node->child_count = 0;
    return node;
}

/* -----------------------------------------------------------------------
   Public API
   ----------------------------------------------------------------------- */
vfs_node_t *vfs_get_root(void) { return root; }
vfs_node_t *vfs_get_cwd(void)  { return cwd;  }
void        vfs_set_cwd(vfs_node_t *dir) { if (dir && dir->type == VFS_DIR) cwd = dir; }

vfs_node_t *vfs_mkdir(vfs_node_t *parent, const char *name) {
    if (!parent || parent->type != VFS_DIR) return NULL;
    if (parent->child_count >= VFS_MAX_FILES) return NULL;
    if (vfs_find(parent, name)) return NULL; /* already exists */

    vfs_node_t *dir = vfs_alloc_node(name, VFS_DIR);
    if (!dir) return NULL;
    dir->parent = parent;
    parent->children[parent->child_count++] = dir;
    return dir;
}

vfs_node_t *vfs_create(vfs_node_t *parent, const char *name) {
    if (!parent || parent->type != VFS_DIR) return NULL;
    if (parent->child_count >= VFS_MAX_FILES) return NULL;
    if (vfs_find(parent, name)) return NULL;

    vfs_node_t *file = vfs_alloc_node(name, VFS_FILE);
    if (!file) return NULL;
    file->parent = parent;
    parent->children[parent->child_count++] = file;
    return file;
}

int vfs_write(vfs_node_t *file, const char *data, uint32_t len) {
    if (!file || file->type != VFS_FILE) return -1;

    /* Free old data if any */
    if (file->data) { kfree(file->data); file->data = NULL; }

    file->data = (char *)kmalloc(len + 1);
    if (!file->data) return -1;

    vfs_memcpy(file->data, data, (int)len);
    file->data[len] = 0;
    file->size = len;
    return (int)len;
}

int vfs_append(vfs_node_t *file, const char *data, uint32_t len) {
    if (!file || file->type != VFS_FILE) return -1;

    uint32_t new_size = file->size + len;
    char *new_data = (char *)kmalloc(new_size + 1);
    if (!new_data) return -1;

    if (file->data) vfs_memcpy(new_data, file->data, (int)file->size);
    vfs_memcpy(new_data + file->size, data, (int)len);
    new_data[new_size] = 0;

    if (file->data) kfree(file->data);
    file->data = new_data;
    file->size = new_size;
    return (int)len;
}

vfs_node_t *vfs_find(vfs_node_t *dir, const char *name) {
    if (!dir || dir->type != VFS_DIR) return NULL;
    for (int i = 0; i < dir->child_count; i++)
        if (vfs_strcmp(dir->children[i]->name, name) == 0)
            return dir->children[i];
    return NULL;
}

/* Resolve a path — supports absolute (/home/file) and relative (file, ../x) */
vfs_node_t *vfs_resolve(const char *path) {
    if (!path || !*path) return cwd;

    vfs_node_t *cur = (path[0] == '/') ? root : cwd;
    if (!cur) return NULL;

    /* Walk path components */
    char component[VFS_NAME_MAX];
    int  i = (path[0] == '/') ? 1 : 0;

    while (path[i]) {
        /* Extract next component */
        int j = 0;
        while (path[i] && path[i] != '/' && j < VFS_NAME_MAX - 1)
            component[j++] = path[i++];
        component[j] = 0;
        if (path[i] == '/') i++;
        if (j == 0) continue;

        /* Handle . and .. */
        if (component[0] == '.' && component[1] == 0) continue;
        if (component[0] == '.' && component[1] == '.' && component[2] == 0) {
            if (cur->parent) cur = cur->parent;
            continue;
        }

        cur = vfs_find(cur, component);
        if (!cur) return NULL;
    }
    return cur;
}

int vfs_delete(vfs_node_t *node) {
    if (!node || node == root) return -1;
    vfs_node_t *parent = node->parent;
    if (!parent) return -1;

    /* Can't delete non-empty directory */
    if (node->type == VFS_DIR && node->child_count > 0) return -2;

    /* Free file data */
    if (node->data) kfree(node->data);

    /* Remove from parent's children array */
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == node) {
            /* Shift remaining children left */
            for (int j = i; j < parent->child_count - 1; j++)
                parent->children[j] = parent->children[j + 1];
            parent->children[parent->child_count - 1] = NULL;
            parent->child_count--;
            break;
        }
    }

    kfree(node);
    return 0;
}

/* Print directory tree (for 'tree' command) */
void vfs_print_tree(vfs_node_t *node, int depth) {
    if (!node) return;

    /* Indent */
    for (int i = 0; i < depth; i++) terminal_write("  ");

    if (node->type == VFS_DIR) {
        terminal_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        terminal_write(node->name);
        terminal_writeline("/");
        for (int i = 0; i < node->child_count; i++)
            vfs_print_tree(node->children[i], depth + 1);
    } else {
        terminal_set_color(COLOR_WHITE, COLOR_BLACK);
        terminal_write(node->name);
        terminal_write("  (");
        terminal_write_dec(node->size);
        terminal_writeline(" B)");
    }
}

/* -----------------------------------------------------------------------
   vfs_init — create root and seed with some initial content
   ----------------------------------------------------------------------- */
void vfs_init(void) {
    root = vfs_alloc_node("/", VFS_DIR);
    if (!root) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_writeline("  [VFS] Failed to allocate root!");
        return;
    }
    root->parent = root; /* root's parent is itself */
    cwd = root;

    /* Create initial directory structure */
    vfs_node_t *home = vfs_mkdir(root, "home");
    vfs_node_t *etc  = vfs_mkdir(root, "etc");
    vfs_node_t *docs = vfs_mkdir(root, "docs");
    (void)etc;

    /* Create a welcome file */
    vfs_node_t *readme = vfs_create(root, "readme.txt");
    const char *readme_txt =
        "Welcome to TanoojOS!\n"
        "This is an in-memory filesystem.\n"
        "Files live in RAM and reset on reboot.\n"
        "Type 'help' for available commands.\n";
    vfs_write(readme, readme_txt, (uint32_t)vfs_strlen(readme_txt));

    /* Create a file in home */
    vfs_node_t *note = vfs_create(home, "notes.txt");
    vfs_write(note, "My first note in TanoojOS!\n", 26);

    /* Create a config file */
    vfs_node_t *cfg = vfs_create(etc, "config.txt");
    vfs_write(cfg, "hostname=TanoojOS\nversion=0.1\n", 30);

    /* docs dir */
    vfs_node_t *doc1 = vfs_create(docs, "about.txt");
    vfs_write(doc1, "TanoojOS - built from scratch in x86 assembly and C.\n", 53);

}
