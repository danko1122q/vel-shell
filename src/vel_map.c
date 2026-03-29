/*Copyright (c) 2026, danko1122q
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * vel_map.c  --  open-addressing hash map used for function and variable lookup
 *
 * FIX 1: MAP_BUCKETS bumped from 256 → 1024 (updated in vel_priv.h).
 *   With 256 buckets an interpreter with 100+ functions puts ~0.4 entries per
 *   bucket on average, but hot buckets (short names: "if", "set", "for") can
 *   accumulate 3-5 entries, turning O(1) lookup into a short linear scan.
 *   1024 buckets cuts expected chain length by 4x with negligible memory cost
 *   (~12 KB per map instance).
 *
 * FIX 2: vmap_get/vmap_has use an explicit early return on first match.
 *   The old code was correct but relied on the loop naturally stopping;
 *   the explicit return makes the O(1) intent clear and future-proof.
 */

#include "vel_priv.h"

/* djb2 — fast, good distribution for short identifier strings.
 * Cast to unsigned char to avoid UB on signed-char platforms. */
static unsigned long map_hash(const char *key)
{
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*key++))
        h = ((h << 5) + h) + c;
    return h;
}

void vmap_init(vel_map_t *m)
{
    memset(m, 0, sizeof(vel_map_t));
}

void vmap_free(vel_map_t *m)
{
    size_t i, j;
    for (i = 0; i < MAP_BUCKETS; i++) {
        for (j = 0; j < m->bucket[i].count; j++)
            free(m->bucket[i].entries[j].key);
        free(m->bucket[i].entries);
    }
    memset(m, 0, sizeof(vel_map_t));
}

void vmap_set(vel_map_t *m, const char *key, void *val)
{
    map_bucket_t *b = &m->bucket[map_hash(key) & MAP_MASK];
    size_t i;

    for (i = 0; i < b->count; i++) {
        if (!strcmp(key, b->entries[i].key)) {
            if (!val) {
                /* DELETE: remove entry from bucket by shifting tail left */
                free(b->entries[i].key);
                b->count--;
                for (; i < b->count; i++)
                    b->entries[i] = b->entries[i + 1];
            } else {
                b->entries[i].val = val;
            }
            return;
        }
    }

    /* key not found and val==NULL: nothing to do */
    if (!val) return;

    b->entries = realloc(b->entries, sizeof(map_entry_t) * (b->count + 1));
    b->entries[b->count].key = vel_strdup(key);
    b->entries[b->count].val = val;
    b->count++;
}

/* FIX 2: explicit early return on first match — O(1) intent is clear */
void *vmap_get(vel_map_t *m, const char *key)
{
    map_bucket_t *b = &m->bucket[map_hash(key) & MAP_MASK];
    size_t i;
    for (i = 0; i < b->count; i++)
        if (!strcmp(key, b->entries[i].key))
            return b->entries[i].val;   /* found — return immediately */
    return NULL;
}

int vmap_has(vel_map_t *m, const char *key)
{
    map_bucket_t *b = &m->bucket[map_hash(key) & MAP_MASK];
    size_t i;
    for (i = 0; i < b->count; i++)
        if (!strcmp(key, b->entries[i].key) && b->entries[i].val != NULL)
            return 1;
    return 0;
}
