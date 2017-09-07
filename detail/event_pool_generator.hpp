#ifndef EVENT_POOL_GENERATOR_HPP_
#define EVENT_POOL_GENERATOR_HPP_

#include <functional>
#include <boost/heap/fibonacci_heap.hpp>
#include "priority_compare_generator.hpp"

namespace pdes{

	namespace detail{

		/**
		 * Generates the types to use for the event pool structures and the handles for those structures.
		 *
		 * \param Task The type of the task
		 */
		template<class Task>
		struct event_pool_generator{
			//! Heap structure type used for the event-pool structures.
			using event_pool = boost::heap::fibonacci_heap<Task*,
					boost::heap::compare<std::binary_negate<priority_compare_generator<Task>::type> > >;

			//! Type used for indexing into the event_pool type.
			using event_pool_handle = typename event_pool::handle_type;
		};

	} // namespace detail

} // namespace pdes



#endif /* EVENT_POOL_GENERATOR_HPP_ */
