#pragma once

#include <string>
#include <iterator>

/**
    \brief A helper class for safely reading data from a std::vector of bytes
*/
class memory_reader
{
public:
    memory_reader(const std::vector<uint8_t>& storage)
        : m_storage(storage)
    {}

    template<typename T>
    bool read(T& value)
    {
        const size_t value_size = sizeof(value);
        if (m_pos + value_size > m_storage.size())
            return false;

        memcpy(&value, &m_storage[m_pos], value_size);
        m_pos += value_size;
        return true;
    }

    bool read_uint32_be(uint32_t& value)
    {
        uint32_t value_be;
        if (!read_uint32(value_be))
            return false;

        value = (value_be & 0xFF) << 24;
        value = value | ((value_be & 0xFF00) << 8);
        value = value | ((value_be & 0xFF0000) >> 8);
        value = value | ((value_be & 0xFF000000) >> 24);

        return true;
    }

    bool read_uint8(uint8_t& value) { return read<uint8_t>(value); }
    bool read_uint16(uint16_t& value) { return read<uint16_t>(value); }
    bool read_uint32(uint32_t& value) { return read<uint32_t>(value); }
    bool read_uint64(uint64_t& value) { return read<uint64_t>(value); }
    bool read_varint(uint64_t& value)
    {
        uint8_t first_byte;
        if (!read_uint8(first_byte))
            return false;

        if (first_byte < 0xFDULL)
        {
            value = first_byte;
            return true;
        }
        else if(first_byte == 0xFD)
        {
            uint16_t value16;
            if (!read_uint16(value16))
                return false;
            value = value16;
        }
        else if(first_byte == 0xFE)
        {
            uint32_t value32;
            if (!read_uint32(value32))
                return false;
            value = value32;
        }
        else if(first_byte == 0xFF)
        {
            return read_uint64(value);
        }
        else
            return false;

        return true;
    }
    
    bool read_buffer(std::vector<uint8_t>& buffer, size_t length)
    {
        buffer.clear();
        if (m_pos + length > m_storage.size())
            return false;
        std::copy(m_storage.begin() + m_pos, m_storage.begin() + m_pos + length, std::back_inserter(buffer));
        m_pos += length;
        return true;
    }

    bool read_buffer(uint8_t* buffer, size_t length)
    {
        if (m_pos + length > m_storage.size())
            return false;
        // Pray that buffer has enough space
        memcpy(buffer, &m_storage[m_pos], length);
        m_pos += length;
        return true;
    }

    bool read_varint_length_and_bufffer(std::vector<uint8_t>& buffer)
    {
        uint64_t length;
        if (!read_varint(length))
            return false;
        return read_buffer(buffer, (size_t)length);
    }

    bool read_string(std::string& str)
    {
        uint64_t length;
        if (!read_varint(length))
            return false;

        if (m_pos + length > m_storage.size())
            return false;

        if (length > 0)
        {
            str.resize(length);
            memcpy(str.data(), &m_storage[m_pos], length);
            m_pos += length;
        }
        return true;
    }

    size_t get_pos() const { return m_pos; }

private:
    const std::vector<uint8_t>& m_storage;
    size_t m_pos = 0;
};
