#ifndef CACHE_ALIGNED_ALLOCATOR_HPP_
#define CACHE_ALIGNED_ALLOCATOR_HPP_

#include <tbb/cache_aligned_allocator.h>

namespace concurrent{

	/**
	 * An allocator that allocates memory on cache line boundaries for the purpose of avoid false-sharing. It can be used as a direct
	 * replacement of std::allocator<T>.
	 *
	 * Currently, this is a wrapper for the cache_aligned_allocator provided by TBB, so refer to http://www.threadingbuildingblocks.org/docs/help/reference/memory_allocation/cache_aligned_allocator_cls.htm
	 * for more details.
	 */
	template<typename T>
	using cache_aligned_allocator = tbb::cache_aligned_allocator<T>;

} // namespace concurrent


#endif /* CACHE_ALIGNED_ALLOCATOR_HPP_ */
