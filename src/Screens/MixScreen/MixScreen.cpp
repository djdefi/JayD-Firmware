#include <Input/InputJayD.h>
#include <SD.h>
#include <Loop/LoopManager.h>
#include <JayD.h>
#include <FS/CompressedFile.h>
#include "MixScreen.h"
#include "../SongList/SongList.h"
#include "../TextInputScreen/TextInputScreen.h"
#include "../../Fonts.h"

MixScreen::MixScreen* MixScreen::MixScreen::instance = nullptr;

MixScreen::MixScreen::MixScreen(Display& display) : Context(display),
													screenLayout(new LinearLayout(&screen, HORIZONTAL)),
													leftLayout(new LinearLayout(screenLayout, VERTICAL)),
													rightLayout(new LinearLayout(screenLayout, VERTICAL)),
													leftSeekBar(new SongSeekBar(leftLayout)),
													rightSeekBar(new SongSeekBar(rightLayout)),
													leftSongName(new SongName(leftLayout)),
													rightSongName(new SongName(rightLayout)), leftVu(&matrixManager.matrixL), rightVu(&matrixManager.matrixR),
													midVu(&matrixManager.matrixBig){

	for(int i = 0; i < 3; i++){
		effectElements[i] = new EffectElement(leftLayout, false);
	}
	for(int i = 3; i < 6; i++){
		effectElements[i] = new EffectElement(rightLayout, true);
	}

	session = DjSession::begin(
			InputJayD::getInstance()->getPotValue(POT_L),
			InputJayD::getInstance()->getPotValue(POT_R),
			InputJayD::getInstance()->getPotValue(POT_MID));
	instance = this;
	buildUI();
	MixScreen::pack();
}

MixScreen::MixScreen::~MixScreen(){
	instance = nullptr;
	if(session && session == DjSession::get()){
		DjSession::end();
		session = nullptr;
	}
	free(selectedBackgroundBuffer);
}

void MixScreen::MixScreen::pack(){
	Context::pack();
	free(selectedBackgroundBuffer);
	selectedBackgroundBuffer = nullptr;
}

void MixScreen::MixScreen::unpack(){
	Context::unpack();

	selectedBackgroundBuffer = static_cast<Color*>(ps_malloc(79 * 128 * 2));
	if(selectedBackgroundBuffer == nullptr){
		Serial.println("Selected background malloc error");
		return;
	}

	fs::File bgFile = CompressedFile::open(SPIFFS.open("/mixSelectedBg.raw.hs"), 13, 12);
	bgFile.read(reinterpret_cast<uint8_t*>(selectedBackgroundBuffer), 79 * 128 * 2);
	bgFile.close();
}

void MixScreen::MixScreen::saveRecording(){
	if(!SD.exists(MixSystem::recordPath)){
		doneRecording = false;
		return;
	}

	Task saveTask("MixSave", [](Task* task){
		String saveFilename = * (String*) task->arg;

		if(SD.exists(saveFilename)){
			SD.remove(saveFilename);
		}

		File inFile = SD.open(MixSystem::recordPath);
		File outFile = SD.open(saveFilename, "w");

		SourceWAV input(inFile);
		OutputAAC output(outFile);

		output.setSource(&input);
		output.start();

		while(output.isRunning()){
			output.loop(0);
		}

		output.stop();
		input.close();

		inFile.close();
		outFile.close();
	}, 8 * 1024, &saveFilename);

	saveTask.start(1, 0);

	while(!saveTask.isStopped()){
		if(millis() - lastDraw >= 30){
			lastDraw = millis();
			drawSaveStatus();
			screen.commit();
		}

		Sched.loop(0);
	}

	SD.remove(MixSystem::recordPath);
	doneRecording = false;
}

void MixScreen::MixScreen::returned(void* data){
	String* filename = (String*) data;
	songListOpen = false;

	if(doneRecording){
		saveFilename = String("/") + *filename + ".aac";
		delete filename;
		return;
	}

	session->setGain(0, InputJayD::getInstance()->getPotValue(POT_L), DJ_ORIGIN_LOCAL_UI);
	session->setGain(1, InputJayD::getInstance()->getPotValue(POT_R), DJ_ORIGIN_LOCAL_UI);
	if(!loadChannel(loadingChannel, *filename)){
		Serial.println("MixScreen: load command rejected");
	}
	delete filename;
}

bool MixScreen::MixScreen::loadChannel(uint8_t channel, const String& path){
	if(!session) return false;
	return session->loadDeck(channel, path.c_str(), DJ_ORIGIN_LOCAL_UI).accepted();
}

void MixScreen::MixScreen::setBigVuStarted(bool bigVuStarted){
	MixScreen::bigVuStarted = bigVuStarted;
}

bool MixScreen::MixScreen::syncFromSnapshot(const DjSnapshot& snapshot, bool force){
	bool changed = force;
	for(uint8_t deck = 0; deck < DJ_DECK_COUNT; deck++){
		const DjDeckSnapshot& deckSnapshot = snapshot.decks[deck];
		SongSeekBar* bar = deck == 0 ? leftSeekBar : rightSeekBar;
		SongName* nameLabel = deck == 0 ? leftSongName : rightSongName;

		if(force || strcmp(displayedPaths[deck], deckSnapshot.path) != 0){
			memcpy(displayedPaths[deck], deckSnapshot.path, DJ_PATH_CAPACITY);
			String name = deckSnapshot.path;
			const int slash = name.lastIndexOf('/');
			const int end = name.endsWith(".aac") ? name.length() - 4 : name.length();
			nameLabel->setSongName(name.substring(slash + 1, end));
			nameLabel->checkScrollUpdate();
			changed = true;
		}

		if(bar->getTotalDuration() != deckSnapshot.duration){
			bar->setTotalDuration(deckSnapshot.duration);
			changed = true;
		}
		if(seekTime == 0 || seekChannel != deck){
			if(bar->getCurrentDuration() != deckSnapshot.elapsed){
				bar->setCurrentDuration(deckSnapshot.elapsed);
				changed = true;
			}
			if(bar->isPlaying() != deckSnapshot.playing){
				bar->setPlaying(deckSnapshot.playing);
				changed = true;
			}
		}

		for(uint8_t slot = 0; slot < DJ_EFFECT_SLOT_COUNT; slot++){
			EffectElement* element = effectElements[deck * DJ_EFFECT_SLOT_COUNT + slot];
			const EffectType type = static_cast<EffectType>(deckSnapshot.effects[slot].type);
			if(force || element->getType() != type){
				element->setType(type);
				changed = true;
			}
			if(element->getIntensity() != deckSnapshot.effects[slot].intensity){
				element->setIntensity(deckSnapshot.effects[slot].intensity);
				changed = true;
			}
		}
	}
	if(isRecording != snapshot.recording){
		isRecording = snapshot.recording;
		changed = true;
	}
	return changed;
}

void MixScreen::MixScreen::start(){
	if(doneRecording){
		lastDraw = 0;
		draw();
		screen.commit();
		saveRecording();
	}

	if(!session) return;
	DjSnapshot snapshot;
	session->copySnapshot(snapshot);
	const bool hasTarget = snapshot.decks[0].path[0] != '\0' || snapshot.decks[1].path[0] != '\0';
	if(!hasTarget && !session->hasPendingLoad()){
		loadingChannel = 0;
		songListOpen = true;
		(new SongList::SongList(*getScreen().getDisplay()))->push(this);
		return;
	}

	session->attachView(leftVu.getInfoGenerator(), rightVu.getInfoGenerator(), midVu.getInfoGenerator());
	syncFromSnapshot(snapshot, true);
	if(bigVuStarted){
		startBigVu();
	}

	matrixManager.fillMatrixMid(snapshot.mix);
	matrixManager.matrixMid.push();

	LoopManager::addListener(&leftVu);
	LoopManager::addListener(&rightVu);
	LoopManager::addListener(this);

	Input.addListener(this);
	InputJayD::getInstance()->addListener(this);

	draw();
	screen.commit();
}

void MixScreen::MixScreen::stop(){
	LoopManager::removeListener(&leftVu);
	LoopManager::removeListener(&rightVu);
	LoopManager::removeListener(&midVu);
	LoopManager::removeListener(this);

	Input.removeListener(this);
	InputJayD::getInstance()->removeListener(this);
	if(session) session->detachView();

	if(bigVuStarted){
		stopBigVu();
	}else{
		if(!matrixManager.matrixBig.getAnimations().empty()){
			delete *matrixManager.matrixBig.getAnimations().begin();
		}
	}

}

void MixScreen::MixScreen::draw(){
	screen.getSprite()->fillRect(79, 0, 2, 128, TFT_BLACK);
	screen.getSprite()->fillRect(leftLayout->getTotalX(), leftLayout->getTotalY(), 79, 128, C_RGB(249, 93, 2));
	screen.getSprite()->fillRect(rightLayout->getTotalX(), rightLayout->getTotalY(), 79, 128, C_RGB(3, 52, 135));
	if(!selectedChannel){
		screen.getSprite()->drawIcon(selectedBackgroundBuffer, screen.getTotalX(), screen.getTotalY(), 79, 128, 1, TFT_TRANSPARENT);
	}else{
		screen.getSprite()->drawIcon(selectedBackgroundBuffer, screen.getTotalX() + 81, screen.getTotalY(), 79, 128, 1, TFT_TRANSPARENT);
	}

	if(isRecording){
		screen.getSprite()->fillCircle(79, 64, 6, TFT_BLACK);
		screen.getSprite()->fillCircle(79, 64, 4, TFT_RED);
	}
	screen.draw();

	if(doneRecording){
		drawSaveStatus();
	}
}

void MixScreen::MixScreen::drawSaveStatus(){
	Sprite* canvas = screen.getSprite();

	canvas->fillRoundRect((screen.getWidth() - 80) / 2, (screen.getHeight() - 40) / 2, 80, 40, 2, C_RGB(52, 204, 235));
	canvas->drawRoundRect((screen.getWidth() - 80) / 2, (screen.getHeight() - 40) / 2, 80, 40, 2, TFT_BLACK);

	canvas->setTextColor(TFT_WHITE);
	canvas->setFont(&u8g2_font_DigitalDisco_tf);
	canvas->setTextDatum(BC_DATUM);
	canvas->drawString("Saving...", screen.getWidth() / 2, (screen.getHeight() - 40) / 2 + 23);
	canvas->setTextDatum(TL_DATUM);

	canvas->fillRoundRect((screen.getWidth() - 80) / 2 + 10 + (cos((float) millis() / 200.0f)+1) / 2.0f * 45.0f, (screen.getHeight() - 40) / 2 + 30, 15, 5, 2, TFT_WHITE);
}

void MixScreen::MixScreen::buildUI(){
	screenLayout->setWHType(PARENT, PARENT);
	screenLayout->setGutter(2);
	screenLayout->addChild(leftLayout);
	screenLayout->addChild(rightLayout);

	leftLayout->setWHType(FIXED, PARENT);
	leftLayout->setWidth(79);
	leftLayout->setGutter(10);
	leftLayout->setPadding(1);


	leftLayout->addChild(leftSeekBar);
	leftLayout->addChild(leftSongName);

	for(int i = 0; i < 3; i++){
		leftLayout->addChild(effectElements[i]);
	}


	rightLayout->setWHType(FIXED, PARENT);
	rightLayout->setWidth(79);
	rightLayout->setGutter(10);
	rightLayout->setPadding(1);


	rightLayout->addChild(rightSeekBar);
	rightLayout->addChild(rightSongName);

	for(int i = 3; i < 6; i++){
		rightLayout->addChild(effectElements[i]);
	}

	screenLayout->reflow();
	leftLayout->reflow();
	rightLayout->reflow();

	screen.addChild(screenLayout);
	screen.repos();
}

void MixScreen::MixScreen::loop(uint micros){
	if(seekTime != 0 && millis() - seekTime >= 100){
		SongSeekBar* bar = seekChannel ? rightSeekBar : leftSeekBar;

		session->seek(seekChannel, bar->getCurrentDuration(), DJ_ORIGIN_PHYSICAL);

		if(wasRunning){
			session->setPlaying(seekChannel, true, DJ_ORIGIN_PHYSICAL);
		}

		seekChannel = -1;
		seekTime = 0;
	}

	bool update = false;
	for(const auto& element : effectElements){
		update |= element->needsUpdate();
	}

	DjSnapshot snapshot;
	if(session && session->copySnapshot(snapshot)){
		update |= syncFromSnapshot(snapshot);
		const bool hasTarget = snapshot.decks[0].path[0] != '\0' || snapshot.decks[1].path[0] != '\0';
		if(!hasTarget && !songListOpen && !session->hasPendingLoad()){
			loadingChannel = 0;
			songListOpen = true;
			(new SongList::SongList(*getScreen().getDisplay()))->push(this);
			return;
		}
	}

	bool songNameUpdateL = leftSongName->checkScrollUpdate();
	bool songNameUpdateR = rightSongName->checkScrollUpdate();
	update |= songNameUpdateL | songNameUpdateR;

	uint32_t currentTime = millis();
	if((update || drawQueued) && (currentTime - lastDraw) >= (isRecording ? 200 : 50)){
		drawQueued = false;
		draw();
		screen.commit();
		lastDraw = currentTime;
	}else if(update){
		drawQueued = true;
	}
}


void MixScreen::MixScreen::potMove(uint8_t id, uint8_t value){
	if(id == POT_MID){
		session->setMix(value, DJ_ORIGIN_PHYSICAL);
		matrixManager.fillMatrixMid(value);
		matrixManager.matrixMid.push();
	}else if(id == POT_L){
		session->setGain(0, value, DJ_ORIGIN_PHYSICAL);
	}else if(id == POT_R){
		session->setGain(1, value, DJ_ORIGIN_PHYSICAL);
	}
}

void MixScreen::MixScreen::startBigVu(){
	LoopManager::addListener(&midVu);
}

void MixScreen::MixScreen::stopBigVu(){
	LoopManager::removeListener(&midVu);
}

void MixScreen::MixScreen::encTwoBot(){
	if(isRecording){
		if(!session->setRecording(false, DJ_ORIGIN_PHYSICAL).accepted()) return;
		doneRecording = true;

		(new TextInputScreen::TextInputScreen(*screen.getDisplay()))->push(this);
	}else{
		session->setRecording(true, DJ_ORIGIN_PHYSICAL);
	}
}

void MixScreen::MixScreen::encTwoTop(){
	if(session){
		session->detachView();
		DjSession::end();
		session = nullptr;
	}
	pop();
}

void MixScreen::MixScreen::btnCombination(){
	stop();

	MatrixPopUpPicker* popUpPicker = new MatrixPopUpPicker(*this);
	popUpPicker->unpack();
	popUpPicker->start();
}

void MixScreen::MixScreen::btn(uint8_t i){
	SongSeekBar* bar = i == 0 ? leftSeekBar : rightSeekBar;
	const bool playing = !bar->isPlaying();
	if(!session->setPlaying(i, playing, DJ_ORIGIN_PHYSICAL).accepted()) return;
	bar->setPlaying(playing);

	drawQueued = true;
}

void MixScreen::MixScreen::btnEnc(uint8_t i){
	if(i > 6) return;

	if(i == 6){
		selectedChannel = !selectedChannel;
	}else{
		EffectElement* effect = effectElements[i];
		effect->setSelected(!effect->isSelected());
	}

	drawQueued = true;
}

void MixScreen::MixScreen::enc(uint8_t index, int8_t value){

	if(index == 6){
		DjSnapshot snapshot;
		if(!session->copySnapshot(snapshot) || !snapshot.decks[selectedChannel].loaded) return;
		if(seekTime == 0){
			seekChannel = selectedChannel;
			wasRunning = snapshot.decks[selectedChannel].playing;
			session->setPlaying(selectedChannel, false, DJ_ORIGIN_PHYSICAL);
		}

		seekTime = millis();

		SongSeekBar* bar = seekChannel ? rightSeekBar : leftSeekBar;
		uint16_t seekTime = constrain(bar->getCurrentDuration() + value, 0, bar->getTotalDuration());
		bar->setCurrentDuration(seekTime);

		drawQueued = true;
		return;
	}

	EffectElement* element = effectElements[index];

	if(element->isSelected()){
		int8_t e = element->getType() + value;
		if(e >= EffectType::COUNT){
			e = e % EffectType::COUNT;
		}else if(e < 0){
			while(e < 0){
				e += EffectType::COUNT;
			}
		}

		// Only one speed allowed
		if(e == EffectType::SPEED){
			for(int i = (index < 3 ? 0 : 3); i < (index < 3 ? 3 : 6); i++){
				if(i == index) continue;
				if(effectElements[i]->getType() != EffectType::SPEED) continue;

				if(value < 0){
					e = e > 0 ? e - 1 : EffectType::COUNT - 1;
				}else{
					e = (e + 1) % EffectType::COUNT;
				}

				break;
			}
		}

		EffectType type = static_cast<EffectType>(e);
		const uint8_t deck = index >= 3;
		const uint8_t slot = index % 3;
		if(!session->setEffectType(deck, slot, type, DJ_ORIGIN_PHYSICAL).accepted()) return;
		element->setType(type);
		element->setIntensity(0);

		if(type == EffectType::SPEED){
			element->setIntensity(255 / 2);
			return;
		}
	}else{
		EffectType type = element->getType();
		if(type == EffectType::NONE) return;

		int16_t intensity = element->getIntensity() + value * 5;
		intensity = max((int16_t) 0, intensity);
		intensity = min((int16_t) 255, intensity);

		if(!session->setEffectIntensity(index >= 3, index % 3, intensity, DJ_ORIGIN_PHYSICAL).accepted()) return;
		element->setIntensity(intensity);
	}

	drawQueued = true;
}

void MixScreen::MixScreen::encBtnHold(uint8_t i){
	if(i == 6){
		loadingChannel = selectedChannel;
		songListOpen = true;
		(new SongList::SongList(*getScreen().getDisplay()))->push(this);
		return;
	}
	if(i > 5) return;

	// Reuses the previously-unbound per-slot encoder hold (L1-L3/R1-R3) as a
	// stable, labeled LOOP/SYNC bank, gated on the beat-grid capability by
	// DjSession itself (setSync/loopEngage reject rather than silently
	// no-op when the grid/BPM capability is unavailable). Mix (potMove),
	// Cues/transport (btn), and Browse (enc/encBtnHold on index 6) are
	// untouched. No new multi-button chords are introduced.
	const uint8_t deck = i >= 3;
	const uint8_t slot = i % 3; // slot 0/1 -> LOOP bank, slot 2 -> SYNC/ASSIST

	DjSnapshot snapshot;
	session->copySnapshot(snapshot);

	if(slot == 2){
		const bool currentlyArmed = snapshot.decks[deck].sync.state != DJ_SYNC_OFF;
		session->setSync(deck, !currentlyArmed, -1, DJ_ORIGIN_PHYSICAL);
	}else if(snapshot.decks[deck].loop.state != DJ_LOOP_INACTIVE){
		session->loopDisengage(deck, DJ_ORIGIN_PHYSICAL);
	}else{
		const DjLoopLength length = slot == 0 ? DJ_LOOP_BEAT_1 : DJ_LOOP_BEAT_4;
		session->loopEngage(deck, length, DJ_ORIGIN_PHYSICAL);
	}

	drawQueued = true;
}
