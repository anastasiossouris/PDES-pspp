/*
 * thread.hpp
 *
 *  Created on: Jan 5, 2014
 *      Author: Vasilis Samoladas (original code)
 */

#ifndef THREAD_HPP_
#define THREAD_HPP_

#include "task.hpp"

/*! \defgroup SIMT Stackless simulation threads and the SIMT macros
 *
 * To define a stackless simulation thread, you should subclass \c SimThread and define
 * a \c run() function.
 * Inside the definition of the run() function of a SimThread's subclass,
 * you should use the following macros:
 * \li \c #SIMT_BEGIN:  mark the start of the thread's body.
 * \li \c #SIMT_END:  mark the end of the thread's body.
 * \li \c #SIMT_SLEEP: put the thread to sleep
 * \li \c #SIMT_STOP: finish execution of the thread, this is the same as reaching
 * the \c #SIMT_END line.
 * \li \c #SIMT_BUSY(t): pause execution until simulation time advances by \c t steps
 * (to \c now()+t).
 * \li \c #SIMT_SUB: define a subroutine inside \c run().
 * \li \c #SIMT_CALL: call a subroutne defined inside \c run().
 * \li \c #SIMT_RETURN: return from a subroutine call.
 *
 * These macros are instrumental in the coding of SimThread run fucntions.
 * They are modelled after protothreads [Dunkels et al, SenSys2006]
 * (see http://www.sics.se/~adam/pt/). You should
 * probably read this paper, but we also provide a brief explanation here,
 * through a (rather artificial) example:
 *
 * \code
 * struct MyThread : SimThread
 * {
 * 		unsigned int x,y; // initially 0, always x<=y
 *
 * 		void incY(unsigned int a) { y+=a; wakeup(); }
 *
 * 		void run() {
 * 			SIMT_BEGIN;
 * 			while(1) {
 * 				if(x>y)
 * 					SIMT_STOP;
 * 				while(x<y) {
 * 					SIMT_BUSY(100);
 * 					x++;
 * 					cout << "increment x\n";
 * 				}
 * 				SIMT_SLEEP;
 * 			}
 * 			SIMT_END;
 * 		}
 * };
 * \endcode
 *
 * The above code defines a SimThread subclass. When instances of MyThread are
 * spawned, the run() method is invoked. We see that #SIMT_BEGIN and #SIMT_END
 * define a block containing the body of the whole function. Inside this block,
 * the macros #SIMT_STOP, #SIMT_BUSY and #SIMT_SLEEP are used to control execution.
 * Whenever one of these macros is reached, execution is paused and
 * the simulation scheduler is called to resume some other SimTask.
 * When this SimThread is resumed, execution seems to resume at the point of the
 * macro's "return".  However,
 * appearances can be deceiving. To better
 * appreciate what is going on, we show the code the compiler sees after
 * expansion of these macros by the preprocessor:
 *
 * \code
 * struct MyThread : SimThread
 * {
 * 		unsigned int x,y; // initially 0, always x<=y
 *
 * 		void incY(unsigned int a) { y+=a; wakeup(); }
 *
 * 		void run() {
 * 			switch(__lc) { case 0:;
 * 			while(1) {
 * 				if(x>y)
 * 					{ this->stop(); return; }
 * 				while(x<y) {
 * 					{ this->wait_for(100);
 * 						this->__lc = __LINE__; return; case __LINE__: ;}
 * 					x++;
 * 					cout << x << endl;
 * 				}
 * 				{ this->sleep();
 * 					this->__lc = __LINE__; return; case __LINE__: ;}
 * 			}
 * 			this->stop(); };
 * 		}
 * };
 * \endcode
 *
 * Are you confused; Well, the SimThread class has an integer attribute __lc,
 * (meaning location). This attribute is used by the \c switch statement
 * every time \c run() is called, to move execution inside the body of the
 * function, at the appropriate \c case labels. The fact that this implies
 * jumping inside the body of loops (or other constructs!) is not a
 * problem for the C++ compiler. This technique is sometimes called Duff's device.
 *
 * In this manner we can get the impression of a context switch, without actually
 * maintaining a real context or a separate stack for this thread.
 * Of course, this technique has several
 * limitations, compared to real threads:
 * \li Any local variables will not retain their value between sucessive calls. Instead,
 * you should define attributes of your thread class to hold such variables that you
 * wish to maintain. Note however, that they should be initialized after
 * \c SIMT_BEGIN, or else every call to run() will change their value.
 * \li You cannot suspend a thread (using #SIMT_BUSY, #SIMT_SLEEP or #SIMT_STOP) from
 * a nested function call (called by run()), and return to that other function.
 *
 * On the other hand, SimThreads have two huge advantages:
 * -# they don't consume any memory for a stack, and
 * -# "context switching" is extremely fast
 *
 * Thus, a simulation can have tens of millions of SimThreads running, without exhausting memory.
 * Contrast this to \em any stack based thread implementation,
 * where a stack has to be at least 4 kbytes (that is, if you want to live dangerously,
 * more like 64 kbytes to be on the safe side). This is at least 100 times more
 * memory per stack/thread!
 *
 * Subroutines can be used to encapsulate common functionality inside a \c run() method.
 * They are discussed in the tutorial, in section \ref simt_subs.
 *
 * N.B.: One might think that the limitations of SimThreads are too severe, and only the
 * most trivial logic can be implemented with them, but, in my experience, even for
 * relatively complex task logic, SimThreads are quite adequate and can express the task
 * logic lucidly and succinctly.
 *
 */
/*@{*/

/**
 * \brief The type used to store the execution point of a SimThread.
 *
 * Normally, this macro should not appear in user code.
 */
#define SIMT_STATE_TYPE unsigned short

/**
 * \brief Begin body of a thread.
 *
 * Mark the beginning of a SimThread's run() function.
 * This macro should appear on a line of its own.
 */
#define SIMT_BEGIN \
	 goto _reenter_switch; _reenter_switch: ; \
	switch(this->__lc) { case 0:

/**
 * \brief End body of a thread.
 *
 * Mark the beginning of a SimThread's run() function.
 * This macro should appear on a line of its own.
 */
#define SIMT_END    this->stop(); }

/**
 * \brief Yield execution of this thread.
 *
 * This macro will set the point of resumtion where it appears and will
 * stop execution of the current thread. Notice that, under normal execution,
 * unless the the wakeup time of this thread has been changed, the
 * function will execute again immediately; thus this macro will not
 * seem to have an effect.
 *
 * This macro should probably not appear verbatim in user code, unless you know what
 * you are doing. It could however be used to define additional macros, besides the SIMT_*
 * family of macros.
 */
#define SIMT_YIELD  { this->__lc=__LINE__; return; case __LINE__:; }

/**
 * \brief Thread is put to sleep.
 */
#define SIMT_SLEEP   { this->sleep(); SIMT_YIELD  }

/**
 * \brief Thread waits for \c t timesteps (in simulation time).
 */
#define SIMT_BUSY(t) { this->wait_for(t); SIMT_YIELD }

/**
 * \brief Thread is stopped.
 */
#define SIMT_STOP    { this->stop(); return; }



/**
 * \brief Declare a local subroutine in a SimThread.
 *
 * This macro declares the following block of code as
 * a local subroutine inside the
 * \c run() method of a SimThread.
 *
 * For example:
 * \code
 * void run() {
 * 		SIMT_BEGIN;
 * 		SIMT_SUB(print_message) {
 * 			cout << "Hello world\n" << endl;
 * 			SIMT_RETURN;
 * 		}
 *
 * 		while(true) {
 * 			SIMT_CALL(print_message);
 * 			SIMT_SLEEP;
 * 		}
 * 		SIMT_END;
 * }
 * \endcode
 */
#define SIMT_SUB(subroutine_name) if(false) subroutine_name:

/**
 * \brief Call a local subroutine in a SimThread.
 */
#define SIMT_CALL(subroutine_name)\
	{ _substack.push(__LINE__); goto subroutine_name; case __LINE__:; }

/**
 * \brief Return from a local subroutine in a SimThread.
 */
#define SIMT_RETURN { __lc=_substack.pop(); goto _reenter_switch; }

/**
 * \brief Declare a bounded stack for local subroutines in a SimThread.
 *
 * This macro instantiates a space-efficient local subroutine stack,
 * when the maximum call depth of local subroutines in a SimThread
 * is known to be \c n (or less). If the maximum call depth is unkown,
 * or greater than around 20, \c SIMT_UNBOUNDED_STACK should be used.
 *
 * Either this macro, or \c SIMT_UNBOUNDED_STACK, but not both,
 * must appear inside the
 * body of every SimThread subclass which employs local subroutines in
 * its \c run() method.
 *
 */
#define SIMT_BOUNDED_STACK(n) SIMT_bounded_stack<n> _substack;

/**
 * \brief Declare an unbounded stack for local subroutines in a SimThread.
 *
 * This macro instantiates a local subroutine stack.
 *
 * When the maximum call depth of local subroutines in a SimThread
 * is known to be less than around 20, SIMT_BOUNDED_STACK should be used.
 *
 * Either this macro, or \c SIMT_BOUNDED_STACK, but not both,
 * must appear inside the
 * body of every SimThread subclass which employs local subroutines in
 * its \c run() method.
 *
 */
#define SIMT_UNBOUNDED_STACK SIMT_unbounded_stack _substack;

/**
 * \brief A stack type instantiated by SIMT_BOUNDED_STACK.
 */
template <size_t n>
struct SIMT_bounded_stack {
	SIMT_STATE_TYPE* top;
	SIMT_STATE_TYPE vec[n];
	inline SIMT_bounded_stack() : top(vec) { }
	inline void push(SIMT_STATE_TYPE addr) { *top++ = addr; }
	inline SIMT_STATE_TYPE pop() { return *(--top); }
};

/**
 * \brief A stack type instantiated by SIMT_UNBOUNDED_STACK.
 */
struct SIMT_unbounded_stack {
	std::vector<SIMT_STATE_TYPE> vec;
	inline void push(SIMT_STATE_TYPE addr) { vec.push_back(addr); }
	inline SIMT_STATE_TYPE pop() { SIMT_STATE_TYPE ret = vec.back(); vec.pop_back(); return ret; }
};

namespace pdes{

	/**
	 * \brief %Stackless simulation threads.
	 *
	 * This subclass of task
	 * is an abstract base class for stackless simulation threads.
	 * See \ref SIMT for documentation on how to use this class.
	 *
	 * There are no additional methods or attributes beyond those
	 * inherited by \c SimTask.
	 *
	 */
	struct thread : public task{
	protected:
		SIMT_STATE_TYPE __lc;
	public:
		/**
		 * \brief Constructor.
		 */
		thread() : __lc(0) { }
	};

} // namespace pdes

#endif /* THREAD_HPP_ */
