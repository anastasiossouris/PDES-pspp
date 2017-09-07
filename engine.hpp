#ifndef CONTEXT_HPP_
#define CONTEXT_HPP_

#include <cassert>
#include <algorithm>
#include <vector>
#include <memory>
#include <iterator>
#include <functional>
#include <exception>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "concurrent/simple_barrier.hpp"
#include "concurrent/tournament_tree.hpp"
#include "concurrent/termination_detection_barrier.hpp"
#include "concurrent/cache_aligned_allocator.hpp"
#include "concurrent/affinity.hpp"
#include "boost/intrusive/list.hpp"
#include "context.hpp"
#include "traits.hpp"
#include "parameters.hpp"
#include "util/xorshift.hpp"

namespace pdes{

	class engine{
	public:
		using time_type = traits::time_type;
		using size_type = traits::size_type;

	private:
		using barrier_type = concurrent::simple_barrier<size_type>;
		using termination_detection_type = concurrent::termination_detection_barrier<size_type>;
		using tournament_tree_type = concurrent::tournament_tree<time_type>;

	public:

		/**
		 * Class engine has only one instance and following the Singleton Pattern this method provides access to that single instance.
		 * Note that this method is not thread-safe.
		 *
		 * \return Pointer to the singleton instance of class engine.
		 * \throw bad_alloc If the singleton instance cannot be allocated the first time.
		 */
		static engine* get_instance(){
			if (!_instance){
				_instance = new engine{};
			}
			return _instance;
		}

		// non-copyable and non-movable
		engine(const engine&) = delete;
		engine& operator=(const engine&) = delete;
		engine(engine&&) = delete;
		engine& operator=(engine&&) = delete;

		void init(parameters& params){
			// First we must initialize the data for the worker threads
			lookahead = params.get_lookahead();
			endtime = params.get_endtime();
			num_threads = params.get_num_threads();
			std::vector<size_type> affs = params.get_thread_aff();

			NO_STEAL_REQUEST = num_threads;

			phases_meet_point.reset(new concurrent::sense_barrier<size_type>{num_threads + 1});
			tournament_tree.reset(new concurrent::tournament_tree<time_type>{num_threads});
			td.reset(new concurrent::termination_detection_barrier<size_type>{num_threads});

			// Initialize the steal-free list for each worker thread
			steal_free_list.resize(num_threads);
			for (auto& vec_ctx_l : steal_free_list){
				vec_ctx_l.resize(num_threads);
			}

			// Initialize the deceased, steal_request and steal_response arrays
			std::vector<std::atomic<bool>, concurrent::cache_aligned_allocator<std::atomic<bool> > > __deceased(num_threads);
			std::vector<std::atomic<size_type>, concurrent::cache_aligned_allocator<std::atomic<size_type> > > __steal_request(num_threads);
			std::vector<std::atomic<context_list_type*>, concurrent::cache_aligned_allocator<std::atomic<context_list_type*> > > __steal_response(num_threads);

			using std::swap;

			assert(deceased.size() == 0);
			assert(steal_request.size() == 0);
			assert(steal_response.size() == 0);

			swap(deceased, __deceased);
			swap(steal_request, __steal_request);
			swap(steal_response, __steal_response);

			for (auto& x : deceased){
				x.store(false);
			}
			for (auto& x : steal_request){
				x.store(NO_STEAL_REQUEST);
			}
			for (auto& x : steal_response){
				x.store(&NO_STEAL_RESPONSE); // this is no needed but...
			}

			// initialize the exceptions
			exceptions.resize(num_threads);

			// we initialize start_simulation flag also here
			start_simulation.store(false, std::memory_order_seq_cst);

			// initialize the phases data. these must be done while holding the lock for memory visibility effects
			// also note that since all threads will acquire the init_phase_lock they will see the changes we make above
			{
				std::unique_lock<std::mutex> lk{init_phase_lock};

				init_phase_done = false;
			}
			{
				std::unique_lock<std::mutex> lk{context_distribution_phase_lock};

				contexts_distributed_done = false;
			}
			{
				std::unique_lock<std::mutex> lk{cleanup_phase_lock};

				cleanup_phase_begin = false;
			}

			// create the threads
			concurrent::affinity aff_setter;
			threads.reserve(num_threads);
			for (size_type i = 0; i < num_threads; ++i){
				size_type id = i;
				size_type core = aff[i];
				std::thread t = std::thread(&thread_pool::worker_thread,
						this,
						id
				);
				threads.push_back(std::move(t));
				// set the affinity for this created thread
				try{
					aff_setter(core, t.native_handle());
				}
				catch(...){
					// tell alive threads to terminate and then wait for them
					{
						std::unique_lock<std::mutex> lk{init_phase_lock};

						init_phase_done = true;
						init_phase_ok = false;
					}
					init_phase_done_condition.notify_all();

					for (size_type j = 0; j < i; ++j){
						threads[j].join();
					}

					throw ;
				}
			}

			// inform the threads that initialization went fine
			{
				std::unique_lock<std::mutex> lk{init_phase_lock};

				init_phase_done = true;
				init_phase_ok = true;
			}
			init_phase_done_condition.notify_all();
		}

		/**
		 * Distribute the simulation contexts. The engine expects pointers to the contexts
		 * so the iterators must be iterators to pointers to contexts.
		 *
		 * This operation can be called only once before the start() method.
		 *
		 * The container from which the iterators are passed here can and should be freed by the client code
		 * after this method returns.
		 */
		template<class InputIt>
		void distribute_contexts(InputIt begin_context, InputIt end_context){
			using iterator_type = InputIt;

			// how many contexts do we have?
			size_type ncontexts = std::distance(begin_context, end_context);
			// roughly how many contexts each worker thread will get
			size_type range = ncontexts/num_threads;

			// distribute the contexts to the threads
			for (size_type i = 0; i < num_threads; ++i){
				// find the range of contexts for the i-th worker thread
				iterator_type first = begin_context;
				std::advance(first, i*range);
				iterator_type last = end_context;
				if (i != num_threads - 1){
					last = begin_context;
					std::advance(last, (i+1)*range);
				}
				// give the contexts to the i-th worker thread
				for (iterator_type context = first; context < last; ++context){
					local_contexts_all[i].push_front(*context);
				}
			}

			// give the signal that the contexts have been distributed
			{
				// also note that this lock ensures that the worker threads will see correctly the contexts at start
				std::unique_lock<std::mutex> lk{context_distribution_phase_lock};

				contexts_distributed_done = true;
			}
			context_distribution_phase_start_condition.notify_all();

			// wait for the threads to collect their contexts
			phases_meet_point->await(num_threads);

			// we no longer need the local_contexts_all vector so we clean it up
			for (size_type i = 0; i < num_threads; ++i){
				local_contexts_all[i].clear();
			}
			local_contexts_all.clear();
		}

		void start(){
			start_simulation.store(true, std::memory_order_seq_cst);
		}

		std::vector<std::exception_ptr> await(){
			// wait for the worker threads to finish the simulation
			phases_meet_point->await(num_threads); // we get the last id

			return exceptions;
		}

		void cleanup(){
			{
				std::unique_lock<std::mutex> lk{cleanup_phase_lock};

				cleanup_phase_begin = true;
			}
			cleanup_phase_start_condition.notify_all();

			// wait for every worker thread to finish their cleanup
			phases_meet_point->await(num_threads);

			// now wait for the threads instances to finish
			std::for_each(threads.begin(), threads.end(), std::mem_fn(&std::thread::join));

			// we can now deallocate the internal data
			phases_meet_point.reset();
			tournament_tree.reset();
			td.reset();

			deceased.clear();
			steal_request.clear();
			steal_response.clear();
			for (size_type i = 0; i < num_threads; ++i){
				for (size_type j = 0; j < num_threads; ++j){
					streal_free_list[i][j].clear();
				}
				steal_free_list[i].clear();
			}
			steal_free_list.clear();
			exceptions.clear();
		}


	private:
		//! Private constructor because engine is a singleton class
		engine(){}

		// the singleton instance of class engine
		static engine* _instance;

		time_type lookahead; //! Simulation's lookahead parameter
		time_type endtime; //! Simulation's endtime parameter
		size_type num_threads; //! Number of worker threads
		std::vector<std::thread> threads; //! Worker threads

		/**
		 * During the initialization the thread calling init() creates the worker threads and sets the affinities for each one.
		 * Because something may go wrong, like a thread could not be created or the affinity could not be set for one of the threads,
		 * we could have a situation where some threads have been created and some not and the main thread needs to exit. In that case we need to
		 * notify the alive threads to stop. To do that we follow the following simple protocol:
		 * 	Each thread that starts waits until the initialization phase ends using a condition variable init_phase_done_condition.
		 * 	The main thread executing init() in case of success (all threads have been started and the affinities have been set successfully)
		 * 	sets to the variable init_phase_ok the value true and then signals the init_phase_done_condition condition variable. If something goes
		 * 	wrong then it sets false to init_phase_ok. In this way, after the alive threads get notified from the init_phase_done_condition they check
		 * 	the value of init_phase_ok to know whether they can proceed or not. To guard the thread from spurious returns from the init_phase_done_condition
		 * 	condition variable await() method we use a init_phase_done that is initialized to false and is set to true before the
		 * 	condition is signaled by the main thread running init(). Since each condition variable must be accompanied by a lock we use a
		 * 	mutex variable called init_phase_lock.
		 */
		bool init_phase_ok;
		bool init_phase_done;
		std::mutex init_phase_lock;
		std::condition_variable init_phase_done_condition;

		/**
		 * After the init phase has been successfully completed, the worker threads wait for the contexts to be distributed and to collect them.
		 * We use condition variables for this as well. context_distribution_phase_start_condition is a condition variable that signals when
		 * then worker threads must collect the contexts. To guard from spurious wakeups we use a boolean flag contexts_distributed_done that
		 * is set to true by the thread running init() before signaling the condition variable, and we use a context_distribution_phase_lock
		 * as a lock for the condition variable.
		 */
		bool contexts_distributed_done;
		std::mutex context_distribution_phase_lock;
		std::condition_variable context_distribution_phase_start_condition;

		/**
		 * After the contexts have been distributed the worker threads must wait for the start() signal. We use a atomic flag for this to avoid
		 * the waiting period introduced by condition variables because we want all worker threads to start at about the same time.
		 */
		std::atomic<bool> start_simulation;

		/**
		 * After the simulation has ended the worker threads must wait for the cleanup() signal. We use again condition variables here.
		 * cleanup_phase_start_condition is the condition variable. cleanup_phase_begin is a flag to guard from spurious wakeups and
		 * cleanup_phase_lock is the lock associated with the condition variable.
		 */
		bool cleanup_phase_begin;
		std::mutex cleanup_phase_lock;
		std::condition_variable cleanup_phase_start_condition;

		/**
		 * After the threads have collected the contexts they must notify the main thread (that called distributed_contexts()). We use a barrier
		 * for this. That is, after the main thread has signaled the contexts_distribution_phase_start_condition, it then waits on the barrier.
		 * When the worker threads collect their contexts they meet at the barrier.  We call this barrier phases_meet_point.
		 *
		 * We use again this barrier when the main thread needs to wait for the simulation to end in the await() method. After each thread determines
		 * simulation's termination it meets at phases_meet_point barrier and then proceeds on to the cleanup phase. It is also used when the cleanup
		 * phase is over.
		 */
		std::unique_ptr<barrier_type> phases_meet_point;

		/**
		 * This is the tournament tree for the implementation of a global min-reduction operation.
		 * It is used in compute_threshold() function by the worker threads in order to compute the threshold
		 * for the next round after they have computed their local thresholds.
		 */
		std::unique_ptr<tournament_tree_type> tournament_tree;

		/**
		 * For the worker threads to know when a round is terminated we use a termination detection mechanism.
		 *
		 * When a thread has work to do it says so by calling set_active() on the termination detection object.
		 * When a thread doesn't have work to do it says so by calling set_inactive() on the termination detection object.
		 * A method td.terminate() returns true if no threads has any more work to do and thus the work-stealing computation
		 * for the round terminates.
		 */
		std::unique_ptr<termination_detection_type> td;

		/**
		 * This part regards how do the worker threads compute the threshold for the next round.
		 * Consider we are at round i. Each worker during the i-th round keeps track of two variables: context_threshold and event_threshold
		 *
		 * How is context_threshold computed:
		 *		Say that at round i the threshold is threshold(i). Worker thread WTk will execute all some contexts conservatively up to time
		 *		threshold(i). For each such context let next_event be the first event with timestamp later than theshold(i) (this is where the worker
		 *		thread stops executing this context). context_threshold holds the minimum of the timestamps of those next_event events for all contexts.
		 *
		 * How is event_threshold computed:
		 *		Say that a worker thread WTk executes a context and that context generates an event for another context using the delayed() methods.
		 *		event_threshold is the minimum enactment time of the messages generated by contexts executed by WTk.
		 *
		 * At the end of round i, WTk computes its threshold as min{context_threshold, event_threshold} + lookahead and uses this value
		 * for the global min reduction operation.
		 *
		 * Since these variables are local to each thread, we declare them thread_local.
		 */
		thread_local static time_type context_threshold;
		thread_local static time_type event_threshold;

		/**
		 * Each worker thread has local a set of simulation contexts to work with named local_contexts.
		 * Since for the work-stealing implementation we will need to perform delete and splice operations we do not use a vector
		 * but a list. Each simulation context that a worker thread executes, is placed in a second list denoted committed_contexts.
		 * To preserve some memory we do not use a list storing pointers to the simulation contexts, but instead we use a intrusive list.
		 * The intrusive list has added locality benefits as well.
		 */
		using context_list_type = boost::intrusive::make_list<context, boost::intrusive::constant_time_size<false>,
							boost::intrusive::base_hook<context_list_base_hook> >::type;
		using context_list_iterator_type = typename context_list_type::iterator;

		/**
		 * Since each local_contexts and committed_contexts are local to each worker thread we use thread_local
		 * storage for them.
		 */
		thread_local static context_list_type local_contexts;
		thread_local static context_list_type committed_contexts;

		/**
		 * This is used to distribute the contexts. At initialization we place the contexts for worker thread id in local_contexts_all[id].
		 * Then, worker thread id will drain them into its local contexts set.
		 */
		std::vector<context_list_type> local_contexts_all;

		/**
		 * Work-Stealing Implementation details:
		 *
		 * We use the receiver-initiated algorithm as described in 'Scheduling Parallel Programs by Work-Stealing
		 * with Private Deques'. For this we use for each worker thread Wti the following registers:
		 * 		+ deceased: A SWMR boolean register indicating whether Wti is deceased or
		 *		not.
		 *		+ steal_request: An atomic register that can be accessed using the
		 *		compare&swap() primitive. It is used by the thief worker threads when they
		 *		choose Wti as their victim. In particular, when steal_request i is NO_STEAL_REQUEST then no
		 *		worker thread has made a steal request to Wti. If another worker thread Wtj
		 *		wants to make a steal request to Wti then it will try to atomically write its
		 *		identity j to steal_request i if it is still NO_STEAL_REQUEST using the compare&swap() primitive.
		 *		+ steal_response: A MWSR that can be read only by Wti and written by any
		 *		other worker thread. Its purpose is for the victim worker thread to pass the simulation contexts
		 *		to Wti when Wti has successfully made a steal request to the
		 *		victim.Before making a steal request, Wti makes steal_response i
		 *		NO_STEAL_RESPONSE and then makes a steal request to Wtj (until successful). Then it local
		 *		spins to steal_response i until Wtj passes the response (which could either a null pointer or a pointer to some contexts).
		 *
		 * Since those must be visible to all worker threads they are not thread local and we use plain vectors
		 * to store them.
		 *
		 * Also, note that in order to avoid false-sharing we use a cache-alignment for these variables. This is critical since they are
		 * read frequently by the thief/victim threads.
		 */
		std::vector<std::atomic<bool>, concurrent::cache_aligned_allocator<std::atomic<bool> > > deceased;
		std::vector<std::atomic<size_type>, concurrent::cache_aligned_allocator<std::atomic<size_type> > > steal_request;
		std::vector<std::atomic<context_list_type*>, concurrent::cache_aligned_allocator<std::atomic<context_list_type*> > > steal_response;
																						// we pass a list so as *not* to restrict
																						// the number of contexts that can be stolen
																						// e.g. for the steal-half approach we will pass
																						// a list with many contexts.
																						// Note that it is important here to have a pointer
																						// to a context list to have a 'guarantee' that
																						// the variable will be lock-free

		/**
		 * A sentinel node that is used by worker threads as a steal_response holder until they get a response.
		 */
		context_list_type NO_STEAL_RESPONSE;

		/**
		 * A value to distinguish whether there is a steal-request by some worker thread in some steal_request[] atomic variable.
		 * Since worker threads have identifiers in the range [0,num_threads) then we can use num_threads as an 'invalid' thread id.
		 * Note that -1 is not *conceptually* right since size_type most likely is a unsigned integer type.
		 */
		size_type NO_STEAL_REQUEST;

		/**
		 * When a thread worker steals from another it gets a context list from the victim's steal_response variable.
		 * In this case to avoid the need for the victim to allocate a new context list to pass to the thief (which
		 * later the thief would have to deallocate) we use the following malloc-free mechanism:
		 *
		 * Each worker thread has a pre-allocated context list for each other worker-thread. When a thief i steals
		 * from victim j, victim j will pass to thief i the context list from the pre-allocated pool. Also, to completely
		 * avoid mallocs, the victim will use a splice operation from its local context list to the pre-allocated context list
		 * to send to the thief.
		 *
		 * In more detail, steal_free_list[victim][thief] is the list used by the victim to pass contexts to the thief worker thread.
		 *
		 * In place those in cache-line boundaries as well, because we do not want steal-handoffs from 2 threads (thief/victim) to get in the way
		 * of other unrelated threads due to false-sharing.
		 */
		std::vector<std::vector<context_list_type, concurrent::cache_aligned_allocator<context_list_type> >,
						concurrent::cache_aligned_allocator<std::vector<context_list_type> > > steal_free_list;


		/**
		 * To steal each thread must use a random number generator. Each one is local to a thread and thus declared thread local.
		 */
		thread_local static std::random_device rd;
		thread_local static util::xorshift gen;
		thread_local static std::uniform_int_distribution<> dis;

		/**
		 * This is used by the worker threads to notify us of exceptions.
		 */
		std::vector<std::exception_ptr> exceptions;

		/**
		 * This is the task run by each worker thread in the simulation.
		 */
		void worker_thread(
				size_type id /** the id of the 'this' worker thread */
				){
			// First initialize the random number generators
			using result_type = std::random_device::result_type;

			result_type random_seed{};

			while (!(random_seed = rd())){} // zero seeds do not work well with xorshift

			gen.seed(random_seed);

			using param_type = std::uniform_int_distribution<>::param_type;

			param_type range{0, num_threads-1};
			dis.param(range);

			// Wait for the initialization phase to end and determine whether to proceed to the contexts distribution phase or not
			bool proceed{false};

			{
				std::unique_lock<std::mutex> lk{init_phase_lock};

				while (!init_phase_done){
					init_phase_done_condition.wait(lk);
				}

				proceed = init_phase_ok;
			}

			if (!proceed){ return; }

			// Wait for the contexts distribution phase to begin
			{
				std::unique_lock<std::mutex> lk{context_distribution_phase_lock};

				while (!contexts_distributed_done){
					context_distribution_phase_start_condition.wait(lk);
				}
			}

			// We can now get our starting contexts. Since at each round we swap the committed contexts with the local contexts
			// and start with the local contexts, we initially place our contexts in the committed_contexts list.
			committed_contexts.splice(committed_contexts.begin(), local_contexts_all[id]);

			// Meet at the barrier to notify that context distribution phase is over
			phases_meet_point->await(id);

			// Wait for the signal to start the simulation
			while (!start_simulation.load(std::memory_order_seq_cst)){}

			// Execute the simulation

			// initially we are bounded by the lookahead so we let event_threshold be endtime
			// and context_threshold be zero.
			context_threshold = 0;
			event_threshold = endtime;

			run_simulation(id);

			// Notify the main thread that the simulation has been terminated
			phases_meet_point->await(id);

			// Wait for the cleanup phase to begin
			{
				std::unique_lock<std::mutex> lk{cleanup_phase_lock};

				while (!cleanup_phase_begin){
					cleanup_phase_start_condition.wait(lk);
				}
			}

			// Perform local cleanup here
			local_cleanup();

			// Notify the main thread that the cleanup phase is over
			phases_meet_point->await(id);
		}

		/**
		 * This is the top level function that implements the simulation for a worker thread with identifier id.
		 */
		void run_simulation(
				size_type id /** worker thread's identifier */
				){
			bool no_exceptions = true; // needed to handle exceptions case

			// Until the simulation's end time
			while (true){
				// Compute theshold for the current round
				// If an exception was raised then we need to inform all worker threads. One simple thing we can do for that is
				// to use 0 as our value in the tournament tree. 0 can be used since we know that it cannot be used otherwise
				// (due to + lookahead and the fact that lookahead is nonzero)
				time_type threshold = no_exceptions ? compute_threshold(id) : 0;

				// Stop simulation if we reached the simulation's end time or got an exception
				if (threshold >= endtime || threshold == 0){ break; }

				// In this round we will compute context_threshold and event_threshold using minimum operations
				// and so we do not want to be restricted by the previous values of those variables. We initialize them
				// to the maximum possible value which is endtime.
				context_threshold = endtime;
				event_threshold = endtime;

				// Start executing the contexts using work-stealing
				no_exceptions = run_work_stealing_scheduler(id, threshold);
			}
		}

		/**
		 * Each worker thread calls run_work_stealing_scheduler() after they have computed the threshold time,
		 * to execute all the contexts.
		 *
		 * The idea behind the algorithm is that the worker thread begins by executing the contexts from its own local_contexts set
		 * and responds to steal-requests in the meantime. When its local_contexts set gets empty it becomes a thief and tries to steal
		 * some contexts from a victim worker thread. When it finally receives some contexts as a response from a victim worker thread,
		 * it adds them to its local_contexts set and starts over again.
		 *
		 * In the meantime, it uses the termination detection object td to know if the work-stealing computation for the round has ended
		 * or not.
		 *
		 * This method returns true if everything worked fine and false if an exception was raised.
		 */
		bool run_work_stealing_scheduler(
				size_type id, /** worker thread's id */
				time_type threshold /** the threshold for the current round */
				){
			// We start with the contexts we executed in the previous round (which are stored in committed_contexts).
			// This splice operation is O(1) and does not use memory operations.
			local_contexts.splice(local_contexts.begin(), committed_contexts);

			// Notify all that we are alive and we have possibly contexts to give (so that others can
			// make us steal requests)
			report_alive(id);
			td->set_active(id);

			// Start with the first context from our local_contexts set
			context_list_iterator_type ctx = local_contexts.begin();

			// While the work-stealing computation for this round is not over
			while (true){
				// While local_contexts set is not empty
				while (ctx != local_contexts.end()){
					// add the context to execute in the committed set
					// Note that the context must be spliced at the committed_contexts before we call handle_steal_request
					// because handle_steal_request will check the same local_contexts set and the first context that we are about to
					// execute next must be removed from there first.
					//
					// Note also that we add on the beginning of the committed_contexts list and not at the end, because we want to benefit
					// from cache locality. Notice that the contexts we execute last here in round R will be placed at the beginning of
					// committed_contexts, and thus in round R+1 they will be the first to execute from local_contexts (after we splice
					// committed_contexts to local_contexts). Thus, there is a high chance that the data for these tasks reside in our cache.
					//
					// This splice operation is O(1) and does not use memory operations.
					committed_contexts.splice(committed_contexts.begin(), local_contexts, local_contexts.begin());

					// Check if we have a pending steal request from a thief
					// XXX: add a period handling mechanism here
					handle_steal_request(id);

					// execute the context by our own
					context* current_context = *committed_contexts.begin();

					try{
						current_context->execute_until(threshold);
					}
					catch(...){
						// this is the only place where an exception may be thrown. we must gracefully shut down here
						handle_exception(id, std::current_exception());

						return false;
					}

					// keep track of the time of the event exceeding the threshold to update context_threshold
					context_threshold = std::min(context_threshold, current_context->top_ready() ? current_context->top_task_timestamp() : endtime);

					// retrieve next context
					ctx = local_contexts.begin();
				}

				assert(local_contexts.empty());

				// In this point, the worker thread exhausted its local_contexts set and now it becomes a thief.
				// Before however it tries to steal from some victim thread, it must:
				//		(1) Report deceased so that other worker threads do not make futile steal attempts to this thread
				//		(2) Notify the termination detection mechanism (this enables threads to know when to stop)
				//		(3) Block steal requests. Even though we report deceased this is also needed because later then this thread
				//		tries to steal it will not have to continuously check for steal requests that can happen because those threads
				//		read deceased[id] before report_deceased(id) is called and thus assume that worker thread id is still alive.
				report_deceased(id);
				td->set_inactive(id);
				block_steal_requests(id);

				// Attempt steals
				while (true){
					if (attempt_steal(id)){ break; }

					// Check if it is time to end the current round
					if (td->terminate()){
						// This is needed because at the next round other threads must be able to make steal requests to us
						unblock_steal_requests(id);
						return true;
					}
				}

				// Since we got contexts now we can give them back if requested and thus re-enable steal attempts
				report_alive(id);
				unblock_steal_requests(id);

				// Retrieve next context
				ctx = local_contexts.begin();

				assert(!local_contexts.empty());
			}
		}

		/**
		 * This function is called by a thief worker thread to make a steal-attempt to some victim worker thread.
		 *
		 * If the steal-attempt is successful then true is returned; otherwise, false is returned.
		 */
		bool attempt_steal(
				size_type thief /** thief's identifier */
				){
			// Choose a victim to make a steal-attempt
			size_type victim = choose_victim(thief);

			assert(0 <= victim && victim < num_threads && victim != thief);

			// Make the steal attempt only if the thread is still alive. This is good for performance (we may get rid of some
			// unsuccessful compare&swap() operation because the deceased victim thread has called block_steal_requests()) but it
			// is also essential for the termination of the run_work_stealing_scheduler() operation at each simulation round. That is
			// because then the thief worker thread will not call td->set_active(thief) and the termination detection object will eventually
			// report termination (consider the scenario that all threads synchronously call set_active() see that no one is alive then
			// some of the threads call set_inactive() but on the td they do not detect termination because some threads are still active and thus
			// they try to steal again~ here we must prevent them from stealing again).
			if (!deceased[victim].load(std::memory_order_seq_cst)){
				// Before we make a steal attempt cancel out any response from a previous steal attempt
				steal_response[thief].store(&NO_STEAL_RESPONSE, std::memory_order_seq_cst);

				// Before we make a steal-attempt we declare active. Suppose we weren't, and we did after we had a successful steal request.
				// Consider the following scenario:
				// 		(1) All are inactive except from the victim worker thread we choose
				//		(2) The victim worker threads responds and later reports inactive.
				//		(3) Now td will report termination and all other threads will proceed to the threshold computation
				//		(4) We now wake up get the response from the victim and we proceed on our own.
				//
				// This is not bad for correctness and not necessary for the steal-one approach (because we stole only one task) but it is
				// necessary for the steal-half approach since we stole many tasks and can benefit from other worker threads being alive
				// and taking some of them from us.
				td->set_active(thief);

				size_type expected = NO_STEAL_REQUEST;

				// Attempt a steal to the victim worker thread
				if (steal_request[victim].compare_exchange_strong(expected, thief, std::memory_order_seq_cst, std::memory_order_seq_cst)){
					// Successful steal attempt so we wait for the victim to respond
					bool success = wait_steal_response(thief, victim);

					if (success){
						context_list_type* stolen_list = steal_response[thief].load(std::memory_order_seq_cst);

						local_contexts.splice(local_contexts.begin(), *stolen_list);

						return true;
					}
				}

				// A failed steal attempt so we are not active anymore
				td->set_inactive(thief);
			}

			return false;
		}

		/**
		 * This function is called by a thief worker thread to wait for a response to its successful steal-request to the
		 * victim worker thread.
		 *
		 * Returns true if we got a response and false otherwise. Note that false is returned in the case we get nullptr as response.
		 */
		bool wait_steal_response(
				size_type thief, /** thief's identifier */
				size_type victim /** victim's identifier */
				){
			context_list_type* response = nullptr;

			while ((response = steal_response[thief].load(std::memory_order_seq_cst)) == &NO_STEAL_RESPONSE){
				// Check if the victim has deceased in the meantime
				if (deceased[victim].load(std::memory_order_seq_cst)){
					// We have two cases to consider here:
					// 		(1) If the victim reports deceased and then calls unblock_steal_requests(), it might be the case
					//		that we read deceased[victim] before it called report_deceased() and thus then attempt a steal, but made
					//		the steal after the victim has called unblock_steal_requests() and later terminated. In this case, we must
					//		reset steal_request[victim] for the next round. In this case a atomic write would suffice.
					//		(2) We read deceased[victim] false and make a succesful steal attempt. Then the victim thread reports
					//		deceased and calls block_steal_requests(). We read deceased[id] true but now the victim thread will change
					//		steal_request using a compare&swap operation and we cannot do a atomic write here. Consider the following scenario:
					//			(a) The victim thread in the block_steal_requests() makes a failed compare&swap() operation because
					//			we currently have a active steal requets. It responds to us with null.
					//			(b) The victim thread writes steal_request[victim] = victim so as to block the steal_requests.
					//			(c) The thief heres writes steal_request[victim] = NO_STEAL_REQUEST. Thus it cancels the block_steal_requests()
					//			operation done by the victim.
					//		What we truly want here is to cancel our request only if we are still the active requester so we use a compare&swap().
					steal_request[victim].compare_exchange_strong(thief, NO_STEAL_REQUEST,std::memory_order_seq_cst, std::memory_order_seq_cst);

					return false;
				}
			}

			return response != nullptr;
		}

		/**
		 * A worker thread calls report_alive() to declare that it has work to do.
		 */
		void report_alive(
				size_type id /** worker thread's identifier */
				){
			deceased[id].store(false, std::memory_order_seq_cst);
		}

		/**
		 * A worker thread calls report_deceased() to declare that it hasn't work to do.
		 */
		void report_deceased(
				size_type id /** worker thread's identifier */
				){
			deceased[id].store(true, std::memory_order_seq_cst);
		}


		/**
		 * A worker thread calls block_steal_requests() after it exhausts its local_contexts set and before
		 * attempting a steal, to inform others that it does not have any contexts to give and block futile steal requests.
		 */
		void block_steal_requests(
				size_type id /** worker thread's identifier */
				){
			size_type expected = NO_STEAL_REQUEST;

			/**
			 * To block steal requests we store our own id in steal_request[id] cause in this way
			 * every steal attempt will fail in the compare&swap() operation since steal_request[id] != NO_STEAL_REQUEST.
			 *
			 * You must do a compare&swap() however because it may be the case that someone has made us a steal-request.
			 * In that case, we respond with null.
			 */
			if (!steal_request[id].compare_exchange_strong(expected, id, std::memory_order_seq_cst, std::memory_order_seq_cst)){
				size_type thief = epxected; // steal_request[id].load(std::memory_order_seq_cst);
											// note that this load operation is performed by the compare_exchange_strong()
											// operation in case of failure.
				steal_response[thief].store(nullptr, std::memory_order_seq_cst);
				steal_request[id].store(id, std::memory_order_seq_cst);
			}
		}

		/**
		 * A worker thread calls unblock_steal_requests() to re-enable steal attempts to itself.
		 */
		void unblock_steal_requests(
				size_type id /** worker thread's identifier */
				){
			steal_request[id].store(NO_STEAL_REQUEST, std::memory_order_seq_cst);
		}

		/**
		 * A worker thread calls this method to check if there is a pending steal-request to itself and responds accordingly.
		 */
		void handle_steal_request(
				size_type id /** worker thread's identifier */
				){
			size_type thief = steal_request[id].load(std::memory_order_seq_cst);

			if (thief == NO_STEAL_REQUEST){ return ; }

			// Respond to the thief thread
			transfer(id, thief);

			// Re-enable steal requests to us
			unblock_steal_requests(id);
		}

		/**
		 * This method is called by a victim worker thread to respond to a steal-request by a thief worker thread.
		 */
		void transfer(
				size_type victim, /** victim's identifier */
				size_type thief /** thief's identifier */
				){
			if (local_contexts.empty()){
				steal_response[thief].store(nullptr, std::memory_order_seq_cst);
				return ;
			}

			context_list_type* steal_list = &steal_free_list[victim][thief];

			assert(steal_list->empty());

			/**
			 * Regardless of the steal policy used (steal-one or steal-half) we do not pass the contexts from the beginning of the list
			 * but the contexts from the end. The rationale is that the contexts at the beginning of the local_contexts list with some
			 * chance will reside in our cache and we want to execute them.
			 */

			#if defined(USE_STEAL_ONE_APPROACH)
			// steal-one approach
			context_list_iterator_type last = local_contexts.end();
			--last;

			steal_list->splice(steal_list->begin(), local_contexts, last);
			#elif defined(USE_STEAL_HALF_APPROACH)
			// steal-half approach
			context_list_iterator_type first = local_contexts.begin();
			context_list_iterator_type last = local_contexts.end();
			// --last; XXX:: boost documentation http://www.boost.org/doc/libs/1_55_0/doc/html/boost/intrusive/list.html#idp33604904-bb
			// says that in the splice operation below both first and last must point to elements contained in the list. That is, as if the
			// range to be spliced is [first,last]. However, [first,last) is what implemented.
			size_type half = local_contexts.size()/2;

			std::advance(first, half);

			steal_list->splice(steal_list->begin(), local_contexts, first, last);
			#else
			#error "steal approach for the transfer() function not defined"
			#endif

			steal_response[thief].store(steal_list, std::memory_order_seq_cst);
		}

		/**
		 * A thief worker thread calls choose_victim() to choose a victim worker thread.
		 *
		 * This implementation chooses randomly among the threads except from the thief.
		 */
		size_type choose_victim(
				size_type thief /** worker thread thief seeks for a victim */
			) const{
			size_type victim;

			do{
				victim = dis(gen);
			} while (victim == thief);

			return victim;
		}

		/**
		 * This function is called by the worker threads to compute the threshold for the next round.
		 */
		time_type compute_threshold(
				size_type id /** worker-thread's id */
				){
			// Find our local threshold
			time_type local_threshold = std::min(context_threshold, event_threshold) + lookahead;

			// Synchronize with the other worker threads to find the global threshold
			termination_detection_type* td_ptr = td.get();

			return tournament_tree->compete(id, local_threshold,
						[td_ptr](){
							td_ptr->reset(); // reset the termination detection object for the next round
						}
					);
		}

		/**
		 * This method is called as a callback whenever the send_message() method is called on the context class.
		 *
		 * Consider a worker thread WTk that has executed a context. One of the tasks in this context wants to send a message and thus calls the send_message
		 * method. The send_message() method will call on_send_message() which will be executed by worker thread WTk. So WTk now can obtain the timestamp  of the
		 * message and use it to update its event_threshold. This timestamp is passed here as parameter.
		 */
		void on_send_message(time_type timestamp){
			event_threshold = std::min(event_threshold, timestamp);
		}

		/**
		 * This method is called by a worker thread when an exception is caught.
		 */
		void handle_exception(size_type id, std::exception_ptr eptr){
			assert(eptr != nullptr);

			// what we can do here that doesn't disrupt the protocol is to stop ourselves and then wait for everybody else to wait the current
			// round. We then leave the notification of the exception that occurred to the tournament tree phase.

			// make the steps as if we are out of local contexts and any steal attempt fails
			report_deceased(id);
			td->set_inactive(id);
			block_steal_requests(id);

			// store the exception we got for the main thread to know about
			// we rely on the phases barrier for memory visibility
			exceptions[id] = eptr;
		}
	};

} // namespace pdes


#endif /* CONTEXT_HPP_ */
