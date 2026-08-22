#include <SD.h>
#include "SongList.h"
#include "../MainMenu/MainMenu.h"
#include <JayD.h>
#include <Loop/LoopManager.h>
#include <SPIFFS.h>
#include <FS/CompressedFile.h>
#include "../../Fonts.h"
#include "../../DjSession/DjSession.h"
#include "../MixScreen/MixControlState.h"

SongList::SongList* SongList::SongList::instance = nullptr;

SongList::SongList::SongList(Display& display, DjSession* browseSession) :
		Context(display), browseSession(browseSession), browseMode(browseSession != nullptr){
	instance = this;

	scrollLayout = new ScrollLayout(&getScreen());
	list = new LinearLayout(scrollLayout, VERTICAL);

	buildUI();
	SongList::pack();
}

SongList::SongList::~SongList(){
	instance = nullptr;
	free(backgroundBuffer);
}

void SongList::SongList::checkSD(){
	for(auto song : songs){
		delete song;
	}
	list->getChildren().clear();
	songs.clear();
	selectedElement = 0;
	empty = true;

	if(!insertedSD){
		insertedSD = SD.begin(22, SPI);
	}

	if(!insertedSD){
		draw();
		screen.commit();
		return;
	}

	// TODO
	// Empty card inserted, taken out, press refresh
	// SD started, opened root returns true
	File root = SD.open("/");
	insertedSD = root;
	if(!insertedSD){
		root.close();
		draw();
		screen.commit();
		return;
	}

	searchDirectories(root);
	root.close();
	empty = songs.empty();

	if(!empty){
		list->reflow();
		list->repos();
		scrollLayout->scrollIntoView(0, 5);
		songs.front()->setSelected(true);
	}

	draw();
	screen.commit();
}

void SongList::SongList::searchDirectories(File dir){
	if(!dir) return;

	File f;
	while(f = dir.openNextFile()){
		if(f.isDirectory()){
			searchDirectories(f);
			f.close();
			continue;
		}

		String name = f.name();
		name.toLowerCase();
		if(!name.endsWith(".aac") || name[name.lastIndexOf('/') + 1] == '.'){
			f.close();
			continue;
		}

		songs.push_back(new ListItem(list, f.name()));
		list->addChild(songs.back());

		f.close();
	}
}

void SongList::SongList::loop(uint t){
	updateBrowseResult();
	if(!insertedSD || empty) return;
	if(songs[selectedElement]->checkScrollUpdate()) {
		draw();
		screen.commit();
	}
}

void SongList::SongList::start(){
	Input.addListener(this);
	waiting = false;
	checkSD();

	LoopManager::addListener(this);

	draw();
	screen.commit();
}

void SongList::SongList::stop(){
	Input.removeListener(this);
	LoopManager::removeListener(this);
}

void SongList::SongList::draw(){

	Sprite* canvas = screen.getSprite();

	canvas->setFont(&u8g2_font_DigitalDisco_tf);
	canvas->setTextColor(TFT_WHITE);

	canvas->drawIcon(backgroundBuffer, 0, 0, 160, 128, 1);

	screen.draw();

	canvas->drawIcon(backgroundBuffer, 0, 0, 160, 19, 1);

	canvas->setTextDatum(BC_DATUM);
	canvas->drawString(browseMode ? "BROWSE  CENTER: RESCAN" : "SD card", screen.getWidth()/2, 15);

	if(waiting){
		canvas->drawString("Loading...", screen.getWidth()/2, 65);
		canvas->setTextDatum(TL_DATUM);
		return;
	}

	if(!insertedSD){
		canvas->drawString("Not inserted!", screen.getWidth()/2, 65);
		canvas->setTextDatum(TL_DATUM);
	}else if(empty){
		canvas->drawString("Empty!", screen.getWidth()/2, 65);
		canvas->setTextDatum(TL_DATUM);
	}

	if(browseMode){
		canvas->fillRect(0, 111, 160, 17, TFT_BLACK);
		canvas->setTextColor(TFT_WHITE);
		canvas->setTextDatum(BC_DATUM);
		const String footer = browseStatus.length() ? browseStatus : "A: LOAD A       B: LOAD B";
		canvas->drawString(footer.substring(0, 26), 80, 124);
		canvas->setTextDatum(TL_DATUM);
	}
}

void SongList::SongList::buildUI(){
	scrollLayout->setWHType(PARENT, FIXED);
	scrollLayout->setHeight(browseMode ? 92 : 110);
	scrollLayout->addChild(list);

	list->setWHType(PARENT, CHILDREN);
	list->setPadding(5);
	list->setGutter(10);

	scrollLayout->reflow();
	list->reflow();

	screen.addChild(scrollLayout);
	screen.repos();
	scrollLayout->setY(18);
}

void SongList::SongList::pack(){
	Context::pack();
	free(backgroundBuffer);
	backgroundBuffer = nullptr;
}

void SongList::SongList::unpack(){
	Context::unpack();

	waiting = true;

	backgroundBuffer = static_cast<Color*>(ps_malloc(160 * 128 * 2));
	if(backgroundBuffer == nullptr){
		Serial.println("SongList bg buffer error");
	}

	fs::File bgFile = CompressedFile::open(SPIFFS.open("/SongListBackground.raw.hs"), 10, 9);
	bgFile.read(reinterpret_cast<uint8_t*>(backgroundBuffer), 160 * 128 * 2);
	bgFile.close();
}

void SongList::SongList::encTwoTop(){
	delete parent;
	stop();
	delete this;
	MainMenu::MainMenu::getInstance()->unpack();
	MainMenu::MainMenu::getInstance()->start();
}

void SongList::SongList::btn(uint8_t i){
	const int8_t deck = MixControlState::browseDeckForButton(i);
	if(!browseMode || deck < 0) return;
	loadSelected(deck);
}

void SongList::SongList::btnEnc(uint8_t i){
	if(i != 6) return;
	if(browseMode){
		browseStatus = "RESCANNING SD...";
		checkSD();
		browseStatus = insertedSD ? "SD RESCANNED" : "SD NOT AVAILABLE";
		draw();
		screen.commit();
		return;
	}

	if(!insertedSD){
		checkSD();
		return;
	}

	String path;
	if(selectedPath(path)) pop(new String(path));
}

void SongList::SongList::enc(uint8_t i, int8_t value){
	if(i == 6) moveSelection(value);
}

void SongList::SongList::encBtnHold(uint8_t i){
	if(i == 6 && browseMode) pop();
}

bool SongList::SongList::allowsEncoderChords() const{
	return !browseMode;
}

void SongList::SongList::moveSelection(int8_t value){
	if(empty || !insertedSD || value == 0) return;
	songs[selectedElement]->setSelected(false);
	selectedElement += value;
	if(selectedElement < 0){
		selectedElement = 0;
	}else if(selectedElement >= songs.size()){
		selectedElement = songs.size() - 1;
	}
	songs[selectedElement]->setSelected(true);
	scrollLayout->scrollIntoView(selectedElement, 6);
	draw();
	screen.commit();
}

bool SongList::SongList::selectedPath(String& path){
	if(empty || !insertedSD || songs.size() <= selectedElement) return false;
	path = songs[selectedElement]->getPath();
	fs::File file = SD.open(path);
	if(file) {
		file.close();
		return true;
	}
	file.close();
	SD.end();
	insertedSD = false;
	checkSD();
	return false;
}

void SongList::SongList::loadSelected(uint8_t deck){
	if(!browseSession || deck >= DJ_DECK_COUNT) return;
	String path;
	if(!selectedPath(path)) return;
	const DjSubmitResult result = browseSession->loadDeck(deck, path.c_str(), DJ_ORIGIN_PHYSICAL);
	if(!result.accepted()){
		browseStatus = "LOAD REJECTED";
		draw();
		screen.commit();
		return;
	}
	pendingLoad[deck] = result.id;
	browseStatus = deck == 0 ? "LOADING DECK A..." : "LOADING DECK B...";
	draw();
	screen.commit();
}

void SongList::SongList::updateBrowseResult(){
	if(!browseMode || (!pendingLoad[0] && !pendingLoad[1])) return;
	DjSnapshot snapshot;
	if(!browseSession->copySnapshot(snapshot)) return;
	for(const auto& result : snapshot.recentResults){
		for(uint8_t deck = 0; deck < DJ_DECK_COUNT; deck++){
			if(!pendingLoad[deck] || result.id != pendingLoad[deck] ||
			   result.status == DJ_COMMAND_ACCEPTED) continue;
			pendingLoad[deck] = 0;
			if(result.status == DJ_COMMAND_APPLIED){
				browseStatus = deck == 0 ? "LOADED DECK A" : "LOADED DECK B";
			}else{
				browseStatus = "LOAD FAILED";
			}
			draw();
			screen.commit();
		}
	}
}
