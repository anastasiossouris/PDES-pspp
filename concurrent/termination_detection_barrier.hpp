/** \file termination_detection_barrier.hpp
 *
 * This file contains the termination_detection_barrier class that implements a Termination Detection Barrier as
 * introduced in section 17.6 of 'The Art of Multiprocessor Programming' by Maurice Herlihy and Nir Shavit.
 *
 * author: Tassos Souris (tassos_souris@yahoo.gr)
 */
#ifndef __TERMINATION_DETECTION_BARRIER_HPP_IS_INCLUDED__
#define __TERMINATION_DETECTION_BARRIER_HPP_IS_INCLUDED__ 1

#include <cstddef>
#include <atomic>

namespace concurrent{

		/**
		 * A termination detection barrier enables a set of n threads to detect when a multi-threaded computation as a whole has
		 * terminated.
		 *
		 * Each thread can be either 'active', meaning that is has work to do, or 'inactive', in which case it has none. Once all threads
		 * have become inactive, then no thread will ever become active again and the multi-threaded computation is terminated.
		 *
		 * A thread declares 'active' using the set_active() function and 'inactive' using the set_inactive() function. The terminate() function
		 * returns true if all threads have declared inactive.
		 *
		 * It is assumed that each participating thread has a unique identifier in the range [0,n), where n is provided in the constructor call.
		 *
		 * Do not use this class to enforce a happens-before relationship among the participating threads (this is the default behavior).
		 * Otherwise, to use sequentially consistent semantics define NO_MEMORY_ORDER_OPTIMIZATION before including this header file.
		 *
		 * Reference: The Art of Multiprocessor Programming section 17.6 'Termination Detecting Barriers'
		 */
		template<class SizeType = std::size_t>
		class termination_detection_barrier{
		public:
			using size_type = SizeType;

			/**
			 * Implementation Notes:
			 *
			 * We use an atomic counter initialized to the number of participating threads n. At the beginning, all threads are inactive.
			 * The following transitions modify the counter as:
			 * 		o inactive --> active : decrement the counter
			 * 		o active --> inactive : increment the counter
			 *
			 * Once all threads have declared inactive (called the set_inactive() operation) then the counter is equal to n (the number of
			 * threads) and the computation is terminated.
			 *
			 * From the above description, it follows that the counter is equal at any time to the number of inactive threads.
			 *
			 * Since the purpose of this object is not to add happens-before relationship among the participating threads
			 * then all operations can be relaxed.
			 */

			/**
			 * Construct a termination_detection_barrier for a multi-threaded computation where n threads are involved.
			 *
			 * Note: this constructor doesn't check the argument n which must be >= 1.
			 *
			 * CAUTION: In order for the termination_detection_barrier to be used correctly the client code must ensure that all participating
			 * threads see the effect of the construction of the object.
			 *
			 * \param n The number of threads expected to use this object.
			 */
			termination_detection_barrier(size_type n) : size{n}, count{n}{}

			// non-copyable and non-movable
			termination_detection_barrier(const termination_detection_barrier&) = delete;
			termination_detection_barrier& operator=(const termination_detection_barrier&) = delete;
			termination_detection_barrier(termination_detection_barrier&&) = delete;
			termination_detection_barrier& operator=(termination_detection_barrier&&) = delete;

			/**
			 * A thread with identifier id calls set_active(id) before it starts looking for work.
			 */
			void set_active(size_type id){
				set(true);
			}

			/**
			 * A thread with identifier id calls set_inactive(id) when it is definitively out of work.
			 */
			void set_inactive(size_type id){
				set(false);			
			}

			/**
			 * Returns true when all treads are inactive.
			 */
			bool terminate() const{
				// Check if the counter (#inactive threads) is equal to the number of participating threads.
				return count.load(
						#ifdef NO_MEMORY_ORDER_OPTIMIZATION
							std::memory_order_seq_cst
						#else
							std::memory_order_relaxed
						#endif
						) == size;
			}

			/**
			 * Resets the state of this termination_detection_object (as if it was constructed again).
			 */
			void reset(){
				count.store(size,
							#ifdef NO_MEMORY_ORDER_OPTIMIZATION
								std::memory_order_seq_cst
							#else
								std::memory_order_relaxed
							#endif
						);
			}

		private:
			size_type size; /** number of threads involved in the computation */
			std::atomic<size_type> count;	/** number of inactive threads */

			/**
			 * Helper function for the set_active() and set_inactive() functions.
			 */
			void set(bool active){
				if (active){
					// reduce the number of inactive threads by 1 (since we are now active)
					count.fetch_sub(1,
								#ifdef NO_MEMORY_ORDER_OPTIMIZATION
									std::memory_order_seq_cst
								#else
									std::memory_order_relaxed
								#endif
							);
				}
				else{
					// increase the number of inactive threads by 1 (since we are now inactive)
					count.fetch_add(1,
								#ifdef NO_MEMORY_ORDER_OPTIMIZATION
									std::memory_order_seq_cst
								#else
									std::memory_order_relaxed
								#endif
							);
				}
			}
		};

} // namespace concurrent

#endif
