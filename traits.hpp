#ifndef TRAITS_HPP_
#define TRAITS_HPP_

#include <cstddef>
#include <cstdint>

namespace pdes{

	/** \brief Useful types for simulation time, priorities etc.
	 *
	 */
	struct traits{
		//! Type for simulation time. The current implementation uses a 64-bit unsigned integer type, whose range of values is enough
		//! for all simulations. Even though time_type could in theory be changed in the future, an integer type will still be used, as opposed
		//! to a double or float for example. The problem with floating-point numbers is that due to rounding errors they end up being slightly
		//! imprecise and, consequently, floating-point numbers that ought to be equal often differ in equality tests. This behavior could result
		//! in erroneous client code or would require from the client to make complex equality tests for the simulation time adding complexity and
		//! overhead to the simulation.
		using time_type = std::uint_fast64_t;

		//! Type to represent sizes, priorities, etc.
		using size_type = std::uint_fast64_t;
	};

} // namespace pdes


#endif /* TRAITS_HPP_ */
