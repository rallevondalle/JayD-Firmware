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

	instance = this;
	buildUI();
	MixScreen::pack();
}

MixScreen::MixScreen::~MixScreen(){
	instance = nullptr;
	free(selectedBackgroundBuffer);
}

void MixScreen::MixScreen::pack(){
	Context::pack();
	free(selectedBackgroundBuffer);
	selectedBackgroundBuffer = nullptr;
}

void MixScreen::MixScreen::unpack(){
	Context::unpack();

	// Clear any existing buffer first
	if(selectedBackgroundBuffer != nullptr){
		free(selectedBackgroundBuffer);
		selectedBackgroundBuffer = nullptr;
	}

	selectedBackgroundBuffer = static_cast<Color*>(ps_malloc(79 * 128 * 2));
	if(selectedBackgroundBuffer == nullptr){
		Serial.println("ERROR: Selected background malloc failed");
		Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());
		return;
	}
	
	// Initialize buffer to prevent garbage data
	memset(selectedBackgroundBuffer, 0, 79 * 128 * 2);

	// Try to open the background file
	fs::File spiffsFile = SPIFFS.open("/mixSelectedBg.raw.hs");
	if(!spiffsFile){
		Serial.println("ERROR: Failed to open SPIFFS background file");
		// Use a fallback - fill with a solid color instead of crashing
		for(int i = 0; i < 79 * 128; i++){
			selectedBackgroundBuffer[i] = C_RGB(50, 50, 50); // Dark gray fallback
		}
		Serial.println("Using fallback background color");
		return;
	}
	
	fs::File bgFile = CompressedFile::open(spiffsFile, 13, 12);
	if(!bgFile){
		Serial.println("ERROR: Failed to open compressed background file");
		spiffsFile.close();
		// Use fallback color
		for(int i = 0; i < 79 * 128; i++){
			selectedBackgroundBuffer[i] = C_RGB(50, 50, 50); // Dark gray fallback
		}
		Serial.println("Using fallback background color");
		return;
	}
	
	size_t bytesRead = bgFile.read(reinterpret_cast<uint8_t*>(selectedBackgroundBuffer), 79 * 128 * 2);
	bgFile.close();
	
	if(bytesRead != 79 * 128 * 2){
		Serial.printf("WARNING: Expected %d bytes, read %d bytes from background file\n", 79 * 128 * 2, bytesRead);
		if(bytesRead == 0){
			// Complete failure - use fallback
			for(int i = 0; i < 79 * 128; i++){
				selectedBackgroundBuffer[i] = C_RGB(50, 50, 50); // Dark gray fallback
			}
			Serial.println("Using fallback background color due to read failure");
		}
	}else{
		Serial.println("Background buffer loaded successfully");
	}
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
	Serial.println("\n=== RETURNED METHOD START ===");
	
	String* filename = (String*) data;
	if(!filename){
		Serial.println("ERROR: Null filename");
		return;
	}

	Serial.printf("Selected file: %s\n", filename->c_str());
	Serial.printf("isLoadingTrack: %s, loadingDeck: %d\n", isLoadingTrack ? "true" : "false", loadingDeck);
	Serial.printf("Current state - f1: %s, f2: %s, system: %p\n", 
		f1 ? "loaded" : "null", f2 ? "loaded" : "null", system);

	if(doneRecording){
		saveFilename = String("/") + *filename + ".aac";
		delete filename;
		return;
	}

	if(filename->length() == 0){
		Serial.println("ERROR: Empty filename");
		delete filename;
		return;
	}
	
	if(hotSwapInProgress){
		Serial.println("ERROR: Hot-swap in progress");
		delete filename;
		return;
	}

	fs::File newFile = SD.open(*filename);
	if(!newFile){
		Serial.printf("ERROR: Failed to open file: %s\n", filename->c_str());
		delete filename;
		return;
	}
	
	if(newFile.size() == 0){
		Serial.println("ERROR: File is empty");
		newFile.close();
		delete filename;
		return;
	}
	
	Serial.printf("File opened successfully: %s (size: %d)\n", newFile.name(), newFile.size());

	if(isLoadingTrack){
		Serial.printf("HOT-SWAP: Loading to deck %d\n", loadingDeck);
		// Hot-swap the track on the specified deck
		hotSwapTrack(loadingDeck, newFile);
		isLoadingTrack = false;
	}else{
		Serial.println("INITIAL LOADING: Assigning to available deck");
		
		// Add extra validation to prevent crashes
		if(!newFile.available() || newFile.size() == 0){
			Serial.println("ERROR: File not available or empty");
			newFile.close();
			return;
		}
		
		if(!f1){
			Serial.println("Assigning to f1 (player 1)");
			f1 = newFile;
			if(f1 && f1.size() > 0){
				f1.seek(0);
				String name = f1.name();
				String songName = name.substring(name.lastIndexOf('/') + 1, name.length() - 4);
				leftSongName->setSongName(songName);
				Serial.printf("f1 assigned: %s\n", songName.c_str());
			}
			keepAudioOnStop = false;
		}else if(!f2){
			Serial.println("Assigning to f2 (player 2)");
			
			// POWER MANAGEMENT: If audio is playing, pause it during file operations
			// This prevents power spikes that cause brownout resets
			bool wasChannelPlaying = false;
			if(system){
				Serial.println("Pausing audio to prevent power spike during f2 loading");
				wasChannelPlaying = !system->isChannelPaused(0);
				if(wasChannelPlaying){
					system->pauseChannel(0);
					delay(50); // Allow audio system to settle
				}
			}
			
			f2 = newFile;
			if(f2 && f2.size() > 0){
				f2.seek(0);
				String name = f2.name();
				String songName = name.substring(name.lastIndexOf('/') + 1, name.length() - 4);
				rightSongName->setSongName(songName);
				Serial.printf("f2 assigned: %s\n", songName.c_str());
				
				// Update UI immediately
				drawQueued = true;
				
				// CRITICAL: If MixSystem exists, we need to update it to know about f2
				Serial.printf("System exists: %s\n", system ? "YES" : "NO");
				if(system){
					Serial.println("WARNING: MixSystem exists but doesn't know about new f2!");
					Serial.println("Deferring MixSystem update to prevent power issues");
				}
			}
			
			// Resume audio if it was playing
			if(system && wasChannelPlaying){
				Serial.println("Resuming audio after f2 loading");
				delay(50); // Small delay before resuming
				system->resumeChannel(0);
			}
			
			keepAudioOnStop = false;
		}else{
			Serial.println("Both decks full - closing file");
			newFile.close();
			keepAudioOnStop = false;
		}
	}

	delete filename;
	Serial.println("=== RETURNED METHOD END ===\n");
}

void MixScreen::MixScreen::setBigVuStarted(bool bigVuStarted){
	MixScreen::bigVuStarted = bigVuStarted;
}

void MixScreen::MixScreen::start(){
	Serial.println("\n=== MIXSCREEN START ===");
	Serial.printf("f1: %s, f2: %s, system: %p\n", 
		f1 ? "loaded" : "null", f2 ? "loaded" : "null", system);
	
	if(doneRecording){
		lastDraw = 0;
		draw();
		screen.commit();
		saveRecording();
	}

	// Initialize DJ mode even if we don't have both tracks loaded yet
	// This allows the mixer interface to be ready for loading tracks individually
	if(!f1 && !f2){
		Serial.println("No tracks loaded - showing song selection");
		// No tracks loaded - go to song selection for the first track
		draw();
		screen.commit();
		delay(100); // Small delay to ensure display is stable
		
		(new SongList::SongList(*getScreen().getDisplay()))->push(this);
		return;
	}

	// Handle single file initialization
	if(f1 && !f2){
		f1.seek(0);
		String name = f1.name();
		leftSongName->setSongName(name.substring(name.lastIndexOf('/') + 1, name.length() - 4));
		rightSongName->setSongName("No track loaded");
	}else if(!f1 && f2){
		f2.seek(0);
		String name = f2.name();
		rightSongName->setSongName(name.substring(name.lastIndexOf('/') + 1, name.length() - 4));
		leftSongName->setSongName("No track loaded");
	}else{
		f1.seek(0);
		f2.seek(0);
		String name = f1.name();
		leftSongName->setSongName(name.substring(name.lastIndexOf('/') + 1, name.length() - 4));
		name = f2.name();
		rightSongName->setSongName(name.substring(name.lastIndexOf('/') + 1, name.length() - 4));
	}

	// Create MixSystem if we have at least one track
	// Add debugging to trace what's happening
	Serial.printf("=== MixSystem Creation Debug ===\n");
	Serial.printf("Current system: %p\n", system);
	Serial.printf("f1 valid: %s (size: %d)\n", (f1 && f1.size() > 0) ? "YES" : "NO", f1 ? f1.size() : 0);
	Serial.printf("f2 valid: %s (size: %d)\n", (f2 && f2.size() > 0) ? "YES" : "NO", f2 ? f2.size() : 0);
	
	// SAFETY: Don't create system if one already exists
	if(system){
		Serial.println("WARNING: MixSystem already exists - skipping creation");
	}else if(f1 || f2){
		Serial.println("Creating MixSystem...");
		
		// POWER MANAGEMENT: Add delay before MixSystem creation
		// to allow power supply to stabilize
		delay(100);
		
		system = new MixSystem(f1, f2);
		Serial.printf("MixSystem created: %p\n", system);
		
		// Another delay after creation to prevent immediate power spikes
		delay(50);
	}else{
		Serial.println("No files - skipping MixSystem creation");
		system = nullptr;
	}

	if(system){
		Serial.println("Configuring MixSystem with power management...");
		
		// Spread out power-hungry operations with delays
		system->setVolume(0, InputJayD::getInstance()->getPotValue(POT_L));
		delay(10);
		system->setVolume(1, InputJayD::getInstance()->getPotValue(POT_R));
		delay(10);

		system->setChannelInfo(0, leftVu.getInfoGenerator());
		delay(10);
		system->setChannelInfo(1, rightVu.getInfoGenerator());
		delay(10);
		system->setChannelInfo(2, midVu.getInfoGenerator());
		delay(10);
		
		if(bigVuStarted){
			startBigVu();
			delay(10);
		}

		uint8_t potMidVal = InputJayD::getInstance()->getPotValue(POT_MID);
		system->setMix(potMidVal);
		delay(10);
		matrixManager.fillMatrixMid(potMidVal);
		matrixManager.matrixMid.push();
		delay(10);

		// Set durations and playing states based on available tracks
		if(f1){
			leftSeekBar->setTotalDuration(system->getDuration(0));
			leftSeekBar->setPlaying(false); // Start paused - user controls playback
		}else{
			leftSeekBar->setTotalDuration(0);
			leftSeekBar->setPlaying(false);
		}
		
		if(f2){
			rightSeekBar->setTotalDuration(system->getDuration(1));
			rightSeekBar->setPlaying(false); // Start paused - user controls playback
		}else{
			rightSeekBar->setTotalDuration(0);
			rightSeekBar->setPlaying(false);
		}
	}else{
		// No system yet - set empty states
		leftSeekBar->setTotalDuration(0);
		rightSeekBar->setTotalDuration(0);
		leftSeekBar->setPlaying(false);
		rightSeekBar->setPlaying(false);
	}


	leftSongName->checkScrollUpdate();
	rightSongName->checkScrollUpdate();

	for(int i = 0; i < 6; i++){
		effectElements[i]->setType(NONE);
		effectElements[i]->setIntensity(0);
	}

	/*system->setChannelDoneCallback(0, [](){
		instance->system->resumeChannel(0);
	});
	system->setChannelDoneCallback(1, [](){
		instance->system->resumeChannel(1);
	});*/


	if(system){
		Serial.println("Starting MixSystem with power management...");
		
		// POWER MANAGEMENT: Delay before starting to prevent power spike
		delay(50);
		system->start();
		
		// Allow system to stabilize after start
		delay(50);
		
		// Ensure both channels start paused - user controls when to play
		if(f1){
			system->pauseChannel(0);
			delay(10);
		}
		if(f2){
			system->pauseChannel(1);
			delay(10);
		}
		
		Serial.println("MixSystem started and channels paused");
	}

	// Add listeners - handle the case where VU listeners might still be active
	if(!listenersActive){
		// Full initialization - add all listeners
		LoopManager::addListener(&leftVu);
		LoopManager::addListener(&rightVu);
		LoopManager::addListener(this);
		Input.addListener(this);
		InputJayD::getInstance()->addListener(this);
		
		listenersActive = true;
	}else{
		// VU listeners might still be active from keepAudioOnStop
		// Just add the UI listeners we need
		LoopManager::addListener(this);
		Input.addListener(this);
		InputJayD::getInstance()->addListener(this);
	}

	draw();
	screen.commit();
	
	Serial.printf("=== MIXSCREEN START COMPLETE - System: %p ===\n\n", system);
}

void MixScreen::MixScreen::stop(){
	Serial.printf("=== MIXSCREEN STOP - System: %p ===\n", system);
	
	// Remove UI listeners but keep VU listeners if preserving audio
	if(!keepAudioOnStop){
		LoopManager::removeListener(&leftVu);
		LoopManager::removeListener(&rightVu);
		LoopManager::removeListener(&midVu);
		listenersActive = false;
	}
	
	LoopManager::removeListener(this);
	Input.removeListener(this);
	InputJayD::getInstance()->removeListener(this);

	if(bigVuStarted){
		stopBigVu();
	}else{
		if(!matrixManager.matrixBig.getAnimations().empty()){
			delete *matrixManager.matrixBig.getAnimations().begin();
		}
	}

	// SMART AUDIO PRESERVATION: Only delete MixSystem when not preserving audio
	if(system && !keepAudioOnStop){
		Serial.printf("Stopping and deleting MixSystem: %p (audio not preserved)\n", system);
		system->stop();
		delete system;
		system = nullptr;
		listenersActive = false;
		Serial.println("MixSystem deleted successfully");
	}else if(system && keepAudioOnStop){
		Serial.printf("Preserving MixSystem: %p (keeping audio alive)\n", system);
		// Keep system running but remove UI listeners only
	}else{
		Serial.println("No MixSystem to delete");
	}
}

void MixScreen::MixScreen::draw(){
	screen.getSprite()->fillRect(79, 0, 2, 128, TFT_BLACK);
	screen.getSprite()->fillRect(leftLayout->getTotalX(), leftLayout->getTotalY(), 79, 128, C_RGB(249, 93, 2));
	screen.getSprite()->fillRect(rightLayout->getTotalX(), rightLayout->getTotalY(), 79, 128, C_RGB(3, 52, 135));
	
	// Draw selection indication
	if(selectedBackgroundBuffer != nullptr){
		// Use the background buffer if available
		if(!selectedChannel){
			screen.getSprite()->drawIcon(selectedBackgroundBuffer, screen.getTotalX(), screen.getTotalY(), 79, 128, 1, TFT_TRANSPARENT);
		}else{
			screen.getSprite()->drawIcon(selectedBackgroundBuffer, screen.getTotalX() + 81, screen.getTotalY(), 79, 128, 1, TFT_TRANSPARENT);
		}
	}else{
		// Fallback: draw a simple border to show selection
		if(!selectedChannel){
			// Left channel selected - draw white border on left
			screen.getSprite()->drawRect(leftLayout->getTotalX(), leftLayout->getTotalY(), 79, 128, TFT_WHITE);
			screen.getSprite()->drawRect(leftLayout->getTotalX()+1, leftLayout->getTotalY()+1, 77, 126, TFT_WHITE);
		}else{
			// Right channel selected - draw white border on right
			screen.getSprite()->drawRect(rightLayout->getTotalX(), rightLayout->getTotalY(), 79, 128, TFT_WHITE);
			screen.getSprite()->drawRect(rightLayout->getTotalX()+1, rightLayout->getTotalY()+1, 77, 126, TFT_WHITE);
		}
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
	if(system && seekTime != 0 && millis() - seekTime >= 100){
		SongSeekBar* bar = seekChannel ? rightSeekBar : leftSeekBar;

		system->seekChannel(seekChannel, bar->getCurrentDuration());

		if(wasRunning){
			system->resumeChannel(seekChannel);
		}

		seekChannel = -1;
		seekTime = 0;
	}

	bool update = false;
	for(const auto& element : effectElements){
		update |= element->needsUpdate();
	}

	// Only update seek bars for channels with valid tracks
	if(system && f1 && f1.size() > 0 && system->getDuration(0) != leftSeekBar->getTotalDuration()){
		leftSeekBar->setTotalDuration(system->getDuration(0));
		update = true;
	}

	if(system && f2 && f2.size() > 0 && system->getDuration(1) != rightSeekBar->getTotalDuration()){
		rightSeekBar->setTotalDuration(system->getDuration(1));
		update = true;
	}

	if(system && f1 && f1.size() > 0 && system->getElapsed(0) != leftSeekBar->getCurrentDuration()){
		if(seekTime == 0 || seekChannel != 0){
			leftSeekBar->setCurrentDuration(system->getElapsed(0));
			update = true;
		}
	}

	if(system && f2 && f2.size() > 0 && system->getElapsed(1) != rightSeekBar->getCurrentDuration()){
		if(seekTime == 0 || seekChannel != 1){
			rightSeekBar->setCurrentDuration(system->getElapsed(1));
			update = true;
		}
	}

	if(system && system->isRecording() != isRecording){
		isRecording = system->isRecording();
		update = true;
	}

	if(system && f1 && f1.size() > 0 && system->isChannelPaused(0) != !leftSeekBar->isPlaying() && seekTime == 0){
		leftSeekBar->setPlaying(!system->isChannelPaused(0));
		update = true;
	}

	if(system && f2 && f2.size() > 0 && system->isChannelPaused(1) != !rightSeekBar->isPlaying() && seekTime == 0){
		rightSeekBar->setPlaying(!system->isChannelPaused(1)); // Fixed: was incorrectly using channel 0
		update = true;
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
	if(!system) return;
	
	if(id == POT_MID){
		system->setMix(value);
		matrixManager.fillMatrixMid(value);
		matrixManager.matrixMid.push();
	}else if(id == POT_L){
		system->setVolume(0, value);
	}else if(id == POT_R){
		system->setVolume(1, value);
	}
}

void MixScreen::MixScreen::startBigVu(){
	LoopManager::addListener(&midVu);
}

void MixScreen::MixScreen::stopBigVu(){
	LoopManager::removeListener(&midVu);
}

void MixScreen::MixScreen::encTwoBot(){
	if(system->isRecording()){
		system->stopRecording();
		doneRecording = true;

		(new TextInputScreen::TextInputScreen(*screen.getDisplay()))->push(this);
	}else{
		system->startRecording();
	}
}

void MixScreen::MixScreen::encTwoTop(){
	pop();
}

void MixScreen::MixScreen::btnCombination(){
	stop();

	MatrixPopUpPicker* popUpPicker = new MatrixPopUpPicker(*this);
	popUpPicker->unpack();
	popUpPicker->start();
}

void MixScreen::MixScreen::btn(uint8_t i){
	Serial.printf("\n=== BTN PRESSED: %d ===\n", i);
	Serial.printf("System pointer: %p\n", system);
	Serial.printf("f1 valid: %s, f2 valid: %s\n", 
		(f1 && f1.size() > 0) ? "YES" : "NO",
		(f2 && f2.size() > 0) ? "YES" : "NO");
	
	// SAFETY: Extra validation of system pointer
	if(!system || system == nullptr) {
		Serial.println("No system - cannot play/pause");
		return;
	}
	
	// Check if the channel has a valid track
	if((i == 0 && (!f1 || f1.size() == 0)) || 
	   (i == 1 && (!f2 || f2.size() == 0))){
		Serial.printf("Channel %d has no valid track\n", i);
		return;
	}
	
	SongSeekBar* bar = i == 0 ? leftSeekBar : rightSeekBar;
	if(!bar){
		Serial.printf("ERROR: No seek bar for channel %d\n", i);
		return;
	}
	
	bool wasPlaying = bar->isPlaying();
	
	Serial.printf("Channel %d was %s, attempting to %s\n", 
		i, wasPlaying ? "playing" : "paused", wasPlaying ? "pause" : "resume");
	
	// SAFETY: Double-check system pointer before calling methods
	if(system && system != nullptr){
		if(wasPlaying){
			system->pauseChannel(i);
		}else{
			system->resumeChannel(i);
		}
		
		bar->setPlaying(!wasPlaying);
		Serial.printf("Channel %d now %s\n", i, bar->isPlaying() ? "playing" : "paused");
	}else{
		Serial.println("ERROR: System became null during button press!");
	}
	
	drawQueued = true;
	Serial.println("=== BTN END ===\n");
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
	if(!system) return; // No system yet - can't use encoders for audio control

	if(index == 6){
		// Check if the selected channel has a valid track
		if((selectedChannel == 0 && (!f1 || f1.size() == 0)) || 
		   (selectedChannel == 1 && (!f2 || f2.size() == 0))){
			return;
		}
		
		if(seekTime == 0){
			seekChannel = selectedChannel;
			wasRunning = !system->isChannelPaused(selectedChannel);
			system->pauseChannel(selectedChannel);
		}

		seekTime = millis();

		SongSeekBar* bar = seekChannel ? rightSeekBar : leftSeekBar;
		uint32_t maxDuration = system->getDuration(selectedChannel);
		uint16_t newSeekTime = constrain(bar->getCurrentDuration() + value, 0, maxDuration);
		bar->setCurrentDuration(newSeekTime);

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

		if(element->getType() == EffectType::SPEED){
			system->removeSpeed(index >= 3);
		}

		EffectType type = static_cast<EffectType>(e);
		element->setType(type);
		element->setIntensity(0);

		if(type == EffectType::SPEED){
			system->addSpeed(index >= 3);
			element->setIntensity(255 / 2);
			return;
		}

		system->setEffect(index >= 3, index < 3 ? index : index - 3, type);
	}else{
		EffectType type = element->getType();
		if(type == EffectType::NONE) return;

		int16_t intensity = element->getIntensity() + value * 5;
		intensity = max((int16_t) 0, intensity);
		intensity = min((int16_t) 255, intensity);

		element->setIntensity(intensity);

		if(type == EffectType::SPEED){
			system->setSpeed(index >= 3, intensity);
		}else{
			system->setEffectIntensity(index >= 3, index < 3 ? index : index - 3, element->getIntensity());
		}
	}

	drawQueued = true;
}

void MixScreen::MixScreen::hotSwapTrack(uint8_t deck, fs::File newFile){
	Serial.printf("\n=== HOT-SWAP START: deck %d ===\n", deck);
	Serial.printf("hotSwapInProgress: %s\n", hotSwapInProgress ? "true" : "false");
	Serial.printf("Current system: %p\n", system);
	
	if(hotSwapInProgress){
		Serial.println("ERROR: Hot-swap already in progress");
		newFile.close();
		return;
	}
	hotSwapInProgress = true;
	
	// Validate the new file first
	if(!newFile || newFile.size() == 0){
		Serial.println("ERROR: Invalid file for hot-swap");
		if(newFile) newFile.close();
		hotSwapInProgress = false;
		return;
	}
	
	Serial.printf("Hot-swapping file: %s (size: %d)\n", newFile.name(), newFile.size());
	
	// Close the old file and assign the new one
	if(deck == 0){
		Serial.println("Hot-swapping f1 (player 1)");
		if(f1) {
			Serial.printf("Closing old f1: %s\n", f1.name());
			f1.close();
		}
		f1 = newFile;
		f1.seek(0);
		String name = f1.name();
		String songName = name.substring(name.lastIndexOf('/') + 1, name.length() - 4);
		leftSongName->setSongName(songName);
		Serial.printf("f1 hot-swapped to: %s\n", songName.c_str());
	}else{
		Serial.println("Hot-swapping f2 (player 2)");
		if(f2) {
			Serial.printf("Closing old f2: %s\n", f2.name());
			f2.close();
		}
		f2 = newFile;
		f2.seek(0);
		String name = f2.name();
		String songName = name.substring(name.lastIndexOf('/') + 1, name.length() - 4);
		rightSongName->setSongName(songName);
		Serial.printf("f2 hot-swapped to: %s\n", songName.c_str());
	}
	
	// Show the track name change immediately  
	drawQueued = true;
	
	hotSwapInProgress = false;
	
	// SMART HOT-SWAP: Update MixSystem without full context restart
	// This preserves audio playback during track loading
	Serial.println("Updating MixSystem to include new track...");
	
	if(system){
		Serial.printf("Recreating MixSystem with updated files (old: %p)\n", system);
		
		// CRITICAL: Store playback states before recreating system
		bool channel0WasPlaying = false;
		bool channel1WasPlaying = false;
		uint32_t channel0Position = 0;
		uint32_t channel1Position = 0;
		
		if(f1 && f1.size() > 0){
			channel0WasPlaying = !system->isChannelPaused(0);
			channel0Position = system->getElapsed(0);
			Serial.printf("Channel 0 state: %s at position %d\n", 
				channel0WasPlaying ? "PLAYING" : "PAUSED", channel0Position);
		}
		
		if(f2 && f2.size() > 0){
			channel1WasPlaying = !system->isChannelPaused(1);
			channel1Position = system->getElapsed(1);
			Serial.printf("Channel 1 state: %s at position %d\n", 
				channel1WasPlaying ? "PLAYING" : "PAUSED", channel1Position);
		}
		
		// Store current mixer settings
		uint8_t leftVol = InputJayD::getInstance()->getPotValue(POT_L);
		uint8_t rightVol = InputJayD::getInstance()->getPotValue(POT_R);
		uint8_t mixVal = InputJayD::getInstance()->getPotValue(POT_MID);
		
		// Stop old system
		system->stop();
		delete system;
		
		// Create new system with both files
		system = new MixSystem(f1, f2);
		
		// Restore settings
		system->setVolume(0, leftVol);
		system->setVolume(1, rightVol);
		system->setMix(mixVal);
		
		// Restore VU meter connections
		system->setChannelInfo(0, leftVu.getInfoGenerator());
		system->setChannelInfo(1, rightVu.getInfoGenerator());
		system->setChannelInfo(2, midVu.getInfoGenerator());
		
		// Update seek bars
		if(f1){
			leftSeekBar->setTotalDuration(system->getDuration(0));
			leftSeekBar->setCurrentDuration(0);  // New tracks start at beginning
			leftSeekBar->setPlaying(false);      // Will be updated below
		}
		if(f2){
			rightSeekBar->setTotalDuration(system->getDuration(1));
			rightSeekBar->setCurrentDuration(0);  // New tracks start at beginning
			rightSeekBar->setPlaying(false);      // Will be updated below
		}
		
		// Start new system
		system->start();
		
		// Add small delay to let system stabilize before state restoration
		delay(100);
		
		// SMART PLAYBACK RESTORATION: Only the non-selected channel keeps playing
		// The channel getting the new track starts paused (like professional CDJs)
		
		if(deck == 0){
			// Loading new track to channel 0 (f1) - it starts paused
			Serial.println("New track on channel 0 - starting paused");
			system->pauseChannel(0);
			leftSeekBar->setPlaying(false);
			
			// Channel 1 (f2) keeps its previous state
			if(f2 && f2.size() > 0){
				if(channel1WasPlaying){
					Serial.println("Restoring channel 1 to PLAYING state");
					// Seek to previous position and resume
					system->seekChannel(1, channel1Position);
					delay(50); // Allow seek to complete
					system->resumeChannel(1);
					rightSeekBar->setPlaying(true);
					rightSeekBar->setCurrentDuration(channel1Position);
					Serial.printf("Channel 1 restored: PLAYING at %d\n", channel1Position);
				}else{
					Serial.println("Keeping channel 1 PAUSED");
					system->pauseChannel(1);
					rightSeekBar->setPlaying(false);
				}
			}
		}else{
			// Loading new track to channel 1 (f2) - it starts paused
			Serial.println("New track on channel 1 - starting paused");
			system->pauseChannel(1);
			rightSeekBar->setPlaying(false);
			
			// Channel 0 (f1) keeps its previous state
			if(f1 && f1.size() > 0){
				if(channel0WasPlaying){
					Serial.println("Restoring channel 0 to PLAYING state");
					// Seek to previous position and resume
					system->seekChannel(0, channel0Position);
					delay(50); // Allow seek to complete
					system->resumeChannel(0);
					leftSeekBar->setPlaying(true);
					leftSeekBar->setCurrentDuration(channel0Position);
					Serial.printf("Channel 0 restored: PLAYING at %d\n", channel0Position);
				}else{
					Serial.println("Keeping channel 0 PAUSED");
					system->pauseChannel(0);
					leftSeekBar->setPlaying(false);
				}
			}
		}
		
		Serial.printf("MixSystem updated successfully (new: %p)\n", system);
	}
	
	// Reset audio preservation flag
	keepAudioOnStop = false;
	
	Serial.println("=== HOT-SWAP COMPLETE ===");
}

void MixScreen::MixScreen::encBtnHold(uint8_t i){
	if(i == 6){
		Serial.printf("=== HOLD BUTTON 6: Loading track for deck %d ===\n", selectedChannel);
		
		// Store which deck we're loading for
		isLoadingTrack = true;
		loadingDeck = selectedChannel;
		keepAudioOnStop = true; // Keep audio playing when SongList is shown
		
		Serial.printf("Set keepAudioOnStop = true, loadingDeck = %d\n", loadingDeck);
		
		(new SongList::SongList(*getScreen().getDisplay()))->push(this);
		return;
	}
}
