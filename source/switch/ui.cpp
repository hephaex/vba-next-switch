#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <vector>

#include <switch.h>

extern "C" {
#include "ini/ini.h"
}

#include "draw.h"
#include "image.h"

#include "../system.h"
#include "../types.h"
#include "ui.h"
#include "util.h"

struct Setting {
	const char* name;
	u32 valuesCount, *valueIdx;
	const char** strValues;
	char generatedString[256];
	bool meta;
};

#define FILENAMEBUFFER_SIZE (1024 * 32)  // 32kb
#define FILENAMES_COUNT_MAX 2048

#define SETTINGS_MAX (128)

static char* filenameBuffer = NULL;
static char* filenames[FILENAMES_COUNT_MAX];
static int filenamesCount = 0;

static char statusMessage[FILENAMES_COUNT_MAX];
static int statusMessageFadeout = 0;

static char selectedPath[PATH_LENGTH] = {'\0'};
static char currentDirectory[PATH_LENGTH] = {'\0'};
static int cursor = 0;
static int scroll = 0;

static const char* pauseMenuItems[] = {"Continue", "Load Savestate", "Write Savestate", "Exit"};

static Setting* settings;
static int settingsMetaStart = 0;
static int settingsCount = 0;
static char* settingStrings[SETTINGS_MAX];
static bool settingsChanged = false;

#define UI_STATESTACK_MAX 4
static UIState uiStateStack[UI_STATESTACK_MAX];
static int uiStateStackCount = 0;

static Image magicarp;

static Image gbaImage;

static const char* settingsPath = "vba-switch.ini";

static void generateSettingString(int idx) {
	Setting* setting = &settings[idx];
	if (!setting->meta) {
		snprintf(setting->generatedString, sizeof(setting->generatedString) - 1, "%s: %s", setting->name,
			 setting->strValues[*setting->valueIdx]);
	} else {
		strcpy_safe(setting->generatedString, setting->name, sizeof(setting->generatedString));
	}
}

void uiStatusMsg(const char* format, ...) {
	statusMessageFadeout = 500;
	va_list args;
	va_start(args, format);
	vsnprintf(statusMessage, sizeof(statusMessage) / sizeof(char), format, args);
	va_end(args);
}

static void enterDirectory() {
	filenamesCount = FILENAMES_COUNT_MAX;
	getDirectoryContents(filenameBuffer, &filenames[0], &filenamesCount, currentDirectory);

	cursor = 0;
	scroll = 0;
}

void uiInit() {
	filenameBuffer = (char*)malloc(FILENAMEBUFFER_SIZE);
	strcpy_safe(currentDirectory, "", PATH_LENGTH);
	enterDirectory();

	settings = (Setting*)malloc(SETTINGS_MAX * sizeof(Setting));

	imageLoad(&magicarp, "romfs:/karpador.png");
	imageLoad(&gbaImage, "romfs:/gba.png");
}

void uiDeinit() {
	imageDeinit(&magicarp);
	imageDeinit(&gbaImage);

	uiSaveSettings();

	free(filenameBuffer);
	free(settings);
}

void uiFinaliseAndLoadSettings() {
	settingsMetaStart = settingsCount;

	// uiAddSetting("Remap Buttons", NULL, result)
	uiAddSetting("Exit", NULL, resultClose, NULL, true);

	ini_t* cfg = ini_load(settingsPath);
	if (cfg) {
		for (int i = 0; i < settingsMetaStart; i++) {
			if (ini_sget(cfg, "misc", settings[i].name, "%d", settings[i].valueIdx)) generateSettingString(i);
		}

		ini_free(cfg);
	}
}

void uiSaveSettings() {
	if (settingsChanged) {
		FILE* f = fopen(settingsPath, "wt");
		if (f) {
			fprintf(f, "[Misc]\n");

			for (int i = 0; i < settingsMetaStart; i++) fprintf(f, "%s=%d\n", settings[i].name, *settings[i].valueIdx);

			fclose(f);
		}
	}
}

void uiGetSelectedFile(char* out, int outLength) { strcpy_safe(out, selectedPath, outLength); }

UIResult uiLoop(u8* fb, u32 fbWidth, u32 fbHeight, u32 keysDown) {
	UIState state = uiGetState();
	if (state == stateRemapButtons) {
		imageDraw(fb, fbWidth, fbHeight, &gbaImage, 0, 0);
	} else if (uiGetState() != stateRunning) {
		int scrollAmount = 0;
		if (keysDown & KEY_DOWN) scrollAmount = 1;
		if (keysDown & KEY_UP) scrollAmount = -1;
		if (keysDown & KEY_LEFT) scrollAmount = -5;
		if (keysDown & KEY_RIGHT) scrollAmount = 5;

		const char** menu = NULL;
		int menuItemsCount;
		if (state == stateSettings) {
			menu = (const char**)settingStrings;
			menuItemsCount = settingsCount;
		} else if (state == statePaused) {
			menu = pauseMenuItems;
			menuItemsCount = sizeof(pauseMenuItems) / sizeof(pauseMenuItems[0]);
		} else {
			menu = (const char**)filenames;
			menuItemsCount = filenamesCount;
		}

		if (scrollAmount > 0) {
			for (int i = 0; i < scrollAmount; i++) {
				if (cursor < menuItemsCount - 1) {
					cursor++;
					if (cursor - scroll >= 60) {
						scroll++;
					}
				}
			}
		} else if (scrollAmount < 0) {
			for (int i = 0; i < -scrollAmount; i++) {
				if (cursor > 0) {
					cursor--;
					if (cursor - scroll < 0) {
						scroll--;
					}
				}
			}
		}

		drawRect(0, 0, fbWidth, fbHeight, MakeColor(50, 50, 50, 255));

		int i = 0;
		int separator = 40;
		int menuHSeparator = 80;
		for (int j = scroll; j < menuItemsCount; j++) {
			u8 color = 255;
			u32 h, w;
			getTextDimensions(font16, menu[j], &w, &h);
			u32 heightOffset = (40 - h) / 2;

			if (i * separator + heightOffset + menuHSeparator > fbHeight - 85) continue;

			if (i + scroll == cursor) {
				drawRect(0, i * separator + menuHSeparator, fbWidth / 1.25, separator, MakeColor(33, 34, 39, 255));
				drawText(font16, 60, i * separator + heightOffset + menuHSeparator, MakeColor(0, 255, 197, 255), menu[j]);
			} else {
				drawText(font16, 60, i * separator + heightOffset + menuHSeparator, MakeColor(color, color, color, 255),
					 menu[j]);
			}

			i++;
			if (i >= 60) break;
		}

		u64 timestamp;
		timeGetCurrentTime(TimeType_UserSystemClock, &timestamp);
		time_t tim = (time_t)timestamp;
		struct tm* timeStruct = localtime(&tim);

		char timeBuffer[64];
		snprintf(timeBuffer, 64, "%02i:%02i", timeStruct->tm_hour + 2, timeStruct->tm_min);

		drawText(font24, fbWidth - 130, 45, MakeColor(255, 255, 255, 255), timeBuffer);

		drawRect(0, fbHeight, fbWidth, 70, MakeColor(50, 50, 50, 255));
		drawRect((u32)((fbWidth - 1215) / 2), fbHeight - 70, 1215, 1, MakeColor(255, 255, 255, 255));

		if (state == stateFileselect) drawText(font14, 60, fbHeight - 42, MakeColor(255, 255, 255, 255), currentDirectory);

		if (keysDown & KEY_X) return resultExit;

		if (keysDown & KEY_A || keysDown & KEY_B) {
			if (state == stateFileselect) {
				if (keysDown & KEY_B) cursor = 0;

				char path[PATH_LENGTH] = {'\0'};

				if (!strcmp(filenames[cursor], "..")) {
					int length = strlen(currentDirectory);
					for (int i = length - 1; i >= 0; i--) {
						if (currentDirectory[i] == '/') {
							strncpy(path, currentDirectory, i);
							path[i] = '\0';
							break;
						}
					}
				} else
					snprintf(path, PATH_LENGTH, "%s/%s", currentDirectory, filenames[cursor]);

				if (isDirectory(path)) {
					strcpy_safe(currentDirectory, path, PATH_LENGTH);
					enterDirectory();
				} else {
					strcpy_safe(selectedPath, path, PATH_LENGTH);
					return resultSelectedFile;
				}
			} else if (state == stateSettings) {
				Setting* setting = &settings[cursor];

				if (setting->meta) return (UIResult)setting->valuesCount;
				*setting->valueIdx += (keysDown & KEY_A ? 1 : -1);
				if (*setting->valueIdx == UINT32_MAX) *setting->valueIdx = setting->valuesCount - 1;
				if (*setting->valueIdx >= setting->valuesCount) *setting->valueIdx = 0;

				generateSettingString(cursor);

				settingsChanged = true;

				return resultSettingsChanged;
			} else {
				if (keysDown & KEY_B) return resultUnpause;

				switch (cursor) {
					case 0:
						return resultUnpause;
					case 1:
						return resultLoadState;
					case 2:
						return resultSaveState;
					case 3:
						return resultClose;
				}
			}
		}
	}

	if (statusMessageFadeout > 0) {
		int fadeout = statusMessageFadeout > 255 ? 255 : statusMessageFadeout;
		drawText(font14, 60, fbHeight - 20, MakeColor(255, 255, 255, fadeout), statusMessage);
		statusMessageFadeout -= 4;
	}

	// imageDraw(fb, fbWidth, fbHeight, &magicarp, fbWidth - 60, fbHeight - 60);

	return resultNone;
}

void uiPushState(UIState state) {
	if (uiStateStackCount < UI_STATESTACK_MAX)
		uiStateStack[uiStateStackCount++] = state;
	else
		printf("warning: push UI stack further than max\n");

	cursor = 0;
	scroll = 0;
}

void uiPopState() {
	if (uiStateStackCount > 0)
		uiStateStackCount--;
	else
		printf("warning: pop empty UI stack\n");

	cursor = 0;
	scroll = 0;
}

UIState uiGetState() {
	if (uiStateStackCount == 0) {
		printf("warning: uiGetState() for empty UI stack");
		return stateFileselect;
	}
	return uiStateStack[uiStateStackCount - 1];
}

void uiAddSetting(const char* name, u32* valueIdx, u32 valuesCount, const char* strValues[], bool meta) {
	settings[settingsCount].name = name;
	settings[settingsCount].valueIdx = valueIdx;
	settings[settingsCount].valuesCount = valuesCount;
	settings[settingsCount].strValues = strValues;
	settings[settingsCount].meta = meta;

	settingStrings[settingsCount] = settings[settingsCount].generatedString;

	generateSettingString(settingsCount);

	settingsCount++;
}