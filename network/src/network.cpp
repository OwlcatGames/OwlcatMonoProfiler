#include "network.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <thread>
#include <mutex>

#include "concurrentqueue.h"
#include "profiler_thread.h"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/error_code.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

namespace owlcat
{
#ifdef DEBUG_NETWORK
	uint32_t message::next_id = 0;
#endif

	namespace protocol
	{
		const char* pipe_name = "\\\\.\\pipe\\OwlcatMonoProfiler";
		const char* error_ok = "OK";
		const char* error_symbols = "SYMBOLS";
		const char* error_detour = "DETOUR";
		const char* error_deque = "DEQUE";
		const char* error_detour_late = "DETOUR2";
	}

	class network::details
	{
		moodycamel::ConcurrentQueue<message> m_read_buffer;

#ifdef DEBUG_NETWORK
		int write_count = 0, read_count = 0;
#endif

		std::thread m_thread;

		asio::io_context m_context;

		asio::ip::tcp::acceptor m_acceptor;
		asio::ip::tcp::socket m_socket;
		asio::ip::tcp::endpoint m_endpoint;

		enum connection_status
		{
			connection_not_init,
			connection_connected,
			connection_disconnected,
		};

		connection_status m_connected = connection_not_init;
		bool m_listening = false;
		// Incremented on every new connection. Read from other threads to detect reconnects.
		std::atomic<uint64_t> m_generation{0};

		/*
			Writing is batched: write_message only appends the message to m_pending_writes,
			and at most one async_write is in flight at any time (overlapping async_writes
			on one socket are not allowed by ASIO anyway). When a write completes, everything
			that accumulated in the meantime is sent as one buffer. This turns thousands of
			tiny per-event writes per frame into a few large ones.
		*/
		std::mutex m_write_mutex;
		// Messages accumulated while the current write is in flight
		std::vector<uint8_t> m_pending_writes;
		// The buffer currently being sent
		std::vector<uint8_t> m_writing;
		// True if an async_write is in flight
		bool m_write_in_progress = false;
		// Total bytes currently buffered on the send side (pending + in-flight). Maintained
		// under m_write_mutex, but read lock-free for the profiler's back-pressure decisions.
		std::atomic<uint64_t> m_buffered_bytes{ 0 };


#ifdef DEBUG_NETWORK
		FILE* m_debug_file;
#endif
		bool m_stop = false;

		void run()
		{
			// Profiler-owned thread: its allocations (send/receive buffers, asio internals)
			// must not be recorded by native hooks
			t_profiler_internal_thread = true;

			while (!m_stop)
				m_context.run_for(std::chrono::seconds(1));
		}

		//message m_current_message;

		void set_disconnected_status(const asio::error_code& ec)
		{
			if (ec == asio::error::eof || ec == asio::error::connection_reset || ec == asio::error::connection_aborted)
				m_connected = connection_disconnected;
		}

		void on_body_read(std::shared_ptr<message> msg, const asio::error_code& ec, size_t read_size)
		{
			if (ec)
			{
				set_disconnected_status(ec);
				return;
			}

			//if (read_size != m_current_message.header.length)
			//	return;

			if (read_size != msg->header.length)
				return;

#ifdef DEBUG_NETWORK
	#ifdef DEBUG_NETWORK_TEXT
			fprintf(m_debug_file, "Body: length=%I64u\n", read_size);
			//fwrite(m_current_message.data.data(), read_size, 1, m_debug_file);
			fwrite(msg->data.data(), read_size, 1, m_debug_file);
			fprintf(m_debug_file, "\n");
	#else
			fwrite(m_current_message.data.data(), read_size, 1, m_debug_file);
	#endif
			fflush(m_debug_file);
#endif			

			//m_read_buffer.enqueue(m_current_message);
			m_read_buffer.enqueue(*msg);
			read_message_header_from_socket();
		}

		void on_header_read(std::shared_ptr<message> msg, const asio::error_code& ec, size_t read_size)
		{
			if (ec)
			{
				set_disconnected_status(ec);
				return;
			}

			if (read_size != sizeof(message::header))
				return;

#ifdef DEBUG_NETWORK
#ifdef DEBUG_NETWORK_TEXT
			//fprintf(m_debug_file, "Header: %i type=%i length=%i\n", read_count++, m_current_message.header.type, m_current_message.header.length);
			fprintf(m_debug_file, "Header: %i (%i) type=%i length=%i\n", read_count++, msg->header.id, msg->header.type, msg->header.length);
#else
			fwrite(&m_current_message.header, sizeof(m_current_message.header), 1, m_debug_file);
#endif
			fflush(m_debug_file);
#endif			

			//assert(m_current_message.header.length > 0);
			//m_current_message.data.resize(m_current_message.header.length);

			//asio::async_read(m_socket, asio::buffer(&m_current_message.data[0], m_current_message.data.size()), [this](auto& ec, auto size) {on_body_read(ec, size); });

			assert(msg->header.length > 0);
			msg->data.resize(msg->header.length);

			asio::async_read(m_socket, asio::buffer(&msg->data[0], msg->data.size()), [this, msg](auto& ec, auto size) {on_body_read(msg, ec, size); });
		}

		void read_message_header_from_socket()
		{
			//m_current_message = message();
			std::shared_ptr<message> msg = std::make_shared<message>();
			//asio::async_read(m_socket, asio::buffer(&m_current_message.header, sizeof(message::header)), [this](auto& ec, auto size) {on_header_read(ec, size); });
			asio::async_read(m_socket, asio::buffer(&msg->header, sizeof(message::header)), [this, msg](auto& ec, auto size) {on_header_read(msg, ec, size); });
		}

		void on_new_connection(const asio::error_code& ec)
		{
			if (ec)
			{
				printf("Listen error: %i (%s)\n", ec.value(), ec.message().c_str());
				m_connected = connection_disconnected;
				return;
			}
			
			m_socket.non_blocking(true);
			m_connected = connection_connected;
			++m_generation;
			// We only allow one connection at a time. Network user is responsible for restarting listening if connection is terminated
			m_listening = false;

			read_message_header_from_socket();
		}

		// Starts sending everything accumulated in m_pending_writes.
		// Called on the network thread only.
		void start_write()
		{
			{
				std::scoped_lock lock(m_write_mutex);
				m_writing.clear();
				if (m_pending_writes.empty())
				{
					m_write_in_progress = false;
					m_buffered_bytes.store(0, std::memory_order_relaxed);
					return;
				}
				m_writing.swap(m_pending_writes);
				// pending is now empty; the buffered bytes are the in-flight m_writing
				m_buffered_bytes.store(m_writing.size(), std::memory_order_relaxed);
			}

			asio::async_write(m_socket, asio::buffer(m_writing.data(), m_writing.size()), [this](const asio::error_code& ec, size_t) { on_write_complete(ec); });
		}

		void on_write_complete(const asio::error_code& ec)
		{
			if (ec)
			{
				set_disconnected_status(ec);

				std::scoped_lock lock(m_write_mutex);
				m_write_in_progress = false;
				m_pending_writes.clear();
				m_buffered_bytes.store(0, std::memory_order_relaxed);
				return;
			}

			// Send whatever has accumulated while this write was in flight
			start_write();
		}

	public:
		details()
			: m_socket(m_context)
			, m_acceptor(m_context)
		{
#ifdef DEBUG_NETWORK
			m_debug_file = fopen("netlog.bin", "w");
#endif
		}

		~details()
		{
			stop();
		}

		void listen_async(int port)
		{
			// stop() might have been called before (e.g. when restarting listening after
			// a disconnect), so reset the stop flag and the context
			m_stop = false;
			m_context.restart();

			m_listening = true;

			m_endpoint = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port);
			m_acceptor.open(m_endpoint.protocol());
			m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
			m_acceptor.bind(m_endpoint);
			m_acceptor.listen();
			m_acceptor.async_accept(m_socket, [this](const asio::error_code& ec) { on_new_connection(ec); });

			m_thread = std::thread([this]() {run(); });
		}

		void listen_sync(int port)
		{
			// stop() might have been called before (e.g. when restarting listening after
			// a disconnect), so reset the stop flag and the context
			m_stop = false;
			m_context.restart();

			m_listening = true;

			m_endpoint = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port);
			m_acceptor.open(m_endpoint.protocol());
			m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
			m_acceptor.bind(m_endpoint);
			m_acceptor.listen();
			asio::error_code ec;
			m_acceptor.accept(m_socket, ec);
			
			on_new_connection(ec);

			m_thread = std::thread([this]() {run(); });
		}
		
		bool connect(const std::string& address, int port)
		{
			// stop() might have been called before, so reset the stop flag and the context
			m_stop = false;
			m_context.restart();

			m_endpoint = asio::ip::tcp::endpoint(asio::ip::make_address(address), port);

			asio::error_code ec;
			m_socket.connect(m_endpoint, ec);
			m_socket.non_blocking(true);
			if (ec)
			{
				printf("Failed to connect: %i (%s)\n", ec.value(), ec.message().c_str());
				return false;
			}

			read_message_header_from_socket();

			m_thread = std::thread([this]() {run(); });

			return true;
		}

		bool is_connected() const
		{			
			return m_socket.is_open() && m_connected == connection_connected;
		}

		bool is_connecting() const
		{
			return m_connected == connection_not_init;
		}

		bool is_listening() const
		{
			return m_acceptor.is_open() && m_listening;
		}

		uint64_t connection_generation() const
		{
			return m_generation;
		}

		void stop()
		{
			m_stop = true;
			if (m_thread.joinable())
				m_thread.join();

			m_socket.close();
			m_acceptor.close();

			// Run down any handlers that are still queued (e.g. cancelled writes),
			// so they can't fire during the next session. The network thread is
			// stopped, so it's safe to do it here.
			m_context.restart();
			m_context.poll();

			m_write_in_progress = false;
			m_pending_writes.clear();
			m_writing.clear();
			m_buffered_bytes.store(0, std::memory_order_relaxed);

			m_connected = connection_not_init;
		}

		void write_message(uint8_t type, uint32_t length, const uint8_t* data)
		{
			if (!m_socket.is_open())
				return;

			assert(length > 0);

			bool kick_writer = false;
			{
				std::scoped_lock lock(m_write_mutex);

				// "struct" is required: message has both a nested type and a member named "header"
				struct message::header hdr{};
#ifdef DEBUG_NETWORK
				hdr.id = message::next_id++;
#endif
				hdr.length = length;
				hdr.type = type;

				size_t old_size = m_pending_writes.size();
				m_pending_writes.resize(old_size + sizeof(hdr) + length);
				memcpy(m_pending_writes.data() + old_size, &hdr, sizeof(hdr));
				memcpy(m_pending_writes.data() + old_size + sizeof(hdr), data, length);
				m_buffered_bytes.store(m_pending_writes.size() + m_writing.size(), std::memory_order_relaxed);

				if (!m_write_in_progress)
				{
					m_write_in_progress = true;
					kick_writer = true;
				}
			}

			// All socket operations must happen on the network thread
			if (kick_writer)
				asio::post(m_context, [this]() { start_write(); });
		}

		bool read_message(message& msg)
		{
			return m_read_buffer.try_dequeue(msg);
		}

		size_t get_read_messages_count() const
		{
			return m_read_buffer.size_approx();
		}

		size_t get_pending_write_bytes()
		{
			// Lock-free: read on the profiler's hot path (back-pressure) and by MEMLOG.
			return (size_t)m_buffered_bytes.load(std::memory_order_relaxed);
		}
	};

	network::network()
	{
		m_details = new details();
	}

	network::~network()
	{
		delete m_details;
	}

	void network::listen_async(int port)
	{
		m_details->listen_async(port);
	}

	void network::listen_sync(int port)
	{
		m_details->listen_sync(port);
	}

	bool network::connect(const std::string& address, int port)
	{
		return m_details->connect(address, port);
	}

	bool network::is_connected() const
	{
		return m_details->is_connected();
	}

	bool network::is_connecting() const
	{
		return m_details->is_connecting();
	}

	bool network::is_listening() const
	{
		return m_details->is_listening();
	}

	uint64_t network::connection_generation() const
	{
		return m_details->connection_generation();
	}

	void network::stop()
	{
		m_details->stop();
	}

	void network::write_message(uint8_t type, uint32_t length, const uint8_t* data)
	{
		m_details->write_message(type, length, data);
	}

	bool network::read_message(message& msg)
	{
		return m_details->read_message(msg);
	}

	size_t network::get_read_messages_count() const
	{
		return m_details->get_read_messages_count();
	}

	size_t network::get_pending_write_bytes() const
	{
		return m_details->get_pending_write_bytes();
	}
}
