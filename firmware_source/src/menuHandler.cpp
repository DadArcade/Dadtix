#include "menuHandler.h"
#include "htmlParser.h"
#include "pageShowcase.h"
#include "device.h"
#include "esp_log.h"
#include "usbMassStorage.h"
#include "driver/adc.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include <algorithm>
#include <utility>
#include <sys/statvfs.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

PageShowcase pageShowcase;

MenuHandler::MenuHandler(Renderer *renderer)
{
    this->renderer = renderer;
    this->leftPageFrameBuffer = (unsigned char*)calloc(EPD_WIDTH * EPD_HEIGHT / 8,sizeof(unsigned char));
    this->rightPageFrameBuffer = (unsigned char*)calloc(EPD_WIDTH * EPD_HEIGHT / 8,sizeof(unsigned char));
    mainMenu = std::make_shared<MenuElement>(renderer, std::string("Diptyx E-reader"),std::string(""));
    authorMenu = std::make_shared<MenuElement>(renderer, std::string("Library"),std::string("Open & read books"));
    settingsMenu = std::make_shared<MenuElement>(renderer, std::string("Settings"),std::string("Edit main settings, render settings, etc."));
    deviceSettingsMenu = std::make_shared<MenuElement>(renderer, std::string("Device settings"),std::string("Edit device behaviour"));
    readSettingsMenu = std::make_shared<MenuElement>(renderer, std::string("Book settings"),std::string("Edit the reading experience"));
    einkSettingsMenu = std::make_shared<MenuElement>(renderer, std::string("E-ink settings"),std::string("Edit E-ink display behaviour"));


    auto horizontalMarginBox = std::make_shared<ValueElement>(renderer, std::string("Horizontal margin"),std::string("Distance between text & display edge"),
    std::vector<int> {0,1,2,3,4,5}, std::vector<std::string> {std::string("0em"),std::string("1em"),std::string("2em"),std::string("3em"),std::string("4em"),std::string("5em")},&(Device::getInstance().renderSettings.marginsHorizontal) );

    auto verticalMarginBox = std::make_shared<ValueElement>(renderer, std::string("Vertical margin"),std::string("Distance between text & display edge"),
    std::vector<int> {0,1,2,3,4,5}, std::vector<std::string> {std::string("0em"),std::string("1em"),std::string("2em"),std::string("3em"),std::string("4em"),std::string("5em")},&(Device::getInstance().renderSettings.marginsVertical));

    auto batteryVoltageBox = std::make_shared<ValueElement>(renderer, std::string("Battery indicator"),std::string("Show or hide battery indicator"),
    std::vector<int> {0,1}, std::vector<std::string> {std::string("Hidden"),std::string("Shown")},&(Device::getInstance().deviceSettings.displayBattery));

    auto lineSpaceBox = std::make_shared<ValueElement>(renderer, std::string("Line spacing"),std::string("additional spacing between lines"),
    std::vector<int> {0,1,2,3,4}, std::vector<std::string> {std::string("0Px"),std::string("1Px"),std::string("2Px"),std::string("3Px"),std::string("4Px")},&(Device::getInstance().renderSettings.lineSpacing));

    auto textBoldBox = std::make_shared<ValueElement>(renderer, std::string("Default font weight"),std::string("Default boldness of text in books"),
    std::vector<int> {0,1}, std::vector<std::string> {std::string("Auto"),std::string("Bold")},&(Device::getInstance().renderSettings.fontBold));

    auto showPagePercentageBox = std::make_shared<ValueElement>(renderer, std::string("Reading progress percentage/pages"),std::string("Show current page as a number or percentage"),
    std::vector<int> {1,0}, std::vector<std::string> {std::string("Percentage"),std::string("Number")},&(Device::getInstance().deviceSettings.showPagePercentage));

    auto displayRefreshBox = std::make_shared<ValueElement>(renderer, std::string("Full refresh interval"),std::string("Fully refresh displays after x pages"),
    std::vector<int> {0,1,2,3,4,5}, std::vector<std::string> {std::string("0"),std::string("1"),std::string("2"),std::string("3"),std::string("4"),std::string("5")},&(Device::getInstance().deviceSettings.displayRefresh));

    auto buzzerBox = std::make_shared<ValueElement>(renderer, std::string("Haptic buzzer"),std::string("Enable or disable buzzer"),
    std::vector<int> {1,0}, std::vector<std::string> {std::string("Enable"),std::string("Disable")},&(Device::getInstance().deviceSettings.buzzerEnabled));

    auto buzzerIntensityBox = std::make_shared<ValueElement>(renderer, std::string("Buzzer intensity"),std::string("Strength of buzzes"),
    std::vector<int> {1,2,3,4,5,6,7,8,9}, std::vector<std::string> {std::string("1"),std::string("2"),std::string("3"),std::string("4"),std::string("5"),std::string("6"),std::string("7"),std::string("8"),std::string("9")},&(Device::getInstance().deviceSettings.buzzerIntensity));

    auto nightModeBox = std::make_shared<ValueElement>(renderer, std::string("Dark mode"),std::string("Invert all colors"),
    std::vector<int> {1,0}, std::vector<std::string> {std::string("Enable"),std::string("Disable")},&(Device::getInstance().deviceSettings.nightMode));

    auto sunlightModeBox = std::make_shared<ValueElement>(renderer, std::string("Sunlight mode"),std::string("Reduce artefacts in sunlight, increases latency"),
    std::vector<int> {1,0}, std::vector<std::string> {std::string("Enable"),std::string("Disable")},&(Device::getInstance().deviceSettings.sunlightMode));

    auto standbyTimeoutBox = std::make_shared<ValueElement>(renderer, std::string("Standby timeout"),std::string("Time in minutes before entering deep sleep"),
    std::vector<int> {2,5,10,20}, std::vector<std::string> {std::string("2"),std::string("5"),std::string("10"),std::string("20")},&(Device::getInstance().deviceSettings.standbyTimeout));

    auto standbyScreenBox = std::make_shared<ValueElement>(renderer, std::string("Standby screen"),std::string("What to display during deep sleep"),
    std::vector<int> {0,1,2}, std::vector<std::string> {std::string("None"),std::string("Current book"),std::string("Custom")},&(Device::getInstance().deviceSettings.standbyScreen));

    auto storeDataOnSDBox = std::make_shared<ValueElement>(renderer, std::string("Data storage location"),std::string("Location to store book data files"),
    std::vector<int> {0,1}, std::vector<std::string> {std::string("Internal"),std::string("SD")},&(Device::getInstance().deviceSettings.storeDataOnSD));

    auto smartImageDetectBox = std::make_shared<ValueElement>(renderer, std::string("Smart image updates"),std::string("Limit full screen updates to large images"),
    std::vector<int> {0,1}, std::vector<std::string> {std::string("disabled"),std::string("enabled")},&(Device::getInstance().deviceSettings.smartImageDetect));

     auto sunlightFullRefreshBox = std::make_shared<ValueElement>(renderer, std::string("Sunlight mode full refresh"),std::string("Force full refresh in sunlight mode"),
    std::vector<int> {0,1}, std::vector<std::string> {std::string("disabled"),std::string("enabled")},&(Device::getInstance().deviceSettings.sunlightFullRefresh));

    auto standbyShutdownBox = std::make_shared<ValueElement>(renderer, std::string("Shutdown timer"),std::string("Shutdown after long standby"),
    std::vector<int> {0,1,2,3,7,14,21,28}, std::vector<std::string> {std::string("disabled"),std::string("1 day"),std::string("2 days"),std::string("3 days"),std::string("1 week"),std::string("2 weeks"),std::string("3 weeks"),std::string("4 weeks")},&(Device::getInstance().deviceSettings.standbyShutdown));

    
    //generate a list for the vcom voltages
    std::vector<int> vcomValues = {}; 
    std::vector<std::string> vcomNames = {};
    for(int i=0;i<80;i++)
    {
        vcomValues.push_back(i);
        std::string valueName = std::to_string(10000 + i*5 + 10) + "V";
        valueName[0] = 45;
        valueName[1] = valueName[2];
        valueName[2] = 46;
        vcomNames.push_back(valueName);
    }

    auto vcomLeftBox = std::make_shared<ValueElement>(renderer, std::string("Vcom voltage left"),std::string("Don't edit, refer to the web-docs"),
    vcomValues, vcomNames,&(Device::getInstance().deviceSettings.vcomLeft));

    auto vcomRightBox = std::make_shared<ValueElement>(renderer, std::string("Vcom voltage right"),std::string("Don't edit, refer to the web-docs"),
    vcomValues, vcomNames,&(Device::getInstance().deviceSettings.vcomRight));

    fontSizeBox = std::make_shared<ValueElement>(renderer, std::string("Font size"),std::string("Size of the text in books"),
    std::vector<int> {1}, std::vector<std::string> {"placeholder"},&(Device::getInstance().renderSettings.fontPoints));

    auto manualButton = std::make_shared<ActionElement>(
    renderer,
    "Device Manual",
    "Review the Diptyx manual",
    []() {
        Device::getInstance().state = Device::State::simpleReader;
        Device::getInstance().activeBookPath = "userManual.epub";
        Device::getInstance().simpleReader->init("userManual.epub",Device::getInstance().renderer);
        Device::getInstance().saveAppState();
    }
);

    auto versionButton = std::make_shared<ActionElement>(
    renderer,
    "Firmware version",
    "Review firmware version and patch notes",
    []() {
    //    Device::getInstance().state = Device::State::simpleReader;
    //     Device::getInstance().activeBookPath = "firmwareVersion.epub";
    //     Device::getInstance().simpleReader->init("firmwareVersion.epub",Device::getInstance().renderer);
    //     Device::getInstance().saveAppState();
    Device::getInstance().notificationHandler->drawNotification("Firmware version: " FIRMWARE_VERSION);
    vTaskDelay(pdMS_TO_TICKS(1000));
    Device::getInstance().menuHandler->drawMenu();
    }
);

    auto restoreBookSettingsButton = std::make_shared<ActionElement>(
    renderer,
    "Restore book settings",
    "Restore the default book settings",
    []() {
        Device::getInstance().renderSettings.fontBold = false;
        Device::getInstance().renderSettings.lineSpacing = 0;
        Device::getInstance().renderSettings.marginsVertical = 0;
        Device::getInstance().renderSettings.marginsHorizontal = 1;
        Device::getInstance().renderSettings.fontPoints = 14;
        std::string fontName = "Espy Serif";
        Device::getInstance().renderSettings.fontFamily  = 0;
        for(int i=0;i<Device::getInstance().renderer->fontHandler.families.size();i++)
        {
            if(fontName==Device::getInstance().renderer->fontHandler.families[i].name) Device::getInstance().renderSettings.fontFamily = i;

        }
        Device::getInstance().saveSettings();
        
        for(int i=0;i<Device::getInstance().menuHandler->readSettingsMenu->children.size();i++) //and update the values in the settings
        {
        if(Device::getInstance().menuHandler->readSettingsMenu->children[i]->getType()==UIElementType::Value)
        {
            std::static_pointer_cast<ValueElement>(Device::getInstance().menuHandler->readSettingsMenu->children[i])->ReadValue();
        }
    }
        Device::getInstance().menuHandler->drawMenu();
    }
);

auto fileTransferButton = std::make_shared<ActionElement>(
    renderer,
    "Transfer files",
    "Enter USB mass storage mode",
    []() {
        //stop usb cdc and unmount sd card
        tinyusb_console_deinit(TINYUSB_CDC_ACM_0);
        tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        tinyusb_driver_uninstall();
        delete(Device::getInstance().sd);
        vTaskDelay(10);
        

        if (usb_msc_sdmmc_start(GPIO_NUM_41, GPIO_NUM_40, GPIO_NUM_39, 1) == ESP_OK) {
            Device::getInstance().notificationHandler->drawManualFileTransfer(false);
            int timeOutTimer = 0;
            // Wait until USB cable disconnected (GPIO low)
            while(gpio_get_level(PAGE_RIGHT_BUTTON)) {

                if(!gpio_get_level(GPIO_NUM_16))
                {
                    timeOutTimer += 100;
                }
                if(timeOutTimer > 1000 * 60 * 10) //if the device is in filetransfer for longer than 10 minutes without a USB connection, we restart
                {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                
            }
            Device::getInstance().notificationHandler->drawManualFileTransfer(true);
            usb_msc_stop();  // stop MSC

            
        } else {
            Device::getInstance().notificationHandler->drawNotification("Error opening mass storage");
        }

        vTaskDelay(10);
        esp_restart(); //restart the device to init everything properly
    }
);


    currentElement = authorMenu;
    mainMenu->addChild(authorMenu);
    mainMenu->addChild(settingsMenu);
    mainMenu->addChild(fileTransferButton);
    settingsMenu->addChild(readSettingsMenu);
    settingsMenu->addChild(deviceSettingsMenu);
    settingsMenu->addChild(einkSettingsMenu);
    settingsMenu->addChild(manualButton);
    settingsMenu->addChild(versionButton);
    layoutFontSelect();
    updateFontSize();
    readSettingsMenu->addChild(fontSizeBox);
    readSettingsMenu->addChild(lineSpaceBox);
    readSettingsMenu->addChild(horizontalMarginBox);
    readSettingsMenu->addChild(verticalMarginBox);
    deviceSettingsMenu->addChild(batteryVoltageBox);
    readSettingsMenu->addChild(textBoldBox);
    readSettingsMenu->addChild(showPagePercentageBox);
    readSettingsMenu->addChild(restoreBookSettingsButton);
    deviceSettingsMenu->addChild(buzzerBox);
    deviceSettingsMenu->addChild(buzzerIntensityBox);
    deviceSettingsMenu->addChild(nightModeBox);
    deviceSettingsMenu->addChild(sunlightModeBox);
    deviceSettingsMenu->addChild(standbyTimeoutBox);
    deviceSettingsMenu->addChild(standbyScreenBox);
    deviceSettingsMenu->addChild(standbyShutdownBox);
    deviceSettingsMenu->addChild(storeDataOnSDBox);

    einkSettingsMenu->addChild(displayRefreshBox);
    einkSettingsMenu->addChild(smartImageDetectBox);
    einkSettingsMenu->addChild(sunlightFullRefreshBox);
    einkSettingsMenu->addChild(vcomLeftBox);
    einkSettingsMenu->addChild(vcomRightBox);
    //drawMenu();
}

void MenuHandler::upButtonAction()
{
    if (auto menu = std::static_pointer_cast<MenuElement>(currentElement)) {

        if(menu->selectedChildIndex>0)
        {
            //if(!this->buzzDisabled) Device::buzz();
            if(menu->children[menu->selectedChildIndex]->getType()==UIElementType::Value)
            {
                std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex])->selected = false;
                std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex])->ReadValue();
            }
            if(menu->selectedChildIndex% MaxElementsPerPage == 0) renderer->epd.forceRefresh();
            else renderer->epd.partialUpdatesRemaining[1] = 8;
            menu->selectedChildIndex--;
            drawMenu();
            //if(!this->buzzDisabled) Device::buzz();
        }

    }
}

void MenuHandler::downButtonAction()
{
    if (auto menu = std::static_pointer_cast<MenuElement>(currentElement)) {

        if(menu->children.size()!=0 && menu->selectedChildIndex<menu->children.size()-1)
        {
            //if(!this->buzzDisabled) Device::buzz();
            if(menu->children[menu->selectedChildIndex]->getType()==UIElementType::Value)
            {
                std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex])->selected = false;
                std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex])->ReadValue();
            }
            menu->selectedChildIndex++;
            if(menu->selectedChildIndex% MaxElementsPerPage == 0) renderer->epd.forceRefresh();
            else renderer->epd.partialUpdatesRemaining[1] = 8;
            drawMenu();
            //if(!this->buzzDisabled) Device::buzz();
        }
    }
}

void MenuHandler::middleButtonAction()
{
    if(currentElement->getType()==UIElementType::Menu || currentElement->getType()==UIElementType::Author)
    {    
        auto menu = std::static_pointer_cast<MenuElement>(currentElement);
        if(menu->children.size() > 0)
        {
            if(menu->children[menu->selectedChildIndex]->getType()==UIElementType::Menu ||
                menu->children[menu->selectedChildIndex]->getType()==UIElementType::Author)
            {        
                //if(!this->buzzDisabled) Device::buzz();
                this->currentElement = menu->children[menu->selectedChildIndex];
                renderer->epd.forceRefresh();
                drawMenu();
                //if(!this->buzzDisabled) Device::buzz();
            }
            else if(menu->children[menu->selectedChildIndex]->getType()==UIElementType::Action)
            {        
                auto actionElement = std::static_pointer_cast<ActionElement>(menu->children[menu->selectedChildIndex]);
                renderer->epd.forceRefresh();
                actionElement->trigger();
                //if(!this->buzzDisabled) Device::buzz();
            }
            else if(menu->children[menu->selectedChildIndex]->getType()==UIElementType::Book)
            {
                auto bookElement = std::static_pointer_cast<BookElement>(menu->children[menu->selectedChildIndex]);
                bookElement->book->favorite = !bookElement->book->favorite;
                //bookElement->elementExtraDescription = (bookElement->book->favorite?std::string("❤"):std::string("")); //book descriptions auto update
                Device::getInstance().bookHandler->saveBook(bookElement->book); //store the updated book
                if(!authorMenu->selectedChildIndex==0) Device::getInstance().bookHandler->refreshFavorites(); //update the favorites vector
                auto favoriteListElement = std::static_pointer_cast<AuthorElement>(authorMenu->children[0]);
                favoriteListElement->initChildren(); //and update the UI element
                favoriteListElement->elementDescription = "Books: " + std::to_string(std::static_pointer_cast<AuthorElement>(authorMenu->children[0])->author->bookList.size());
                if(favoriteListElement->selectedChildIndex>=favoriteListElement->children.size()) favoriteListElement->selectedChildIndex = favoriteListElement->children.size()-1;
                drawMenu();
            }
            else if(menu->children[menu->selectedChildIndex]->getType()==UIElementType::Value)
            {
                //if(!this->buzzDisabled) Device::buzz();
                auto valueElement = std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex]);
                if(valueElement->selected) 
                {
                    valueElement->writeValue();
                    if(valueElement->valueAdress==&(Device::getInstance().deviceSettings.storeDataOnSD))
                    {
                        if(Device::getInstance().deviceSettings.storeDataOnSD) Device::getInstance().bookHandler->transferDataToSD();
                        else Device::getInstance().bookHandler->transferDataToFlash();
                    }
                    updateFontSize();
                    updateFont();
                    Device::getInstance().saveSettings();
                    renderer->epd.forceRefresh();
                }
                valueElement->selected = !valueElement->selected;
                drawMenu();
                //if(!this->buzzDisabled) Device::buzz();
            }
        }
    }
}

void MenuHandler::rightButtonAction()
{
    auto menu = std::static_pointer_cast<MenuElement>(currentElement);
    if(menu->selectedChildIndex>=menu->children.size()) return;
    if(menu->children[menu->selectedChildIndex]->getType()==UIElementType::Value &&
    std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex])->selected)
    {
        //if(!this->buzzDisabled) Device::buzz();
        auto valueElement = std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex]);
        if(valueElement->selectedValueIndex<valueElement->values.size()-1 || valueElement->infinitescrolling==true) valueElement->selectedValueIndex++;
        if(valueElement->selectedValueIndex>valueElement->values.size()-1) valueElement->selectedValueIndex-= valueElement->values.size();
        drawValuePartial();
        //if(!this->buzzDisabled) Device::buzz();
    }
    else if(menu->children[menu->selectedChildIndex]->getType()==UIElementType::Book)
    {
            //if(!this->buzzDisabled) Device::buzz();
            auto bookElement = std::static_pointer_cast<BookElement>(menu->children[menu->selectedChildIndex]);
            Book *book = bookElement->book;

            if(book->badParse)
            {
                Device::getInstance().notificationHandler->drawErrorNotification(book->title);
                vTaskDelay(500);
                drawMenu();
                return;
            }
            if(!book->matchesRenderSettings(book->getCurrentRenderSettings()))
            {
                Device::getInstance().notificationHandler->drawIndexingNotification(book->title,0);
                Device::getInstance().bookHandler->reindexBook(book);
                //book->currentPage = 0;
            }
            Device::getInstance().notificationHandler->drawBookOpeningNotification(book->title);
            Reader *reader = Device::getInstance().reader;
            reader->init(book,renderer);

            
            renderer->epd.forceRefresh();
            Device::getInstance().state=Device::State::Reading;
            Device::getInstance().activeBookPath = book->path;
            Device::getInstance().activeAuthorName = menu->elementName;
            Device::getInstance().saveAppState();
            reader->openPage();
            //if(!this->buzzDisabled) Device::buzz();
    }
    else middleButtonAction();
}

void MenuHandler::rightPageAction()
{
    rightButtonAction();
}

void MenuHandler::leftPageAction()
{
    leftButtonAction();
}

void MenuHandler::leftButtonAction()
{
    if (auto menu = std::static_pointer_cast<MenuElement>(currentElement)) {
        if(menu->children.size()>0 && menu->children[menu->selectedChildIndex]->getType()==UIElementType::Value &&
        std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex])->selected)
        {
            //if(!this->buzzDisabled) Device::buzz();
            auto valueElement = std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex]);
            if(valueElement->selectedValueIndex>0 || valueElement->infinitescrolling==true) valueElement->selectedValueIndex--;
            if(valueElement->selectedValueIndex<0) valueElement->selectedValueIndex += valueElement->values.size();
            drawValuePartial();
            //if(!this->buzzDisabled) Device::buzz();
        }
        else if (auto parent = menu->getParent()) {
            // parent is always a MenuElement
            auto parentMenu = std::static_pointer_cast<MenuElement>(parent);

            // Find this menu inside parent's children
            for (int i = 0; i < parentMenu->children.size(); i++) {
                if (parentMenu->children[i] == currentElement) {
                    parentMenu->selectedChildIndex = i;
                    break;
                }
            }

            //if(!this->buzzDisabled) Device::buzz();
            this->currentElement = parent;
            Device::getInstance().bookHandler->refreshFavorites();
            renderer->epd.forceRefresh();
            drawMenu();
            //if(!this->buzzDisabled) Device::buzz();
        }
    }
}

void MenuHandler::layoutReadMenu(std::vector<Author>& authorList)
{
    authorMenu->children.clear(); //clear all the books
    for(int i=0;i<authorList.size();i++) //fill in the authors and the books
    {
        auto authorElement = std::make_shared<AuthorElement>(renderer,&authorList[i]);
        authorElement->initChildren();
        this->authorMenu->addChild(authorElement);
    }
    for(int i=0;i<readSettingsMenu->children.size();i++) //and update the values in the settings
    {
        if(readSettingsMenu->children[i]->getType()==UIElementType::Value)
        {
            std::static_pointer_cast<ValueElement>(readSettingsMenu->children[i])->ReadValue();
        }
    }
    for(int i=0;i<deviceSettingsMenu->children.size();i++) //and update the values in the settings
    {
        std::static_pointer_cast<ValueElement>(deviceSettingsMenu->children[i])->ReadValue();
    }
    for(int i=0;i<einkSettingsMenu->children.size();i++) //and update the values in the settings
    {
        std::static_pointer_cast<ValueElement>(einkSettingsMenu->children[i])->ReadValue();
    }
}

void MenuHandler::layoutFontSelect()
{
    //vTaskDelay(1500);
    std::vector<std::string> fontNames;
    std::vector<int> fontIDs;
    for(int i=0;i<renderer->fontHandler.families.size();i++)
    {
         printf("fontFam: %s", renderer->fontHandler.families[i].name.c_str());
          printf("fontFam: %d", i);
        fontNames.push_back(renderer->fontHandler.families[i].name);
        fontIDs.push_back(i);
    }
    fontSelectBox = std::make_shared<ValueElement>(renderer, std::string("Active font"),std::string("Font used for text in books"),fontIDs ,fontNames,&(Device::getInstance().renderSettings.fontFamily));
    fontSelectBox->infinitescrolling = true;
    readSettingsMenu->addChild(fontSelectBox);
}

void MenuHandler::updateFontSelect()
{
    for(int i=0;i<fontSelectBox->valueDescriptions.size();i++)
    {
        if(fontSelectBox->valueDescriptions[i]==renderer->fontHandler.currentFont.family) fontSelectBox->selectedValueIndex=i;

    }
}

void MenuHandler::updateFontSize()
{
    std::string currentFontName = fontSelectBox->valueDescriptions[fontSelectBox->selectedValueIndex];
    int currentFontSize = Device::getInstance().renderSettings.fontPoints;
    fontSizeBox->selectedValueIndex = 0;
    std::vector<int> availableFontSizes;
    for(int i=0;i<renderer->fontHandler.families.size();i++)
    {
        if(renderer->fontHandler.families[i].name==currentFontName)
        {
            for(int j=0;j<renderer->fontHandler.families[i].fonts.size();j++)
            {
                availableFontSizes.push_back(renderer->fontHandler.families[i].fonts[j].pointSize);
                if(renderer->fontHandler.families[i].fonts[j].pointSize==currentFontSize) fontSizeBox->selectedValueIndex = j;
            }
        }
    }

    fontSizeBox->values.clear();
    fontSizeBox->valueDescriptions.clear();

    for(int i=0;i<availableFontSizes.size();i++)
    {
        fontSizeBox->values.push_back(availableFontSizes[i]);
        fontSizeBox->valueDescriptions.push_back(std::to_string(availableFontSizes[i]) + "Px");
    }

}

void MenuHandler::updateFont()
{
    std::string fontName = fontSelectBox->valueDescriptions[fontSelectBox->selectedValueIndex];
    int fontSize = fontSizeBox->values[fontSizeBox->selectedValueIndex];
    std::string fontFileName = "";

    for(int i=0;i<renderer->fontHandler.families.size();i++)
    {
        if(renderer->fontHandler.families[i].name==fontName)
        {
            for(int j=0;j<renderer->fontHandler.families[i].fonts.size();j++)
            {
                if(renderer->fontHandler.families[i].fonts[j].pointSize==fontSize) fontFileName = renderer->fontHandler.families[i].fonts[j].fileName;
            }
        }
    }
    if(fontFileName.length()>0) 
    {
        renderer->fontHandler.loadFont(fontFileName);
        Device::getInstance().renderSettings.fontPoints = fontSize;
    }
}

static std::string truncateString(const std::string& str, size_t maxLen) {
    if (str.length() <= maxLen) return str;
    if (maxLen <= 3) return str.substr(0, maxLen);
    return str.substr(0, maxLen - 3) + "...";
}

static std::pair<std::string, std::string> splitTitle(const std::string& title, size_t maxLenPerLine) {
    if (title.length() <= maxLenPerLine) {
        return { title, "" };
    }
    size_t splitPos = title.rfind(' ', maxLenPerLine);
    if (splitPos == std::string::npos || splitPos < maxLenPerLine / 2) {
        splitPos = maxLenPerLine;
    }
    std::string line1 = title.substr(0, splitPos);
    std::string line2 = title.substr(splitPos + (title[splitPos] == ' ' ? 1 : 0));
    if (line2.length() > maxLenPerLine) {
        line2 = truncateString(line2, maxLenPerLine);
    }
    return { line1, line2 };
}

static void drawOrnamentalCard(Renderer* renderer, int x, int y, int width, int height)
{
    if (!renderer || width <= 0 || height <= 0) return;

    // 1. Cast shadow (offset +3 in X, -3 in Y, 50% checkerboard dither)
    const int sh_dx = 3;
    const int sh_dy = -3;
    for (int i = 0; i < height; i++) {
        int cy = y + i + sh_dy;
        if (cy >= 0 && cy < EPD_WIDTH) {
            int slx = x + sh_dx;
            int srx = x + width + sh_dx;
            for (int cx = slx; cx <= srx; cx++) {
                if (cx >= 0 && cx < EPD_HEIGHT && (cx + cy) % 2 == 0) {
                    renderer->drawPixel(cx, cy, false);
                }
            }
        }
    }

    // 2. White background fill
    for (int i = 0; i < height; i++) {
        int cy = y + i;
        if (cy >= 0 && cy < EPD_WIDTH) {
            for (int cx = x; cx <= x + width; cx++) {
                if (cx >= 0 && cx < EPD_HEIGHT) {
                    renderer->drawPixel(cx, cy, true);
                }
            }
        }
    }

    // 3. Outer border (2 pixels thick, black)
    for (int cx = x; cx <= x + width; cx++) {
        renderer->drawPixel(cx, y, false);
        renderer->drawPixel(cx, y + 1, false);
        renderer->drawPixel(cx, y + height - 1, false);
        renderer->drawPixel(cx, y + height - 2, false);
    }
    for (int cy = y; cy <= y + height; cy++) {
        renderer->drawPixel(x, cy, false);
        renderer->drawPixel(x + 1, cy, false);
        renderer->drawPixel(x + width, cy, false);
        renderer->drawPixel(x + width - 1, cy, false);
    }

    // 4. Inner stone moulding (1 pixel line, 3 pixels inside)
    const int gap = 3;
    if (width > 2 * gap && height > 2 * gap) {
        for (int cx = x + gap; cx <= x + width - gap; cx++) {
            renderer->drawPixel(cx, y + gap, false);
            renderer->drawPixel(cx, y + height - 1 - gap, false);
        }
        for (int cy = y + gap; cy <= y + height - gap; cy++) {
            renderer->drawPixel(x + gap, cy, false);
            renderer->drawPixel(x + width - gap, cy, false);
        }
    }

    // 5. Corner diamond / notch accents (classic Gothic touch)
    const int c_off = 7;
    int corners_x[4] = { x + c_off, x + width - c_off, x + c_off, x + width - c_off };
    int corners_y[4] = { y + c_off, y + c_off, y + height - 1 - c_off, y + height - 1 - c_off };
    for (int c = 0; c < 4; c++) {
        renderer->drawPixel(corners_x[c], corners_y[c], false);
        renderer->drawPixel(corners_x[c] - 1, corners_y[c], false);
        renderer->drawPixel(corners_x[c] + 1, corners_y[c], false);
        renderer->drawPixel(corners_x[c], corners_y[c] - 1, false);
        renderer->drawPixel(corners_x[c], corners_y[c] + 1, false);
    }
}

void MenuHandler::drawBookInfo(Book *book)
{
    if (!book || !renderer) return;

    // 1. Top Header Gothic Box
    renderer->drawGothicBox(80, 595, 320, 38, true);
    std::string headerText = "✦  BOOK DETAILS  ✦";
    int hx = (EPD_HEIGHT - headerText.length() * 8) / 2;
    renderer->drawString(hx, 606, headerText, 1, true, false, false);

    // 2. Book Title & Author Card
    drawOrnamentalCard(renderer, 16, 490, 448, 95);
    std::string titleStr = book->title.empty() ? book->path : book->title;
    auto titleLines = splitTitle(titleStr, 34);
    if (titleLines.second.empty()) {
        int tx = 16 + (448 - titleLines.first.length() * 8) / 2;
        if (tx < 30) tx = 30;
        renderer->drawString(tx, 546, titleLines.first, 1, true, false, true);
    } else {
        int tx1 = 16 + (448 - titleLines.first.length() * 8) / 2;
        int tx2 = 16 + (448 - titleLines.second.length() * 8) / 2;
        if (tx1 < 30) tx1 = 30;
        if (tx2 < 30) tx2 = 30;
        renderer->drawString(tx1, 554, titleLines.first, 1, true, false, true);
        renderer->drawString(tx2, 536, titleLines.second, 1, true, false, true);
    }

    std::string authorStr = "by " + (book->author.empty() ? "Unknown Author" : book->author);
    if (book->favorite) authorStr += "  ❤";
    authorStr = truncateString(authorStr, 40);
    int ax = 16 + (448 - authorStr.length() * 8) / 2;
    if (ax < 30) ax = 30;
    renderer->drawString(ax, 508, authorStr, 1, false, false, true);

    // 3. Reading Progress & Status Card
    drawOrnamentalCard(renderer, 16, 360, 448, 118);

    std::string statusStr;
    if (book->badParse) {
        statusStr = "ERROR (Corrupt File)";
    } else if (book->totalPageCount <= 0) {
        statusStr = "UNINDEXED (Open to parse)";
    } else if (book->currentPage >= book->totalPageCount - 2) {
        statusStr = "COMPLETED ★";
    } else if (book->currentPage > 0) {
        statusStr = "IN PROGRESS";
    } else {
        statusStr = "UNREAD";
    }

    int percent = 0;
    if (book->totalPageCount > 1) {
        percent = (book->currentPage * 100) / (book->totalPageCount - 1);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
    } else if (book->totalPageCount == 1 && book->currentPage >= 1) {
        percent = 100;
    }

    std::string statusLine = "Status : " + statusStr;
    renderer->drawString(32, 452, statusLine, 1, true, false, true);

    std::string pageLine;
    if (book->totalPageCount > 0) {
        pageLine = "Page " + std::to_string(book->currentPage + 1) + " of " + std::to_string(book->totalPageCount) + " (" + std::to_string(percent) + "% completed)";
    } else {
        pageLine = "Page count unknown";
    }
    renderer->drawString(32, 430, pageLine, 1, false, false, true);

    // Custom Outlined Progress Bar
    int pb_x = 32;
    int pb_y = 404;
    int pb_w = 416;
    int pb_h = 14;
    for (int cx = pb_x; cx < pb_x + pb_w; cx++) {
        renderer->drawPixel(cx, pb_y, false);
        renderer->drawPixel(cx, pb_y + 1, false);
        renderer->drawPixel(cx, pb_y + pb_h - 1, false);
        renderer->drawPixel(cx, pb_y + pb_h - 2, false);
    }
    for (int cy = pb_y; cy < pb_y + pb_h; cy++) {
        renderer->drawPixel(pb_x, cy, false);
        renderer->drawPixel(pb_x + 1, cy, false);
        renderer->drawPixel(pb_x + pb_w - 1, cy, false);
        renderer->drawPixel(pb_x + pb_w - 2, cy, false);
    }
    int inner_w = pb_w - 6;
    int inner_h = pb_h - 6;
    int fill_w = (inner_w * percent) / 100;
    for (int cy = pb_y + 3; cy < pb_y + 3 + inner_h; cy++) {
        for (int cx = pb_x + 3; cx < pb_x + 3 + inner_w; cx++) {
            if (cx < pb_x + 3 + fill_w) {
                renderer->drawPixel(cx, cy, false);
            } else {
                if ((cx % 2 == 0) && (cy % 2 == 0)) {
                    renderer->drawPixel(cx, cy, false);
                } else {
                    renderer->drawPixel(cx, cy, true);
                }
            }
        }
    }

    std::string chapterProgress;
    if (book->chapterCount > 0) {
        chapterProgress = "Chapter " + std::to_string(book->currentPageChapterIndex + 1) + " of " + std::to_string(book->chapterCount);
        if (book->chapterPageCounts.size() > (size_t)book->currentPageChapterIndex && book->chapterPageCounts[book->currentPageChapterIndex] > 0) {
            chapterProgress += " (" + std::to_string(book->chapterPageCounts[book->currentPageChapterIndex]) + " pages in chapter)";
        }
    } else {
        chapterProgress = "Chapters: None indexed";
    }
    renderer->drawString(32, 378, truncateString(chapterProgress, 46), 1, false, false, true);

    // 4. Book Metadata & Specifications Card
    drawOrnamentalCard(renderer, 16, 88, 448, 260);
    std::string metaHeader = "✦  SPECIFICATIONS & SETTINGS  ✦";
    int mhx = 16 + (448 - metaHeader.length() * 8) / 2;
    renderer->drawString(mhx, 324, metaHeader, 1, true, false, true);
    for (int cx = 32; cx <= 448; cx++) {
        renderer->drawPixel(cx, 316, false);
    }

    std::string bmStr;
    if (book->bookMarks.empty()) {
        bmStr = "None";
    } else {
        bmStr = std::to_string(book->bookMarks.size()) + " saved (p.";
        for (size_t i = 0; i < std::min((size_t)4, book->bookMarks.size()); i++) {
            if (i > 0) bmStr += ", ";
            bmStr += std::to_string(book->bookMarks[i].pageIndex + 1);
        }
        if (book->bookMarks.size() > 4) bmStr += ", ...";
        bmStr += ")";
    }

    std::string shortPath = book->path;
    size_t lastSlash = shortPath.find_last_of("/\\");
    if (lastSlash != std::string::npos) shortPath = shortPath.substr(lastSlash + 1);
    shortPath = truncateString(shortPath, 28);

    std::string fontName = book->renderSettings.fontFamily.empty() ? "System Default" : book->renderSettings.fontFamily;
    std::string fontPt = book->renderSettings.fontPoints > 0 ? (std::to_string(book->renderSettings.fontPoints) + " px") : "Default (14 px)";

    renderer->drawString(32, 292, "Chapters   : " + std::to_string(book->chapterCount) + (book->chapterCount == 1 ? " chapter" : " chapters"), 1, false, false, true);
    renderer->drawString(32, 270, truncateString("Bookmarks  : " + bmStr, 46), 1, false, false, true);
    renderer->drawString(32, 248, "Favorite   : " + std::string(book->favorite ? "Yes (❤ Marked in Favorites)" : "No"), 1, false, false, true);
    renderer->drawString(32, 226, "File Name  : " + shortPath, 1, false, false, true);
    renderer->drawString(32, 204, "Storage    : " + std::string(Device::getInstance().deviceSettings.storeDataOnSD ? "SD Card (/sdcard)" : "Internal Flash (/littlefs)"), 1, false, false, true);
    renderer->drawString(32, 182, "Font Style : " + fontName + " • " + fontPt, 1, false, false, true);
    renderer->drawString(32, 160, "Weight     : " + std::string(book->renderSettings.fontBold ? "Bold text" : "Standard weight"), 1, false, false, true);
    renderer->drawString(32, 138, "Margins    : Horiz " + std::to_string(book->renderSettings.marginsHorizontal) + "em / Vert " + std::to_string(book->renderSettings.marginsVertical) + "em", 1, false, false, true);
    renderer->drawString(32, 116, "Line Space : " + std::to_string(book->renderSettings.lineSpacing) + " px additional", 1, false, false, true);

    // 5. Bottom Navigation Legend (Inverted Gothic Box)
    renderer->drawGothicBox(16, 32, 448, 42, true);
    std::string legend = "▶ Open Book     ● Toggle Fav     ◀ Back";
    int lx = (EPD_HEIGHT - legend.length() * 8) / 2;
    renderer->drawString(lx, 45, legend, 1, true, false, false);
}

void MenuHandler::drawAuthorInfo(Author *author)
{
    if (!author || !renderer) return;

    bool isFavorites = (author->name == "Favorite books");

    // 1. Top Header Gothic Box
    renderer->drawGothicBox(80, 595, 320, 38, true);
    std::string headerText = isFavorites ? "✦  FAVORITE BOOKS  ✦" : "✦  AUTHOR OVERVIEW  ✦";
    int hx = (EPD_HEIGHT - headerText.length() * 8) / 2;
    renderer->drawString(hx, 606, headerText, 1, true, false, false);

    // 2. Author Name Card
    drawOrnamentalCard(renderer, 16, 490, 448, 95);
    std::string authorDisplayName = isFavorites ? "Favorite Collection ❤" : author->name;
    authorDisplayName = truncateString(authorDisplayName, 34);
    int ax = 16 + (448 - authorDisplayName.length() * 8) / 2;
    if (ax < 30) ax = 30;
    renderer->drawString(ax, 546, authorDisplayName, 1, true, false, true);

    std::string countStr = std::to_string(author->bookList.size()) + (author->bookList.size() == 1 ? " Book in Collection" : " Books in Collection");
    int cx = 16 + (448 - countStr.length() * 8) / 2;
    if (cx < 30) cx = 30;
    renderer->drawString(cx, 514, countStr, 1, false, false, true);

    // 3. Collection Statistics Card
    drawOrnamentalCard(renderer, 16, 360, 448, 118);
    std::string statsHeader = "✦  COLLECTION STATISTICS  ✦";
    int shx = 16 + (448 - statsHeader.length() * 8) / 2;
    renderer->drawString(shx, 454, statsHeader, 1, true, false, true);
    for (int cx = 32; cx <= 448; cx++) {
        renderer->drawPixel(cx, 446, false);
    }

    int totalPages = 0;
    int completedCount = 0;
    int inProgressCount = 0;
    int unreadCount = 0;
    int favoriteCount = 0;
    int totalBookmarks = 0;

    for (Book* b : author->bookList) {
        if (!b) continue;
        totalPages += b->totalPageCount;
        if (b->favorite) favoriteCount++;
        totalBookmarks += b->bookMarks.size();
        if (b->totalPageCount > 0 && b->currentPage >= b->totalPageCount - 2) {
            completedCount++;
        } else if (b->currentPage > 0) {
            inProgressCount++;
        } else {
            unreadCount++;
        }
    }

    renderer->drawString(32, 422, "Total Pages  : " + std::to_string(totalPages) + " pages across " + std::to_string(author->bookList.size()) + " books", 1, false, false, true);
    renderer->drawString(32, 398, "Progress     : " + std::to_string(completedCount) + " finished, " + std::to_string(inProgressCount) + " reading, " + std::to_string(unreadCount) + " unread", 1, false, false, true);
    renderer->drawString(32, 374, "Saved Data   : " + std::to_string(totalBookmarks) + " bookmarks, " + std::to_string(favoriteCount) + " favorited", 1, false, false, true);

    // 4. Books in Collection Card
    drawOrnamentalCard(renderer, 16, 88, 448, 260);
    std::string booksHeader = isFavorites ? "✦  FAVORITE TITLES  ✦" : "✦  BOOKS IN COLLECTION  ✦";
    int bhx = 16 + (448 - booksHeader.length() * 8) / 2;
    renderer->drawString(bhx, 324, booksHeader, 1, true, false, true);
    for (int cx = 32; cx <= 448; cx++) {
        renderer->drawPixel(cx, 316, false);
    }

    if (author->bookList.empty()) {
        std::string empty1 = isFavorites ? "No favorite books added yet." : "No books found for this author.";
        std::string empty2 = isFavorites ? "While browsing any book, press [●]" : "Add EPUB books to SD card to populate.";
        std::string empty3 = isFavorites ? "to mark it as a favorite." : "";
        renderer->drawString(32, 260, empty1, 1, false, false, true);
        renderer->drawString(32, 230, empty2, 1, false, false, true);
        if (!empty3.empty()) renderer->drawString(32, 204, empty3, 1, false, false, true);
    } else {
        int maxPreview = std::min((size_t)4, author->bookList.size());
        for (int i = 0; i < maxPreview; i++) {
            Book* b = author->bookList[i];
            if (!b) continue;
            int y_top = 286 - i * 46;

            std::string titleDisplay = std::to_string(i + 1) + ". " + b->title;
            if (b->favorite && !isFavorites) titleDisplay += " ❤";
            titleDisplay = truncateString(titleDisplay, 46);
            renderer->drawString(32, y_top, titleDisplay, 1, true, false, true);

            std::string bookProg;
            if (b->badParse) {
                bookProg = "Parse Error";
            } else if (b->totalPageCount <= 0) {
                bookProg = "Unindexed";
            } else if (b->currentPage >= b->totalPageCount - 2) {
                bookProg = "Completed ★ (" + std::to_string(b->totalPageCount) + " pages)";
            } else if (b->currentPage > 0) {
                int pct = (b->currentPage * 100) / (b->totalPageCount - 1);
                bookProg = "Page " + std::to_string(b->currentPage + 1) + "/" + std::to_string(b->totalPageCount) + " (" + std::to_string(pct) + "%)";
            } else {
                bookProg = "Unread (" + std::to_string(b->totalPageCount) + " pages)";
            }
            if (isFavorites && !b->author.empty()) {
                bookProg += " • by " + b->author;
            }
            bookProg = "   " + truncateString(bookProg, 46);
            renderer->drawString(32, y_top - 18, bookProg, 1, false, false, true);
        }

        if (author->bookList.size() > 4) {
            std::string moreStr = "  + " + std::to_string(author->bookList.size() - 4) + " more book(s) in collection";
            renderer->drawString(32, 98, moreStr, 1, false, false, true);
        }
    }

    // 5. Bottom Navigation Legend (Inverted Gothic Box)
    renderer->drawGothicBox(16, 32, 448, 42, true);
    std::string legend = "▶ View Books     ● Enter Author     ◀ Back";
    int lx = (EPD_HEIGHT - legend.length() * 8) / 2;
    renderer->drawString(lx, 45, legend, 1, true, false, false);
}

void MenuHandler::drawLibraryInfo()
{
    BookHandler *bh = Device::getInstance().bookHandler;
    if (!bh || !renderer) return;

    // Count totals (skip index 0 which is the Favorites virtual author)
    int totalBooks = 0;
    int realAuthorCount = 0;
    for (size_t i = 1; i < bh->authorList.size(); i++) {
        realAuthorCount++;
        totalBooks += (int)bh->authorList[i].bookList.size();
    }

    // --- Disk space ---
    size_t usedBytes = 0, totalBytesFS = 0;
    bool haveSpace = false;
    const char *mountPath = Device::getInstance().deviceSettings.storeDataOnSD ? "/sdcard" : "/littlefs";
    struct statvfs svfs;
    if (statvfs(mountPath, &svfs) == 0) {
        totalBytesFS = (size_t)svfs.f_blocks * svfs.f_frsize;
        size_t freeBytes  = (size_t)svfs.f_bfree  * svfs.f_frsize;
        usedBytes = totalBytesFS - freeBytes;
        haveSpace = true;
    }

    // Helper: format bytes as "X.X MB" or "X KB"
    auto fmtBytes = [](size_t b) -> std::string {
        if (b >= 1024*1024) {
            int mb10 = (int)((b * 10) / (1024*1024));
            return std::to_string(mb10 / 10) + "." + std::to_string(mb10 % 10) + " MB";
        }
        return std::to_string(b / 1024) + " KB";
    };

    // ---- 1. Top header box ----
    renderer->drawGothicBox(80, 595, 320, 38, true);
    std::string headerText = "✦  LIBRARY OVERVIEW  ✦";
    int hx = (EPD_HEIGHT - (int)headerText.length() * 8) / 2;
    renderer->drawString(hx, 606, headerText, 1, true, false, false);

    // ---- 2. Summary card ----
    drawOrnamentalCard(renderer, 16, 490, 448, 95);
    std::string totalStr = std::to_string(totalBooks) + (totalBooks == 1 ? " Book" : " Books");
    totalStr += "  •  " + std::to_string(realAuthorCount) + (realAuthorCount == 1 ? " Author" : " Authors");
    int tx = 16 + (448 - (int)totalStr.length() * 8) / 2;
    if (tx < 30) tx = 30;
    renderer->drawString(tx, 546, totalStr, 1, true, false, true);

    std::string spaceStr;
    if (haveSpace) {
        spaceStr = "Used: " + fmtBytes(usedBytes) + " / " + fmtBytes(totalBytesFS);
    } else {
        spaceStr = "Storage: " + std::string(Device::getInstance().deviceSettings.storeDataOnSD ? "SD Card" : "Internal Flash");
    }
    spaceStr = truncateString(spaceStr, 40);
    int sx = 16 + (448 - (int)spaceStr.length() * 8) / 2;
    if (sx < 30) sx = 30;
    renderer->drawString(sx, 514, spaceStr, 1, false, false, true);

    // ---- 3. Storage bar (if we have space info) ----
    if (haveSpace && totalBytesFS > 0) {
        int pb_x = 32, pb_y = 498, pb_w = 416, pb_h = 10;
        // border
        for (int cx = pb_x; cx < pb_x + pb_w; cx++) {
            renderer->drawPixel(cx, pb_y,          false);
            renderer->drawPixel(cx, pb_y + pb_h - 1, false);
        }
        for (int cy = pb_y; cy < pb_y + pb_h; cy++) {
            renderer->drawPixel(pb_x,          cy, false);
            renderer->drawPixel(pb_x + pb_w - 1, cy, false);
        }
        int inner_w = pb_w - 4;
        int fill_w = (int)(((long long)inner_w * (long long)usedBytes) / (long long)totalBytesFS);
        for (int cy = pb_y + 2; cy < pb_y + pb_h - 2; cy++) {
            for (int cx = pb_x + 2; cx < pb_x + 2 + inner_w; cx++) {
                if (cx < pb_x + 2 + fill_w) renderer->drawPixel(cx, cy, false);
                else renderer->drawPixel(cx, cy, true);
            }
        }
    }

    // ---- 4. Author list card ----
    drawOrnamentalCard(renderer, 16, 88, 448, 388);
    std::string listHeader = "✦  AUTHORS  ✦";
    int lhx = 16 + (448 - (int)listHeader.length() * 8) / 2;
    renderer->drawString(lhx, 454, listHeader, 1, true, false, true);
    for (int cx = 32; cx <= 448; cx++) renderer->drawPixel(cx, 446, false);

    if (realAuthorCount == 0) {
        std::string e1 = "No books found in library.";
        std::string e2 = "Add .epub files to SD card or";
        std::string e3 = "use Transfer Files mode.";
        renderer->drawString(32, 380, e1, 1, false, false, true);
        renderer->drawString(32, 352, e2, 1, false, false, true);
        renderer->drawString(32, 324, e3, 1, false, false, true);
    } else {
        // Show up to 10 authors; each row is 32px tall
        int maxShow = std::min(realAuthorCount, 10);
        for (int i = 0; i < maxShow; i++) {
            Author &a = bh->authorList[i + 1]; // skip favorites at index 0
            int y_row = 422 - i * 32;

            std::string nameStr = std::to_string(i + 1) + ". " + a.name;
            nameStr = truncateString(nameStr, 28);
            renderer->drawString(32, y_row, nameStr, 1, true, false, true);

            std::string cntStr = std::to_string(a.bookList.size()) + (a.bookList.size() == 1 ? " bk" : " bks");
            // right-align count inside card
            int cx_cnt = 448 - (int)cntStr.length() * 8;
            if (cx_cnt < 240) cx_cnt = 240;
            renderer->drawString(cx_cnt, y_row, cntStr, 1, false, false, true);

            // thin separator line
            if (i < maxShow - 1) {
                for (int cx = 32; cx <= 448; cx += 2) renderer->drawPixel(cx, y_row - 8, false);
            }
        }
        if (realAuthorCount > 10) {
            std::string moreStr = "  + " + std::to_string(realAuthorCount - 10) + " more author(s)";
            renderer->drawString(32, 98, moreStr, 1, false, false, true);
        }
    }

    // ---- 5. Bottom navigation legend ----
    renderer->drawGothicBox(16, 32, 448, 42, true);
    std::string legend = "▶ Browse Library     ◀ Main Menu";
    int lx = (EPD_HEIGHT - (int)legend.length() * 8) / 2;
    renderer->drawString(lx, 45, legend, 1, true, false, false);
}

void MenuHandler::drawLibraryDetails()
{
    if (!currentElement) return;

    // RTTI is disabled (-fno-rtti), so use getType() + static_pointer_cast instead of dynamic_pointer_cast
    UIElementType curType = currentElement->getType();
    if (curType != UIElementType::Menu && curType != UIElementType::Author) return;
    auto menu = std::static_pointer_cast<MenuElement>(currentElement);

    if (menu->children.empty()) {
        if (menu->getType() == UIElementType::Author) {
            auto authorElem = std::static_pointer_cast<AuthorElement>(menu);
            if (authorElem && authorElem->author) {
                drawAuthorInfo(authorElem->author);
                return;
            }
        } else if (currentElement == authorMenu) {
            drawOrnamentalCard(renderer, 16, 220, 448, 180);
            std::string line1 = "✦  LIBRARY IS EMPTY  ✦";
            std::string line2 = "No EPUB books found on storage.";
            std::string line3 = "Please add .epub books to SD card";
            std::string line4 = "or use Transfer Files mode.";
            int x1 = (EPD_HEIGHT - line1.length() * 8) / 2;
            int x2 = (EPD_HEIGHT - line2.length() * 8) / 2;
            int x3 = (EPD_HEIGHT - line3.length() * 8) / 2;
            int x4 = (EPD_HEIGHT - line4.length() * 8) / 2;
            renderer->drawString(x1, 350, line1, 1, true, false, true);
            renderer->drawString(x2, 310, line2, 1, false, false, true);
            renderer->drawString(x3, 280, line3, 1, false, false, true);
            renderer->drawString(x4, 250, line4, 1, false, false, true);
            return;
        }
        return;
    }

    if (menu->selectedChildIndex >= menu->children.size()) return;

    auto selectedChild = menu->children[menu->selectedChildIndex];
    if (!selectedChild) return;

    // When Library (authorMenu) is highlighted on the main menu, show full library overview
    if (selectedChild == authorMenu) {
        drawLibraryInfo();
        return;
    }

    if (selectedChild->getType() == UIElementType::Book) {
        auto bookElem = std::static_pointer_cast<BookElement>(selectedChild);
        if (bookElem && bookElem->book) {
            drawBookInfo(bookElem->book);
        }
    } else if (selectedChild->getType() == UIElementType::Author) {
        auto authorElem = std::static_pointer_cast<AuthorElement>(selectedChild);
        if (authorElem && authorElem->author) {
            drawAuthorInfo(authorElem->author);
        }
    }
}

void MenuHandler::drawMenu()
{ESP_LOGI("MenuHandler", "Draw menu called");
    Device::getInstance().clearButtonLatches();
    Device::getInstance().setLatchTimeOut(200000);
    //this->displayIdleCallbackReturn = nullptr;
    renderer->framebuffer = this->leftPageFrameBuffer;
    std::static_pointer_cast<MenuElement>(currentElement)->renderElement();

    //renderer->epd.DisplayPicture(true, renderer->framebuffer);

    renderer->clearScreenBuffer(rightPageFrameBuffer);
    if(currentElement==readSettingsMenu)
    {
        const char *html = pageShowcase.getPage();
        HtmlParser *parser = nullptr;
        parser = new HtmlParser(html, pageShowcase.size(), "",this->renderer,0,nullptr,rightPageFrameBuffer,Device::getInstance().reader->leftPageFrameBuffer); //dump the right page into the readers framebuffer
        parser->parse();
        delete parser;
    }
    else
    {
        renderer->framebuffer = this->rightPageFrameBuffer;
        drawLibraryDetails();
    }
    this->renderer->drawBattery(rightPageFrameBuffer,Device::getInstance().getBatteryPercentage());
    renderer->epd.DisplayPictureBoth(leftPageFrameBuffer,rightPageFrameBuffer);

    if(Device::getInstance().buttonLatchedStates[MIDDLE_BUTTON]) {middleButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_RIGHT_BUTTON]) {rightPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_LEFT_BUTTON]) {leftPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_DOWN_BUTTON]) {downButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_UP_BUTTON]) {upButtonAction(); return;}
}

void MenuHandler::drawValuePartial()
{ESP_LOGI("MenuHandler", "Draw value called");
    Device::getInstance().clearButtonLatches();
    Device::getInstance().setLatchTimeOut(200000);
  
    renderer->framebuffer = this->leftPageFrameBuffer;
    std::static_pointer_cast<MenuElement>(currentElement)->renderElement();

    auto menu = std::static_pointer_cast<MenuElement>(currentElement);
    auto activeValueElement = std::static_pointer_cast<ValueElement>(menu->children[menu->selectedChildIndex]);

    int x = activeValueElement->drawValueXPos;
    int y = activeValueElement->drawValueYPos;
    int width = (activeValueElement->maxStringWidth+1) * GLYPH_WIDTH/2;
    int height = GLYPH_HEIGHT;


    renderer->epd.DisplayPicturePartial(true, leftPageFrameBuffer,
                                x,y,x+width,y+height);


    if(Device::getInstance().buttonLatchedStates[MIDDLE_BUTTON]) {middleButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_RIGHT_BUTTON]) {rightPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[PAGE_LEFT_BUTTON]) {leftPageAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_DOWN_BUTTON]) {downButtonAction(); return;}
    if(Device::getInstance().buttonLatchedStates[ARROW_UP_BUTTON]) {upButtonAction(); return;}
}