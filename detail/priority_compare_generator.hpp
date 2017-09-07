#ifndef PRIORITY_COMPARE_GENERATOR_HPP_
#define PRIORITY_COMPARE_GENERATOR_HPP_

#include <cassert>
#include <functional>

namespace pdes{

	namespace detail{

		/** \brief Priority comparison between tasks.
		 *
		 * Simulation tasks that are derived from base class task are executed in timestamp order. priority_compare provides the function
		 * to compare tasks according to their timestamp order, so that tasks can be added in ordered containers. To resolve any ties among the
		 * timestamps of two tasks, priority_compare uses lexicographical ordering on the timestamp and local priority of each task.
		 *
		 * That is, task A precedes task B if and only if:
		 * 			timestamp(A) < timestamp(B) || (timestamp(A) == timestamp(B) && priority(A) <= priority(B))
		 */
		template<class Task>
		struct priority_compare : public std::binary_function<Task*, Task*, bool>{ // XXX: Validate here that i passed correct arguments from Mayer

			/**
			 * Compare the tasks pointed to by parameters t1 and t2 according to the lexicographical order of the timestamps
			 * and local priorities for each task.
			 *
			 * \param t1 Pointer to the first task
			 * \param t2 Pointer to the second task
			 * \return True if t1 precedes t2 and false otherwise.
			 */
			bool operator()(const Task* t1, const Task* t2) const{
				assert(t1 != nullptr);
				assert(t2 != nullptr);

				return t1->get_timestamp() < t2->get_timestamp()
								 || (t1->get_timestamp() == t2->get_timestamp()  && t1->get_priority() < t2->get_priority());
			}
		};

		template<class Task>
		struct priority_compare_generator{
			using type = priority_compare<Task>;
		};

	} // namespace detail

} // namespace pdes


#endif /* PRIORITY_COMPARE_GENERATOR_HPP_ */
