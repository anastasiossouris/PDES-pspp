#include <cassert>
#include "context.hpp"


namespace pdes{

		void context::spawn(task* simtask, context::time_type t, context::size_type prio){
			assert(simtask != nullptr);
			assert(t >= now());

			using handle_type = event_pool_handle;

			// we must set the timestamp and the priority of the task before we insert them in the fel
			// so that they get insterted correctly
			simtask->set_timestamp(t);
			simtask->set_priority(prio);

			handle_type pool_handle = fel.push(simtask);

			simtask->set_context(this);
			simtask->set_pool_handle(pool_handle);
		}

		void context::spawn_delayed(task* simtask, context::time_type delay, context::size_type prio){
			assert(simtask != nullptr);

			using handle_type = event_pool_handle;

			// we must set the timestamp and the priority of the task before we insert them in the fel
			// so that they get insterted correctly
			simtask->set_timestamp(now() + delay);
			simtask->set_priority(prio);

			handle_type pool_handle = fel.push(simtask);

			simtask->set_context(this);
			simtask->set_pool_handle(pool_handle);
		}

		void context::spawn_sleeping(task* simtask){
			assert(simtask != nullptr);

			// it is enough to record ourselves as the context for the task
			simtask->set_context(this);
		}

		void context::send_message(task* simtask, context::time_type t, context::size_type prio){
			assert(simtask != nullptr);

			// insert the task in the messages fel
			std::unique_lock<std::mutex> lock{messages_lock};

			using handle_type = event_pool_handle;

			// we must set the timestamp and the priority of the task before we insert them in the messages fel
			// so that they get insterted correctly
			simtask->set_timestamp(t);
			simtask->set_priority(prio);

			handle_type pool_handle = messages_fel.push(simtask);

			simtask->set_context(this);
			simtask->set_pool_handle(pool_handle);

			lock.unlock();
			lock.release();
		}

		void context::run_top(){
			assert(top_ready());

			task* simtask = fel.top();

			assert(simtask != nullptr);
			assert(simtask->get_timestamp() >= now());

			current_time = simtask->get_timestamp();

			simtask->run();
		}

		void context::execute_until(time_type endtime){
			// drain messages
			drain_messages();

			// now execute from the local fel
			while (is_top_task_le_threshold(endtime)){
				run_top();
			}
		}

		void context::drain_messages(){
			std::lock_guard<std::mutex> lk{messages_lock};

			// merge the messages into the main fel
			fel.merge(messages_fel);
		}

} // namespace pdes



