#include "DjSession.h"
#include <Arduino.h>
#include <AudioLib/EffectType.hpp>
#include <Loop/LoopManager.h>
#include <SD.h>
#include <esp_system.h>

DjSession* DjSession::instance = nullptr;
uint64_t DjSession::bootId = 0;
uint32_t DjSession::sessionCounter = 0;
static const char* metadataPath = "/library.jydm";

static uint64_t metadataIdentity(fs::File& file){
	if(!file) return 0;
	uint64_t key = file.size();
	uint8_t crc[4] = {};
	if(file.size() >= 48 && file.seek(44) && file.read(crc, sizeof(crc)) == sizeof(crc)){
		for(uint8_t byte : crc) key = (key ^ byte) * 1099511628211ULL;
	}
	return key;
}

DjSession* DjSession::begin(uint8_t leftGain, uint8_t rightGain, uint8_t initialMix){
	if(instance) return instance;
	if(bootId == 0){
		bootId = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
	}
	instance = new DjSession(leftGain, rightGain, initialMix);
	LoopManager::addListener(instance);
	return instance;
}

DjSession* DjSession::get(){
	return instance;
}

void DjSession::end(){
	if(!instance) return;
	DjSession* session = instance;
	instance = nullptr;
	LoopManager::removeListener(session);
	session->shutdown();
	delete session;
}

DjSession::DjSession(uint8_t leftGain, uint8_t rightGain, uint8_t initialMix) :
		gains{ leftGain, rightGain }, mix(initialMix), sessionId(++sessionCounter){
	system = new MixSystem();
	publishSnapshot();
}

DjSession::~DjSession(){
	delete system;
	system = nullptr;
}

DjSubmitResult DjSession::submit(DjCommand command){
	commandMutex.lock();
	command.id = ++nextCommandId;
	if(command.id == 0) command.id = ++nextCommandId;

	DjCommandError error = ending ? DJ_COMMAND_ERROR_SESSION_ENDING : validate(command);
	if(error != DJ_COMMAND_ERROR_NONE){
		commandResults.record(command, DJ_COMMAND_REJECTED, error);
		commandMutex.unlock();
		return { command.id, DJ_COMMAND_REJECTED, error };
	}

	uint32_t supersededId = 0;
	if(commandQueue.supersede(command, supersededId)){
		commandResults.finish(supersededId, DJ_COMMAND_SUPERSEDED, DJ_COMMAND_ERROR_NONE);
		commandResults.record(command, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
		commandMutex.unlock();
		return { command.id, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE };
	}

	if(!commandQueue.push(command)){
		queueDrops++;
		commandResults.record(command, DJ_COMMAND_REJECTED, DJ_COMMAND_ERROR_QUEUE_FULL);
		commandMutex.unlock();
		return { command.id, DJ_COMMAND_REJECTED, DJ_COMMAND_ERROR_QUEUE_FULL };
	}

	commandResults.record(command, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE);
	commandMutex.unlock();
	return { command.id, DJ_COMMAND_ACCEPTED, DJ_COMMAND_ERROR_NONE };
}

DjSubmitResult DjSession::loadDeck(
	uint8_t deck,
	const char* path,
	DjCommandOrigin origin,
	const DjTrackIdentity* identity
){
	DjCommand command = {};
	command.origin = origin;
	command.type = DJ_COMMAND_LOAD_DECK;
	command.deck = deck;
	metadataMutex.lock();
	command.libraryGeneration = libraryGeneration;
	command.libraryKey = libraryKey;
	metadataMutex.unlock();
	if(identity) command.trackIdentity = *identity;
	if(path){
		const size_t length = strlen(path);
		if(length < DJ_PATH_CAPACITY) memcpy(command.path, path, length + 1);
	}
	return submit(command);
}

DjSubmitResult DjSession::setPlaying(uint8_t deck, bool playing, DjCommandOrigin origin){
	DjCommand command = {};
	command.origin = origin;
	command.type = DJ_COMMAND_SET_PLAYING;
	command.deck = deck;
	command.value = playing;
	return submit(command);
}

DjSubmitResult DjSession::seek(uint8_t deck, uint16_t seconds, DjCommandOrigin origin){
	DjCommand command = {};
	command.origin = origin;
	command.type = DJ_COMMAND_SEEK;
	command.deck = deck;
	command.value = seconds;
	return submit(command);
}

DjSubmitResult DjSession::setGain(uint8_t deck, uint8_t gain, DjCommandOrigin origin){
	DjCommand command = {};
	command.origin = origin;
	command.type = DJ_COMMAND_SET_GAIN;
	command.deck = deck;
	command.value = gain;
	return submit(command);
}

DjSubmitResult DjSession::setMix(uint8_t value, DjCommandOrigin origin){
	DjCommand command = {};
	command.origin = origin;
	command.type = DJ_COMMAND_SET_MIX;
	command.value = value;
	return submit(command);
}

DjSubmitResult DjSession::setEffectType(uint8_t deck, uint8_t slot, uint8_t type, DjCommandOrigin origin){
	DjCommand command = {};
	command.origin = origin;
	command.type = DJ_COMMAND_SET_EFFECT_TYPE;
	command.deck = deck;
	command.slot = slot;
	command.value = type;
	return submit(command);
}

DjSubmitResult DjSession::setEffectIntensity(uint8_t deck, uint8_t slot, uint8_t intensity, DjCommandOrigin origin){
	DjCommand command = {};
	command.origin = origin;
	command.type = DJ_COMMAND_SET_EFFECT_INTENSITY;
	command.deck = deck;
	command.slot = slot;
	command.value = intensity;
	return submit(command);
}

DjSubmitResult DjSession::setRecording(bool recording, DjCommandOrigin origin){
	DjCommand command = {};
	command.origin = origin;
	command.type = DJ_COMMAND_SET_RECORDING;
	command.value = recording;
	return submit(command);
}

DjCommandError DjSession::validate(const DjCommand& command) const{
	if(command.type > DJ_COMMAND_SET_RECORDING) return DJ_COMMAND_ERROR_INVALID_VALUE;
	const bool deckCommand = command.type == DJ_COMMAND_LOAD_DECK ||
							 command.type == DJ_COMMAND_SET_PLAYING ||
							 command.type == DJ_COMMAND_SEEK ||
							 command.type == DJ_COMMAND_SET_GAIN ||
							 command.type == DJ_COMMAND_SET_EFFECT_TYPE ||
							 command.type == DJ_COMMAND_SET_EFFECT_INTENSITY;
	if(deckCommand && command.deck >= DJ_DECK_COUNT) return DJ_COMMAND_ERROR_INVALID_DECK;
	if((command.type == DJ_COMMAND_SET_EFFECT_TYPE ||
		command.type == DJ_COMMAND_SET_EFFECT_INTENSITY) &&
	   command.slot >= DJ_EFFECT_SLOT_COUNT) return DJ_COMMAND_ERROR_INVALID_SLOT;
	if(command.type == DJ_COMMAND_SET_EFFECT_TYPE && command.value >= DJ_EFFECT_COUNT){
		return DJ_COMMAND_ERROR_INVALID_VALUE;
	}
	if((command.type == DJ_COMMAND_SET_PLAYING || command.type == DJ_COMMAND_SET_RECORDING) &&
	   command.value > 1) return DJ_COMMAND_ERROR_INVALID_VALUE;
	if((command.type == DJ_COMMAND_SET_GAIN ||
		command.type == DJ_COMMAND_SET_MIX ||
		command.type == DJ_COMMAND_SET_EFFECT_INTENSITY) &&
	   command.value > 255) return DJ_COMMAND_ERROR_INVALID_VALUE;
	if(command.type == DJ_COMMAND_LOAD_DECK){
		if(command.path[0] == '\0' || memchr(command.path, '\0', DJ_PATH_CAPACITY) == nullptr){
			return DJ_COMMAND_ERROR_INVALID_PATH;
		}
	}
	return DJ_COMMAND_ERROR_NONE;
}

bool DjSession::copySnapshot(DjSnapshot& snapshot){
	snapshotMutex.lock();
	snapshots.copy(snapshot);
	snapshotMutex.unlock();
	return snapshot.sessionActive;
}

bool DjSession::hasPendingLoad(){
	commandMutex.lock();
	const bool found = commandQueue.contains(DJ_COMMAND_LOAD_DECK);
	commandMutex.unlock();
	return found;
}

bool DjSession::libraryWorkAllowed(){
	commandMutex.lock();
	const bool playbackChangePending =
		commandQueue.contains(DJ_COMMAND_LOAD_DECK) ||
		commandQueue.contains(DJ_COMMAND_SET_PLAYING) ||
		commandQueue.contains(DJ_COMMAND_SET_RECORDING);
	commandMutex.unlock();
	if(playbackChangePending) return false;

	DjSnapshot snapshot;
	copySnapshot(snapshot);
	return djAllowsLibraryWork(snapshot);
}

DjMetadataState DjSession::metadataState(JaydMetadata::Status status){
	switch(status){
		case JaydMetadata::Status::Ready: return DJ_METADATA_VALID;
		case JaydMetadata::Status::Stale: return DJ_METADATA_STALE;
		case JaydMetadata::Status::Corrupt: return DJ_METADATA_CORRUPT;
		case JaydMetadata::Status::Unsupported: return DJ_METADATA_UNSUPPORTED;
		case JaydMetadata::Status::Missing: return DJ_METADATA_ABSENT;
	}
	return DJ_METADATA_CORRUPT;
}

DjMetadataState DjSession::refreshLibraryMetadata(uint32_t generation, uint64_t key){
	metadataMutex.lock();
	fs::File file = SD.open(metadataPath);
	const uint64_t fileKey = metadataIdentity(file);
	if(metadataInitialized && libraryGeneration == generation &&
	   libraryKey == key && metadataFileKey == fileKey){
		file.close();
		const DjMetadataState state = metadataState(metadataReaderStatus);
		metadataMutex.unlock();
		return state;
	}
	const bool hadAssociation[DJ_DECK_COUNT] = {
		deckMetadata[0].attached(),
		deckMetadata[1].attached()
	};
	metadataReader.close();
	metadataReaderStatus = metadataReader.open(file);
	libraryGeneration = generation;
	libraryKey = key;
	metadataFileKey = fileKey;
	metadataInitialized = true;

	for(uint8_t deck = 0; deck < DJ_DECK_COUNT; ++deck){
		DjMetadataState state = DJ_METADATA_ABSENT;
		if(system && system->hasChannel(deck)){
			state = hadAssociation[deck] || metadataReaderStatus == JaydMetadata::Status::Ready
				? DJ_METADATA_STALE
				: metadataState(metadataReaderStatus);
		}
		deckMetadata[deck].invalidate(state, generation);
	}
	const DjMetadataState state = metadataState(metadataReaderStatus);
	metadataMutex.unlock();
	publishSnapshot();
	return state;
}

void DjSession::invalidateLibraryMetadata(){
	metadataMutex.lock();
	metadataReader.close();
	metadataReaderStatus = JaydMetadata::Status::Missing;
	libraryGeneration = 0;
	libraryKey = 0;
	metadataFileKey = 0;
	metadataInitialized = false;
	for(uint8_t deck = 0; deck < DJ_DECK_COUNT; ++deck){
		deckMetadata[deck].invalidate(
			deckMetadata[deck].attached() ? DJ_METADATA_STALE : DJ_METADATA_ABSENT,
			0
		);
	}
	metadataMutex.unlock();
	publishSnapshot();
}

DjMetadataState DjSession::resolveMetadata(
	const char* path,
	uint32_t generation,
	uint64_t key,
	const DjTrackIdentity* identity,
	JaydMetadata::Track& track,
	DjTrackMetadataSnapshot& metadata
){
	metadata = {};
	metadata.libraryGeneration = libraryGeneration;
	metadata.state = metadataState(metadataReaderStatus);
	if(metadataReaderStatus != JaydMetadata::Status::Ready) return metadata.state;
	if(generation != libraryGeneration || key != libraryKey){
		metadata.state = DJ_METADATA_STALE;
		return metadata.state;
	}

	const char* normalized = path;
	if(normalized && normalized[0] == '/') ++normalized;
	const uint8_t* fingerprint = identity &&
		(identity->flags & DJ_TRACK_IDENTITY_FINGERPRINT)
		? identity->fingerprint : nullptr;
	const uint8_t* sourceId = identity &&
		(identity->flags & DJ_TRACK_IDENTITY_SOURCE)
		? identity->sourceId : nullptr;
	const JaydMetadata::Status match = metadataReader.trackByPath(
		normalized,
		track,
		fingerprint,
		sourceId
	);
	metadata.state = metadataState(match);
	if(match != JaydMetadata::Status::Ready) return metadata.state;

	if(!metadataReader.readStringHash(track.provenance, metadata.provenanceHash)){
		metadata.state = DJ_METADATA_CORRUPT;
		return metadata.state;
	}
	metadata.sourceSampleRate = track.sampleRate;
	metadata.sourceDurationFrames = track.durationFrames;
	metadata.bpmMilli = track.bpmMilli;
	metadata.key = track.key;
	metadata.rating = track.rating;
	metadata.cueCount = track.cueCount;
	metadata.gridCount = track.gridCount;
	metadata.phraseCount = track.phraseCount;
	if(track.sampleRate && track.durationFrames) metadata.capabilities |= DJ_METADATA_HAS_SOURCE_FRAMES;
	if(track.bpmMilli) metadata.capabilities |= DJ_METADATA_HAS_BPM;
	if(track.key) metadata.capabilities |= DJ_METADATA_HAS_KEY;
	if(track.rating != 255) metadata.capabilities |= DJ_METADATA_HAS_RATING;
	if(track.cueCount) metadata.capabilities |= DJ_METADATA_HAS_CUES;
	if(track.gridCount) metadata.capabilities |= DJ_METADATA_HAS_GRID;
	if(track.phraseCount) metadata.capabilities |= DJ_METADATA_HAS_PHRASES;

	for(uint32_t index = 0; index < track.gridCount; ++index){
		JaydMetadata::Grid grid;
		if(!metadataReader.readGrid(track, index, grid)){
			metadata = {};
			metadata.libraryGeneration = libraryGeneration;
			metadata.state = DJ_METADATA_CORRUPT;
			return metadata.state;
		}
		if(grid.beatNumber == 1) ++metadata.downbeatCount;
		if(grid.confidence > metadata.confidence) metadata.confidence = grid.confidence;
	}
	for(uint32_t index = 0; index < track.phraseCount; ++index){
		JaydMetadata::Phrase phrase;
		if(!metadataReader.readPhrase(track, index, phrase)){
			metadata = {};
			metadata.libraryGeneration = libraryGeneration;
			metadata.state = DJ_METADATA_CORRUPT;
			return metadata.state;
		}
		if(phrase.confidence > metadata.confidence) metadata.confidence = phrase.confidence;
	}
	if(metadata.downbeatCount) metadata.capabilities |= DJ_METADATA_HAS_DOWNBEATS;
	metadata.state = DJ_METADATA_VALID;
	return metadata.state;
}

DjMetadataState DjSession::lookupTrackMetadata(
	const char* path,
	DjTrackMetadataSnapshot& metadata,
	const DjTrackIdentity* identity
){
	metadataMutex.lock();
	JaydMetadata::Track track{};
	const DjMetadataState state = resolveMetadata(
		path,
		libraryGeneration,
		libraryKey,
		identity,
		track,
		metadata
	);
	metadataMutex.unlock();
	return state;
}

bool DjSession::copyCue(uint8_t deck, uint16_t index, JaydMetadata::Cue& cue){
	if(deck >= DJ_DECK_COUNT) return false;
	metadataMutex.lock();
	const bool copied = deckMetadata[deck].attached() &&
		deckMetadata[deck].snapshot().libraryGeneration == libraryGeneration &&
		metadataReader.readCue(metadataTracks[deck], index, cue);
	metadataMutex.unlock();
	return copied;
}

bool DjSession::copyGrid(uint8_t deck, uint16_t index, JaydMetadata::Grid& grid){
	if(deck >= DJ_DECK_COUNT) return false;
	metadataMutex.lock();
	const bool copied = deckMetadata[deck].attached() &&
		deckMetadata[deck].snapshot().libraryGeneration == libraryGeneration &&
		metadataReader.readGrid(metadataTracks[deck], index, grid);
	metadataMutex.unlock();
	return copied;
}

bool DjSession::copyPhrase(uint8_t deck, uint16_t index, JaydMetadata::Phrase& phrase){
	if(deck >= DJ_DECK_COUNT) return false;
	metadataMutex.lock();
	const bool copied = deckMetadata[deck].attached() &&
		deckMetadata[deck].snapshot().libraryGeneration == libraryGeneration &&
		metadataReader.readPhrase(metadataTracks[deck], index, phrase);
	metadataMutex.unlock();
	return copied;
}

void DjSession::attachView(InfoGenerator* left, InfoGenerator* right, InfoGenerator* output){
	if(!system || !left || !right || !output) return;
	if(viewAttached && viewInfo[0] == left && viewInfo[1] == right && viewInfo[2] == output) return;
	viewAttached = true;
	if(viewInfo[0] == left && viewInfo[1] == right && viewInfo[2] == output) return;
	viewInfo[0] = left;
	viewInfo[1] = right;
	viewInfo[2] = output;
	system->setChannelInfo(0, left);
	system->setChannelInfo(1, right);
	system->setChannelInfo(2, output);
}

void DjSession::detachView(){
	viewAttached = false;
}

void DjSession::loop(uint micros){
	(void) micros;

	DjCommand command;
	commandMutex.lock();
	const bool available = commandQueue.pop(command);
	commandMutex.unlock();
	if(available){
		DjCommandError error = DJ_COMMAND_ERROR_NONE;
		const bool applied = apply(command, error);
		commandMutex.lock();
		commandResults.finish(command.id, applied ? DJ_COMMAND_APPLIED : DJ_COMMAND_FAILED, error);
		commandMutex.unlock();
	}

	publishSnapshot();
}

bool DjSession::apply(const DjCommand& command, DjCommandError& error){
	if(ending){
		error = DJ_COMMAND_ERROR_SESSION_ENDING;
		return false;
	}

	if(command.type == DJ_COMMAND_LOAD_DECK) return applyLoad(command, error);

	if((command.type == DJ_COMMAND_SET_PLAYING ||
		command.type == DJ_COMMAND_SEEK) &&
	   !system->hasChannel(command.deck)){
		error = DJ_COMMAND_ERROR_NO_DECK;
		return false;
	}

	switch(command.type){
		case DJ_COMMAND_SET_PLAYING:
			if(command.value) system->resumeChannel(command.deck);
			else system->pauseChannel(command.deck);
			return true;
		case DJ_COMMAND_SEEK:
			system->seekChannel(command.deck, command.value);
			return true;
		case DJ_COMMAND_SET_GAIN:
			gains[command.deck] = command.value;
			system->setVolume(command.deck, gains[command.deck]);
			return true;
		case DJ_COMMAND_SET_MIX:
			mix = command.value;
			system->setMix(mix);
			return true;
		case DJ_COMMAND_SET_EFFECT_TYPE: {
			DjEffectTransition transition;
			if(!effectState.setType(command.deck, command.slot, command.value,
									system->hasChannel(command.deck), transition)){
				error = DJ_COMMAND_ERROR_INVALID_VALUE;
				return false;
			}
			if(transition.removeSpeed) system->removeSpeed(command.deck);
			if(transition.clearEffect){
				system->setEffect(command.deck, command.slot, EffectType::NONE);
			}else{
				system->setEffect(command.deck, command.slot, static_cast<EffectType>(command.value));
			}
			if(transition.addSpeed) system->addSpeed(command.deck);
			if(transition.setSpeed){
				system->setSpeed(command.deck, effectState.get(command.deck, command.slot).intensity);
			}
			return true;
		}
		case DJ_COMMAND_SET_EFFECT_INTENSITY: {
			DjEffectTransition transition;
			if(!effectState.setIntensity(command.deck, command.slot, command.value, transition)){
				error = DJ_COMMAND_ERROR_NO_EFFECT;
				return false;
			}
			const DjEffectSnapshot& effect = effectState.get(command.deck, command.slot);
			if(transition.setSpeed){
				system->setSpeed(command.deck, effect.intensity);
			}else if(effect.type != DJ_EFFECT_SPEED){
				system->setEffectIntensity(command.deck, command.slot, effect.intensity);
			}
			return true;
		}
		case DJ_COMMAND_SET_RECORDING:
			if(!system->hasChannel(0) && !system->hasChannel(1)){
				error = DJ_COMMAND_ERROR_NO_DECK;
				return false;
			}
			recordingRequested = command.value;
			if(recordingRequested) system->startRecording();
			else system->stopRecording();
			{
				const uint32_t started = millis();
				while(system->isRecording() != recordingRequested &&
					  millis() - started < 250){
					Sched.loop(0);
				}
				if(system->isRecording() != recordingRequested){
					recordingRequested = system->isRecording();
					error = DJ_COMMAND_ERROR_RECORDING_FAILED;
					return false;
				}
			}
			return true;
		default:
			error = DJ_COMMAND_ERROR_INVALID_VALUE;
			return false;
	}
}

bool DjSession::applyLoad(const DjCommand& command, DjCommandError& error){
	fs::File file = SD.open(command.path);
	if(!file){
		error = DJ_COMMAND_ERROR_OPEN_FAILED;
		return false;
	}

	JaydMetadata::Track candidateTrack{};
	DjTrackMetadataSnapshot candidateMetadata{};
	metadataMutex.lock();
	resolveMetadata(
		command.path,
		command.libraryGeneration,
		command.libraryKey,
		&command.trackIdentity,
		candidateTrack,
		candidateMetadata
	);

	const bool hadLeft = system->hasChannel(0);
	const bool hadRight = system->hasChannel(1);
	const bool opened = system->openChannel(command.deck, file);
	if(!opened){
		deckMetadata[command.deck].commitIfLoaded(candidateMetadata, false, false);
		metadataMutex.unlock();
		file.close();
		error = DJ_COMMAND_ERROR_OPEN_FAILED;
		return false;
	}

	files[command.deck] = file;
	memcpy(paths[command.deck], command.path, strlen(command.path) + 1);
	metadataTracks[command.deck] = candidateTrack;
	deckMetadata[command.deck].commitIfLoaded(
		candidateMetadata,
		candidateMetadata.state == DJ_METADATA_VALID,
		true
	);
	metadataMutex.unlock();
	system->setVolume(command.deck, gains[command.deck]);
	DjEffectTransition effectTransition;
	effectState.deckLoaded(command.deck, effectTransition);
	if(effectTransition.addSpeed) system->addSpeed(command.deck);
	if(effectTransition.setSpeed){
		for(uint8_t slot = 0; slot < DJ_EFFECT_SLOT_COUNT; slot++){
			const DjEffectSnapshot& effect = effectState.get(command.deck, slot);
			if(effect.type != DJ_EFFECT_SPEED) continue;
			system->setSpeed(command.deck, effect.intensity);
			break;
		}
	}

	if(!system->isRunning()){
		mix = command.deck == 0 ? 0 : 255;
		system->setMix(mix);
		system->start();
	}else if(!hadLeft && !hadRight){
		mix = command.deck == 0 ? 0 : 255;
		system->setMix(mix);
	}
	return true;
}

void DjSession::publishSnapshot(){
	DjSnapshot snapshot = {};
	snapshot.seq = ++snapshotSeq;
	snapshot.bootId = bootId;
	snapshot.sessionId = sessionId;
	snapshot.sessionActive = !ending;
	snapshot.mixerRunning = system && system->isRunning();
	snapshot.mix = mix;
	snapshot.recording = recordingRequested || (system && system->isRecording());

	for(uint8_t deck = 0; deck < DJ_DECK_COUNT; deck++){
		DjDeckSnapshot& deckSnapshot = snapshot.decks[deck];
		deckSnapshot.loaded = system && system->hasChannel(deck);
		deckSnapshot.playing = deckSnapshot.loaded && !system->isChannelPaused(deck);
		deckSnapshot.elapsed = deckSnapshot.loaded ? system->getElapsed(deck) : 0;
		deckSnapshot.duration = deckSnapshot.loaded ? system->getDuration(deck) : 0;
		deckSnapshot.timingQuality = deckSnapshot.loaded ? DJ_TIMING_COARSE : DJ_TIMING_UNAVAILABLE;
		deckSnapshot.gain = gains[deck];
		memcpy(deckSnapshot.path, paths[deck], DJ_PATH_CAPACITY);
		effectState.copyDeck(deck, deckSnapshot.effects);
	}
	metadataMutex.lock();
	for(uint8_t deck = 0; deck < DJ_DECK_COUNT; ++deck){
		snapshot.decks[deck].metadata = deckMetadata[deck].snapshot();
	}
	metadataMutex.unlock();

	commandMutex.lock();
	snapshot.queueDepth = commandQueue.depth();
	snapshot.queueDrops = queueDrops;
	commandResults.copyTo(snapshot.recentResults);
	commandMutex.unlock();

	snapshotMutex.lock();
	snapshots.publish(snapshot);
	snapshotMutex.unlock();
}

void DjSession::shutdown(){
	commandMutex.lock();
	ending = true;
	DjCommand command;
	while(commandQueue.pop(command)){
		commandResults.finish(command.id, DJ_COMMAND_FAILED, DJ_COMMAND_ERROR_SESSION_ENDING);
	}
	commandMutex.unlock();

	publishSnapshot();
	if(system) system->stop();
	metadataMutex.lock();
	metadataReader.close();
	metadataMutex.unlock();
}
