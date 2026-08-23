#ifndef JAYD_FIRMWARE_DJASSISTGRIDCACHE_H
#define JAYD_FIRMWARE_DJASSISTGRIDCACHE_H

#include "DjAssistTypes.h"

// Bounded, fixed-capacity (DJ_ASSIST_GRID_CACHE_SLOTS), POD, host-testable
// manager for the on-demand beat-grid-anchor hydration cache described in
// DjAssistGridCacheSlot's doc comment (DjAssistTypes.h). Pure logic, no
// locking/threading of its own - DjAssistController serializes every call
// under its existing candidateMutex_, exactly like entries_[]/entryTotal_
// already are, so this class itself stays Arduino/thread-free and directly
// host-testable (see tests/dj_assist_bridge_selfcheck.cpp).
class DjAssistGridCache {
public:
	// Records a hydration request for the given key. Idempotent: a request
	// whose key already matches an existing Pending or Ready slot is a
	// no-op that keeps the existing slot (never restarts an
	// already-resolved or in-flight hydration). Otherwise claims the next
	// slot in fixed round-robin order. slots_ is a plain, non-ps_malloc'd,
	// DJ_ASSIST_GRID_CACHE_SLOTS-sized array (a few KiB, see
	// DjAssistTypes.h's static_assert), so this can never fail with an
	// allocation error - it always returns true. With at most one Auto DJ
	// load in flight at a time (see AutoDjLoadPort's "at most one load in
	// flight" contract) round-robin eviction never displaces a slot a
	// caller is still waiting on in practice; even if it did, the caller's
	// own lookup() simply reports "no usable grid" for the evicted key -
	// a cache miss is always a safe fallback, never a correctness issue.
	bool request(uint32_t libraryGeneration, uint32_t metadataRevision, const DjTrackIdentity& identity);

	// Index of the first Pending slot, or -1 if none. Used by the
	// background fill worker to resolve exactly one hydration request per
	// step (bounded, incremental - see DjAssistController::fillWorkerStep()).
	int findPending() const;

	// Read-only introspection of one slot by index - used by the fill
	// worker (to read back the key of a Pending slot found via
	// findPending() before doing the actual, possibly SD-backed, read
	// outside any lock) and by tests.
	const DjAssistGridCacheSlot& at(uint8_t index) const;

	// Commits a hydration result for slot `index`, but only if it is
	// STILL Pending for the EXACT key passed in - a newer request() call
	// may have already evicted/overwritten this slot while the caller was
	// off doing the actual read (mirrors fillWorkerStep()'s own
	// re-validate-before-commit discipline for entries_[]). ok == false,
	// anchorCount == 0, or a null anchors pointer all commit a Failed
	// result with zero anchors (never a partial/garbage anchor set).
	void resolve(
		int index, uint32_t libraryGeneration, uint32_t metadataRevision, const DjTrackIdentity& identity,
		bool ok, const DjGridAnchor* anchors, uint16_t anchorCount
	);

	// Read-only lookup for DjSession::resolveIdentityLoad(): true only for
	// an exact-key Ready slot. Stale/mismatched/pending/failed/never-
	// requested keys all report false with outAnchorCount left at 0 - the
	// caller already treats that identically to "no usable grid" (same
	// fallback buildGrid()/DjBeatGrid::build() use for a track with no
	// usable grid data at all), so a miss here can never fail a load.
	bool lookup(
		uint32_t libraryGeneration, uint32_t metadataRevision, const DjTrackIdentity& identity,
		DjGridAnchor* outAnchors, uint16_t& outAnchorCount
	) const;

private:
	int findMatchingSlot(uint32_t libraryGeneration, uint32_t metadataRevision, const DjTrackIdentity& identity) const;

	DjAssistGridCacheSlot slots_[DJ_ASSIST_GRID_CACHE_SLOTS] = {};
	uint8_t nextSlot_ = 0;
};

#endif
