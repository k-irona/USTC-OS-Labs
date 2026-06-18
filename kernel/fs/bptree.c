#include "types.h"
#include "defs.h"
#include "bptree.h"

struct bptree_node {
    int leaf;
    int nkey;
    char *keys[BPTREE_MAX_KEYS + 1];
    struct bptree_node *child[BPTREE_MAX_KEYS + 2];
    struct bptree_values values[BPTREE_MAX_KEYS + 1];
    struct bptree_node *next;
};

void bptree_init(struct bptree *tree, bptree_alloc_fn alloc, void *arg) {
    if (tree == 0) {
        return;
    }
    tree->root = 0;
    tree->alloc = alloc;
    tree->alloc_arg = arg;
    tree->oom = 0;
}

void bptree_reset(struct bptree *tree) {
    if (tree == 0) {
        return;
    }
    tree->root = 0;
    tree->oom = 0;
}

static void *bptree_alloc(struct bptree *tree, uint n) {
    if (tree == 0 || tree->alloc == 0) {
        return 0;
    }
    void *p = tree->alloc(n, tree->alloc_arg);
    if (p == 0) {
        tree->oom = 1;
    }
    return p;
}

static int bptree_keycmp_token(const char *key, const char *token, int len) {
    int i = 0;
    while (key[i] != 0 && i < len) {
        uchar kc = (uchar)key[i];
        uchar tc = (uchar)token[i];
        if (kc != tc) {
            return (int)kc - (int)tc;
        }
        i++;
    }
    if (key[i] == 0 && i == len) {
        return 0;
    }
    return key[i] == 0 ? -1 : 1;
}

static char *bptree_strdup_key(struct bptree *tree, const char *key, int len) {
    char *s = (char *)bptree_alloc(tree, (uint)(len + 1));
    if (s == 0) {
        return 0;
    }
    memmove(s, key, (uint)len);
    s[len] = 0;
    return s;
}

int bptree_values_contains(struct bptree_values *values, void *value) {
    if (values == 0 || value == 0) {
        return 0;
    }
    for (struct bptree_value_chunk *c = values->head; c; c = c->next) {
        for (int i = 0; i < c->n; i++) {
            if (c->values[i] == value) {
                return 1;
            }
        }
    }
    return 0;
}

static int bptree_values_add(struct bptree *tree, struct bptree_values *values, void *value) {
    if (values == 0 || value == 0) {
        return -1;
    }
    if (bptree_values_contains(values, value)) {
        return 0;
    }
    if (values->tail == 0 || values->tail->n >= BPTREE_VALUES_PER_CHUNK) {
        struct bptree_value_chunk *chunk =
            (struct bptree_value_chunk *)bptree_alloc(tree, sizeof(*chunk));
        if (chunk == 0) {
            return -1;
        }
        if (values->tail) {
            values->tail->next = chunk;
        } else {
            values->head = chunk;
        }
        values->tail = chunk;
    }
    values->tail->values[values->tail->n++] = value;
    values->count++;
    return 0;
}

void bptree_values_remove(struct bptree_values *values, void *value) {
    if (values == 0 || value == 0) {
        return;
    }
    for (struct bptree_value_chunk *c = values->head; c; c = c->next) {
        for (int i = 0; i < c->n; i++) {
            if (c->values[i] == value) {
                for (int j = i + 1; j < c->n; j++) {
                    c->values[j - 1] = c->values[j];
                }
                c->n--;
                values->count--;
                return;
            }
        }
    }
}

static struct bptree_node *bptree_new_node(struct bptree *tree, int leaf) {
    struct bptree_node *node = (struct bptree_node *)bptree_alloc(tree, sizeof(*node));
    if (node == 0) {
        return 0;
    }
    memset(node, 0, sizeof(*node));
    node->leaf = leaf;
    return node;
}

struct bptree_values *bptree_lookup(struct bptree *tree, const char *key, int len) {
    if (tree == 0 || key == 0 || len <= 0) {
        return 0;
    }

    /*
     * LAB BONUS TODO [B.1]
     *
     * Search from tree->root down to a leaf. In each internal node, choose the
     * child whose key range may contain key[0..len). In the leaf, return the
     * posting list for an equal key, or 0 when the keyword is absent.
     *
     * Hint: bptree_keycmp_token(node->keys[i], key, len) compares a stored
     * zero-terminated key with a non-zero-terminated token slice.
     */
    struct bptree_node *node = tree->root;
    while (node && !node->leaf) {
        int i = 0;
        while (i < node->nkey && bptree_keycmp_token(node->keys[i], key, len) <= 0) {
            i++;
        }
        node = node->child[i];
    }

    if (node == 0) {
        return 0;
    }
    for (int i = 0; i < node->nkey; i++) {
        int cmp = bptree_keycmp_token(node->keys[i], key, len);
        if (cmp == 0) {
            return &node->values[i];
        }
        if (cmp > 0) {
            break;
        }
    }
    return 0;
}

static int bptree_split_leaf(struct bptree *tree, struct bptree_node *node,
                             char **promoted, struct bptree_node **right) {
    /*
     * LAB BONUS TODO [B.1]
     *
     * Split an overflowing leaf node. Move the upper half of keys and posting
     * lists into a new right leaf, link the leaf chain, and promote the first
     * key of the right leaf to the parent.
     */
    struct bptree_node *r = bptree_new_node(tree, 1);
    if (r == 0) {
        return -1;
    }

    int old_nkey = node->nkey;
    int left_nkey = old_nkey / 2;
    int right_nkey = old_nkey - left_nkey;
    for (int i = 0; i < right_nkey; i++) {
        int from = left_nkey + i;
        r->keys[i] = node->keys[from];
        r->values[i] = node->values[from];
        node->keys[from] = 0;
        memset(&node->values[from], 0, sizeof(node->values[from]));
    }
    node->nkey = left_nkey;
    r->nkey = right_nkey;
    r->next = node->next;
    node->next = r;

    *promoted = r->keys[0];
    *right = r;
    return 0;
}

static int bptree_split_internal(struct bptree *tree, struct bptree_node *node,
                                 char **promoted, struct bptree_node **right) {
    /*
     * LAB BONUS TODO [B.1]
     *
     * Split an overflowing internal node. Promote the middle separator key and
     * move the keys/children on its right into a new internal node.
     */
    struct bptree_node *r = bptree_new_node(tree, 0);
    if (r == 0) {
        return -1;
    }

    int old_nkey = node->nkey;
    int mid = old_nkey / 2;
    *promoted = node->keys[mid];

    int right_nkey = old_nkey - mid - 1;
    for (int i = 0; i < right_nkey; i++) {
        r->keys[i] = node->keys[mid + 1 + i];
        node->keys[mid + 1 + i] = 0;
    }
    for (int i = 0; i < right_nkey + 1; i++) {
        r->child[i] = node->child[mid + 1 + i];
        node->child[mid + 1 + i] = 0;
    }
    node->keys[mid] = 0;
    node->nkey = mid;
    r->nkey = right_nkey;
    *right = r;
    return 0;
}

static int bptree_insert_rec(struct bptree *tree, struct bptree_node *node,
                             const char *key, int len, void *value,
                             char **promoted, struct bptree_node **right) {
    /*
     * LAB BONUS TODO [B.1]
     *
     * Recursively insert value for key[0..len).
     *
     * Leaf case:
     *   - If the key already exists, append value to its posting list with
     *     bptree_values_add().
     *   - Otherwise copy the key with bptree_strdup_key(), insert a new sorted
     *     leaf entry, and create its posting list.
     *   - If the leaf overflows, call bptree_split_leaf().
     *
     * Internal case:
     *   - Descend to the correct child.
     *   - If the child split, insert the promoted separator and right child.
     *   - If this node overflows, call bptree_split_internal().
     *
     * On return, set *promoted and *right when this node split; otherwise set
     * them to 0.
     */
    *promoted = 0;
    *right = 0;

    if (node->leaf) {
        int pos = 0;
        while (pos < node->nkey) {
            int cmp = bptree_keycmp_token(node->keys[pos], key, len);
            if (cmp == 0) {
                return bptree_values_add(tree, &node->values[pos], value);
            }
            if (cmp > 0) {
                break;
            }
            pos++;
        }

        char *stored_key = bptree_strdup_key(tree, key, len);
        if (stored_key == 0) {
            return -1;
        }
        for (int i = node->nkey; i > pos; i--) {
            node->keys[i] = node->keys[i - 1];
            node->values[i] = node->values[i - 1];
        }
        node->keys[pos] = stored_key;
        memset(&node->values[pos], 0, sizeof(node->values[pos]));
        node->nkey++;

        if (bptree_values_add(tree, &node->values[pos], value) < 0) {
            for (int i = pos + 1; i < node->nkey; i++) {
                node->keys[i - 1] = node->keys[i];
                node->values[i - 1] = node->values[i];
            }
            node->nkey--;
            node->keys[node->nkey] = 0;
            memset(&node->values[node->nkey], 0, sizeof(node->values[node->nkey]));
            return -1;
        }
        if (node->nkey > BPTREE_MAX_KEYS) {
            return bptree_split_leaf(tree, node, promoted, right);
        }
        return 0;
    }

    int child_pos = 0;
    while (child_pos < node->nkey &&
           bptree_keycmp_token(node->keys[child_pos], key, len) <= 0) {
        child_pos++;
    }

    char *child_promoted = 0;
    struct bptree_node *child_right = 0;
    if (bptree_insert_rec(tree, node->child[child_pos], key, len, value,
                          &child_promoted, &child_right) < 0) {
        return -1;
    }
    if (child_promoted == 0) {
        return 0;
    }

    for (int i = node->nkey; i > child_pos; i--) {
        node->keys[i] = node->keys[i - 1];
    }
    for (int i = node->nkey + 1; i > child_pos + 1; i--) {
        node->child[i] = node->child[i - 1];
    }
    node->keys[child_pos] = child_promoted;
    node->child[child_pos + 1] = child_right;
    node->nkey++;

    if (node->nkey > BPTREE_MAX_KEYS) {
        return bptree_split_internal(tree, node, promoted, right);
    }
    return 0;
}

int bptree_insert(struct bptree *tree, const char *key, int len, void *value) {
    if (tree == 0 || len <= 0 || value == 0) {
        return -1;
    }

    /*
     * LAB BONUS TODO [B.1]
     *
     * Create the root leaf when the tree is empty. Then call bptree_insert_rec().
     * If the old root splits, allocate a new internal root containing the
     * promoted separator key and two children.
     */
    if (tree->root == 0) {
        tree->root = bptree_new_node(tree, 1);
        if (tree->root == 0) {
            return -1;
        }
    }

    char *promoted = 0;
    struct bptree_node *right = 0;
    struct bptree_node *old_root = tree->root;
    if (bptree_insert_rec(tree, old_root, key, len, value, &promoted, &right) < 0) {
        return -1;
    }
    if (promoted == 0) {
        return 0;
    }

    struct bptree_node *new_root = bptree_new_node(tree, 0);
    if (new_root == 0) {
        return -1;
    }
    new_root->keys[0] = promoted;
    new_root->child[0] = old_root;
    new_root->child[1] = right;
    new_root->nkey = 1;
    tree->root = new_root;
    return 0;
}
