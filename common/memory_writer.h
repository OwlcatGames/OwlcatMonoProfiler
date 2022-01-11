#pragma once

#include <iterator>
#include <string.h>

class memory_writer
{
public:
    memory_writer(std::vector<uint8_t>& storage)
        : m_storage(storage)
    {}

    template<typename T>
    void write(const T& value)
    {
        const size_t value_size = sizeof(value);
        size_t prev_size = m_storage.size();
        m_storage.resize(prev_size + value_size);
        uint8_t* current_end = &m_storage[prev_size];
        memcpy(current_end, &value, value_size);
    }

    void write_uint32_be(uint32_t value)
    {
        auto value_be = (value & 0xFF) << 24;
        value_be = value_be | ((value & 0xFF00) << 8);
        value_be = value_be | ((value & 0xFF0000) >> 8);
        value_be = value_be | ((value & 0xFF000000) >> 24);
        write_uint32(value_be);
    }

    void write_uint8(uint8_t value) { write<uint8_t>(value); }
    void write_uint16(uint16_t value) { write<uint16_t>(value); }
    void write_uint32(uint32_t value) { write<uint32_t>(value); }
    void write_uint64(uint64_t value) { write<uint64_t>(value); }
    void write_varint(uint64_t value)
    {
        if (value < 0xFDULL)
        {
            write_uint8((uint8_t)value);
        }
        else if(value < 0xFFFFULL)
        {
            write_uint8(0xFD);
            write_uint16((uint16_t)value);
        }
        else if(value < 0xFFFFFFFFULL)
        {
            write_uint8(0xFE);
            write_uint32((uint32_t)value);
        }
        else
        {
            write_uint8(0xFF);
            write_uint64(value);
        }
    }
    
    void write_buffer(const std::vector<uint8_t>& buffer)
    {
        std::copy(buffer.begin(), buffer.end(), std::back_inserter(m_storage));
    }

    void write_buffer_reversed(const std::vector<uint8_t>& buffer)
    {
        std::copy(buffer.rbegin(), buffer.rend(), std::back_inserter(m_storage));
    }

    void write_buffer(const uint8_t* buffer, size_t length)
    {
        std::copy(buffer, buffer+length, std::back_inserter(m_storage));
    }

    void write_varint_length_and_bufffer(const std::vector<uint8_t>& buffer)
    {
        write_varint(buffer.size());
        write_buffer(buffer);
    }

    void write_string(const char *str)
    {
        auto length = strlen(str);
        write_varint(length);
        std::copy(str, str + length, std::back_inserter(m_storage));
    }

private:
    std::vector<uint8_t>& m_storage;
};
