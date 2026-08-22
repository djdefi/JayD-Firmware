#ifndef JAYD_FIRMWARE_DJ_AUTO_DJ_HISTORY_H
#define JAYD_FIRMWARE_DJ_AUTO_DJ_HISTORY_H

#include "DjAutoDjTypes.h"

struct AutoDjHistoryEntry {
	AutoDjIdentity identity = {};
	uint32_t artistHash = 0;
	uint32_t titleHash = 0;
	uint32_t sequence = 0;
};

// Bounded, POD-backed ring buffer of recently-loaded tracks. Used to keep the
// planner from immediately repeating a track or artist/title. A hash of 0
// means "unknown" and is never treated as a match - missing metadata must
// never produce a false exclusion (or false inclusion).
class DjAutoDjHistory {
public:
	void record(const AutoDjIdentity& identity, uint32_t artistHash, uint32_t titleHash){
		AutoDjHistoryEntry& entry = entries[next];
		entry.identity = identity;
		entry.artistHash = artistHash;
		entry.titleHash = titleHash;
		entry.sequence = sequenceCounter++;
		next = static_cast<uint8_t>((next + 1) % AUTO_DJ_HISTORY_CAPACITY);
		if(filled < AUTO_DJ_HISTORY_CAPACITY) filled++;
	}

	// True if `identity` appears in the most recent `window` plays.
	bool wasRecentlyPlayed(const AutoDjIdentity& identity, uint8_t window) const{
		if(!identity.valid()) return false;
		return findRecent(window, [&](const AutoDjHistoryEntry& entry){
			return entry.identity.sameTrack(identity);
		});
	}

	bool artistOnCooldown(uint32_t artistHash, uint8_t window) const{
		if(artistHash == 0) return false;
		return findRecent(window, [&](const AutoDjHistoryEntry& entry){
			return entry.artistHash == artistHash;
		});
	}

	bool titleOnCooldown(uint32_t titleHash, uint8_t window) const{
		if(titleHash == 0) return false;
		return findRecent(window, [&](const AutoDjHistoryEntry& entry){
			return entry.titleHash == titleHash;
		});
	}

	uint8_t size() const{
		return filled;
	}

	void clear(){
		next = 0;
		filled = 0;
		sequenceCounter = 0;
	}

private:
	template<typename Predicate>
	bool findRecent(uint8_t window, Predicate predicate) const{
		if(window == 0 || filled == 0) return false;
		const uint8_t limit = window < filled ? window : filled;
		for(uint8_t i = 0; i < limit; i++){
			const uint8_t index = static_cast<uint8_t>((next + AUTO_DJ_HISTORY_CAPACITY - 1 - i) % AUTO_DJ_HISTORY_CAPACITY);
			if(predicate(entries[index])) return true;
		}
		return false;
	}

	AutoDjHistoryEntry entries[AUTO_DJ_HISTORY_CAPACITY] = {};
	uint8_t next = 0;
	uint8_t filled = 0;
	uint32_t sequenceCounter = 0;
};

#endif //JAYD_FIRMWARE_DJ_AUTO_DJ_HISTORY_H
