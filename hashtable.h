#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <cstddef>
#include <cstdint>

struct HNode {
    HNode *next;
    uint64_t hash_value{};
};

struct HTab {
    HNode **tab;
    std::size_t size{};
    std::size_t mask{};
};

struct HMap {
    HTab newer{};
    HTab older{};
    std::size_t migrate_pos{};
};

HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *));
HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *));
void hm_insert(HMap *hmap, HNode *node);
void hm_clear(HMap *hmap);
void hm_foreach(HMap *hmap, bool (*callback)(HNode *, void *), void *arg);
std::size_t hm_size(HMap *hmap);

#endif
