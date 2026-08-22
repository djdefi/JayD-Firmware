#ifndef JAYD_FIRMWARE_DJ_AUTO_DJ_QUEUE_H
#define JAYD_FIRMWARE_DJ_AUTO_DJ_QUEUE_H

#include "DjAutoDjTypes.h"

enum class AutoDjEntrySource : uint8_t {
	Pinned,  // explicitly chosen by the user; always served before planned entries
	Planned  // added by the planner's own selection
};

struct AutoDjQueueEntry {
	AutoDjIdentity identity = {};
	AutoDjEntrySource source = AutoDjEntrySource::Planned;
	uint32_t artistHash = 0;
	uint32_t titleHash = 0;
	uint16_t reasons = AUTO_DJ_REASON_NONE;
	uint32_t sequence = 0; // monotonic insertion order, useful for diagnostics
};

// Fixed-capacity (<= AUTO_DJ_QUEUE_CAPACITY), POD-backed up-next queue. No
// dynamic allocation, no arbitrary paths - only stable library identities.
// Pinned entries are always served ahead of planned ones; within each group,
// FIFO insertion order is preserved. Backed by a plain contiguous array
// (entries always occupy [0, count)); removals shift the tail down, which is
// simple to reason about and cheap enough at this bounded capacity.
class DjAutoDjQueue {
public:
	bool pushPinned(const AutoDjIdentity& identity, uint32_t artistHash = 0, uint32_t titleHash = 0){
		return push(identity, AutoDjEntrySource::Pinned, artistHash, titleHash, AUTO_DJ_REASON_NONE);
	}

	bool pushPlanned(const AutoDjIdentity& identity, uint32_t artistHash, uint32_t titleHash, uint16_t reasons){
		return push(identity, AutoDjEntrySource::Planned, artistHash, titleHash, reasons);
	}

	bool full() const{
		return count == AUTO_DJ_QUEUE_CAPACITY;
	}

	bool empty() const{
		return count == 0;
	}

	uint8_t depth() const{
		return count;
	}

	bool peekNext(AutoDjQueueEntry& entry) const{
		const int index = frontIndex();
		if(index < 0) return false;
		entry = entries[index];
		return true;
	}

	bool popNext(AutoDjQueueEntry& entry){
		const int index = frontIndex();
		if(index < 0) return false;
		entry = entries[index];
		removeAt(static_cast<uint8_t>(index));
		return true;
	}

	// True if any queued entry (pinned or planned, regardless of position)
	// already carries this stable identity. Used to keep the planner from
	// queuing the same track twice while it is already queued or in flight.
	bool containsIdentity(const AutoDjIdentity& identity) const{
		for(uint8_t index = 0; index < count; index++){
			if(entries[index].identity.sameTrack(identity)) return true;
		}
		return false;
	}

	// Removes the entry with the given monotonic sequence number, wherever
	// it currently sits in the array. `sequence` is assigned once at push
	// time and never reused, so this always targets exactly the entry that
	// was originally submitted for loading - not "whatever is currently at
	// the front", which can change if a different track is pinned while
	// that load is in flight. Returns false (queue unchanged) if the entry
	// is no longer present, e.g. it was already dropped by
	// invalidateGeneration().
	bool removeBySequence(uint32_t sequence, AutoDjQueueEntry& removed){
		for(uint8_t index = 0; index < count; index++){
			if(entries[index].sequence == sequence){
				removed = entries[index];
				removeAt(index);
				return true;
			}
		}
		return false;
	}

	// Drops entries whose library generation no longer matches the current
	// one. Used defensively when the library index rebuilds underneath a
	// queued reference, so a stale entry is never loaded blind. Returns the
	// number of entries removed.
	uint8_t invalidateGeneration(uint32_t currentGeneration){
		uint8_t removed = 0;
		uint8_t index = 0;
		while(index < count){
			if(entries[index].identity.libraryGeneration != currentGeneration){
				removeAt(index);
				removed++;
				// Do not advance index: the next entry has shifted into this slot.
			} else {
				index++;
			}
		}
		return removed;
	}

	void clear(){
		count = 0;
	}

private:
	int frontIndex() const{
		if(count == 0) return -1;
		for(uint8_t index = 0; index < count; index++){
			if(entries[index].source == AutoDjEntrySource::Pinned) return index;
		}
		return 0;
	}

	bool push(const AutoDjIdentity& identity, AutoDjEntrySource source,
			  uint32_t artistHash, uint32_t titleHash, uint16_t reasons){
		if(full()) return false;
		AutoDjQueueEntry& entry = entries[count];
		entry.identity = identity;
		entry.source = source;
		entry.artistHash = artistHash;
		entry.titleHash = titleHash;
		entry.reasons = reasons;
		entry.sequence = nextSequence++;
		count++;
		return true;
	}

	void removeAt(uint8_t index){
		for(uint8_t i = index; static_cast<uint8_t>(i + 1) < count; i++){
			entries[i] = entries[i + 1];
		}
		count--;
	}

	AutoDjQueueEntry entries[AUTO_DJ_QUEUE_CAPACITY] = {};
	uint8_t count = 0;
	uint32_t nextSequence = 1;
};

#endif //JAYD_FIRMWARE_DJ_AUTO_DJ_QUEUE_H
