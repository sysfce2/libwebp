// Copyright 2017 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Improves a given set of backward references by analyzing its bit cost.
// The algorithm is similar to the Zopfli compression algorithm but tailored to
// images.
//
// Author: Vincent Rabaud (vrabaud@google.com)
//

#include <assert.h>
#include <string.h>

#include "src/dsp/lossless_common.h"
#include "src/enc/backward_references_enc.h"
#include "src/enc/histogram_enc.h"
#include "src/utils/color_cache_utils.h"
#include "src/utils/utils.h"
#include "src/webp/format_constants.h"
#include "src/webp/types.h"

#define VALUES_IN_BYTE 256

extern void VP8LClearBackwardRefs(VP8LBackwardRefs* const refs);
extern int VP8LDistanceToPlaneCode(int xsize, int dist);
extern void VP8LBackwardRefsCursorAdd(VP8LBackwardRefs* const refs,
                                      const PixOrCopy v);

typedef struct {
  uint32_t alpha[VALUES_IN_BYTE];
  uint32_t red[VALUES_IN_BYTE];
  uint32_t blue[VALUES_IN_BYTE];
  uint32_t distance[NUM_DISTANCE_CODES];
  uint32_t* literal;
} CostModel;

static void ConvertPopulationCountTableToBitEstimates(
    int num_symbols, const uint32_t population_counts[], uint32_t output[]) {
  uint32_t sum = 0;
  int nonzeros = 0;
  int i;
  for (i = 0; i < num_symbols; ++i) {
    sum += population_counts[i];
    if (population_counts[i] > 0) {
      ++nonzeros;
    }
  }
  if (nonzeros <= 1) {
    memset(output, 0, num_symbols * sizeof(*output));
  } else {
    const uint32_t logsum = VP8LFastLog2(sum);
    for (i = 0; i < num_symbols; ++i) {
      output[i] = logsum - VP8LFastLog2(population_counts[i]);
    }
  }
}

static int CostModelBuild(CostModel* const m, int xsize, int cache_bits,
                          const VP8LBackwardRefs* const refs) {
  int ok = 0;
  VP8LHistogram* const histo = VP8LAllocateHistogram(cache_bits);
  if (histo == NULL) goto Error;

  // The following code is similar to VP8LHistogramCreate but converts the
  // distance to plane code.
  VP8LHistogramInit(histo, cache_bits, /*init_arrays=*/1);
  VP8LHistogramStoreRefs(refs, VP8LDistanceToPlaneCode, xsize, histo);

  ConvertPopulationCountTableToBitEstimates(
      VP8LHistogramNumCodes(histo->palette_code_bits), histo->literal,
      m->literal);
  ConvertPopulationCountTableToBitEstimates(VALUES_IN_BYTE, histo->red, m->red);
  ConvertPopulationCountTableToBitEstimates(VALUES_IN_BYTE, histo->blue,
                                            m->blue);
  ConvertPopulationCountTableToBitEstimates(VALUES_IN_BYTE, histo->alpha,
                                            m->alpha);
  ConvertPopulationCountTableToBitEstimates(NUM_DISTANCE_CODES, histo->distance,
                                            m->distance);
  ok = 1;

Error:
  VP8LFreeHistogram(histo);
  return ok;
}

static WEBP_INLINE int64_t GetLiteralCost(const CostModel* const m,
                                          uint32_t v) {
  return (int64_t)m->alpha[v >> 24] + m->red[(v >> 16) & 0xff] +
         m->literal[(v >> 8) & 0xff] + m->blue[v & 0xff];
}

static WEBP_INLINE int64_t GetCacheCost(const CostModel* const m,
                                        uint32_t idx) {
  const int literal_idx = VALUES_IN_BYTE + NUM_LENGTH_CODES + idx;
  return (int64_t)m->literal[literal_idx];
}

static WEBP_INLINE int64_t GetLengthCost(const CostModel* const m,
                                         uint32_t length) {
  int code, extra_bits;
  VP8LPrefixEncodeBits(length, &code, &extra_bits);
  return (int64_t)m->literal[VALUES_IN_BYTE + code] +
         ((int64_t)extra_bits << LOG_2_PRECISION_BITS);
}

static WEBP_INLINE int64_t GetDistanceCost(const CostModel* const m,
                                           uint32_t distance) {
  int code, extra_bits;
  VP8LPrefixEncodeBits(distance, &code, &extra_bits);
  return (int64_t)m->distance[code] +
         ((int64_t)extra_bits << LOG_2_PRECISION_BITS);
}

static WEBP_INLINE void AddSingleLiteralWithCostModel(
    const uint32_t* const argb, VP8LColorCache* const hashers,
    const CostModel* const cost_model, int idx, int use_color_cache,
    int64_t prev_cost, int64_t* const cost, uint16_t* const dist_array) {
  int64_t cost_val = prev_cost;
  const uint32_t color = argb[idx];
  const int ix = use_color_cache ? VP8LColorCacheContains(hashers, color) : -1;
  if (ix >= 0) {
    // use_color_cache is true and hashers contains color
    cost_val += DivRound(GetCacheCost(cost_model, ix) * 68, 100);
  } else {
    if (use_color_cache) VP8LColorCacheInsert(hashers, color);
    cost_val += DivRound(GetLiteralCost(cost_model, color) * 82, 100);
  }
  if (cost[idx] > cost_val) {
    cost[idx] = cost_val;
    dist_array[idx] = 1;  // only one is inserted.
  }
}

// -----------------------------------------------------------------------------
// CostManager and interval handling

// Empirical value to avoid high memory consumption but good for performance.
#define COST_CACHE_INTERVAL_SIZE_MAX 500

// To perform backward reference every pixel at index 'index' is considered and
// the cost for the MAX_LENGTH following pixels computed. Those following pixels
// at index 'index' + k (k from 0 to MAX_LENGTH) have a cost of:
//     cost = distance cost at index + GetLengthCost(cost_model, k)
// and the minimum value is kept. GetLengthCost(cost_model, k) is cached in an
// array of size MAX_LENGTH.
// Instead of performing MAX_LENGTH comparisons per pixel, we keep track of the
// minimal values using intervals of constant cost.
// An interval is defined by the 'index' of the pixel that generated it and
// is only useful in a range of indices from 'start' to 'end' (exclusive), i.e.
// it contains the minimum value for pixels between start and end.
// Intervals are stored in a linked list and ordered by 'start'. When a new
// interval has a better value, old intervals are split or removed. There are
// therefore no overlapping intervals.
typedef struct CostInterval CostInterval;
struct CostInterval {
  int64_t cost;
  int start;
  int end;
  int index;
  CostInterval* previous;
  CostInterval* next;
};

// The GetLengthCost(cost_model, k) are cached in a CostCacheInterval.
typedef struct {
  int64_t cost;
  int start;
  int end;  // Exclusive.
} CostCacheInterval;

// This structure is in charge of managing intervals and costs.
// It caches the different CostCacheInterval, caches the different
// GetLengthCost(cost_model, k) in cost_cache and the CostInterval's (whose
// 'count' is limited by COST_CACHE_INTERVAL_SIZE_MAX).
#define COST_MANAGER_MAX_FREE_LIST 10
typedef struct {
  CostInterval* head;
  int count;  // The number of stored intervals.
  CostCacheInterval* cache_intervals;
  size_t cache_intervals_size;
  // Contains the GetLengthCost(cost_model, k).
  int64_t cost_cache[MAX_LENGTH];
  int64_t* costs;
  uint16_t* dist_array;
  // Most of the time, we only need few intervals -> use a free-list, to avoid
  // fragmentation with small allocs in most common cases.
  CostInterval intervals[COST_MANAGER_MAX_FREE_LIST];
  CostInterval* free_intervals;
  // These are regularly malloc'd remains. This list can't grow larger than than
  // size COST_CACHE_INTERVAL_SIZE_MAX - COST_MANAGER_MAX_FREE_LIST, note.
  CostInterval* recycled_intervals;
} CostManager;

static void CostIntervalAddToFreeList(CostManager* const manager,
                                      CostInterval* const interval) {
  interval->next = manager->free_intervals;
  manager->free_intervals = interval;
}

static int CostIntervalIsInFreeList(const CostManager* const manager,
                                    const CostInterval* const interval) {
  return (interval >= &manager->intervals[0] &&
          interval <= &manager->intervals[COST_MANAGER_MAX_FREE_LIST - 1]);
}

static void CostManagerInitFreeList(CostManager* const manager) {
  int i;
  manager->free_intervals = NULL;
  for (i = 0; i < COST_MANAGER_MAX_FREE_LIST; ++i) {
    CostIntervalAddToFreeList(manager, &manager->intervals[i]);
  }
}

static void DeleteIntervalList(CostManager* const manager,
                               const CostInterval* interval) {
  while (interval != NULL) {
    const CostInterval* const next = interval->next;
    if (!CostIntervalIsInFreeList(manager, interval)) {
      WebPSafeFree((void*)interval);
    }  // else: do nothing
    interval = next;
  }
}

static void CostManagerClear(CostManager* const manager) {
  if (manager == NULL) return;

  WebPSafeFree(manager->costs);
  WebPSafeFree(manager->cache_intervals);

  // Clear the interval lists.
  DeleteIntervalList(manager, manager->head);
  manager->head = NULL;
  DeleteIntervalList(manager, manager->recycled_intervals);
  manager->recycled_intervals = NULL;

  // Reset pointers, 'count' and 'cache_intervals_size'.
  memset(manager, 0, sizeof(*manager));
  CostManagerInitFreeList(manager);
}

static int CostManagerInit(CostManager* const manager,
                           uint16_t* const dist_array, int pix_count,
                           const CostModel* const cost_model) {
  int i;
  const int cost_cache_size = (pix_count > MAX_LENGTH) ? MAX_LENGTH : pix_count;

  manager->costs = NULL;
  manager->cache_intervals = NULL;
  manager->head = NULL;
  manager->recycled_intervals = NULL;
  manager->count = 0;
  manager->dist_array = dist_array;
  CostManagerInitFreeList(manager);

  // Fill in the 'cost_cache'.
  // Has to be done in two passes due to a GCC bug on i686
  // related to https://gcc.gnu.org/bugzilla/show_bug.cgi?id=323
  for (i = 0; i < cost_cache_size; ++i) {
    manager->cost_cache[i] = GetLengthCost(cost_model, i);
  }
  manager->cache_intervals_size = 1;
  for (i = 1; i < cost_cache_size; ++i) {
    // Get the number of bound intervals.
    if (manager->cost_cache[i] != manager->cost_cache[i - 1]) {
      ++manager->cache_intervals_size;
    }
  }

  // With the current cost model, we usually have below 20 intervals.
  // The worst case scenario with a cost model would be if every length has a
  // different cost, hence MAX_LENGTH but that is impossible with the current
  // implementation that spirals around a pixel.
  assert(manager->cache_intervals_size <= MAX_LENGTH);
  manager->cache_intervals = (CostCacheInterval*)WebPSafeMalloc(
      manager->cache_intervals_size, sizeof(*manager->cache_intervals));
  if (manager->cache_intervals == NULL) {
    CostManagerClear(manager);
    return 0;
  }

  // Fill in the 'cache_intervals'.
  {
    CostCacheInterval* cur = manager->cache_intervals;

    // Consecutive values in 'cost_cache' are compared and if a big enough
    // difference is found, a new interval is created and bounded.
    cur->start = 0;
    cur->end = 1;
    cur->cost = manager->cost_cache[0];
    for (i = 1; i < cost_cache_size; ++i) {
      const int64_t cost_val = manager->cost_cache[i];
      if (cost_val != cur->cost) {
        ++cur;
        // Initialize an interval.
        cur->start = i;
        cur->cost = cost_val;
      }
      cur->end = i + 1;
    }
    assert((size_t)(cur - manager->cache_intervals) + 1 ==
           manager->cache_intervals_size);
  }

  manager->costs = (int64_t*)WebPSafeMalloc(pix_count, sizeof(*manager->costs));
  if (manager->costs == NULL) {
    CostManagerClear(manager);
    return 0;
  }
  // Set the initial 'costs' to INT64_MAX for every pixel as we will keep the
  // minimum.
  for (i = 0; i < pix_count; ++i) manager->costs[i] = WEBP_INT64_MAX;

  return 1;
}

// Given the cost and the position that define an interval, update the cost at
// pixel 'i' if it is smaller than the previously computed value.
static WEBP_INLINE void UpdateCost(CostManager* const manager, int i,
                                   int position, int64_t cost) {
  const int k = i - position;
  assert(k >= 0 && k < MAX_LENGTH);

  if (manager->costs[i] > cost) {
    manager->costs[i] = cost;
    manager->dist_array[i] = k + 1;
  }
}

// Given the cost and the position that define an interval, update the cost for
// all the pixels between 'start' and 'end' excluded.
static WEBP_INLINE void UpdateCostPerInterval(CostManager* const manager,
                                              int start, int end, int position,
                                              int64_t cost) {
  int i;
  for (i = start; i < end; ++i) UpdateCost(manager, i, position, cost);
}

// Given two intervals, make 'prev' be the previous one of 'next' in 'manager'.
static WEBP_INLINE void ConnectIntervals(CostManager* const manager,
                                         CostInterval* const prev,
                                         CostInterval* const next) {
  if (prev != NULL) {
    prev->next = next;
  } else {
    manager->head = next;
  }

  if (next != NULL) next->previous = prev;
}

// Pop an interval in the manager.
static WEBP_INLINE void PopInterval(CostManager* const manager,
                                    CostInterval* const interval) {
  if (interval == NULL) return;

  ConnectIntervals(manager, interval->previous, interval->next);
  if (CostIntervalIsInFreeList(manager, interval)) {
    CostIntervalAddToFreeList(manager, interval);
  } else {  // recycle regularly malloc'd intervals too
    interval->next = manager->recycled_intervals;
    manager->recycled_intervals = interval;
  }
  --manager->count;
  assert(manager->count >= 0);
}

// Update the cost at index i by going over all the stored intervals that
// overlap with i.
// If 'do_clean_intervals' is set to something different than 0, intervals that
// end before 'i' will be popped.
static WEBP_INLINE void UpdateCostAtIndex(CostManager* const manager, int i,
                                          int do_clean_intervals) {
  CostInterval* current = manager->head;

  while (current != NULL && current->start <= i) {
    CostInterval* const next = current->next;
    if (current->end <= i) {
      if (do_clean_intervals) {
        // We have an outdated interval, remove it.
        PopInterval(manager, current);
      }
    } else {
      UpdateCost(manager, i, current->index, current->cost);
    }
    current = next;
  }
}

// Given a current orphan interval and its previous interval, before
// it was orphaned (which can be NULL), set it at the right place in the list
// of intervals using the 'start' ordering and the previous interval as a hint.
static WEBP_INLINE void PositionOrphanInterval(CostManager* const manager,
                                               CostInterval* const current,
                                               CostInterval* previous) {
  assert(current != NULL);

  if (previous == NULL) previous = manager->head;
  while (previous != NULL && current->start < previous->start) {
    previous = previous->previous;
  }
  while (previous != NULL && previous->next != NULL &&
         previous->next->start < current->start) {
    previous = previous->next;
  }

  if (previous != NULL) {
    ConnectIntervals(manager, current, previous->next);
  } else {
    ConnectIntervals(manager, current, manager->head);
  }
  ConnectIntervals(manager, previous, current);
}

// Insert an interval in the list contained in the manager by starting at
// 'interval_in' as a hint. The intervals are sorted by 'start' value.
static WEBP_INLINE void InsertInterval(CostManager* const manager,
                                       CostInterval* const interval_in,
                                       int64_t cost, int position, int start,
                                       int end) {
  CostInterval* interval_new;

  if (start >= end) return;
  if (manager->count >= COST_CACHE_INTERVAL_SIZE_MAX) {
    // Serialize the interval if we cannot store it.
    UpdateCostPerInterval(manager, start, end, position, cost);
    return;
  }
  if (manager->free_intervals != NULL) {
    interval_new = manager->free_intervals;
    manager->free_intervals = interval_new->next;
  } else if (manager->recycled_intervals != NULL) {
    interval_new = manager->recycled_intervals;
    manager->recycled_intervals = interval_new->next;
  } else {  // malloc for good
    interval_new = (CostInterval*)WebPSafeMalloc(1, sizeof(*interval_new));
    if (interval_new == NULL) {
      // Write down the interval if we cannot create it.
      UpdateCostPerInterval(manager, start, end, position, cost);
      return;
    }
  }

  interval_new->cost = cost;
  interval_new->index = position;
  interval_new->start = start;
  interval_new->end = end;
  PositionOrphanInterval(manager, interval_new, interval_in);

  ++manager->count;
}

// Given a new cost interval defined by its start at position, its length value
// and distance_cost, add its contributions to the previous intervals and costs.
// If handling the interval or one of its subintervals becomes to heavy, its
// contribution is added to the costs right away.
static WEBP_INLINE void PushInterval(CostManager* const manager,
                                     int64_t distance_cost, int position,
                                     int len) {
  size_t i;
  CostInterval* interval = manager->head;
  CostInterval* interval_next;
  const CostCacheInterval* const cost_cache_intervals =
      manager->cache_intervals;
  // If the interval is small enough, no need to deal with the heavy
  // interval logic, just serialize it right away. This constant is empirical.
  const int kSkipDistance = 10;

  if (len < kSkipDistance) {
    int j;
    for (j = position; j < position + len; ++j) {
      const int k = j - position;
      int64_t cost_tmp;
      assert(k >= 0 && k < MAX_LENGTH);
      cost_tmp = distance_cost + manager->cost_cache[k];

      if (manager->costs[j] > cost_tmp) {
        manager->costs[j] = cost_tmp;
        manager->dist_array[j] = k + 1;
      }
    }
    return;
  }

  for (i = 0;
       i < manager->cache_intervals_size && cost_cache_intervals[i].start < len;
       ++i) {
    // Define the intersection of the ith interval with the new one.
    int start = position + cost_cache_intervals[i].start;
    const int end =
        position +
        (cost_cache_intervals[i].end > len ? len : cost_cache_intervals[i].end);
    const int64_t cost = distance_cost + cost_cache_intervals[i].cost;

    for (; interval != NULL && interval->start < end;
         interval = interval_next) {
      interval_next = interval->next;

      // Make sure we have some overlap
      if (start >= interval->end) continue;

      if (cost >= interval->cost) {
        // When intervals are represented, the lower, the better.
        // [**********************************************************[
        // start                                                    end
        //                   [----------------------------------[
        //                   interval->start        interval->end
        // If we are worse than what we already have, add whatever we have so
        // far up to interval.
        const int start_new = interval->end;
        InsertInterval(manager, interval, cost, position, start,
                       interval->start);
        start = start_new;
        if (start >= end) break;
        continue;
      }

      if (start <= interval->start) {
        if (interval->end <= end) {
          //                   [----------------------------------[
          //                   interval->start        interval->end
          // [**************************************************************[
          // start                                                        end
          // We can safely remove the old interval as it is fully included.
          PopInterval(manager, interval);
        } else {
          //              [------------------------------------[
          //              interval->start          interval->end
          // [*****************************[
          // start                       end
          interval->start = end;
          break;
        }
      } else {
        if (end < interval->end) {
          // [--------------------------------------------------------------[
          // interval->start                                    interval->end
          //                     [*****************************[
          //                     start                       end
          // We have to split the old interval as it fully contains the new one.
          const int end_original = interval->end;
          interval->end = start;
          InsertInterval(manager, interval, interval->cost, interval->index,
                         end, end_original);
          interval = interval->next;
          break;
        } else {
          // [------------------------------------[
          // interval->start          interval->end
          //                     [*****************************[
          //                     start                       end
          interval->end = start;
        }
      }
    }
    // Insert the remaining interval from start to end.
    InsertInterval(manager, interval, cost, position, start, end);
  }
}

static int BackwardReferencesHashChainDistanceOnly(
    int xsize, int ysize, const uint32_t* const argb, int cache_bits,
    const VP8LHashChain* const hash_chain, const VP8LBackwardRefs* const refs,
    uint16_t* const dist_array) {
  int i;
  int ok = 0;
  int cc_init = 0;
  const int pix_count = xsize * ysize;
  const int use_color_cache = (cache_bits > 0);
  const size_t literal_array_size =
      sizeof(*((CostModel*)NULL)->literal) * VP8LHistogramNumCodes(cache_bits);
  const size_t cost_model_size = sizeof(CostModel) + literal_array_size;
  CostModel* const cost_model =
      (CostModel*)WebPSafeCalloc(1ULL, cost_model_size);
  VP8LColorCache hashers;
  CostManager* cost_manager =
      (CostManager*)WebPSafeCalloc(1ULL, sizeof(*cost_manager));
  int offset_prev = -1, len_prev = -1;
  int64_t offset_cost = -1;
  int first_offset_is_constant = -1;  // initialized with 'impossible' value
  int reach = 0;

  if (cost_model == NULL || cost_manager == NULL) goto Error;

  cost_model->literal = (uint32_t*)(cost_model + 1);
  if (use_color_cache) {
    cc_init = VP8LColorCacheInit(&hashers, cache_bits);
    if (!cc_init) goto Error;
  }

  if (!CostModelBuild(cost_model, xsize, cache_bits, refs)) {
    goto Error;
  }

  if (!CostManagerInit(cost_manager, dist_array, pix_count, cost_model)) {
    goto Error;
  }

  // We loop one pixel at a time, but store all currently best points to
  // non-processed locations from this point.
  dist_array[0] = 0;
  // Add first pixel as literal.
  AddSingleLiteralWithCostModel(argb, &hashers, cost_model, /*idx=*/0,
                                use_color_cache, /*prev_cost=*/0,
                                cost_manager->costs, dist_array);

  for (i = 1; i < pix_count; ++i) {
    const int64_t prev_cost = cost_manager->costs[i - 1];
    int offset, len;
    VP8LHashChainFindCopy(hash_chain, i, &offset, &len);

    // Try adding the pixel as a literal.
    AddSingleLiteralWithCostModel(argb, &hashers, cost_model, i,
                                  use_color_cache, prev_cost,
                                  cost_manager->costs, dist_array);

    // If we are dealing with a non-literal.
    if (len >= 2) {
      if (offset != offset_prev) {
        const int code = VP8LDistanceToPlaneCode(xsize, offset);
        offset_cost = GetDistanceCost(cost_model, code);
        first_offset_is_constant = 1;
        PushInterval(cost_manager, prev_cost + offset_cost, i, len);
      } else {
        assert(offset_cost >= 0);
        assert(len_prev >= 0);
        assert(first_offset_is_constant == 0 || first_offset_is_constant == 1);
        // Instead of considering all contributions from a pixel i by calling:
        //         PushInterval(cost_manager, prev_cost + offset_cost, i, len);
        // we optimize these contributions in case offset_cost stays the same
        // for consecutive pixels. This describes a set of pixels similar to a
        // previous set (e.g. constant color regions).
        if (first_offset_is_constant) {
          reach = i - 1 + len_prev - 1;
          first_offset_is_constant = 0;
        }

        if (i + len - 1 > reach) {
          // We can only be go further with the same offset if the previous
          // length was maxed, hence len_prev == len == MAX_LENGTH.
          // TODO(vrabaud), bump i to the end right away (insert cache and
          // update cost).
          // TODO(vrabaud), check if one of the points in between does not have
          // a lower cost.
          // Already consider the pixel at "reach" to add intervals that are
          // better than whatever we add.
          int offset_j, len_j = 0;
          int j;
          assert(len == MAX_LENGTH || len == pix_count - i);
          // Figure out the last consecutive pixel within [i, reach + 1] with
          // the same offset.
          for (j = i; j <= reach; ++j) {
            VP8LHashChainFindCopy(hash_chain, j + 1, &offset_j, &len_j);
            if (offset_j != offset) {
              VP8LHashChainFindCopy(hash_chain, j, &offset_j, &len_j);
              break;
            }
          }
          // Update the cost at j - 1 and j.
          UpdateCostAtIndex(cost_manager, j - 1, 0);
          UpdateCostAtIndex(cost_manager, j, 0);

          PushInterval(cost_manager, cost_manager->costs[j - 1] + offset_cost,
                       j, len_j);
          reach = j + len_j - 1;
        }
      }
    }

    UpdateCostAtIndex(cost_manager, i, 1);
    offset_prev = offset;
    len_prev = len;
  }

  ok = !refs->error;
Error:
  if (cc_init) VP8LColorCacheClear(&hashers);
  CostManagerClear(cost_manager);
  WebPSafeFree(cost_model);
  WebPSafeFree(cost_manager);
  return ok;
}

// We pack the path at the end of *dist_array and return
// a pointer to this part of the array. Example:
// dist_array = [1x2xx3x2] => packed [1x2x1232], chosen_path = [1232]
static void TraceBackwards(uint16_t* const dist_array, int dist_array_size,
                           uint16_t** const chosen_path,
                           int* const chosen_path_size) {
  uint16_t* path = dist_array + dist_array_size;
  uint16_t* cur = dist_array + dist_array_size - 1;
  while (cur >= dist_array) {
    const int k = *cur;
    --path;
    *path = k;
    cur -= k;
  }
  *chosen_path = path;
  *chosen_path_size = (int)(dist_array + dist_array_size - path);
}

static int BackwardReferencesHashChainFollowChosenPath(
    const uint32_t* const argb, int cache_bits,
    const uint16_t* const chosen_path, int chosen_path_size,
    const VP8LHashChain* const hash_chain, VP8LBackwardRefs* const refs) {
  const int use_color_cache = (cache_bits > 0);
  int ix;
  int i = 0;
  int ok = 0;
  int cc_init = 0;
  VP8LColorCache hashers;

  if (use_color_cache) {
    cc_init = VP8LColorCacheInit(&hashers, cache_bits);
    if (!cc_init) goto Error;
  }

  VP8LClearBackwardRefs(refs);
  for (ix = 0; ix < chosen_path_size; ++ix) {
    const int len = chosen_path[ix];
    if (len != 1) {
      int k;
      const int offset = VP8LHashChainFindOffset(hash_chain, i);
      VP8LBackwardRefsCursorAdd(refs, PixOrCopyCreateCopy(offset, len));
      if (use_color_cache) {
        for (k = 0; k < len; ++k) {
          VP8LColorCacheInsert(&hashers, argb[i + k]);
        }
      }
      i += len;
    } else {
      PixOrCopy v;
      const int idx =
          use_color_cache ? VP8LColorCacheContains(&hashers, argb[i]) : -1;
      if (idx >= 0) {
        // use_color_cache is true and hashers contains argb[i]
        // push pixel as a color cache index
        v = PixOrCopyCreateCacheIdx(idx);
      } else {
        if (use_color_cache) VP8LColorCacheInsert(&hashers, argb[i]);
        v = PixOrCopyCreateLiteral(argb[i]);
      }
      VP8LBackwardRefsCursorAdd(refs, v);
      ++i;
    }
  }
  ok = !refs->error;
Error:
  if (cc_init) VP8LColorCacheClear(&hashers);
  return ok;
}

// Returns 1 on success.
extern int VP8LBackwardReferencesTraceBackwards(
    int xsize, int ysize, const uint32_t* const argb, int cache_bits,
    const VP8LHashChain* const hash_chain,
    const VP8LBackwardRefs* const refs_src, VP8LBackwardRefs* const refs_dst);
int VP8LBackwardReferencesTraceBackwards(int xsize, int ysize,
                                         const uint32_t* const argb,
                                         int cache_bits,
                                         const VP8LHashChain* const hash_chain,
                                         const VP8LBackwardRefs* const refs_src,
                                         VP8LBackwardRefs* const refs_dst) {
  int ok = 0;
  const int dist_array_size = xsize * ysize;
  uint16_t* chosen_path = NULL;
  int chosen_path_size = 0;
  uint16_t* dist_array =
      (uint16_t*)WebPSafeMalloc(dist_array_size, sizeof(*dist_array));

  if (dist_array == NULL) goto Error;

  if (!BackwardReferencesHashChainDistanceOnly(
          xsize, ysize, argb, cache_bits, hash_chain, refs_src, dist_array)) {
    goto Error;
  }
  TraceBackwards(dist_array, dist_array_size, &chosen_path, &chosen_path_size);
  if (!BackwardReferencesHashChainFollowChosenPath(
          argb, cache_bits, chosen_path, chosen_path_size, hash_chain,
          refs_dst)) {
    goto Error;
  }
  ok = 1;
Error:
  WebPSafeFree(dist_array);
  return ok;
}
