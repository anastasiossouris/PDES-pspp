#ifndef __AFFINITY_HPP_IS_INCLUDED__
#define __AFFINITY_HPP_IS_INCLUDED__ 1

#include <stdexcept>
#define _GNU_SOURCE
#include <unistd.h>
#include <pthread.h>

namespace concurrent{

	struct affinity{		
		/**
 		 * Set's the affinity of the current thread to the passed core.
 		 *
 		 * \param core The core to which to set the affinity of the current thread
 		 * \throw runtime_error If the affinity cannot be set
		 */		
		void operator()(int core) const{
			(*this)(core, pthread_self());
		}

		/**
		 * Set's the affinity of the thread with the given id to the passed core.
		 *
		 * \param core The core to which to set the affinity of the current thread
		 * \param id The identifier of the thread.
		 * \throw runtime_error If the affinity cannot be set
		 */
		void operator()(int core, pthread_t id){
			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(core, &cpuset);

			if (pthread_setaffinity_np(id, sizeof(cpu_set_t), &cpuset)){
				throw std::runtime_error("call to pthread_setaffinity_np() failed");
			}
		}
	};

} // namespace concurrent

#endif
