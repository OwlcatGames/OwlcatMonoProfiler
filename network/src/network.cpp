#include "network.h"

#include <deque>
#include <thread>
#include <mutex>

#include "concurrentqueue.h"

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


#ifdef DEBUG_NETWORK
		FILE* m_debug_file;
#endif
		bool m_stop = false;

		void run()
		{
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
			// We only allow one connection at a time. Network user is responsible for restarting listening if connection is terminated
			m_listening = false;

			read_message_header_from_socket();
		}

		void on_message_written(std::shared_ptr<message> msg, const asio::error_code& ec, size_t write_size)
		{
#ifdef DEBUG_NETWORK
#ifdef DEBUG_NETWORK_TEXT
			fprintf(m_debug_file, "Header: %i (%i) type=%i length=%i\n", write_count++, (msg->header.id), msg->header.type, msg->header.length);
			fprintf(m_debug_file, "Body: length=%I64u\n", msg->data.size());
			fwrite(msg->data.data(), msg->data.size(), 1, m_debug_file);
			fprintf(m_debug_file, "\n");
#else
			fwrite(&msg->header, sizeof(msg->header), 1, m_debug_file);
			fwrite(msg->data.data(), msg->data.size(), 1, m_debug_file);
#endif
			fflush(m_debug_file);
#endif
			set_disconnected_status(ec);
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

		void stop()
		{		
			m_stop = true;
			if (m_thread.joinable())
				m_thread.join();

			m_socket.close();
			m_acceptor.close();

			m_connected = connection_not_init;
		}

		void write_message(uint8_t type, uint32_t length, const uint8_t* data)
		{
			if (!m_socket.is_open())
				return;

			std::shared_ptr<message> msg = std::make_shared<message>(type, length, data);
			
			assert(msg->data.size() > 0);

			std::vector<asio::const_buffer> buffers{ asio::const_buffer(&msg->header, sizeof(message::header)), asio::const_buffer(msg->data.data(), msg->data.size()) };

			asio::async_write(m_socket, buffers, [this, msg](auto& ec, auto size) {/*on_message_written(msg, ec, size);*/ });

			//asio::async_write(m_socket, asio::buffer(&msg->header, sizeof(message::header)), [this, msg](auto& ec, auto size) {/*on_message_written(msg, ec, size);*/ });
			//asio::async_write(m_socket, asio::buffer(msg->data.data(), msg->data.size()), [this, msg](auto& ec, auto size) {on_message_written(msg, ec, size); });
		}

		bool read_message(message& msg)
		{
			return m_read_buffer.try_dequeue(msg);
		}

		size_t get_read_messages_count() const
		{
			return m_read_buffer.size_approx();
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
}
