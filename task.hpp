#ifndef TASK_HPP_
#define TASK_HPP_

#include <utility>
#include "event_pool.hpp"
#include "traits.hpp"

namespace pdes{

 	 // forward declaration of class context
	class context;

	/** \brief Base class for simulation tasks.
	 *
	 * Implementation notes: The implementation has been adapted from the original PSPP code.
	 */
	class task{
	public:
		using time_type = traits::time_type; //! Type for the timestamp of the task
		using size_type = traits::size_type; //! Type for the priority of the task

		/**
		 * Constructs a new task
		 *
		 * \param t The timestamp of the constructed task
		 * \param prio The local priority of the task
		 */
		task(time_type t = time_type{}, size_type prio = size_type{}) :timestamp{t}, priority{prio}, ctx{nullptr} {}

		virtual ~task(){}

		// non-copyable
		task(const task&) = delete;
		task& operator=(const task&) = delete;

		// movable

		//! Move constructor
		task(task&& other) : timestamp(other.timestamp), priority(other.priority), ctx(other.ctx), pool_handle(std::move(other.pool_handle)){
			other.ctx = nullptr;
		}

		//! Move assignment operator
		task& operator=(task&& other){
			if (this != &other){
				timestamp = other.timestamp;
				priority = other.priority;
				ctx = other.ctx;
				pool_handle = std::move(other.pool_handle);
				other.ctx = nullptr;
			}
			return *this;
		}

		/**
		 * This method must be overriden by concrete simulation tasks. This method is called each time the task executes.
		 */
		virtual void run() = 0;

		/**
		 * Assigns prio as the local priority of this task.
		 *
		 * \param prio The local priority of this task.
		 */
		void set_priority(size_type prio){ priority = prio; }

		/**
		 * Returns the local priority of this task.
		 *
		 * \return The local priority of this task.
		 */
		size_type get_priority() const{ return priority; }

		/**
		 * Returns the timestamp of this task; that is, the simulated time at which this task will execute.
		 *
		 * \return When this task will execute in simulated time.
		 */
		time_type get_timestamp() const{ return timestamp; }

		/**
		 * Make the task BUSY for a duration t >= 0.
		 *
		 * This method should only be called while this task executes (in the run() method).
		 *
		 * \param t Duration during which this task must be kept busy.
		 */
		void wait_for(time_type t);

		/**
		 * Put the task to sleeping mode.
		 *
		 * This method should only be called while this task executes (in the run() method).
		 */
		void sleep();

		/**
		 * Initialize the task to the INIT state.
		 *
		 * This method should only be called while this task executes (in the run() method).
		 */
		void stop();

		/**
		 * Wakes a sleeping task. If the task is in SLEEPING mode then it is awaked and executed at time t with the
		 * current local priority of this task.
		 *
		 * \param t Simulated time when this task is awaken.
		 * \throw bad_alloc If there is no enough memory to add the task
		 */
		void wakeup(time_type t);

		/**
		 * Wakes a seeping task. If the task is in SLEEPING mode then it is awaked and executed at time t with prio as
		 * its priority.
		 *
		 * \param t Simulated time when this task is awaken
		 * \param prio The priority of this task
		 * \throw bad_alloc If there is no enough memory to add the task
		 */
		void wakeup(time_type t, size_type prio);
	private:
		friend class context;

		time_type timestamp; //! Simulated time when this task is to be executed
		size_type priority; //! Local priority of this task.
		context* ctx; //! The context where this task is spawned.
		event_pool_handle pool_handle; //! Position of this task in the event-pool

		/**
		 * Assigns the simulated time when this task must execute.
		 *
		 * \param t Simulated time when this task must execute.
		 */
		void set_timestamp(time_type t){ timestamp = t; }

		/**
		 * Assigns the pointer to the context where this task has been spawned.
		 *
		 * \param p Pointer to the context where this task is spawned.
		 */
		void set_context(context* p){
			assert(p != nullptr);
			ctx = p;
		}

		/**
		 * Assigns the pool handle for this task (its place in the event-pool).
		 *
		 * \param ph Handle for this task in the event pool.
		 */
		void set_pool_handle(event_pool_handle ph){
			pool_handle = ph;
		}

		/**
		 * Returns a pointer to the event-pool structure where this task is spawned.
		 *
		 * \return Pointer to the event-pool structure where this task is spawned.
		 */
		context* get_context() const{ return ctx; }
	};

} // namespace pdes


#endif /* TASK_HPP_ */
