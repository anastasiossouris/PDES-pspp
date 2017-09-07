#ifndef PARAMETERS_HPP_
#define PARAMETERS_HPP_

#include <stdexcept>
#include <vector>
#include "traits.hpp"

namespace pdes{

	/** \brief Defines simulation parameters
	 *
	 * class parameters can be used to specify the simulation parameters to be used by the simulation engine.
	 *
	 * The client has to specify the following parameters:
	 * 		- The simulation's lookahead (lookahead parameter)
	 * 		- When the simulation ends (endtime parameter)
	 * 		- Number of worker threads to use (num_threads parameter)
	 * 		- Core affinity for each worker thread.
	 * 			- This parameter has a form of a vector where in place i, where i ranges from 0 to num_threads - 1, the core affinity
	 * 			of thread i is stored.
	 */
	class parameters{
	public:
		using time_type = traits::time_type;
		using size_type = traits::size_type;

		parameters() = default;
		parameters(const parameters&) = default;
		parameters(parameters&&) = default;
		parameters& operator=(const parameters&) = default;
		parameters& operator=(parameters&&) = default;

		// forward-declarations of helper setters
		class thread_aff_setter;
		class num_threads_setter;
		class endtime_setter;
		class lookahead_setter;

		//! A helper object to set the thread affinities for a simulation parameters object
		class thread_aff_setter{
		public:
			friend class num_threads_setter;

			/**
			 * Sets the thread affinities.
			 *
			 * \param a Affinity vector for each thread.
			 * \throw invalid_argument If the size of the vector a doesn't match the number of worker threads.
			 */
			void set_thread_aff(std::vector<size_type> a){
				params->set_thread_aff(a);
			}
		private:
			parameters* params{nullptr};

			/**
			 * Constructs a new thread_aff_setter object in order to set the thread affinities parameter for p.
			 *
			 * \param p The parameters object for which to set the thread affinities parameter
			 */
			thread_aff_setter(parameters* p) : params{p} {}

			thread_aff_setter(const thread_aff_setter&) = default;
			thread_aff_setter& operator=(const thread_aff_setter&) = default;
			thread_aff_setter(thread_aff_setter&&) = default;
			thread_aff_setter& operator=(thread_aff_setter&&) = default;
		};

		//! A helper object to set the number of threads for a simulation parameters object
		class num_threads_setter{
		public:
			/**
			 * Sets the number of worker threads to use for the simulation.
			 *
			 * \param n Number of worker threads.
			 * \return A thread_aff_setter object to set the thread affinities parameter for the simulation
			 * \throw invalid_argument If an invalid number for worker threads has been specified.
			 */
			thread_aff_setter set_num_threads(size_type n){
				params->set_num_threads(n);

				return thread_aff_setter{params};
			}
		private:
			parameters* params{nullptr};

			friend class endtime_setter;

			/**
			 * Constructs a new num_threads_setter object in order to set the number of threads parameter for p.
			 *
			 * \param p The parameters object for which to set the number of threads parameter
			 */
			num_threads_setter(parameters* p) : params{p} {}

			num_threads_setter(const num_threads_setter&) = default;
			num_threads_setter& operator=(const num_threads_setter&) = default;
			num_threads_setter(num_threads_setter&&) = default;
			num_threads_setter& operator=(num_threads_setter&&) = default;
		};

		//! A helper object to set the endtime for a simulation parameters object
		class endtime_setter{
		public:
			/**
			 * Sets the endtime parameter for the simulation.
			 *
			 * \param e Simulation's endtime
			 * \return A num_threads_setter object to set the number of worker threads parameter for the simulation.
			 */
			num_threads_setter set_endtime(time_type e){
				params->set_endtime(e);

				return num_threads_setter{params};
			}
		private:
			parameters* params{nullptr};

			friend class lookahead_setter;

			/**
			 * Constructs a new endtime_setter object in order to set the endtime parameter for p.
			 *
			 * \param p The parameters object for which to set the endtime parameter
			 */
			endtime_setter(parameters* p) : params{p} {}

			endtime_setter(const endtime_setter&) = default;
			endtime_setter& operator=(const endtime_setter&) = default;
			endtime_setter(endtime_setter&&) = default;
			endtime_setter& operator=(endtime_setter&&) = default;
		};

		//! A helper object to set the lookahead for a simulation parameters object
		class lookahead_setter{
		public:

			/**
			 * Sets the lookahead parameter for the simulation parameter object for which this lookahead_setter was obtained.
			 *
			 * \param l Simulation's lookahead
			 * \return A endtime_setter object to set the endtime simulation parameter.
			 */
			endtime_setter set_lookahead(time_type l){
				params->set_lookahead(l);

				return endtime_setter{params};
			}
		private:
			parameters* params{nullptr};

			friend class parameters;

			/**
			 * Constructs a new lookahead_setter object in order to set the lookahead parameter for p.
			 *
			 * \param p The parameters object for which to set the lookahead parameter
			 */
			lookahead_setter(parameters* p) : params{p} {}

			lookahead_setter(const lookahead_setter&) = default;
			lookahead_setter& operator=(const lookahead_setter&) = default;
			lookahead_setter(lookahead_setter&&) = default;
			lookahead_setter& operator=(lookahead_setter&&) = default;
		};

		/**
		 * Obtain a lookahead_setter object.
		 *
		 * \return A lookahead_setter object to set the lookahead parameter for this parameters object.
		 */
		lookahead_setter get_lookahead_setter(){
			return lookahead_setter{this};
		}

		// Getters

		/**
		 * Returns the lookahead parameter for the simulation.
		 *
		 * \return Simulation's lookahead
		 */
		time_type get_lookahead() const{ return lookahead; }

		/**
		 * Returns the endtime parameter for the simulation.
		 *
		 * \return Simulation's endtime
		 */
		time_type get_endtime() const{ return endtime; }

		/**
		 * Returns the number of worker threads parameter for the simulation.
		 *
		 * \return Number of worker threads for the simulation.
		 */
		size_type get_num_threads() const{ return num_threads; }

		/**
		 * Returns the affinity for each worker thread.
		 *
		 * \return Each thread's affinity.
		 */
		std::vector<size_type> get_thread_aff() const{ return thread_aff; }

	private:
		time_type lookahead; //! The lookahead for the simulation
		time_type endtime; //! The time to end the simulation
		size_type num_threads; //! Number of worker threads to use in the engine
		std::vector<size_type> thread_aff; //! Core affinity per thread

		// Setters

		/**
		 * Sets the lookahead parameter for the simulation.
		 *
		 * \param l Simulation's lookahead
		 * \return Reference to the this parameters object.
		 */
		parameters& set_lookahead(time_type l){ return lookahead = l, *this; }

		/**
		 * Sets the endtime parameter for the simulation.
		 *
		 * \param e Simulation's endtime
		 * \return Reference to the this parameters object.
		 */
		parameters& set_endtime(time_type e){ return endtime = e, *this; }

		/**
		 * Sets the number of worker threads to use for the simulation.
		 *
		 * \param n Number of worker threads.
		 * \return Reference to the this parameters object.
		 * \throw invalid_argument If an invalid number for worker threads has been specified.
		 */
		parameters& set_num_threads(size_type n){ return check_num_threads(n), num_threads = n, *this; }

		/**
		 * Sets the thread affinities.
		 *
		 * \param a Affinity vector for each thread.
		 * \return Reference to the this parameter object.
		 * \throw invalid_argument If the size of the vector a doesn't match the number of worker threads.
		 */
		parameters& set_thread_aff(std::vector<size_type> a){ return check_thread_aff(a), thread_aff = a, *this; }

		// Helper functions to check the validity of the parameters

		void check_num_threads(size_type n) const{
			if (n <=0){
				throw std::invalid_argument("invalid number of worker threads specified in simulation parameters");
			}
		}

		void check_thread_aff(std::vector<size_type>& a) const{
			if (a.size() != num_threads){
				throw std::invalid_argument("thread affinities vector size does not match number of worker threads");
			}
		}
	};

	/**
	 * This helper function constructs a parameters object to be used by the engine where the thread affinities policy is specified by
	 * assigning each worker thread to each core in a cyclic manner. For this reason the number of available hardware contexts must be
	 * specified as num_cores.
	 *
	 * \return A parameters object for the parameters requested.
	 */
	inline parameters default_parameters(traits::time_type lookahead, //! Simulation's lookahead parameter
			traits::time_type endtime, //! Simulation's endtime parameter
			traits::size_type num_threads, //! Number of worker threads to use for the simulation
			traits::size_type num_cores //! Number of hardware contexts available
			){
		parameters params;

		// create the aff vector
		std::vector<traits::size_type> affs(num_threads);

		for (traits::size_type i = 0; i < num_threads; ++i){
			affs[i] = i%num_cores;
		}

		params.get_lookahead_setter().set_lookahead(lookahead).set_endtime(endtime).set_num_threads(num_threads).set_thread_aff(affs);

		return params;
	}

} // namespace pdes


#endif /* PARAMETERS_HPP_ */
