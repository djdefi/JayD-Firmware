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

	MixSystem* system = nullptr;
	fs::File files[DJ_DECK_COUNT];
	char paths[DJ_DECK_COUNT][DJ_PATH_CAPACITY] = {};
	uint8_t gains[DJ_DECK_COUNT] = { 255, 255 };
	uint8_t mix = 127;
	DjEffectState effectState;
	bool recordingRequested = false;
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

	DjCommandError validate(const DjCommand& command) const;
	bool apply(const DjCommand& command, DjCommandError& error);
	bool applyLoad(const DjCommand& command, DjCommandError& error);
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
