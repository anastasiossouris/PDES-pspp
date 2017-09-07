#include <cassert>
#include "task.hpp"
#include "context.hpp"

namespace pdes{

	void task::wait_for(task::time_type t){
		assert(t >= 0);
		assert(ctx != nullptr);

		timestamp += t;

		// this method is executed only when the task is running so we can safely manipulate the local fel of the context
		// associated with this task
		ctx->fel.increase(pool_handle);
	}


	void task::sleep(){
		assert(ctx != nullptr);

		// and we also must remove us from the fel. we can do a pop() here because we know
		// that we are running and thus we are at the start of the fel
		ctx->fel.pop();
	}

	void task::stop(){
		assert(ctx != nullptr);

		// we must remove us from the fel. we can do a pop() here because we know
		// that we are running and thus we are at the start of the fel.
		ctx->fel.pop();

		// re-initialize here
		ctx = nullptr;
	}

	void wakeup(task::time_type t){
		wakeup(t, priority);
	}

	void wakeup(task::time_type t, size_type prio){
		assert(ctx != nullptr);

		// following the usage guidelines we know that we are executing at the local fel
		// and we can safely add the task at the local fel here
		timestamp = t;
		priority = prio;
		pool_handle = ctx->fel.push(this);
	}

} // namespace pdes



