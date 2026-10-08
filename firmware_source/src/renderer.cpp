#include "renderer.h"
#include "esp_log.h"
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <cctype>
#include <cmath>
#include "device.h"



static const char *TAG = "renderer";


static const std::unordered_map<std::string, int> htmlEntities = {
    {"amp",  '&'},
    {"lt",   '<'},
    {"gt",   '>'},
    {"quot", '"'},
    {"apos", '\''},
    {"nbsp", 0x00A0}, // non-breaking space
};

int utf8ToCodePoints(const char* utf8, int* out, int maxOutLen) {
    int i = 0;  // Index in utf8 input
    int j = 0;  // Index in output array

    while (utf8[i] && j < maxOutLen) {
        uint8_t c = (uint8_t)utf8[i];
        int codepoint = 0;
        int bytes = 0;

        if ((c & 0x80) == 0x00) {
            // 1-byte ASCII
            codepoint = c;
            bytes = 1;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8
            codepoint = (c & 0x1F) << 6;
            codepoint |= ((uint8_t)utf8[i+1] & 0x3F);
            bytes = 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8
            codepoint = (c & 0x0F) << 12;
            codepoint |= ((uint8_t)utf8[i+1] & 0x3F) << 6;
            codepoint |= ((uint8_t)utf8[i+2] & 0x3F);
            bytes = 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8
            codepoint = (c & 0x07) << 18;
            codepoint |= ((uint8_t)utf8[i+1] & 0x3F) << 12;
            codepoint |= ((uint8_t)utf8[i+2] & 0x3F) << 6;
            codepoint |= ((uint8_t)utf8[i+3] & 0x3F);
            bytes = 4;
        } else {
            // Invalid byte, skip
            i++;
            continue;
        }

        out[j++] = codepoint;
        i += bytes;
    }

    return j;  // Number of codepoints written
}

std::vector<int> decodeHtmlEntities(const std::vector<int>& input) {
    std::vector<int> output;

    size_t i = 0;
    bool lastWasSpace = false;

    while (i < input.size()) {
        // Handle HTML entities
        if (input[i] == '&') {
            // Collect characters until ';' or non-ASCII
            std::string entity;
            size_t j = i + 1;
            while (j < input.size() && input[j] != ';' && input[j] < 128) {
                entity.push_back(static_cast<char>(input[j]));
                j++;
            }

            if (j < input.size() && input[j] == ';') {
                int codepoint = -1;

                if (!entity.empty() && entity[0] == '#') {
                    // Numeric entity
                    if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
                        codepoint = std::strtol(entity.c_str() + 2, nullptr, 16);
                    } else {
                        codepoint = std::strtol(entity.c_str() + 1, nullptr, 10);
                    }
                } else {
                    // Named entity
                    auto it = htmlEntities.find(entity);
                    if (it != htmlEntities.end()) {
                        codepoint = it->second;
                    }
                }

                if (codepoint != -1) {
                    // Collapse whitespace from entities (nbsp, etc.)
                    if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
                        codepoint == '\r' || codepoint == 0x00A0) 
                    {
                        if (!lastWasSpace)
                            output.push_back(' ');
                        lastWasSpace = true;
                    } else {
                        output.push_back(codepoint);
                        lastWasSpace = false;
                    }

                    i = j + 1;
                    continue;
                }
            }
        }

        int cp = input[i];

        if(cp==0xFEFF) //zero-width space
        {
            i++;
            continue;
        }

        // Whitespace collapsers
        bool isSpaceChar =
            (cp == ' '  || cp == '\t' ||
             cp == '\n' || cp == '\r' ||
             cp == 0x00A0 || cp == 0x000A|| cp == 0x0009); 

        if (isSpaceChar) {
            if (!lastWasSpace)
                output.push_back(' ');
            lastWasSpace = true;
            i++;
            continue;
        }

        // Regular character
        output.push_back(cp);
        lastWasSpace = false;
        i++;
    }

    return output;
}



std::vector<int> utf8ToCodePoints(const std::string& utf8) {
    std::vector<int> codepoints;

    size_t i = 0;
    while (i < utf8.size()) {
        uint8_t c = static_cast<uint8_t>(utf8[i]);
        int codepoint = 0;
        int bytes = 0;

        if ((c & 0x80) == 0x00) {
            // 1-byte ASCII
            codepoint = c;
            bytes = 1;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8
            codepoint = (c & 0x1F) << 6;
            codepoint |= (static_cast<uint8_t>(utf8[i+1]) & 0x3F);
            bytes = 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8
            codepoint = (c & 0x0F) << 12;
            codepoint |= (static_cast<uint8_t>(utf8[i+1]) & 0x3F) << 6;
            codepoint |= (static_cast<uint8_t>(utf8[i+2]) & 0x3F);
            bytes = 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8
            codepoint = (c & 0x07) << 18;
            codepoint |= (static_cast<uint8_t>(utf8[i+1]) & 0x3F) << 12;
            codepoint |= (static_cast<uint8_t>(utf8[i+2]) & 0x3F) << 6;
            codepoint |= (static_cast<uint8_t>(utf8[i+3]) & 0x3F);
            bytes = 4;
        } else {
            // Invalid UTF-8 byte, skip it
            i++;
            continue;
        }

        codepoints.push_back(codepoint);
        i += bytes;
    }

    return codepoints;
}

void Renderer::init(void)
{
    //framebuffer = (unsigned char*)malloc(EPD_WIDTH * EPD_HEIGHT / 8);
    //this->clearScreenBuffer();
    if (this->epd.Init() != 0) {
        ESP_LOGE(TAG, "e-Paper init failed");
        return;
    }

    ESP_LOGI(TAG, "e-Paper init");
}


void Renderer::renderChecker(void)
{
    for(long i=0;i<EPD_WIDTH/8;i++)
    {
        for(long j=0;j<EPD_HEIGHT;j++)
        {
            if(j%2==0) this->framebuffer[i*EPD_HEIGHT + j] = 0x00;
            else this->framebuffer[i*EPD_HEIGHT + j] = 0xFF;
        }
    }
}


int Renderer::drawCharacter(int x, int y, int UTF8Index, bool bold,bool italics, int size,bool color, bool uniFont)
{
    if(uniFont || !fontHandler.checkCharExist(UTF8Index))
    {
        const char* glyphData = fontHandler.getChar(UTF8Index);
        bool wideGlyph = fontHandler.getCharWidth(UTF8Index) -1;
        for(int i=0;i<16;i++)
        {
            for(int j=0;j<16;j++)
            {
                int coordx = x + (16-i)*size;
                int coordy = y + (16-j)*size;

                if(italics && j<4) coordx+= size;
                if(italics && j>=8) coordx-= size;
                if(italics && j>=12) coordx-= size;

                if(!wideGlyph)
                {
                int byteIndex = j + int(i<8)*16;
                int bitIndex = i%8;
                if((glyphData[byteIndex] >> bitIndex) & 1) this->drawSquare(coordx,coordy,size,size,!color);
                }
                else
                {
                int byteIndex = (j*2) + int(i<8);
                int bitIndex = i%8;
                if((glyphData[byteIndex] >> bitIndex) & 1) this->drawSquare(coordx,coordy,size,size,!color);
                }
            }
        }
        if(bold) this->drawCharacter(x+size,y,UTF8Index, false,italics,size,color);
        return (8 + 8*wideGlyph)*size;
    }
    else
    {
        if(UTF8Index>9999 && fontHandler.uniFontEnabled==false) return 0;
        int width = fontHandler.getFontCharBitmapWidth(UTF8Index);
        int height = fontHandler.getFontCharHeight(UTF8Index);
        int italicsOffset = 0;
        for(int i=0;i<width;i++)
        {
            for(int j=0;j<height;j++)
            {
                if(italics) italicsOffset = (j-height/2)/2;
                int pixelIndex = i+(height-j-1)*width;
                if(fontHandler.getFontChar(UTF8Index,pixelIndex)) drawSquare(x+(i+fontHandler.getLeftBearing(UTF8Index)+italicsOffset)*size,y+j*size,size,size,!color);
                
            }
            
        }
        if(bold) this->drawCharacter(x+size,y,UTF8Index, false,italics,size,color,false);
        return (fontHandler.getFontCharWidth(UTF8Index) + bold)*size;
    }
}

void Renderer::drawText(int x, int y, int* text, int text_length, int size, bool* boldMask,bool* italicsMask)
{
    bool bold=false;
    bool italics=false;
    int currentCharacterX = x;
    for(int i=0;i<text_length;i++)
    {   
        if(boldMask) bold=boldMask[i];
        if(italicsMask) italics=italicsMask[i];
        currentCharacterX += this->drawCharacter(currentCharacterX,y,text[i],bold,italics,size);
    }
}

void Renderer::drawText(int x, int y, int* text, int text_length, int size, bool bold,bool italics)
{
    int currentCharacterX = x;
    for(int i=0;i<text_length;i++)
    {   
        currentCharacterX += this->drawCharacter(currentCharacterX,y,text[i],bold,italics,size);
    }
}

void Renderer::drawString(int x, int y,std::string textString, int size, bool bold,bool italics,bool color)
{
    int stringSize = (int) textString.length();
    int* intArray = new int[stringSize];
    int utf8StringSize = utf8ToCodePoints(textString.c_str(), intArray, stringSize);

    int currentCharacterX = x;
    for(int i=0;i<utf8StringSize;i++)
    {   
        currentCharacterX += this->drawCharacter(currentCharacterX,y,intArray[i],bold,italics,size,color);
    }
    delete[] intArray;
}

void Renderer::drawString(int x, int y, std::vector<int> intArray, int size, std::vector<int> boldMask,std::vector<int> italicsMask,bool color,bool uniFont)
{
    int currentCharacterX = x;
    for(int i=0;i<intArray.size();i++)
    {   
        currentCharacterX += this->drawCharacter(currentCharacterX,y,intArray[i],boldMask[i],italicsMask[i],size,color,uniFont);
    }
}


void Renderer::drawSquare(int startx, int starty, int lengthx, int lengthy, bool value)
{
    for(long i=0;i<lengthx;i++)
    {
        for(long j=0;j<lengthy;j++)
        {
            int coordx = startx + i;
            int coordy = starty + j;
            this->drawPixel(coordx,coordy,value);
        }
    }
}

void Renderer::drawPaddedBox(int startx, int starty, int lengthx, int lengthy, int padding, bool value)
{
    if (!value) {
        // Just fill the whole box
        drawSquare(startx, starty, lengthx, lengthy, false);
    } else {
        // Draw the border as four strips
        // Top
        drawSquare(startx, starty, lengthx, padding, false);
        // Bottom
        drawSquare(startx, starty + lengthy - padding, lengthx, padding, false);
        // Left
        drawSquare(startx, starty + padding, padding, lengthy - 2 * padding, false);
        // Right
        drawSquare(startx + lengthx - padding, starty + padding, padding, lengthy - 2 * padding, false);
    }
}

void Renderer::drawGothicBox(int x, int y, int width, int height, bool inverted)
{
    if (height <= 0 || width <= 0) return;

    // Classic Gothic window pointed arch (ogival / two-centered arch),
    // created by two intersecting circular arcs on the left and right ends.
    int d = height / 2;
    int x_left_tip = x;
    int x_right_tip = x + width;
    int x_sh_left = x_left_tip + d;
    int x_sh_right = x_right_tip - d;

    std::vector<int> outer_lx(height), outer_rx(height);
    std::vector<int> inner_lx(height), inner_rx(height);

    const int moulding_gap = 3;

    // Two intersecting circular arcs:
    // Lower arc (y_local <= height/2): circle centered at (x_c, height)
    // Upper arc (y_local > height/2): circle centered at (x_c, 0)
    float x_c = (float)(d * d - 0.75f * height * height) / (2.0f * (float)d);
    float R_sq = x_c * x_c + (float)(height * height);

    int in_h = height - 2 * moulding_gap;
    int in_d = d - moulding_gap;
    if (in_d < 4) in_d = 4;
    float in_x_c = (float)(in_d * in_d - 0.75f * in_h * in_h) / (2.0f * (float)in_d);
    float in_R_sq = in_x_c * in_x_c + (float)(in_h * in_h);

    for (int i = 0; i < height; i++) {
        float dy = (i <= height / 2) ? (float)(height - 1 - i) : (float)i;
        float term = R_sq - dy * dy;
        int off = (int)roundf(x_c + sqrtf(term > 0.0f ? term : 0.0f));
        if (off < 0) off = 0;
        outer_lx[i] = x_sh_left - off;
        outer_rx[i] = x_sh_right + off;

        int in_y = i - moulding_gap;
        if (in_y < 0) in_y = 0;
        if (in_y >= in_h) in_y = in_h - 1;
        float in_dy = (in_y <= in_h / 2) ? (float)(in_h - 1 - in_y) : (float)in_y;
        float in_term = in_R_sq - in_dy * in_dy;
        int in_off = (int)roundf(in_x_c + sqrtf(in_term > 0.0f ? in_term : 0.0f));
        if (in_off < 0) in_off = 0;
        inner_lx[i] = x_sh_left - in_off;
        inner_rx[i] = x_sh_right + in_off;
    }

    // 1. Cast shadow (offset down and right: sh_dx = +4, sh_dy = -4)
    // 50% checkerboard dither for soft e-paper drop shadow
    const int sh_dx = 4;
    const int sh_dy = -4;
    for (int i = 0; i < height; i++) {
        int cy = y + i + sh_dy;
        if (cy >= 0 && cy < EPD_WIDTH) {
            int slx = outer_lx[i] + sh_dx;
            int srx = outer_rx[i] + sh_dx;
            for (int cx = slx; cx <= srx; cx++) {
                if ((cx + cy) % 2 == 0) {
                    drawPixel(cx, cy, false);
                }
            }
        }
    }

    // 2. Fill background
    // false = black (inverted/selected), true = white (unselected)
    bool fill_val = inverted ? false : true;
    for (int i = 0; i < height; i++) {
        int cy = y + i;
        int lx = outer_lx[i];
        int rx = outer_rx[i];
        for (int cx = lx; cx <= rx; cx++) {
            drawPixel(cx, cy, fill_val);
        }
    }

    // 3. Outer border (2 pixels thick, black)
    for (int cx = x_sh_left; cx <= x_sh_right; cx++) {
        drawPixel(cx, y, false);
        drawPixel(cx, y + 1, false);
        drawPixel(cx, y + height - 1, false);
        drawPixel(cx, y + height - 2, false);
    }
    for (int i = 0; i < height; i++) {
        int cy = y + i;
        drawPixel(outer_lx[i], cy, false);
        drawPixel(outer_lx[i] + 1, cy, false);
        drawPixel(outer_rx[i], cy, false);
        drawPixel(outer_rx[i] - 1, cy, false);
    }

    // 4. Inner stone moulding (double border following the gothic window shape)
    // White (true) when inverted, black (false) when normal
    bool inner_val = inverted ? true : false;
    for (int cx = x_sh_left; cx <= x_sh_right; cx++) {
        drawPixel(cx, y + moulding_gap, inner_val);
        drawPixel(cx, y + height - 1 - moulding_gap, inner_val);
    }
    for (int i = moulding_gap; i < height - moulding_gap; i++) {
        int cy = y + i;
        drawPixel(inner_lx[i], cy, inner_val);
        drawPixel(inner_rx[i], cy, inner_val);
    }
}

void Renderer::drawTextBox(int y, std::string line1, bool line1Bold,bool inverted)
{
    int originX = 14;
    int rectHeight = 2 * GLYPH_HEIGHT;
    int rectWidth = EPD_HEIGHT - 2 * originX;
    int padding = 4;
    int d = rectHeight / 2;
    int textStartX = originX + d + 8;
    drawGothicBox(originX, y, rectWidth, rectHeight, inverted);
    drawString(textStartX, y + 0.5*GLYPH_HEIGHT + padding, line1, 1, line1Bold, false, !inverted);
}

void Renderer::drawTextBox(int y, std::string line1, bool line1Bold,std::string line2, bool line2Bold,bool inverted)
{
    int originX = 14;
    int rectHeight = 3 * GLYPH_HEIGHT;
    int rectWidth = EPD_HEIGHT - 2 * originX;
    int padding = 4;
    int d = rectHeight / 2;
    int textStartX = originX + d + 8;
    drawGothicBox(originX, y, rectWidth, rectHeight, inverted);
    drawString(textStartX, y + 1.5*GLYPH_HEIGHT + padding, line1, 1, line1Bold, false, !inverted);
    drawString(textStartX, y + 0.5*GLYPH_HEIGHT + padding, line2, 1, line2Bold, false, !inverted);
}

void Renderer::drawTextBox(int y, std::string line1, bool line1Bold,std::string line2, bool line2Bold,std::string line3, bool line3Bold,bool inverted)
{
    int originX = 14;
    int rectHeight = 4 * GLYPH_HEIGHT;
    int rectWidth = EPD_HEIGHT - 2 * originX;
    int padding = 4;
    int d = rectHeight / 2;
    int textStartX = originX + d + 8;
    drawGothicBox(originX, y, rectWidth, rectHeight, inverted);
    drawString(textStartX, y + 2.5*GLYPH_HEIGHT + padding, line1, 1, line1Bold, false, !inverted);
    drawString(textStartX, y + 1.5*GLYPH_HEIGHT + padding, line2, 1, line2Bold, false, !inverted);
    drawString(textStartX, y + 0.5*GLYPH_HEIGHT + padding, line3, 1, line3Bold, false, !inverted);
}

void Renderer::drawPixel(int x, int y, bool value)
{
    if (x < 0 || x >= EPD_HEIGHT) return;
    if (y < 0 || y >= EPD_WIDTH) return;

    int flippedX = (EPD_HEIGHT - 1) - x;
    int flippedY = (EPD_WIDTH  - 1) - y;

    int byteLoc = (flippedY / 8) + flippedX * (EPD_WIDTH / 8);
    int bitLoc = 7 - (flippedY % 8);

    if (!value)
        framebuffer[byteLoc] |= (1 << bitLoc);
    else
        framebuffer[byteLoc] &= ~(1 << bitLoc);
}

void Renderer::drawBookMark(unsigned char* framebuffer,bool value)
{
    this->framebuffer = framebuffer;
    int size = 50;
    for(int x=0;x<size;x++)
    {
        for(int y=0;y<size;y++)
        {
            if(y<x) drawPixel(x+EPD_HEIGHT-size,EPD_WIDTH-y,value);
        }
    }

}

void Renderer::drawPageOverlay(unsigned char* framebuffer,int currentPage,int totalPages)
{
    this->framebuffer = framebuffer;
    std::string pageString;
    if(totalPages < 2) return;
    if(!Device::getInstance().deviceSettings.showPagePercentage) pageString = std::to_string(currentPage) + "/" + std::to_string(totalPages);
    else pageString = std::to_string(((currentPage-1)*100) / (totalPages-1)) + "%";
    int pageStringLength = pageString.length();
    int coordx = EPD_HEIGHT/2 - GLYPH_WIDTH/4 - GLYPH_WIDTH*pageStringLength/4;
    int coordy = 0;
    int *textArray = (int*) calloc(pageStringLength,sizeof(int));
    for(int i=0;i<pageStringLength;i++)
    {
        textArray[i] = static_cast<int>(pageString[i]);
    }
    this->drawText(coordx,coordy,textArray,pageStringLength,1,nullptr,nullptr);
    free(textArray);
}

void Renderer::drawVoltage(unsigned char* framebuffer,float voltage)
{
    this->framebuffer = framebuffer;
    std::string voltageString = std::to_string(voltage).substr(0, 4) + "V";
    int voltageStringLength = voltageString.length();
    int coordx = EPD_HEIGHT - GLYPH_WIDTH/2 -  (GLYPH_WIDTH*(voltageStringLength/2+4));
    int coordy = 0;
    this->drawString(coordx,coordy,voltageString,1,false,false);
}

void Renderer::drawBattery(unsigned char* framebuffer,int level)
{
    this->framebuffer = framebuffer;
    drawSquare(EPD_HEIGHT-6*GLYPH_WIDTH,0,EPD_HEIGHT,GLYPH_HEIGHT,true);
    drawSquare(0,0,6*GLYPH_WIDTH,GLYPH_HEIGHT,true);
    std::string voltageString = std::to_string(level)+ "%";
    int voltageStringLength = voltageString.length();
    int coordx = EPD_HEIGHT - GLYPH_WIDTH/2 -  (GLYPH_WIDTH*(voltageStringLength/2+1));
    int coordy = 0;
    if (Device::getInstance().deviceSettings.displayBattery) {
        if(Device::getInstance().usb_state==Device::usbState::Charging || Device::getInstance().usb_state==Device::usbState::Query) this->drawCharacter(coordx-GLYPH_WIDTH,coordy,128268);
        else if(level>20) this->drawCharacter(coordx-GLYPH_WIDTH,coordy,128267);
        else this->drawCharacter(coordx-GLYPH_WIDTH,coordy,129707);
        this->drawString(coordx,coordy,voltageString,1,false,false);
    }
    if(Device::getInstance().deviceSettings.sunlightMode) {
        this->drawCharacter(GLYPH_WIDTH,coordy,9788);
    }
}

void Renderer::clearScreenBuffer(void)
{
    for(int i=0;i<EPD_WIDTH*EPD_HEIGHT/8;i++)
    {
        this->framebuffer[i] = 0x00;
    }
}

void Renderer::clearScreenBuffer(unsigned char* framebuffer)
{
    for(int i=0;i<EPD_WIDTH*EPD_HEIGHT/8;i++)
    {
        framebuffer[i] = 0x00;
    }
}

void Renderer::drawClearScreen(bool screenID)
{
    epd.Clear(screenID);
}

void Renderer::drawScreen(bool screenID)
{
    ESP_LOGE(TAG, "drawing screenbuffer");
    epd.DisplayPicture(screenID, this->framebuffer);
}

void Renderer::drawImage(Image image,int startX, int startY)
{
    if (image.imageData.empty() || image.imageWidth == 0 || image.imageHeight == 0) {
    ESP_LOGE(TAG, "No image data available for drawing");
    return;
    }
    int w = image.imageWidth;
    int h = image.imageHeight;
    std::vector<uint8_t> pixels = image.imageData;
    for(int x=0;x<w;x++)
    {
        for(int y=0;y<h;y++)
        {
            bool pixelVal = pixels[x+y*w]>250;
            this->drawPixel(x+startX,EPD_WIDTH-y-startY,pixelVal);
        }

    }
}


void Renderer::deepSleep(void)
{
    epd.DeepSleep(true);
    epd.DeepSleep(false);
}

void Renderer::drawScreenPartial(bool screenID,int partialMinX,int partialMinY,int partialMaxX,int partialMaxY)
{
    ESP_LOGE(TAG, "drawing partial screenbuffer");
    epd.DisplayPicturePartial(screenID, this->framebuffer, partialMinX, partialMinY, partialMaxX, partialMaxY);
}

void Renderer::drawProgressBar(int y,int length,int percent)
{
    int solidSquares = (length * percent)/100;
    int greySquares = length - solidSquares;
    int startX = (EPD_HEIGHT/2)-length*GLYPH_WIDTH/4;
    int currentX = startX;
    for(int i=0;i<length;i++)
    {   
        if(i<solidSquares) currentX += this->drawCharacter(currentX,y,9608,false,false,1);
        else currentX += this->drawCharacter(currentX,y,9617,false,false,1);
    }

}