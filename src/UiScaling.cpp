// Subtitles keep a fixed pixel size, so they shrink as resolution goes up. At
// 1280x720 they are comfortably large, at 3440x1440 barely readable.
//
// The drawing object at 0x00A95650 switches between two reference frames:
//
//     SetDrawReference(this, ref):
//         if (this->[0x58] == ref) return;
//         this->[0x58] = ref;
//         if (ref) { this->[0x28] = 1/1280;  this->[0x2c] = 1/720;   }
//         else     { this->[0x28] = 1/width; this->[0x2c] = 1/height; }
//
// Normalising against 1280x720 makes an element scale with resolution;
// normalising against the real resolution keeps its pixel size. Text takes the
// second path.
//
// ref is a pointer, not a flag - the other caller passes an object address - so
// it cannot simply be forced non-zero. Instead the original runs untouched and
// only the two scale factors are corrected afterwards.
//
// The reference width follows the real aspect ratio, otherwise text is stretched
// horizontally on ultrawide displays: 1280x720 is 16:9, while 3440x1440 is 21:9,
// which works out to 1.34x too wide. Anchoring height at 720 and deriving width
// gives 1720x720 there, and exactly 1280x720 on any 16:9 mode.

#include "Common.hpp"

static safetyhook::InlineHook gSetDrawReference;

static const unsigned kScaleX = 0x28;
static const unsigned kScaleY = 0x2C;
static const float kReferenceHeight = 720.0f;

static const unsigned short* gScreenWidth = nullptr;
static const unsigned short* gScreenHeight = nullptr;

static void __fastcall SetDrawReference_Hook(void* self, int, unsigned ref)
{
    gSetDrawReference.thiscall<void>(self, ref);

    if (ref != 0 || !self)
        return;

    float referenceWidth = 1280.0f;
    if (gScreenWidth && gScreenHeight)
    {
        const unsigned short w = *gScreenWidth;
        const unsigned short h = *gScreenHeight;
        if (w && h)
            referenceWidth = kReferenceHeight * (float)w / (float)h;
    }

    const float scale = cfg::uiScale > 0.0f ? cfg::uiScale : 1.0f;

    __try
    {
        *(float*)((char*)self + kScaleX) = scale / referenceWidth;
        *(float*)((char*)self + kScaleY) = scale / kReferenceHeight;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }
}

void ApplyUiScaling()
{
    if (!cfg::uiScaling)
        return;

    const DWORD address = Find("55 8B EC 8B 45 08 39 41 58 74 49 89 41 58 85 C0", "SetDrawReference");
    if (!address)
        return;

    // Screen dimensions come out of the function's own else branch:
    //   +0x28  0F B7 15 <width>
    //   +0x32  66 A1    <height>
    const unsigned char* code = (const unsigned char*)address;
    if (code[0x28] == 0x0F && code[0x29] == 0xB7 && code[0x2A] == 0x15 &&
        code[0x32] == 0x66 && code[0x33] == 0xA1)
    {
        gScreenWidth = *(const unsigned short**)(address + 0x2B);
        gScreenHeight = *(const unsigned short**)(address + 0x34);
    }
    else
    {
        Log("UiScaling: screen dimensions not found, falling back to a fixed 1280");
    }

    gSetDrawReference = Hook((void*)address, &SetDrawReference_Hook);
    Log("UiScaling: active (scale %.2f, screen %ux%u)", cfg::uiScale,
        gScreenWidth ? *gScreenWidth : 0, gScreenHeight ? *gScreenHeight : 0);
}
