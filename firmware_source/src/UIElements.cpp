#include "UIElements.h"
#include "device.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static inline float sdSegment(float px, float py, float ax, float ay, float bx, float by)
{
    float pax = px - ax, pay = py - ay;
    float bax = bx - ax, bay = by - ay;
    float denom = bax * bax + bay * bay;
    float h = (denom > 1e-6f) ? (pax * bax + pay * bay) / denom : 0.0f;
    if (h < 0.0f) h = 0.0f;
    if (h > 1.0f) h = 1.0f;
    float dx = pax - bax * h;
    float dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy);
}

static inline float sdCircleOutline(float px, float py, float cx, float cy, float r)
{
    return fabsf(sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r);
}

static inline float sdCircleSigned(float px, float py, float cx, float cy, float r)
{
    return sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r;
}

// Unsigned distance to a pointed 2-arc Gothic lancet arch springing from (x_mid +- hw, y_spring) to apex (x_mid, y_spring + arch_h)
static float sdPointedArchRib(float px, float py, float x_mid, float y_spring, float hw, float arch_h)
{
    if (hw <= 1.0f || arch_h <= 1.0f) return 1e5f;
    float dx_sym = fabsf(px - x_mid);
    float x_c = (hw * hw - arch_h * arch_h) / (2.0f * hw);
    float R = hw - x_c;
    float rx = dx_sym - x_c;
    float ry = py - y_spring;
    if (ry <= 0.0f) {
        return sqrtf((dx_sym - hw) * (dx_sym - hw) + ry * ry);
    }
    if (rx * arch_h < (-x_c) * ry) {
        return sqrtf(dx_sym * dx_sym + (py - (y_spring + arch_h)) * (py - (y_spring + arch_h)));
    }
    return fabsf(sqrtf(rx * rx + ry * ry) - R);
}

static inline float sdPointedArchRibHoriz(float px, float py, float x_spring, float y_mid, float hh, float arch_w)
{
    return sdPointedArchRib(py, px, y_mid, x_spring, hh, arch_w);
}

namespace {

struct VerticalLancetBay {
    float x_left;
    float x_right;
    float top_left;
    float top_right;
    float y_spring;
    float arch_h;
    bool cusped;
};

struct HorizLancetBay {
    float y_bot;
    float y_top;
    float right_bot;
    float right_top;
    float x_spring;
    float arch_w;
    bool cusped;
};

struct FoilLobe {
    float cx;
    float cy;
};

struct CornerSpandrelLayout {
    int U;
    int V;
    bool grand;
    float R_norm;
    float C_norm;
    float oc_r;
    float lobe_r;
    float core_r;
    float r_ap;
    std::vector<FoilLobe> foilLobes;
    std::vector<VerticalLancetBay> hWingBays;
    std::vector<HorizLancetBay> vWingBays;

    float archV(float u) const {
        if (u < 0.0f) return (float)V;
        float nu = u / (float)U;
        float term = R_norm * R_norm - (C_norm - nu) * (C_norm - nu);
        if (term <= 0.0f) return 0.0f;
        float nv = C_norm - sqrtf(term);
        return (nv > 0.0f) ? (nv * (float)V) : 0.0f;
    }

    float archU(float v) const {
        if (v < 0.0f) return (float)U;
        float nv = v / (float)V;
        float term = R_norm * R_norm - (C_norm - nv) * (C_norm - nv);
        if (term <= 0.0f) return 0.0f;
        float nu = C_norm - sqrtf(term);
        return (nu > 0.0f) ? (nu * (float)U) : 0.0f;
    }

    float sdArchSigned(float u, float v) const {
        float nx = (C_norm * (float)U - u) / (float)U;
        float ny = (C_norm * (float)V - v) / (float)V;
        float q = sqrtf(nx * nx + ny * ny);
        if (q < 1e-5f) return 1e5f;
        float f = R_norm - q;
        float gx = nx / (q * (float)U);
        float gy = ny / (q * (float)V);
        return f / sqrtf(gx * gx + gy * gy);
    }
};

static CornerSpandrelLayout buildCornerSpandrelLayout(int U, int V)
{
    CornerSpandrelLayout S;
    S.U = U;
    S.V = V;
    S.grand = (U >= 140 && V >= 140);
    S.R_norm = 2.05f;
    S.C_norm = 0.5f * (1.0f + sqrtf(2.0f * S.R_norm * S.R_norm - 1.0f));

    // Binary-search primary rose circle tangent to u = 0, v = 0, and inner arch order (d_arch = -5.5)
    float lo_r = 6.0f, hi_r = (float)std::min(U, V) * 0.45f;
    for (int iter = 0; iter < 24; iter++) {
        float mid_r = 0.5f * (lo_r + hi_r);
        float d_to_inner_arch = -S.sdArchSigned(mid_r, mid_r) - 5.5f;
        if (mid_r < d_to_inner_arch) lo_r = mid_r;
        else hi_r = mid_r;
    }
    S.oc_r = 0.5f * (lo_r + hi_r);
    S.r_ap = 0.1715729f * S.oc_r; // Exact Apollonius corner tangent circle radius

    // Precompute cusped foil lobe centers once outside the pixel loop
    int lobes = S.grand ? 6 : 4;
    float phase = S.grand ? 0.261799f : 0.785398f;
    float inner_r = S.oc_r - 1.5f;
    float ratio = (lobes == 4) ? 0.55f : 0.495f;
    S.lobe_r = inner_r * ratio;
    S.core_r = (lobes <= 4) ? (inner_r * 0.44f) : (inner_r * 0.53f);
    float c_dist = inner_r - S.lobe_r;
    const float two_pi = 6.283185307f;
    for (int k = 0; k < lobes; k++) {
        float ang = phase + two_pi * (float)k / (float)lobes;
        S.foilLobes.push_back({S.oc_r + c_dist * cosf(ang), S.oc_r + c_dist * sinf(ang)});
    }

    // Precompute horizontal wing Perpendicular Gothic mullioned lancet bays
    float x_start = 2.0f * S.oc_r;
    float x_end = (float)U - 24.0f;
    int n_hbays = S.grand ? 3 : 2;
    if (x_end - x_start >= 22.0f) {
        float bay_w = (x_end - x_start) / (float)n_hbays;
        for (int k = 0; k < n_hbays; k++) {
            float xl = x_start + k * bay_w;
            float xr = xl + bay_w;
            float top_y = S.archV(0.5f * (xl + xr)) - 6.0f;
            if (top_y >= 11.0f) {
                float ah = std::min(bay_w * 0.72f, top_y * 0.52f);
                S.hWingBays.push_back({xl, xr, S.archV(xl) - 5.5f, S.archV(xr) - 5.5f, top_y - ah, ah, S.grand && (top_y >= 34.0f)});
            }
        }
    }

    // Precompute vertical wing transomed Gothic lancet bays climbing the side jamb
    float y_start = 2.0f * S.oc_r;
    float y_end = (float)V - 24.0f;
    int n_vbays = S.grand ? 3 : ((V >= 76) ? 1 : 0);
    if (n_vbays > 0 && y_end - y_start >= 16.0f) {
        float bay_h = (y_end - y_start) / (float)n_vbays;
        for (int k = 0; k < n_vbays; k++) {
            float yb = y_start + k * bay_h;
            float yt = yb + bay_h;
            float right_x = S.archU(0.5f * (yb + yt)) - 6.0f;
            if (right_x >= 11.0f) {
                float aw = std::min(bay_h * 0.72f, right_x * 0.52f);
                S.vWingBays.push_back({yb, yt, S.archU(yb) - 5.5f, S.archU(yt) - 5.5f, right_x - aw, aw, S.grand && (right_x >= 34.0f)});
            }
        }
    }

    return S;
}

static float evalSpandrelStoneDistance(const CornerSpandrelLayout& S, float u, float v, float d_arch)
{
    float d_stone = 1e5f;

    // 1. Perimeter stone frame centerlines (u = 0 along jamb, v = 0 along lintel/sill)
    if (v >= -1.5f) d_stone = std::min(d_stone, fabsf(u));
    if (u >= -1.5f) d_stone = std::min(d_stone, fabsf(v));

    if (u < -0.5f || v < -0.5f) return d_stone;

    // 2. Double-order sweeping Gothic window arch (d_arch = 0 outer order + d_arch = -5.5 inner order)
    if (u >= 0.0f && v >= 0.0f) {
        d_stone = std::min(d_stone, fabsf(d_arch));
        if (d_arch <= 0.0f) {
            d_stone = std::min(d_stone, fabsf(d_arch + 5.5f));
        }
    }

    if (d_arch >= -4.5f) return d_stone;

    // 3. Primary tangent circular rose medallion + precomputed cusped Gothic foil lobes
    float d_center = sdCircleSigned(u, v, S.oc_r, S.oc_r, S.oc_r);
    d_stone = std::min(d_stone, fabsf(d_center));
    if (d_center < 0.0f) {
        float d_foil = d_center + (S.oc_r - S.core_r);
        for (const auto& lobe : S.foilLobes) {
            float dl = sdCircleSigned(u, v, lobe.cx, lobe.cy, S.lobe_r);
            if (dl < d_foil) d_foil = dl;
        }
        d_stone = std::min(d_stone, fabsf(d_foil));
    }

    // 4. Exact Apollonius corner circle tucked in the 90-degree corner vertex pocket
    if (S.r_ap >= 3.5f) {
        d_stone = std::min(d_stone, sdCircleOutline(u, v, S.r_ap, S.r_ap, S.r_ap));
    }

    // 5. Horizontal wing Perpendicular mullions & cusped lancet arches
    for (const auto& bay : S.hWingBays) {
        d_stone = std::min(d_stone, sdSegment(u, v, bay.x_left, 0.0f, bay.x_left, bay.top_left));
        d_stone = std::min(d_stone, sdSegment(u, v, bay.x_right, 0.0f, bay.x_right, bay.top_right));
        float xm = 0.5f * (bay.x_left + bay.x_right);
        float hw = 0.5f * (bay.x_right - bay.x_left);
        d_stone = std::min(d_stone, sdPointedArchRib(u, v, xm, bay.y_spring, hw, bay.arch_h));

        if (bay.cusped && u > bay.x_left && u < bay.x_right && v >= bay.y_spring - 1.0f && v < bay.y_spring + bay.arch_h) {
            float cr = hw * 0.44f;
            float d_c1 = sdCircleSigned(u, v, bay.x_left + cr, bay.y_spring, cr);
            float d_c2 = sdCircleSigned(u, v, bay.x_right - cr, bay.y_spring, cr);
            float d_top = sdCircleSigned(u, v, xm, bay.y_spring + bay.arch_h * 0.48f, hw * 0.46f);
            d_stone = std::min(d_stone, fabsf(std::min(d_c1, std::min(d_c2, d_top))));
        }
    }

    // 6. Vertical wing transoms & cusped lancet arches climbing the jamb
    for (const auto& bay : S.vWingBays) {
        d_stone = std::min(d_stone, sdSegment(u, v, 0.0f, bay.y_bot, bay.right_bot, bay.y_bot));
        d_stone = std::min(d_stone, sdSegment(u, v, 0.0f, bay.y_top, bay.right_top, bay.y_top));
        float ym = 0.5f * (bay.y_bot + bay.y_top);
        float hh = 0.5f * (bay.y_top - bay.y_bot);
        d_stone = std::min(d_stone, sdPointedArchRibHoriz(u, v, bay.x_spring, ym, hh, bay.arch_w));

        if (bay.cusped && v > bay.y_bot && v < bay.y_top && u >= bay.x_spring - 1.0f && u < bay.x_spring + bay.arch_w) {
            float cr = hh * 0.44f;
            float d_c1 = sdCircleSigned(u, v, bay.x_spring, bay.y_bot + cr, cr);
            float d_c2 = sdCircleSigned(u, v, bay.x_spring, bay.y_top - cr, cr);
            float d_top = sdCircleSigned(u, v, bay.x_spring + bay.arch_w * 0.48f, ym, hh * 0.46f);
            d_stone = std::min(d_stone, fabsf(std::min(d_c1, std::min(d_c2, d_top))));
        }
    }

    return d_stone;
}

// Rasterizes a symmetric left+right pair of corner spandrels together so each (u, v) distance is evaluated only once
static void drawGothicCornerPair(Renderer* renderer, int W, int H, int U, int V, bool flipY)
{
    const CornerSpandrelLayout S = buildCornerSpandrelLayout(U, V);

    for (int iu = -3; iu <= U + 4; iu++) {
        int sx_left = 5 + iu;
        int sx_right = W - 6 - iu;
        for (int iv = -3; iv <= V + 4; iv++) {
            float u = (float)iu;
            float v = (float)iv;
            float d_arch = S.sdArchSigned(u, v);

            if (u > 3.6f && v > 3.6f && d_arch > 3.6f) continue;

            float d_stone = evalSpandrelStoneDistance(S, u, v, d_arch);
            bool pixel_white = (d_stone <= 1.55f) || (d_stone > 3.35f);

            int sy = flipY ? (H - 6 - iv) : (5 + iv);
            renderer->drawPixel(sx_left, sy, pixel_white);
            renderer->drawPixel(sx_right, sy, pixel_white);
        }
    }
}

static const unsigned char* getCachedMenuBackground(Renderer* renderer, bool isMainMenu)
{
    const size_t fbSize = (EPD_WIDTH * EPD_HEIGHT) / 8;
    static unsigned char* mainBgCache = nullptr;
    static unsigned char* subBgCache = nullptr;

    unsigned char*& targetCache = isMainMenu ? mainBgCache : subBgCache;
    if (targetCache != nullptr) {
        return targetCache;
    }

    targetCache = (unsigned char*)calloc(fbSize, sizeof(unsigned char));
    if (!targetCache) {
        return nullptr;
    }

    unsigned char* prevFb = renderer->framebuffer;
    renderer->framebuffer = targetCache;

    const int W = EPD_HEIGHT; // 480 (horizontal X axis)
    const int H = EPD_WIDTH;  // 648 (vertical Y axis)

    // 1. Continuous double-moulded Gothic stone border framing the screen perimeter
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

    // 2. Upper corners: Sweeping Gothic window corner tracery spandrels flanking the title
    drawGothicCornerPair(renderer, W, H, 106, 52, true);

    // 3. Lower corners: Full architectural Gothic window corner tracery spandrels
    if (isMainMenu) {
        drawGothicCornerPair(renderer, W, H, 192, 204, false);
    } else {
        drawGothicCornerPair(renderer, W, H, 106, 82, false);
    }

    renderer->framebuffer = prevFb;
    return targetCache;
}

// Restores the background slice behind a single menu row directly from the cached 1-bit background buffer
static void restoreMenuRowBackground(unsigned char* dstFb, const unsigned char* cachedBg, int rowY)
{
    const int stride = EPD_WIDTH / 8;
    int topFlippedY = (EPD_WIDTH - 1) - (rowY + 4 * GLYPH_HEIGHT);
    int botFlippedY = (EPD_WIDTH - 1) - (rowY - 5);
    int byteStart = std::max(0, topFlippedY / 8);
    int byteEnd = std::min(stride - 1, botFlippedY / 8);
    size_t copyLen = (size_t)(byteEnd - byteStart + 1);

    // Menu buttons + drop shadows span x = 14 .. EPD_HEIGHT - 10; in flipped X that is col = 9 .. EPD_HEIGHT - 14
    for (int x = 12; x <= EPD_HEIGHT - 10; x++) {
        int flippedX = (EPD_HEIGHT - 1) - x;
        size_t offset = (size_t)(flippedX * stride + byteStart);
        memcpy(dstFb + offset, cachedBg + offset, copyLen);
    }
}

} // namespace

void MenuElement::renderElement()
{
    const bool isMainMenu = (elementName == "Diptyx E-reader");
    const unsigned char* cachedBg = getCachedMenuBackground(renderer, isMainMenu);

    const int maxVisible = MaxElementsPerPage;
    int currentPage = selectedChildIndex / maxVisible;
    int startIndex = currentPage * maxVisible;
    int endIndex = std::min(startIndex + maxVisible, (int)children.size());

    auto rowYForIndex = [&](int idx) -> int {
        int localIndex = idx - startIndex;
        int y = EPD_WIDTH - (int)((localIndex + 1.5f) * 5 * GLYPH_HEIGHT);
        if (startIndex > 0) y -= 2 * GLYPH_HEIGHT;
        return y;
    };

    bool activeItemEditing = false;
    if (selectedChildIndex >= 0 && selectedChildIndex < (int)children.size() &&
        children[selectedChildIndex]->getType() == UIElementType::Value) {
        activeItemEditing = std::static_pointer_cast<ValueElement>(children[selectedChildIndex])->selected;
    }

    static const MenuElement* lastMenu = nullptr;
    static unsigned char* lastFb = nullptr;
    static int lastPage = -1;
    static size_t lastChildCount = 0;
    static int lastSelectedIdx = -1;
    static bool lastEditing = false;

    // Fast path: when moving selection within the same menu & page without toggling value edit mode,
    // only restore and redraw the previously selected row and newly selected row!
    if (cachedBg != nullptr &&
        lastMenu == this &&
        lastFb == renderer->framebuffer &&
        lastPage == currentPage &&
        lastChildCount == children.size() &&
        !lastEditing && !activeItemEditing &&
        lastSelectedIdx >= startIndex && lastSelectedIdx < endIndex &&
        lastSelectedIdx != selectedChildIndex) {
        int prevY = rowYForIndex(lastSelectedIdx);
        int currY = rowYForIndex(selectedChildIndex);
        restoreMenuRowBackground(renderer->framebuffer, cachedBg, prevY);
        restoreMenuRowBackground(renderer->framebuffer, cachedBg, currY);
        children[lastSelectedIdx]->renderIcon(prevY, false);
        children[selectedChildIndex]->renderIcon(currY, true);
        lastSelectedIdx = selectedChildIndex;
        return;
    }

    if (cachedBg != nullptr) {
        memcpy(renderer->framebuffer, cachedBg, (EPD_WIDTH * EPD_HEIGHT) / 8);
    } else {
        renderer->clearScreenBuffer();
    }

    // Draw return arrow plaque beside bottom-left Gothic corner tracery if not main menu
    if (!isMainMenu) {
        renderer->drawGothicBox(118, 20, 68, 38, false);
        renderer->drawCharacter(134, 20, 8617, false, false, 2, true);
    }

    // Draw title centered
    renderer->drawString(
        (EPD_HEIGHT - elementName.length() * GLYPH_WIDTH) / 2,
        EPD_WIDTH - GLYPH_HEIGHT * 3,
        elementName,
        2, true, false, true
    );

    int lowestY = 0;

    // Draw visible children
    for (int i = startIndex; i < endIndex; i++) {
        int y = rowYForIndex(i);
        lowestY = y;
        children[i]->renderIcon(y, selectedChildIndex == i);
    }

    // Show up arrow if there are items above
    if (startIndex > 0) {
        renderer->drawCharacter(
            EPD_HEIGHT / 2 - GLYPH_WIDTH, // x pos
            EPD_WIDTH - GLYPH_HEIGHT * 5, // near top
            8593, // ↑
            true, false, 2, true
        );
    }

    // Show down arrow if there are items below
    if (endIndex < (int)children.size()) {
        int yOffset = (startIndex > 0) ? (2 * GLYPH_HEIGHT) : 0;
        renderer->drawCharacter(
            EPD_HEIGHT / 2 - GLYPH_WIDTH,       // x pos
            lowestY - 3 * GLYPH_HEIGHT - yOffset, // near bottom
            8595, // ↓
            true, false, 2, true
        );
    }

    lastMenu = this;
    lastFb = renderer->framebuffer;
    lastPage = currentPage;
    lastChildCount = children.size();
    lastSelectedIdx = selectedChildIndex;
    lastEditing = activeItemEditing;
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
    if (!Device::getInstance().deviceSettings.showPagePercentage) {
        this->elementDescription = (book->totalPageCount > 2 ? std::to_string((100 * (book->currentPage)) / (book->totalPageCount - 2)) + "%" : "100%");
    } else {
        this->elementDescription = std::to_string(book->currentPage + 1) + "/" +
                    (book->totalPageCount > 0 ? std::to_string(book->totalPageCount) : "?");
    }
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
