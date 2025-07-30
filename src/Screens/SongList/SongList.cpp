#include <SD.h>
#include "SongList.h"
#include "../MainMenu/MainMenu.h"
#include <JayD.h>
#include <Loop/LoopManager.h>
#include <SPIFFS.h>
#include <FS/CompressedFile.h>
#include "../../Fonts.h"
#include <algorithm>

SongList::SongList* SongList::SongList::instance = nullptr;
const char* SongList::SongList::indexFileName = "/.jayd_song_index";

SongList::SongList::SongList(Display& display) : Context(display){
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

	if(loadSongIndex()){
		Serial.printf("Loaded %d songs from index\n", songs.size());
		// Sort loaded songs alphabetically
		if(!songs.empty()){
			std::sort(songs.begin(), songs.end(), [](ListItem* a, ListItem* b) {
				String pathA = a->getPath();
				String pathB = b->getPath();
				// Extract filename for comparison
				String fileA = pathA.substring(pathA.lastIndexOf('/') + 1);
				String fileB = pathB.substring(pathB.lastIndexOf('/') + 1);
				fileA.toLowerCase();
				fileB.toLowerCase();
				return fileA < fileB;
			});
			
			// Rebuild UI with sorted order
			list->getChildren().clear();
			for(auto song : songs){
				list->addChild(song);
			}
		}
	}else{
		Serial.println("Index not found or outdated, scanning SD card...");
		scanning = true;
		scanProgress = 0;
		totalFiles = 0;
		draw();
		screen.commit();
		searchDirectories(root);
		scanning = false;
		Serial.printf("Scan complete: found %d songs\n", songs.size());
		
		// Sort songs alphabetically
		if(!songs.empty()){
			Serial.println("Sorting songs alphabetically...");
			std::sort(songs.begin(), songs.end(), [](ListItem* a, ListItem* b) {
				String pathA = a->getPath();
				String pathB = b->getPath();
				// Extract filename for comparison
				String fileA = pathA.substring(pathA.lastIndexOf('/') + 1);
				String fileB = pathB.substring(pathB.lastIndexOf('/') + 1);
				fileA.toLowerCase();
				fileB.toLowerCase();
				return fileA < fileB;
			});
			
			// Rebuild UI with sorted order
			list->getChildren().clear();
			for(auto song : songs){
				list->addChild(song);
			}
			Serial.println("Songs sorted alphabetically");
		}
		
		draw();
		screen.commit();
		if(!songs.empty()){
			createSongIndex();
		}
	}
	root.close();
	empty = songs.empty();

	// Always add the manual scan option at the bottom
	songs.push_back(new ListItem(list, "⚙ Scan SD Card"));
	list->addChild(songs.back());

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
		String name = f.name();
		String fileName = name.substring(name.lastIndexOf('/') + 1);
		
		// Skip hidden files and directories (starting with . or ._)
		if(fileName.startsWith(".") || fileName.startsWith("._")){
			f.close();
			continue;
		}
		
		if(f.isDirectory()){
			Serial.printf("Scanning directory: %s\n", name.c_str());
			searchDirectories(f);
			f.close();
			continue;
		}

		// Only process .aac files
		String lowerName = fileName;
		lowerName.toLowerCase();
		if(!lowerName.endsWith(".aac")){
			f.close();
			continue;
		}

		totalFiles++;
		if(totalFiles % 10 == 0 && scanning){
			draw();
			screen.commit();
		}

		Serial.printf("Found AAC file: %s\n", name.c_str());
		songs.push_back(new ListItem(list, name));
		list->addChild(songs.back());
		scanProgress++;

		f.close();
	}
}

void SongList::SongList::loop(uint t){
	if(!insertedSD || empty) return;
	if(songs[selectedElement]->checkScrollUpdate()) {
		draw();
		screen.commit();
	}
}

void SongList::SongList::start(){

	InputJayD::getInstance()->setEncoderMovedCallback(ENC_MID, [](int8_t value){
		if(instance == nullptr) return;

		if(instance->empty || !instance->insertedSD) return;

		instance->songs[instance->selectedElement]->setSelected(false);
		instance->selectedElement += value;
		
		// Circular navigation - wrap around at both ends
		if(instance->selectedElement < 0){
			instance->selectedElement = instance->songs.size() - 1;
		}else if(instance->selectedElement >= instance->songs.size()){
			instance->selectedElement = 0;
		}

		instance->songs[instance->selectedElement]->setSelected(true);

		instance->scrollLayout->scrollIntoView(instance->selectedElement, 6);
		instance->draw();
		instance->screen.commit();


	});

	InputJayD::getInstance()->setBtnPressCallback(BTN_MID, [](){
		if(instance == nullptr) return;

		if(!instance->insertedSD){
			instance->checkSD();
			return;
		}

		if(instance->songs.size() <= instance->selectedElement) return;

		String path = instance->songs[instance->selectedElement]->getPath();
		
		// Check if scan option was selected
		if(path == "⚙ Scan SD Card"){
			Serial.println("=== MANUAL SD SCAN TRIGGERED ===");
			instance->forceScanSD();
			instance->checkSD(); // Refresh the list
			return;
		}

		if(instance->empty || !instance->insertedSD) return;

		fs::File file = SD.open(path);
		if(!file){
			file.close();
			SD.end();
			instance->insertedSD = false;
			instance->checkSD();
			return;
		}
		file.close();

		Serial.printf("\n=== SONGLIST SELECTION ===\n");
		Serial.printf("Selected file: %s\n", path.c_str());
		Serial.printf("Free heap before pop: %u bytes\n", ESP.getFreeHeap());
		Serial.println("Calling pop() to return to MixScreen...");
		
		instance->pop(new String(path));
		
		Serial.println("=== SONGLIST SELECTION END ===\n");
	});

	Input.addListener(this);
	waiting = false;
	checkSD();

	LoopManager::addListener(this);

	draw();
	screen.commit();
}

void SongList::SongList::stop(){
	InputJayD::getInstance()->removeEncoderMovedCallback(ENC_MID);
	InputJayD::getInstance()->removeBtnPressCallback(BTN_MID);
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
	String headerText = "SD card";
	if(!empty && insertedSD){
		headerText += " - " + String(songs.size()) + " songs";
	}
	canvas->drawString(headerText, screen.getWidth()/2, 15);

	if(waiting){
		canvas->drawString("Loading...", screen.getWidth()/2, 65);
		canvas->setTextDatum(TL_DATUM);
		return;
	}
	
	if(scanning){
		canvas->drawString("Scanning SD...", screen.getWidth()/2, 50);
		String progress = String(scanProgress) + " songs found";
		canvas->drawString(progress, screen.getWidth()/2, 70);
		String fileCount = String(totalFiles) + " files checked";
		canvas->drawString(fileCount, screen.getWidth()/2, 90);
		canvas->setTextDatum(TL_DATUM);
		return;
	}

	if(!insertedSD){
		canvas->drawString("Not inserted!", screen.getWidth()/2, 65);
		canvas->setTextDatum(TL_DATUM);
	}else if(empty){
		String debugMsg = String(totalFiles) + " files scanned";
		canvas->drawString("No AAC files!", screen.getWidth()/2, 55);
		canvas->drawString(debugMsg, screen.getWidth()/2, 75);
		canvas->setTextDatum(TL_DATUM);
	}
}

void SongList::SongList::buildUI(){
	scrollLayout->setWHType(PARENT, FIXED);
	scrollLayout->setHeight(110);
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

bool SongList::SongList::indexExists(){
	File indexFile = SD.open(indexFileName);
	bool exists = indexFile && !indexFile.isDirectory();
	if(indexFile) indexFile.close();
	return exists;
}

bool SongList::SongList::indexNeedsUpdate(){
	if(!indexExists()) return true;
	
	File indexFile = SD.open(indexFileName);
	if(!indexFile) return true;
	
	time_t indexTime = indexFile.getLastWrite();
	indexFile.close();
	
	File root = SD.open("/");
	if(!root) return true;
	
	time_t rootTime = root.getLastWrite();
	root.close();
	
	return rootTime > indexTime;
}

bool SongList::SongList::loadSongIndex(){
	if(indexNeedsUpdate()){
		return false;
	}
	
	File indexFile = SD.open(indexFileName, FILE_READ);
	if(!indexFile){
		return false;
	}
	
	String line;
	while(indexFile.available()){
		line = indexFile.readStringUntil('\n');
		line.trim();
		if(line.length() > 0){
			songs.push_back(new ListItem(list, line));
			list->addChild(songs.back());
		}
	}
	indexFile.close();
	
	return !songs.empty();
}

void SongList::SongList::createSongIndex(){
	File indexFile = SD.open(indexFileName, FILE_WRITE);
	if(!indexFile){
		Serial.println("Failed to create song index file");
		return;
	}
	
	for(auto song : songs){
		indexFile.println(song->getPath());
	}
	indexFile.close();
	Serial.printf("Created song index with %d songs\n", songs.size());
}

void SongList::SongList::encTwoTop(){
	Serial.println("=== DUAL ENCODER MENU ACTIVATED (SongList) ===");
	Serial.println("Switching to main menu for mode selection...");
	
	// Go to main menu for mode selection (Playback/DJ/Settings)
	delete parent;
	stop();
	delete this;
	MainMenu::MainMenu::getInstance()->unpack();
	MainMenu::MainMenu::getInstance()->start();
}

void SongList::SongList::forceScanSD(){
	Serial.println("=== forceScanSD() called ===");
	if(!SD.begin(22, SPI)){
		Serial.println("SD card initialization failed");
		return;
	}
	
	// Delete existing index to force rescan
	if(SD.exists(indexFileName)){
		SD.remove(indexFileName);
		Serial.println("Removed existing song index to force rescan");
	} else {
		Serial.println("No existing index found to remove");
	}
	
	Serial.println("SD card will be rescanned on next SongList access");
}
