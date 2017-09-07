/** \file tournament_tree.hpp
 *  \brief Contains the tournament_tree class.
 *
 * @author Tassos Souris
 * @email tassos_souris@yahoo.gr
 */
#ifndef __TOURNAMENT_TREE_HPP_IS_INCLUDED__
#define __TOURNAMEN_TREE_HPP_IS_INCLUDED__ 1

#include<cassert>
#include<functional>
#include<memory>
#include<type_traits>
#include<stdexcept>
#include<atomic>

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE (64)
#endif

namespace concurrent{

	namespace internal{
		
		/**
		 * Tests whether a particular type is suitable for the implementation of the tournament_tree
		 * that is whether it has nothrow copy/move constructors and assignments operators.
		 * 
		 * We make this test because the tournament_tree implementation for the time being does not handle exceptions
		 * (which would complicate the implementation) and thus we require that operations on the Key type be non-throw.
		 */
		template<typename T>
		struct is_key_compliant : public std::integral_constant<
				bool,
				std::is_nothrow_copy_constructible<T>::value // copy constructor is required
				&& std::is_nothrow_copy_assignable<T>::value // copy assignment operator is required
				&& std::conditional<std::is_move_constructible<T>::value, 
					std::is_nothrow_move_constructible<T>, 
					std::true_type>::type::value // move constructor not required
				&& std::conditional<std::is_move_assignable<T>::value, 
					std::is_nothrow_move_assignable<T>, 
					std::true_type>::type::value // move assignment operator not required			
				>{
					
		};

	} // namespace internal

	/** \class tournament_tree
	 *	\brief Implementation of a tournament tree.
	 *
	 *	tournament_tree implements a tournament tree for n threads. It is assumed that each thread has a unique index in the range [0,n).
	 *	A tournament tree can be used to select among the values from the n threads of type Key, the one value which is the winner according to 
	 *	a comparison function of type Compare. By default, std::less<Key> is used as Compare and thus the minimum over all values is chosen at the end.
	 *	
	 *	A user supplied key comparison function Compare must have the following prototype:
	 *		bool operator()(const Key& lhs, const Key& rhs) const;
	 *	, and return true if lhs is the winner and false otherwise.
	 */
	template<class Key, class Compare = std::less<Key> >
	class tournament_tree{
	public:
		// enable only for compliant types
		static_assert(internal::is_key_compliant<Key>::value, 
				"tournament_tree requires nothrow constructors and assignment operators");

		using size_type = unsigned int; // To represent the identifier of a participant as well as levels
		using key_type = Key; 
		using key_compare = Compare;

		// Explicit prohibit copy and move operations. The rationale is that a tournament tree is created
		// for a set of threads that are "tied" to it.
		tournament_tree(const tournament_tree&) = delete;
		tournament_tree& operator=(const tournament_tree&) = delete;
		tournament_tree(tournament_tree&&) = delete;
		tournament_tree& operator=(tournament_tree&&) = delete;	
		
		/**
		 * Imlementation details:
		 *
		 * The implementation is based on the presentation of tournament trees in section 2.1.5 'Mutex for n Processes :
		 * A Tournament-Based Algorithm' from the book 'Concurrent Programming: Algorithms, Principles, and Foundations'
		 * by Michel Raynal. The algorithm has been adapted so that it works for any number of threads and not just with n being a power of 2.
		 *
		 * Also, this is a memory-order optimized version utilizing relaxed and acquire-release memory orderings.
		 */

	private:
	
		 /** \brief A match object determines the result of a match between two participants.
		  *
		  * A tournament tree consists of a number of match nodes, where at each match node two participants meet and compete.	
		  * The participants are assumed to have indices 0 and 1. To avoid ties we use lexicographical ordering on the value of the items
		  * and the indices.
		  */
		class match{
		private:
			/**
			 * Comparison based on the lexicographical order of (a,i) and (b,j)
			 */
			struct tie_compare{
				bool operator()(const key_type& a, size_type i, const key_type& b, size_type j) const{
					return kcmp(a,b) || (!kcmp(b,a) && i < j);
				}

				// Note on optimization applied on the comparison function:
				// 
				// Normally the lexicographic order implies the following comparison:
				// 	a < b || (a == b && i < j)
				//
				// Due to the short-circuit evaluation rules of C++ if a < b then the (a == b && i < j)
				// part will not be evaluated. So, if the part (a == b && i < j) is evaluated we know
				// that a >= b. To establish that a == b we need only test whether  b >= a. And thus we use
				// !kcmp(b,a). Notice that if we had instead tested for equality between a and b (which we would 
				// have to do in case we didn't had short-circuit evaluation) because the comparison function kcmp
				// is based on equivalence we would have to do the following test : !kcmp(a,b) && !kcmp(b,a).
				// We get rid of the duplicate test !kcmp(a,b).

				key_compare kcmp;	
			};
		public:
			enum class result{ winner, loser };

			match(){
				// initialize a match object
				#if defined(NO_MEMORY_ORDER_OPTIMIZATION)
				arrived[0].store(false, std::memory_order_seq_cst);
				arrived[1].store(false, std::memory_order_seq_cst);
				done.store(false, std::memory_order_seq_cst);
				#else
				arrived[0].store(false, std::memory_order_relaxed);
				arrived[1].store(false, std::memory_order_relaxed);
				done.store(false, std::memory_order_relaxed);
				#endif
			}
			match(const match&) = delete;
			match& operator=(const match&) = delete;
			match(match&&) = delete;
			match& operator=(match&&) = delete;

			/**
			 * Notes on the algorithm used:
			 *		A match node is an object where two participants with indices 0 and 1 meet and compete with each other.
			 *		This functionality is provided with 3 functions. 
			 *			1) First the participants arrive at the compete() function and determine which is the winner and which is the loser.
			 *			2) The winner proceeds to other match nodes and when it has the winning value it must return to this match node and notify the loser
			 *			of the winning value by calling the set_winning_value() function.
			 *			3) The loser spins using the wait() function until the winner arrives and notifies it of the winning value.
			 *
			 *		We use the following variables:
			 *			- item[2] is an array of non-atomic variables used by the participants to deposit their values during the compete() call.
			 *			- arrived[2] is an array of atomic boolean registers used by the participants in the compete() call to know when the other has arrived.
			 *			- winning_value is a non-atomic variable used by the winner to deposit the winning value in the set_winning_value() function.
			 *			- done is an atomic boolean register used to synchronize the winner and the loser. The loser spins on the done variable until
			 *			the winner comes and sets it true. The the loser knows that the winner has stored the winning value.
			 *
			 *		Steps for operation compete(i,value):
			 *			1. Wait for the previous round to end: spin until done is false
			 *			2. Deposit our value: item[i] = value
			 *			3. Notify that we have arrived: arrived[i] = true
			 *			4. Wait for the other party to arrive: wait until arrived[1-i] is true
			 *			5. Get item[i] and item[1-i] and determine which is the winner and which the loser.
			 *
			 *		Steps for operation set_winning_value(i,value):
			 *			1. winning_value = value
			 *			2. Notify loser: done = true
			 *
			 *		Steps for operation wait(i):
			 *			1. Wait for winner: wait until done is true
			 *			2. Cleanup for next round: arrived[i] = false and arrived[j] = false
			 *			3. Enable next round: done = false.
			 *
			 *		Notice why the participants at the start of a round need to wait for done to become false. Assume that step 1 of function compete()
			 *		did not exists and consider the following scenario.
			 *			Participants i and j arrive at the compete call. i is the winner and j the loser. However j gets delayed.
			 *			i returns with the winning value writes arrived[i]=false and sets done to true. Then i comes again to compete.
			 *			Without the done variable, that is without knowing whether the loser at the previous round got the value or not,
			 *			i will see that arrived[j] is true (erroneously) and will proceed on its own. 
			 *
			 *		Another think to notice is that the loser is responsible for cleaning up for both him and the winner.
			 *		This is because of the following scenario:
			 *		The winner comes and sets arrived[winner] = true
			 *		The loser comes and sets arrived[loser] = true *but* doesn't see the winner
			 *		The winner proceeds and learns the winning value and comes back to call set_winning_value().
			 *		If the winner were responsible for reseting arrived[winner] then it could happen that
			 *		the loser doesn't see that arrived[winner] is true in the compete() function.
			 *
			 * Notes on the memory order optimization:
			 *
			 * The following must be established:
			 *		+ At the start of a given round when two participants use the compete() function both 
			 *		see arrived[0] = arrived[1] = false. This enables them to start a new round waiting for each other.
			 *		+ When the participants arrive and meet at the compete() function both must see the value deposited by the other.
			 *		+ The loser must see the winning value deposited by the winner when it sees that done is true.
			 *
			 * 1) Ensuring that a participant sees the value deposited by its opponent in the compete() function
			 *		Consider participant i that arrives and deposits its value using a store operation on item[i].
			 *		It announces its presence by a store operation to arrived[i] with a value true. 
			 *			(a) The store operation on item[i] happens-before the store operation to arrived[i] with value true.
			 *		Participant j=1-i arrives and spins on the arrived[i] variable until it reads true. Consider the load operation 
			 *		by j on the arrived[i] atomic variable that returned the value true. At this point, we must establish a synchronizes-with
			 *		relationship between the store operation to arrived[i] with the value true by i and that particular load operation. We do this
			 *		by using a release memory order on the store operation to arrived[i] by i and a acquire memory order on the load operation to arrived[i] by j.
			 *			(b) The store operation on arrived[i] with value true by i synchronizes-with the load operation on arrived[i] by j that returns the value true.
			 *
			 *		It follows from the fact that a synchronizes-with relationship introduces a happens-before relationship between the two threads, that
			 *			(c) The store operation on arrived[i] with value true by i happens-before the load operation on arrived[i] by j that returns the value true.
			 *		
			 *		By (a),(c) and the transitivity of the happens-before relationship we conclude that:
			 *			(d) The store operation to item[i] by i happens-before the store operation to arrived[i] with value true by i which happens-before
			 *			the load operation on arrived[i] by j that returns the value true which happens-before the load operation on item[i] by j.
			 *		Thus,
			 *			(e) The store operation of item[i] by i happens-before the load operation on item[i] by j
			 *		, and we conclude that j loads the correct value from item[i].
			 *		
			 *		Using the same reasoning with i and j reversed by can show that i also loads the correct value from item[j].
			 *
			 *	2) Ensuring that the loser sees the winning value.
			 *		Assume that participant i is a loser and participant j=1-i is a winner. Participant i spins on the done atomic variable until the winner
			 *		arrives and writes true to done.
			 *		Consider the actions of the winner j:
			 *			(a) The store operation on the winning_value happens-before the store operation on the done variable with value true.
			 *		Consider the load operation by i on the done atomic variable that returned the value true. At this point, we must establish a synchronizes-with
			 *		relationship between the store operation to done with the value true by j and that particular load operation. We do this by using 
			 *		a release memory order on the store operation to done by j and a acquire memory order on done by i.
			 *			(b) The store operation on done with value true by j synchronizes-with the load operation on done by i that returns the value true.
			 *
			 *		It follows from the fact that a synchronizes-with relationship introduces a happens-before relationship between two threads, that:
			 *			(c) The store operation on done with value true by j happens-before the load operation on done by i that returns the value true.
			 *
			 *		By (a),(c) and the transitivity of the happens-before relationship we conclude that:
			 *			(d) The store operation on winning_value by j happens-before the store operation to done with value true by j which happens
			 *			before the load operation of done with value true by i which happens before the load operation of winning_value by i.
			 *		Thus,
			 *			(e) The store operation to winning_value by j happens-before the load operation of winning_value by i.
			 *		, and we conclude that i loads the correct value from winning_value.
			 *
			 *	3) Ensuring that at the start of each round both arrived[0] and arrived[1] are false.
			 *		Consider the two participants i and j=1-i. When they call compete() they first spin on the done variable until it is false.
			 *		Initially, done is false and also arrived[0] and arrived[1] are initialized to false and thus we are correct.
			 *		Assume that at round k the claim holds.
			 *		Consider the load operations of both i and j on the done variable that returns the value false at round k+1. The value false was deposited
			 *		by the store operation by the loser on round k on the wait() function.
			 *		Consider the actions of the loser thread in the wait() function:
			 *			(a) The store to arrived[loser] with value false happens-before the store to arrived[winner] with value false which happens-before the store to done with value false by the loser.
			 *
			 *		At this point, we must establish a synchronizes-with relationship between participants i and j on round k+1 and that particular store 
			 *		operation. We do this by using a release memory order on the store operation to done by the loser and a acquire memory order on the 
			 *		load operation of done at the start of the compete() function.
			 *			(b) The store operation to done with value false by the loser of the previous round synchronizes-with the load operations on done
			 *			that return that value false by both participants on the beginning of the current round.
			 *
			 *		If follows from the fact that a synchronizes-with relationship introduces a happens-before relationship between threads, that:
			 *			(c) The store operation to done with value false by the loser of the previous round happens-before the load operations on done
			 *			that return that value false by both participants on the beginning of the current round.
			 *
			 *		By (a), (b), (c) and the transitivity of the happens-before relationship we conclude that:
			 *			(e) The store operation to arrived[loser] with value false by the loser of the previous round happens-before
			 *			the store operation to arrived[winner] with value false by the loser of the previous round which happens-before the 
			 *			store operation to done with value false by the loser of the previous round which happens-before the load operations
			 *			on done that read this value false by both participants on the start of the current value.
			 *		Thus, the participants at the start of the current round start with both arrived[0] and arrived[1] being false.
			 *		
			 *		By the principle of mathematical induction, the claim holds for all rounds k.
			 */

			/**
			 * Two participants with ids 0 and 1 can call compete() on a match node. This operation
			 * returns to the participants the result of the match. If a participant is a winner then 
			 * it is its responsibility to return later and call set_winning_value() on this match node
			 * so that to inform the loser. On the contrary, if a participant is a loser then it must await
			 * on this match node for the winner to come back.
			 */
			result compete(size_type i, const key_type& key){
				assert((i == 0 || i == 1) && "match expects 0 or 1 as participant index");
					
				#if defined(NO_MEMORY_ORDER_OPTIMIZATION)
				// wait for the previous round to end; this means that the loser from the previous round
				// must get the winning value
				while (done.load(std::memory_order_seq_cst)){}
					
				size_type j = 1- i; // our opponent
					
				item[i] = key;
				arrived[i].store(true, std::memory_order_seq_cst); // we have arrived
					
				// wait for the opponent to arrive
				while (!arrived[j].load(std::memory_order_seq_cst)){}
					
				// compete and determine whether we are a winner or a loser
				return cmp(item[i], i, item[j], j) ? result::winner : result::loser;
				#else
				// wait for the previous round to end; this means that the loser from the previous round
				// must get the winning value
				while (done.load(std::memory_order_acquire)){}
					
				size_type j = 1- i; // our opponent
					
				item[i] = key;
				arrived[i].store(true, std::memory_order_release); // we have arrived
					
				// wait for the opponent to arrive
				while (!arrived[j].load(std::memory_order_acquire)){}
					
				// compete and determine whether we are a winner or a loser
				return cmp(item[i], i, item[j], j) ? result::winner : result::loser ;
				#endif
			}
			
			/**
			 * This is used mostly for debugging purposes and is not a functionality that will be kept.
			 */
			template<class Combiner>
			result compete(size_type i, key_type& key, Combiner combiner){
				assert((i == 0 || i == 1) && "match expects 0 or 1 as participant index");
					
				#if defined(NO_MEMORY_ORDER_OPTIMIZATION)
				// wait for the previous round to end; this means that the loser from the previous round
				// must get the winning value
				while (done.load(std::memory_order_seq_cst)){}
					
				size_type j = 1- i; // our opponent
					
				item[i] = key;
				arrived[i].store(true, std::memory_order_seq_cst); // we have arrived
					
				// wait for the opponent to arrive
				while (!arrived[j].load(std::memory_order_seq_cst)){}
					
				// compete and determine whether we are a winner or a loser
				result res = cmp(item[i], i, item[j], j) ? result::winner : result::loser ;
				
				if (res == result::winner){
					key = combiner(item[i], item[j]);
				}
				
				return res;
				#else
				// wait for the previous round to end; this means that the loser from the previous round
				// must get the winning value
				while (done.load(std::memory_order_acquire)){}
					
				size_type j = 1- i; // our opponent
					
				item[i] = key;
				arrived[i].store(true, std::memory_order_release); // we have arrived
					
				// wait for the opponent to arrive
				while (!arrived[j].load(std::memory_order_acquire)){}
					
				// compete and determine whether we are a winner or a loser
				result res = cmp(item[i], i, item[j], j) ? result::winner : result::loser ;
				
				if (res == result::winner){
					key = combiner(item[i], item[j]);
				}
				
				return res;
				#endif
			}
				
			// Called by a winner to deposit a winning value.
			void set_winning_value(size_type i, const key_type& value){
				assert((i == 0 || i == 1) && "match expects 0 or 1 as participant index");
					
				#if defined(NO_MEMORY_ORDER_OPTIMIZATION)
				winning_value = value;
					
				// notify loser that we are done
				done.store(true, std::memory_order_seq_cst);
				#else
				winning_value = value;
					
				// notify loser that we are done
				done.store(true, std::memory_order_release);
				#endif
			}
				
			// Called by a loser to wait for the winner to deposit the winning value.
			key_type wait(size_type i){
				assert((i == 0 || i == 1) && "match expects 0 or 1 as participant index");
						
				size_type j = 1-i; // our opponent

				#if defined(NO_MEMORY_ORDER_OPTIMIZATION)
				// wait for winner to come
				while (!done.load(std::memory_order_seq_cst)){}
					
				key_type win = winning_value;
					
				// reset our participant status for that match node for the next round 
				// note that in the next round there may be another thread in our place
				arrived[i].store(false, std::memory_order_seq_cst);
				arrived[j].store(false, std::memory_order_seq_cst);

				// enable the next round (this round is over!)
				done.store(false, std::memory_order_seq_cst);
				
				return win;
				#else
				// wait for winner to come
				while (!done.load(std::memory_order_acquire)){}
					
				key_type win = winning_value;
					
				// reset our participant status for that match node for the next round 
				// note that in the next round there may be another thread in our place
				arrived[i].store(false, std::memory_order_relaxed);
				arrived[j].store(false, std::memory_order_relaxed);
	
				// enable the next round (this round is over!)
				done.store(false, std::memory_order_release);
				
				return win;
				#endif
			}
			
		private:
		
			/**
			 * False-Sharing Effects:
			 *
			 * We do not want interference from other match nodes so we could only place each match node at a cache-line and say that we do not care
			 * about cache-coherent traffic for one match node. However, one can devise situations where by placing each individual data on its own cache-line
			 * we can save some cache-misses and invalidations.
			 */
			alignas(CACHE_LINE_SIZE) key_type item[2]; // used by the participants to deposit their values
			alignas(CACHE_LINE_SIZE) key_type winning_value; // the winning value for a given round
			alignas(CACHE_LINE_SIZE) std::atomic<bool> arrived[2]; // used to count how many parties have arrived to the match object
			alignas(CACHE_LINE_SIZE) std::atomic<bool> done; // used to indicate the winning value has been placed by the winner	
			tie_compare cmp; // used to produce the result of the match	
		};
			
		using match_result = typename match::result;
	
	public:
			
		struct default_combiner{
			key_type operator()(const key_type& winner, const key_type& loser) const{
				return winner;
			}
		};
		
		// Create a tournament tree for _num_participants total participants.
		tournament_tree(size_type _num_participants) : num_participants{_num_participants}{
			if (num_participants <= 1){
				throw std::invalid_argument("invalid number of participants in tournament_tree constructor");
			}
				
			// for the tournament tree we need n-1 match nodes but we use counting from 1
			// so we allocate n match nodes.
			match_tree.reset(new match[num_participants]);
		}

		/**
		 * Returns the number of participants allowed to access this tournament tree.
		 */
		size_type participants() const{
			return num_participants;
		}
		
		/**
		 * A participant with id in the range [0,participants()), competes in this tournament tree with item as its value.
		 * The function returns the result of the competition.
		 *
		 * @param id The identifier of the participant.
		 * @param item The value with which the participant competes.
		 * @return The result of the competition.
		 */ 
		key_type compete(size_type id, const key_type& item){
			// Check that we have been given a valid id
			assert(valid_id(id) && "invalid id in tournament_tree compete");
				
			// In this implementation we use counting from 1 but we accept ids in the range [0,num_participants)
			++id;
				
			// Remember our initial participant id cause it will be needed in the distribution phase.
			size_type initial_id = id + num_participants - 1;
							
			key_type compete_result; 
			size_type node_id = initial_id;
			size_type p_id;
			size_type level = 1;
			match_result result;
				
			// Competition Phase
			compete(p_id, node_id, level, result, item);
				
			// Computation Phase
			compute(result, node_id, p_id, level, compete_result, item);
				
			// Distribution Phase
			distribute(initial_id, compete_result, level);
				
			return compete_result;
		}
			
		/**
		 * A participant with id in the range [0,participants()), competes in this tournament tree with item as its value.
		 * The function returns the result of the competition.
		 *
		 * @param id The identifier of the participant.
		 * @param item The value with which the participant competes.
		 * @param exit_callback A function object to call when the winner has been determined but before all threads have been notified.
		 * @return The result of the competition.
		 */ 
		template<class ExitCallback>
		key_type compete(size_type id, const key_type& item, const ExitCallback& exit_callback){
			// Check that we have been given a valid id
			assert(valid_id(id) && "invalid id in tournament_tree compete");
				
			// In this implementation we use counting from 1 but we accept ids in the range [0,num_participants)
			++id;
				
			// Remember our initial participant id cause it will be needed in the distribution phase.
			size_type initial_id = id + num_participants - 1;
							
			key_type compete_result; 
			size_type node_id = initial_id;
			size_type p_id;
			size_type level = 1;
			match_result result;
				
			// Competition Phase
			compete(p_id, node_id, level, result, item);
				
			// Computation Phase
			compute(result, node_id, p_id, level, compete_result, item, exit_callback);
				
			// Distribution Phase
			distribute(initial_id, compete_result, level);
				
			return compete_result;
		}
		
		template<class ExitCallback, class Combiner>
		key_type compete(size_type id, key_type& item, const ExitCallback& exit_callback, Combiner combiner){
			// Check that we have been given a valid id
			assert(valid_id(id) && "invalid id in tournament_tree compete");
				
			// In this implementation we use counting from 1 but we accept ids in the range [0,num_participants)
			++id;
				
			// Remember our initial participant id cause it will be needed in the distribution phase.
			size_type initial_id = id + num_participants - 1;
							
			key_type compete_result; 
			size_type node_id = initial_id;
			size_type p_id;
			size_type level = 1;
			match_result result;
				
			// Competition Phase
			while (true){
				p_id = node_id&1; // = node_id%2 - our participant id (0 or 1) to compete at the node
								  // bithack reference: http://graphics.stanford.edu/~seander/bithacks.html#ModulusDivisionEasy
				node_id >>= 1; //node_id = node_id/2 the node at which we will compete
				result = match_tree[node_id].compete(p_id, item, combiner);
				if (result == match_result::loser || is_root(node_id)){ 
					// We stop the competition phase either when we loose or when we reach the root node
					break; 
				}
				++level;
			}
				
			// Computation Phase
			switch(result){
			case match_result::loser:
				// Losers need first wait for the winners of their nodes to notify them of the result.
				compete_result = match_tree[node_id].wait(p_id);
				break;
			case match_result::winner:
				exit_callback();
				// The sole winner of course won with his own value
				compete_result = item; 
				match_tree[node_id].set_winning_value(p_id, compete_result);
				break;
			}
			--level; // we are done with this level; at the distribution phase we will start from below
				
			// Distribution Phase
			for (; level >= 1; --level){
				/**
				 * In this phase we follow the reverse path from the node we stopped before to the leaf node where we started.
				 *
				 * Fortunately, we can compute the node_id for the nodes at each level we passed using the node_id_at_level() function. 
				 * Refer to that function for more information on how the computation is done. 
				 *
				 * It remains to compute our p_id for the node. Notice that in the computation phase, when we arrived at that node (the child node
				 * we have just chosen), we calculated our participant id p_id as node_id%2 where node_id is the identifier of the node we were at 
				 * at previous level. So, we can compute p_id by finding the node_id we had at level-1 using node_id_at_level() and taking the modulo 2.
				 */
				node_id = node_id_at_level(initial_id,level);
				p_id = node_id_at_level(initial_id,level-1)&1; // = node_id_at_level(initial_id,level-1)%2;
															  // bithack reference: http://graphics.stanford.edu/~seander/bithacks.html#ModulusDivisionEasy
				match_tree[node_id].set_winning_value(p_id, compete_result);
			}
				
			return compete_result;
		}
			
	private:
		size_type num_participants;
		std::unique_ptr<match[]> match_tree{nullptr}; // the tournament tree
			
		/**
		 * Returns the node identifier for the participant with the given id when it reaches level k.
		 */
		 size_type node_id_at_level(size_type id, size_type k) const{
			/**
			 * Notice that for each level when a participant competes at a node it calculates the node's id
			 * using the formula node_id/2 where node_id is the previous node_id the participant had. Thus, for each
			 * level we keep dividing by 2 and since our initial node id (before level=1) is id, when we are at level k 
			 * we have divided our initial id by 2 k times total, thus the formula is id/(2^k). 
			 * 
			 * Also, since the formula includes a division by 2 we can use a right logical shift with k. Since size_type is
			 * unsigned this is valid.
			 */
			return (id >> k);
		 }
			 
		 /**
		  * Returns true if the given id is within the range of allowable participant identifiers and false if not.
		  */
		 bool valid_id(size_type id) const{
			return 0 <= id && id < num_participants;
		 }
			 
		 /**
		  * Returns true if the given node id represents the root node and false if not.
		  */
		 bool is_root(size_type id) const{
			return id == 1;
		 }
			
		 /**
		  * Competition phase. The participants traverse the tree from their associated leaf node up to the root and
		  * compete with the other participants that arrive at the nodes.
		  */
		 void compete(size_type& p_id, size_type& node_id, size_type& level, match_result& result, const key_type& item){
			while (true){
				p_id = node_id&1; // = node_id%2 - our participant id (0 or 1) to compete at the node
								 // bithack reference: http://graphics.stanford.edu/~seander/bithacks.html#ModulusDivisionEasy
				node_id >>= 1; //node_id = node_id/2 the node at which we will compete
				result = match_tree[node_id].compete(p_id, item);
				if (result == match_result::loser || is_root(node_id)){ 
					// We stop the competition phase either when we loose or when we reach the root node
					break; 
				}
				++level;
			}
		}
			
		/**
		 * Computation phase. A winner sets the winning value and loser waits for the winner.
		 */
		void compute(match_result result, size_type node_id, size_type p_id, size_type& level, key_type& compete_result,
				const key_type& item){
			switch(result){
			case match_result::loser:
				// Losers need first wait for the winners of their nodes to notify them of the result.
				compete_result = match_tree[node_id].wait(p_id);
				break;
			case match_result::winner:
				// The sole winner of course won with his own value
				compete_result = item; 
				match_tree[node_id].set_winning_value(p_id, compete_result);
				break;
			}
			--level; // we are done with this level; at the distribution phase we will start from below
		}
		
		/**
		 * Computation phase. A winner sets the winning value and loser waits for the winner.
		 */
		template<class ExitCallback>
		void compute(match_result result, size_type node_id, size_type p_id, size_type& level, key_type& compete_result, 
				const key_type& item, const ExitCallback& exit_callback){
			switch(result){
			case match_result::loser:
				// Losers need first wait for the winners of their nodes to notify them of the result.
				compete_result = match_tree[node_id].wait(p_id);
				break;
			case match_result::winner:
				exit_callback();
				// The sole winner of course won with his own value
				compete_result = item; 
				match_tree[node_id].set_winning_value(p_id, compete_result);
				break;
			}
			--level; // we are done with this level; at the distribution phase we will start from below
		}
		
		 /**
		  * Distribution phase for the competition. A participant traverses the tournament tree from top to bottom
		  * notifying of the result (winning value) the other participants in lower nodes it had won.
		  */
		 void distribute(size_type initial_id, const key_type& compete_result, size_type level){
			size_type node_id;
			size_type p_id;

			// Distribution Phase
			for (; level >= 1; --level){
				/**
				 * In this phase we follow the reverse path from the node we stopped before to the leaf node where we started.
				 *
				 * Fortunately, we can compute the node_id for the nodes at each level we passed using the node_id_at_level() function. 
				 * Refer to that function for more information on how the computation is done. 
				 *
				 * It remains to compute our p_id for the node. Notice that in the computation phase, when we arrived at that node (the child node
				 * we have just chosen), we calculated our participant id p_id as node_id%2 where node_id is the identifier of the node we were at 
				 * at previous level. So, we can compute p_id by finding the node_id we had at level-1 using node_id_at_level() and taking the modulo 2.
				 */
				node_id = node_id_at_level(initial_id,level);
				p_id = node_id_at_level(initial_id,level-1)&1; // = node_id_at_level(initial_id,level-1)%2;
															   // bithack reference: http://graphics.stanford.edu/~seander/bithacks.html#ModulusDivisionEasy
				match_tree[node_id].set_winning_value(p_id, compete_result);
			}
		}
	};
		
} // namespace concurrent

#endif
