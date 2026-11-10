#include "Ext/ExtensionIndex.h"

#include "Core/Log.h"
#include "Ext/AbstractExt.h"

#include <algorithm>
#include <cstdint>

namespace venture
{
ExtensionIndex& ExtensionIndex::Instance()
{
	// Function-local static: safe from static initializers of any translation unit.
	static ExtensionIndex index;
	return index;
}

bool ExtensionIndex::Attach(const AbstractClass* const owner, AbstractExt* const ext)
{
	if (!owner || !ext)
		return false;

	const auto key = static_cast<const void*>(owner);
	const auto [it, inserted] = m_index.emplace(key, ext);
	(void)it;
	if (!inserted)
	{
		// One extension per owner.  This is a programming error in the platform's
		// feature composition (two stores claimed the same owner class), so keep
		// the first instance and report failure to the caller.
		return false;
	}
	return true;
}

void ExtensionIndex::Detach(const AbstractClass* const owner)
{
	if (!owner)
		return;

	m_index.erase(static_cast<const void*>(owner));
}

AbstractExt* ExtensionIndex::Fetch(const AbstractClass* const owner) const
{
	if (!owner)
		return nullptr;

	const auto it = m_index.find(static_cast<const void*>(owner));
	return it == m_index.end() ? nullptr : it->second;
}

void ExtensionIndex::RegisterStore(const ExtStoreDesc* const desc)
{
	if (!desc)
	{
		Log::Error("extension store registration rejected: null descriptor");
		return;
	}

	if (desc->typeId == 0 || !desc->clear)
	{
		Log::Error("extension store registration rejected: store \"%s\" has typeId=0 or no clear callback",
			desc->name ? desc->name : "?");
		return;
	}

	for (const ExtStoreDesc* const existing : m_stores)
	{
		if (existing == desc)
			return; // idempotent re-registration of the same store

		if (existing->typeId == desc->typeId)
		{
			// A-10: the type id keys the store in the payload and in FindStore; two
			// stores with the same id would silently split ownership of a block
			// between them (the first registered store wins every lookup).  Keep the
			// original store and refuse the duplicate - this is a feature
			// composition error, not a recoverable condition.
			Log::Error("extension store registration rejected: type id %u is already registered by \"%s\" (duplicate store \"%s\")",
				static_cast<unsigned>(desc->typeId),
				existing->name ? existing->name : "?",
				desc->name ? desc->name : "?");
			return;
		}
	}

	m_stores.push_back(desc);
}

const ExtStoreDesc* ExtensionIndex::FindStore(const std::uint32_t typeId) const
{
	for (const ExtStoreDesc* const desc : m_stores)
	{
		if (desc->typeId == typeId)
			return desc;
	}
	return nullptr;
}

const std::vector<const ExtStoreDesc*>& ExtensionIndex::Stores() const
{
	return m_stores;
}

void ExtensionIndex::ClearAll()
{
	// The callbacks clear their own store; they must not touch this index's map
	// directly, so no iterator is invalidated here.
	for (const ExtStoreDesc* const desc : m_stores)
	{
		if (desc->clear)
			desc->clear();
	}

	m_index.clear();
}

bool ExtensionIndex::IsLoadingSaveGame() const
{
	return m_loadingSaveGame;
}

void ExtensionIndex::EnterLoadWindow()
{
	m_loadingSaveGame = true;
	// A load never starts with leftovers from a previous one.
	ClearDeferredAllocations();
	ClearLoadedExtensions();
}

void ExtensionIndex::ExitLoadWindow()
{
	m_loadingSaveGame = false;
}

void ExtensionIndex::ResetForScenarioClear()
{
	// A scenario change is also reachable after a failed savegame load, when the
	// engine returned from LoadGame before its normal close point and the deferred
	// records of the aborted load are still around: drop both, unconditionally.
	ExitLoadWindow();
	ClearDeferredAllocations();
}

void ExtensionIndex::DeferAllocation(const ExtStoreDesc* const desc, AbstractClass* const owner)
{
	if (!desc || !owner)
		return;

	for (const DeferredAllocation& existing : m_deferred)
	{
		if (existing.owner == owner)
			return;
	}

	m_deferred.push_back(DeferredAllocation{ desc, owner });
}

const std::vector<ExtensionIndex::DeferredAllocation>& ExtensionIndex::DeferredAllocations() const
{
	return m_deferred;
}

void ExtensionIndex::ClearDeferredAllocations()
{
	m_deferred.clear();
}

void ExtensionIndex::RegisterLoadedExtension(const ExtStoreDesc* const desc, AbstractExt* const ext)
{
	if (!desc || !ext)
		return;

	m_loaded.push_back(LoadedExtension{ desc, ext });
}

const std::vector<ExtensionIndex::LoadedExtension>& ExtensionIndex::LoadedExtensions() const
{
	return m_loaded;
}

void ExtensionIndex::ClearLoadedExtensions()
{
	m_loaded.clear();
}

void ExtensionIndex::NotifyInvalidPointer(const AbstractClass* const owner, const bool removed)
{
	// Extensions are removed by the owner's destructor hook, not here; this is the
	// subscription point for feature-owned pointer-keyed caches (and a diagnostic
	// counter for the base).
	(void)removed;
	if (!owner)
		return;

	++m_invalidPointerNotifications;

	// A-06: the engine announced that this owner pointer became invalid.  A
	// deferred allocation still recorded for it was created by an object that is
	// gone by the time the load window closes; FinishLoad must not resurrect an
	// extension on the stale address.
	m_deferred.erase(
		std::remove_if(m_deferred.begin(), m_deferred.end(),
			[owner](const DeferredAllocation& entry) { return entry.owner == owner; }),
		m_deferred.end());
}

std::size_t ExtensionIndex::StoreCount() const
{
	return m_stores.size();
}

std::size_t ExtensionIndex::ExtCount() const
{
	return m_index.size();
}

std::size_t ExtensionIndex::InvalidPointerNotifications() const
{
	return m_invalidPointerNotifications;
}
}
