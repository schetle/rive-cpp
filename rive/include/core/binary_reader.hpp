
#ifndef _RIVE_CORE_BINARY_READER_HPP_
#define _RIVE_CORE_BINARY_READER_HPP_

#include <string>

namespace rive
{
	class BinaryReader
	{
    private:
        uint8_t* m_Position;
        uint8_t* m_End;
        bool m_Overflowed;
        size_t m_Length;

        void overflow();

    public:
        BinaryReader(uint8_t* bytes, size_t length);
        bool didOverflow() const;

        size_t lengthInBytes() const;

        uint64_t readVarUint();
        int64_t readVarInt();
        std::string readString(bool explicitLength = false);
        double readFloat64();
        float readFloat32();
        uint8_t readByte();
        BinaryReader read(size_t length);
	};
} // namespace rive

#endif