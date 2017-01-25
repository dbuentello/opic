/* op_heap.c --- 
 * 
 * Filename: op_heap.c
 * Description: 
 * Author: Felix Chern
 * Maintainer: 
 * Copyright: (c) 2016 Felix Chern
 * Created: Sat Oct 22, 2016
 * Version: 
 * Package-Requires: ()
 * Last-Updated: 
 *           By: 
 *     Update #: 0
 * URL: 
 * Doc URL: 
 * Keywords: 
 * Compatibility: 
 * 
 */

/* Commentary: 
 * 
 * 
 * 
 */

/* Change Log:
 * 
 * 
 */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Code: */

#include "op_heap.h"
#include "op_heap_internal.h"
#include "span.h"
#include "huge_page.h"
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>


typedef struct RawType RawType;
typedef struct TypeAlias TypeAlias;

static __thread int thread_id = -1;
static atomic_uint round_robin = 0;

OP_LOGGER_FACTORY(logger, "opic.malloc.op_heap");


struct OPHeap
{
  // TODO: add a magic in the very front so UNIX
  // can recognize us?
  // TODO: references to root objects
  // TODO: first hpage
  // TODO: rwlock for access (user can only set rlock)
  atomic_uint_fast64_t occupy_bmap[VSPAN_BMAP_NUM];
  atomic_uint_fast64_t header_bmap[VSPAN_BMAP_NUM];
  struct RawType raw_type;
  struct TypeAlias type_alias[2048];
  HugePage first_hpage;
};

int OPHeapCreate(OPHeap** heap_ref)
{
  ptrdiff_t offset = OPHEAP_SIZE;
  void* addr = NULL + offset;
  void* map_addr = MAP_FAILED;
  OPHeap* self;
  for (int i = 0; i < (1<<15); i++)
    {
      OP_LOG_INFO(logger,
                  "Attempt to create OPHeap on %p",
                  addr);
      map_addr = mmap(addr, OPHEAP_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_ANON | MAP_PRIVATE | MAP_FIXED,
                      -1, 0);
      if (map_addr != MAP_FAILED)
        {
          OP_LOG_INFO(logger,
                      "Successfully created OPHeap on %p",
                      map_addr);
          *heap_ref = map_addr;
          self = *heap_ref;
          goto init_opheap;
        }
      else
        addr += OPHEAP_SIZE;
    }
  OP_LOG_ERROR(logger,
               "Cannot allocate OPHeap, reached address limit");
  return -1;

 init_opheap:
  memset(self, 0, sizeof(OPHeap));
  // TODO assign first hugepage to raw
  atomic_store_explicit(&self->occupy_bmap[0], 1, memory_order_relaxed);
  atomic_store_explicit(&self->header_bmap[0], 1, memory_order_relaxed);
  return 0;
}

void* OPAllocRaw(OPHeap* self, size_t size)
{
  if (thread_id == -1)
    thread_id = atomic_fetch_add_explicit
      (&round_robin, 1, memory_order_acquire) % 16;

  int tid = thread_id;
  void* addr = NULL;

  if (size <= 256)
    {
      int size_class = (int)(size >> 4);
      size_class += (int)size - (size_class << 4) == 0 ? 0 : 1;

      UnarySpan* uspan = NULL;

      a_int16_t* pcard = self->raw_type.uspan_card[size_class][tid];
      while (1)
        {
          if (!atomic_check_in(pcard))
            continue;
          uspan = self->raw_type.uspan[size_class][tid];
          if (uspan == NULL)
            {
              if (!atomic_book_critical(pcard))
                {
                  atomic_check_out(pcard);
                  continue;
                }
              atomic_enter_critical(pcard);
              Magic magic =
                {
                  .raw_uspan =
                  {
                    .pattern = RAW_USPAN_PATTERN,
                    .obj_size = size_class,
                    .thread_id = tid
                  }
                };
              while (1)
                {
                  // TODO: would this be a dead lock?
                  a_int16_t* hpage_pcard = self->raw_type.hpage_pcard;
                  if (!atomic_check_in(hpage_pcard))
                    continue;
                  HugePage* hpage = self->raw_type.hpage;
                  if (hpage == NULL)
                    {
                      if(!atomic_book_critical(hpage_pcard))
                        {
                          atomic_check_out(hpage_pcard);
                          continue;
                        }
                      atomic_enter_critical(hpage_pcard);
                      Magic magic =
                        {.raw_hpage = {.pattern = RAW_HPAGE_PATTERN}};
                      hpage = ObtainHPage(self, magic);
                      enqueue_hpage(&self->raw_type.hpage, hpage);
                      atomic_exit_critical(hpage_pcard);
                    }
                  // TODO: We can optimize for different object sizes.
                  // Larger object can use larger span.
                  // Or the size that get allocated frequently can use
                  // larger span.
                  uspan = ObtainUSpan(hpage, magic, 1);
                  atomic_check_out(hpage_pcard);
                  if (uspan)
                    break;
                }
              enqueue_uspan(&self->raw_type.uspan[size_class][tid],
                            uspan);
              atomic_exit_critical(pcard);
              break;
            }
          if (!USpanMalloc(uspan, &addr))
            {
              atomic_check_out(pcard);
              continue;
            }
          atomic_check_out(pcard);
          return addr;
        }
    }
  else if (size <= 2048)
    {
      int size_class;
      int favor;
      int large_uspan_id;
      if (size <= 512)
        {
          large_uspan_id = 0;
          size_class = 512;
          favor = FAVOR_512;
        }
      else if (size <= 1024)
        {
          large_uspan_id = 1;
          size_class = 1024;
          favor = FAVOR_1024;
        }
      else
        {
          large_uspan_id = 2;
          size_class = 2048;
          favor = FAVOR_2048;
        }

      UnarySpan* uspan = NULL;

      while (1)
        {
          acq_rlock(&self->raw_type.large_uspan_rwlock[large_uspan_id],
                    &self->raw_type.remain_favor,
                    favor);
          uspan = self->raw_type.large_uspan[large_uspan_id];
          if (uspan == NULL)
            {
              favor_write(&self->raw_type.remain_favor,
                          favor);
              rel_rlock(&self->raw_type.large_uspan_rwlock[large_uspan_id]);
              insert_new_large_uspan(&self->raw_type,
                                     large_uspan_id,
                                     size_class,
                                     favor,
                                     self);
              continue;
            }
          addr = USpanMalloc(uspan);
          if (addr == NULL)
            {
              favor_write(&self->raw_type.remain_favor,
                          favor);
              rel_rlock(&self->raw_type.large_uspan_rwlock[large_uspan_id]);
              remove_large_uspan
                (&self->raw_type.large_uspan[large_uspan_id],
                 &self->raw_type.large_uspan_rwlock[large_uspan_id],
                 &self->raw_type.remain_favor,
                 favor,
                 uspan);
              continue;
            }
          break;
        }
      rel_rlock(&self->raw_type.large_uspan_rwlock[large_uspan_id]);
      return addr;
    }
  else
    {
      op_assert(0, "not implemented yet\n");
    }

  return NULL;
}

void OPFree2(void* addr)
{
  OPHeap* self = (OPHeap*)(((uintptr_t)addr) & OPHEAP_SIZE);
  ptrdiff_t diff = (uintptr_t)addr - (uintptr_t)self;

  int hpage_idx = diff >> 21;

  if (hpage_idx == 0)
    {
      // TODO: special logic to handle first hpage
      return;
    }

  // TODO the logic here is terrible, need to rewrite.
  /* uint64_t bmap = atomic_load_explicit(&self->header_bmap[hpage_idx >> 6], */
  /*                                      memory_order_relaxed); */
  /* uint64_t clear_low = bitmap & ~((1UL << hpage_idx % 64)-1); */
  /* int hpage_header_idx = ((hpage_idx >> 6) << 6) + __builtin_ctzl(clear_low); */
  /* span = base + (span_header_idx << 12); */
  /* header_mask = ~(clear_low & (~(clear_low) + 1)); */
}


HugePage* ObtainHPage(OPHeap* self, Magic magic)
{
  uint64_t old_bmap, new_bmap;
  ptrdiff_t item_offset;

  for (int i = 0; i < VSPAN_BMAP_NUM;)
    {
      old_bmap = atomic_load_explicit(&self->occupy_bmap[i],
                                      memory_order_relaxed);
      do
        {
          if (old_bmap == ~0UL) goto next_bmap;
          new_bmap = (old_bmap + 1);
          item_offset = __builtin_ctzl(new_bmap);
          new_bmap |= old_bmap;
        } while (!atomic_compare_exchange_weak_explicit
                 (&self->occupy_bmap[i], &old_bmap, new_bmap,
                  memory_order_acquire,
                  memory_order_relaxed));

      HugePage* hpage = HugePageInit((void*)self + ((i << 6) + item_offset), magic);
      atomic_fetch_or_explicit(&self->header_bmap[i], 1UL << item_offset,
                               memory_order_release);
      return hpage;

    next_bmap:
      i++;
    }
  op_assert(0, "Running out of hpage\n");
}

void enqueue_uspan(UnarySpan* uspan)
{
  OPHeap* heap = (OPHeap*)(((uintptr_t)uspan) & OPHEAP_SIZE);
  Magic magic = uspan->magic;

  switch (magic.generic.pattern)
    {
    case TYPED_USPAN_PATTERN:
      {
        TypeAlias* ta = &heap->type_alias[magic.typed_uspan.type_alias];
        int tid = magic.typed_uspan.thread_id;
        do
          {
            favor_write(&ta->uspan_favor, tid);
          } while (!acq_wlock(&ta->uspan_rwlock[tid],
                              &ta->uspan_favor,
                              tid));
        insert_uspan_internal(&ta->uspan[tid],
                              uspan);
        rel_wlock(&ta->uspan_rwlock[tid]);
      }
      break;
    case RAW_USPAN_PATTERN:
      {
        uint16_t size_class = magic.raw_uspan.obj_size >> 4;
        int tid = magic.raw_uspan.thread_id;
        // this doesn't work, the callee is holding rlock
        do
          {
            favor_write(&heap->raw_type.uspan_favor[tid],
                        tid);
          } while (!acq_wlock(&heap->raw_type.uspan_rwlock[size_class][tid],
                              &heap->raw_type.uspan_favor[size_class],
                              tid));
        insert_uspan_internal(&heap->raw_type.uspan[size_class][tid],
                              uspan);
        rel_wlock(&heap->raw_type.uspan_rwlock[size_class][tid]);
      }
      break;
    case LARGE_USPAN_PATTERN:
      {
        uint16_t obj_size = magic.large_uspan.obj_size;
        int large_uspan_id =
          obj_size == 512 ? 0 :
          obj_size == 1024 ? 1 : 2;
        int favor =
          obj_size == 512 ? FAVOR_512 :
          obj_size == 1024 ? FAVOR_1024 : FAVOR_2048;
        do
          {
            favor_write(&heap->raw_type.remain_favor,
                        favor);
          } while (!acq_wlock
                   (&heap->raw_type.large_uspan_rwlock[large_uspan_id],
                    &heap->raw_type.remain_favor,
                    favor));
        insert_uspan_internal(&heap->raw_type.large_uspan[large_uspan_id],
                              uspan);
        rel_wlock(&heap->raw_type.large_uspan_rwlock[large_uspan_id]);
      }
      break;
    default:
      op_assert(0, "Unexpected uspan\n");
    }
}

void dequeue_uspan(UnarySpan* uspan)
{
  OPHeap* heap = (OPHeap*)(((uintptr_t)uspan) & OPHEAP_SIZE);
  Magic magic = uspan->magic;

  switch (magic.generic.pattern)
    {
    case TYPED_USPAN_PATTERN:
      {
        // TODO decrement refcnt of spans in typed uspan
        TypeAlias* ta = &heap->type_alias[magic.typed_uspan.type_alias];
        int tid = magic.typed_uspan.thread_id;
        do
          {
            favor_write(&ta->uspan_favor, tid);
          } while (!acq_wlock(&ta->uspan_rwlock[tid],
                              &ta->uspan_favor,
                              tid));
        UnarySpan** it = &ta->uspan[tid];
        while (*it != uspan)
          it = &(*it)->next;
        *it = (*it)->next;
        rel_wlock(&ta->uspan_rwlock[tid]);
      }
      break;
    case RAW_USPAN_PATTERN:
      {
        uint16_t size_class = magic.raw_uspan.obj_size >> 4;
        int tid = magic.raw_uspan.thread_id;
        do
          {
            favor_write(&heap->raw_type.uspan_favor[tid],
                        tid);
          } while (!acq_wlock(&heap->raw_type.uspan_rwlock[size_class][tid],
                              &heap->raw_type.uspan_favor[size_class],
                              tid));
        UnarySpan** it = &heap->raw_type.uspan[size_class][tid];
        while (*it != uspan)
          it = &(*it)->next;
        *it = (*it)->next;
        rel_wlock(&heap->raw_type.uspan_rwlock[size_class][tid]);
      }
      break;
    case LARGE_USPAN_PATTERN:
      {
        uint16_t obj_size = magic.large_uspan.obj_size;
        int large_uspan_id =
          obj_size == 512 ? 0 :
          obj_size == 1024 ? 1 : 2;
        int favor =
          obj_size == 512 ? FAVOR_512 :
          obj_size == 1024 ? FAVOR_1024 : FAVOR_2048;
        do
          {
            favor_write(&heap->raw_type.remain_favor,
                        favor);
          } while (!acq_wlock
                   (&heap->raw_type.large_uspan_rwlock[large_uspan_id],
                    &heap->raw_type.remain_favor,
                    favor));
        UnarySpan** it = &heap->raw_type.large_uspan[large_uspan_id];
        while (*it != uspan)
          it = &(*it)->next;
        *it = (*it)->next;
        rel_wlock(&heap->raw_type.large_uspan_rwlock[large_uspan_id]);
      }
      break;
    default:
      op_assert(0, "Unexpected uspan\n");
    }
}

/* op_heap.c ends here */
