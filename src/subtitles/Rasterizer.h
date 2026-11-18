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
#include <vector>
#include "..\SubPic\ISubPic.h"
#ifdef _VSMOD // patch m004. gradient colors
#include "STS.h"
#endif

#ifdef _VSMOD // patch m006. moveable vector clip
class MOD_MOVEVC
{
public:
    // movevc
    bool enable;
    CSize size;
    CPoint pos;

    //CSize canvas;	// canvas size
    CSize spd;		// output canvas size
    CPoint curpos;  // output origin point
    int hfull;		// full height
    byte* alphamask;
    bool isInverse;

    MOD_MOVEVC();

    byte GetAlphaValue(int wx, int wy) const;
    void clear();
};
#endif

#define PT_MOVETONC 0xfe
#define PT_BSPLINETO 0xfc
#define PT_BSPLINEPATCHTO 0xfa

class RasterizerNfo
{
public:
    int w;
    int h;
    int rowOffset = 0;
    int overlayp;
    int pitch;

    int xo;

    DWORD sw[4];
    byte* s;
    byte* src;
    DWORD* dst;
    MOD_BLEND mod_blendMode = BLEND_NORMAL;
};

struct OverlayDraw
{
    CRect bounds;
    RasterizerNfo info;
    bool body;
    BYTE* alphaMask;

#ifdef _VSMOD
    int colorType;
    MOD_GRADIENT gradient;
    MOD_MOVEVC maskPosition;
#else
    int maskPitch;
#endif

    void Blend(int top, int bottom) const;
};

struct OverlayDrawList
{
    std::vector<OverlayDraw> draws;
    // A word can replace its coverage mask between the shadow and outline.
    // Keep earlier phases alive until all bands have read them.
    std::vector<std::unique_ptr<BYTE[]>> retiredMasks;
};

class OverlayCaptureScope
{
    OverlayDrawList* previous;

public:
    explicit OverlayCaptureScope(OverlayDrawList& list);
    ~OverlayCaptureScope();
    OverlayCaptureScope(const OverlayCaptureScope&) = delete;
    OverlayCaptureScope& operator=(const OverlayCaptureScope&) = delete;
};

#if defined (_VSMOD) && defined(_LUA)
class Rasterizer : public CMyLua
#else
class Rasterizer
#endif
{
    bool fFirstSet;
    CPoint firstp, lastp;

protected:
    BYTE* mpPathTypes;
    POINT* mpPathPoints;
    size_t mPathPoints;
    bool CopyPath(const POINT* points, const BYTE* types, size_t count);

private:
    int mWidth, mHeight;

    typedef std::pair<unsigned __int64, unsigned __int64> tSpan;
    typedef std::vector<tSpan> tSpanBuffer;

    tSpanBuffer mOutline;
    tSpanBuffer mWideOutline;
    int mWideBorder;

    struct Edge
    {
        int next;
        int posandflag;
    } *mpEdgeBuffer = nullptr;
    size_t mEdgeHeapSize;
    size_t mEdgeNext;

    size_t* mpScanBuffer = nullptr;

    typedef unsigned char byte;

protected:
    int mPathOffsetX, mPathOffsetY;
    int mOffsetX, mOffsetY;
    int mOverlayWidth, mOverlayHeight;
    byte *mpOverlayBuffer;

private:
    void _TrashPath();
    void _TrashOverlay();
    void _TrashScanBuffers();
    void _ReallocEdgeBuffer(size_t edges);
    void _EvaluateBezier(int ptbase, bool fBSpline);
    void _EvaluateLine(int pt1idx, int pt2idx);
    void _EvaluateLine(int x0, int y0, int x1, int y1);
    static void _OverlapRegion(tSpanBuffer& dst, tSpanBuffer& scratch,
        tSpanBuffer::const_iterator first, tSpanBuffer::const_iterator last, int dx, int dy);
    bool WidenOutlineByRow(int rx, int ry);
public:
#if defined (_VSMOD) && defined(_LUA)
    int     m_entry; // id
    int     m_layer;
    CString LuaRendererHandler;
#endif

    Rasterizer();
    virtual ~Rasterizer();

    bool BeginPath(HDC hdc);
    bool EndPath(HDC hdc);
    bool PartialBeginPath(HDC hdc, bool bClearPath);
    bool PartialEndPath(HDC hdc, long dx, long dy);
    bool ScanConvert();
    bool CreateWidenedRegion(int borderX, int borderY);
    void DeleteOutlines();
    bool Rasterize(int xsub, int ysub, int fBlur, double fGaussianBlurX, double fGaussianBlurY);
    int getOverlayWidth();
#ifdef _VSMOD // patch m004. gradient colors
    CRect Draw(SubPicDesc& spd, CRect& clipRect, byte* pAlphaMask, int xsub, int ysub, const DWORD* switchpts, bool fBody, bool fBorder, int typ, MOD_GRADIENT& mod_grad, MOD_MOVEVC& mod_vc, MOD_BLEND mod_blendMode);
#else
    CRect Draw(SubPicDesc& spd, CRect& clipRect, byte* pAlphaMask, int xsub, int ysub, const DWORD* switchpts, bool fBody, bool fBorder);
#endif
    void FillSolidRect(SubPicDesc& spd, int x, int y, int nWidth, int nHeight, DWORD lColor);
};

