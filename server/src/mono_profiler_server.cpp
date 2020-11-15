#include "mono_profiler_server.h"
#include "mono_profiler.h"

#include "network.h"
#include "logger.h"

#include <memory>
#include <thread>

#include <memory_writer.h>
#include <memory_reader.h>

#if defined(WIN32) || defined(WIN64)
extern bool g_is_detoured;
#endif

namespace owlcat
{
	class mono_profiler_server::details
	{
		/*
			Class that handles reporting profiler events to client over network
		*/
		class network_events_sink : public events_sink
		{
			network& m_network;

		public:
			network_events_sink(network& network) : m_network(network) {}

			virtual void report_alloc(uint64_t frame, uint64_t addr, uint32_t size, const char* full_name, const char* callstack) override
			{
				if (!m_network.is_connected())
					return;

				static std::vector<uint8_t> data;
				data.reserve(5 * 1024);
				data.clear();
				memory_writer writer(data);
				writer.write_uint64(frame);
				writer.write_uint64(addr);
				writer.write_uint32(size);
				writer.write_string(full_name);
				writer.write_string(callstack);

				m_network.write_message(protocol::message::SRV_ALLOC, (uint32_t)data.size(), (uint8_t*)&data[0]);
			}

			virtual void report_free(uint64_t frame, uint64_t addr, uint32_t size) override
			{
				if (!m_network.is_connected())
					return;

				static std::vector<uint8_t> data;
				data.reserve(32);
				data.clear();
				memory_writer writer(data);
				writer.write_uint64(frame);
				writer.write_uint64(addr);
				writer.write_uint32(size);

				m_network.write_message(protocol::message::SRV_FREE, (uint32_t)data.size(), (uint8_t*)&data[0]);
			}

			virtual void report_references(uint64_t request_id, const std::vector<object_references_t>& references) override
			{
				if (!m_network.is_connected())
					return;

				static std::vector<uint8_t> data;
				data.reserve(1024);
				data.clear();				
				memory_writer writer(data);
				
				writer.write_uint64(request_id);
				writer.write_varint(references.size());
				for (auto& refs : references)
				{
					writer.write_varint(refs.addr);
					writer.write_string(refs.type.c_str());
					writer.write_varint(refs.parents.size());
					for(auto& parent : refs.parents)
						writer.write_varint(parent);
				}

				m_network.write_message(protocol::message::SRV_REFERENCES, (uint32_t)data.size(), (uint8_t*)&data[0]);
			}

			virtual void report_paused(uint64_t request_id, bool ok) override
			{
				if (!m_network.is_connected())
					return;

				std::vector<uint8_t> data;
				data.clear();
				memory_writer writer(data);

				writer.write_uint64(request_id);
				writer.write_uint8(ok ? 0 : 1);

				m_network.write_message(protocol::message::SRV_PAUSE, (uint32_t)data.size(), (uint8_t*)&data[0]);
			}

			virtual void report_resumed(uint64_t request_id, bool ok) override
			{
				if (!m_network.is_connected())
					return;

				std::vector<uint8_t> data;
				data.clear();
				memory_writer writer(data);

				writer.write_uint64(request_id);
				writer.write_uint8(ok ? 0 : 1);

				m_network.write_message(protocol::message::SRV_RESUME, (uint32_t)data.size(), (uint8_t*)&data[0]);
			}
		};

		network m_network;
		network_events_sink m_sink;
		mono_profiler m_profiler;

		std::thread m_watchdog;
		bool m_stop_watchdog = false;
		bool m_wait_for_connection = false;
		int m_port = 0;

		std::thread m_commands_thread;
		bool m_stop_commands_thread = false;

	public:
		details()
			: m_sink(m_network)
			, m_profiler(&m_sink)
		{
		}

		~details()
		{
			m_stop_watchdog = true;
			if (m_watchdog.joinable())
				m_watchdog.join();
			m_stop_commands_thread = true;
			if (m_commands_thread.joinable())
				m_commands_thread.join();

			m_network.stop();
		}

		/*
			Starts the profiler and networking. If wait_for_connection is true,
			will block the main thread until connection with client is established.			
		*/
		void start(bool wait_for_connection, int port)
		{
			m_wait_for_connection = wait_for_connection;
			m_port = port;

			if (!m_network.is_connected())
			{
				if (wait_for_connection)
					m_network.listen_sync(port);
				else
					m_network.listen_async(port);
			}

			// Stop watchdog thread if we're restarting
			m_stop_watchdog = true;
			if (m_watchdog.joinable())
				m_watchdog.join();
			m_stop_watchdog = false;

			// Stop commands thread if we're restarting
			m_stop_commands_thread = true;
			if (m_commands_thread.joinable())
				m_commands_thread.join();
			m_stop_commands_thread = false;

			// Create watchdog thread. It's sole purpose is to watch for network disconnects and enter
			// listening mode if this happens
			m_watchdog = std::thread([this]()
				{					
					while (!m_stop_watchdog)
					{
						if (!m_network.is_connected() && !m_network.is_listening())
						{
							m_network.stop();

							if (m_wait_for_connection)
								m_network.listen_sync(m_port);
							else
								m_network.listen_async(m_port);
						}

						std::this_thread::sleep_for(std::chrono::milliseconds(500));
					}
				});

			// Start commands thread. It reads commands from client
			m_commands_thread = std::thread(&details::process_messages, this);

			// Start the profiler
			m_profiler.start();
		}

		void process_messages()
		{
			while (!m_stop_commands_thread)
			{
				message msg;
				if (!m_network.read_message(msg))
				{
					std::this_thread::yield();
					continue;
				}

				memory_reader reader(msg.data);

				if (msg.header.type == protocol::command::CMD_REFERENCES)
				{					
					uint64_t request_id;
					uint64_t count;
					std::vector<uint64_t> adddresses;
					reader.read_uint64(request_id);
					reader.read_uint64(count);
					for (uint64_t i = 0; i < count; ++i)
					{
						uint64_t addr;
						reader.read_uint64(addr);
						adddresses.push_back(addr);
					}

					m_profiler.find_references(request_id, adddresses);
				}
				else if (msg.header.type == protocol::command::CMD_PAUSE)
				{
					uint64_t request_id;
					reader.read_uint64(request_id);

					m_profiler.pause_app(request_id);
				}
				else if (msg.header.type == protocol::command::CMD_RESUME)
				{
					uint64_t request_id;
					reader.read_uint64(request_id);

					m_profiler.resume_app(request_id);
				}
			}
		}

		void stop()
		{
			m_stop_watchdog = true;
			if (m_watchdog.joinable())
				m_watchdog.join();

			m_network.stop();			
		}

		void on_frame()
		{
			m_profiler.on_frame();
		}
	};

	mono_profiler_server::mono_profiler_server()
	{
		m_details = new details();
	}

	mono_profiler_server::~mono_profiler_server()
	{
		delete m_details;
	}

	void mono_profiler_server::start(bool wait_for_connection, int port)
	{
		m_details->start(wait_for_connection, port);
	}

	void mono_profiler_server::stop()
	{
		m_details->stop();
	}

	void mono_profiler_server::on_frame()
	{
		m_details->on_frame();
	}
}
