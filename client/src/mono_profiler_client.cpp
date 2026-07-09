#include "mono_profiler_client.h"
#include "network.h"
#include "memory_reader.h"
#include "memory_writer.h"
#include "persistent_storage.h"
#include "db_migrations.h"
#include "db_queries.h"
#include "event_log.h"
#include "capture_container.h"

#include <memory>
#include <string>
#include <thread>
#include <filesystem>
#include <cassert>
#include <atomic>
#include <map>
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
		// Database ids (already translated from server-side ids when the message was read)
		uint64_t type_id;
		uint64_t callstack_id;
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

		// Total number of events stored (written to the event log). Atomic: updated on
		// the processing thread, read from the UI thread to display the storage rate.
		std::atomic<uint64_t> m_db_inserted_events = 0;

		// The event log of the current capture: events live here, not in the database
		// (see event_log.h)
		event_log_writer m_event_log;
		std::string m_event_log_file_name;
		// Byte offset in the event log where the current frame's events begin
		uint64_t m_current_frame_begin = 0;

		// Files extracted from an opened capture container into a temporary directory,
		// and the directory itself; cleaned up when the capture is closed
		std::vector<std::string> m_extracted_files;
		std::string m_extract_dir;

		void cleanup_extracted_files()
		{
			std::error_code ec;
			for (auto& file : m_extracted_files)
				std::filesystem::remove(file, ec);
			m_extracted_files.clear();

			if (!m_extract_dir.empty())
			{
				std::filesystem::remove(m_extract_dir, ec);
				m_extract_dir.clear();
			}
		}

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

		// Maps from server-side ids (as sent over the wire) to database ids.
		// The server interns by class/method pointers and can report different ids
		// for the same name (e.g. different instantiations of a generic type), and its
		// id space restarts when the profiler in the game restarts. Database ids are
		// canonicalized by name, so we translate every server id on arrival.
		std::unordered_map<uint64_t, uint64_t> m_server_type_map;
		std::unordered_map<uint64_t, uint64_t> m_server_callstack_map;

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

		// Translates a server-side type id into a database id
		uint64_t translate_server_type_id(uint64_t server_id)
		{
			auto iter = m_server_type_map.find(server_id);
			if (iter != m_server_type_map.end())
				return iter->second;

			// Should never happen: definitions always precede allocations that reference them
			printf("Allocation references unknown type id %llu\n", (unsigned long long)server_id);
			uint64_t type_id = get_or_create_type_id("<unknown type>");
			m_server_type_map.emplace(server_id, type_id);
			return type_id;
		}

		// Translates a server-side callstack id into a database id
		uint64_t translate_server_callstack_id(uint64_t server_id)
		{
			auto iter = m_server_callstack_map.find(server_id);
			if (iter != m_server_callstack_map.end())
				return iter->second;

			// Should never happen: definitions always precede allocations that reference them
			printf("Allocation references unknown callstack id %llu\n", (unsigned long long)server_id);
			uint64_t callstack_id = get_or_create_callstack_id("<unknown callstack>");
			m_server_callstack_map.emplace(server_id, callstack_id);
			return callstack_id;
		}

		// Saves events from the current frame: the events themselves go to the event
		// log file, the frame's stats and its byte range in the log go to the database
		void save_frame_events()
		{
			// There should be no events before we receice the first frame
			if (m_prev_frame == 0xFFFFFFFFFFFFFFFF)
			{
#ifdef WIN32
				if (!m_frame_events.empty())
					__debugbreak();
#endif
				return;
			}

			for (auto& e : m_frame_events)
				m_event_log.append(e.frame, e.addr, e.type_id, e.callstack_id, e.size, e.type == profiler_event::alloc);

			// The events must hit the file before the frame's byte range is published
			// to the database, or a concurrent reader could read past the valid data
			m_event_log.flush();

			queries::insert_frame_stats(m_db, m_prev_frame, m_frame_allocs, m_frame_frees, m_size_running_total, m_current_frame_begin, m_event_log.position());

			m_db_inserted_events += m_frame_events.size();

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
				m_current_frame_begin = m_event_log.position();

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
					uint64_t server_type_id;
					uint64_t server_callstack_id;
					bool all_ok =
						reader.read_uint64(frame) &&
						reader.read_uint64(addr) &&
						reader.read_uint32(size) &&
						reader.read_varint(server_type_id) &&
						reader.read_varint(server_callstack_id);

#ifdef WIN32
					// Frames should always be sequential
					if (frame < m_prev_frame && m_prev_frame != 0xFFFFFFFFFFFFFFFF)
						__debugbreak();
#endif

					if (all_ok)
					{
						try_save_events(frame);
						m_frame_events.push_back({profiler_event::alloc, frame, addr, size, translate_server_type_id(server_type_id), translate_server_callstack_id(server_callstack_id)});
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
						reader.read_uint64(frame) &&
						reader.read_uint64(addr) &&
						reader.read_uint32(size);

					if (all_ok)
					{
						try_save_events(frame);
						m_frame_events.push_back({ profiler_event::free, frame, addr, size, 0, 0 });		
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
				else if (msg.header.type == protocol::message::SRV_TYPE)
				{
					uint64_t server_id;
					std::string name;
					bool all_ok =
						reader.read_varint(server_id) &&
						reader.read_string(name);

					if (all_ok)
						m_server_type_map[server_id] = get_or_create_type_id(name);
					else
						printf("Received type definition, but msg is broken\n");
				}
				else if (msg.header.type == protocol::message::SRV_CALLSTACK)
				{
					uint64_t server_id;
					std::string callstack;
					bool all_ok =
						reader.read_varint(server_id) &&
						reader.read_string(callstack);

					if (all_ok)
						m_server_callstack_map[server_id] = get_or_create_callstack_id(callstack);
					else
						printf("Received callstack definition, but msg is broken\n");
				}
				else
				{
					printf("Received bad message\n");
#ifdef WIN32
					__debugbreak();
#endif
				}

				// Avoid storing too many events in queue
				if (m_frame_events.size() > 100000)
					save_frame_events();
			}

			// Submit last events before quitting. save_frame_events is called directly:
			// all buffered events belong to the current frame, so try_save_events would
			// consider them already saved and drop them.
			save_frame_events();
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
			cleanup_extracted_files();
		}

#if defined(WIN32)
		mono_profiler_client::LaunchResult launch_executable(const std::string& executable, const std::string& commandline, int port, const std::string& db_file_name, const std::string& dll_location, uint32_t capture_flags, const std::string& native_config)
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

			// The port must be known to the injected DLL before we connect, so it goes through
			// the environment (the child inherits it). Everything else about the capture is
			// sent over the connection (see start -> CMD_CONFIGURE). This also finally makes
			// the port configurable (the DLL used to hardcode 8888).
			char port_str[16];
			sprintf(port_str, "%d", port);
			SetEnvironmentVariableA("OWLCAT_PROFILER_PORT", port_str);

			bool created = DetourCreateProcessWithDlls(executable.c_str(), (char*)commandline.c_str(), nullptr, nullptr, true, CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi, 1, &dllLoc, nullptr);

			// Don't leave it lingering in our own environment
			SetEnvironmentVariableA("OWLCAT_PROFILER_PORT", nullptr);

			if (!created)
			{
				int error_code = GetLastError();
				return mono_profiler_client::DETOUR_CREATE_WITH_DLL_FAILED;
			}

			ResumeThread(pi.hThread);
			
			auto start_time = std::chrono::system_clock::now();
			HANDLE pipe = INVALID_HANDLE_VALUE;
			while (std::chrono::system_clock::now() - start_time < std::chrono::seconds(15))
			{
				pipe = CreateFile(owlcat::protocol::pipe_name, GENERIC_READ | FILE_WRITE_ATTRIBUTES | FILE_FLAG_WRITE_THROUGH, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if (pipe != INVALID_HANDLE_VALUE)
				{
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}

			if (pipe == INVALID_HANDLE_VALUE)
				return mono_profiler_client::DETOUR_PIPE_TIMEOUT;

			DWORD dwMode = PIPE_READMODE_MESSAGE;
			if (!SetNamedPipeHandleState(pipe, &dwMode, NULL, NULL))
				return mono_profiler_client::DETOUR_PIPE_FAILED;

			char error_code[16];
			DWORD read_bytes = 0;
			if (!ReadFile(pipe, error_code, sizeof(error_code), &read_bytes, NULL))
				return mono_profiler_client::DETOUR_PIPE_READ_FAILED;

			if (strncmp(error_code, owlcat::protocol::error_symbols, sizeof(error_code)) == 0)
				return mono_profiler_client::PDB_NOT_FOUND;
			if (strncmp(error_code, owlcat::protocol::error_detour, sizeof(error_code)) == 0)
				return mono_profiler_client::DETOUR_FAILED;
			if (strncmp(error_code, owlcat::protocol::error_deque, sizeof(error_code)) == 0)
				return mono_profiler_client::BAD_VERSION;
			if (strncmp(error_code, owlcat::protocol::error_detour_late, sizeof(error_code)) == 0)
				return mono_profiler_client::DETOUR_FAILED_LATE;

			// We only support launching applications on the same computer, so use loopback IP
			return start("127.0.0.1", port, db_file_name.c_str(), capture_flags, native_config) ? mono_profiler_client::OK : mono_profiler_client::CONNECT_FAILED;
		}
#else
		mono_profiler_client::LaunchResult launch_executable(const std::string& executable, const std::string& commandline, int port, const std::string& db_file_name, const std::string& dll_location)
		{
			std::filesystem::path exec_path(executable);
			std::string cwd = exec_path.parent_path().string();

			return mono_profiler_client::CONNECT_FAILED;
		}
#endif

		bool start(const std::string& addr, int server_port, const std::string& db_file_name, uint32_t capture_flags, const std::string& native_config)
		{
			m_network_settings.addr = addr;
			m_network_settings.port = server_port;

			if (!m_network.connect(addr, server_port))
				return false;

			// Tell the server what to capture. This is the first thing sent; the in-game
			// profiler defers its startup (and native hooking) until it receives this.
			{
				std::vector<uint8_t> cmd;
				memory_writer writer(cmd);
				writer.write_uint32(capture_flags);
				writer.write_string(native_config.c_str());
				m_network.write_message(protocol::command::CMD_CONFIGURE, (uint32_t)cmd.size(), cmd.data());
			}

			m_db.close();

			// This is a new session with a new database: reset id maps and frame-tracking
			// state possibly left over from a previous session or a previously opened database
			m_type_to_id_map.clear();
			m_id_to_type_map.clear();
			m_callstacks_to_id_map.clear();
			m_id_to_callstacks_map.clear();
			m_server_type_map.clear();
			m_server_callstack_map.clear();
			m_next_type_id = 0;
			m_next_callstack_id = 0;

			m_frame_events.clear();
			m_prev_frame = 0xFFFFFFFFFFFFFFFF;
			m_frame_allocs = 0;
			m_frame_frees = 0;
			m_size_running_total = 0;
			m_has_min_frame = false;
			m_min_frame = 0;
			m_max_frame = 0;
			m_stop = false;
			m_db_inserted_events = 0;

			m_event_log.close();
			cleanup_extracted_files();

			m_db_file_name = db_file_name;

			// Remove existing database (if we use a non-temporary one)
			if (std::filesystem::exists(m_db_file_name))
				std::filesystem::remove(m_db_file_name);
			if (!m_db.open(m_db_file_name, true))
				return false;

			m_db.pragma("PRAGMA locking_mode = EXCLUSIVE");
			m_db.pragma("PRAGMA foreign_keys = OFF");
			m_db.pragma("PRAGMA recursive_triggers = OFF");
			// WAL performance is not good enough
			//m_db.pragma("PRAGMA journal_mode=wal");			
			//m_db.pragma("PRAGMA journal_mode=memory");
			m_db.pragma("PRAGMA journal_mode=OFF");
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

			// The events themselves are stored in the event log file next to the
			// database (see event_log.h)
			m_event_log_file_name = m_db_file_name + ".events";
			if (std::filesystem::exists(m_event_log_file_name))
				std::filesystem::remove(m_event_log_file_name);
			if (!m_event_log.create(m_event_log_file_name))
				return false;
			m_current_frame_begin = m_event_log.position();

			m_thread = std::thread(&mono_profiler_client::details::process_messages, this);

			return true;
		}

		void stop()
		{
			m_stop = true;
			if (m_thread.joinable())
				m_thread.join();
			m_network.stop();

			// No more events will be written. The log file stays on disk: readers
			// open it by name for analysis queries.
			m_event_log.close();
		}

		void close_db()
		{
			assert(!m_thread.joinable() && (!is_connected() || is_connecting()));
			m_event_log.close();
			m_db.close();
			cleanup_extracted_files();
		}

		bool save_db(const std::string& new_db_file_name, bool move)
		{
			// Saving is only possible when the capture is stopped
			if (m_thread.joinable())
				return false;

			// The working files stay in place and open: saving packs a snapshot of them
			// into a single container file. The database is snapshotted through the
			// backup API first, because the live connection can hold dirty pages that
			// are not in the file yet.
			const std::string db_snapshot = new_db_file_name + ".dbtmp";
			if (!m_db.save(db_snapshot))
				return false;

			std::map<std::string, std::string> files;
			files[capture_container::entry_database] = db_snapshot;
			files[capture_container::entry_events] = m_event_log_file_name;
			bool ok = capture_container::pack(new_db_file_name, files);

			std::error_code ec;
			std::filesystem::remove(db_snapshot, ec);

			return ok;
		}

		bool open_data(const std::string& file)
		{
			if (m_thread.joinable())
				return false;

			m_event_log.close();
			if (m_db.is_open())
				m_db.close();
			cleanup_extracted_files();

			// Don't mix ids from a previously opened database or session
			m_type_to_id_map.clear();
			m_id_to_type_map.clear();
			m_callstacks_to_id_map.clear();
			m_id_to_callstacks_map.clear();

			// Unpack the container into a temporary directory: SQLite needs a plain
			// file, and the event log is addressed by byte offsets
			std::error_code ec;
			auto temp_root = std::filesystem::temp_directory_path(ec) / "OwlcatMonoProfiler";
			char unique_name[64];
			sprintf(unique_name, "capture_%llu", (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count());
			auto extract_dir = temp_root / unique_name;
			std::filesystem::create_directories(extract_dir, ec);
			if (ec)
				return false;

			std::map<std::string, std::string> extracted;
			if (!capture_container::unpack(file, extract_dir.string(), extracted))
				return false;

			auto db_entry = extracted.find(capture_container::entry_database);
			auto events_entry = extracted.find(capture_container::entry_events);
			if (db_entry == extracted.end() || events_entry == extracted.end())
				return false;

			m_extract_dir = extract_dir.string();
			for (auto& entry : extracted)
				m_extracted_files.push_back(entry.second);

			m_db_file_name = db_entry->second;
			m_event_log_file_name = events_entry->second;

			return m_db.open(m_db_file_name, false) && upgrade_database(m_db) && queries::register_queries(m_db) && load_types_and_callstacks();
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

		bool get_allocation_type_and_stack(uint64_t address, uint64_t& type_id, uint64_t& stack_id)
		{
			// Only the byte range published to the database is guaranteed to be readable
			auto range_cursor = queries::select_frame_event_range(m_db, 0, (uint64_t)INT64_MAX);
			if (range_cursor.has_error() || !range_cursor.next())
				return false;

			uint64_t end_offset = range_cursor.get_uint64("end_offset");

			auto reader = event_log_reader::open(m_event_log_file_name);
			if (reader == nullptr)
				return false;

			event_view found;
			if (!reader->find_last_allocation(address, end_offset, found))
				return false;

			type_id = found.type_id;
			stack_id = found.callstack_id;

			return true;
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

			// Find the byte range of the requested frames in the event log
			auto range_cursor = queries::select_frame_event_range(m_db, from_frame, to_frame);
			if (range_cursor.has_error() || !range_cursor.next())
				return;

			uint64_t begin_offset = range_cursor.get_uint64("begin_offset");
			uint64_t end_offset = range_cursor.get_uint64("end_offset");

			auto reader = event_log_reader::open(m_event_log_file_name);
			if (reader == nullptr)
				return;

			uint64_t events_count = reader->count_events(begin_offset, end_offset);
			uint64_t row_num = 0;
			bool cancelled = false;

			reader->read_range(begin_offset, end_offset, [&](const event_view& e)
			{
				// Allocation: write object to map. Deallocation: remove object from map.
				if (e.is_alloc)
					live_objects_map.emplace(e.addr, live_object(e.addr, e.size, e.frame, e.type_id, e.callstack_id));
				else
					live_objects_map.erase(e.addr);

				if (progress_func != nullptr && !progress_func((size_t)row_num++, (size_t)events_count))
				{
					cancelled = true;
					return false;
				}

				return true;
			});

			if (cancelled)
				return;

			// Flatten the map
			for (auto& pair : live_objects_map)
			{
				objects.push_back(pair.second);
			}
		}

		size_t get_network_messages_count() const { return m_network.get_read_messages_count(); }

		uint64_t get_db_inserted_events_count() const { return m_db_inserted_events; }

		const char* get_event_log_path() const { return m_event_log_file_name.c_str(); }
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
	mono_profiler_client::LaunchResult mono_profiler_client::launch_executable(const std::string& executable, const std::string& args, int port, const std::string& db_file_name, const std::string& dll_location, uint32_t capture_flags, const std::string& native_config)
	{
		return m_details->launch_executable(executable, args, port, db_file_name, dll_location, capture_flags, native_config);
	}

	bool mono_profiler_client::start(const std::string& addr, int server_port, const std::string& db_file_name, uint32_t capture_flags, const std::string& native_config)
	{
		return m_details->start(addr, server_port, db_file_name, capture_flags, native_config);
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

	uint64_t mono_profiler_client::get_db_inserted_events_count() const
	{
		return m_details->get_db_inserted_events_count();
	}

	const char* mono_profiler_client::get_event_log_path() const
	{
		return m_details->get_event_log_path();
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

	bool mono_profiler_client_data::get_allocation_type_and_stack(uint64_t address, uint64_t& type_id, uint64_t& stack_id) const
	{
		return m_source->get_allocation_type_and_stack(address, type_id, stack_id);
	}
}
