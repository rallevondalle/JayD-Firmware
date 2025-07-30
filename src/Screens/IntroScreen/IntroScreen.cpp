#include "IntroScreen.h"
#include <FS.h>
#include <Loop/LoopManager.h>
#include <FS/CompressedFile.h>
#include "../MainMenu/MainMenu.h"
#include "../MixScreen/MixScreen.h"
#include <SPIFFS.h>
#include <AudioLib/Systems/PlaybackSystem.h>
#include <Settings.h>
#include <JayD.h>


IntroScreen::IntroScreen *IntroScreen::IntroScreen::instance = nullptr;


IntroScreen::IntroScreen::IntroScreen(Display &display) : Context(display){
	Serial.println("\n=== INTROSCREEN CONSTRUCTOR ===");
	Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
	
	instance = this;

	fs::File f = SPIFFS.open("/intro.g565.hs");
	if(!f){
		Serial.println("Error opening intro gif");
		return;
	}

	gif = new AnimatedSprite(screen.getSprite(), CompressedFile::open(f, 9, 8, 33734));
	gif->setSwapBytes(false);
	gif->setXY(0, 0);

	IntroScreen::pack();
}

IntroScreen::IntroScreen::~IntroScreen(){
	delete gif;
	instance = nullptr;
}

void IntroScreen::IntroScreen::draw(){
	gif->nextFrame();
	gif->push();
}

void IntroScreen::IntroScreen::start(){
	if(!gif) return;

	gif->setLoopDoneCallback([]{
		if(instance == nullptr) return;

		Serial.println("\n=== INTRO COMPLETE - LAUNCHING MIXSCREEN ===");
		Serial.printf("Free heap before MixScreen: %u bytes\n", ESP.getFreeHeap());

		Display& display = *instance->getScreen().getDisplay();

		instance->stop();
		delete instance;

		Serial.println("Creating MixScreen...");
		MixScreen::MixScreen* mixScreen = new MixScreen::MixScreen(display);
		Serial.printf("MixScreen created: %p\n", mixScreen);
		
		mixScreen->unpack();
		Serial.println("MixScreen unpacked, starting...");
		mixScreen->start();
		
		Serial.println("=== MIXSCREEN LAUNCHED ===\n");
	});

	LoopManager::addListener(this);
	matrixManager.startRandom();

	draw();
	screen.commit();
}

void IntroScreen::IntroScreen::stop(){
	LoopManager::removeListener(this);
}

void IntroScreen::IntroScreen::loop(uint micros){
	if(!gif || !gif->checkFrame()) return;

	draw();
	screen.commit();
}


