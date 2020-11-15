#pragma once

#include <cstdint>
#include <vector>
#include <string>

//#define DEBUG_NETWORK
//
//#ifdef DEBUG_NETWORK
//	#define DEBUG_NETWORK_TEXT
//#endif

namespace owlcat
{
	namespace protocol
	{
		enum message
		{
			SRV_ALLOC = 1,
			SRV_FREE,
			SRV_REFERENCES,
			SRV_PAUSE,
			SRV_RESUME,
		};

		enum command
		{
			CMD_REFERENCES = 1,
			CMD_PAUSE,
			CMD_RESUME,
		};
	}

	struct message
	{
#ifdef DEBUG_NETWORK
static uint32_t next_id;
#endif
		message() {}
		message(uint8_t type, uint32_t length, const uint8_t* data)
		{
#ifdef DEBUG_NETWORK
			header.id = next_id++;
#endif
			header.type = type;
			header.length = length;
			this->data.assign(data, data + length);
		}

		struct header
		{
#ifdef DEBUG_NETWORK
uint32_t id;
#endif
			uint32_t length = 0;
			uint8_t type = 0;
		};

		header header;
		std::vector<uint8_t> data;
	};

	class network
	{
		class details;
		details* m_details;

	public:
		network();
		~network();

		void listen_async(int port);
		void listen_sync(int port);
		bool connect(const std::string& address, int port);

		bool is_connected() const;
		bool is_connecting() const;
		bool is_listening() const;

		void stop();

		void write_message(uint8_t type, uint32_t length, const uint8_t* data);
		bool read_message(message& msg);

		size_t get_read_messages_count() const;
	};
}
