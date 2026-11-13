#pragma once

#include <cstddef>
#include <cstdint>

class AbstractClass;

namespace venture
{
class ExtStreamWriter;
class ExtStreamReader;

// ---------------------------------------------------------------------------
// Extension storage - root of the extension hierarchy.
//
// Extensions are stored outside the game objects (a self-built index keyed by the
// owner pointer), not in the engine's unused AbstractClass slot: the platform must
// coexist with Phobos-lineage DLLs, and those own that slot.
//
// Exactly ONE extension instance exists per owner object.  It is an instance of
// the *most derived* class in the hierarchy the platform declares (see
// ExtHierarchy.h), and every base view reaches that same instance through the
// global index + the runtime type-id chain below.  That mirrors the call shape of
// the platform this base is meant to host (`UnitExt::Fetch(unit)`,
// `TechnoExt::Fetch(techno)`) without copying its container implementation.
//
// Lifetime is owned by the platform's lifecycle hooks (object ctor/dtor, scenario
// clear, savegame load) - a feature never deletes an extension itself.
// ---------------------------------------------------------------------------
class AbstractExt
{
public:
	// Type-id chain.  The base hierarchy uses 0x0001..0x00FF (see
	// ExtHierarchy.h); feature modules must start at FirstUserTypeId.
	static constexpr std::uint32_t TypeId = 0x0001u;
	static constexpr std::uint32_t BaseTypeId = 0u;

	static constexpr std::uint32_t FirstUserTypeId = 0x0100u;

	explicit AbstractExt(AbstractClass* const owner) : m_owner(owner) {}
	virtual ~AbstractExt() = default;

	AbstractExt(const AbstractExt&) = delete;
	AbstractExt& operator=(const AbstractExt&) = delete;

	// Actual type of this instance; a derived extension class overrides it with
	// its own TypeId.  FetchExtAs<> validates with it (the DLL is built with
	// /GR-, so there is no RTTI and no dynamic_cast).
	virtual std::uint32_t ExtTypeId() const { return TypeId; }

	AbstractClass* Owner() const { return m_owner; }

	// Load path only: the instance is constructed with the save-time owner
	// pointer, that pointer is registered with the swizzle manager, and the
	// engine rewrites the field to the loaded owner before PostLoad().
	void SetOwner(AbstractClass* const owner) { m_owner = owner; }
	void** OwnerSlot() { return reinterpret_cast<void**>(&m_owner); }

	// Savegame hooks (see ExtStream.h).  An extension without state can leave the
	// defaults; one with state overrides both.
	virtual void Save(ExtStreamWriter& stream) const { (void)stream; }
	virtual void Load(ExtStreamReader& stream) { (void)stream; }

	// --- allocation-order links -------------------------------------------
	// The owning ExtensionStore keeps a doubly linked list of its instances in
	// allocation order (deterministic savegame order, O(1) removal).  An
	// extension belongs to exactly one store, so the links live here instead of
	// in a separate container node - that keeps the per-instance heap
	// allocations to "extension + map node".
	AbstractExt* StoreNext() const { return m_storeNext; }
	AbstractExt* StorePrev() const { return m_storePrev; }
	void SetStoreNext(AbstractExt* const next) { m_storeNext = next; }
	void SetStorePrev(AbstractExt* const prev) { m_storePrev = prev; }

	// Called once every pointer of the extension has been swizzled.
	virtual void PostLoad() {}

	// Called when the engine created the owner while a savegame was loading and
	// the savegame carried no data for it: the fresh-object path.
	virtual void OnDeferredAllocation() {}

private:
	AbstractClass* m_owner;
	AbstractExt* m_storeNext = nullptr;
	AbstractExt* m_storePrev = nullptr;
};

// Registers one (type, base) pair of the extension hierarchy.  Idempotent for a
// repeated identical pair.  A feature extension class only has to call this once
// - EnsureExtTypeRegistered<> below does it from Fetch/store use.
//
// The registration is validated (D-22): the type id must be >=
// FirstUserTypeId, the type cannot be its own base, and a type id that was
// already registered with a different base is rejected.  A rejected feature type
// id is poisoned - IsExtTypeDerivedFrom() reports false for it, so a broken
// composition fails at the lookup (FetchExtAs returns nullptr) instead of
// producing a wrong static_cast.  The built-in ids seeded for AbstractExt /
// TechnoExt / FootExt / UnitExt (0x0001-0x0004) are never poisoned: they are
// registered before any feature runs, so a conflicting feature declaration of
// one of them is merely rejected and the built-in base views of every real
// instance keep working instead of one bad feature breaking them all (SAVE-04).
// The problem is reported through the sink (SetExtTypeProblemSink), which
// Platform::Startup() points at the logger.
// Returns true when the pair is registered (or was already).
bool RegisterExtType(std::uint32_t typeId, std::uint32_t baseTypeId);

// --- problem reporting / startup queue (START-07) ---------------------------
// Where a rejected registration is reported; null disables the message.  The
// message pointer is only valid for the duration of the call.
//
// The sink is installed by Platform::Startup(), i.e. after the platform DLL's
// static initializers ran, and a feature store may register its type from a
// static initializer.  A problem rejected while no sink is installed is kept in
// a bounded queue instead of being dropped; Platform::Startup() calls
// ExtTypeProblemDrain() immediately after installing the sink, so such a
// problem still reaches the log.  Problems rejected after the sink is installed
// reach it immediately.
using ExtTypeProblemSink = void (*)(const char* message);
void SetExtTypeProblemSink(ExtTypeProblemSink sink);

// Capacity of the startup problem queue (START-07).  Problems beyond it are not
// queued; the drain reports their count as one overflow summary instead.
constexpr std::size_t MaxPendingExtTypeProblems = 16;

// Hands every queued startup problem to `sink`, in recording order, and returns
// how many messages were handed over (the queued problems plus one overflow
// summary when problems were dropped).  A null sink reports nothing and leaves
// the queue untouched.  Every message is copied out before the sink runs, so the
// sink may log or re-enter.  Platform::Startup() calls this once right after
// SetExtTypeProblemSink(); tests use it to observe the queue.
std::size_t ExtTypeProblemDrain(ExtTypeProblemSink sink);

// Problems waiting in the startup queue, and problems dropped because that
// bounded queue was full (diagnostics/tests).  A successful drain empties both.
std::size_t PendingExtTypeProblemCount();
std::size_t DroppedExtTypeProblemCount();

// Number of rejected registrations so far (diagnostics/tests).
std::size_t ExtTypeProblemCount();

// True when actualTypeId is wantedTypeId or derives from it.  wantedTypeId == 0
// matches every extension.  A poisoned type id never matches (see
// RegisterExtType), and a chain that passes through a poisoned intermediate
// type is refused as well (SAVE-04): the C++ relationship across the break is
// unknown, so a looked-up instance must not be cast through it.
bool IsExtTypeDerivedFrom(std::uint32_t actualTypeId, std::uint32_t wantedTypeId);

// Function-local static: registers the pair on first use from any call site.
template <std::uint32_t TypeId, std::uint32_t BaseTypeId>
inline void EnsureExtTypeRegistered()
{
	static const bool registered = (RegisterExtType(TypeId, BaseTypeId), true);
	(void)registered;
}
}
