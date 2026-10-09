#include "UIElements.h"
#include "device.h"
#include <cmath>

static void drawPointedArch(Renderer* renderer, int ax0, int ay_spring, int aw, int a_arch_h, int thick, bool fill_quarry = false, int quarry_base_y = 0)
{
    float hw = aw * 0.5f;
    if (hw <= 0.0f || a_arch_h <= 0) return;

    float d = (float)(a_arch_h * a_arch_h - hw * hw) / (2.0f * hw);
    if (d < 0.0f) d = 0.0f;
    float R = hw + d;
    float left_center_x = ax0 + hw + d;
    float right_center_x = ax0 + hw - d;

    for (int dy_i = 0; dy_i <= a_arch_h; dy_i++) {
        int y = ay_spring + dy_i;
        float term = R * R - (float)(dy_i * dy_i);
        if (term >= 0.0f) {
            float dx = sqrtf(term);
            int lx = (int)roundf(left_center_x - dx);
            int rx = (int)roundf(right_center_x + dx);
            if (lx <= rx) {
                if (fill_quarry) {
                    int rel_y = y - quarry_base_y;
                    for (int qx = lx + thick; qx <= rx - thick; qx++) {
                        int rel_x = qx - (ax0 + 1);
                        if (((rel_x + rel_y) % 10 == 0 || (rel_x - rel_y) % 10 == 0) && (y % 2 == 0)) {
                            renderer->drawPixel(qx, y, false);
                        }
                    }
                }
                for (int t = 0; t < thick; t++) {
                    renderer->drawPixel(lx + t, y, false);
                    renderer->drawPixel(rx - t, y, false);
                }
            }
        }
    }
}

static void drawSingleLancet(Renderer* renderer, int x0, int y0, int w, int spring_h, int arch_h, int thick)
{
    for (int y = y0; y <= y0 + spring_h; y++) {
        for (int t = 0; t < thick; t++) {
            renderer->drawPixel(x0 + t, y, false);
            renderer->drawPixel(x0 + w - t, y, false);
        }
    }
    drawPointedArch(renderer, x0, y0 + spring_h, w, arch_h, thick);
}

static void drawLancetTriplet(Renderer* renderer, int x0, int y0)
{
    const int total_w = 50;
    for (int x = x0 - 2; x <= x0 + total_w + 2; x++) {
        renderer->drawPixel(x, y0, false);
        renderer->drawPixel(x, y0 + 1, false);
    }

    // Flanking slender pointed lancets + taller double-moulded center lancet
    drawSingleLancet(renderer, x0, y0 + 1, 14, 10, 15, 1);
    drawSingleLancet(renderer, x0 + 36, y0 + 1, 14, 10, 15, 1);
    drawSingleLancet(renderer, x0 + 16, y0 + 1, 18, 13, 21, 2);
    drawPointedArch(renderer, x0 + 19, y0 + 14, 12, 16, 1);

    // Quatrefoil diamond accent inside the central lancet light
    int cx = x0 + 25;
    int cy = y0 + 18;
    renderer->drawPixel(cx, cy + 2, false);
    renderer->drawPixel(cx, cy - 2, false);
    renderer->drawPixel(cx - 2, cy, false);
    renderer->drawPixel(cx + 2, cy, false);

    // Finial cross crowning the tall central lancet
    int apex_y = y0 + 35;
    for (int dy = 1; dy <= 5; dy++) {
        renderer->drawPixel(cx, apex_y + dy, false);
    }
    renderer->drawPixel(cx - 2, apex_y + 3, false);
    renderer->drawPixel(cx - 1, apex_y + 3, false);
    renderer->drawPixel(cx + 1, apex_y + 3, false);
    renderer->drawPixel(cx + 2, apex_y + 3, false);
}

static void drawCathedralWindow(Renderer* renderer, int x0, int y0)
{
    const int w = 50;
    const int cx = x0 + w / 2;
    const int spring_h = 30;
    const int arch_h = 42;
    const int spring_y = y0 + spring_h;

    // Stepped moulded stone sill
    for (int x = x0 - 3; x <= x0 + w + 3; x++) {
        renderer->drawPixel(x, y0, false);
        renderer->drawPixel(x, y0 + 1, false);
    }
    for (int x = x0; x <= x0 + w; x++) {
        renderer->drawPixel(x, y0 + 4, false);
    }

    const int left_light_x0 = x0 + 4;
    const int left_light_x1 = cx - 1;
    const int right_light_x0 = cx + 1;
    const int right_light_x1 = x0 + w - 4;
    const int quarry_base_y = y0 + 4;

    for (int y = y0 + 1; y <= spring_y; y++) {
        renderer->drawPixel(x0, y, false);
        renderer->drawPixel(x0 + 1, y, false);
        renderer->drawPixel(x0 + w, y, false);
        renderer->drawPixel(x0 + w - 1, y, false);

        renderer->drawPixel(left_light_x0, y, false);
        renderer->drawPixel(right_light_x1, y, false);

        renderer->drawPixel(cx - 1, y, false);
        renderer->drawPixel(cx, y, false);
        renderer->drawPixel(cx + 1, y, false);

        if (y > quarry_base_y && (y % 2 == 0)) {
            int rel_y = y - quarry_base_y;
            for (int lx = left_light_x0 + 1; lx < left_light_x1; lx++) {
                int rel_x = lx - (left_light_x0 + 1);
                if ((rel_x + rel_y) % 10 == 0 || (rel_x - rel_y) % 10 == 0) {
                    renderer->drawPixel(lx, y, false);
                }
            }
            for (int rx = right_light_x0 + 1; rx < right_light_x1; rx++) {
                int rel_x = rx - (right_light_x0 + 1);
                if ((rel_x + rel_y) % 10 == 0 || (rel_x - rel_y) % 10 == 0) {
                    renderer->drawPixel(rx, y, false);
                }
            }
        }
    }

    // Outer pointed Gothic arch (2px) & inner pointed arch (1px)
    drawPointedArch(renderer, x0, spring_y, w, arch_h, 2);
    drawPointedArch(renderer, left_light_x0, spring_y, right_light_x1 - left_light_x0, arch_h - 6, 1);

    // Twin steeply pointed sub-lancet lights with stained-glass quarries up to their tips
    drawPointedArch(renderer, left_light_x0, spring_y, left_light_x1 - left_light_x0, 19, 1, true, quarry_base_y);
    drawPointedArch(renderer, right_light_x0, spring_y, right_light_x1 - right_light_x0, 19, 1, true, quarry_base_y);

    // Quatrefoil rose oculus in the pointed upper tympanum
    const int oculus_r = 6;
    const int oculus_y = spring_y + 25;
    for (int dy = -oculus_r - 1; dy <= oculus_r + 1; dy++) {
        for (int dx = -oculus_r - 1; dx <= oculus_r + 1; dx++) {
            float dist = sqrtf((float)(dx * dx + dy * dy));
            if (fabsf(dist - (float)oculus_r) <= 0.65f) {
                renderer->drawPixel(cx + dx, oculus_y + dy, false);
            }
        }
    }
    renderer->drawPixel(cx, oculus_y + oculus_r - 2, false);
    renderer->drawPixel(cx, oculus_y - oculus_r + 2, false);
    renderer->drawPixel(cx + oculus_r - 2, oculus_y, false);
    renderer->drawPixel(cx - oculus_r + 2, oculus_y, false);
    renderer->drawPixel(cx, oculus_y, false);

    // Finial cross crowning the cathedral arch
    int apex_y = spring_y + arch_h;
    for (int dy = 1; dy <= 6; dy++) {
        renderer->drawPixel(cx, apex_y + dy, false);
    }
    for (int dx = -2; dx <= 2; dx++) {
        renderer->drawPixel(cx + dx, apex_y + 4, false);
    }
}

static void drawGothicMenuBackground(Renderer* renderer)
{
    const int W = EPD_HEIGHT; // 480 (horizontal X axis)
    const int H = EPD_WIDTH;  // 648 (vertical Y axis)

    // 1. Double stone border framing the screen perimeter while leaving the middle section white
    for (int x = 2; x < W - 2; x++) {
        renderer->drawPixel(x, 2, false);
        renderer->drawPixel(x, 3, false);
        renderer->drawPixel(x, H - 3, false);
        renderer->drawPixel(x, H - 4, false);
    }
    for (int x = 7; x < W - 7; x++) {
        renderer->drawPixel(x, 7, false);
        renderer->drawPixel(x, H - 8, false);
    }
    for (int y = 2; y < H - 2; y++) {
        renderer->drawPixel(2, y, false);
        renderer->drawPixel(3, y, false);
        renderer->drawPixel(W - 3, y, false);
        renderer->drawPixel(W - 4, y, false);
    }
    for (int y = 7; y < H - 7; y++) {
        renderer->drawPixel(7, y, false);
        renderer->drawPixel(W - 8, y, false);
    }

    // Corner square stone bosses where inner perimeter lines intersect
    const int cornerX[2] = { 7, W - 10 };
    const int cornerY[2] = { 7, H - 10 };
    for (int ix = 0; ix < 2; ix++) {
        for (int iy = 0; iy < 2; iy++) {
            renderer->drawSquare(cornerX[ix], cornerY[iy], 3, 3, false);
        }
    }

    // 2. Upper corners: Stepped 3-lancet Gothic clerestory triplets flanking the header
    drawLancetTriplet(renderer, 15, 595);
    drawLancetTriplet(renderer, W - 15 - 50 - 1, 595);

    // 3. Lower corners: Two-light Gothic cathedral windows with quatrefoil rose & stained-glass quarries
    drawCathedralWindow(renderer, 15, 13);
    drawCathedralWindow(renderer, W - 15 - 50 - 1, 13);

    // 4. Horizontal Gothic friezes connecting the corner windows
    for (int x = 70; x < W - 70; x++) {
        renderer->drawPixel(x, 595, false);
    }
    const int friezeFinials[3] = { 70, W / 2, W - 71 };
    for (int i = 0; i < 3; i++) {
        int fx = friezeFinials[i];
        for (int d = -3; d <= 3; d++) {
            renderer->drawPixel(fx + d, 595, false);
            renderer->drawPixel(fx, 595 + d, false);
        }
    }

    for (int x = 70; x < W - 70; x++) {
        renderer->drawPixel(x, 13, false);
        if (x % 2 == 0) {
            renderer->drawPixel(x, 17, false);
        }
    }
    for (int i = 0; i < 3; i++) {
        int fx = friezeFinials[i];
        for (int dy = -4; dy <= 4; dy++) {
            int span = 4 - abs(dy);
            for (int dx = -span; dx <= span; dx++) {
                renderer->drawPixel(fx + dx, 15 + dy, (abs(dx) + abs(dy)) < 2);
            }
        }
    }
}

void MenuElement::renderElement()
{
    renderer->clearScreenBuffer();
    drawGothicMenuBackground(renderer);

    // Draw return arrow plaque beside bottom-left Gothic window if not main menu
    if (elementName != "Diptyx E-reader") {
        renderer->drawGothicBox(82, 22, 68, 38, false);
        renderer->drawCharacter(98, 22, 8617, false, false, 2, true);
    }

    // Draw title centered
    renderer->drawString(
        (EPD_HEIGHT - elementName.length() * GLYPH_WIDTH) / 2,
        EPD_WIDTH - GLYPH_HEIGHT * 3,
        elementName,
        2, true, false, true
    );

    const int maxVisible = MaxElementsPerPage; 
    int currentPage = selectedChildIndex / maxVisible;
    int startIndex = currentPage * maxVisible;
    int endIndex = std::min(startIndex + maxVisible, (int)children.size());
    int lowestY = 0;

    // Draw visible children
    for (int i = startIndex; i < endIndex; i++) {
        int localIndex = i - startIndex; // position within page
        int y = EPD_WIDTH - ((localIndex + 1.5) * 5 * GLYPH_HEIGHT);
        lowestY = y;
        if(startIndex>0) y -= 2 * GLYPH_HEIGHT;

        children[i]->renderIcon(y,selectedChildIndex == i);

        // Draw arrow for currently selected item
    }

    // Show up arrow if there are items above
    if (startIndex > 0) {
        renderer->drawCharacter(
            EPD_HEIGHT / 2- GLYPH_WIDTH,       // x pos
            EPD_WIDTH - GLYPH_HEIGHT * 5, // near top
            8593, // ↑
            true, false, 2, true
        );
    }

    // Show down arrow if there are items below
    if (endIndex < children.size()) {
        int yOffset = 0;
        if(startIndex > 0) yOffset = 2*GLYPH_HEIGHT;
        renderer->drawCharacter(
            EPD_HEIGHT / 2 - GLYPH_WIDTH,       // x pos
            lowestY-3*GLYPH_HEIGHT - yOffset,         // near bottom
            8595, // ↓
            true, false, 2, true
        );
    }

    
}

void UIElement::renderIcon(int y, bool highlight)
{
    renderer->drawTextBox(
        y,elementName,true,elementDescription,false,elementExtraDescription,false,highlight);
    if (highlight) {
        renderer->drawCharacter(EPD_HEIGHT - 60, y + 0.5 * GLYPH_HEIGHT,8594,true, false, 3, false);
    }
}

void MenuElement::renderIcon(int y, bool highlight)
{
    renderer->drawTextBox(
        y,elementName,true,elementDescription,false,std::string(""),false,highlight);
    if (highlight) {
        renderer->drawCharacter(EPD_HEIGHT - 60, y + 0.5 * GLYPH_HEIGHT,8594,true, false, 3, false);
    }
}

void BookElement::renderIcon(int y, bool highlight)
{
    if(!Device::getInstance().deviceSettings.showPagePercentage) this->elementDescription = (book->totalPageCount > 0 ? std::to_string((100*(book->currentPage))/(book->totalPageCount-2)) + "%" : "?%");
    else this->elementDescription = std::to_string(book->currentPage+1) + "/" +
                    (book->totalPageCount > 0 ? std::to_string(book->totalPageCount) : "?");
    this->elementExtraDescription = book->favorite?std::string("❤"):std::string("");
    renderer->drawTextBox(
        y,elementName,true,elementDescription,false,elementExtraDescription,false,highlight);
    if (highlight) {
        renderer->drawCharacter(EPD_HEIGHT - 60, y + 0.5 * GLYPH_HEIGHT,8594,true, false, 3, false);
    }
}


void ValueElement::renderIcon(int y, bool highlight)
{
    renderer->drawTextBox(y,elementName,true,elementDescription,false,std::string(""),false,highlight);
    
    std::string valueString = this->valueDescriptions[selectedValueIndex];
    if(selected)
    {
        if(selectedValueIndex > 0 || infinitescrolling==true) valueString = "< " + valueString;
        if(selectedValueIndex<values.size()-1  || infinitescrolling==true) valueString = valueString + " >";
    }
    int stringWidth = (valueString.length()+1) * GLYPH_WIDTH/2;
    int textStartX = EPD_HEIGHT - 48 - stringWidth;
    if(!selected) renderer->drawString(textStartX, y + 1.5 * GLYPH_HEIGHT,valueString, 1,true, false, !highlight);
    else
    {
        renderer->drawSquare(textStartX,y + 1.5 * GLYPH_HEIGHT,stringWidth,GLYPH_HEIGHT,true);
        renderer->drawString(textStartX, y + 1.5 * GLYPH_HEIGHT,valueString, 1,true, false, true);
    }
    drawValueYPos = y + 1.5 * GLYPH_HEIGHT;
    drawValueXPos = EPD_HEIGHT - 48 - (maxStringWidth+1)*GLYPH_WIDTH/2;
}

void ValueElement::writeValue()
{
    if(!this->values.empty()) *(this->valueAdress) = this->values[this->selectedValueIndex];
}

void ValueElement::ReadValue()
{
    for(int i=0;i<values.size();i++)
    {
        if(*(this->valueAdress) == this->values[i]) this->selectedValueIndex = i;
    }
}


void MenuElement::enterElement()
{
    
}
