#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Ext/AbstractExt.h"

class AbstractClass;

namespace venture
{
class ExtStreamWriter;
class ExtStreamReader;

// One registered extension store (see ExtensionStore.h).  The descriptor lives in
// the store object (static storage duration) and is registered the first time the
// store is used; the index keeps pointers to them so scenario clear and the
// savegame layer can walk every store without knowing its C++ type.
struct ExtStoreDesc
{
	std::uint32_t typeId = 0;       // extension class type id (unique per store)
	std::uint32_t dataVersion = 1;  // savegame format version of this store's rows
	const char* name = "store";     // diagnostic name, set by the feature
	bool saveable = false;          // participates in the savegame trailer
	void (*clear)() = nullptr;      // drop every instance of this store

	// Savegame layer.  All four are null for a store that does not participate;
	// a saveable store fills every one of them.
	void (*save)(ExtStreamWriter&) = nullptr;   // writes [u32 count][rows...]
	void (*load)(ExtStreamReader&) = nullptr;   // reads that block, creates instances
	bool (*adopt)(AbstractExt*) = nullptr;      // appends a loaded instance to the store
	void (*allocateDeferred)(AbstractClass*) = nullptr; // fresh object created during load
};

// Global, type-erased index over every live extension instance.
//
// ExtensionStore keeps its own typed map for fast access; this index exists so
// the base can answer "does this game object have an extension at all", so a base
// class view can fetch a derived instance, and so bulk operations (scenario clear,
// savegame serialization) can walk all stores without knowing their types.
//
// Keys are stored as const void* to keep this header free of engine headers.
class ExtensionIndex
{
public:
	struct DeferredAllocation
	{
		const ExtStoreDesc* desc;
		AbstractClass* owner;
	};

	struct LoadedExtension
	{
		const ExtStoreDesc* desc;
		AbstractExt* ext;
	};

	static ExtensionIndex& Instance();

	// --- polymorphic owner -> extension index -----------------------------
	// Attach fails (returns false) when the owner already has an extension: an
	// object may carry exactly one, so a platform with several feature modules
	// for the same owner class has to merge them into one extension class.
	bool Attach(const AbstractClass* owner, AbstractExt* ext);
	void Detach(const AbstractClass* owner);
	AbstractExt* Fetch(const AbstractClass* owner) const;

	// --- store registry ----------------------------------------------------
	void RegisterStore(const ExtStoreDesc* desc);
	const ExtStoreDesc* FindStore(std::uint32_t typeId) const;
	const std::vector<const ExtStoreDesc*>& Stores() const;
	std::size_t StoreCount() const;
	std::size_t ExtCount() const;

	// Scenario clear entry point: clears every registered store, then the index.
	void ClearAll();

	// --- savegame load coordination ---------------------------------------
	// While a savegame loads, extension allocation from object constructors is
	// deferred: the trailer may restore an extension for the same owner, and any
	// owner left over is a fresh object the game created itself.
	bool IsLoadingSaveGame() const;
	void EnterLoadWindow();
	void ExitLoadWindow();

	// Scenario clear / return to the main menu (A-04 fallback): closes a load
	// window the engine's failure exits may have left open and drops the deferred
	// records of a load that never finished, so the next scenario starts clean.
	void ResetForScenarioClear();

	void DeferAllocation(const ExtStoreDesc* desc, AbstractClass* owner);
	const std::vector<DeferredAllocation>& DeferredAllocations() const;
	void ClearDeferredAllocations();

	// Instances created from the trailer, adopted after the engine's swizzle pass.
	void RegisterLoadedExtension(const ExtStoreDesc* desc, AbstractExt* ext);
	const std::vector<LoadedExtension>& LoadedExtensions() const;
	void ClearLoadedExtensions();

	// Pointer invalidation notification hook. Extensions are owned by their
	// object's lifetime (ctor/dtor hooks), so they are not removed here; this
	// records a diagnostic count and is the hook a future subscriber API for
	// pointer-keyed feature caches would build on (there is none yet).  A
	// deferred allocation recorded for the invalidated owner is dropped (A-06):
	// the engine says the pointer is dead, so restoring or creating an extension
	// for it after the load window would use a stale address.
	void NotifyInvalidPointer(const AbstractClass* owner, bool removed);
	std::size_t InvalidPointerNotifications() const;

private:
	ExtensionIndex() = default;

	std::unordered_map<const void*, AbstractExt*> m_index;
	std::vector<const ExtStoreDesc*> m_stores;
	std::vector<DeferredAllocation> m_deferred;
	std::vector<LoadedExtension> m_loaded;
	std::size_t m_invalidPointerNotifications = 0;
	bool m_loadingSaveGame = false;
};

// Fetches the extension of `owner` when it is, or derives from, TExt.
template <typename TExt>
TExt* FetchExtAs(const AbstractClass* const owner)
{
	EnsureExtTypeRegistered<TExt::TypeId, TExt::BaseTypeId>();

	const AbstractExt* const ext = ExtensionIndex::Instance().Fetch(owner);
	if (!ext || !IsExtTypeDerivedFrom(ext->ExtTypeId(), TExt::TypeId))
		return nullptr;
	return static_cast<TExt*>(const_cast<AbstractExt*>(ext));
}
}
