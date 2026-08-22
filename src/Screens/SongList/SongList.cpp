#include <SD.h>
#include "SongList.h"
#include "../MainMenu/MainMenu.h"
#include <JayD.h>
#include <SPIFFS.h>
#include <FS/CompressedFile.h>
#include "../../Fonts.h"

namespace {
bool isVisibleAAC(const char* path){
	const char* name = strrchr(path, '/');
	name = name == nullptr ? path : name + 1;
	if(name[0] == '.') return false;

	const size_t length = strlen(name);
	if(length < 4 || name[length - 4] != '.') return false;

	return (name[length - 3] == 'a' || name[length - 3] == 'A') &&
		   (name[length - 2] == 'a' || name[length - 2] == 'A') &&
		   (name[length - 1] == 'c' || name[length - 1] == 'C');
}
}

SongList::SongList* SongList::SongList::instance = nullptr;

SongList::SongList::SongList(Display& display) : Context(display){
	instance = this;
	SongList::pack();
}

SongList::SongList::~SongList(){
	instance = nullptr;
	clearSongs();
	free(backgroundBuffer);
}

void SongList::SongList::clearSongs(){
	free(pathBuffer);
	free(songOffsets);
	pathBuffer = nullptr;
	songOffsets = nullptr;
	pathBytes = 0;
	pathCapacity = 0;
	songCount = 0;
	songCapacity = 0;
}

bool SongList::SongList::reservePaths(size_t required){
	if(required <= pathCapacity) return true;

	size_t capacity = pathCapacity == 0 ? 4096 : pathCapacity;
	while(capacity < required && capacity < maxPathPayload){
		capacity = min(capacity * 2, maxPathPayload);
	}
	if(capacity < required) return false;

	char* resized = static_cast<char*>(ps_realloc(pathBuffer, capacity));
	if(resized == nullptr) return false;
	pathBuffer = resized;
	pathCapacity = capacity;
	return true;
}

bool SongList::SongList::reserveSongs(size_t required){
	if(required <= songCapacity) return true;

	size_t capacity = songCapacity == 0 ? 64 : songCapacity;
	while(capacity < required && capacity < maxTrackCount){
		capacity = min(capacity * 2, maxTrackCount);
	}
	if(capacity < required) return false;

	uint32_t* resized = static_cast<uint32_t*>(
		ps_realloc(songOffsets, capacity * sizeof(uint32_t))
	);
	if(resized == nullptr) return false;
	songOffsets = resized;
	songCapacity = capacity;
	return true;
}

bool SongList::SongList::addSong(const char* path){
	const size_t length = strnlen(path, maxPathLength + 1);
	if(length == 0 || length > maxPathLength){
		scanLimited = true;
		return true;
	}

	const size_t storedLength = length + 1;
	if(songCount >= maxTrackCount || storedLength > maxPathPayload - pathBytes){
		scanLimited = true;
		return false;
	}

	if(!reservePaths(pathBytes + storedLength) || !reserveSongs(songCount + 1)){
		allocationFailed = true;
		return false;
	}

	songOffsets[songCount++] = pathBytes;
	memcpy(pathBuffer + pathBytes, path, storedLength);
	pathBytes += storedLength;
	return true;
}

const char* SongList::SongList::songPath(size_t index) const{
	if(index >= songCount || pathBuffer == nullptr || songOffsets == nullptr) return nullptr;
	return pathBuffer + songOffsets[index];
}

void SongList::SongList::checkSD(){
	clearSongs();
	selectedElement = 0;
	firstVisible = 0;
	empty = true;
	scanLimited = false;
	allocationFailed = false;
	waiting = true;

	if(!insertedSD){
		insertedSD = SD.begin(22, SPI);
	}

	if(!insertedSD){
		waiting = false;
		draw();
		screen.commit();
		return;
	}

	draw();
	screen.commit();

	File root = SD.open("/");
	insertedSD = root;
	if(!insertedSD){
		root.close();
		waiting = false;
		draw();
		screen.commit();
		return;
	}

	searchDirectories(root);
	root.close();

	if(allocationFailed){
		Serial.printf(
			"SongList: allocation failed after %u tracks (%u bytes)\n",
			static_cast<unsigned int>(songCount),
			static_cast<unsigned int>(pathBytes)
		);
		clearSongs();
	}else{
		if(pathBytes < pathCapacity){
			char* compacted = static_cast<char*>(ps_realloc(pathBuffer, pathBytes));
			if(compacted != nullptr) pathBuffer = compacted;
			pathCapacity = pathBytes;
		}
		if(songCount < songCapacity){
			uint32_t* compacted = static_cast<uint32_t*>(
				ps_realloc(songOffsets, songCount * sizeof(uint32_t))
			);
			if(compacted != nullptr) songOffsets = compacted;
			songCapacity = songCount;
		}
	}

	waiting = false;
	empty = songCount == 0;
	Serial.printf(
		"SongList: indexed %u tracks (%u path bytes)%s\n",
		static_cast<unsigned int>(songCount),
		static_cast<unsigned int>(pathBytes),
		scanLimited ? ", limit reached" : ""
	);
	draw();
	screen.commit();
}

bool SongList::SongList::searchDirectories(File dir){
	if(!dir) return true;

	File f;
	while(f = dir.openNextFile()){
		if(f.isDirectory()){
			const bool keepScanning = searchDirectories(f);
			f.close();
			if(!keepScanning) return false;
			continue;
		}

		const char* path = f.name();
		if(!isVisibleAAC(path)){
			f.close();
			continue;
		}

		const bool keepScanning = addSong(path);
		f.close();
		if(!keepScanning) return false;
	}
	return true;
}

void SongList::SongList::loop(uint t){}

void SongList::SongList::start(){

	InputJayD::getInstance()->setEncoderMovedCallback(ENC_MID, [](int8_t value){
		if(instance == nullptr) return;

		if(instance->empty || !instance->insertedSD) return;

		instance->selectedElement += value;
		if(instance->selectedElement < 0){
			instance->selectedElement = 0;
		}else if(instance->selectedElement >= instance->songCount){
			instance->selectedElement = instance->songCount - 1;
		}

		if(instance->selectedElement < instance->firstVisible){
			instance->firstVisible = instance->selectedElement;
		}else if(instance->selectedElement >= instance->firstVisible + visibleRows){
			instance->firstVisible = instance->selectedElement - visibleRows + 1;
		}
		instance->draw();
		instance->screen.commit();


	});

	InputJayD::getInstance()->setBtnPressCallback(BTN_MID, [](){
		if(instance == nullptr) return;

		if(!instance->insertedSD){
			instance->checkSD();
			return;
		}

		if(instance->empty || !instance->insertedSD || instance->songCount <= instance->selectedElement) return;

		const char* selectedPath = instance->songPath(instance->selectedElement);
		if(selectedPath == nullptr) return;
		String path(selectedPath);
		fs::File file = SD.open(path);
		if(!file){
			file.close();
			SD.end();
			instance->insertedSD = false;
			instance->checkSD();
			return;
		}
		file.close();

		instance->pop(new String(path));
	});

	Input.addListener(this);
	waiting = false;
	checkSD();

	draw();
	screen.commit();
}

void SongList::SongList::stop(){
	InputJayD::getInstance()->removeEncoderMovedCallback(ENC_MID);
	InputJayD::getInstance()->removeBtnPressCallback(BTN_MID);
	Input.removeListener(this);
}

void SongList::SongList::draw(){

	Sprite* canvas = screen.getSprite();

	canvas->setFont(&u8g2_font_DigitalDisco_tf);
	canvas->setTextColor(TFT_WHITE);

	if(backgroundBuffer == nullptr){
		canvas->clear(TFT_BLACK);
	}else{
		canvas->drawIcon(backgroundBuffer, 0, 0, 160, 128, 1);
	}

	if(backgroundBuffer != nullptr){
		canvas->drawIcon(backgroundBuffer, 0, 0, 160, 19, 1);
	}

	canvas->setTextDatum(BC_DATUM);
	canvas->drawString(scanLimited ? "SD card LIMIT" : "SD card", screen.getWidth()/2, 15);

	if(waiting){
		canvas->drawString("Loading...", screen.getWidth()/2, 65);
		canvas->setTextDatum(TL_DATUM);
		return;
	}

	if(!insertedSD){
		canvas->drawString("Not inserted!", screen.getWidth()/2, 65);
		canvas->setTextDatum(TL_DATUM);
		return;
	}else if(empty){
		canvas->drawString(allocationFailed ? "Memory error!" : "Empty!", screen.getWidth()/2, 65);
		canvas->setTextDatum(TL_DATUM);
		return;
	}

	canvas->setFont(&u8g2_font_profont12_tf);
	canvas->setTextDatum(CL_DATUM);
	for(uint8_t row = 0; row < visibleRows; row++){
		const size_t index = firstVisible + row;
		const char* path = songPath(index);
		if(path == nullptr) break;

		const int y = 23 + row * rowHeight;
		if(index == selectedElement){
			canvas->drawRect(3, y, 154, rowHeight - 2, TFT_LIGHTGREY);
		}

		const char* filename = strrchr(path, '/');
		filename = filename == nullptr ? path : filename + 1;
		size_t length = strlen(filename);
		if(length >= 4) length -= 4;

		char label[maxPathLength + 4];
		memcpy(label, filename, length);
		label[length] = '\0';
		if(canvas->textWidth(label) > 142){
			size_t low = 0;
			size_t high = length;
			while(low < high){
				const size_t middle = (low + high + 1) / 2;
				memcpy(label, filename, middle);
				memcpy(label + middle, "...", 4);
				if(canvas->textWidth(label) <= 142){
					low = middle;
				}else{
					high = middle - 1;
				}
			}
			memcpy(label, filename, low);
			memcpy(label + low, "...", 4);
		}
		canvas->drawString(label, 8, y + rowHeight / 2);
	}
	canvas->setTextDatum(TL_DATUM);
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
		return;
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
