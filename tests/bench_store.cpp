// Microbenchmark for the extension store hot paths.
//
// Not a unit test and not a pass/fail gate: scripts/linux/bench_store.sh builds
// and runs it (release flags, see scripts/linux/common.sh) and saves the output;
// nothing asserts on the numbers.  It reports, for 1k / 5k / 20k live extension
// instances:
//
//   * Allocate  - map insert + heap allocation + allocation-order bookkeeping,
//   * Fetch     - typed lookup in three access patterns:
//                   seq  - walk every owner in allocation order (a different
//                          owner every call: this measures the map lookup, not
//                          a repeated hot entry),
//                   skew - round-robin over min(live, 256) owners (again a
//                          different owner every call; the 256-owner window
//                          bounds the working set),
//                   same - the same owner every call (the per-frame/per-shot
//                          feature pattern; the store's one-entry Fetch cache
//                          answers this one without touching the map),
//   * Remove    - destruction incl. removing the instance from the store,
//   * Clear     - scenario teardown of all instances.
//
// Calibration and limitations (SAVE-08):
//   * each size runs once, with no warm-up and no repeated trials, so run-to-run
//     noise (Wine, host load, frequency scaling) is not quantified - compare the
//     shape and the relative order of the columns, not the last digit;
//   * the numbers are absolute measurements of the container shipped in this
//     tree (ExtensionStore.h), not a before/after comparison: the repository has
//     no second/reference container to measure against, and earlier numbers
//     cannot be reproduced from the current tree;
//   * the benchmark links the store the DLL uses (release-aligned flags), but it
//     runs in a console process under Wine, so the environment is close to, not
//     identical to, the game;
//   * the savegame save/load paths and the deferred-allocation list are not
//     measured here (a load-time workload needs an engine-side fixture).
//
// The measured container is described in docs/EngineBindings.md.
//
// TEST_DEPS: src/Ext/AbstractExt.cpp src/Ext/ExtensionIndex.cpp src/Core/Log.cpp src/Core/PlatformPaths.cpp

#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

#include <windows.h>

class AbstractClass
{
public:
	virtual ~AbstractClass() = default;
};

class ObjectClass : public AbstractClass {};
class TechnoClass : public ObjectClass {};
class FootClass : public TechnoClass {};
class UnitClass : public FootClass {};

#include "Ext/ExtHierarchy.h"
#include "Ext/ExtensionStore.h"

namespace
{
double g_ticksPerMicrosecond = 1.0;

void InitTimer()
{
	LARGE_INTEGER frequency{};
	QueryPerformanceFrequency(&frequency);
	g_ticksPerMicrosecond = static_cast<double>(frequency.QuadPart) / 1000000.0;
}

double NowMicroseconds()
{
	LARGE_INTEGER counter{};
	QueryPerformanceCounter(&counter);
	return static_cast<double>(counter.QuadPart) / g_ticksPerMicrosecond;
}

class BenchUnitExt final : public venture::UnitExt
{
public:
	static constexpr std::uint32_t TypeId = 0x0121u;
	static constexpr std::uint32_t BaseTypeId = venture::UnitExt::TypeId;

	explicit BenchUnitExt(AbstractClass* const owner) : UnitExt(owner) {}

	std::uint32_t ExtTypeId() const override { return TypeId; }

	std::uint32_t Value = 0;
	std::uint32_t Payload[4] = {};
};

using BenchStore = venture::ExtensionStore<UnitClass, BenchUnitExt>;

// The store gained a Reserve() hint with the container rework.  ReserveIfSupported()
// keeps this benchmark compilable against a store header without Reserve() (for
// example an older derived-platform copy); the header shipped in this tree always
// has Reserve(), so the fallback branch is dead code here (SAVE-08) and is kept
// only as a defensive shim.
template <typename TStore, typename = void>
struct HasReserve : std::false_type {};

template <typename TStore>
struct HasReserve<TStore, std::void_t<decltype(std::declval<TStore&>().Reserve(std::size_t{}))>> : std::true_type {};

template <typename TStore>
void ReserveIfSupported(TStore& store, const std::size_t expected)
{
	if constexpr (HasReserve<TStore>::value)
		store.Reserve(expected);
	else
		(void)store, (void)expected;
}

struct Row
{
	int live;
	double allocateNs;
	double fetchSeqNs;
	double fetchSkewNs;
	double fetchSameNs;
	double removeUs;
	double clearNs;
};

Row RunCase(const int live)
{
	BenchStore& store = BenchStore::Instance();
	std::vector<UnitClass*> units(static_cast<std::size_t>(live));

	for (int i = 0; i < live; ++i)
		units[static_cast<std::size_t>(i)] = new UnitClass();

	Row row{};
	row.live = live;

	ReserveIfSupported(store, static_cast<std::size_t>(live));

	// -- allocate --------------------------------------------------------
	{
		const double start = NowMicroseconds();
		for (int i = 0; i < live; ++i)
			(void)store.Allocate(units[static_cast<std::size_t>(i)]);
		const double elapsed = NowMicroseconds() - start;
		row.allocateNs = elapsed * 1000.0 / static_cast<double>(live);
	}

	// -- fetch: allocation-order walk over every owner (map lookup per call) --
	{
		const int iterations = live * 20;
		volatile std::uint32_t sink = 0;
		const double start = NowMicroseconds();
		for (int i = 0; i < iterations; ++i)
		{
			BenchUnitExt* const ext = store.Fetch(units[static_cast<std::size_t>(i % live)]);
			sink += ext ? ext->Value : 0;
		}
		const double elapsed = NowMicroseconds() - start;
		row.fetchSeqNs = elapsed * 1000.0 / static_cast<double>(iterations);
		(void)sink;
	}

	// -- fetch: round-robin over min(live, 256) owners (no hot entry) -----
	{
		const int iterations = live * 20;
		const int window = live > 256 ? 256 : live;
		int cursor = 0;
		volatile std::uint32_t sink = 0;
		const double start = NowMicroseconds();
		for (int i = 0; i < iterations; ++i)
		{
			BenchUnitExt* const ext = store.Fetch(units[static_cast<std::size_t>(cursor)]);
			sink += ext ? ext->Value : 0;
			++cursor;
			if (cursor == window)
				cursor = 0;
		}
		const double elapsed = NowMicroseconds() - start;
		row.fetchSkewNs = elapsed * 1000.0 / static_cast<double>(iterations);
		(void)sink;
	}

	// -- fetch: one owner repeatedly (the one-entry Fetch cache path) -----
	{
		const int iterations = live * 20;
		const UnitClass* const sameOwner = units[0];
		volatile std::uint32_t sink = 0;
		const double start = NowMicroseconds();
		for (int i = 0; i < iterations; ++i)
		{
			BenchUnitExt* const ext = store.Fetch(sameOwner);
			sink += ext ? ext->Value : 0;
		}
		const double elapsed = NowMicroseconds() - start;
		row.fetchSameNs = elapsed * 1000.0 / static_cast<double>(iterations);
		(void)sink;
	}

	// -- remove (random order, timing only the removals) ------------------
	{
		const double start = NowMicroseconds();
		for (int i = 0; i < live; ++i)
		{
			// Even/odd walk: not allocation order, so an O(n) vector search would
			// show its full cost.
			const int index = (i * 7919) % live;
			store.Remove(units[static_cast<std::size_t>(index)]);
		}
		const double elapsed = NowMicroseconds() - start;
		row.removeUs = elapsed / static_cast<double>(live);
	}

	// -- clear -----------------------------------------------------------
	{
		for (int i = 0; i < live; ++i)
			(void)store.Allocate(units[static_cast<std::size_t>(i)]);

		const double start = NowMicroseconds();
		store.Clear();
		const double elapsed = NowMicroseconds() - start;
		row.clearNs = elapsed * 1000.0 / static_cast<double>(live);
	}

	for (int i = 0; i < live; ++i)
		delete units[static_cast<std::size_t>(i)];

	return row;
}
}

int __cdecl main()
{
	InitTimer();

	BenchStore& store = BenchStore::Instance();
	store.SetName("UnitClass/BenchUnitExt");

	std::printf("extension store microbenchmark (Wine + MSVC 14.44; -O2 /Oy like the release DLL; the full flag list is in scripts/linux/bench_store.sh)\n");
	std::printf("%8s %14s %14s %14s %14s %14s %14s\n",
		"live", "allocate", "fetch(seq)", "fetch(skew)", "fetch(same)", "remove", "clear");
	std::printf("%8s %14s %14s %14s %14s %14s %14s\n",
		"", "ns/op", "ns/op", "ns/op", "ns/op", "us/op", "ns/op");

	for (const int live : { 1000, 5000, 20000 })
	{
		const Row row = RunCase(live);
		std::printf("%8d %14.1f %14.1f %14.1f %14.1f %14.3f %14.1f\n",
			row.live, row.allocateNs, row.fetchSeqNs, row.fetchSkewNs, row.fetchSameNs,
			row.removeUs, row.clearNs);
	}

	// Sanity check: the store is empty again after the benchmark.
	std::printf("\nfinal store size: %u\n", static_cast<unsigned>(store.Size()));
	return store.Size() == 0 ? 0 : 1;
}
