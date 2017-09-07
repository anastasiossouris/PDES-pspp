#ifndef CONTEXT_HPP_
#define CONTEXT_HPP_

#include <cassert>
#include <mutex>
#include <boost/intrusive/list_hook.hpp>
#include "event_pool.hpp"
#include "task.hpp"
#include "traits.hpp"

namespace pdes{

	//! Tag used for the base hook in the intrusive list for contexts in the engine.
	struct context_list_base_hook_tag;

	//! Base hook for the intrusive list keeping contexts in the engine.
	using context_list_base_hook = boost::intrusive::list_base_hook<boost::intrusive::tag<context_list_base_hook_tag>,
																		boost::intrusive::link_mode<boost::intrusive::normal_link> >;

	/** \brief A simulation context that keeps simulation tasks.
	 *
	 *	Each context conceptually represents a Logical Process (LP) that has an event-queue with tasks to execute in timestamp order
	 *	and which can receive timestamped event messages from other Logical Processes (that is, contexts).
	 */
	class context : public context_list_base_hook{
	public:
		using time_type = traits::time_type;
		using size_type = traits::size_type;

		/**
		 * Construct a new context with current time _now. The default value of _now is is time_type{}.
		 *
		 * \param _now The simulated time from which to start this context.
		 */
		context(time_type _now = time_type{}) : current_time{_now} {}

		~context(){
			// we must deallocate user simulation tasks. remember that the policy for user tasks is that we use new/delete for them.
			using iterator = event_pool::iterator;

			for (iterator it = fel.begin(); it != fel.end(); ++it){
				delete *it;
			}
		}

		// non-copyable and non-movable
		context(const context&) = delete;
		context& operator=(const context&) = delete;
		context(context&&) = delete;
		context& operator=(context&&) = delete;

		/**
		 * Spawn the simulation task pointed to by simtask to be executed at simulated time t. The priority of the task spawned is
		 * given by parameter prio.
		 *
		 * Tasks must be spawned in a context in increasing timestamp order. That is, if the current simulated time of this context is t, then
		 * a task that is spawned must be such that its timestamp is greater than or equal to t. This method uses runtime assertions to test
		 * this condition, as well as to test whether simtask is a null pointer or not, and does not throw exceptions.
		 *
		 * This method should be called only when the contexts are created at the start of the simulation and the initial tasks are spawned to
		 * them before the engine is started. After that, it can only be called by the engine itself. User tasks should not explicitly call spawn() on any task
		 * while the simulation is executing (due to race conditions that can and will occur). Instead, call method send_message() on the target context
		 * to achieve the same result.
		 *
		 * \param simtask Pointer to the simulation task
		 * \param t Simulated time when the task must be executed
		 * \param prio The local priority of the task spawned.
		 * \throw bad_alloc If memory allocation cannot happen in order to record the task
		 */
		void spawn(task* simtask, time_type t, size_type prio);

		/**
		 * Spawn the simulation task pointed to by simtask to be executed at time now() + delay. The priority of the task spawned is given by
		 * parameter prio.
		 *
		 * The same usage guidelines apply as method spawn().
		 *
		 * \param simtask Pointer to the simulation task
		 * \param delay Delay from the current time after which to execute the simulation task.
		 * \param prio The local priority of the task spawned.
		 * \throw bad_alloc If memory allocation cannot happen in order to record the task
		 */
		void spawn_delayed(task* simtask, time_type delay, size_type prio);

		/**
		 * Spawn the simulation task pointed to by simtask in sleeping state.
		 *
		 * The same usage guidelines apply as method spawn().
		 *
		 * \param simtask Pointer to the simulation task
		 */
		void spawn_sleeping(task* simtask);

		/**
		 * This method is used by any context to send a message to this context. The message is represented by the simulation task pointed to by
		 * simtask which will be executed at time t. The priority of the message is given by parameter prio.
		 *
		 * To avoid race conditions and extra synchronization this method does not check that the timestamp of the message is greater than or
		 * equal to now(). However this condition will be checked when the message will be executed.
		 *
		 * \param simtask Pointer to the simulation task.
		 * \param t The simulated time when the task is to be executed.
		 * \param prio The local priority of the task.
		 * \throw bad_alloc If memory allocation cannot happen in order to record the message
		 */
		void send_message(task* simtask, time_type t, size_type prio);

		/**
		 * Returns the current simulation time of this context. This returned time has one of the following meanings:
		 * 			1. The start simulated time of this context as specified in the constructor call.
		 * 			2. The timestamp of the currently running simulation task.
		 * 			3. The timestamp of the simulation task that was lastly executed.
		 *
		 * That is, this method does not return the timestamp of the task at the top of this context's event-pool.
		 *
		 * This method is intended to be used by the engine. The only guaranteed safe place to call this method in client code is from within
		 * the run() method of the task executing at the top of this context.
		 *
		 * \return The current simulation time of this context.
		 */
		time_type now() const{ return current_time; }

	private:

		friend class task;
		friend class engine;

		time_type current_time; //! Current simulation time
		event_pool fel; //! Future Event List structure storing the tasks of this context
		event_pool messages_fel; //! Future Event List structure storing the messages send to this context during a round
		std::mutex messages_lock; //! Mutex to guard access to messages_fel

		/**
		 * Executes the simulation task at the top of the future event list.
		 */
		void run_top();

		/**
		 * Returns whether there is a task at the top of the main fel or not.
		 *
		 * \return True if the main fel is not empty; otherwise, false is returned.
		 */
		bool top_ready() const{ return !fel.empty(); }

		/**
		 * This method checks if the task at the top of the context event-pool structure has a timestamp less than or equal to
		 * endtime. If yes then true is returned. Otherwise, false is returned (which is the value returned also if no tasks exist
		 * in the main fel).
		 *
		 * \param endtime Maximum simulated time to check for
		 * \return True if the condition holds; otherwise false is returned.
		 */
		bool is_top_task_le_threshold(time_type endtime) const{ return top_ready() && top_task_timestamp() <= endtime; }

		/**
		 * Returns the timestamp of the task on the top of the context. Note that this function assumes that the context is not empty.
		 *
		 * \return The timestamp of the task on the top of the context.
		 */
		time_type top_task_timestamp() const{ return fel.top()->get_timestamp(); }

		/**
		 * Execute simulation tasks from the local future event list of this context until either there are no more
		 * tasks to execute or endtime has been reached in simulated time.
		 *
		 * This method is intended to be executed only by the engine (thus it is private).
		 *
		 * \param endtime Maximum simulated time to reach.
		 */
		void execute_until(time_type endtime);

		/**
		 * This method drains the messages in the messages_fel into the main fel.
		 */
		void drain_messages();
	};

} // namespace pdes


#endif /* CONTEXT_HPP_ */
