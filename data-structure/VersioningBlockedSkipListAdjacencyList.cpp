//
// Created by per on 23.12.20.
//

#include "VersioningBlockedSkipListAdjacencyList.h"

//
// Created by per on 28.09.20.
//

#include <cstring>
#include <map>
#include <set>
#include <chrono>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <sched.h>
#include <cassert>
#include <functional>
#include "internal-driver/Configuration.h"
#include "SizeVersionChainEntry.h"
#include "VersionedBlockedPropertyEdgeIterator.h"
#include "VersionedBlockedEdgeIterator.h"
#include "EdgeBlock.h"
#include "EdgeVersionRecord.h"

#define MIN_BLOCK_SIZE 2u

#define CACHELINE_SIZE 64
#define PAGE_SIZE 4096

#define COLLECT_VERSIONS_ON_INSERT 1

#define ASSERT_CONSISTENCY  1
#define ASSERT_SIZE 1
#define ASSERT_WEIGHTS 0

#define PER_BLOCK 512 

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

// TODO not rewritten to handle versions.
#define intersect(start_a, end_a, start_b, end_b, out) while (start_a < end_a && start_b < end_b) { \
  const dst_t a = *start_a;                                                                        \
  const dst_t b = *start_b; \
  if (a == b) {\
    *out_iterator = *start_a;\
    start_a++;\
    start_b++;\
  } else if (a < b) {\
    start_a++;\
  } else {\
    start_b++;\
  }\
}

//thread_local mt19937 VersioningBlockedSkipListAdjacencyList::level_generator = mt19937((uint) time(NULL));
thread_local mt19937 VersioningBlockedSkipListAdjacencyList::level_generator = mt19937((uint) 42);

thread_local int VersioningBlockedSkipListAdjacencyList::gced_edges = 0;
thread_local int VersioningBlockedSkipListAdjacencyList::gc_merges = 0;
thread_local int VersioningBlockedSkipListAdjacencyList::gc_to_single_block = 0;

thread_local FreeList VersioningBlockedSkipListAdjacencyList::local_free_list(PER_BLOCK);



bool check[] = {false, false, false, false, false, false, false, false, false};

void VersioningBlockedSkipListAdjacencyList::bulkload(const SortedCSRDataSource &src) {
  throw NotImplemented();
}


//Dont know where is used
void *VersioningBlockedSkipListAdjacencyList::write_to_blocks(const dst_t *start, const dst_t *end) {
  // uint size = end - start;
  // if (size == 0) {
  //   return nullptr;
  // } else if (size <= block_size) {
  //   size_t block_size = max(MIN_BLOCK_SIZE, round_up_power_of_two(size));
  //   dst_t *block = (dst_t *) malloc((block_size) * sizeof(dst_t));
  //   memcpy((void *) block, (void *) start, size * sizeof(dst_t));
  //   return (void *) ((uint64_t) block | EDGE_SET_TYPE_MASK);
  // } else {
  //   VSkipListHeader *first_block = nullptr;
  //   VSkipListHeader *last_block = nullptr;

  //   size_t block_fill = block_size * bulk_load_fill_rate;

  //   while (start < end) {
  //     VSkipListHeader *block = (VSkipListHeader *) malloc(memory_block_size());

  //     if (first_block == nullptr) {
  //       first_block = block;
  //       block->before = nullptr;
  //     } else {
  //       last_block->next_levels[0]->add_new_pointer(block, version, );
  //       block->before = last_block;
  //     }
  //     last_block = block;

  //     block->data = get_data_pointer(block);
  //     block->size = block_fill < (uint) (end - start) ? block_fill : end - start;

  //     dst_t *block_data = get_data_pointer(block);
  //     memcpy((void *) block_data, (void *) start, block->size * sizeof(dst_t));

  //     block->max = block_data[block->size - 1];

  //     start += block->size;
  //   }
  //   last_block->next_levels[0] = nullptr;

  //   auto i = first_block;
  //   vector<VSkipListHeader *> level_blocks(SKIP_LIST_LEVELS, first_block);
  //   while (i != nullptr) {
  //     auto height = get_height();
  //     for (uint l = 1; l < SKIP_LIST_LEVELS; l++) {
  //       i->next_levels[l] = nullptr;
  //       if (i != first_block && l < height) {
  //         level_blocks[l]->next_levels[l] = i;
  //         level_blocks[l] = i;
  //       }
  //     }
  //     i = i->next_levels[0];
  //   }

  //   auto b = first_block;
  //   uint a_size = 0;
  //   while (b != nullptr) {
  //     for (auto i = 0; i < b->size; i++) {
  //       a_size++;
  //     }
  //     b = b->next_levels[0];
  //   }
  //   assert(a_size == size);


  //   return first_block;
  // }
}

dst_t *VersioningBlockedSkipListAdjacencyList::get_data_pointer(VSkipListHeader *header) const {
  return (dst_t *) ((char *) header + skip_list_header_size());
}

void VersioningBlockedSkipListAdjacencyList::validate_change(version_t version, vertex_id_t v, int tp, size_t before){
  long long simple_size = neighbourhood_size_version_p(v, version);
  long long other_size = 0;
  SnapshotTransaction tx = tm.getSnapshotTransaction(this, false, false);
  SORTLEDTON_ITERATE(tx, v, {
    other_size++;
  });

  if(other_size!=simple_size){
    cout<<"size before: "<<before<<endl;
    cout<<"validation failed: "<<simple_size<<" "<<other_size<<" in: "<<tp<<endl<<endl<<endl;;
    edge_t e(v, 100);
    get_weight_version_p(e,version, nullptr);
    cout<<endl;
    SORTLEDTON_ITERATE(tx, v, {
        cout<<e<<" ";
      });
    cout<<endl<<endl;
    exit(0);
  }
}

bool VersioningBlockedSkipListAdjacencyList::insert_edge_version(edge_t edge, version_t version, char *properties, size_t properties_size, bool debug) {
  assert((properties != nullptr && properties_size != 0) || (properties == nullptr && properties_size == 0));
  assert(properties_size == property_size && "We allow only properties of the same size for all edges.");

  uint64_t start = __rdtsc();
  
  void *adjacency_list = raw_neighbourhood_version(edge.src, version);
  //TODO what is this doing?
  __builtin_prefetch((void *) ((uint64_t) adjacency_list & ~EDGE_SET_TYPE_MASK));
  // __builtin_prefetch((void *) ((uint64_t) ((dst_t *) adjacency_list + 1) & ~SIZE_VERSION_MASK));

#if defined(DEBUG) && ASSERT_SIZE
  size_t size = neighbourhood_size_version_p(edge.src, version);
#endif

  // Insert to empty list
  if (unlikely(adjacency_list == nullptr)) {
    insert_empty(edge, version, properties);
    return true;
  } else {
    switch (get_set_type(edge.src, version)) {
      case SINGLE_BLOCK: {
        size_t siz = neighbourhood_size_version_p(edge.src, version);
        insert_single_block(edge, version, properties, debug);
        uint64_t end = __rdtsc();
        update_write_time_p(edge.src, end - start, 1);
        
        return true;
      }
      case SKIP_LIST: {
        size_t siz = neighbourhood_size_version_p(edge.src, version);
        insert_skip_list(edge, version, properties);
        uint64_t end = __rdtsc();
        update_write_time_p(edge.src, end - start, 1);
        
        return true;
      }
      default: {
        throw NotImplemented();
      }
    }
  }

#if defined(DEBUG) && ASSERT_SIZE
  size_t size_after = neighbourhood_size_version_p(edge.src, version);
  assert(size + 1 == size_after);
#endif

}

bool VersioningBlockedSkipListAdjacencyList::insert_edge_version(edge_t edge, version_t version) {
  return insert_edge_version(edge, version, nullptr,0);
}

VSkipListHeader* VersioningBlockedSkipListAdjacencyList::get_latest_next_pointer(VSkipListHeader *pHeader, uint16_t level, version_t version){
  VSkipListHeader* ans; 

  ans = (VSkipListHeader*)pHeader->next_levels[level]->get_pointer(version);
  
  return ans;
}

/**
 * Finds the block that contains the upper bound of element.
 *
 * Returns blocks for all levels in the out parameter blocks.
 *
 * The block for level 0 is the block containing the upper bound while all other
 * blocks are lower bounds.
 *
 * @param pHeader
 * @param element
 * @param blocks vector with one entry for each level
 */
VSkipListHeader *
VersioningBlockedSkipListAdjacencyList::
find_block(VSkipListHeader *pHeader, dst_t element, version_t version, VSkipListHeader *blocks[SKIP_LIST_LEVELS]) {
  for (int l = SKIP_LIST_LEVELS - 1; 0 <= l; l--) {
    VSkipListHeader *next_level = get_latest_next_pointer(pHeader,l, version);
    while (next_level != nullptr && next_level->max < element &&
           get_latest_next_pointer(next_level,0,version) != nullptr) {
      // The last block is special case it can be the one to insert but is not a lower bound
      pHeader = next_level;
      next_level = get_latest_next_pointer(pHeader, l, version);
    }
    blocks[l] = pHeader;
  }
  VSkipListHeader *ans = get_latest_next_pointer(blocks[0],0,version);
  return ans != nullptr && blocks[0]->max < element ? ans : blocks[0];
}

/**
 * Finds the block which contains element if element is in the list.
 *
 * Does not keep track of the "path" of elements leading there as this is not necessary for intersections.
 *
 * @param pHeader
 * @param element
 * @return the block potentially containing element or nullptr if element is bigger than all elements in the list.
 */
VSkipListHeader *
VersioningBlockedSkipListAdjacencyList::find_block1(VSkipListHeader *pHeader, dst_t element, version_t version) {
  VSkipListHeader *blocks[SKIP_LIST_LEVELS];
  for(int i=0;i<SKIP_LIST_LEVELS;i++)
    blocks[i] = nullptr;
  return find_block(pHeader, element, version, blocks);
}

bool VersioningBlockedSkipListAdjacencyList::has_edge_version_p(edge_t edge, version_t version, bool debug) {
  dst_t *pos;
  dst_t *end;
  switch (get_set_type(edge.src, version)) {
    case SKIP_LIST: {
      VSkipListHeader *head = (VSkipListHeader *) raw_neighbourhood_version(edge.src, version);
      if (head != nullptr) {
        VSkipListHeader *blocks[SKIP_LIST_LEVELS];
        for(int i=0;i<SKIP_LIST_LEVELS;i++)
          blocks[i] = nullptr;
        auto block = find_block(head, edge.dst, version, blocks);
        auto block1 = find_block1(head,edge.dst, version);
        
        EdgeBlock eb = EdgeBlock::from_vskip_list_header(block, block_size, property_size);
        end = get_data_pointer(block) + eb.get_max_edge_index();
        // cout<<"max occupied index in "<< edge.src<<": "<<eb.get_max_edge_index()<<endl;
        pos = find_upper_bound(get_data_pointer(block), eb.get_max_edge_index(), edge.dst);

        if(debug){
          cout<<"looking in has_edge skip_list: "<<edge.src<< " "<<edge.dst<<endl;
          edge_t e(edge.src, edge.dst);
          get_weight_version_p(e, version, nullptr);

          cout<<"\n\nblock returned"<<endl;
          for(int i=0;i<block_size;i++)
            cout<< *(block->data+i)<<" ";
          cout<<endl;
          cout<<endl<<eb.get_max_edge_index()<<endl;
          cout<<*pos<<endl;
        }
      } else {
        return false;
      }
      break;
    }
    case SINGLE_BLOCK: {
      auto start = (dst_t *) raw_neighbourhood_version(edge.src, version);
      auto[capacity, size, curr_version] = adjacency_index.get_single_block_size(edge.src,version);
      
      EdgeBlock eb = EdgeBlock::from_single_block(start, capacity, size, property_size);
      end = start + eb.get_max_edge_index();
      // cout<<"max occupied index in "<< edge.src<<": "<<eb.get_max_edge_index()<<endl;
      pos = find_upper_bound(start, eb.get_max_edge_index(), edge.dst);
      if(debug){
        edge_t e(edge.src, edge.dst);
        cout<<"looking again for edge: "<<edge.src<<" "<<edge.dst<<endl;
        get_weight_version_p(e, version, nullptr);
      }
      break;
    }
  }
  if (pos == end) {
    // cout<<"reached end looking for "<<edge.dst<<endl;
    return false;
  } else if (!is_versioned(*pos)) {
    // cout<<edge.dst<<" "<<*pos<<endl;
    return *pos == edge.dst;
  }
}

/**
 * Finds the upper bound for value in a sorted array.
 *
 * Ignores versions.
 *
 * @param start
 * @param end
 * @param value
 * @return a pointer to the position of the upper bound or end.
 * @return a pointer to the position of the upper bound or end.
 */
dst_t *VersioningBlockedSkipListAdjacencyList::find_upper_bound(dst_t *start, uint16_t size, dst_t value, bool debug) {
  return EdgeBlock::find_upper_bound(start, size, value);
}

void VersioningBlockedSkipListAdjacencyList::intersect_neighbourhood_version_p(vertex_id_t a, vertex_id_t b,
                                                                               vector<dst_t> &out, version_t version) {
  throw NotImplemented();
//  auto s_a = neighbourhood_size_p(a);
//  auto s_b = neighbourhood_size_p(b);
//
//  out.clear();
//
//  if (s_a == 0 || s_b == 0) {
//    return;
//  }
//
//  if (s_b < s_a) {
//    swap(s_a, s_b);
//    swap(a, b);
//  }
//
//  if (get_set_type(a) == SINGLE_BLOCK && get_set_type(b) == SINGLE_BLOCK) {
//    call_single_single++;
//    auto out_iterator = back_inserter(out);
//    auto start_a = (dst_t *) raw_neighbourhood(a);
//    auto end_a = start_a + neighbourhood_size(a);
//    auto start_b = (dst_t *) raw_neighbourhood(b);
//    auto end_b = start_b + neighbourhood_size(b);
//
//    intersect(start_a, end_a, start_b, end_b, out_iterator)
//  } else if (get_set_type(a) == SINGLE_BLOCK) {
//    call_single++;
//    auto out_iterator = back_inserter(out);
//
//    auto start_a = (dst_t *) raw_neighbourhood(a);
//    auto end_a = start_a + neighbourhood_size(a);
//
//    SkipListHeader *ns_b = (SkipListHeader *) raw_neighbourhood(b);
//
//    if (32 * s_a < s_b) {
//      while (start_a < end_a) {
//        auto b_block = find_block1(ns_b, *start_a);
//        if (b_block == nullptr) {
//          return;
//        }
//
//        auto start_b = b_block->data;
//        auto end_b = start_b + b_block->size;
//
//        if (binary_search(start_b, end_b, *start_a)) {
//          *out_iterator = *start_a;
//        }
//        start_a++;
//      }
//    } else {
//      while (start_a < end_a && ns_b != nullptr) {
//        auto start_b = ns_b->data;
//        auto end_b = start_b + ns_b->size;
//
//        intersect(start_a, end_a, start_b, end_b, out_iterator)
//
//        ns_b = (SkipListHeader *) ns_b->next;
//      }
//    }
//  } else {
//    call_skip++;
//    auto out_iterator = back_inserter(out);
//
//    SkipListHeader *ns_a = (SkipListHeader *) raw_neighbourhood(a);
//    SkipListHeader *ns_b = (SkipListHeader *) raw_neighbourhood(b);
//
//    if (32 * s_a < s_b) {
//      while (ns_a != nullptr) {
//        auto start_a = ns_a->data;
//        auto end_a = ns_a->data + ns_a->size;
//
//        while (start_a < end_a) {
//          auto b_block = find_block1(ns_b, *start_a);
//          if (b_block == nullptr) {
//            return;
//          }
//          auto start_b = b_block->data;
//          auto end_b = start_b + b_block->size;
//
//          if (binary_search(start_b, end_b, *start_a)) {
//            *out_iterator = *start_a;
//          }
//          start_a++;
//        }
//
//        ns_a = (SkipListHeader *) ns_a->next;
//      }
//    } else {
//      auto start_a = ns_a->data;
//      auto end_a = ns_a->data + ns_a->size;
//      auto start_b = ns_b->data;
//      auto end_b = ns_b->data + ns_b->size;
//      while (ns_a != nullptr && ns_b != nullptr) {
//        intersect(start_a, end_a, start_b, end_b, out_iterator)
//
//        if (start_a == end_a) {
//          ns_a = (SkipListHeader *) ns_a->next;
//          if (ns_a != nullptr) {
//            start_a = ns_a->data;
//            end_a = ns_a->data + ns_a->size;
//          }
//        } else {
//          ns_b = (SkipListHeader *) ns_b->next;
//          if (ns_b != nullptr) {
//            start_b = ns_b->data;
//            end_b = ns_b->data + ns_b->size;
//          }
//        }
//      }
//    }
//  }

}


// TODO cleanup index usage
size_t VersioningBlockedSkipListAdjacencyList::neighbourhood_size_version_p(vertex_id_t src, version_t version) {
  switch (get_set_type(src, version)) {
    case VSKIP_LIST: {
      auto [size, curr_version]=adjacency_index.get_version_size(src,version);
      // cout<<"size of src: "<<size<<endl;
      return size;
    }
    case VSINGLE_BLOCK: {
      auto[block_capacity, size, curr_version] = adjacency_index.get_single_block_size(src,version);
      // If the size is not versioned, this means the correct size is stored in the index   
            // cout<<"size of src: "<<size<<endl;

      return size;
      
    }
    default: {
      throw NotImplemented();
    }
  }
}

//DONT_NEED THIS ONE
// bool VersioningBlockedSkipListAdjacencyList::size_is_versioned(vertex_id_t v) {
//   return adjacency_index.size_is_versioned(v);
// }

VersioningBlockedSkipListAdjacencyList::VersioningBlockedSkipListAdjacencyList(size_t block_size, size_t property_size,
                                                                               TransactionManager &tm)
        : tm(tm), block_size(block_size), property_size(property_size)
//        , pool(memory_block_size())
{
  cout << "Sortledton.3" << endl;
  if (round_up_power_of_two(block_size) != block_size) {
    throw ConfigurationError("Block size needs to be a power of two.");
  }
}

size_t VersioningBlockedSkipListAdjacencyList::vertex_count_version(version_t version) {
  return adjacency_index.get_vertex_count(version);
}

void *VersioningBlockedSkipListAdjacencyList::raw_neighbourhood_version(vertex_id_t src, version_t version) {
  return adjacency_index.neighbourhood_version(src, version);
}

size_t VersioningBlockedSkipListAdjacencyList::memory_block_size() {
  return get_single_block_memory_size(block_size) + skip_list_header_size();
}

size_t VersioningBlockedSkipListAdjacencyList::get_height() {
  uniform_real_distribution<double> d(0.0, 1.0);
  auto level = 1;
  while (d(level_generator) < p && level < SKIP_LIST_LEVELS) {
    level += 1;
  }
  return level;
}

// TODO remove function
size_t VersioningBlockedSkipListAdjacencyList::skip_list_header_size() const {
  return sizeof(VSkipListHeader) + SKIP_LIST_LEVELS*sizeof(AllInlineAccessPointers);
}

VAdjacencySetType VersioningBlockedSkipListAdjacencyList::get_set_type(vertex_id_t v, version_t version) {
  return adjacency_index.get_adjacency_set_type(v, version);  // TODO remove
}

void VersioningBlockedSkipListAdjacencyList::insert_empty(edge_t edge, version_t version, char *properties) {
  
 
  // cout<<"edge_list_size for "<<edge.src<<" before: "<<neighbourhood_size_version(edge.src,version)<<endl;
  adjacency_index.create_new_version(edge.src,version, tm.getMinActiveVersion());
  EdgeBlock eb = new_single_edge_block(MIN_BLOCK_SIZE);
  eb.insert_edge(edge.dst, 0, properties); //weight?
  //need to account for versions
  adjacency_index.store_single_block(edge.src, eb.get_single_block_pointer(), MIN_BLOCK_SIZE, 1, version);
#if defined(DEBUG) && ASSERT_CONSISTENCY
  assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
#endif
  // cout<<"edge_list_size for "<<edge.src<<" after: "<<neighbourhood_size_version(edge.src,version)<<endl;
     
}

void VersioningBlockedSkipListAdjacencyList::insert_single_block(edge_t edge, version_t version, char *properties, bool debug) {

  auto [block_capacity, size, curr_version] = adjacency_index.get_single_block_size(edge.src, version);
  auto eb = EdgeBlock::from_single_block((dst_t *) raw_neighbourhood_version(edge.src, version), block_capacity, size,
                                        property_size);

  adjacency_index.create_new_version(edge.src, version, tm.getMinActiveVersion());  


#if COLLECT_VERSIONS_ON_INSERT
  // if (is_versioned) {
  //   versions_remaining = eb.gc(tm.getMinActiveVersion(), tm.get_sorted_versions());
  // }
#endif
  // cout<<"inserting edge "<<edge.src<<" "<<edge.dst<<endl;
  version_t last_epoch = tm.getMinActiveVersion();

  if (eb.has_space_to_insert_edge()) {
    if(curr_version < version){
      // cout<<"new version being created :"<<edge.src<<endl;
      uint64_t start = __rdtsc();
      local_free_list.add_node(eb.get_single_block_pointer(), version, 0);
      EdgeBlock new_eb = new_single_edge_block(block_capacity);
      eb.copy_into(new_eb);
      eb = new_eb;
      uint64_t end = __rdtsc();
      update_copy_time_p(edge.src, end - start, 1);
    }
    if(debug) cout<<"had space to insert"<<endl;
      eb.insert_edge(edge.dst, 0, properties, debug); 
      adjacency_index.store_single_block(edge.src, eb.get_single_block_pointer(), eb.get_block_capacity(),
                                   eb.get_edges(),version, last_epoch);
#if defined(DEBUG) && ASSERT_CONSISTENCY
    assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
#endif
  } else {  // else resize block or add skip list
    if (block_capacity == block_size) {
      // Block should be split into 2 skip list blocks, we do this in two steps, convert to SkipListHeader and then by recursion split into two.
      VSkipListHeader *new_block = new_skip_list_block();
      auto new_eb = EdgeBlock::from_vskip_list_header(new_block, block_size, property_size);
      eb.copy_into(new_eb);
      new_eb.update_skip_list_header(new_block);

      // std::cout<<"creating the block for the first time: ";
      // for(int i=0;i<block_size;i++)
      // std::cout<<*(eb.start+i)<<" ";
      // std::cout<<std::endl;


      adjacency_index[edge.src].adjacency_set.sizes[0] = block_size; 
      // uint64_t start = __rdtsc();
      adjacency_index[edge.src].adjacency_set.add_new_pointer((void*) new_block, version, tm.getMinActiveVersion(),2);
      // uint64_t end = __rdtsc();
      new_block->before = nullptr;
      new_block->data = new_eb.get_single_block_pointer();
      new_block->height = SKIP_LIST_LEVELS;
      new_block->version = version;

      insert_skip_list(edge, version, properties);
     
      if(curr_version==version){
        free_block(eb.get_single_block_pointer(), get_single_block_memory_size(block_capacity));
        
      }
      else {
        local_free_list.add_node(eb.get_single_block_pointer(),version, 0);
        
      }
      
      // recursive call of depth 2, inefficient could be done with one time less copying.
      return ;
    } else { // Block full: we double size and copy.
      if(debug) cout<<"doubled size and then inserting"<<endl;
      auto new_eb = new_single_edge_block(block_capacity * 2);
      eb.copy_into(new_eb);
      new_eb.insert_edge(edge.dst, version, properties, debug);

      //versions to be taken care of
      adjacency_index.store_single_block(edge.src, new_eb.get_single_block_pointer(), new_eb.get_block_capacity(),
                                         new_eb.get_edges(),version, last_epoch);

      if(curr_version==version){
          free_block(eb.get_single_block_pointer(), 0);
          
      }
      
      else{ 
        local_free_list.add_node(eb.get_single_block_pointer(),version, 0);
        
      }
#if defined(DEBUG) && ASSERT_CONSISTENCY
      assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
#endif
    }
  }
     return;

}

void VersioningBlockedSkipListAdjacencyList::update_adjacency_size(vertex_id_t v, bool deletion, version_t version) {
  assert(get_set_type(v, version) == VSKIP_LIST);
  // TODO needs unit testing.
  auto s = adjacency_index.get_version_size(v,version);

  auto update = 1;
  if (deletion) {
    update = -1;
  }
  adjacency_index[v].adjacency_set.sizes[0]+=update;
}

void VersioningBlockedSkipListAdjacencyList::add_new_pointer(VSkipListHeader *pHeader, uint16_t level, VSkipListHeader *pointer, version_t version){

  version_t oldest_running_epoch = tm.getMinActiveVersion();
  pHeader->next_levels[level]->add_new_pointer(pointer, version, oldest_running_epoch, 4);
}

VSkipListHeader* VersioningBlockedSkipListAdjacencyList::copy_skip_list_block(dst_t src, VSkipListHeader *block, VSkipListHeader *blocks_per_level[SKIP_LIST_LEVELS], version_t version, int type){
  uint64_t start = __rdtsc();
  VSkipListHeader *new_old_block;
  uint16_t height = block->height;
      new_old_block = new_skip_list_block();
      // block->version = version;
      VSkipListHeader *pHead = (VSkipListHeader*)raw_neighbourhood_version(src, version);
      // find_block(pHead, *(block->data), version, blocks_per_level);

      EdgeBlock eb = EdgeBlock::from_vskip_list_header(block, block_size, property_size);
      EdgeBlock new_old_eb = EdgeBlock::from_vskip_list_header(new_old_block,block_size,property_size);
      eb.copy_into(new_old_eb);
      

      //update header 
      new_old_block->version = version;
      new_old_block->before = block->before;
      new_old_block->data = (dst_t*)((char*)new_old_block + skip_list_header_size());
      new_old_block->height = block->height;
      new_old_eb.update_skip_list_header(new_old_block);


    //update in parents
    int count = 0;
    for (uint l = 0; l < SKIP_LIST_LEVELS; l++) {

      if (l < height) {
        VSkipListHeader *next_on_level = get_latest_next_pointer(blocks_per_level[l],l,version);
        if (next_on_level == block){
          add_new_pointer(blocks_per_level[l], l, new_old_block,version);
          count++;
        }
      }
      else break;

      if(blocks_per_level[l]==block)
        blocks_per_level[l] = new_old_block;
    }

    for(int i=0;i<SKIP_LIST_LEVELS;i++){
      VSkipListHeader* pointer =  get_latest_next_pointer(block,i,version);
  
      new_old_block->next_levels[i]->add_new_pointer(pointer , version, tm.getMinActiveVersion(),6);
    }
    
    VSkipListHeader *next = (VSkipListHeader*)(new_old_block->next_levels[0]->get_latest_pointer());
    if(next!=nullptr)
      next->before = new_old_block;

    if(block == (VSkipListHeader*)adjacency_index.neighbourhood_version(src,version)){
      // uint64_t start = __rdtsc();
      adjacency_index[src].adjacency_set.add_new_pointer((void*)new_old_block, version, tm.getMinActiveVersion(), 7);
      // uint64_t end = __rdtsc();
    }

    local_free_list.add_node(block, version, block->changes);
    uint64_t end = __rdtsc();

    update_copy_time_p(src, end - start, 1);
    return new_old_block;
}

void VersioningBlockedSkipListAdjacencyList::insert_skip_list(edge_t edge, version_t version, char *properties) {


  VSkipListHeader *adjacency_list = (VSkipListHeader *) raw_neighbourhood_version(edge.src, version);

  VSkipListHeader *blocks_per_level[SKIP_LIST_LEVELS];
  auto block = find_block(adjacency_list, edge.dst, version, blocks_per_level);
  VSkipListHeader* new_old_block;

  if(block->version<version){
          // cout<<"new version being created :"<<edge.src<<endl;

    new_old_block = copy_skip_list_block(edge.src, block,blocks_per_level,version, 2);
    adjacency_list = (VSkipListHeader *) raw_neighbourhood_version(edge.src, version);
  }
  else new_old_block = block;
  EdgeBlock eb = EdgeBlock::from_vskip_list_header(new_old_block, block_size, property_size);

  // if(get_set_type(physical_id(519679), version) == SKIP_LIST){
  //   head = (VSkipListHeader*) raw_neighbourhood_version(physical_id(519679),version);
  //   v = head->version;
  //   // cout<<logical_id(edge.src)<<" "<<logical_id(edge.dst)<<endl;
  //   if(v>150)
  //   {
  //     cout<<"src this time "<<logical_id(edge.src)<<endl;
  //     exit(0);
  //   }
  // }
  
  // Handle a full block
  if (!eb.has_space_to_insert_edge()) {
    VSkipListHeader *new_block = new_skip_list_block();
    auto new_edge_block = EdgeBlock::from_vskip_list_header(new_block, block_size, property_size);
    auto height = get_height();

    eb.split_into(new_edge_block);
    eb.update_skip_list_header(new_old_block);
    new_edge_block.update_skip_list_header(new_block);

    new_block->version = version;
    new_block->before = new_old_block;
    new_block->data = (dst_t*)((char*)new_block + skip_list_header_size());
    new_block->height = height;
    
    // Insert new block into the skip list at level 0.
    new_block->next_levels[0]->add_new_pointer(get_latest_next_pointer(new_old_block,0,version), version, tm.getMinActiveVersion(),8);

    if (new_block->next_levels[0]->get_latest_pointer() != nullptr) {
      ((VSkipListHeader*)(new_block->next_levels[0]->get_latest_pointer()))->before = new_block;
    }
    add_new_pointer(new_old_block, 0, new_block,version);

    // Update skip list on all levels but 0
    int count = 1;
    for (uint l = 1; l < SKIP_LIST_LEVELS; l++) {
      if (l < height) {
        VSkipListHeader *parent_next_pointer = get_latest_next_pointer(blocks_per_level[l],l,version);
        if (parent_next_pointer != new_old_block && blocks_per_level[l]!=new_old_block) {
          add_new_pointer(new_block,l,parent_next_pointer,version);
          add_new_pointer(blocks_per_level[l],l,new_block,version);
          count++;
        } else {
          parent_next_pointer = get_latest_next_pointer(new_old_block,l,version);
          add_new_pointer(new_block,l,parent_next_pointer,version);
          add_new_pointer(new_old_block,l,new_block,version);
          count++;
          
        }
        blocks_per_level[l] = new_block;
      }
    }

    // if(count<height){
    //   cout<<"split block not updating all pointers"<<endl;
    //   cout<<"count: "<<count<<" height: "<<height<<endl;
    //   exit(0);
    // }

#if defined(DEBUG) && ASSERT_CONSISTENCY
    assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
#endif
    new_old_block->changes++;
    // Recursive call of max depth 1.
    insert_skip_list(edge, version, properties);
    return;
  } else {
    eb.insert_edge(edge.dst, version, properties);
    eb.update_skip_list_header(new_old_block);
    new_old_block->changes++;
    update_adjacency_size(edge.src, false, version);      //TODO
#if defined(DEBUG) && ASSERT_CONSISTENCY
    assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
#endif
    balance_block(new_old_block, adjacency_list, edge.src, version);
#if defined(DEBUG) && ASSERT_CONSISTENCY
    assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
#endif
  }
  
}

void VersioningBlockedSkipListAdjacencyList::report_storage_size() {
  throw NotImplemented();
//  size_t vertices = sizeof(SkipListHeader *) * adjacency_index.size();
//
//  // All numbers in bytes
//  size_t edges_single_block = 0;           // Edges in single block actual storage needs.
//  size_t edges_single_block_strictly = 0;  // Edges in single block minus storage overhead for having blocks with the sizes of power of twos only.
//  size_t edges_multi_block = 0;            // Edges in multi blocks but not the header.
//  size_t edges_multi_block_header = 0;     // Only the header of multi blocks.
//  size_t edges_multi_block_strictly = 0;   // Strictly needed storage for edges in multi blocks, so without the storage overhead of using a fixed size.
//
//  for (auto v = 0; v < vertex_count(); v++) {
//    if (get_set_type(v) == SINGLE_BLOCK) {
//      edges_single_block_strictly += neighbourhood_size(v) * sizeof(dst_t);
//      edges_single_block += round_up_power_of_two(neighbourhood_size(v)) * sizeof(dst_t);
//    } else {
//      SkipListHeader *ns = (SkipListHeader *) raw_neighbourhood(v);
//
//      while (ns != nullptr) {
//        edges_multi_block += block_size * sizeof(dst_t);
//        edges_multi_block_header += sizeof(BlockHeader) + LEVELS * sizeof(SkipListHeader *);
//        edges_multi_block_strictly += ns->size * sizeof(dst_t);
//        ns = (SkipListHeader *) ns->next;
//      }
//    }
//  }
//  // Total size of all edges
//  size_t edges = edges_single_block + edges_multi_block + edges_multi_block_header;
//
//  cout << "All metrics in MB" << endl;
//  cout << setw(30) << "Vertices: " << right << setw(20) << vertices / 1000000 << endl;
//  cout << endl;
//
//  cout << setw(30) << "Single block: " << right << setw(20) << edges_single_block / 1000000 << endl;
//  cout << setw(30) << "Single block overhead: " << right << setw(20)
//       << (edges_single_block - edges_single_block_strictly) / 1000000 << endl;
//  cout << setw(30) << "Multi block header: " << right << setw(20) << edges_multi_block_header / 1000000 << endl;
//  cout << setw(30) << "Edge multi block: " << right << setw(20) << edges_multi_block / 1000000 << endl;
//  cout << setw(30) << "Multi block overhead: " << right << setw(20)
//       << (edges_multi_block - edges_multi_block_strictly) / 1000000 << endl;
//
//  cout << setw(30) << "Edges: " << right << setw(20) << edges / 1000000 << endl;
//  cout << endl;
//  cout << setw(30) << "Total: " << right << setw(20) << (edges + vertices) / 1000000 << endl;
}

void VersioningBlockedSkipListAdjacencyList::aquire_vertex_lock_p(vertex_id_t v) {
  adjacency_index.aquire_vertex_lock_p(v);
}

void VersioningBlockedSkipListAdjacencyList::release_vertex_lock_p(vertex_id_t v) {
  adjacency_index.release_vertex_lock_p(v);
}

void VersioningBlockedSkipListAdjacencyList::aquire_vertex_lock_shared_p(vertex_id_t v) {
  adjacency_index.aquire_vertex_lock_shared_p(v);
}

void VersioningBlockedSkipListAdjacencyList::release_vertex_lock_shared_p(vertex_id_t v) {
  adjacency_index.release_vertex_lock_shared_p(v);
}

void assert_size_version_chain(forward_list<SizeVersionChainEntry>* chain) {
  version_t version_before = numeric_limits<version_t>::max();
  auto i = chain->begin();
  while (i != chain->end()) {
    assert(version_before > i->version);
    version_before = i->version;
    i++;
  }
  assert(version_before == FIRST_VERSION);
}


forward_list<SizeVersionChainEntry>
VersioningBlockedSkipListAdjacencyList::gc_adjacency_size(forward_list<SizeVersionChainEntry> &chain,
                                                          version_t collect_after) {
  // List used to store removed versions, we return this for reuse. There is no guarantue upon the order of the versions.
  // They should be reused or freed after.
  forward_list<SizeVersionChainEntry> to_drop;
#if defined(DEBUG) && ASSERT_CONSISTENCY
  assert_size_version_chain(&chain);
#endif

  auto sorted_active_versions = tm.get_sorted_versions();
  auto i = 0u; // Offset of the current active version.

  auto current = chain.begin(); // Will point to the version read by the current active version
  auto before = chain.begin(); // Will point to the version read by the active version before current.

  // Find the youngest version read by an active transaction.
  for (; i < sorted_active_versions.size(); i++) {
    auto v = sorted_active_versions[i];
    if (v == NO_TRANSACTION) {
      continue;
    }
    while (current->version > v) {
      current++;
      before++;
    }
    break;
  }
  // Current and before now points to the youngest read version in this chain.
  // We do not collect before to not complicate the process of building the list of sorted active transactions.

  i++; // We are not interested to the version older than the last v.
  assert_size_version_chain(&chain);
  for (; i < sorted_active_versions.size(); i++) {
    while (current->version > sorted_active_versions[i]) {
      current++;
    }
#if defined(DEBUG) && ASSERT_CONSISTENCY
    assert_size_version_chain(&chain);
#endif
    // Remove versions between
    if (current != before) {
      to_drop.splice_after(to_drop.before_begin(), chain, before, current);
    }
#if defined(DEBUG) && ASSERT_CONSISTENCY
    assert_size_version_chain(&chain);
#endif
    before = current;
  }


  // Current is now the oldest version read by any transaction. Mark it as FIRST_VERSION.
  current->version = FIRST_VERSION;

  // Collect all versions older than read by the oldest transaction
  to_drop.splice_after(to_drop.before_begin(), chain, current, chain.end());
#if defined(DEBUG) && ASSERT_CONSISTENCY
  assert_size_version_chain(&chain);
#endif
  return to_drop;
}

forward_list<SizeVersionChainEntry> *
VersioningBlockedSkipListAdjacencyList::construct_version_chain_from_block(vertex_id_t v, version_t version) {
  auto block = (dst_t *) raw_neighbourhood_version(v, version);
  auto[_, size, curr_version] = adjacency_index.get_single_block_size(v,version);
  auto min_version = tm.getMinActiveVersion();

  vector<version_t> versions_to_construct;
  for (auto i = block; i < block + size; i++) {
    if (is_versioned(*i)) {
      if (more_versions_existing(*(i + 1))) {
        EdgeVersionRecord vr(make_unversioned(*i), i + 1, nullptr, false, property_size);
        auto versions = vr.get_versions();
        for (auto v : versions) {
          if (min_version < v) {
            versions_to_construct.push_back(v);
          }
        }
      } else {
        auto v = (version_t) *(i + 1);
        auto t = timestamp(v);
        if (min_version < t) {
          versions_to_construct.push_back(t);
        }
      }
    }
  }

  sort(versions_to_construct.begin(), versions_to_construct.end());

  auto chain = new forward_list<SizeVersionChainEntry>();
  chain->push_front(SizeVersionChainEntry(FIRST_VERSION, neighbourhood_size_version_p(v, min_version)));

  for (uint i = 0; i < versions_to_construct.size(); i++) {
    chain->push_front(
            SizeVersionChainEntry(versions_to_construct[i], neighbourhood_size_version_p(v, versions_to_construct[i])));
  }
  return chain;
}


//DO NOT NEED THIS 
// void *VersioningBlockedSkipListAdjacencyList::raw_neighbourhood_size_entry(vertex_id_t v) {
//   return adjacency_index.raw_neighbourhood_size_entry(v);
// }

void VersioningBlockedSkipListAdjacencyList::gc_thread(version_t version, int id){
  
  auto current_thread = pthread_self();
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    if(id<4)
    CPU_SET(16+id, &cpu_set);
    else 
    CPU_SET(20+id, &cpu_set);
    auto rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set), &cpu_set);
    if (rc != 0) { cout<<"[pin_thread_to_cpu] pthread_setaffinity_np, rc: " << rc; }

  struct free_block block(0);
  int sum = 0;
  while(FreeList::get_next_block(version, block)){
                // cout<<"num elements: "<<block.num_elements<<endl;
                sum+=block.num_elements;
                // numBlocks++;
                // using clock = chrono::steady_clock;
                // clock::time_point m_t0; // start time
                //   clock::time_point m_t1;
                //   m_t0 = clock::now();
                for(int i=0;i<block.num_elements;i++){
                    // memory += block.type[i];
                  free_block(block.block[i],0);
                  
                }
                free_block(block.block,0);
                // m_t1 = clock::now();
                // std::chrono::duration<double> duration = m_t1 - m_t0;
                // cout<<"cleanded a block in "<<duration.count()<<endl;
          } 
  // cout<<"freed "<<sum<<" blocks"<<endl;
}

void helloWorld(){
  cout<<"hello\n";
}

void VersioningBlockedSkipListAdjacencyList::createEpoch(version_t version){
  versions.push(version);
}

void VersioningBlockedSkipListAdjacencyList::gc_all() {
  
  version_t min_active = tm.getMinActiveVersion();
  cout<<"calling GC for versions less than "<<min_active<<endl;

  vector<int> usedVersions; int sum=0;
  while(!versions.empty())
  {
    version_t version = versions.front();

    if(version<min_active)
    {
      sum=0; double memory = 0;
      if(version>100){
        usedVersions.push_back(version);
        cout<<"cleaning version "<<version<<endl;
        vector<int> all;
      // vector<std::thread> threads;
        
        using clock = chrono::steady_clock;
        int numBlocks = 0; double timetaken = 0; 
        clock::time_point m_t0; // start time
        clock::time_point m_t1; m_t0 = clock::now();
          
          std::vector<thread> threads;
          int num_gc_thread = 8;
          for(int i=0;i<num_gc_thread;i++){
            std::thread th (&VersioningBlockedSkipListAdjacencyList::gc_thread, version, i);
            threads.push_back(move(th));
          }

          for(int i=0;i<num_gc_thread;i++)
          {
            threads[i].join();
          }

          m_t1 = clock::now();
          std::chrono::duration<double> duration = m_t1 - m_t0;
          double total_time_taken = duration.count();
          cout<<"Total time taken: "<<total_time_taken<<" total memory freed: "<<memory<<endl;
          cout<<"number of blocks collected = "<<sum<<" across "<<numBlocks <<" in time: "<<timetaken<<endl;
          
          int count = 0;
          
      }
        versions.pop();
    }
    else break;
  }
  for(auto x:usedVersions)
    FreeList::eraseVersion(x);
  
}

//DO NOT NEED THIS ONE
void VersioningBlockedSkipListAdjacencyList::gc_vertex(vertex_id_t v) {
//   aquire_vertex_lock_p(v);
//   switch (get_set_type(v, FIRST_VERSION)) {
//     case VSINGLE_BLOCK: {
//       gc_block(v);
//       break;
//     }
//     case VSKIP_LIST: {
//       gc_skip_list(v);
//       break;
//     }
//   }
// #if defined(DEBUG) && ASSERT_CONSISTENCY
//   assert_adjacency_list_consistency(v, tm.getMinActiveVersion(), tm.getMinActiveVersion());
// #endif
//   if (get_set_type(v, FIRST_VERSION) == VSKIP_LIST && size_is_versioned(v)) {
//     auto chain = (forward_list<SizeVersionChainEntry> *) ((uint64_t) raw_neighbourhood_size_entry(v) &
//                                                           ~SIZE_VERSION_MASK);
//     gc_adjacency_size(*chain, tm.getMinActiveVersion());
//     if (chain->begin()->version == FIRST_VERSION) {
//       adjacency_index[v].size = chain->begin()->current_size;
//       free(chain);
//     }
//   }
//   release_vertex_lock_p(v);
}

bool VersioningBlockedSkipListAdjacencyList::gc_block(vertex_id_t v) {
//   assert(get_set_type(v, FIRST_VERSION) == VSINGLE_BLOCK);
//   auto[capacity, size] = adjacency_index.get__block_size(v);
//   bool version_remaining = false; // If a version remains after shifting.
//   if (is_versioned) {
//     EdgeBlock eb = EdgeBlock::from_single_block((dst_t *) raw_neighbourhood_version(v, FIRST_VERSION), capacity, size,
//                                                 property_count, property_size);
//     auto min_version = tm.getMinActiveVersion();
//     version_remaining = eb.gc(min_version, tm.get_sorted_versions());
//     adjacency_index.set_block_size(v, eb.get_block_capacity(), eb.get_edges_and_versions(), eb.get_property_count(),
//                                    version_remaining);
//   }
//   return !version_remaining;

  return false;
}

bool VersioningBlockedSkipListAdjacencyList::gc_skip_list(vertex_id_t v) {
//   auto min_version = tm.getMinActiveVersion();
//   bool version_remaining = false;
//   if (size_is_versioned(v)) {
//     VSkipListHeader *before = nullptr;
//     auto i = (VSkipListHeader *) raw_neighbourhood_version(v, FIRST_VERSION);
//     VSkipListHeader *blocks[SKIP_LIST_LEVELS];
//     for (int j = 0; j < SKIP_LIST_LEVELS; j++) {
//       blocks[j] = i;
//     }
//     while (i != nullptr) {
//       for (auto l = 0; l < SKIP_LIST_LEVELS; l++) {
//         if (i->next_levels[l] != nullptr) {
//           blocks[l] = i;
//         }
//       }
//       auto after = i->next_levels[0];
//       version_remaining |= gc_skip_list_block(&i, before, after, min_version, blocks, 0);
//       if (i != nullptr) {
//         before = i;
//         i = i->next_levels[0];
//       } else {
//         i = after;
//       }
//     }
//     auto skip_list = (VSkipListHeader *) raw_neighbourhood_version(v, FIRST_VERSION);
//     if (skip_list->next_levels[0] == nullptr) {  //  Single block skip list
//       skip_list_to_single_block(v, version_remaining);
//     }
//   }
//   return !version_remaining;
return false;
}

bool VersioningBlockedSkipListAdjacencyList::gc_skip_list_block(VSkipListHeader **to_clean, VSkipListHeader *before,
                                                                VSkipListHeader *after, version_t min_version,
                                                                VSkipListHeader *blocks[SKIP_LIST_LEVELS],
                                                                int leave_space) {
//   auto eb = EdgeBlock::from_vskip_list_header(*to_clean, block_size, property_size);
//   bool versions_remaining = eb.gc(min_version, tm.get_sorted_versions());
//   eb.update_skip_list_header(*to_clean);

  // TODO reimplement list merging

//  if (before != nullptr && new_size + leave_space < block_size / 2) {
//    if (new_size + before->size + leave_space <= block_size) {
//      merge_skip_list_blocks(*to_clean, before, blocks);
//      *to_clean = nullptr;
//    } else {
//      /**
//       * Moves elements from the block before the cleaned block into this block to keep it more than half full.
//       *
//       * It moves as many element as needed to make the cleaned block half full again.
//       * It moves data from the end of the block before to the beginning of this block.
//       */
//      auto move_elements = block_size / 2 - new_size;
//      auto before_data = get_data_pointer(before);
//
//      if (is_versioned(before_data[before->size - move_elements - 1])) {
//        move_elements += 1;
//      }
//
//      for (int i = new_size - 1; 0 <= i; i--) {
//        data[i + move_elements] = data[i];
//      }
//      memcpy(data, before_data + before->size - move_elements, sizeof(dst_t) * move_elements);
//
//
//      // TODO need to move properties
//
//      before->size = before->size - move_elements;
//      (*to_clean)->size = (uint16_t) (new_size + move_elements);
//
//      if (is_versioned(before_data[before->size - 2])) {
//        before->max = make_unversioned(before_data[before->size - 2]);
//      } else {
//        before->max = before_data[before->size - 1];
//      }
//#if defined(DEBUG) && ASSERT_CONSISTENCY
//      assert_block_consistency(get_data_pointer(*to_clean), get_data_pointer(*to_clean) + (*to_clean)->size,
//                               FIRST_VERSION);
//      assert_block_consistency(get_data_pointer(before), get_data_pointer(before) + before->size, FIRST_VERSION);
//#endif
//    }
//  }
//   return versions_remaining;
return false;
}


//UPDATED
void VersioningBlockedSkipListAdjacencyList::merge_skip_list_blocks(VSkipListHeader *block, VSkipListHeader *head,
                                                                    vertex_id_t src, version_t version) {

  if(block->version<version){
    auto eb = EdgeBlock::from_vskip_list_header(block, block_size, property_size);
    VSkipListHeader *predecessors[SKIP_LIST_LEVELS];
    // Find predecessors, that needs to happen before moving the elements
    find_block(head, eb.get_min_edge(), version, predecessors);
    block = copy_skip_list_block(src, block, predecessors, version,3);
    head = (VSkipListHeader*) raw_neighbourhood_version(src, version);
  }

  auto before = block->before;
  auto after = get_latest_next_pointer(block,0,version);

  // for(int i=0;i<block->size;i++)
  // cout<<*(block->data + i)<<" ";
  // cout<<endl<<endl;

  if (block == head) {  // The first block
    if (after != nullptr) {  // Their is a second block. We merge with this one.
      merge_skip_list_blocks(after, head, src, version);
    } else {
      return;  // Nop we cannot merge a singleton list. We do not implement turning it back into a single block adjacency set
    }
  } else if (after == nullptr) {
    // When merging the last block, we instead merge the block before it into the last block.
    if (before == head) {
      return;  // Nop we do not merge lists with only two blocks. We keep both blocks and they might become empty.
    }
    merge_skip_list_blocks(before, head, src, version);
  } else {  // We are dealing with a middle block, we merge it into the smaller neighbour.
    auto eb = EdgeBlock::from_vskip_list_header(block, block_size, property_size);
    VSkipListHeader *predecessors[SKIP_LIST_LEVELS];
    // Find predecessors, that needs to happen before moving the elements
    find_block(head, eb.get_min_edge(), version, predecessors);
    // First, we move all elements out of the block in question.
    // cout<<"predecessors after searching for : "<<eb.get_min_edge();
    // for(int i=0;i<SKIP_LIST_LEVELS;i++)
    //   cout<<"level :"<<i<<" "<<*(predecessors[i]->data)<<endl;
    if (after->size < before->size) {

      // cout<<"if after->size"<<endl;
      auto eb_after = EdgeBlock::from_vskip_list_header(after, block_size, property_size);
      if(after->version!=version)
      {
        VSkipListHeader *after_parents[SKIP_LIST_LEVELS];
        find_block(head,eb_after.get_min_edge(),version,after_parents);
        after = copy_skip_list_block(src,after,after_parents,version,4);
        eb_after = EdgeBlock::from_vskip_list_header(after, block_size, property_size);
      }

      assert(after->size + block->size <= block_size && "The caller ensures this block can be merged.");
      EdgeBlock::move_forward(eb, eb_after, eb.count_edges());
      eb_after.update_skip_list_header(after);

        for (auto l = 0; l < SKIP_LIST_LEVELS; l++) {
            if (get_latest_next_pointer(predecessors[l],l,version) == block) 
              add_new_pointer(predecessors[l],l,get_latest_next_pointer(block,l,version),version);
            // if(predecessors[l] == block)
            // {
            //   cout<<"the predecessor is itself equal to the block on level "<<l<<endl<<endl;
            //   exit(0);
            // }
          }
      after->before = before;
      {
        free_block(block, memory_block_size());
      }
      gc_merges += 1;


    } else {
      // cout<<"else after->size"<<endl;
      auto eb_before = EdgeBlock::from_vskip_list_header(before, block_size, property_size);
      if(before->version!=version)
      {
        VSkipListHeader *before_parents[SKIP_LIST_LEVELS];

        find_block(head,eb_before.get_min_edge(),version,before_parents);
        before= copy_skip_list_block(src, before,before_parents,version,5);
        eb_before = EdgeBlock::from_vskip_list_header(before, block_size, property_size);
        for(int i=0;i<SKIP_LIST_LEVELS;i++)
          if(*(before->data) == *(predecessors[i]->data))
            predecessors[i] = before;
      }

      assert(before->size + block->size <= block_size && "The caller ensures this block can be merged.");
      EdgeBlock::move_backward(eb, eb_before, eb.count_edges());
      eb_before.update_skip_list_header(before);

      for (auto l = 0; l < SKIP_LIST_LEVELS; l++) {
            // cout<<"level :"<<l<<" "<<*(predecessors[l]->data)<<endl;
            if (get_latest_next_pointer(predecessors[l],l,version) == block) 
              add_new_pointer(predecessors[l],l,get_latest_next_pointer(block,l,version),version);
      }
      after->before = before;
      // cout<<"\n\nall this ahppening"<<endl<<endl;
      {
        free_block(block, memory_block_size());
      }
      gc_merges += 1;

    }

    // Second, we remove it from the skiplist.
    
  }
}

void VersioningBlockedSkipListAdjacencyList::skip_list_to_single_block(vertex_id_t v, version_t version) {
  assert(get_set_type(v, FIRST_VERSION) == VSKIP_LIST);
  assert(adjacency_index.get_latest_version(v,version) == version);

  auto skip_list_block = (VSkipListHeader *) raw_neighbourhood_version(v, FIRST_VERSION);
  assert(skip_list_block->next_levels[0]->get_latest_pointer() == nullptr);

  auto size = (uint64_t) skip_list_block->size;
  if (size < block_size / 2) {
    auto single_block_size = round_up_power_of_two(size);
    dst_t* single_block = (dst_t *) get_block(get_single_block_memory_size(single_block_size));
    EdgeBlock e_b((dst_t*)get_data_pointer(skip_list_block), block_size, skip_list_block->size, property_size);
    EdgeBlock new_e_b((dst_t*)single_block, single_block_size, 0, property_size);

    e_b.copy_into(new_e_b);
    {
      free_block(skip_list_block, memory_block_size());
    }

    adjacency_index.store_single_block(v, new_e_b.get_single_block_pointer(), new_e_b.get_block_capacity(),
                                       size, version);
    gc_to_single_block += 1;
  }
}

size_t VersioningBlockedSkipListAdjacencyList::assert_edge_block_consistency(EdgeBlock eb, vertex_id_t src,
                                                                             version_t version) {
#if defined(DEBUG) && ASSERT_WEIGHTS
  auto l_v = logical_id(src);
  auto property_start = eb.properties_start();
  auto property_offset = 0;
#endif

  auto start = eb.start;
  auto size = eb.get_edges();

  long before = -1;
  auto versions = 0;

  for (auto i = start; i < start + size; i++) {
    auto e = *i;
    if (is_versioned(e)) {
      versions++;
      auto ue = make_unversioned(e);
      assert(before < (long) ue);
      before = make_unversioned(e);

      assert(i + 1 < start + size);

      if (*(i + 1) & MORE_VERSION_MASK) {
        EdgeVersionRecord vr{make_unversioned(e), i + 1, nullptr, false, 0};
        vr.assert_version_list(version);
      } else {
        auto timest = timestamp(*(i + 1));
//        assert(version <= timest);
// TODO this should be min version
      }
      i += 1; // Jump over version
    } else {
      if (!(before < (long) e)) {
        eb.print_block([](dst_t d) { return d; });
      }
      assert(before < (long) e);
      before = e;

    }
#if defined(DEBUG) && ASSERT_WEIGHTS
    if (typeid(weight_t) == typeid(dst_t)) {
      auto p = ((dst_t *) property_start)[property_offset];
      if (p != l_v) {
        auto uv = adjacency_index.logical_id(make_unversioned(e));
        if (uv != p) {

          eb.print_block([&index = adjacency_index](dst_t p_id) -> dst_t { return index.logical_id(p_id); });
          cout << "eb: " << eb.start << endl;
        }
        assert(p == uv);
      }
    } else if (typeid(weight_t) == typeid(double)) {
      auto p = ((double *) property_start)[property_offset];
      assert(p < 1.1);
      assert(0.01 <= p);
    } else {
      throw ConfigurationError("Cannot check weight conistency for types other than dst_t or double");
    }
    property_offset += 1;
#endif
  }
  return versions;
}

void VersioningBlockedSkipListAdjacencyList::assert_adjacency_list_consistency(vertex_id_t v, version_t min_version, version_t current_version) {
  auto actual_size = 0u;
  switch (get_set_type(v, min_version)) {
    case VSKIP_LIST: {
      auto start = (VSkipListHeader *) raw_neighbourhood_version(v, min_version);

      auto i = start;
      VSkipListHeader *before = nullptr;
      VSkipListHeader *blocks[SKIP_LIST_LEVELS];
      for (int j = 0; j < SKIP_LIST_LEVELS; j++) {
        blocks[j] = start;
      }

      while (i != nullptr) {
        assert(i->size <= block_size);
        // If not the last block, the last block could contain less than b_size / 2 elements after bulkloading.
        // && if not the first block because the first block might have less than block_size / 2 elemetns because I only move elements forwards in GC
        if (i->next_levels[0]->get_latest_pointer() != nullptr && i != start) {
          // TODO fix that the fact that the last block is less than half full after bulkloading.
//          assert( block_size / 2 - 3 <= i->size);  // TODO there's a bug such that some blocks are slightly smaller than block_size / 2
        }

        auto eb = EdgeBlock::from_vskip_list_header(i, block_size, property_size);
        auto versions = assert_edge_block_consistency(eb, v, min_version);

        assert(i->max == eb.get_max_edge());
        actual_size += eb.count_edges();

        for (auto l = 0; l < SKIP_LIST_LEVELS; l++) {
          if (i->next_levels[l]->get_latest_pointer() != nullptr) {
            if (i != start) {
              assert(blocks[l]->next_levels[l]->get_latest_pointer() == i);
            }
            blocks[l] = i;
            auto min_after = get_min_from_skip_list_header((VSkipListHeader*)(i->next_levels[l]->get_latest_pointer()));
            assert(i->max < min_after);
          }
        }
        assert(before == i->before);
        before = i;
        i = (VSkipListHeader*)i->next_levels[0]->get_latest_pointer();
      }
      break;
    }
    case VSINGLE_BLOCK: {
      auto[capacity, size, curr_version] = adjacency_index.get_single_block_size(v, current_version);
      auto start = (dst_t *) raw_neighbourhood_version(v, min_version);
      auto eb = EdgeBlock::from_single_block(start, capacity, size, property_size);

      auto versions = assert_edge_block_consistency(eb, v, min_version);

      // assert((is_versioned && 0 < versions) || (!is_versioned && versions == 0));
      actual_size = eb.count_edges();
      break;
    }
  }
#if defined(DEBUG) && ASSERT_SIZE
  forward_list<SizeVersionChainEntry>* chain = nullptr;
  switch(get_set_type(v, min_version)) {
    case VSKIP_LIST: {
      if (size_is_versioned(v)) {
        chain = (forward_list<SizeVersionChainEntry> *) ((uint64_t) adjacency_index[v].size & ~SIZE_VERSION_MASK);
        assert_size_version_chain(chain);
      }
    }
  }
  auto retrieved_size = neighbourhood_size_version_p(v, current_version);
  assert(retrieved_size == actual_size);
#endif
}


dst_t VersioningBlockedSkipListAdjacencyList::get_min_from_skip_list_header(VSkipListHeader *header) {
  return make_unversioned(get_data_pointer(header)[0]);
}

VersioningBlockedSkipListAdjacencyList::~VersioningBlockedSkipListAdjacencyList() {

  //  ofstream output;
  // output.open("wait_time.csv");

  // int N = max_physical_vertex();
  // for(int i=0;i<N;i++){
  //   size_t size = neighbourhood_size_version_p(i, 100000000000000);
  //   double wait = adjacency_index[i].wait_time_aggregate;
  //   double ans=0;
  //   if(adjacency_index[i].wait_time_num_invoke)
  //    ans = wait/adjacency_index[i].wait_time_num_invoke;
  //   output<<size<<","<<wait<<","<<adjacency_index[i].wait_time_num_invoke<<","<<ans<<endl;
  // }

  // ofstream output_shared;
  // output_shared.open("wait_time_shared.csv");

  // for(int i=0;i<N;i++){
  //   size_t size = neighbourhood_size_version_p(i, 100000000000000);
  //   double wait_shared = adjacency_index[i].wait_time_aggregate_shared;
  //   double ans=0;
  //   if(adjacency_index[i].wait_time_shared_num_invoke)
  //     ans = wait_shared/adjacency_index[i].wait_time_shared_num_invoke;
  //   output_shared<<size<<","<<wait_shared<<","<<adjacency_index[i].wait_time_shared_num_invoke<<","<<ans<<endl;
  // }

  // ofstream copy_output;
  // copy_output.open("copy_time.csv");

  // for(int i=0;i<N;i++){
  //   size_t size = neighbourhood_size_version_p(i, 100000000000000);
  //   double wait_shared = adjacency_index[i].copy_time_aggregate;
  //   double ans=0;
  //   if(adjacency_index[i].copy_time_num_invoke)
  //     ans = wait_shared/adjacency_index[i].copy_time_num_invoke;
  //   copy_output<<size<<","<<wait_shared<<","<<adjacency_index[i].copy_time_num_invoke<<","<<ans<<endl;
  // }

  // ofstream write_output;
  // write_output.open("write_time.csv");

  // for(int i=0;i<N;i++){
  //   size_t size = neighbourhood_size_version_p(i, 100000000000000);
  //   double wait_shared = adjacency_index[i].write_time_aggregate;
  //   double ans=0;
  //   if(adjacency_index[i].write_time_num_invoke)
  //     ans = wait_shared/adjacency_index[i].write_time_num_invoke;
  //   write_output<<size<<","<<wait_shared<<","<<adjacency_index[i].write_time_num_invoke<<","<<ans<<endl;
  // }

  // ofstream read_output;
  // read_output.open("read_time.csv");

  // for(int i=0;i<N;i++){
  //   size_t size = neighbourhood_size_version_p(i, 100000000000000);
  //   double wait_shared = adjacency_index[i].read_time_aggregate;
  //   double ans=0;
  //   if(adjacency_index[i].read_time_num_invoke)
  //     ans = wait_shared/adjacency_index[i].read_time_num_invoke;
  //   read_output<<size<<","<<wait_shared<<","<<adjacency_index[i].read_time_num_invoke<<","<<ans<<endl;
  // }

  // for(int i=0;i<N;i++)
  // {
  //   cout<<adjacency_index[i].adjacency_set.get_latest_version()<<endl;
  // }

  gc_all();  // Make the data structure completely unversioned.

#if defined(DEBUG) && ASSERT_CONSISTENCY
  for (vertex_id_t i = 0; i < get_max_vertex(); i++) {
    switch (get_set_type(i, FIRST_VERSION)) {
      case VSINGLE_BLOCK: {
        auto[capacity, size, pc, _] = adjacency_index.get_block_size(i);
        assert(size == pc);  // Every version has been cleaned up. There should be as many edges as properties.
        break;
      }
      case VSKIP_LIST: {
        auto sl = (VSkipListHeader *) raw_neighbourhood_version(i, FIRST_VERSION);
        while (sl != nullptr) {
          //assert(sl->size == sl->properties);
          sl = sl->next_levels[0];
        }
        break;
      }
    }
  }
#endif

  for (auto v = 0u; v < get_max_vertex(); v++) {
    free_adjacency_set(v);
  }
}

void VersioningBlockedSkipListAdjacencyList::free_adjacency_set(vertex_id_t v) {
  // assert(!size_is_versioned(v));
  version_t curr_version = tm.get_epoch();
  switch (get_set_type(v, curr_version)) {
    case VSKIP_LIST: {
      auto skip_list_header = (VSkipListHeader *) raw_neighbourhood_version(v, curr_version);

      while (skip_list_header != nullptr) {
        auto next = get_latest_next_pointer(skip_list_header,0,curr_version);
        free_block(skip_list_header, memory_block_size());

        skip_list_header = next;
      }
      // adjacency_index[v].adjacency_set = (uint64_t)
              // nullptr;
      // adjacency_index[v].size = 0;
      break;
    }
    case VSINGLE_BLOCK: {
      auto block = (dst_t *) raw_neighbourhood_version(v, curr_version);
      auto[capacity, size, block_version] = adjacency_index.get_single_block_size(v, curr_version);
      free_block(block, 0);
      adjacency_index.set_block_size(v, 0, 0, curr_version);
      // adjacency_index[v].adjacency_set = (uint64_t)
      //         nullptr;
      break;
    };
  }

}

bool VersioningBlockedSkipListAdjacencyList::insert_vertex_version(vertex_id_t v, version_t version) {
  return adjacency_index.insert_vertex(v, version);
}

bool VersioningBlockedSkipListAdjacencyList::has_vertex_version_p(vertex_id_t v, version_t version) {
  return adjacency_index.has_vertex_version_p(v, version);
}

size_t VersioningBlockedSkipListAdjacencyList::get_max_vertex() {
  return adjacency_index.get_high_water_mark();
}

size_t VersioningBlockedSkipListAdjacencyList::edge_count_version(version_t version) {
  size_t sum = 0;
  long long other_sum = 0;
  ofstream output;
  output.open("edges_size.txt");
  for (size_t v = 0, sz = get_max_vertex(); v < sz; v++) {
    bool locked = false;
    if(tm.get_epoch() == version){
      aquire_vertex_lock_p(v);
      locked = true;
    }
    sum += neighbourhood_size_version_p(v, version);
    // cout<<logical_id(v)<<" "<<neighbourhood_size_version_p(v, version)<<endl;
    // output<<logical_id(v)<<" "<<neighbourhood_size_version(v, version)<<endl;
    if(locked)
    release_vertex_lock_p(v);
  }
  // output.close();
  SnapshotTransaction tx = tm.getSnapshotTransaction(this, false, false);
  for (size_t v = 0, sz = get_max_vertex(); v < sz; v++) {
    long long edge_num = 0;
    long long num_edge = neighbourhood_size_version_p(v, version);
    SORTLEDTON_ITERATE(tx, v, {
      other_sum++;
      edge_num++;
    });
    
  }
  cout<<"other sum: "<<other_sum<<" sum: "<<sum<<endl;
  return sum;
}

bool VersioningBlockedSkipListAdjacencyList::aquire_vertex_lock(vertex_id_t v) {
  return adjacency_index.aquire_vertex_lock(v);
}

void VersioningBlockedSkipListAdjacencyList::release_vertex_lock(vertex_id_t v) {
  adjacency_index.release_vertex_lock(v);
}

vertex_id_t VersioningBlockedSkipListAdjacencyList::physical_id(vertex_id_t v) {
  return adjacency_index.physical_id(v).value();
}

vertex_id_t VersioningBlockedSkipListAdjacencyList::logical_id(vertex_id_t v) {
  return adjacency_index.logical_id(v);
}

void VersioningBlockedSkipListAdjacencyList::rollback_vertex_insert(vertex_id_t v) {
  adjacency_index.rollback_vertex_insert(v);
}

bool VersioningBlockedSkipListAdjacencyList::has_vertex_version(vertex_id_t v, version_t version) {
  return adjacency_index.has_vertex(v);
}

size_t VersioningBlockedSkipListAdjacencyList::max_physical_vertex() {
  return adjacency_index.get_high_water_mark();
}

EdgeBlock VersioningBlockedSkipListAdjacencyList::new_single_edge_block(size_t capacity) {
  assert(capacity <= block_size);
  assert(MIN_BLOCK_SIZE <= capacity);
  auto block = (dst_t *) get_block(get_single_block_memory_size(capacity));
  return EdgeBlock(block, capacity, 0, property_size);
}

VSkipListHeader *VersioningBlockedSkipListAdjacencyList::new_skip_list_block() {
  auto h = (VSkipListHeader *) get_block(memory_block_size());
  // std::cout<<"\npointer: "<<h<<std::endl;

  h->data = get_data_pointer(h);
  h->before = nullptr;
  h->height = 0;
  h->size = 0;
  h->max = 0;
  h->changes = 0;
  void *start = (void*)(h+1);

  // std::cout<<"size of header: "<<sizeof(VSkipListHeader)<<" + "<<sizeof(AllInlineAccessPointers)*SKIP_LIST_LEVELS<<std::endl;

  // std::cout<<"number of bytes between start and accesspointers: "<<((char*)start-(char*)h)<<std::endl;
  // std::cout<<"number of bytes between accesspointers and data: "<<((char*)(h->data)-(char*)start)<<std::endl;


  for(int i=0;i<SKIP_LIST_LEVELS;i++){
      h->next_levels[i] = new(start) AllInlineAccessPointers();
      start = start + sizeof(AllInlineAccessPointers);
  }

  // std::cout<<"Successfully initialised:"<<((char*)start-(char*)h->data)<<"\n";
  
  return h;
}

size_t VersioningBlockedSkipListAdjacencyList::get_property_size() {
  return property_size;
}

VersionedBlockedEdgeIterator
VersioningBlockedSkipListAdjacencyList::neighbourhood_version_blocked_p(vertex_id_t src, version_t version) {
  // aquire_vertex_lock_shared_p(src);  // Only released once the iterator is closed. Dirty!
  void *set = raw_neighbourhood_version(src, version);

  switch (get_set_type(src, version)) {
    case VSINGLE_BLOCK: {
      auto[capacity, s, curr_version] = adjacency_index.get_single_block_size(src,version);
      EdgeBlock eb = EdgeBlock::from_single_block((dst_t*)set, capacity, s, property_size);
      return VersionedBlockedEdgeIterator(this, src, (dst_t *) set, capacity, s, property_size, version);
    }
    case VSKIP_LIST: {
      VSkipListHeader* ans = (VSkipListHeader*)set;
      
      return VersionedBlockedEdgeIterator(this, src, (VSkipListHeader *) set, block_size, property_size, version);
    }
    default: {
      throw NotImplemented();
    }
  }
}

VersionedBlockedPropertyEdgeIterator
VersioningBlockedSkipListAdjacencyList::neighbourhood_version_blocked_with_properties_p(vertex_id_t src,
                                                                                        version_t version) {
  // aquire_vertex_lock_shared_p(src);  // Only released once the iterator is closed. Dirty!
  void *set = raw_neighbourhood_version(src, version);

  switch (get_set_type(src, version)) {
    case VSINGLE_BLOCK: {
      auto[capacity, s, curr_version] = adjacency_index.get_single_block_size(src,version);
      auto eb = EdgeBlock::from_single_block((dst_t *) set, capacity, s, property_size);
      return VersionedBlockedPropertyEdgeIterator(this, src, eb.start, eb.get_edges(), capacity,
                                                  version, property_size,
                                                  (weight_t *) eb.properties_start());
    }
    case VSKIP_LIST: {
      return VersionedBlockedPropertyEdgeIterator(this, src, (VSkipListHeader *) set, block_size,
                                                  version, property_size);
    }
    default: {
      throw NotImplemented();
    }
  }

}


void *VersioningBlockedSkipListAdjacencyList::get_block(size_t size) {
//  return pool.get_block(size);
  if (size == memory_block_size()) {
    return aligned_alloc(PAGE_SIZE, size);
  } else {
    return aligned_alloc(CACHELINE_SIZE, size);
  }
}

void VersioningBlockedSkipListAdjacencyList::free_block(void *block, size_t size) {
//  pool.free_block(block, size);
  free(block);
}

size_t VersioningBlockedSkipListAdjacencyList::get_single_block_memory_size(size_t capacity) {
  // cout<<capacity<<endl;
  // std::cout<<"memory bytes = "<<(max(1ul,capacity/64))*sizeof(uint64_t) + capacity * sizeof(dst_t) + capacity * sizeof(weight_t) + capacity * property_size<<" property size ="<<property_size<<std::endl;
  return  (max(1ul,capacity/64))*sizeof(uint64_t) + capacity * sizeof(dst_t) + capacity * sizeof(weight_t) + capacity * property_size;
}

void
VersioningBlockedSkipListAdjacencyList::balance_block(VSkipListHeader *block, VSkipListHeader *head, vertex_id_t src, version_t version) {
    
  if (block->size < low_skiplist_block_bound()) {   // Block is too empty

    if(block->version!=version){
      VSkipListHeader *predecessors[SKIP_LIST_LEVELS];
      EdgeBlock eb = EdgeBlock::from_vskip_list_header(block, block_size, property_size);
      find_block(head, eb.get_min_edge(),version, predecessors);
      block = copy_skip_list_block(src, block, predecessors, version,6);
      head = (VSkipListHeader*) raw_neighbourhood_version(src, version);
    }

    auto before_block = block->before;
    auto next_block = get_latest_next_pointer(block,0,version);

    if (before_block == nullptr && next_block == nullptr) {
      return; // NOP we do not implement merging back into a single block
    }

    // Pick bigger block for rebalance and handle first and last block.
    auto balance_against = before_block;
    if (before_block == nullptr) {
      balance_against = next_block;
    } else if (next_block == nullptr) {
      balance_against = before_block;
    } else if (before_block->size < next_block->size) {
      balance_against = next_block;
    }

    // TODO maybe version clean the block we balancing again here.

    if (balance_against->size + block->size <= block_size) {
      // cout<<"merging "<<*(block->data)<<" and "<<*(balance_against->data)<<endl;
      merge_skip_list_blocks(block, head, src, version);
        // cout<<"printing after merge"<<endl<<endl;
        // edge_t e{100,200};
        // head = (VSkipListHeader*) raw_neighbourhood_version(src, 200);
        // cout<<"head: "<<head<<endl;
        // edge_t e{0,567};
        // cout<<has_edge_version_p(e,100)<<" <- has edge check from balance"<<endl;
    } else {
      auto balanced = (balance_against->size + block->size) / 2;
      auto to_move = balance_against->size - balanced;

      if(balance_against->version!=version){
        VSkipListHeader *predecessors[SKIP_LIST_LEVELS];
        EdgeBlock eb = EdgeBlock::from_vskip_list_header(balance_against, block_size, property_size);
        find_block(head, eb.get_min_edge() ,version, predecessors);
        balance_against = copy_skip_list_block(src, balance_against, predecessors, version,7);
      }

      auto to = EdgeBlock::from_vskip_list_header(block, block_size, property_size);
      auto from = EdgeBlock::from_vskip_list_header(balance_against, block_size, property_size);

      if (to.get_max_edge()<from.get_max_edge()) {
        EdgeBlock::move_backward(from, to, to_move);
      } else {
        EdgeBlock::move_forward(from, to, to_move);
      }
      to.update_skip_list_header(block);
      from.update_skip_list_header(balance_against);
      balance_against->changes++;
    }
    edge_t e{0,200};
    // cout<<"balanced block "<<endl<<endl;
    // get_weight_version_p(e, 100, nullptr);
  }
  else{
    // cout<<"\nNot balancing block\n";
  }
  
}

size_t VersioningBlockedSkipListAdjacencyList::low_skiplist_block_bound() {
  // TODO lower to 0.4
  return block_size *
         0.45;  // We choose 0.4 to avoid going back and forth between growing and shrinking blocks due to removing versions.
}

bool VersioningBlockedSkipListAdjacencyList::delete_edge_version(edge_t edge, version_t version) {
  uint64_t start = __rdtsc();
  void *adjacency_list = raw_neighbourhood_version(edge.src, version);
  __builtin_prefetch((void *) ((uint64_t) adjacency_list & ~EDGE_SET_TYPE_MASK));
  // __builtin_prefetch((void *) ((uint64_t) ((dst_t *) adjacency_list + 1) & ~SIZE_VERSION_MASK));
  // Insert to empty list
  if (unlikely(adjacency_list == nullptr)) {
    return false;
  } else {
    switch (get_set_type(edge.src, version)) {
      case SINGLE_BLOCK: {
        size_t siz = neighbourhood_size_version_p(edge.src, version);
        auto ans = delete_from_single_block(edge, version);
        edge_t e(0, 3322);
        // if(has_edge_version_p(e, 100)) cout<<"3322 present after deletion"<<endl;
        // else cout<<"3322 not present after deletion"<<endl;
        // get_weight_version_p(e, version, nullptr);
        uint64_t end = __rdtsc();
        update_write_time_p(edge.src, end - start, 1);
        // validate_change(version, edge.src, 3, siz);
        return ans;
      }
      case SKIP_LIST: {
        size_t siz = neighbourhood_size_version_p(edge.src, version);
        auto ans = delete_skip_list(edge, version);
        edge_t e(0, 3322);
        uint64_t end = __rdtsc();
        update_write_time_p(edge.src, end - start, 1);
        // if(has_edge_version_p(e, 100)) cout<<"3322 present after deletion"<<endl;
        // else cout<<"3322 not present after deletion"<<endl;
        // get_weight_version_p(e, version, nullptr);
        // validate_change(version, edge.src, 4, siz);
        return ans;
      }
      default: {
        throw NotImplemented();
      }
    }
  }
  return true;
}


bool VersioningBlockedSkipListAdjacencyList::delete_from_single_block(edge_t edge, version_t version) {

  auto[block_capacity, size, curr_version] = adjacency_index.get_single_block_size(edge.src, version);
  auto eb = EdgeBlock::from_single_block((dst_t *) raw_neighbourhood_version(edge.src, version), block_capacity, size, property_size);
  if(curr_version!=version){
          // cout<<"new version being created :"<<edge.src<<endl;
    uint64_t start = __rdtsc();
    adjacency_index.create_new_version(edge.src, version, tm.getMinActiveVersion());
    int siz = 24*block_capacity;
    
    EdgeBlock new_eb = new_single_edge_block(block_capacity);
    eb.copy_into(new_eb);
    local_free_list.add_node(eb.get_single_block_pointer(),version, 0);
    eb = EdgeBlock::from_single_block(new_eb.get_single_block_pointer(), block_capacity, size, property_size);
    uint64_t end = __rdtsc();
    update_copy_time_p(edge.src, end - start, 1);
  }
#if defined(DEBUG) && ASSERT_CONSISTENCY
  assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
#endif
  
    eb.delete_edge(edge.dst);
    adjacency_index.store_single_block(edge.src, eb.get_single_block_pointer(), block_capacity, size-1, version);
  return true;
}

bool VersioningBlockedSkipListAdjacencyList::delete_skip_list(edge_t edge, version_t version) {
  VSkipListHeader *adjacency_list = (VSkipListHeader *) raw_neighbourhood_version(edge.src, version);

  VSkipListHeader *blocks_per_level[SKIP_LIST_LEVELS];
  auto block = find_block(adjacency_list, edge.dst, version, blocks_per_level);

  version_t old_version = block->version; bool flag=false;
  adjacency_index.create_new_version(edge.src, version, tm.getMinActiveVersion());
  if(block->version!=version){
    flag = true;
    block = copy_skip_list_block(edge.src, block, blocks_per_level,version,8);
    adjacency_list = (VSkipListHeader *) raw_neighbourhood_version(edge.src, version);
  }
  
  auto eb = EdgeBlock::from_vskip_list_header(block, block_size, property_size);
  
#if defined(DEBUG) && ASSERT_CONSISTENCY
  assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
#endif
  {
    
      bool  ret = eb.delete_edge(edge.dst);
      block->changes++;
      eb.update_skip_list_header(block);
      update_adjacency_size(edge.src, true, version);   //TODO
      #if defined(DEBUG) && ASSERT_CONSISTENCY
          assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
      #endif
      balance_block(block, adjacency_list, edge.src, version);
    
#if defined(DEBUG) && ASSERT_CONSISTENCY
    assert_adjacency_list_consistency(edge.src, tm.getMinActiveVersion(), version);
#endif 

  
    return ret;
    // return true;
  }
}

bool VersioningBlockedSkipListAdjacencyList::get_weight_version_p(edge_t edge, version_t version, char *out) {
  switch (get_set_type(edge.src, version)) {
    case SKIP_LIST: {
      VSkipListHeader *head = (VSkipListHeader *) raw_neighbourhood_version(edge.src, version);

      cout<<"been called: "<<edge.src<<" "<<head << "\n\n"<<endl;;
      while(head!=nullptr){
        if(head ==0 ) break;

        EdgeBlock x = EdgeBlock::from_vskip_list_header(head, block_size,property_size);
        dst_t *iter = x.start;
        x.print_bitset();
        for(int i=0;i<512;i++)
          std::cout<<*(iter+i)<<" ";
        std::cout<<std::endl;

        std::cout<<"max for the block: "<<head->max<<" size for the block: "<<head->size<<" version: "<<head->version<<std::endl;
        std::cout<<"size form eb: "<<x.get_edges()<<endl<<endl;;
        cout<<"max edge index: "<<x.get_max_edge_index()<<endl;

        head = (VSkipListHeader*)head->next_levels[0]->get_pointer(version);

      }
      return false;
      if (head != nullptr) {
        auto block = find_block1(head, edge.dst, version);
        auto eb = EdgeBlock::from_vskip_list_header(block, block_size, property_size);
        return eb.get_weight(edge.dst, out);
      } else {
        return false;
      }
      break;
    }
    case SINGLE_BLOCK: {
      cout<<"been called: "<<edge.src<<" " << "\n\n"<<endl;;
      auto[block_capacity, size, curr_version] = adjacency_index.get_single_block_size(edge.src,version);
      auto eb = EdgeBlock::from_single_block((dst_t *) raw_neighbourhood_version(edge.src, version), block_capacity,
                                             size, property_size);
        dst_t *iter = eb.start;
        eb.print_bitset();
        for(int i=0;i<block_capacity;i++)
          std::cout<<*(iter+i)<<" ";
        std::cout<<std::endl;

        std::cout<<"single block max for the block: "<<eb.get_max_edge()<<" size for the block: "<<eb.get_edges()<<" version: "<<version<<std::endl<<std::endl;
        return false;
      return eb.get_weight(edge.dst, out);
    }
    default:
      throw NotImplemented();

  }
}

forward_list<SizeVersionChainEntry>::iterator
VersioningBlockedSkipListAdjacencyList::get_version_from_chain(forward_list<SizeVersionChainEntry> &chain,
                                                               version_t version) {
  auto i = chain.begin();
  while (i->version > version) {
    i++;
    assert(i != chain.end());  // This should not happen as the last entry in a chain has v == FIRST_VERSION.
  }
  return i;
}
