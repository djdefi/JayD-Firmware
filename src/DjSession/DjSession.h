#ifndef JAYD_FIRMWARE_DJSESSION_H
#define JAYD_FIRMWARE_DJSESSION_H

#include <AudioLib/InfoGenerator.h>
#include <AudioLib/Systems/MixSystem.h>
#include <FS.h>
#include <Loop/LoopListener.h>
#include <Sync/Mutex.h>
#include "../Metadata/JaydMetadata.h"
#include "DjSessionState.h"

class DjSession : public LoopListener {
public:
	static DjSession* begin(uint8_t leftGain, uint8_t rightGain, uint8_t mix);
	static DjSession* get();
	static void end();

	DjSubmitResult submit(DjCommand command);
	DjSubmitResult loadDeck(
		uint8_t deck,
		const char* path,
		DjCommandOrigin origin,
		const DjTrackIdentity* identity = nullptr
	);
	DjSubmitResult setPlaying(uint8_t deck, bool playing, DjCommandOrigin origin);
	DjSubmitResult seek(uint8_t deck, uint16_t seconds, DjCommandOrigin origin);
	DjSubmitResult setGain(uint8_t deck, uint8_t gain, DjCommandOrigin origin);
	DjSubmitResult setMix(uint8_t mix, DjCommandOrigin origin);
	DjSubmitResult setEffectType(uint8_t deck, uint8_t slot, uint8_t type, DjCommandOrigin origin);
	DjSubmitResult setEffectIntensity(uint8_t deck, uint8_t slot, uint8_t intensity, DjCommandOrigin origin);
	DjSubmitResult setRecording(bool recording, DjCommandOrigin origin);
	DjSubmitResult setQuantize(uint8_t deck, DjQuantizeResolution resolution, DjCommandOrigin origin);
	DjSubmitResult loopEngage(uint8_t deck, DjLoopLength length, DjCommandOrigin origin);
	DjSubmitResult loopDisengage(uint8_t deck, DjCommandOrigin origin);
	DjSubmitResult loopReloop(uint8_t deck, DjCommandOrigin origin);
	// masterDeck: -1 requests auto-master (the other deck); otherwise an explicit deck index.
	DjSubmitResult setSync(uint8_t deck, bool armed, int8_t masterDeck, DjCommandOrigin origin);
	DjSubmitResult setCue(uint8_t deck, uint8_t cue, DjCommandOrigin origin);
	DjSubmitResult triggerCue(uint8_t deck, uint8_t cue, DjCommandOrigin origin);
	DjSubmitResult clearCue(uint8_t deck, uint8_t cue, DjCommandOrigin origin);

	bool copySnapshot(DjSnapshot& snapshot);
	bool hasPendingLoad();
	bool libraryWorkAllowed();
	DjMetadataState refreshLibraryMetadata(uint32_t generation, uint64_t libraryKey);
	void invalidateLibraryMetadata();
	DjMetadataState lookupTrackMetadata(
		const char* path,
		DjTrackMetadataSnapshot& metadata,
		const DjTrackIdentity* identity = nullptr
	);
	bool copyCue(uint8_t deck, uint16_t index, JaydMetadata::Cue& cue);
	bool copyGrid(uint8_t deck, uint16_t index, JaydMetadata::Grid& grid);
	bool copyPhrase(uint8_t deck, uint16_t index, JaydMetadata::Phrase& phrase);
	void attachView(InfoGenerator* left, InfoGenerator* right, InfoGenerator* output);
	void detachView();
	void loop(uint micros) override;

private:
	DjSession(uint8_t leftGain, uint8_t rightGain, uint8_t mix);
	~DjSession() override;

	static DjSession* instance;
	static uint64_t bootId;
	static uint32_t sessionCounter;
	static bool orphanRecoveryDone;

	MixSystem* system = nullptr;
	fs::File files[DJ_DECK_COUNT];
	char paths[DJ_DECK_COUNT][DJ_PATH_CAPACITY] = {};
	uint8_t gains[DJ_DECK_COUNT] = { 255, 255 };
	uint8_t mix = 127;
	DjEffectState effectState;
	DjCueState cues;
	bool ending = false;
	bool viewAttached = false;
	InfoGenerator* viewInfo[3] = {};

	Mutex commandMutex;
	Mutex snapshotMutex;
	Mutex metadataMutex;
	DjCommandQueue commandQueue;
	DjCommandResults commandResults;
	DjSnapshotBuffers snapshots;
	uint32_t nextCommandId = 0;
	uint32_t queueDrops = 0;
	uint64_t snapshotSeq = 0;
	uint32_t sessionId = 0;
	JaydMetadata::Reader metadataReader;
	JaydMetadata::Status metadataReaderStatus = JaydMetadata::Status::Missing;
	uint32_t libraryGeneration = 0;
	uint64_t libraryKey = 0;
	uint64_t metadataFileKey = 0;
	bool metadataInitialized = false;
	JaydMetadata::Track metadataTracks[DJ_DECK_COUNT] = {};
	DjDeckMetadataState deckMetadata[DJ_DECK_COUNT];

	DjBeatGrid grids[DJ_DECK_COUNT];
	DjLoopEngine loopEngines[DJ_DECK_COUNT];
	DjSyncController syncControllers[DJ_DECK_COUNT];
	DjQuantizeResolution quantizeResolution[DJ_DECK_COUNT] = { DJ_QUANTIZE_OFF, DJ_QUANTIZE_OFF };
	bool syncArmed[DJ_DECK_COUNT] = {};
	int8_t syncMasterDeck[DJ_DECK_COUNT] = { -1, -1 }; // -1 = auto (the other deck)
	DjCommandError syncLastError[DJ_DECK_COUNT] = {};

	DjRecordingSnapshot recordingSnapshot;
	DjRecordingState lastRecordingState = DJ_RECORDING_IDLE;
	// Set to the specific storage-layer error when finalizeRecording() fails
	// (naming space exhausted vs. rename I/O failure); DJ_RECORDING_ERROR_NONE
	// otherwise. Overrides a library-reported success once set.
	DjRecordingError finalizeError = DJ_RECORDING_ERROR_NONE;

	DjCommandError validate(const DjCommand& command) const;
	bool hasDeck(uint8_t deck) const;
	bool apply(const DjCommand& command, DjCommandError& error, DjCommandStatus& status, DjCommandResult& diagnostics);
	bool applyLoad(const DjCommand& command, DjCommandError& error);
	bool buildGrid(uint8_t deck);
	uint8_t resolveMasterDeck(uint8_t followerDeck) const;
	void tickLoops();
	void tickSync();
	void pollRecording();
	static DjRecordingState mapRecordingState(RecordingState state);
	static DjRecordingError mapRecordingError(RecordingError error);
	DjMetadataState resolveMetadata(
		const char* path,
		uint32_t generation,
		uint64_t key,
		const DjTrackIdentity* identity,
		JaydMetadata::Track& track,
		DjTrackMetadataSnapshot& metadata
	);
	static DjMetadataState metadataState(JaydMetadata::Status status);
	void publishSnapshot();
	void shutdown();
};

#endif
