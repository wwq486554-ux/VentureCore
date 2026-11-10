#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "Ext/AbstractExt.h"
#include "Ext/ExtensionIndex.h"
#include "Ext/ExtStream.h"
#include "Ext/ExtSwizzle.h"

class AbstractClass;

namespace venture
{
// Typed owner -> extension store.
//
// One store per (game class, extension class) pair; the extension class is the
// platform's most derived one for that owner class (see ExtHierarchy.h).  The
// store keeps
//   * an **intrusive doubly linked list** of its instances in allocation order
//     (`AbstractExt::StoreNext/StorePrev`): deterministic savegame order and
//     O(1) removal, with no separate container node allocation,
//   * a map owner -> extension for O(1) typed Fetch and Remove,
//   * a one-entry Fetch cache: features fetch the same owner repeatedly between
//     two events (per frame, per shot), so the most recent (owner, extension)
//     pair is answered without touching the map.
//
// The store registers a descriptor with ExtensionIndex on first use, so scenario
// clear and the savegame layer see it without a central registration list.  A
// feature module owns its store instance:
//
//   class MyUnitExt final : public UnitExt { ... };
//   using MyStore = ExtensionStore<UnitClass, MyUnitExt>;
//   MyStore& store = MyStore::instance();
//   store.SetName("UnitClass/MyUnitExt");
//   store.Reserve(expectedLiveCount); // optional, avoids map rehashing
//   store.SetSaveable(true);          // state survives save/load
//
// Extension instances are owned by the store; a feature never deletes one itself.
template <typename TOwner, typename TExt>
class ExtensionStore
{
	static_assert(std::is_base_of_v<AbstractExt, TExt>, "TExt must derive from AbstractExt");
	static_assert(std::is_base_of_v<AbstractClass, TOwner>, "TOwner must derive from AbstractClass");
	static_assert(TExt::TypeId >= AbstractExt::FirstUserTypeId,
		"a platform extension class must use TypeId >= AbstractExt::FirstUserTypeId (0x0100)");
	static_assert(std::is_constructible_v<TExt, AbstractClass*>,
		"an extension class constructor must accept AbstractClass* (the store also constructs it while loading a savegame)");

public:
	static ExtensionStore& Instance()
	{
		static ExtensionStore store;
		return store;
	}

	// Diagnostic name shown by logs/documentation ("UnitClass/ExampleUnitExt").
	void SetName(const char* const name)
	{
		if (name && *name)
			m_desc.name = name;
	}

	// Marks the store as participating in the savegame trailer.  `dataVersion`
	// identifies the byte layout of the store's rows; bump it when a field is
	// added/removed so old saves are rejected for this store instead of being
	// misread.
	void SetSaveable(const bool saveable, const std::uint32_t dataVersion = 1)
	{
		m_desc.saveable = saveable;
		m_desc.dataVersion = dataVersion;
	}

	const ExtStoreDesc& Descriptor() const { return m_desc; }

	// Pre-sizes the owner map for an expected number of live instances.  Optional;
	// removes the rehash spikes while a battle creates thousands of objects.
	void Reserve(const std::size_t expectedLiveCount)
	{
		m_map.reserve(expectedLiveCount);
	}

	// Allocates the extension for owner if it has none and returns it.  Safe to
	// call for an owner that already has one (returns the existing instance).
	// While a savegame is loading the allocation is deferred (the trailer may
	// restore an extension for the same owner): returns nullptr.
	TExt* Allocate(TOwner* const owner)
	{
		if (!owner)
			return nullptr;

		if (TExt* const existing = this->Fetch(owner))
			return existing;

		if (ExtensionIndex::Instance().IsLoadingSaveGame())
		{
			ExtensionIndex::Instance().DeferAllocation(&m_desc, owner);
			return nullptr;
		}

		return AllocateNew(owner);
	}

	// Removes and destroys the extension of owner, if any. No-op otherwise.
	// O(1): unlink from the allocation-order list, erase the map entry, detach
	// from the global index.
	void Remove(TOwner* const owner)
	{
		if (!owner)
			return;

		const auto it = m_map.find(static_cast<const void*>(owner));
		if (it == m_map.end())
			return;

		TExt* const ext = it->second;
		m_map.erase(it);
		Unlink(ext);
		ExtensionIndex::Instance().Detach(owner);
		delete ext;
	}

	TExt* Fetch(const TOwner* const owner) const
	{
		if (!owner)
			return nullptr;

		if (owner == m_lastOwner)
			return m_lastExt;

		const auto it = m_map.find(static_cast<const void*>(owner));
		TExt* const ext = it == m_map.end() ? nullptr : it->second;

		m_lastOwner = owner;
		m_lastExt = ext;
		return ext;
	}

	// Drops every extension in this store (scenario clear).
	void Clear()
	{
		TExt* node = m_head;
		while (node)
		{
			TExt* const next = static_cast<TExt*>(node->StoreNext());
			ExtensionIndex::Instance().Detach(node->Owner());
			delete node;
			node = next;
		}

		m_head = nullptr;
		m_tail = nullptr;
		m_size = 0;
		m_map.clear();
		InvalidateFetchCache();
	}

	std::size_t Size() const
	{
		return m_size;
	}

private:
	// Canary written into every saved row: rejects a trailer whose store rows
	// come from a different extension class layout.
	static constexpr std::uint32_t kCanary = 0x56430000u | (TExt::TypeId & 0xFFFFu);

	ExtensionStore()
	{
		RegisterExtType(TExt::TypeId, TExt::BaseTypeId);
		m_desc.typeId = TExt::TypeId;
		m_desc.clear = &ClearCallback;
		m_desc.count = &CountCallback;
		m_desc.save = &SaveCallback;
		m_desc.load = &LoadCallback;
		m_desc.adopt = &AdoptCallback;
		m_desc.allocateDeferred = &AllocateDeferredCallback;
		m_map.reserve(64);
		ExtensionIndex::Instance().RegisterStore(&m_desc);
	}

	void InvalidateFetchCache() const
	{
		m_lastOwner = nullptr;
		m_lastExt = nullptr;
	}

	TExt* AllocateNew(TOwner* const owner)
	{
		auto* const ext = new TExt(owner);

		if (!ExtensionIndex::Instance().Attach(owner, ext))
		{
			// The owner already carries an extension of another store: one object
			// may only have one.  Report the composition error through the return
			// value; the caller should log it.
			delete ext;
			return nullptr;
		}

		InsertItem(owner, ext);
		return ext;
	}

	// Appends to the allocation-order list and the owner map.
	void InsertItem(const AbstractClass* const owner, TExt* const ext)
	{
		ext->SetStorePrev(m_tail);
		ext->SetStoreNext(nullptr);
		if (m_tail)
			m_tail->SetStoreNext(ext);
		else
			m_head = ext;
		m_tail = ext;
		++m_size;

		m_map.emplace(static_cast<const void*>(owner), ext);
		InvalidateFetchCache();
	}

	void Unlink(TExt* const ext)
	{
		AbstractExt* const prev = ext->StorePrev();
		AbstractExt* const next = ext->StoreNext();

		if (prev)
			prev->SetStoreNext(next);
		else
			m_head = static_cast<TExt*>(next);

		if (next)
			next->SetStorePrev(prev);
		else
			m_tail = static_cast<TExt*>(prev);

		ext->SetStorePrev(nullptr);
		ext->SetStoreNext(nullptr);
		--m_size;
		InvalidateFetchCache();
	}

	static void ClearCallback()
	{
		Instance().Clear();
	}

	static std::size_t CountCallback()
	{
		return Instance().Size();
	}

	// --- savegame layer ----------------------------------------------------

	static void SaveCallback(ExtStreamWriter& stream)
	{
		const ExtensionStore& store = Instance();
		stream.WriteU32(static_cast<std::uint32_t>(store.m_size));

		for (const AbstractExt* node = store.m_head; node != nullptr; node = node->StoreNext())
		{
			const auto* const ext = static_cast<const TExt*>(node);

			// The extension's own save-time address: another saved pointer may
			// refer to it, so the loader registers it as an identity change.
			stream.WritePointer(ext);
			// The owner's save-time address: the loader registers the owner slot
			// for the engine's swizzle pass.
			stream.WritePointer(ext->Owner());
			stream.WriteU32(kCanary);

			ExtByteStreamWriter data;
			ext->Save(data);
			stream.WriteU32(static_cast<std::uint32_t>(data.Data().size()));
			if (!data.Data().empty())
				stream.Write(data.Data().data(), data.Data().size());
		}
	}

	static void LoadCallback(ExtStreamReader& stream)
	{
		std::uint32_t count = 0;
		if (!stream.ReadU32(count))
			return;

		for (std::uint32_t i = 0; i < count; ++i)
		{
			std::uint32_t extSaveAddress = 0;
			std::uint32_t ownerSaveAddress = 0;
			std::uint32_t canary = 0;
			std::uint32_t dataSize = 0;

			if (!stream.ReadU32(extSaveAddress) || !stream.ReadU32(ownerSaveAddress)
				|| !stream.ReadU32(canary) || !stream.ReadU32(dataSize))
				return;

			if (canary != kCanary)
			{
				// Wrong extension class / format: reject the whole block.
				stream.Fail();
				return;
			}

			// A-02/W5b: the row data size is savegame-controlled.  Validate it
			// before allocating: it must fit in the bytes the store block still
			// carries (the block itself was already bounded by ReadPayload).
			if (dataSize > stream.Remaining())
			{
				stream.Fail();
				return;
			}

			std::vector<std::uint8_t> data(dataSize);
			if (dataSize != 0 && !stream.ReadBytes(data.data(), dataSize))
				return;

			// Construct with the save-time owner address; the swizzle pass rewrites
			// the owner slot to the loaded object before PostLoad().
			AbstractClass* const rawOwner = reinterpret_cast<AbstractClass*>(
				static_cast<std::uintptr_t>(ownerSaveAddress));
			auto* const ext = new TExt(rawOwner);

			// While the payload is read, ExtSwizzle collects these registrations
			// in a staging buffer instead of handing them to the engine: if a later
			// row/block fails, ExtSave::ReadPayload discards the staged entries
			// *before* the instances created here are deleted, so the engine's
			// remap pass can never write through a pointer to a deleted extension.
			ExtSwizzle::RegisterIdentity(reinterpret_cast<void*>(static_cast<std::uintptr_t>(extSaveAddress)), ext);
			ExtSwizzle::RegisterForChange(ext->OwnerSlot());

			ExtByteStreamReader rowReader(data.data(), data.size());
			ext->Load(rowReader);
			if (!rowReader.Ok())
			{
				delete ext;
				stream.Fail();
				return;
			}

			ExtensionIndex::Instance().RegisterLoadedExtension(&Instance().m_desc, ext);
		}
	}

	static bool AdoptCallback(AbstractExt* const ext)
	{
		if (!ext)
			return false;

		auto* const typed = static_cast<TExt*>(ext);
		AbstractClass* const owner = typed->Owner();
		if (!owner)
			return false;

		if (!ExtensionIndex::Instance().Attach(owner, typed))
			return false;

		Instance().InsertItem(owner, typed);
		return true;
	}

	static void AllocateDeferredCallback(AbstractClass* const owner)
	{
		if (!owner)
			return;

		TExt* const ext = Instance().AllocateNew(static_cast<TOwner*>(owner));
		if (ext)
			ext->OnDeferredAllocation();
	}

	ExtStoreDesc m_desc{};
	std::unordered_map<const void*, TExt*> m_map;
	TExt* m_head = nullptr;   // allocation order, oldest first
	TExt* m_tail = nullptr;   // allocation order, newest last
	std::size_t m_size = 0;

	// Single-entry Fetch cache.  Mutable because Fetch is logically const; the
	// game runs the platform hooks on one thread.
	mutable const TOwner* m_lastOwner = nullptr;
	mutable TExt* m_lastExt = nullptr;
};
}
