#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "Core/Log.h"
#include "Ext/AbstractExt.h"
#include "Ext/ExtensionIndex.h"
#include "Ext/ExtSave.h"
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
	static_assert(TExt::TypeId <= 0xFFFFu,
		"the savegame row canary keeps the low 16 bits of TypeId - keep extension type ids <= 0xFFFF");
	static_assert(std::is_constructible_v<TExt, AbstractClass*>,
		"an extension class constructor must accept AbstractClass* (the store also constructs it while loading a savegame)");
	// SAVE-08: the owner map is keyed by `const void*` so one map type serves both
	// the typed lookups (Fetch/Remove cast a TOwner*) and the savegame adopt path
	// (InsertItem/AdoptCallback receive an AbstractClass*).  The two addresses are
	// equal only while AbstractClass is TOwner's primary base at offset 0 - true for
	// every engine class used as TOwner today (they derive from AbstractClass as
	// their first/only base).  This cannot be asserted at compile time in standard
	// C++ for a polymorphic class (there is no compile-time base-offset query, and
	// offsetof is only standard for standard-layout types), so it stays a documented
	// assumption: if a future TOwner puts AbstractClass behind another base, the
	// adopt path would insert a key that Fetch never matches.  Revisit this comment
	// (and the casts in Fetch/Remove/InsertItem) if the hierarchy ever changes.

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
	// misread.  The version is part of the row canary, which keeps its low 16
	// bits (see RowCanary()).
	void SetSaveable(const bool saveable, const std::uint32_t dataVersion = 1)
	{
		assert(dataVersion <= 0xFFFFu
			&& "the savegame row canary keeps the low 16 bits of dataVersion - keep it small");
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
	//
	// SAVE-03: a feature extension's destructor runs arbitrary feature code and
	// may re-enter the store (Fetch / Remove / Clear).  The list and the owner map
	// are therefore detached *before* the first instance is deleted, so a
	// destructor can never observe a half-torn-down store: Fetch answers nullptr,
	// Remove is a no-op and a nested Clear is empty.  (Remove() has always erased
	// the map entry and invalidated the cache before deleting for the same
	// reason.)  Instances a destructor allocates while the store is being cleared
	// are drained by the next round - bounded, because a store-teardown loop must
	// not be steered by feature code.
	void Clear()
	{
		for (int round = 0; round < kMaxClearRounds && m_head != nullptr; ++round)
		{
			std::vector<TExt*> instances;
			instances.reserve(m_size);

			for (TExt* node = m_head; node != nullptr; node = static_cast<TExt*>(node->StoreNext()))
				instances.push_back(node);

			// Detach the whole store first: map lookups/caches must not see the
			// nodes that are about to be deleted.
			m_head = nullptr;
			m_tail = nullptr;
			m_size = 0;
			m_map.clear();
			InvalidateFetchCache();

			for (TExt* const ext : instances)
			{
				ExtensionIndex::Instance().Detach(ext->Owner());
				delete ext;
			}
		}
	}

	std::size_t Size() const
	{
		return m_size;
	}

private:
	// SAVE-03: number of detach+delete rounds Clear() runs at most.  One round is
	// the normal case; the extra ones only drain instances a destructor allocated
	// while the store was being torn down.
	static constexpr int kMaxClearRounds = 4;

	// Canary written into every saved row.  It identifies the extension class AND
	// the store's row layout version, so a Save()/Load() drift (fields added or
	// removed without bumping dataVersion) is rejected instead of misread:
	//
	//   RowCanary(version) = 0x56430000 ^ (TypeId & 0xFFFF) ^ ((version & 0xFFFF) << 16)
	//
	// Low 16 bits are the TypeId, high 16 bits the row version with the "VC" magic
	// folded into the high half.  The mapping is injective for TypeId and
	// dataVersion that fit 16 bits (enforced by the static_assert/assert above);
	// docs/SaveFormat.md documents the formula.
	static constexpr std::uint32_t RowCanary(const std::uint32_t dataVersion)
	{
		return 0x56430000u ^ (static_cast<std::uint32_t>(TExt::TypeId) & 0xFFFFu)
			^ ((dataVersion & 0xFFFFu) << 16);
	}

	ExtensionStore()
	{
		RegisterExtType(TExt::TypeId, TExt::BaseTypeId);
		m_desc.typeId = TExt::TypeId;
		m_desc.clear = &ClearCallback;
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
			stream.WriteU32(RowCanary(store.m_desc.dataVersion));

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

		// SAVE-05: rowCount is savegame-controlled.  Reject an impossible row
		// count *before creating a single instance*: every row occupies at least
		// MinRowSize serialized bytes, and one store never legitimately carries
		// more than MaxRowsPerStore rows.  Without this bound a 16 MiB payload
		// (~1,048,573 minimal rows) would create one extension object + one engine
		// identity entry per row - a load-time memory/sort denial-of-service.
		if (count > ExtSave::MaxRowsPerStore
			|| static_cast<std::size_t>(count) > stream.Remaining() / ExtSave::MinRowSize)
		{
			Log::Warn("savegame: store \"%s\" block rejected - rowCount %u exceeds the load limit "
				"(%u row(s) per store, or more rows than the remaining %zu byte(s) can hold)",
				Instance().m_desc.name ? Instance().m_desc.name : "?",
				static_cast<unsigned>(count),
				static_cast<unsigned>(ExtSave::MaxRowsPerStore),
				stream.Remaining());
			stream.Fail();
			return;
		}

		for (std::uint32_t i = 0; i < count; ++i)
		{
			std::uint32_t extSaveAddress = 0;
			std::uint32_t ownerSaveAddress = 0;
			std::uint32_t canary = 0;
			std::uint32_t dataSize = 0;

			if (!stream.ReadU32(extSaveAddress) || !stream.ReadU32(ownerSaveAddress)
				|| !stream.ReadU32(canary) || !stream.ReadU32(dataSize))
				return;

			if (canary != RowCanary(Instance().m_desc.dataVersion))
			{
				// Wrong extension class / row layout version: reject the whole block.
				stream.Fail();
				return;
			}

			// A-02/W5b: the row data size is savegame-controlled.  Validate it
			// before allocating: it must fit in the bytes the store block still
			// carries (the block itself was already bounded by ReadPayload) and in
			// the per-row cap (SAVE-05 - one row's data can never legitimately need
			// the whole payload cap).
			if (dataSize > ExtSave::MaxRowDataSize || dataSize > stream.Remaining())
			{
				Log::Warn("savegame: store \"%s\" row rejected - dataSize %u exceeds the load limit "
					"(%u byte(s) per row, or more than the remaining %zu byte(s))",
					Instance().m_desc.name ? Instance().m_desc.name : "?",
					static_cast<unsigned>(dataSize),
					static_cast<unsigned>(ExtSave::MaxRowDataSize),
					stream.Remaining());
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
			// A-09: the row must be consumed exactly.  A Load() that reads fewer
			// bytes than Save() wrote is a layout drift; trailing bytes in the row
			// (or a short read) mean the row cannot be interpreted safely, so the
			// whole payload is rejected instead of silently restored in part.
			if (!rowReader.Ok() || rowReader.Remaining() != 0)
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
	// Owner -> extension.  Keyed by const void*: Fetch/Remove pass a TOwner* and
	// InsertItem/AdoptCallback an AbstractClass*; both must be the same address
	// (SAVE-08 - see the note at the top of the class).
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
