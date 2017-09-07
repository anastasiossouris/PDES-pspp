#ifndef EVENT_POOL_HPP_
#define EVENT_POOL_HPP_

#include "detail/event_pool_generator.hpp"

namespace pdes{

	// forward declaration of task
	class task;

	using event_pool_generator = detail::event_pool_generator<task>;

	using event_pool = typename event_pool_generator::event_pool;
	using event_pool_handle = typename event_pool_generator::event_pool_handle;

} // namespace pdes

#endif /* EVENT_POOL_HPP_ */
