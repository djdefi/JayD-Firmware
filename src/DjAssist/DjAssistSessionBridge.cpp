#include "DjAssistSessionBridge.h"
#include "DjAssistScoring.h"
#include <string.h>

namespace DjAssistBridge {

namespace {
bool isNonZero16(const uint8_t bytes[16]){
	for(uint8_t i = 0; i < 16; ++i){
		if(bytes[i] != 0) return true;
	}
	return false;
}
} // namespace

DjTrackIdentity buildTrackIdentity(const uint8_t fingerprint[16], const uint8_t sourceId[16]){
	DjTrackIdentity identity;
	if(isNonZero16(fingerprint)){
		identity.flags |= DJ_TRACK_IDENTITY_FINGERPRINT;
		memcpy(identity.fingerprint, fingerprint, 16);
	}
	if(isNonZero16(sourceId)){
		identity.flags |= DJ_TRACK_IDENTITY_SOURCE;
		memcpy(identity.sourceId, sourceId, 16);
	}
	return identity;
}

DjAssistLibraryEntry buildLibraryEntry(
	uint32_t libraryIndex,
	const DjTrackIdentity& identity,
	DjMetadataState state,
	uint32_t sampleRate,
	uint64_t durationFrames,
	uint32_t bpmMilli,
	uint16_t key,
	uint8_t rating,
	uint32_t cueCount,
	uint32_t gridCount,
	uint32_t phraseCount
){
	DjAssistLibraryEntry entry;
	entry.libraryIndex = libraryIndex;
	entry.identity = identity;
	entry.state = state;
	entry.bpmMilli = bpmMilli;
	entry.key = key;
	entry.rating = rating;
	entry.durationFrames = durationFrames;
	entry.sampleRate = sampleRate;

	uint16_t capabilities = 0;
	if(sampleRate && durationFrames) capabilities |= DJ_METADATA_HAS_SOURCE_FRAMES;
	if(bpmMilli) capabilities |= DJ_METADATA_HAS_BPM;
	if(key) capabilities |= DJ_METADATA_HAS_KEY;
	if(rating != 255) capabilities |= DJ_METADATA_HAS_RATING;
	if(cueCount) capabilities |= DJ_METADATA_HAS_CUES;
	if(gridCount) capabilities |= DJ_METADATA_HAS_GRID;
	if(phraseCount) capabilities |= DJ_METADATA_HAS_PHRASES;
	entry.capabilities = capabilities;

	return entry;
}

uint8_t computeCrossfadeMix(
	uint8_t toDeck,
	uint64_t elapsedMicros,
	uint8_t crossfadeBeats,
	uint32_t bpmMilli
){
	const uint8_t targetEndpoint = toDeck == 0 ? 0 : 255;
	if(bpmMilli == 0 || crossfadeBeats == 0) return targetEndpoint;

	// microsPerBeat = 60,000,000 us/min * 1000 (bpmMilli scale) / bpmMilli.
	const uint64_t microsPerBeat = 60000000000ULL / bpmMilli;
	if(microsPerBeat == 0) return targetEndpoint;

	const uint64_t elapsedBeats64 = elapsedMicros / microsPerBeat;
	const uint16_t totalSteps = crossfadeBeats;
	const uint16_t stepIndex = elapsedBeats64 > 0xFFFFU ? uint16_t(0xFFFFU) : uint16_t(elapsedBeats64);
	const uint8_t curve = DjAssistScoring::crossfadeCurve(stepIndex, totalSteps);
	// crossfadeCurve ramps 0->255 as stepIndex advances; that already
	// matches the toDeck==1 direction (0=deck0, 255=deck1). toDeck==0 needs
	// the mirror image (255 -> 0 as the transition progresses).
	return toDeck == 0 ? uint8_t(255 - curve) : curve;
}

DjAssistRollbackPhase nextRollbackPhase(
	DjAssistRollbackPhase phase,
	bool crossfadeSubmitted,
	bool syncSubmitted,
	bool startDeckSubmitted
){
	if(phase == DJ_ASSIST_ROLLBACK_IDLE) phase = DJ_ASSIST_ROLLBACK_MIX;
	if(phase == DJ_ASSIST_ROLLBACK_MIX && !crossfadeSubmitted) phase = DJ_ASSIST_ROLLBACK_SYNC;
	if(phase == DJ_ASSIST_ROLLBACK_SYNC && !syncSubmitted) phase = DJ_ASSIST_ROLLBACK_STOP_DECK;
	if(phase == DJ_ASSIST_ROLLBACK_STOP_DECK && !startDeckSubmitted) phase = DJ_ASSIST_ROLLBACK_DONE;
	return phase;
}

DjAssistCommandOutcome evaluateCommandOutcome(DjCommandStatus status){
	switch(status){
		case DJ_COMMAND_APPLIED:
			return DJ_ASSIST_COMMAND_DONE;
		case DJ_COMMAND_REJECTED:
		case DJ_COMMAND_FAILED:
			return DJ_ASSIST_COMMAND_FAILED;
		case DJ_COMMAND_SUPERSEDED:
			return DJ_ASSIST_COMMAND_RESUBMIT;
		case DJ_COMMAND_ACCEPTED:
		case DJ_COMMAND_PENDING:
		default:
			return DJ_ASSIST_COMMAND_WAIT;
	}
}

bool detectManualMixOverride(
	const DjCommandResult* recentResults,
	uint8_t resultCount,
	uint32_t watermarkId
){
	if(!recentResults) return false;
	for(uint8_t i = 0; i < resultCount; ++i){
		const DjCommandResult& result = recentResults[i];
		if(result.id == 0 || result.id <= watermarkId) continue;
		if(result.type != DJ_COMMAND_SET_MIX) continue;
		if(result.origin == DJ_ORIGIN_SYSTEM) continue;
		if(result.status == DJ_COMMAND_REJECTED) continue;
		return true;
	}
	return false;
}

} // namespace DjAssistBridge
