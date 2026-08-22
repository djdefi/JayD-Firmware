#ifndef JAYD_FIRMWARE_DJSESSION_H
#define JAYD_FIRMWARE_DJSESSION_H

#include <AudioLib/InfoGenerator.h>
#include <AudioLib/Systems/MixSystem.h>
#include <FS.h>
#include <Loop/LoopListener.h>
#include <Sync/Mutex.h>
#include "DjSessionState.h"

class DjSession : public LoopListener {
public:
	static DjSession* begin(uint8_t leftGain, uint8_t rightGain, uint8_t mix);
	static DjSession* get();
	static void end();

	DjSubmitResult submit(DjCommand command);
	DjSubmitResult loadDeck(uint8_t deck, const char* path, DjCommandOrigin origin);
	DjSubmitResult setPlaying(uint8_t deck, bool playing, DjCommandOrigin origin);
	DjSubmitResult seek(uint8_t deck, uint16_t seconds, DjCommandOrigin origin);
	DjSubmitResult setGain(uint8_t deck, uint8_t gain, DjCommandOrigin origin);
	DjSubmitResult setMix(uint8_t mix, DjCommandOrigin origin);
	DjSubmitResult setEffectType(uint8_t deck, uint8_t slot, uint8_t type, DjCommandOrigin origin);
	DjSubmitResult setEffectIntensity(uint8_t deck, uint8_t slot, uint8_t intensity, DjCommandOrigin origin);
	DjSubmitResult setRecording(bool recording, DjCommandOrigin origin);
	DjSubmitResult setCue(uint8_t deck, uint8_t cue, DjCommandOrigin origin);
	DjSubmitResult triggerCue(uint8_t deck, uint8_t cue, DjCommandOrigin origin);
	DjSubmitResult clearCue(uint8_t deck, uint8_t cue, DjCommandOrigin origin);

	bool copySnapshot(DjSnapshot& snapshot);
	bool hasPendingLoad();
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
	DjCueState cues;
	bool ending = false;
	bool viewAttached = false;
	InfoGenerator* viewInfo[3] = {};

	Mutex commandMutex;
	Mutex snapshotMutex;
	DjCommandQueue commandQueue;
	DjCommandResults commandResults;
	DjSnapshotBuffers snapshots;
	uint32_t nextCommandId = 0;
	uint32_t queueDrops = 0;
	uint64_t snapshotSeq = 0;
	uint32_t sessionId = 0;

	DjCommandError validate(const DjCommand& command) const;
	bool hasDeck(uint8_t deck) const;
	bool apply(const DjCommand& command, DjCommandError& error);
	bool applyLoad(const DjCommand& command, DjCommandError& error);
	void publishSnapshot();
	void shutdown();
};

#endif
