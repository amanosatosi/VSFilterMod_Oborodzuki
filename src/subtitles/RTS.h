/*
 *	Copyright (C) 2003-2006 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#pragma once

#include <memory>
#include "STS.h"
#include "Rasterizer.h"
#include "NumericExpression.h"
#include "AssMotion.h"
#include "RenderGeometry.h"
#include "ShapedText.h"
#include "Furigana.h"
#ifdef _VSMOD
#include "ColumnLayout.h"
#endif
#include "..\SubPic\ISubPic.h"

class CMyFont : public CFont
{
public:
    CMyFont(STSStyle& style);
    CMyFont(const LOGFONT& font);
};

class CPolygon;

class CWord : public Rasterizer
{
    bool m_fDrawn;
    CPoint m_p;
    
#if defined (_VSMOD) && defined(_LUA)
    void CustomTransform(CPoint org, CString F, int Layer);
#endif

    void Transform(CPoint org);

    bool CreateOpaqueBox();
#ifdef _VSMOD
    struct NativeOutlineMasks;
    std::unique_ptr<NativeOutlineMasks> m_nativeMasks;
    bool CreateNativeOutline();
    bool RasterizeNativeOutline(CPoint p);
    std::unique_ptr<CWord> m_undecoratedGlyphs;
    std::unique_ptr<Rasterizer> m_decorationMask;
    virtual std::unique_ptr<CWord> CreateUndecoratedWord() { return {}; }
    bool RasterizeDecorations(CPoint p, CPoint org);
    bool NeedsDecorationMask() const;
#endif

protected:
    double m_scalex, m_scaley;
    CStringW m_str;
#ifdef _VSMOD
    std::unique_ptr<CRect> m_distortionDomain;
    struct LayoutPlacement
    {
        CPoint offset;
        double cosine = 1, sine = 0, center = 0, baseline = 0;
        int clipStart = INT_MIN, clipEnd = INT_MAX;
        bool rotate = false;
    };
    std::unique_ptr<LayoutPlacement> m_layoutPlacement;
    void RotateLayoutOutline();
    bool m_projectedPath = false;
    bool m_rawPathReady = false;
#endif

    void MeasureSelectedFont();
    virtual bool CreatePath() = 0;

public:
    bool m_fWhiteSpaceChar, m_fLineBreak;

    STSStyle m_style;

    CPolygon* m_pOpaqueBox;

    bool isOpaqueBox;

    int m_ktype, m_kstart, m_kend;

    int m_width, m_ascent, m_descent;
    int m_logicalLine = 0;

    CWord(STSStyle& style, CStringW str, int ktype, int kstart, int kend, double scalex, double scaley); // str[0] = 0 -> m_fLineBreak = true (in this case we only need and use the height of m_font from the whole class)
    CWord(CWord& word);
    virtual ~CWord();

    virtual CWord* Copy() = 0;
    virtual bool Append(CWord* w);
    bool NeedsPreparation(CPoint p) const
    {
        return !m_fDrawn || (m_p.x & 7) != (p.x & 7) || (m_p.y & 7) != (p.y & 7);
    }

#ifdef _VSMOD
    CPoint LayoutPosition(CPoint flat) const;
    void PlaceLayout(CPoint offset, double cosine, double sine, double center, double baseline);
    void ClipRubyCoverage(int start, int end)
    {
        m_layoutPlacement->clipStart = start;
        m_layoutPlacement->clipEnd = end;
    }
    CRect RubyClip(const CRect& clip, int x, int y) const;
    void OffsetLayout(CPoint offset);
    bool RawOutlineBounds(CRect& bounds);
    void SetDistortionDomain(const CRect& domain) { m_distortionDomain = std::make_unique<CRect>(domain); }
    void AddWarpedBounds(CPoint position, RenderGeometry::Point& min, RenderGeometry::Point& max) const;
    bool m_verticalWord = false;
    bool m_isRubyReading = false;
    unsigned m_rubyGroup = 0;
    unsigned m_columnCell = 0;
    int VerticalPaintSplit(int y, double progress) const
    {
        const int top = m_layoutPlacement && m_layoutPlacement->clipStart != INT_MIN ?
            m_layoutPlacement->clipStart : mOffsetY;
        const int bottom = m_layoutPlacement && m_layoutPlacement->clipEnd != INT_MAX ?
            m_layoutPlacement->clipEnd : mOffsetY + mOverlayHeight * 8;
        return (y + top + int(progress * (bottom - top)) + 4) >> 3;
    }
    int HorizontalRubyPaintSplit(int fallback, double progress) const
    {
        if (!m_layoutPlacement || (m_layoutPlacement->clipStart == INT_MIN &&
                m_layoutPlacement->clipEnd == INT_MAX))
            return fallback;
        const int left = m_layoutPlacement->clipStart == INT_MIN ? mOffsetX : m_layoutPlacement->clipStart;
        const int right = m_layoutPlacement->clipEnd == INT_MAX ?
            mOffsetX + mOverlayWidth * 8 : m_layoutPlacement->clipEnd;
        return (left - mOffsetX + int(progress * (right - left))) >> 3;
    }
    bool PrepareProjectedPath(const RenderGeometry::Perspective& projection, CPoint relativeOrigin,
        CPoint position, int64_t& areaBudget);
    void ResetProjectedPath();
    bool m_wrapAfter = false;
    bool HasNativeOutline() const;
    CRect DrawNativeOutline(SubPicDesc& spd, CRect& clip, BYTE* mask, int x, int y, int time, int fade,
        MOD_MOVEVC& vectorClip);
    CRect DrawNativeShadow(SubPicDesc& spd, CRect& clip, BYTE* mask, int x, int y, DWORD* colors,
        MOD_MOVEVC& vectorClip);
    CRect DrawBodyPaint(SubPicDesc& spd, CRect& clip, BYTE* mask, int x, int y, const DWORD* colors,
        MOD_MOVEVC& vectorClip, bool reverse, int channel = 0);
#endif

#if defined (_VSMOD) && defined(_LUA)
    void Paint(CPoint p, CPoint org, int Layer);
#else
    void Paint(CPoint p, CPoint org);
#endif
};

class CText : public CWord
{
protected:
    virtual bool CreatePath();
#ifdef _VSMOD
    std::unique_ptr<CWord> CreateUndecoratedWord() override;
#endif

public:
    CText(STSStyle& style, CStringW str, int ktype, int kstart, int kend, double scalex, double scaley);

    virtual CWord* Copy();
    virtual bool Append(CWord* w);
};

class CPolygon : public CWord
{
    bool Get6BitFixedPoint(CStringW& str, LONG& ret);
    bool GetLONG(CStringW& str, LONG& ret);
    bool GetPOINT(CStringW& str, POINT& ret);
    bool ParseStr();

protected:
    double m_scalex, m_scaley;
    int m_baseline;

    CAtlArray<BYTE> m_pathTypesOrg;
    CAtlArray<CPoint> m_pathPointsOrg;

    virtual bool CreatePath();

public:
    CPolygon(STSStyle& style, CStringW str, int ktype, int kstart, int kend, double scalex, double scaley, int baseline);
    virtual ~CPolygon();

    virtual CWord* Copy();
    virtual bool Append(CWord* w);
};

class CClipper : public CPolygon
{
private:
    std::shared_ptr<BYTE> m_maskStorage;
    CWord* Copy();
    virtual bool Append(CWord* w);

public:
#if defined (_VSMOD) && defined(_LUA)
    CString LuaStyle;
    CClipper(CStringW str, CSize size, double scalex, double scaley, bool inverse, CString LuaStyle, lua_State * L, std::wofstream * LuaLog, int entry);

    void ParseLuaTable(STSStyle& style, CPoint & pos, CPoint & org);
#else
    CClipper(CStringW str, CSize size, double scalex, double scaley, bool inverse);
#endif
    virtual ~CClipper();
    void MakeMaskWritable();

    CSize m_size;
    bool m_inverse;
    BYTE* m_pAlphaMask;
};

class CLine : public CAtlList<CWord*>
{
public:
    int m_width, m_ascent, m_descent, m_borderX, m_borderY;
    int m_horizontalAnchor = -1;

    virtual ~CLine();

    void Compact();

#ifdef _VSMOD // patch m006. moveable vector clip
    CRect PaintShadow(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha, MOD_MOVEVC& mod_vc, REFERENCE_TIME rt);
    CRect PaintOutline(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha, MOD_MOVEVC& mod_vc, REFERENCE_TIME rt);
    CRect PaintBody(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha, MOD_MOVEVC& mod_vc, REFERENCE_TIME rt);
#else
    CRect PaintShadow(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha);
    CRect PaintOutline(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha);
    CRect PaintBody(SubPicDesc& spd, CRect& clipRect, BYTE* pAlphaMask, CPoint p, CPoint org, int time, int alpha);
#endif
};

enum eftype
{
    EF_MOVE = 0,	// {\move(x1=param[0], y1=param[1], x2=param[2], y2=param[3], t1=t[0], t2=t[1])} or {\pos(x=param[0], y=param[1])}
    EF_ORG,			// {\org(x=param[0], y=param[1])}
    EF_FADE,		// {\fade(a1=param[0], a2=param[1], a3=param[2], t1=t[0], t2=t[1], t3=t[2], t4=t[3])} or {\fad(t1=t[1], t2=t[2])
    EF_BANNER,		// Banner;delay=param[0][;lefttoright=param[1];fadeawaywidth=param[2]]
    EF_SCROLL,		// Scroll up/down=param[3];top=param[0];bottom=param[1];delay=param[2][;fadeawayheight=param[4]]
#ifdef _VSMOD // patch m006. moveable vector clip
    EF_VECTCLP
#endif
};

#ifdef _VSMOD // patch m006. moveable vector clip
#define EF_NUMBEROFEFFECTS 6
#else
#define EF_NUMBEROFEFFECTS 5
#endif

class Effect
{
public:
    enum eftype type;
    int param[9];
    int t[4];
    DWORD fadeColors[2]{};
    bool hasFadeColor[2]{};
};

#if defined (_VSMOD) && defined(_LUA)
class CSubtitle : public CAtlList<CLine*>, public CMyLua
#else
class CSubtitle : public CAtlList<CLine*>
#endif
{
    int GetFullWidth();
    int GetFullLineWidth(POSITION pos);
    int GetWrapWidth(POSITION pos, int maxwidth);
    CLine* GetNextLine(POSITION& pos, int maxwidth);

public:
    unsigned m_paintSegments = 0;
    std::unique_ptr<KfxPaint::SlideSettings> m_slideSettings;
#ifdef _VSMOD
    int m_borderStyleOverride = -1;
    int m_boxMode = -1;
    bool m_revealKaraoke = false;
    std::vector<std::unique_ptr<CWord>> m_eventBox;
    std::unique_ptr<AssNumeric::Context> m_layoutVariables;
    bool m_measuringLayout = false;
    std::unique_ptr<AssMotion::Track> m_motion;
    std::unique_ptr<std::array<AssNumeric::Operand, 2>> m_origin;
    std::shared_ptr<const RenderGeometry::Curve> m_curve;
    double m_curveX = 0, m_curveY = 0, m_curveScale = 1;
    int m_curveAnchor = 0;
    bool m_curveShapeFailed = false;
    void CurveLines();
    bool m_vertical = false, m_verticalLaidOut = false;
    int m_verticalProfile = 0, m_verticalDirection = 0, m_verticalAnchor = 0;
    double m_verticalSpacing = 0, m_verticalColumnGap = 0;
    bool MakeVerticalLines(int height, CSize& bounds);
    std::unique_ptr<RenderGeometry::Pins> m_perspectivePins;
    std::unique_ptr<RenderGeometry::Perspective> m_perspective;
    CPoint m_perspectiveOrigin = CPoint(INT_MIN, INT_MIN);
    int m_perspectiveLeft = 0;
    REFERENCE_TIME m_perspectiveTime = 0;
    bool m_perspectiveJitter = false;
    void ConfigurePerspective();
    void PreparePerspective(CPoint anchor, CPoint origin, const CRect& bounds, REFERENCE_TIME timestamp);
    struct RubyGroup
    {
        struct Timing { int type, start, end; double width; };
        Furigana::Options options;
        std::array<std::vector<std::unique_ptr<CWord>>, 2> annotations;
        std::vector<Timing> baseTiming;
    };
    std::vector<RubyGroup> m_rubyGroups;
    Furigana::Options m_rubyOptions;
    bool m_keepRubyKaraoke = false;
    void PlaceHorizontalRuby();
    std::vector<std::unique_ptr<SemanticColumns::Block>> m_columnBlocks;
    SemanticColumns::Block* m_columns = nullptr;
    void ArrangeColumns();
    bool m_hasExtendedWarp = false;
    int m_warpedAnchor = 0;
    CPoint m_warpedShift = CPoint(0, 0);
    void ConfigureDistortion();
    std::unique_ptr<KfxPaint::Cycles> m_cycles;
    double m_clipOffsetX = 0, m_clipOffsetY = 0;
    bool m_hasRectClip = false;
    struct LineAlignment { int first, inherited, full; };
    std::map<int, LineAlignment> m_lineAlignment;
    int m_logicalLine = 0, m_textAnchor = -1, m_textWidth = 0;
    unsigned m_positionedAlphaScopes = 0;
    void CreateEventBox(int borderStyle, int time, bool scaledBorder);
#endif
    int m_scrAlignment;
    int m_wrapStyle;
    bool m_fAnimated;
    int m_relativeTo;

    Effect* m_effects[EF_NUMBEROFEFFECTS];

    CAtlList<CWord*> m_words;

    CClipper* m_pClipper;

    CRect m_rect, m_clip;
    int m_topborder, m_bottomborder;
    bool m_clipInverse;

    double m_scalex, m_scaley;

public:
    CSubtitle();
    virtual ~CSubtitle();
    virtual void Empty();

    void CreateClippers(CSize size);

    void MakeLines(CSize size, CRect marginRect);
    void AppendWord(CWord* word);
    int LineLeft(const CLine& line, int anchor) const;
};

class CScreenLayoutAllocator
{
    typedef struct
    {
        CRect r;
        int segment, entry, layer;
    } SubRect;

    CAtlList<SubRect> m_subrects;

public:
    virtual void Empty();

    void AdvanceToSegment(int segment, const CAtlArray<int>& sa);
    CRect AllocRect(CSubtitle* s, int segment, int entry, int layer, int collisions);
};

struct OverrideTagCache;
struct TextGeometryCache;
struct TextDeviceContext;
class EventWorkerPool;
struct EventWorkerStatistics;

[uuid("537DCACA-2812-4a4f-B2C6-1A34C17ADEB0")]
class CRenderedTextSubtitle : public CSimpleTextSubtitle, public ISubPicProviderImpl, public ISubStream
{
    CAtlMap<int, CSubtitle*> m_subtitleCache;
    std::unique_ptr<OverrideTagCache> m_overrideTags;
    std::unique_ptr<TextGeometryCache> m_textGeometry;
    std::unique_ptr<TextDeviceContext> m_textDC;
    std::unique_ptr<EventWorkerPool> m_eventWorkers;
#ifdef _VSMOD
    std::unique_ptr<ShapedText::Context> m_shaper;
    bool ParseShapedString(CSubtitle* sub, const CStringW& text, STSStyle& style, bool verticalVariants);
    bool ParseFurigana(CSubtitle* sub, Furigana::Fields& fields, STSStyle& style);
#endif

    CScreenLayoutAllocator m_sla;

#if defined(_VSMOD) && defined(_LUA)
    int   m_entry; // Id of line

    int   m_start; // Start time
    int   m_end;   // End time
#endif

    CSize m_size;
    CRect m_vidrect;

    // temp variables, used when parsing the script
    int m_time, m_delay;
    int m_animStart, m_animEnd;
    bool m_exactAnimationEnd = false;
    double m_animAccel;
    int m_ktype, m_kstart, m_kend;
    int m_nPolygon;
    int m_polygonBaselineOffset;
    STSStyle *m_pStyleOverride; // the app can decide to use this style instead of a built-in one
    bool m_doOverrideStyle;

    void ParseEffect(CSubtitle* sub, CString str);
    void ParseString(CSubtitle* sub, CStringW str, STSStyle& style);
    void ParsePolygon(CSubtitle* sub, CStringW str, STSStyle& style);
    bool ParseSSATag(CSubtitle* sub, CStringW str, STSStyle& style, STSStyle& org, bool fAnimate = false,
        const KfxPaint::SlideSettings* inheritedSlides = nullptr);
    bool ParseHtmlTag(CSubtitle* sub, CStringW str, STSStyle& style, STSStyle& org);

#if defined(_VSMOD) && defined(_LUA)
    void ParseLuaTable(CSubtitle* sub, STSStyle& style);
#endif

    double CalcAnimation(double dst, double src, bool fAnimate);

    CSubtitle* GetSubtitle(int entry, bool measuringLayout = false);

protected:
    virtual void OnChanged();

public:
    EventWorkerStatistics WorkerStatistics() const;
    CRenderedTextSubtitle(CCritSec* pLock, STSStyle *styleOverride = NULL, bool doOverride = false);
    virtual ~CRenderedTextSubtitle();

    virtual void Copy(CSimpleTextSubtitle& sts);
    virtual void Empty();

    // call to signal this RTS to ignore any of the styles and apply the given override style
    void SetOverride(bool doOverride = true, STSStyle *styleOverride = NULL)
    {
        m_doOverrideStyle = doOverride;
        if(styleOverride != NULL) m_pStyleOverride = styleOverride;
    }

public:
    bool Init(CSize size, CRect vidrect); // will call Deinit()
    void Deinit();

    DECLARE_IUNKNOWN
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv);

    // ISubPicProvider
    STDMETHODIMP_(POSITION) GetStartPosition(REFERENCE_TIME rt, double fps);
    STDMETHODIMP_(POSITION) GetNext(POSITION pos);
    STDMETHODIMP_(REFERENCE_TIME) GetStart(POSITION pos, double fps);
    STDMETHODIMP_(REFERENCE_TIME) GetStop(POSITION pos, double fps);
    STDMETHODIMP_(bool) IsAnimated(POSITION pos);
    STDMETHODIMP Render(SubPicDesc& spd, REFERENCE_TIME rt, double fps, RECT& bbox);

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClassID);

    // ISubStream
    STDMETHODIMP_(int) GetStreamCount();
    STDMETHODIMP GetStreamInfo(int i, WCHAR** ppName, LCID* pLCID);
    STDMETHODIMP_(int) GetStream();
    STDMETHODIMP SetStream(int iStream);
    STDMETHODIMP Reload();
};
