#include "DjSession.h"
#include <Arduino.h>
#include <AudioLib/EffectType.hpp>
#include <Loop/LoopManager.h>
#include <SD.h>
#include <esp_system.h>

DjSession* DjSession::instance = nullptr;
uint64_t DjSession::bootId = 0;
uint32_t DjSession::sessionCounter = 0;

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

DjSubmitResult DjSession::loadDeck(uint8_t deck, const char* path, DjCommandOrigin origin){
	DjCommand command = {};
	command.origin = origin;
	command.type = DJ_COMMAND_LOAD_DECK;
	command.deck = deck;
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
			if(command.value) system->startRecording();
			else system->stopRecording();
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

	const bool hadLeft = system->hasChannel(0);
	const bool hadRight = system->hasChannel(1);
	if(!system->openChannel(command.deck, file)){
		file.close();
		error = DJ_COMMAND_ERROR_OPEN_FAILED;
		return false;
	}

	files[command.deck] = file;
	memcpy(paths[command.deck], command.path, strlen(command.path) + 1);
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
	snapshot.recording = system && system->isRecording();

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
}
