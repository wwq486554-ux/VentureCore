#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "Ext/ExtSwizzle.h"

namespace venture
{
// ---------------------------------------------------------------------------
// Byte streams used by the extension savegame layer.
//
// An extension with state overrides AbstractExt::Save(ExtStreamWriter&) and
// AbstractExt::Load(ExtStreamReader&) and writes/reads its fields through these
// helpers.  Pointers to game objects/other extensions are written with
// WritePointer() and read with ReadPointerForChange(): the save-time value is
// stored in the field and the field itself is registered with the engine's
// swizzle manager, which rewrites it to the loaded address before PostLoad().
//
// The interfaces are pure C++ so the same serializers are unit-tested with the
// in-memory ByteStream implementations below.
// ---------------------------------------------------------------------------
class ExtStreamWriter
{
public:
	ExtStreamWriter() = default;
	virtual ~ExtStreamWriter() = default;

	ExtStreamWriter(const ExtStreamWriter&) = delete;
	ExtStreamWriter& operator=(const ExtStreamWriter&) = delete;

	virtual void Write(const void* data, std::size_t size) = 0;
	virtual std::size_t Offset() const = 0;

	void WriteU8(const std::uint8_t value) { Write(&value, sizeof(value)); }
	void WriteU16(const std::uint16_t value) { Write(&value, sizeof(value)); }
	void WriteU32(const std::uint32_t value) { Write(&value, sizeof(value)); }
	void WriteI32(const std::int32_t value) { Write(&value, sizeof(value)); }
	void WriteF32(const float value) { Write(&value, sizeof(value)); }
	void WriteBool(const bool value) { WriteU8(value ? 1u : 0u); }

	void WritePointer(const void* const pointer)
	{
		WriteU32(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer)));
	}

	template <typename T>
	void WritePod(const T& value)
	{
		static_assert(std::is_trivially_copyable_v<T>, "WritePod needs a trivially copyable type");
		Write(&value, sizeof(value));
	}
};

class ExtStreamReader
{
public:
	ExtStreamReader() = default;
	virtual ~ExtStreamReader() = default;

	ExtStreamReader(const ExtStreamReader&) = delete;
	ExtStreamReader& operator=(const ExtStreamReader&) = delete;

	virtual bool ReadBytes(void* data, std::size_t size) = 0;
	virtual std::size_t Offset() const = 0;
	virtual std::size_t Remaining() const = 0;

	bool Ok() const { return m_ok; }
	void Fail() { m_ok = false; }

	bool ReadU8(std::uint8_t& value) { return ReadBytes(&value, sizeof(value)); }
	bool ReadU16(std::uint16_t& value) { return ReadBytes(&value, sizeof(value)); }
	bool ReadU32(std::uint32_t& value) { return ReadBytes(&value, sizeof(value)); }
	bool ReadI32(std::int32_t& value) { return ReadBytes(&value, sizeof(value)); }
	bool ReadF32(float& value) { return ReadBytes(&value, sizeof(value)); }

	bool ReadBool(bool& value)
	{
		std::uint8_t raw = 0;
		if (!ReadU8(raw))
			return false;
		value = raw != 0;
		return true;
	}

	// Reads a save-time pointer into `slot` and registers the slot for swizzling.
	bool ReadPointerForChange(void** const slot)
	{
		std::uint32_t raw = 0;
		if (!ReadU32(raw))
			return false;

		*slot = reinterpret_cast<void*>(static_cast<std::uintptr_t>(raw));
		ExtSwizzle::RegisterForChange(slot);
		return true;
	}

	template <typename T>
	bool ReadPod(T& value)
	{
		static_assert(std::is_trivially_copyable_v<T>, "ReadPod needs a trivially copyable type");
		return ReadBytes(&value, sizeof(value));
	}

protected:
	void MarkFailed() { m_ok = false; }

private:
	bool m_ok = true;
};

// In-memory writer, used for one store's block and by the unit tests.
class ExtByteStreamWriter final : public ExtStreamWriter
{
public:
	void Write(const void* const data, const std::size_t size) override
	{
		const auto* const bytes = static_cast<const std::uint8_t*>(data);
		m_data.insert(m_data.end(), bytes, bytes + size);
	}

	std::size_t Offset() const override { return m_data.size(); }

	const std::vector<std::uint8_t>& Data() const { return m_data; }
	std::vector<std::uint8_t>& Data() { return m_data; }

private:
	std::vector<std::uint8_t> m_data;
};

// Bounds-checked in-memory reader; all accessors fail once it runs past the end.
class ExtByteStreamReader final : public ExtStreamReader
{
public:
	ExtByteStreamReader(const std::uint8_t* const data, const std::size_t size) :
		m_data(data), m_size(size), m_offset(0)
	{
	}

	bool ReadBytes(void* const data, const std::size_t size) override
	{
		if (m_offset + size > m_size)
		{
			MarkFailed();
			return false;
		}

		if (size != 0)
			std::memcpy(data, m_data + m_offset, size);
		m_offset += size;
		return true;
	}

	std::size_t Offset() const override { return m_offset; }
	std::size_t Remaining() const override { return m_size - m_offset; }

private:
	const std::uint8_t* m_data;
	std::size_t m_size;
	std::size_t m_offset;
};

// Standard IEEE 802.3 CRC-32 (reflected, polynomial 0xEDB88320).  Used to guard
// the savegame trailer payload; implemented here so the format is testable
// without the engine and stable across compiler updates.
inline std::uint32_t ExtCrc32(const void* const data, const std::size_t size)
{
	struct Table
	{
		std::uint32_t entries[256];

		Table()
		{
			for (std::uint32_t i = 0; i < 256; ++i)
			{
				std::uint32_t value = i;
				for (int bit = 0; bit < 8; ++bit)
					value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
				entries[i] = value;
			}
		}
	};

	static const Table table;

	const auto* const bytes = static_cast<const std::uint8_t*>(data);
	std::uint32_t crc = 0xFFFFFFFFu;
	for (std::size_t i = 0; i < size; ++i)
		crc = table.entries[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}
}
