#include <algorithm>
#include <cstdint>
struct AVLNode {
    AVLNode *parent{nullptr};
    AVLNode *left{nullptr};
    AVLNode *right{nullptr};
    uint32_t height{0};
};

inline void avl_init(AVLNode *node) {
    node->left = node->right = node->parent = nullptr;
    node->height = 1;
}

static uint32_t avl_height(AVLNode *node) { return node ? node->height : 0; }

static void avl_update(AVLNode *node) {
    node->height =
        1 + std::max(avl_height(node->left), avl_height(node->right));
}

static AVLNode *rot_left(AVLNode *node) {
    AVLNode *parent{node->parent};
    AVLNode *new_node{node->right};
    AVLNode *inner{new_node->left};

    node->right = inner;
    if (inner) {
        inner->parent = node;
    }

    new_node->parent = parent;
    new_node->left = node;
    node->parent = new_node;
    avl_update(node);
    avl_update(new_node);
    return new_node;
}

static AVLNode *rot_right(AVLNode *node) {
    AVLNode *parent{node->parent};
    AVLNode *new_node{node->left};
    AVLNode *inner{new_node->right};

    node->left = inner;
    if (inner) {
        inner->parent = node;
    }

    new_node->parent = parent;
    new_node->right = node;
    node->parent = new_node;
    avl_update(node);
    avl_update(new_node);
    return new_node;
}

static AVLNode *avl_fix_left(AVLNode *node) {
    if (avl_height(node->left->left) < avl_height(node->left->right)) {
        node->left = rot_left(node->left);
    }

    return rot_right(node);
}
