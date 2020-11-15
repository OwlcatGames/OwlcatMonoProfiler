#include "mono_profiler_client.h"
#include "network.h"
#include "memory_reader.h"
#include "memory_writer.h"
#include "persistent_storage.h"
#include "db_migrations.h"
#include "db_queries.h"

#include <memory>
#include <string>
#include <thread>
#include <filesystem>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>

#if defined(WIN32)
#include <Windows.h>
#include "detours.h"
#endif

namespace owlcat
{
	struct profiler_event
	{
		enum type_t
		{
			alloc,
			free
		};

		type_t type;

		uint64_t frame;
		uint64_t addr;
		uint32_t size;
		std::string obj_type;
		std::string callstack;
	};

	struct base_command
	{
		const protocol::command type;
		const uint64_t request_id;

		base_command(protocol::command _type, uint64_t _request_id)
			: type(_type)
			, request_id(_request_id)
		{}

		virtual ~base_command() {}
	};

	struct command_find_references : public base_command
	{
		command_find_references(uint64_t request_id, std::vector<uint64_t> _addresses, find_references_callback _callback)
			: base_command(protocol::command::CMD_REFERENCES, request_id)
			, addresses(_addresses)
			, callback{ _callback }
		{}

		std::vector<uint64_t> addresses;
		find_references_callback callback;
	};

	struct command_pause_app : public base_command
	{
		command_pause_app(uint64_t request_id, pause_app_callback _callback)
			: base_command(protocol::command::CMD_PAUSE, request_id)
			, callback(_callback)
		{}

		pause_app_callback callback;
	};

	struct command_resume_app : public base_command
	{
		command_resume_app(uint64_t request_id, resume_app_callback _callback)
			: base_command(protocol::command::CMD_RESUME, request_id)
			, callback(_callback)
		{}

		resume_app_callback callback;
	};

	class mono_profiler_client::details
	{
		network m_network;

		struct network_settings
		{
			std::string addr;
			int port;
		};

		network_settings m_network_settings;
		uint64_t m_next_request_id = 0;

		// List of commands pending responses
		std::vector<std::shared_ptr<base_command>> m_commands;

		// Returns a command for the specified request_id and type, or nullptr is none is found
		// If the command is found, it is removed from m_commands!
		template<typename T>
		std::shared_ptr<T> get_command(uint64_t request_id, protocol::command type)
		{
			auto iter = std::find_if(m_commands.begin(), m_commands.end(), [&](auto& cmd) {return cmd->request_id == request_id; });
			if (iter == m_commands.end())
				return nullptr;

			assert((*iter)->type == type);

			auto result = std::static_pointer_cast<T>(*iter);

			m_commands.erase(iter);
			return result;
		}

		uint64_t add_command(std::shared_ptr<base_command> cmd)
		{
			m_commands.push_back(cmd);
			return cmd->request_id;
		}

		bool m_stop = false;
		std::thread m_thread;
		persistent_storage::persistent_storage m_db;

		std::vector<profiler_event> m_frame_events;
		uint64_t m_prev_frame = 0xFFFFFFFFFFFFFFFF;

		// Number of allocation and free events this frame
		uint64_t m_frame_allocs = 0;
		uint64_t m_frame_frees = 0;
		// Running total of allocated memory
		int64_t m_size_running_total = 0;

		bool m_has_min_frame = false;
		// Minimun and maximum known frame
		uint64_t m_min_frame = 0;
		uint64_t m_max_frame = 0;

		std::unique_ptr<mono_profiler_client_data> m_data_interface;

		// Maps between type ID and type name
		std::unordered_map<std::string, uint64_t> m_type_to_id_map;
		std::unordered_map<uint64_t, std::string> m_id_to_type_map;
		
		// Maps between callstack ID and callstack text
		std::unordered_map<std::string, uint64_t> m_callstacks_to_id_map;
		std::unordered_map<uint64_t, std::string> m_id_to_callstacks_map;

		uint64_t m_next_type_id = 0;
		uint64_t m_next_callstack_id = 0;

		std::string m_db_file_name;

		// Inserts a new type ID into database, or returns one already present from memory cache
		uint64_t get_or_create_type_id(const std::string& type)
		{
			auto iter = m_type_to_id_map.find(type);
			if (iter != m_type_to_id_map.end())
				return iter->second;

			uint64_t type_id = m_next_type_id++;
			m_type_to_id_map.insert(std::make_pair(type, type_id));
			m_id_to_type_map.insert(std::make_pair(type_id, type));

			queries::insert_type(m_db, type, type_id);

			return type_id;
		}

		// Inserts a new callstack ID into database, or returns one already present from memory cache
		uint64_t get_or_create_callstack_id(const std::string& callstack)
		{
			auto iter = m_callstacks_to_id_map.find(callstack);
			if (iter != m_callstacks_to_id_map.end())
				return iter->second;

			uint64_t callstack_id = m_next_callstack_id++;
			m_callstacks_to_id_map.insert(std::make_pair(callstack, callstack_id));
			m_id_to_callstacks_map.insert(std::make_pair(callstack_id, callstack));

			queries::insert_callstack(m_db, callstack, callstack_id);

			return callstack_id;
		}

		// Saves events from the current frame into database using a transaction for maximum performance
		void save_frame_events()
		{
			// There should be no events before we receice the first frame
			if (m_prev_frame == 0xFFFFFFFFFFFFFFFF)
			{
				if (!m_frame_events.empty())
					__debugbreak();
				return;
			}

			persistent_storage::transaction transaction(m_db, persistent_storage::transaction_behaviour::rollback);
			for (auto& e : m_frame_events)
			{
				if (e.type == profiler_event::alloc)
				{
					uint64_t type_id = get_or_create_type_id(e.obj_type);
					uint64_t callstack_id = get_or_create_callstack_id(e.callstack);
					queries::insert_alloc_event(m_db, e.frame, e.addr, e.size, type_id, callstack_id);
				}
				else
					queries::insert_free_event(m_db, e.frame, e.addr, e.size);
			}
			queries::insert_frame_stats(m_db, m_prev_frame, m_frame_allocs, m_frame_frees, m_size_running_total);
			transaction.commit();
			m_frame_events.clear();
		}

		// Check if the last received event's frame is different from the previous event's frame,
		// and saves the current frame events if it is.
		void try_save_events(uint64_t frame)
		{
			if (frame != m_prev_frame)
			{
				if (!m_has_min_frame) 
				{
					m_min_frame = frame;
					m_has_min_frame = true;
				}
				if (frame > m_max_frame) m_max_frame = frame;

				save_frame_events();
				m_prev_frame = frame;

				m_frame_allocs = 0;
				m_frame_frees = 0;
			}
		}

		// Loads object types and callstacks from opened database into memory caches
		bool load_types_and_callstacks()
		{
			auto types_cursor = queries::select_types(m_db);
			if (types_cursor.has_error())
				return false;

			while (types_cursor.next())
			{
				auto type_id = types_cursor.get_uint64("type_id");
				auto type = types_cursor.get_string("name");

				m_type_to_id_map.insert(std::make_pair(type, type_id));
				m_id_to_type_map.insert(std::make_pair(type_id, type));
			}

			auto callstacks_cursor = queries::select_callstacks(m_db);
			if (callstacks_cursor.has_error())
				return false;

			while (callstacks_cursor.next())
			{
				auto callstack_id = callstacks_cursor.get_uint64("callstack_id");
				auto callstack = callstacks_cursor.get_string("callstack");

				m_callstacks_to_id_map.insert(std::make_pair(callstack, callstack_id));
				m_id_to_callstacks_map.insert(std::make_pair(callstack_id, callstack));
			}

			auto frames_cursor = queries::select_min_max_frame(m_db);
			if (!frames_cursor.next())
				return false;

			m_min_frame = frames_cursor.get_uint64("min_frame");
			m_max_frame = frames_cursor.get_uint64("max_frame");

			return true;
		}

	public:
		void process_messages()
		{
			m_frame_events.reserve(1024);
			while (true)
			{
				message msg;
				if (!m_network.read_message(msg))
				{
					if (m_stop)
						break;

					std::this_thread::yield();
					continue;
				}
				
				memory_reader reader(msg.data);

				if (msg.header.type == protocol::message::SRV_ALLOC)
				{
					uint64_t frame;
					uint64_t addr;
					uint32_t size;
					std::string name;
					std::string callstack;
					bool all_ok =
						reader.read_uint64(frame) &&
						reader.read_uint64(addr) &&
						reader.read_uint32(size) &&
						reader.read_string(name) &&
						reader.read_string(callstack);

					// Frames should always be sequential
					if (frame < m_prev_frame && m_prev_frame != 0xFFFFFFFFFFFFFFFF)
						__debugbreak();

					if (all_ok)
					{
						try_save_events(frame);
						m_frame_events.push_back({profiler_event::alloc, frame, addr, size, name, callstack});
						++m_frame_allocs;
						m_size_running_total += size;
					}
					else
						printf("Received alloc, but msg is broken\n");
				}
				else if (msg.header.type == protocol::message::SRV_FREE)
				{
					uint64_t frame;
					uint64_t addr;
					uint32_t size;
					bool all_ok =
						reader.read_uint64(frame);
						reader.read_uint64(addr);
						reader.read_uint32(size);

					if (all_ok)
					{
						try_save_events(frame);
						m_frame_events.push_back({ profiler_event::free, frame, addr, size, "", "" });		
						++m_frame_frees;
						m_size_running_total -= size;
					}
					else
						printf("Received free, but msg is broken\n");
				}
				else if (msg.header.type == protocol::message::SRV_REFERENCES)
				{
					uint64_t request_id;
					if (!reader.read_uint64(request_id))
					{
						printf("Received references, but msg is broken\n");
						continue;
					}

					auto cmd = get_command<command_find_references>(request_id, protocol::command::CMD_REFERENCES);
					if (cmd == nullptr)
						continue;

					uint64_t count;
					if (!reader.read_varint(count))
					{
						printf("Received references, but msg is broken\n");
						continue;
					}

					std::vector<object_references_t> result;

					for (uint64_t i = 0; i < count; ++i)
					{
						uint64_t addr;
						uint64_t parents_count;
						std::string type;

						reader.read_varint(addr);
						reader.read_string(type);
						reader.read_varint(parents_count);						

						object_references_t obj;
						obj.address = addr;
						obj.type = type;

						for (uint64_t j = 0; j < parents_count; ++j)
						{
							uint64_t parent_addr;
							reader.read_varint(parent_addr);
							obj.parents.push_back({parent_addr});
						}

						result.push_back(obj);
					}

					cmd->callback(cmd->addresses, "", result);
				}
				else if (msg.header.type == protocol::message::SRV_PAUSE)
				{
					uint64_t request_id;
					if (!reader.read_uint64(request_id))
					{
						printf("Received pause, but msg is broken\n");
						continue;
					}

					auto cmd = get_command<command_pause_app>(request_id, protocol::command::CMD_PAUSE);
					if (cmd == nullptr)
						continue;

					uint8_t error;
					if (!reader.read_uint8(error))
					{
						printf("Received pause, but msg is broken\n");
						continue;
					}

					cmd->callback(error == 0);
				}
				else if (msg.header.type == protocol::message::SRV_RESUME)
				{
					uint64_t request_id;
					if (!reader.read_uint64(request_id))
					{
						printf("Received resume, but msg is broken\n");
						continue;
					}

					auto cmd = get_command<command_resume_app>(request_id, protocol::command::CMD_RESUME);
					if (cmd == nullptr)
						continue;

					uint8_t error;
					if (!reader.read_uint8(error))
					{
						printf("Received resume, but msg is broken\n");
						continue;
					}

					cmd->callback(error == 0);
				}
				else
				{
					printf("Received bad message\n");
					__debugbreak();
				}

				// Avoid storing too many events in queue
				if (m_frame_events.size() > 10000)
					save_frame_events();
			}

			// Submit last events before quitting
			if (!m_frame_events.empty())
				try_save_events(m_frame_events[0].frame);
		}

	public:
		details()
		{
			m_data_interface.reset(new mono_profiler_client_data(this));
		}

		~details()
		{
			stop();
			
			m_db.close();
		}

#if defined(WIN32)
		bool launch_executable(const std::string& executable, const std::string& commandline, int port, const std::string& db_file_name, const std::string& dll_location)
		{
			std::filesystem::path exec_path(executable);
			std::string cwd = exec_path.parent_path().string();

			STARTUPINFO si;
			PROCESS_INFORMATION pi;

			ZeroMemory(&si, sizeof(si));
			si.cb = sizeof(si);
			si.dwX = 0;
			si.dwY = 0;
			si.dwXSize = 1920;
			si.dwYSize = 1080;
			ZeroMemory(&pi, sizeof(pi));

			const char* dllLoc = dll_location.c_str();

			if (!DetourCreateProcessWithDlls(executable.c_str(), (char*)commandline.c_str(), nullptr, nullptr, true, CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi, 1, &dllLoc, nullptr))
			{
				int error_code = GetLastError();
				return false;
			}			

			ResumeThread(pi.hThread);

			// We need to wait until the app is started and the server is listening before attempting a connection, but
			// we have no reliable way to do this for now, so just wait 5 seconds.
			// We can't use WaitForInputIdle here, because server will block the main thread when listening for connection
			// TODO: Investigate creating named pipes?
			Sleep(5000);

			// We only support launching applications on the same computer, so use loopback IP
			return start("127.0.0.1", port, db_file_name.c_str());
		}
#endif

		bool start(const std::string& addr, int server_port, const std::string& db_file_name)
		{
			m_network_settings.addr = addr;
			m_network_settings.port = server_port;

			if (!m_network.connect(addr, server_port))
				return false;

			m_db.close();

			m_db_file_name = db_file_name;

			// Remove existing database (if we use a non-temporary one)
			if (std::filesystem::exists(m_db_file_name))
				std::filesystem::remove(m_db_file_name);
			if (!m_db.open(m_db_file_name, true))
				return false;

			// WAL performance is not good enough
			//m_db.pragma("PRAGMA journal_mode=wal");
			m_db.pragma("PRAGMA journal_mode=memory");
			// More performance at a cost of less reliability
			m_db.pragma("PRAGMA synchronous=OFF");
			// Set a BIG memory cache: we want to keep a lot in memory
			char cache_size_pragma[256];
			const uint64_t gb2 = 2ULL * 1024 * 1024 * 1024;
			const uint64_t page_size = 65536;
			sprintf(cache_size_pragma, "PRAGMA cache_size=%I64u", gb2 / page_size);
			m_db.pragma(cache_size_pragma);
			m_db.pragma("PRAGMA page_size=65536");
			
			// This call creates database structure
			if (!upgrade_database(m_db))
				return false;

			if (!queries::register_queries(m_db))
				return false;

			m_thread = std::thread(&mono_profiler_client::details::process_messages, this);

			return true;
		}

		void stop()
		{
			m_stop = true;
			if (m_thread.joinable())
				m_thread.join();
			m_network.stop();			
		}

		void close_db()
		{
			assert(!m_thread.joinable() && (!is_connected() || is_connecting()));
			m_db.close();
		}

		bool save_db(const std::string& new_db_file_name, bool move)
		{
			// Save in-memory or temporary database to file
			if (!m_db.save(new_db_file_name))
				return false;

			// Reopen saved database
			m_db_file_name = new_db_file_name;
			return open_data(m_db_file_name.c_str());
		}

		bool open_data(const std::string& file)
		{
			if (m_thread.joinable())
				return false;

			if (m_db.is_open())
				m_db.close();

			return m_db.open(file, false) && upgrade_database(m_db) && queries::register_queries(m_db) && load_types_and_callstacks();
		}

		bool is_connected() const
		{
			return m_network.is_connected();
		}

		bool is_connecting() const
		{
			return m_network.is_connecting();
		}

		bool is_data_open() const
		{
			return m_db.is_open();
		}

		mono_profiler_client_data* get_data()
		{
			return m_data_interface.get();
		}

		void find_objects_references(const std::vector<uint64_t>& addresses, find_references_callback callback)
		{
			auto request_id = add_command(std::make_shared<command_find_references>(m_next_request_id++, addresses, callback));
				
			std::vector<uint8_t> cmd;
			memory_writer writer(cmd);
			writer.write_uint64(request_id);
			writer.write_uint64(addresses.size());
			for(auto addr : addresses)
				writer.write_uint64(addr);
			m_network.write_message(protocol::command::CMD_REFERENCES, (uint32_t)cmd.size(), cmd.data());
		}

		void pause_app(pause_app_callback callback)
		{
			auto request_id = add_command(std::make_shared<command_pause_app>(m_next_request_id++, callback));

			std::vector<uint8_t> cmd;
			memory_writer writer(cmd);
			writer.write_uint64(request_id);
			m_network.write_message(protocol::command::CMD_PAUSE, (uint32_t)cmd.size(), cmd.data());
		}

		void resume_app(resume_app_callback callback)
		{
			auto request_id = add_command(std::make_shared<command_resume_app>(m_next_request_id++, callback));

			std::vector<uint8_t> cmd;
			memory_writer writer(cmd);
			writer.write_uint64(request_id);
			m_network.write_message(protocol::command::CMD_RESUME, (uint32_t)cmd.size(), cmd.data());
		}

		const char* get_type_name(uint64_t type_id) const
		{
			auto iter = m_id_to_type_map.find(type_id);
			if (iter != m_id_to_type_map.end())
				return iter->second.c_str();

			return "";
		}

		const char* get_callstack(uint64_t callstack_id) const
		{
			auto iter = m_id_to_callstacks_map.find(callstack_id);
			if (iter != m_id_to_callstacks_map.end())
				return iter->second.c_str();

			return "";
		}

		size_t get_types_count() const
		{
			return m_id_to_type_map.size();
		}

		size_t get_callstacks_count() const
		{
			return m_id_to_callstacks_map.size();
		}

public:
		void get_frame_boundaries(uint64_t& min, uint64_t& max)
		{
			min = m_min_frame;
			max = m_max_frame;
		}

		void get_stats(std::vector<uint64_t>& alloc_counts, std::vector<uint64_t>& free_counts, uint64_t& max_allocs, uint64_t& max_frees, std::vector<uint64_t>& size_points, int64_t& max_size, uint64_t from_frame, uint64_t to_frame)
		{
			// If the starting frame falls into an area where no events were reported,
			// we need to query last known size of allocated memory before the starting frame
			uint64_t last_known_frame_size = 0;
			auto result_last_known = queries::select_last_good_size(m_db, from_frame);
			if (result_last_known.next())
			{
				last_known_frame_size = result_last_known.get_uint64("size");
			}

			max_allocs = 0;
			max_frees = 0;
			max_size = 0;
			auto result = queries::select_stats(m_db, from_frame, to_frame);
			alloc_counts.clear();
			free_counts.clear();
			size_points.clear();
			
			uint64_t prev_frame = from_frame;
			bool first_frame = true;

			while (result.next())
			{
				uint64_t frame = result.get_uint64("frame");
				uint64_t allocs = result.get_uint64("allocs");
				uint64_t frees = result.get_uint64("frees");
				int64_t size = result.get_int64("size");

				if (allocs > max_allocs) max_allocs = allocs;
				if (frees > max_frees) max_frees = frees;
				if (size > max_size) max_size = size;
				
				// Fill gaps if the first reported frame greater than the first requested frame
				if (first_frame && frame > prev_frame)
				{					
					for (auto f = from_frame; f < frame; ++f)
					{
						alloc_counts.push_back(0);
						free_counts.push_back(0);
						size_points.push_back(last_known_frame_size);
					}
				}
				// Fill gaps if no events were reported in some frame
				else if (frame > prev_frame + 1)
				{
					for (auto f = prev_frame + 1; f < frame; ++f)
					{
						alloc_counts.push_back(0);
						free_counts.push_back(0);
						size_points.push_back(size_points.back());
					}
				}

				first_frame = false;
				prev_frame = frame;

				alloc_counts.push_back(allocs);
				free_counts.push_back(frees);
				size_points.push_back(size);
			}

			// Fill gaps to the last requested frame
			if (prev_frame < to_frame && !size_points.empty())
			{
				for (auto f = prev_frame + 1; f <= to_frame && f <= m_max_frame; ++f)
				{
					alloc_counts.push_back(0);
					free_counts.push_back(0);
					size_points.push_back(size_points.back());
				}
			}
		}

		/*
			This function builds a list of live objects, i.e. objects that were allocated, but not freed during the
			specified timeframe. Notice, that these are NOT ALL leaks, but some of such objects might be leaked.

			We build the list by starting from an empty list, and then processing all events that fall into the timeframe.
			When we encounter an allocation event, we add the object to the list, and when we encounter a free event,
			we remove it. Therefore, in the end, only objects that were allocated, but not freed are left.
		*/
		void get_live_objects(std::vector<live_object>& objects, int from_frame, int to_frame, progress_func_t progress_func)
		{
			objects.clear();

			// Map of address -> live object info
			// TODO: Try to use sparse_map or robin_map here to speed up everything?
			std::unordered_map<uint64_t, live_object> live_objects_map;
			live_objects_map.reserve(1024 * 1024);

			size_t events_count = queries::select_events_count(m_db, from_frame, to_frame);

			size_t row_num = 0;

			auto result = queries::select_events(m_db, from_frame, to_frame);
			while(result.next())
			{
				uint64_t event_type = result.get_uint64("event_type_id");
				uint64_t addr = result.get_uint64("address");

				// Allocation: write object to map
				if (event_type == 1)
				{
					uint64_t frame = result.get_uint64("frame");
					uint64_t size = result.get_uint64("size");
					uint64_t type = result.get_uint64("type_id");
					uint64_t callstack = result.get_uint64("callstack_id");
					live_objects_map.emplace(addr, live_object(addr, size, frame, type, callstack));
				}
				// Deallocation: remove object from map
				else
				{
					auto iter = live_objects_map.find(addr);
					if (iter != live_objects_map.end())
						live_objects_map.erase(iter);
				}

				if (progress_func != nullptr)
					if (!progress_func(row_num++, events_count))
						return;
			}

			// Flatten the map
			for (auto& pair : live_objects_map)
			{
				objects.push_back(pair.second);
			}
		}		

		size_t get_network_messages_count() const { return m_network.get_read_messages_count(); }
	};

	mono_profiler_client::mono_profiler_client()
	{
		m_details = new details();
	}

	mono_profiler_client::~mono_profiler_client()
	{
		delete m_details;
	}

#if defined(WIN32)
	bool mono_profiler_client::launch_executable(const std::string& executable, const std::string& args, int port, const std::string& db_file_name, const std::string& dll_location)
	{
		return m_details->launch_executable(executable, args, port, db_file_name, dll_location);
	}
#endif

	bool mono_profiler_client::start(const std::string& addr, int server_port, const std::string& db_file_name)
	{
		return m_details->start(addr, server_port, db_file_name);
	}

	void mono_profiler_client::stop()
	{
		m_details->stop();
	}

	void mono_profiler_client::close_db()
	{
		m_details->close_db();
	}

	bool mono_profiler_client::save_db(const std::string& new_db_file_name, bool move)
	{
		return m_details->save_db(new_db_file_name, move);
	}

	void close_db();

	bool mono_profiler_client::open_data(const std::string& file)
	{
		return m_details->open_data(file);
	}

	bool mono_profiler_client::is_connected() const
	{
		return m_details->is_connected();
	}

	bool mono_profiler_client::is_connecting() const
	{
		return m_details->is_connecting();
	}

	bool mono_profiler_client::is_data_open() const
	{
		return m_details->is_data_open();
	}

	size_t mono_profiler_client::get_network_messages_count() const 
	{
		return m_details->get_network_messages_count();
	}

	mono_profiler_client_data* mono_profiler_client::get_data()
	{
		return m_details->get_data();
	}

	void mono_profiler_client::find_objects_references(const std::vector<uint64_t>& addresses, find_references_callback callback)
	{
		return m_details->find_objects_references(addresses, callback);
	}

	void mono_profiler_client::pause_app(pause_app_callback callback)
	{
		return m_details->pause_app(callback);
	}

	void mono_profiler_client::resume_app(resume_app_callback callback)
	{
		return m_details->resume_app(callback);
	}

	const char* mono_profiler_client::get_type_name(uint64_t type_id) const
	{
		return m_details->get_type_name(type_id);
	}

	mono_profiler_client_data::mono_profiler_client_data(mono_profiler_client::details* source)
		: m_source(source)
	{}

	void mono_profiler_client_data::get_frame_boundaries(uint64_t& min, uint64_t& max)
	{
		m_source->get_frame_boundaries(min, max);
	}

	void mono_profiler_client_data::get_frame_stats(std::vector<uint64_t>& alloc_counts, std::vector<uint64_t>& free_counts, uint64_t& max_allocs, uint64_t& max_frees, std::vector<uint64_t>& size_points, int64_t& max_size, uint64_t from_frame, uint64_t to_frame)
	{
		m_source->get_stats(alloc_counts, free_counts, max_allocs, max_frees, size_points, max_size, from_frame, to_frame);
	}

	void mono_profiler_client_data::get_live_objects(std::vector<live_object>& objects, int from, int to, progress_func_t progress_func)
	{
		m_source->get_live_objects(objects, from, to, progress_func);
	}

	const char* mono_profiler_client_data::get_type_name(uint64_t type_id) const
	{
		return m_source->get_type_name(type_id);
	}

	const char* mono_profiler_client_data::get_callstack(uint64_t callstack_id) const
	{
		return m_source->get_callstack(callstack_id);
	}

	size_t mono_profiler_client_data::get_types_count() const
	{
		return m_source->get_types_count();
	}

	size_t mono_profiler_client_data::get_callstacks_count() const
	{
		return m_source->get_callstacks_count();
	}
}
