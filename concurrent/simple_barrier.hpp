#ifndef SIMPLE_BARRIER_HPP_
#define SIMPLE_BARRIER_HPP_

#include <cstddef>
#include <stdexcept>
#include <mutex>
#include <condition_variable>

namespace concurrent{

	template<class SizeType = std::size_t>
	class simple_barrier{
	public:
		using size_type = SizeType;

		simple_barrier(size_type n) : expected{n}, arrived{0}, generation{0} {
			if (n == 0){
				throw std::invalid_argument("invalid argument to simple_barrier constructor");
			}
		}

		// non-copyable
		simple_barrier(const simple_barrier&) = delete;
		simple_barrier& operator=(const simple_barrier&) = delete;

		// movable
		simple_barrier(simple_barrier&&) = default;
		simple_barrier& operator=(simple_barrier&&) = default;

		void await(){
			std::unique_lock<std::mutex> lk{lock};

			size_type current_generation = generation;
			++arrived;

			if (arrived == expected){
				// initialize for next round
				++generation;
				arrived = 0;
				lk.unlock();
				lk.release();
				cond.notify_all();
			}
			else{
				// wait for the condition
				while (current_generation == generation){
					cond.wait(lock);
				}
			}
		}
	private:
		size_type expected;
		size_type arrived;
		size_type generation;
		std::mutex lock;
		std::condition_variable cond;
	};

} // namespace concurrent


#endif /* SIMPLE_BARRIER_HPP_ */
