#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

struct HNode {
    HNode *next;
    uint64_t hash_value;
};

struct HTab {
    HNode **tab;
    std::size_t size;
    std::size_t mask;
};

static void h_init(std::size_t n, HTab *htab) {
    assert(n > 0 && ((n - 1) & n) == 0);
    htab->tab = static_cast<HNode **>(std::calloc(n, sizeof(HNode *)));
    htab->size = 0;
    htab->mask = n - 1;
}

static void h_insert(HTab *htab, HNode *node) {
    std::size_t pos{node->hash_value & htab->mask};
    node->next = htab->tab[pos];
    htab->tab[pos] = node;
}

static HNode **h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *)) {
    std::size_t pos{key->hash_value & htab->mask};
    HNode **from = &htab->tab[pos];
    for (HNode *cur; (cur = *from) != NULL; from = &cur->next) {
        if (cur->hash_value == key->hash_value && eq(cur, key)) {
            return from;
        }
    }
    return NULL;
}

static HNode *h_detach(HTab *htab, HNode **target) {
    HNode *node = *target;
    *target = node->next;
    htab->size--;
    return node;
}
