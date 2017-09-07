#include "engine.hpp"

namespace pdes{

	// the singleton instance for class engine
	engine* engine::_instance = nullptr;

	// Define the thread-local variables here
	thread_local engine::time_type engine::context_threshold;
	thread_local engine::time_type engine::event_threshold;

	thread_local engine::context_list_type engine::local_contexts;
	thread_local engine::context_list_type engine::committed_contexts;

	thread_local std::random_device engine::rd;
	thread_local xorshift engine::gen;
	thread_local std::uniform_int_distribution<> engine::dis;

} // namespace pdes
