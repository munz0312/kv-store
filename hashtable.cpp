#include "hashtable.h"
#include <cassert>
#include <cstddef>
#include <cstdlib>

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
    htab->size++;
}

static HNode **h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *)) {
    if (!htab->tab)
        return NULL;
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

static void h_foreach(HTab *htab, bool (*callback)(HNode *, void *),
                      void *arg) {
    if (!htab->tab) {
        return;
    }

    for (std::size_t i = 0; i <= htab->mask; i++) {
        for (HNode *n = htab->tab[i]; n != NULL; n = n->next) {
            if (!callback(n, arg))
                return;
        }
    }
}

constexpr std::size_t k_max_load_factor{8};

constexpr std::size_t k_rehashing_work{128};

static void hm_help_rehashing(HMap *hmap) {
    std::size_t nwork{0};
    while (nwork < k_rehashing_work && hmap->older.size > 0) {
        HNode **from = &hmap->older.tab[hmap->migrate_pos];
        if (!*from) {
            hmap->migrate_pos++;
            continue;
        }
        h_insert(&hmap->newer, h_detach(&hmap->older, from));
        nwork++;
    }

    if (hmap->older.size == 0 && hmap->older.tab) {
        free(hmap->older.tab);
        hmap->older = HTab{};
    }
}

static void hm_trigger_rehashing(HMap *hmap) {
    hmap->older = hmap->newer;
    h_init((hmap->newer.size + 1) * 2, &hmap->newer);
}

HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);
    HNode **from = h_lookup(&hmap->newer, key, eq);
    if (!from) {
        from = h_lookup(&hmap->older, key, eq);
    }
    return from ? *from : NULL;
};

HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);
    if (HNode **from = h_lookup(&hmap->newer, key, eq)) {
        return h_detach(&hmap->newer, from);
    }
    if (HNode **from = h_lookup(&hmap->older, key, eq)) {
        return h_detach(&hmap->older, from);
    }
    return NULL;
}

void hm_insert(HMap *hmap, HNode *node) {
    if (!(hmap->newer.tab)) {
        h_init(4, &hmap->newer);
    }
    h_insert(&hmap->newer, node);
    if (!(hmap->older.tab)) {
        std::size_t threshold{(hmap->newer.mask + 1) * k_max_load_factor};
        if (hmap->newer.size >= threshold) {
            hm_trigger_rehashing(hmap);
        }
    }

    hm_help_rehashing(hmap);
}

void hm_clear(HMap *hmap) {
    free(hmap->older.tab);
    free(hmap->newer.tab);
    *hmap = HMap{};
}

std::size_t hm_size(HMap *hmap) { return hmap->older.size + hmap->newer.size; }

void hm_foreach(HMap *hmap, bool (*callback)(HNode *, void *), void *arg) {
    h_foreach(&hmap->newer, callback, arg);
    h_foreach(&hmap->older, callback, arg);
}
