#include "DjAssistGridCache.h"
#include "DjAssistScoring.h"

namespace {

bool sameKey(
	const DjAssistGridCacheSlot& slot, uint32_t libraryGeneration, uint32_t metadataRevision,
	const DjTrackIdentity& identity
){
	return slot.libraryGeneration == libraryGeneration && slot.metadataRevision == metadataRevision &&
		DjAssistScoring::identityMatches(slot.identity, identity);
}

} // namespace

int DjAssistGridCache::findMatchingSlot(
	uint32_t libraryGeneration, uint32_t metadataRevision, const DjTrackIdentity& identity
) const{
	for(uint8_t i = 0; i < DJ_ASSIST_GRID_CACHE_SLOTS; ++i){
		if(slots_[i].state == DjAssistGridCacheState::Empty) continue;
		if(sameKey(slots_[i], libraryGeneration, metadataRevision, identity)) return int(i);
	}
	return -1;
}

bool DjAssistGridCache::request(uint32_t libraryGeneration, uint32_t metadataRevision, const DjTrackIdentity& identity){
	const int existing = findMatchingSlot(libraryGeneration, metadataRevision, identity);
	if(existing >= 0){
		const DjAssistGridCacheState state = slots_[size_t(existing)].state;
		if(state == DjAssistGridCacheState::Pending || state == DjAssistGridCacheState::Ready) return true;
	}
	const uint8_t index = nextSlot_;
	nextSlot_ = uint8_t((nextSlot_ + 1) % DJ_ASSIST_GRID_CACHE_SLOTS);
	slots_[index] = DjAssistGridCacheSlot();
	slots_[index].state = DjAssistGridCacheState::Pending;
	slots_[index].libraryGeneration = libraryGeneration;
	slots_[index].metadataRevision = metadataRevision;
	slots_[index].identity = identity;
	return true;
}

int DjAssistGridCache::findPending() const{
	for(uint8_t i = 0; i < DJ_ASSIST_GRID_CACHE_SLOTS; ++i){
		if(slots_[i].state == DjAssistGridCacheState::Pending) return int(i);
	}
	return -1;
}

const DjAssistGridCacheSlot& DjAssistGridCache::at(uint8_t index) const{
	static const DjAssistGridCacheSlot empty = {};
	if(index >= DJ_ASSIST_GRID_CACHE_SLOTS) return empty;
	return slots_[index];
}

void DjAssistGridCache::resolve(
	int index, uint32_t libraryGeneration, uint32_t metadataRevision, const DjTrackIdentity& identity,
	bool ok, const DjGridAnchor* anchors, uint16_t anchorCount
){
	if(index < 0 || index >= int(DJ_ASSIST_GRID_CACHE_SLOTS)) return;
	DjAssistGridCacheSlot& slot = slots_[size_t(index)];
	// Only commit if the slot is still Pending for the EXACT key the
	// caller resolved - a newer request() may have evicted/overwritten it
	// while the (possibly slow, SD-backed) read was in flight elsewhere.
	if(slot.state != DjAssistGridCacheState::Pending || !sameKey(slot, libraryGeneration, metadataRevision, identity)){
		return;
	}
	if(!ok || anchorCount == 0 || anchors == nullptr){
		slot.state = DjAssistGridCacheState::Failed;
		slot.anchorCount = 0;
		return;
	}
	const uint16_t count = anchorCount > DJ_GRID_ANCHOR_CAPACITY ? DJ_GRID_ANCHOR_CAPACITY : anchorCount;
	for(uint16_t i = 0; i < count; ++i) slot.anchors[i] = anchors[i];
	slot.anchorCount = count;
	slot.state = DjAssistGridCacheState::Ready;
}

bool DjAssistGridCache::lookup(
	uint32_t libraryGeneration, uint32_t metadataRevision, const DjTrackIdentity& identity,
	DjGridAnchor* outAnchors, uint16_t& outAnchorCount
) const{
	outAnchorCount = 0;
	const int index = findMatchingSlot(libraryGeneration, metadataRevision, identity);
	if(index < 0 || slots_[size_t(index)].state != DjAssistGridCacheState::Ready) return false;
	const DjAssistGridCacheSlot& slot = slots_[size_t(index)];
	if(outAnchors == nullptr) return false;
	for(uint16_t i = 0; i < slot.anchorCount; ++i) outAnchors[i] = slot.anchors[i];
	outAnchorCount = slot.anchorCount;
	return true;
}
