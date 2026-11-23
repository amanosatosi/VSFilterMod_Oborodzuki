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

#include "stdafx.h"
#include "RenderProfile.h"
#include <cstdint>
#include <string.h>
#include <math.h>
#include <new>
#include <vector>
#include <algorithm>
#include "Rasterizer.h"
#include "SeparableFilter.h"
#include "OverlayMix.h"

#ifndef _MAX	/* avoid collision with common (nonconforming) macros */
#define _MAX	(max)
#define _MIN	(min)
#define _IMPL_MAX max
#define _IMPL_MIN min
#else
#define _IMPL_MAX _MAX
#define _IMPL_MIN _MIN
#endif

static thread_local OverlayDrawList* activeOverlayDraws = nullptr;

#ifdef _VSMOD
static thread_local const OverlayPaintScope* activeKfxPaint = nullptr;

OverlayPaintScope::OverlayPaintScope(KfxPaint::Fade fade, double sx, double sy)
    : previous(activeKfxPaint), fade(fade), scaleX(std::abs(sx)), scaleY(std::abs(sy))
{
    activeKfxPaint = this;
}

OverlayPaintScope::~OverlayPaintScope()
{
    activeKfxPaint = previous;
}

bool IsCapturingOverlays() { return activeOverlayDraws != nullptr; }
#endif

OverlayCaptureScope::OverlayCaptureScope(OverlayDrawList& list) : previous(activeOverlayDraws)
{
    activeOverlayDraws = &list;
}

OverlayCaptureScope::~OverlayCaptureScope()
{
    activeOverlayDraws = previous;
}

int Rasterizer::getOverlayWidth()
{
    return mOverlayWidth * 8;
}

Rasterizer::Rasterizer() : mpPathTypes(NULL), mpPathPoints(NULL), mPathPoints(0), mpOverlayBuffer(NULL)
{
    mOverlayWidth = mOverlayHeight = 0;
    mPathOffsetX = mPathOffsetY = 0;
    mOffsetX = mOffsetY = 0;
}

Rasterizer::~Rasterizer()
{
    _TrashPath();
    _TrashOverlay();
    if (mpEdgeBuffer || mpScanBuffer)
        _TrashScanBuffers();
}

void Rasterizer::_TrashPath()
{
    free(mpPathTypes);
    free(mpPathPoints);
    mpPathTypes = NULL;
    mpPathPoints = NULL;
    mPathPoints = 0;
}

void Rasterizer::_TrashOverlay()
{
    delete [] mpOverlayBuffer;
    mpOverlayBuffer = NULL;
}

void Rasterizer::_TrashScanBuffers()
{
    free(mpEdgeBuffer);
    delete[] mpScanBuffer;
    mpEdgeBuffer = nullptr;
    mpScanBuffer = nullptr;
}

void Rasterizer::_ReallocEdgeBuffer(size_t edges)
{
    if (edges > SIZE_MAX / sizeof(Edge))
        throw std::bad_alloc();
    auto* grown = static_cast<Edge*>(realloc(mpEdgeBuffer, sizeof(Edge) * edges));
    if (!grown)
        throw std::bad_alloc();
    mpEdgeBuffer = grown;
    mEdgeHeapSize = edges;
}

void Rasterizer::_EvaluateBezier(int ptbase, bool fBSpline)
{
    const POINT* pt0 = mpPathPoints + ptbase;
    const POINT* pt1 = mpPathPoints + ptbase + 1;
    const POINT* pt2 = mpPathPoints + ptbase + 2;
    const POINT* pt3 = mpPathPoints + ptbase + 3;

    double x0 = pt0->x;
    double x1 = pt1->x;
    double x2 = pt2->x;
    double x3 = pt3->x;
    double y0 = pt0->y;
    double y1 = pt1->y;
    double y2 = pt2->y;
    double y3 = pt3->y;

    double cx3, cx2, cx1, cx0, cy3, cy2, cy1, cy0;

    if(fBSpline)
    {
        // 1   [-1 +3 -3 +1]
        // - * [+3 -6 +3  0]
        // 6   [-3  0 +3  0]
        //	   [+1 +4 +1  0]

        double _1div6 = 1.0 / 6.0;

        cx3 = _1div6 * (-  x0 + 3 * x1 - 3 * x2 + x3);
        cx2 = _1div6 * (3 * x0 - 6 * x1 + 3 * x2);
        cx1 = _1div6 * (-3 * x0	   + 3 * x2);
        cx0 = _1div6 * (x0 + 4 * x1 + 1 * x2);

        cy3 = _1div6 * (-  y0 + 3 * y1 - 3 * y2 + y3);
        cy2 = _1div6 * (3 * y0 - 6 * y1 + 3 * y2);
        cy1 = _1div6 * (-3 * y0     + 3 * y2);
        cy0 = _1div6 * (y0 + 4 * y1 + 1 * y2);
    }
    else // bezier
    {
        // [-1 +3 -3 +1]
        // [+3 -6 +3  0]
        // [-3 +3  0  0]
        // [+1  0  0  0]

        cx3 = -  x0 + 3 * x1 - 3 * x2 + x3;
        cx2 =  3 * x0 - 6 * x1 + 3 * x2;
        cx1 = -3 * x0 + 3 * x1;
        cx0 =    x0;

        cy3 = -  y0 + 3 * y1 - 3 * y2 + y3;
        cy2 =  3 * y0 - 6 * y1 + 3 * y2;
        cy1 = -3 * y0 + 3 * y1;
        cy0 =    y0;
    }

    //
    // This equation is from Graphics Gems I.
    //
    // The idea is that since we're approximating a cubic curve with lines,
    // any error we incur is due to the curvature of the line, which we can
    // estimate by calculating the maximum acceleration of the curve.  For
    // a cubic, the acceleration (second derivative) is a line, meaning that
    // the absolute maximum acceleration must occur at either the beginning
    // (|c2|) or the end (|c2+c3|).  Our bounds here are a little more
    // conservative than that, but that's okay.
    //
    // If the acceleration of the parametric formula is zero (c2 = c3 = 0),
    // that component of the curve is linear and does not incur any error.
    // If a=0 for both X and Y, the curve is a line segment and we can
    // use a step size of 1.

    double maxaccel1 = fabs(2 * cy2) + fabs(6 * cy3);
    double maxaccel2 = fabs(2 * cx2) + fabs(6 * cx3);

    double maxaccel = maxaccel1 > maxaccel2 ? maxaccel1 : maxaccel2;
    double h = 1.0;

    if(maxaccel > 8.0) h = sqrt(8.0 / maxaccel);

    if(!fFirstSet)
    {
        firstp.x = (LONG)cx0;
        firstp.y = (LONG)cy0;
        lastp = firstp;
        fFirstSet = true;
    }

    for(double t = 0; t < 1.0; t += h)
    {
        double x = cx0 + t * (cx1 + t * (cx2 + t * cx3));
        double y = cy0 + t * (cy1 + t * (cy2 + t * cy3));
        _EvaluateLine(lastp.x, lastp.y, (int)x, (int)y);
    }

    double x = cx0 + cx1 + cx2 + cx3;
    double y = cy0 + cy1 + cy2 + cy3;
    _EvaluateLine(lastp.x, lastp.y, (int)x, (int)y);
}

void Rasterizer::_EvaluateLine(int pt1idx, int pt2idx)
{
    const POINT* pt1 = mpPathPoints + pt1idx;
    const POINT* pt2 = mpPathPoints + pt2idx;

    _EvaluateLine(pt1->x, pt1->y, pt2->x, pt2->y);
}

void Rasterizer::_EvaluateLine(int x0, int y0, int x1, int y1)
{
#ifdef _VSMOD
    if (m_geometricEdges && (x0 != x1 || y0 != y1))
    {
        if (m_geometricEdges->size() >= 65536)
            m_geometricEdges.reset();
        else
            m_geometricEdges->push_back({x0, y0, x1, y1, m_geometricContour});
    }
#endif
    if(lastp.x != x0 || lastp.y != y0)
    {
        _EvaluateLine(lastp.x, lastp.y, x0, y0);
    }

    if(!fFirstSet)
    {
        firstp.x = x0;
        firstp.y = y0;
        fFirstSet = true;
    }
    lastp.x = x1;
    lastp.y = y1;

    if(y1 > y0)	// down
    {
        __int64 xacc = (__int64)x0 << 13;

        // prestep y0 down

        int dy = y1 - y0;
        int y = ((y0 + 3)&~7) + 4;
        int iy = y >> 3;

        y1 = (y1 - 5) >> 3;

        if(iy <= y1)
        {
            __int64 invslope = (__int64(x1 - x0) << 16) / dy;

            while(mEdgeNext + y1 + 1 - iy > mEdgeHeapSize)
                _ReallocEdgeBuffer(mEdgeHeapSize * 2);

            xacc += (invslope * (y - y0)) >> 3;

            while(iy <= y1)
            {
                int ix = (int)((xacc + 32768) >> 16);

                mpEdgeBuffer[mEdgeNext].next = mpScanBuffer[iy];
                mpEdgeBuffer[mEdgeNext].posandflag = ix * 2 + 1;

                mpScanBuffer[iy] = mEdgeNext++;

                ++iy;
                xacc += invslope;
            }
        }
    }
    else if(y1 < y0) // up
    {
        __int64 xacc = (__int64)x1 << 13;

        // prestep y1 down

        int dy = y0 - y1;
        int y = ((y1 + 3)&~7) + 4;
        int iy = y >> 3;

        y0 = (y0 - 5) >> 3;

        if(iy <= y0)
        {
            __int64 invslope = (__int64(x0 - x1) << 16) / dy;

            while(mEdgeNext + y0 + 1 - iy > mEdgeHeapSize)
                _ReallocEdgeBuffer(mEdgeHeapSize * 2);

            xacc += (invslope * (y - y1)) >> 3;

            while(iy <= y0)
            {
                int ix = (int)((xacc + 32768) >> 16);

                mpEdgeBuffer[mEdgeNext].next = mpScanBuffer[iy];
                mpEdgeBuffer[mEdgeNext].posandflag = ix * 2;

                mpScanBuffer[iy] = mEdgeNext++;

                ++iy;
                xacc += invslope;
            }
        }
    }
}

bool Rasterizer::BeginPath(HDC hdc)
{
    _TrashPath();

    return !!::BeginPath(hdc);
}

bool Rasterizer::CopyPath(const POINT* points, const BYTE* types, size_t count)
{
    if (!count)
    {
        _TrashPath();
        return true;
    }
    auto* copyPoints = static_cast<POINT*>(malloc(count * sizeof(POINT)));
    auto* copyTypes = static_cast<BYTE*>(malloc(count * sizeof(BYTE)));
    if (!copyPoints || !copyTypes)
    {
        free(copyPoints);
        free(copyTypes);
        return false;
    }
    memcpy(copyPoints, points, count * sizeof(POINT));
    memcpy(copyTypes, types, count * sizeof(BYTE));
    _TrashPath();
    mPathPoints = count;
    mpPathPoints = copyPoints;
    mpPathTypes = copyTypes;
    return true;
}

bool Rasterizer::EndPath(HDC hdc)
{
    ::CloseFigure(hdc);

    if(::EndPath(hdc))
    {
        mPathPoints = GetPath(hdc, NULL, NULL, 0);

        if(!mPathPoints)
            return true;

        mpPathTypes = (BYTE*)malloc(sizeof(BYTE) * mPathPoints);
        mpPathPoints = (POINT*)malloc(sizeof(POINT) * mPathPoints);

        if(mPathPoints == GetPath(hdc, mpPathPoints, mpPathTypes, mPathPoints))
            return true;
    }

    ::AbortPath(hdc);

    return false;
}

bool Rasterizer::PartialBeginPath(HDC hdc, bool bClearPath)
{
    if(bClearPath)
        _TrashPath();

    return !!::BeginPath(hdc);
}

bool Rasterizer::PartialEndPath(HDC hdc, long dx, long dy)
{
    ::CloseFigure(hdc);

    if(::EndPath(hdc))
    {
        int nPoints;
        BYTE* pNewTypes;
        POINT* pNewPoints;

        nPoints = GetPath(hdc, NULL, NULL, 0);

        if(!nPoints)
            return true;

        pNewTypes = (BYTE*)realloc(mpPathTypes, (mPathPoints + nPoints) * sizeof(BYTE));
        pNewPoints = (POINT*)realloc(mpPathPoints, (mPathPoints + nPoints) * sizeof(POINT));

        if(pNewTypes)
            mpPathTypes = pNewTypes;

        if(pNewPoints)
            mpPathPoints = pNewPoints;

        BYTE* pTypes = DNew BYTE[nPoints];
        POINT* pPoints = DNew POINT[nPoints];

        if(pNewTypes && pNewPoints && nPoints == GetPath(hdc, pPoints, pTypes, nPoints))
        {
            for(ptrdiff_t i = 0; i < nPoints; ++i)
            {
                mpPathPoints[mPathPoints + i].x = pPoints[i].x + dx;
                mpPathPoints[mPathPoints + i].y = pPoints[i].y + dy;
                mpPathTypes[mPathPoints + i] = pTypes[i];
            }

            mPathPoints += nPoints;

            delete[] pTypes;
            delete[] pPoints;
            return true;
        }
        else
            DebugBreak();

        delete[] pTypes;
        delete[] pPoints;
    }

    ::AbortPath(hdc);

    return false;
}

bool Rasterizer::ScanConvert(bool geometric)
{
    PROFILE_RENDER_STAGE(ScanConvert);
    size_t lastmoveto = -1;
    size_t i;

    // Drop any outlines we may have.

    mOutline.clear();
    mWideOutline.clear();
    mWideBorder = 0;
#ifdef _VSMOD
    if (geometric)
        m_geometricEdges = std::make_unique<std::vector<ContourEdge>>();
    else
        m_geometricEdges.reset();
    m_geometricContour = 0;
#endif

    // Determine bounding box

    if(!mPathPoints)
    {
        mPathOffsetX = mPathOffsetY = 0;
        mWidth = mHeight = 0;
        return 0;
    }

    ptrdiff_t minx = INT_MAX;
    ptrdiff_t miny = INT_MAX;
    ptrdiff_t maxx = INT_MIN;
    ptrdiff_t maxy = INT_MIN;

    for(i = 0; i < mPathPoints; ++i)
    {
        ptrdiff_t ix = mpPathPoints[i].x;
        ptrdiff_t iy = mpPathPoints[i].y;

        if(ix < minx) minx = ix;
        if(ix > maxx) maxx = ix;
        if(iy < miny) miny = iy;
        if(iy > maxy) maxy = iy;
    }

    minx = (minx >> 3) & ~7;
    miny = (miny >> 3) & ~7;
    maxx = (maxx + 7) >> 3;
    maxy = (maxy + 7) >> 3;

    for(i = 0; i < mPathPoints; ++i)
    {
        mpPathPoints[i].x -= minx * 8;
        mpPathPoints[i].y -= miny * 8;
    }

    if(minx > maxx || miny > maxy)
    {
        mWidth = mHeight = 0;
        mPathOffsetX = mPathOffsetY = 0;
        _TrashPath();
        return true;
    }

    mWidth = maxx + 1 - minx;
    mHeight = maxy + 1 - miny;

    mPathOffsetX = minx;
    mPathOffsetY = miny;

    // Initialize edge buffer.  We use edge 0 as a sentinel.

    mEdgeNext = 1;
    _ReallocEdgeBuffer(2048);

    // Initialize scanline list.

    mpScanBuffer = DNew size_t[mHeight];
    memset(mpScanBuffer, 0, mHeight * sizeof(size_t));

    // Scan convert the outline.  Yuck, Bezier curves....

    // Unfortunately, Windows 95/98 GDI has a bad habit of giving us text
    // paths with all but the first figure left open, so we can't rely
    // on the PT_CLOSEFIGURE flag being used appropriately.

    fFirstSet = false;
    firstp.x = firstp.y = 0;
    lastp.x = lastp.y = 0;

    for(i = 0; i < mPathPoints; ++i)
    {
        BYTE t = mpPathTypes[i] & ~PT_CLOSEFIGURE;

        switch(t)
        {
        case PT_MOVETO:
            if(lastmoveto >= 0 && firstp != lastp)
                _EvaluateLine(lastp.x, lastp.y, firstp.x, firstp.y);
#ifdef _VSMOD
            if (m_geometricEdges) ++m_geometricContour;
#endif
            lastmoveto = i;
            fFirstSet = false;
            lastp = mpPathPoints[i];
            break;
        case PT_MOVETONC:
            break;
        case PT_LINETO:
            if(mPathPoints - (i - 1) >= 2) _EvaluateLine(i - 1, i);
            break;
        case PT_BEZIERTO:
            if(mPathPoints - (i - 1) >= 4) _EvaluateBezier(i - 1, false);
            i += 2;
            break;
        case PT_BSPLINETO:
            if(mPathPoints - (i - 1) >= 4) _EvaluateBezier(i - 1, true);
            i += 2;
            break;
        case PT_BSPLINEPATCHTO:
            if(mPathPoints - (i - 3) >= 4) _EvaluateBezier(i - 3, true);
            break;
        }
    }

    if(lastmoveto >= 0 && firstp != lastp)
        _EvaluateLine(lastp.x, lastp.y, firstp.x, firstp.y);

    // Free the path since we don't need it anymore.

    _TrashPath();

    // Convert the edges to spans.  We couldn't do this before because some of
    // the regions may have winding numbers >+1 and it would have been a pain
    // to try to adjust the spans on the fly.  We use one heap to detangle
    // a scanline's worth of edges from the singly-linked lists, and another
    // to collect the actual scans.

    std::vector<int> heap;

    mOutline.reserve(mEdgeNext / 2);

    __int64 y = 0;

    for(y = 0; y < mHeight; ++y)
    {
        int count = 0;

        // Detangle scanline into edge heap.

        for(size_t ptr = (mpScanBuffer[y] & size_t(-1)); ptr; ptr = mpEdgeBuffer[ptr].next)
        {
            heap.push_back(mpEdgeBuffer[ptr].posandflag);
        }

        // Sort edge heap.  Note that we conveniently made the opening edges
        // one more than closing edges at the same spot, so we won't have any
        // problems with abutting spans.

        std::sort(heap.begin(), heap.end()/*begin() + heap.size()*/);

        // Process edges and add spans.  Since we only check for a non-zero
        // winding number, it doesn't matter which way the outlines go!

        std::vector<int>::iterator itX1 = heap.begin();
        std::vector<int>::iterator itX2 = heap.end(); // begin() + heap.size();

        size_t x1, x2;

        for(; itX1 != itX2; ++itX1)
        {
            size_t x = *itX1;

            if(!count)
                x1 = (x >> 1);

            if(x & 1)
                ++count;
            else
                --count;

            if(!count)
            {
                x2 = (x >> 1);

                if(x2 > x1)
                    mOutline.push_back(std::pair<__int64, __int64>((y << 32) + x1 + 0x4000000040000000i64, (y << 32) + x2 + 0x4000000040000000i64)); // G: damn Avery, this is evil! :)
            }
        }

        heap.clear();
    }

    // Dump the edge and scan buffers, since we no longer need them.

    _TrashScanBuffers();

    // All done!

    return true;
}

using namespace std;

template<class IteratorA, class IteratorB, class Offset, class Emit>
static void MergeOutlineSpans(IteratorA itA, IteratorA itAE, IteratorB itB, IteratorB itBE,
    Offset offset1, Offset offset2, Emit emit)
{
    while(itA != itAE && itB != itBE)
    {
        if((*itB).first + offset1 < (*itA).first)
        {
            // B span is earlier.  Use it.

            auto x1 = (*itB).first + offset1;
            auto x2 = (*itB).second + offset2;

            ++itB;

            // B spans don't overlap, so begin merge loop with A first.

            for(;;)
            {
                // If we run out of A spans or the A span doesn't overlap,
                // then the next B span can't either (because B spans don't
                // overlap) and we exit.

                if(itA == itAE || (*itA).first > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itA++).second);
                }
                while(itA != itAE && (*itA).first <= x2);

                // If we run out of B spans or the B span doesn't overlap,
                // then the next A span can't either (because A spans don't
                // overlap) and we exit.

                if(itB == itBE || (*itB).first + offset1 > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itB++).second + offset2);
                }
                while(itB != itBE && (*itB).first + offset1 <= x2);
            }

            // Flush span.

            emit(x1, x2);
        }
        else
        {
            // A span is earlier.  Use it.

            auto x1 = (*itA).first;
            auto x2 = (*itA).second;

            ++itA;

            // A spans don't overlap, so begin merge loop with B first.

            for(;;)
            {
                // If we run out of B spans or the B span doesn't overlap,
                // then the next A span can't either (because A spans don't
                // overlap) and we exit.

                if(itB == itBE || (*itB).first + offset1 > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itB++).second + offset2);
                }
                while(itB != itBE && (*itB).first + offset1 <= x2);

                // If we run out of A spans or the A span doesn't overlap,
                // then the next B span can't either (because B spans don't
                // overlap) and we exit.

                if(itA == itAE || (*itA).first > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itA++).second);
                }
                while(itA != itAE && (*itA).first <= x2);
            }

            // Flush span.

            emit(x1, x2);
        }
    }

    // Copy over leftover spans.

    while(itA != itAE)
    {
        emit(itA->first, itA->second);
        ++itA;
    }

    while(itB != itBE)
    {
        emit(itB->first + offset1, itB->second + offset2);
        ++itB;
    }
}

void Rasterizer::_OverlapRegion(tSpanBuffer& dst, tSpanBuffer& scratch,
    tSpanBuffer::const_iterator first, tSpanBuffer::const_iterator last, int dx, int dy)
{
    const auto verticalOffset = static_cast<unsigned __int64>(dy) << 32;
    scratch.clear();
    scratch.reserve(dst.size() + (last - first));
    dst.swap(scratch);

    MergeOutlineSpans(scratch.cbegin(), scratch.cend(), first, last,
        verticalOffset - dx, verticalOffset + dx,
        [&](unsigned __int64 left, unsigned __int64 right) { dst.emplace_back(left, right); });
}

bool Rasterizer::WidenOutlineByRow(int rx, int ry)
{
    using RowSpan = std::pair<DWORD, DWORD>;
    constexpr size_t rowCapacity = 64;
    std::vector<int> radius(size_t(ry) + 1);
    for (int dy = 0; dy <= ry; ++dy)
        radius[dy] = int(0.5 + sqrt(float(ry * ry - dy * dy)) * float(rx) / float(ry));
    const DWORD maxRadius = static_cast<DWORD>(radius.front());

    // Packed spans are sorted by subpixel row. Each destination row receives
    // the same source rows in the same order as the whole-outline merge.
    struct SourceRow
    {
        DWORD firstSpan;
        int firstIdenticalRow;
    };
    std::vector<SourceRow> rows(size_t(mHeight) + 1);
    std::vector<RowSpan> sourceSpans;
    sourceSpans.reserve(mOutline.size());
    for (int y = 0; y < mHeight; ++y)
    {
        auto& row = rows[y];
        row.firstSpan = DWORD(sourceSpans.size());
        row.firstIdenticalRow = y;
        const auto packedY = static_cast<unsigned __int64>(y) + 0x40000000;
        while (sourceSpans.size() < mOutline.size() &&
            (mOutline[sourceSpans.size()].first >> 32) == packedY)
        {
            const auto& current = mOutline[sourceSpans.size()];
            const DWORD left = DWORD(current.first), right = DWORD(current.second);
            if ((current.second >> 32) != packedY || left < maxRadius || right > MAXDWORD - maxRadius)
                return false;
            sourceSpans.emplace_back(left, right);
        }
        const size_t count = sourceSpans.size() - row.firstSpan;
        if (count > rowCapacity)
            return false;
        if (count == 1 && y && row.firstSpan - rows[y - 1].firstSpan == 1 &&
            sourceSpans[row.firstSpan - 1] == sourceSpans[row.firstSpan])
            row.firstIdenticalRow = rows[y - 1].firstIdenticalRow;
    }
    if (sourceSpans.size() != mOutline.size())
        return false;
    rows[mHeight].firstSpan = DWORD(sourceSpans.size());

    RowSpan rowStorage[rowCapacity], scratchStorage[rowCapacity];
    auto row = &rowStorage[0];
    auto scratch = &scratchStorage[0];
    mWideOutline.reserve(mOutline.size());
    for (int y = -ry; y < mHeight + ry; ++y)
    {
        size_t rowCount = 0;
        const int firstSource = min(y + ry, mHeight - 1);
        const int lastSource = max(y - ry, 0);
        for (int source = firstSource; source >= lastSource;)
        {
            const auto& sourceRow = rows[source];
            const size_t begin = sourceRow.firstSpan;
            const size_t end = rows[source + 1].firstSpan;
            int nearest = source--;
            if (begin != end)
            {
                // Identical single spans expand into nested intervals. If the
                // accumulated row has at most one span, the widest interval
                // gives the same merge without visiting every source row.
                if (end - begin == 1 && rowCount <= 1 && sourceRow.firstIdenticalRow < nearest)
                {
                    const int first = (std::max)(lastSource, sourceRow.firstIdenticalRow);
                    nearest = (std::max)(first, (std::min)(y, nearest));
                    source = first - 1;
                }
                const DWORD dx = radius[abs(y - nearest)];
                if (rowCount == 1 && row[0].first <= sourceSpans[begin].first - dx &&
                    row[0].second >= sourceSpans[end - 1].second + dx)
                    continue;
                if (end - begin == 1)
                {
                    const DWORD left = sourceSpans[begin].first - dx;
                    const DWORD right = sourceSpans[begin].second + dx;
                    size_t first = 0;
                    while (first < rowCount && row[first].second < left)
                        ++first;
                    if (first < rowCount && row[first].first <= right)
                    {
                        row[first].first = (std::min)(row[first].first, left);
                        row[first].second = (std::max)(row[first].second, right);
                        size_t next = first + 1;
                        while (next < rowCount && row[next].first <= row[first].second)
                        {
                            row[first].second = (std::max)(row[first].second, row[next].second);
                            ++next;
                        }
                        if (next > first + 1)
                        {
                            std::move(row + next, row + rowCount, row + first + 1);
                            rowCount -= next - first - 1;
                        }
                        continue;
                    }
                    if (rowCount < rowCapacity)
                    {
                        std::move_backward(row + first, row + rowCount, row + rowCount + 1);
                        row[first] = RowSpan(left, right);
                        ++rowCount;
                        continue;
                    }
                }
                // A shifted row wholly covered by existing disjoint spans
                // cannot change the merge. Check neighbours too: overlapping
                // legacy spans can coalesce even if the new span is contained.
                if (rowCount > 1)
                {
                    size_t covered = begin, span = 0;
                    for (; covered < end; ++covered)
                    {
                        const DWORD left = sourceSpans[covered].first - dx;
                        const DWORD right = sourceSpans[covered].second + dx;
                        while (span < rowCount && row[span].second < left)
                            ++span;
                        if (span == rowCount || row[span].first > left || row[span].second < right ||
                            (span && row[span - 1].second >= row[span].first) ||
                            (span + 1 < rowCount && row[span + 1].first <= row[span].second))
                            break;
                    }
                    if (covered == end)
                        continue;
                }
                if (rowCount + end - begin > rowCapacity)
                {
                    mWideOutline.clear();
                    return false;
                }
                size_t count = 0;
                MergeOutlineSpans(row, row + rowCount, sourceSpans.data() + begin,
                    sourceSpans.data() + end, DWORD(0) - dx, dx,
                    [&](DWORD left, DWORD right) { scratch[count++] = RowSpan(left, right); });
                std::swap(row, scratch);
                rowCount = count;
            }
        }
        const auto packedY = (static_cast<unsigned __int64>(y) + 0x40000000) << 32;
        for (size_t i = 0; i < rowCount; ++i)
            mWideOutline.emplace_back(packedY | row[i].first, packedY | row[i].second);
    }
    return true;
}

bool Rasterizer::PrepareOutline(OutlineCache& cache, int rx, int ry)
{
    const auto build = [&] {
        return ScanConvert() && (!(rx || ry) || CreateWidenedRegion(rx, ry));
    };
    if (!mPathPoints || mPathPoints > 2048)
        return build();

    // Sampling chooses a slot only. The full transformed path and both
    // integer radii must match before any geometry can be reused.
    uint64_t probe = mPathPoints;
    for (size_t i = 0; i < 8; ++i) {
        const auto& p = mpPathPoints[(mPathPoints - 1) * i / 7];
        probe = (probe ^ DWORD(p.x)) * 1099511628211ull;
        probe = (probe ^ DWORD(p.y)) * 1099511628211ull;
    }
    probe = (probe ^ DWORD(rx)) * 1099511628211ull ^ DWORD(ry);
    auto& slot = cache.slots[(probe ^ (probe >> 32) ^ (probe >> 17)) % cache.slots.size()];
    const auto* hit = slot.entry.get();
    if (hit && hit->points.size() == mPathPoints && hit->rx == rx && hit->ry == ry &&
        !memcmp(hit->points.data(), mpPathPoints, mPathPoints * sizeof(POINT)) &&
        !memcmp(hit->types.data(), mpPathTypes, mPathPoints)) {
        mOutline = hit->fill;
        mWideOutline = hit->border;
        mWidth = hit->width;
        mHeight = hit->height;
        mPathOffsetX = hit->offsetX;
        mPathOffsetY = hit->offsetY;
        mWideBorder = (std::max)(rx, ry);
        _TrashPath();
        return true;
    }

    // An animation with a different outline every frame should not allocate
    // cache entries or evict a useful shape on every miss.
    if (slot.lastProbe != probe) {
        slot.lastProbe = probe;
        return build();
    }
    auto entry = std::make_unique<OutlineCache::Entry>();
    entry->points.assign(mpPathPoints, mpPathPoints + mPathPoints);
    entry->types.assign(mpPathTypes, mpPathTypes + mPathPoints);
    if (!build()) return false;
    const size_t pathBytes = entry->points.size() * (sizeof(POINT) + sizeof(BYTE));
    const size_t spanLimit = (65536 - pathBytes) / sizeof(tSpan);
    if (mOutline.size() > spanLimit || mWideOutline.size() > spanLimit - mOutline.size())
        return true;
    entry->rx = rx;
    entry->ry = ry;
    entry->width = mWidth;
    entry->height = mHeight;
    entry->offsetX = mPathOffsetX;
    entry->offsetY = mPathOffsetY;
    entry->fill = mOutline;
    entry->border = mWideOutline;
    slot.entry = std::move(entry);
    return true;
}

bool Rasterizer::CreateWidenedRegion(int rx, int ry)
{
    PROFILE_RENDER_STAGE(WidenOutline);
    if(rx < 0) rx = 0;
    if(ry < 0) ry = 0;

    mWideBorder = max(rx, ry);
    if (mOutline.empty())
        return true;
#ifdef _VSMOD
    if (m_geometricEdges && !m_geometricEdges->empty())
        return CreateMiteredRegion(rx, ry);
#endif

    // Keep the whole-outline path for dimensions where the packed horizontal
    // offset may carry into the row, or where indexing rows would be costly.
    if (ry > 0 && ry <= 2048 && rx <= 0x20000000 && mHeight > 0 && mHeight <= 0x100000 &&
        size_t(mHeight / 2) <= mOutline.size() && mOutline.size() <= MAXDWORD &&
        mWideOutline.empty() && WidenOutlineByRow(rx, ry))
        return true;

    tSpanBuffer scratch;
    if(ry > 0)
    {
        // Do a half circle.
        // _OverlapRegion mirrors this so both halves are done.
        for(ptrdiff_t y = -ry; y <= ry; ++y)
        {
            int x = (int)(0.5 + sqrt(float(ry * ry - y * y)) * float(rx) / float(ry));

            _OverlapRegion(mWideOutline, scratch, mOutline.cbegin(), mOutline.cend(), x, y);
        }
    }
    else if(ry == 0 && rx > 0)
    {
        // There are artifacts if we don't make at least two overlaps of the line, even at same Y coord
        _OverlapRegion(mWideOutline, scratch, mOutline.cbegin(), mOutline.cend(), rx, 0);
        _OverlapRegion(mWideOutline, scratch, mOutline.cbegin(), mOutline.cend(), rx, 0);
    }

    return true;
}

#ifdef _VSMOD
void Rasterizer::SubtractOutlineSpans(const tSpanBuffer& outer, const tSpanBuffer& inner,
    tSpanBuffer& output, uint64_t offset)
{
    auto cut = inner.begin();
    for (const auto& span : outer)
    {
        auto start = span.first;
        while (cut != inner.end() && cut->second + offset <= start) ++cut;
        while (cut != inner.end() && cut->first + offset < span.second)
        {
            if (cut->first + offset > start)
                output.emplace_back(start, cut->first + offset);
            start = (std::max)(start, cut->second + offset);
            if (start >= span.second) break;
            ++cut;
        }
        if (start < span.second)
            output.emplace_back(start, span.second);
    }
}

bool Rasterizer::CreateDecorationMask(const Rasterizer& decorated, const Rasterizer& glyphs)
{
    if (decorated.mOutline.empty() || glyphs.mOutline.empty()) return false;
    mWidth = decorated.mWidth;
    mHeight = decorated.mHeight;
    mPathOffsetX = decorated.mPathOffsetX;
    mPathOffsetY = decorated.mPathOffsetY;
    mWideBorder = 0;
    const auto dx = int64_t(glyphs.mPathOffsetX) - mPathOffsetX;
    const auto dy = int64_t(glyphs.mPathOffsetY) - mPathOffsetY;
    const uint64_t offset = uint64_t(dy * 0x100000000ll + dx);
    SubtractOutlineSpans(decorated.mOutline, glyphs.mOutline, mOutline, offset);
    return !mOutline.empty();
}

bool Rasterizer::CreateMiteredRegion(int rx, int ry)
{
    struct Point { double x, y; };
    const double sx = (std::min)(rx, 2048) * 8.0;
    const double sy = (std::min)(ry, 2048) * 8.0;
    mWideBorder = int(2 * (std::max)(sx, sy) / 8) + 8;
    if (!sx && !sy) return true;

    const auto normal = [&](const ContourEdge& edge) {
        const double dx = (double(edge.x1) - edge.x0) / (std::max)(1., sx);
        const double dy = (double(edge.y1) - edge.y0) / (std::max)(1., sy);
        const double inverse = 1 / std::hypot(dx, dy);
        return Point{-dy * inverse, dx * inverse};
    };
    const auto shift = [&](Point point, Point n, double side) {
        return Point{point.x + sx * n.x * side, point.y + sy * n.y * side};
    };

    std::vector<POINT> points;
    std::vector<BYTE> types;
    // Stroke primitives have the same winding so overlaps merge in the scan
    // converter. The original fill is unioned afterward, including its holes.
    const auto add = [&](std::array<Point, 4> polygon, unsigned count) {
        double area = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % count];
            area += a.x * b.y - a.y * b.x;
        }
        if (std::abs(area) < 0.1) return;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto& p = polygon[area > 0 ? i : count - 1 - i];
            points.push_back({LONG(std::lround(p.x)), LONG(std::lround(p.y))});
            types.push_back(i ? PT_LINETO : PT_MOVETO);
        }
    };
    const auto& edges = *m_geometricEdges;
    for (size_t begin = 0; begin < edges.size();)
    {
        size_t end = begin + 1;
        while (end < edges.size() && edges[end].contour == edges[begin].contour) ++end;
        for (size_t i = begin; i < end; ++i)
        {
            const auto& edge = edges[i];
            const Point a{double(edge.x0), double(edge.y0)}, b{double(edge.x1), double(edge.y1)};
            const Point n = normal(edge);
            add({shift(a, n, -1), shift(b, n, -1), shift(b, n, 1), shift(a, n, 1)}, 4);
            const auto& following = edges[i + 1 == end ? begin : i + 1];
            if (following.x0 != edge.x1 || following.y0 != edge.y1) continue;
            const Point next = normal(following);
            const double denominator = 1 + n.x * next.x + n.y * next.y;
            Point miter{};
            if (denominator > 1e-8)
                miter = {(n.x + next.x) / denominator, (n.y + next.y) / denominator};
            const bool sharp = denominator > 1e-8 && miter.x * miter.x + miter.y * miter.y <= 4;
            const double side = n.x * next.y - n.y * next.x > 0 ? -1. : 1.;
            if (sharp) add({b, shift(b, n, side), shift(b, miter, side), shift(b, next, side)}, 4);
            else add({b, shift(b, n, side), shift(b, next, side), b}, 3);
        }
        begin = end;
    }
    if (points.empty())
    {
        mWideOutline = mOutline;
        return true;
    }
    Rasterizer strokes;
    if (!strokes.CopyPath(points.data(), types.data(), points.size()) || !strokes.ScanConvert())
        return false;
    const uint64_t offset = uint64_t(int64_t(strokes.mPathOffsetY) * 0x100000000ll + strokes.mPathOffsetX);
    mWideOutline.reserve(mOutline.size() + strokes.mOutline.size());
    MergeOutlineSpans(mOutline.cbegin(), mOutline.cend(), strokes.mOutline.cbegin(), strokes.mOutline.cend(),
        offset, offset, [&](uint64_t start, uint64_t end) { mWideOutline.emplace_back(start, end); });
    return true;
}

bool Rasterizer::CreateOutlineRings(const std::vector<OutlineTarget>& layers, Rasterizer* shadow)
{
    const tSpanBuffer* inner = &mOutline;
    tSpanBuffer previous;
    auto initMask = [this](Rasterizer& target) {
        target.mWidth = mWidth;
        target.mHeight = mHeight;
        target.mPathOffsetX = mPathOffsetX;
        target.mPathOffsetY = mPathOffsetY;
        target.mWideBorder = mWideBorder;
    };
    for (const auto& layer : layers)
    {
        mWideOutline.clear();
        if (!CreateWidenedRegion(layer.x, layer.y))
            return false;
        initMask(*layer.mask);
        SubtractOutlineSpans(mWideOutline, *inner, layer.mask->mOutline, 0);
        mWideOutline.swap(previous);
        inner = &previous;
    }
    if (shadow)
    {
        initMask(*shadow);
        shadow->mOutline.swap(previous);
    }
    mWideOutline.clear();
    mWideBorder = 0;
    return true;
}
#endif

void Rasterizer::DeleteOutlines()
{
    mWideOutline.clear();
    mOutline.clear();
}

bool Rasterizer::Rasterize(int xsub, int ysub, int fBlur, double fGaussianBlurX, double fGaussianBlurY,
    bool separateMask)
{
    PROFILE_RENDER_STAGE(Rasterize);
    if (activeOverlayDraws && mpOverlayBuffer)
    {
        std::unique_ptr<BYTE[]> previous(mpOverlayBuffer);
        mpOverlayBuffer = nullptr;
        activeOverlayDraws->retiredMasks.push_back(std::move(previous));
    }
    else
        _TrashOverlay();

    if(!mWidth || !mHeight)
    {
        mOverlayWidth = mOverlayHeight = 0;
        return true;
    }

    xsub &= 7;
    ysub &= 7;

    int width = mWidth + xsub;
    int height = mHeight;// + ysub

    mOffsetX = mPathOffsetX - xsub;
    mOffsetY = mPathOffsetY - ysub;

    mWideBorder = (mWideBorder + 7)&~7;

    double fGaussianBlur = max(fGaussianBlurX, fGaussianBlurY);

    int direction = 0;
    if (fGaussianBlurX != 0 && fGaussianBlurY == 0)
        direction = 1;
    else if (fGaussianBlurX == 0 && fGaussianBlurY != 0)
        direction = 2;

    if(!mWideOutline.empty() || separateMask || fBlur || fGaussianBlur > 0)
    {
        int bluradjust = 0;
        if(fGaussianBlur > 0)
            bluradjust += (int)(fGaussianBlur * 3 * 8 + 0.5) | 1;
        if(fBlur)
            bluradjust += 8;

        // Expand the buffer a bit when we're blurring, since that can also widen the borders a bit
        bluradjust = (bluradjust + 7)&~7;

        width += 2 * mWideBorder + bluradjust * 2;
        height += 2 * mWideBorder + bluradjust * 2;

        xsub += mWideBorder + bluradjust;
        ysub += mWideBorder + bluradjust;

        mOffsetX -= mWideBorder + bluradjust;
        mOffsetY -= mWideBorder + bluradjust;
    }

    mOverlayWidth = ((width + 7) >> 3) + 1;
    mOverlayHeight = ((height + 14) >> 3) + 1;

    mpOverlayBuffer = DNew byte[2 * mOverlayWidth * mOverlayHeight];
    memset(mpOverlayBuffer, 0, 2 * mOverlayWidth * mOverlayHeight);

    // Are we doing a border?

    tSpanBuffer* pOutline[2] = {&mOutline, &mWideOutline};

    for(ptrdiff_t i = countof(pOutline) - 1; i >= 0; i--)
    {
        tSpanBuffer::iterator it = pOutline[i]->begin();
        tSpanBuffer::iterator itEnd = pOutline[i]->end();

        for(; it != itEnd; ++it)
        {
            size_t y = (((*it).first >> 32) - 0x40000000 + ysub);
            size_t x1 = (((*it).first & 0xffffffff) - 0x40000000 + xsub);
            size_t x2 = (((*it).second & 0xffffffff) - 0x40000000 + xsub);

            if(x2 > x1)
            {
                size_t first = x1 >> 3;
                size_t last = (x2 - 1) >> 3;
                byte* dst = mpOverlayBuffer + 2 * (mOverlayWidth * (y >> 3) + first) + i;

                if(first == last)
                    *dst += x2 - x1;
                else
                {
                    *dst += ((first + 1) << 3) - x1;
                    dst += 2;

                    while(++first < last)
                    {
                        *dst += 0x08;
                        dst += 2;
                    }

                    *dst += x2 - (last << 3);
                }
            }
        }
    }

    PROFILE_RENDER_STAGE(Blur);
    // Do some gaussian blur magic
    if(fGaussianBlur > 0)
    {
        GaussianKernel filter(fGaussianBlur);
        if(mOverlayWidth >= filter.width && mOverlayHeight >= filter.width)
        {
            size_t pitch = mOverlayWidth * 2;

            byte *tmp = DNew byte[pitch*mOverlayHeight];
            if(!tmp) return(false);

            int border = !mWideOutline.empty() ? 1 : 0;

            byte *src = mpOverlayBuffer + border;

            

            SeparableFilterX<2>(src, tmp, mOverlayWidth, mOverlayHeight, pitch, filter.kernel, filter.width, filter.divisor, direction);
            SeparableFilterY<2>(tmp, src, mOverlayWidth, mOverlayHeight, pitch, filter.kernel, filter.width, filter.divisor, direction);

            delete[] tmp;
        }
    }

    // If we're blurring, do a 3x3 box blur
    // Can't do it on subpictures smaller than 3x3 pixels
    for(int pass = 0; pass < fBlur; pass++)
    {
        if(mOverlayWidth >= 3 && mOverlayHeight >= 3)
        {
            int pitch = mOverlayWidth * 2;

            byte* tmp = DNew byte[pitch*mOverlayHeight];
            if(!tmp) return(false);

            memcpy(tmp, mpOverlayBuffer, pitch * mOverlayHeight);

            int border = !mWideOutline.empty() ? 1 : 0;

            // This could be done in a separated way and win some speed
            for(ptrdiff_t j = 1; j < mOverlayHeight - 1; j++)
            {
                byte* src = tmp + pitch * j + 2 + border;
                byte* dst = mpOverlayBuffer + pitch * j + 2 + border;

                for(ptrdiff_t i = 1; i < mOverlayWidth - 1; i++, src += 2, dst += 2)
                {
                    *dst = (src[-2-pitch] + (src[-pitch] << 1) + src[+2-pitch]
                            + (src[-2] << 1) + (src[0] << 2) + (src[+2] << 1)
                            + src[-2+pitch] + (src[+pitch] << 1) + src[+2+pitch]) >> 4;
                }
            }

            delete [] tmp;
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////

static __forceinline void pixmix(DWORD *dst, DWORD color, DWORD alpha, MOD_BLEND mod_blendMode = BLEND_NORMAL)
{
    DWORD a = (((alpha) * (color >> 24)) >> 6) & 0xff;
    DWORD ia = 256 - a;
    a += 1;

    //DWORD tmp = (((((*dst >> 8) & 0x00ff0000) * ia) & 0xff000000) >> 24) & 0xFF;
    *dst = ((((*dst & 0x00ff00ff) * ia + (color & 0x00ff00ff) * a) & 0xff00ff00) >> 8)
           | ((((*dst & 0x0000ff00) * ia + (color & 0x0000ff00) * a) & 0x00ff0000) >> 8)
           | ((((*dst >> 8) & 0x00ff0000) * ia) & 0xff000000);
}

#include <xmmintrin.h>
#include <emmintrin.h>

static __forceinline void pixmix_sse2(DWORD* dst, DWORD color, DWORD alpha, MOD_BLEND mod_blendMode = BLEND_NORMAL)
{
    alpha = (((alpha) * (color >> 24)) >> 6) & 0xff;
    color &= 0xffffff;

    __m128i zero = _mm_setzero_si128();
    __m128i a = _mm_set1_epi32(((alpha + 1) << 16) | (0x100 - alpha));
    __m128i d = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*dst), zero);
    __m128i s = _mm_unpacklo_epi8(_mm_cvtsi32_si128(color), zero);
    __m128i r = _mm_unpacklo_epi16(d, s);

    r = _mm_madd_epi16(r, a);
    r = _mm_srli_epi32(r, 8);
    r = _mm_packs_epi32(r, r);
    r = _mm_packus_epi16(r, r);

    *dst = (DWORD)_mm_cvtsi128_si32(r);
}

// For CPUID usage in Rasterizer::Draw
#include "../dsutil/vd.h"

template<class Mixer>
static void BlendOverlay(RasterizerNfo& info, bool body, COverlayGetter* color, COverlayGetter* alpha)
{
    if (alpha)
        COverlayAlphaMixer<Mixer>(&info, color, alpha).Draw(body);
    else
        Mixer(&info, color).Draw(body);
}

#ifdef _VSMOD
// Each band makes its own sampler from immutable event paint and full bounds.
class COverlayKfxPaint : public COverlayGetter
{
    const OverlayDraw& draw;
    KfxPaint::Sampler samplers[2][2];
    struct DotGrid
    {
        DWORD color = 0;
        int period = 0, rows = 0, radius = 0;
        bool Sample(int x, int y) const
        {
            if (!period) return false;
            const int py = y * 16 + 8;
            const int row = py / rows;
            const int dy = py % rows - rows / 2;
            const int px = x * 16 + 8 + ((row & 1) ? period / 2 : 0);
            const int dx = px % period - period / 2;
            return int64_t(dx) * dx + int64_t(dy) * dy <= int64_t(radius) * radius;
        }
    } dots[2];

    int Channel(int slice) const
    {
        return draw.colorType ? draw.colorType : slice ^ int(draw.reversePaint);
    }

    DWORD Sample(int slice, int x, int y) const
    {
        if (draw.boundary.Active())
        {
            const double px = draw.bounds.left + x + .5;
            const double py = draw.bounds.bottom - y - .5;
            slice = px * draw.boundary.x + py * draw.boundary.y < draw.boundary.offset ? 0 : 1;
        }
        const auto& gradient = draw.gradient;
        const auto& rgb = samplers[slice][0];
        const auto& alpha = samplers[slice][1];
        const int channel = Channel(slice);
        const DWORD solid = draw.info.sw[slice * 2];
        const unsigned bits = gradient.legacyParts >> (2 * channel);
        const bool image = gradient.mode[channel] == 2;
        const bool previousShader = !gradient.paint &&
            (gradient.mode[draw.colorType] || (draw.colorType == 0 && gradient.mode[1]));
        const bool legacyRgb = !rgb.Active() && (previousShader || image || (bits & 1));
        const bool legacyAlpha = !alpha.Active() && (previousShader || image || (bits & 2));
        const DWORD legacy = (legacyRgb || legacyAlpha) && (gradient.mode[channel] || previousShader) ?
            gradient.getmixcolor(x, y, channel) : solid;
        DWORD color = (legacyRgb ? legacy : solid) & 0xFFFFFF;
        DWORD opacity = (legacyAlpha ? legacy : solid) >> 24;
        if (rgb.Active() || alpha.Active())
        {
            const double px = draw.bounds.left + x + 0.5;
            const double py = draw.bounds.bottom - y - 0.5;
            double u = 0, v = 0;
            if (rgb.UsesCorners() || alpha.UsesCorners())
            {
                u = KfxPaint::Unit(double(gradient.xoffset + x) / gradient.width);
                v = KfxPaint::Unit(1.0 - double(y) / gradient.height);
            }
            if (rgb.Active()) color = rgb.At(px, py, u, v);
            if (alpha.Active())
                opacity = ((255 - alpha.At(px, py, u, v)) * (255 - gradient.fadalpha) + 127) / 255;
        }
        if (dots[slice].Sample(gradient.xoffset + x, gradient.yoffset + draw.info.h - 1 - y))
            color = dots[slice].color;
        if (draw.paintFade.amount > 0)
            color = KfxPaint::Mix(color, draw.paintFade.color, draw.paintFade.amount, false, false);
        return color | (opacity << 24);
    }

public:
    explicit COverlayKfxPaint(const OverlayDraw& draw) : draw(draw)
    {
        if (!draw.gradient.paint)
            return;
        const auto& paint = *draw.gradient.paint;
        if (paint.dots && draw.colorType < 3)
            for (int slice = 0; slice < (draw.colorType ? 1 : 2); ++slice)
            {
                const unsigned target = paint.dotTarget >= 0 ? paint.dotTarget : Channel(slice);
                const auto layer = paint.dots->Layer(target);
                const auto period = (layer.spacing < 0 ? 2.5 * layer.diameter : layer.spacing) * draw.paintScaleY;
                if (layer.color >= 0 && layer.diameter > 0 && period > 0)
                    dots[slice] = {DWORD(layer.color), (std::max)(1, int(std::lround(period * 16))),
                        (std::max)(1, int(std::lround(period * 13.8564064606))),
                        (std::max)(1, int(std::lround(layer.diameter * draw.paintScaleY * 8)))};
            }
        for (int slice = 0; slice < (draw.colorType ? 1 : 2); ++slice)
            for (int component = 0; component < 2; ++component)
            {
                const auto& source = paint.parts[Channel(slice) * 2 + component];
                if (!source) continue;
                CRect rect = draw.paintBounds;
                if (draw.paintSegments)
                {
                    const auto found = draw.paintSegments->find((uint64_t(draw.gradient.paintGroup) << 32) | source->segment);
                    if (found != draw.paintSegments->end()) rect = found->second;
                }
                samplers[slice][component].Prepare(source.get(),
                    {double(rect.left), double(rect.top), double(rect.right), double(rect.bottom)},
                    draw.paintScaleX, draw.paintScaleY, component != 0, draw.gradient.paintOffsetY / 8.);
            }
    }

    DWORD getcolor1(int x, int y) override { return Sample(0, x, y); }
    DWORD getcolor2(int x, int y) override { return Sample(draw.colorType ? 0 : 1, x, y); }
};
#endif

void OverlayDraw::Blend(int top, int bottom) const
{
    const int first = (std::max)(top, int(bounds.top));
    const int last = (std::min)(bottom, int(bounds.bottom));
    if (first >= last)
        return;

    PROFILE_RENDER_STAGE(Composite);
    RasterizerNfo band = info;
    const ptrdiff_t skip = first - bounds.top;
    band.dst = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(band.dst) + skip * band.pitch);
    band.src += skip * 2 * band.overlayp;
    band.s += skip * 2 * band.overlayp;
    band.h = last - first;
    // The mixers count down from the bottom of the original clipped draw.
    band.rowOffset = bounds.bottom - last;

#ifdef _VSMOD
    const bool legacyShading = gradient.mode[colorType] || (colorType == 0 && gradient.mode[1]);
    auto fadeSolid = [&](DWORD source) {
        return (source & 0xFF000000u) |
            KfxPaint::Mix(source & 0xFFFFFF, paintFade.color, paintFade.amount, false, false);
    };
    COverlayColor solid(fadeSolid(band.sw[0]), fadeSolid(band.sw[2]));
    COverlayGetter* color = &solid;
    COverlayGradient shaded(gradient, colorType);
    if (legacyShading)
        color = &shaded;
    COverlayAlpha mask(maskPosition);
#else
    COverlayColor solid(band.sw[0], band.sw[2]);
    COverlayGetter* color = &solid;
    COverlayAlpha mask(alphaMask, maskPitch, info.h);
#endif
    auto* alpha = alphaMask ? &mask : nullptr;
#ifdef _VSMOD
    if (gradient.paint || boundary.Active() || (legacyShading && paintFade.amount > 0))
    {
        if (boundary.Active())
            band.sw[1] = 0xffffffff;
        else if (gradient.paint && band.sw[1] != 0xffffffff)
        {
            const int64_t split = int64_t(int32_t(band.sw[3])) - band.xo;
            band.sw[3] = split >= 0 ? DWORD(split) : DWORD(-1);
            band.xo = 0;
        }
        if (gradient.paint && !body && alpha)
            band.s = band.src;
        COverlayKfxPaint extended(*this);
        if (g_cpuid.m_flags & CCpuID::sse2)
            BlendOverlay<COverlayMixerSSE2>(band, body, &extended, alpha);
        else
            BlendOverlay<COverlayMixer>(band, body, &extended, alpha);
#ifndef _M_X64
        _mm_empty();
#endif
        return;
    }
#endif
    if (g_cpuid.m_flags & CCpuID::sse2)
        BlendOverlay<COverlayMixerSSE2>(band, body, color, alpha);
    else
        BlendOverlay<COverlayMixer>(band, body, color, alpha);
#ifndef _M_X64
    _mm_empty();
#endif
}

#ifdef _VSMOD
namespace
{
struct PositionedDraw
{
    struct Pixel { DWORD color = 0; BYTE coverage = 0; };
    const OverlayDraw& draw;
    COverlayKfxPaint color;
    KfxPaint::Sampler opacity;
    std::vector<Pixel> row;
    size_t scope;
    int divider;

    PositionedDraw(const OverlayDraw& source, size_t scopeIndex)
        : draw(source), color(source), row(source.bounds.Width()), scope(scopeIndex),
          divider(source.info.sw[1] == DWORD(-1) ? source.bounds.Width() :
            int((std::max)(int64_t(0), (std::min)(int64_t(source.bounds.Width()),
                int64_t(int32_t(source.info.sw[3])) + 1 - source.info.xo))))
    {
        opacity.Prepare(draw.gradient.positionedOpacity.get(), {}, draw.paintScaleX, draw.paintScaleY, true,
            draw.gradient.paintOffsetY / 8.);
    }

    void Sample(int y, int left, float* above)
    {
        const int fromBottom = draw.bounds.bottom - y - 1;
        const auto offset = 2 * ptrdiff_t(draw.info.overlayp) * (y - draw.bounds.top);
        const auto* source = draw.info.src + offset;
        const auto* selected = draw.info.s + offset;
        if (!draw.body && draw.alphaMask && !draw.gradient.paint)
            source = selected;
        for (int x = 0; x < int(row.size()); ++x)
        {
            auto& pixel = row[x];
            pixel.coverage = draw.body ? selected[2 * x] :
                BYTE((std::max)(0, source[2 * x + 1] - source[2 * x]));
            if (draw.alphaMask && pixel.coverage)
                pixel.coverage = BYTE(pixel.coverage * draw.maskPosition.GetAlphaValue(x, fromBottom) >> 6);
            if (!pixel.coverage) continue;
            pixel.color = x < divider ? color.getcolor1(x, fromBottom) : color.getcolor2(x, fromBottom);
            const unsigned original = pixel.coverage * (pixel.color >> 24) >> 6;
            const int screenX = draw.bounds.left + x;
            auto& remaining = above[screenX - left];
            const auto inverse = opacity.At(screenX + 0.5, y + 0.5, 0, 0);
            if (inverse)
            {
                // Allocate this source's share of the masked finished paint.
                // The sources above it must still attenuate it in painter order.
                const double k = (255 - inverse) / 255.0;
                const auto adjusted = unsigned(std::lround(original * k * remaining / (1 - k + k * remaining)));
                pixel.coverage = adjusted ? 64 : 0;
                pixel.color = (pixel.color & 0xffffff) | (adjusted << 24);
            }
            remaining *= 1 - original / 255.f;
        }
    }

    template<class Mixer>
    void Blend(int y, Mixer& mixer) const
    {
        auto* dst = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(draw.info.dst) +
            ptrdiff_t(y - draw.bounds.top) * draw.info.pitch);
        for (int x = 0; x < int(row.size()); ++x)
            if (row[x].coverage)
                mixer.PixMix(dst + x, row[x].color, row[x].coverage, draw.info.mod_blendMode);
    }
};

void BlendPositionedOverlays(const OverlayDraw* const* draws, size_t count, int top, int bottom)
{
    PROFILE_RENDER_STAGE(Composite);
    int left = INT_MAX, right = INT_MIN;
    int firstRow = bottom, lastRow = top;
    std::vector<std::pair<const KfxPaint::Paint*, unsigned>> scopes;
    std::vector<std::unique_ptr<PositionedDraw>> positioned(count);
    for (size_t i = 0; i < count; ++i)
    {
        const auto& draw = *draws[i];
        firstRow = (std::min)(firstRow, int(draw.bounds.top));
        lastRow = (std::max)(lastRow, int(draw.bounds.bottom));
        const auto* mask = draw.gradient.positionedOpacity.get();
        if (!mask) continue;
        const auto key = std::make_pair(mask, draw.gradient.paintGroup);
        const auto old = std::find(scopes.begin(), scopes.end(), key);
        const size_t scope = old - scopes.begin();
        if (old == scopes.end()) scopes.push_back(key);
        positioned[i] = std::make_unique<PositionedDraw>(draw, scope);
        left = (std::min)(left, int(draw.bounds.left));
        right = (std::max)(right, int(draw.bounds.right));
    }
    if (scopes.empty())
    {
        for (size_t i = 0; i < count; ++i) draws[i]->Blend(top, bottom);
        return;
    }
    const size_t width = right - left;
    std::vector<float> above(width * scopes.size());
    RasterizerNfo unused{};
    COverlayMixer scalar(&unused, nullptr);
    COverlayMixerSSE2 simd(&unused, nullptr);
    const bool sse2 = (g_cpuid.m_flags & CCpuID::sse2) != 0;
    for (int y = (std::max)(top, firstRow); y < (std::min)(bottom, lastRow); ++y)
    {
        std::fill(above.begin(), above.end(), 1.f);
        for (size_t i = count; i-- > 0;)
            if (positioned[i] && draws[i]->bounds.top <= y && draws[i]->bounds.bottom > y)
                positioned[i]->Sample(y, left, above.data() + positioned[i]->scope * width);
        for (size_t i = 0; i < count; ++i)
        {
            if (draws[i]->bounds.top > y || draws[i]->bounds.bottom <= y) continue;
            if (!positioned[i])
                draws[i]->Blend(y, y + 1);
            else if (sse2)
                positioned[i]->Blend(y, simd);
            else
                positioned[i]->Blend(y, scalar);
        }
    }
#ifndef _M_X64
    _mm_empty();
#endif
}
}

void BlendCapturedBands(const std::vector<const OverlayDraw*>& draws, int top, int bottom)
{
    for (size_t i = 0; i < draws.size();)
    {
        const auto* owner = draws[i]->captureOwner;
        if (!owner || !owner->hasPositionedAlpha)
        {
            draws[i++]->Blend(top, bottom);
            continue;
        }
        size_t end = i + 1;
        while (end < draws.size() && draws[end]->captureOwner == owner) ++end;
        BlendPositionedOverlays(draws.data() + i, end - i, top, bottom);
        i = end;
    }
}
#endif

void OverlayDrawList::Blend() const
{
#ifdef _VSMOD
    if (hasPositionedAlpha)
    {
        std::vector<const OverlayDraw*> commands;
        commands.reserve(draws.size());
        for (const auto& draw : draws) commands.push_back(&draw);
        BlendPositionedOverlays(commands.data(), commands.size(), 0, INT_MAX);
        return;
    }
#endif
    for (const auto& draw : draws)
        draw.Blend(draw.bounds.top, draw.bounds.bottom);
}

// Render a subpicture onto a surface.
// spd is the surface to render on.
// clipRect is a rectangular clip region to render inside.
// pAlphaMask is an alpha clipping mask.
// xsub and ysub ???
// switchpts seems to be an array of fill colours interlaced with coordinates.
//    switchpts[i*2] contains a colour and switchpts[i*2+1] contains the coordinate to use that colour from
// fBody tells whether to render the body of the subs.
// fBorder tells whether to render the border of the subs.
#ifdef _VSMOD // patch m004. gradient colors
CRect Rasterizer::Draw(SubPicDesc& spd, CRect& clipRect, byte* pAlphaMask, int xsub, int ysub,
                       const DWORD* switchpts, bool fBody, bool fBorder, int typ, MOD_GRADIENT& mod_grad, MOD_MOVEVC& mod_vc, MOD_BLEND mod_blendMode, bool reversePaint, bool applyPositionedAlpha,
                       const KfxPaint::PaintBoundary* boundary)
#else
CRect Rasterizer::Draw(SubPicDesc& spd, CRect& clipRect, byte* pAlphaMask, int xsub, int ysub,
                       const DWORD* switchpts, bool fBody, bool fBorder)
#endif
{
    CRect bbox(0, 0, 0, 0);

    if(!mpOverlayBuffer || !switchpts || !fBody && !fBorder) return(bbox);

    // Limit drawn area to intersection of rendering surface and rectangular clip area
    CRect r(0, 0, spd.w, spd.h);
    r &= clipRect;

    // Remember that all subtitle coordinates are specified in 1/8 pixels
    // (x+4)>>3 rounds to nearest whole pixel.
    // ??? What is xsub, ysub, mOffsetX and mOffsetY ?
    int x = (xsub + mOffsetX + 4) >> 3;
    int y = (ysub + mOffsetY + 4) >> 3;
    int w = mOverlayWidth;
    int h = mOverlayHeight;
    int xo = 0, yo = 0;

    int yBeforeClip = y;

#ifdef _VSMOD
    CRect paintBounds(0, 0, 0, 0);
    if (mod_grad.paint)
    {
        const auto& parts = mod_grad.paint->parts;
        const int last = typ ? 2 * typ + 2 : 4;
        const int first = 2 * typ;
        bool hasField = false;
        for (int part = first; part < last; ++part)
            hasField = hasField || (parts[part] && !parts[part]->positioned);
        if (hasField)
        {
            PROFILE_RENDER_STAGE(PaintBounds);
            int left = w, right = 0, top = h, bottom = 0;
            const int offset = fBorder ? 1 : 0;
            for (int row = 0; row < h; ++row)
            {
                const auto* mask = mpOverlayBuffer + 2 * ptrdiff_t(w) * row + offset;
                int begin = 0;
                while (begin < w && !mask[2 * begin]) ++begin;
                if (begin == w) continue;
                int end = w;
                while (end > begin && !mask[2 * (end - 1)]) --end;
                left = (std::min)(left, begin);
                right = (std::max)(right, end);
                top = (std::min)(top, row);
                bottom = row + 1;
            }
            if (top < bottom)
                paintBounds.SetRect(x + left, y + top, x + right, y + bottom);
            else
                paintBounds.SetRect(x, y, x+w, y+h);
            if (activeOverlayDraws && top < bottom)
                for (int part = first; part < last; ++part)
                    if (parts[part] && !parts[part]->positioned)
                    {
                        auto& segments = activeOverlayDraws->paintSegments;
                        const auto key = (uint64_t(mod_grad.paintGroup) << 32) | parts[part]->segment;
                        const auto inserted = segments.emplace(key, paintBounds);
                        if (!inserted.second) inserted.first->second |= paintBounds;
                    }
        }
    }
#endif

    // Again, limiting?
    if(x < r.left)
    {
        xo = r.left - x;
        w -= r.left - x;
        x = r.left;
    }
    if(y < r.top)
    {
        yo = r.top - y;
        h -= r.top - y;
        y = r.top;
    }
    if(x + w > r.right) w = r.right - x;
    if(y + h > r.bottom) h = r.bottom - y;

    // Check if there's actually anything to render
    if(w <= 0 || h <= 0) return(bbox);

    bbox.SetRect(x, y, x + w, y + h);
    bbox &= CRect(0, 0, spd.w, spd.h);

    OverlayDraw draw;
    draw.bounds = bbox;
    draw.body = fBody;
    auto& info = draw.info;
    info.dst = reinterpret_cast<DWORD*>(static_cast<BYTE*>(spd.bits) + ptrdiff_t(spd.pitch) * y) + x;
    std::copy_n(switchpts, 4, info.sw);
    info.w = w;
    info.h = h;
    info.xo = xo;
    info.overlayp = mOverlayWidth;
    info.pitch = spd.pitch;
    info.src = mpOverlayBuffer + 2 * (ptrdiff_t(mOverlayWidth) * yo + xo);
    info.s = fBorder ? info.src + 1 : info.src;
    draw.alphaMask = pAlphaMask ? pAlphaMask + ptrdiff_t(spd.w) * y + x : nullptr;
#ifdef _VSMOD
    draw.maskPosition = mod_vc;
    draw.maskPosition.hfull = h;
    draw.maskPosition.curpos = CPoint(x, y);
    draw.maskPosition.alphamask = draw.alphaMask;
    draw.gradient = mod_grad;
    if (!applyPositionedAlpha) draw.gradient.positionedOpacity.reset();
    draw.gradient.width = mOverlayWidth;
    draw.gradient.height = mOverlayHeight;
    draw.gradient.xoffset = xo;
    draw.gradient.yoffset = yo;
    draw.gradient.clipDiff = (std::max)(0, yBeforeClip + mOverlayHeight - int(clipRect.bottom));
    draw.colorType = typ;
    if (boundary) draw.boundary = *boundary;
    draw.reversePaint = reversePaint && mod_grad.paint;
    if (mod_grad.paint)
    {
        draw.paintBounds = paintBounds;
        draw.paintSegments = activeOverlayDraws ? &activeOverlayDraws->paintSegments : nullptr;
    }
    if (activeKfxPaint)
    {
        draw.paintFade = activeKfxPaint->fade;
        draw.paintScaleX = activeKfxPaint->scaleX;
        draw.paintScaleY = activeKfxPaint->scaleY;
    }
    info.mod_blendMode = mod_blendMode;
#else
    draw.maskPitch = spd.w;
#endif

#if defined (_VSMOD) && defined(_LUA)
    if (LuaRendererHandler.GetLength() > 0)
    {
        // Lua can read and write pixels in this draw. The caller runs the
        // callback in order, with the full clipped rectangle.
        ASSERT(!activeOverlayDraws);
        PROFILE_RENDER_STAGE(Composite);
        COverlayColor solid(info.sw[0], info.sw[2]);
        COverlayGradient shaded(draw.gradient, typ);
        auto* color = (draw.gradient.mode[typ] || (typ == 0 && draw.gradient.mode[1]))
            ? static_cast<COverlayGetter*>(&shaded) : &solid;
        COverlayAlpha mask(draw.maskPosition);
        auto* alpha = draw.alphaMask ? &mask : nullptr;
        auto runCallback = [&](auto& mix) {
            mix.L = L;
            mix.LuaLog = LuaLog;
            mix.m_entry = m_entry;
            mix.m_layer = m_layer;
            mix.Function = LuaRendererHandler;
            mix.Draw(fBody);
        };
        if (g_cpuid.m_flags & CCpuID::sse2)
        {
            COverlayLuaMixer<COverlayMixerSSE2> mix(&info, color, alpha);
            runCallback(mix);
        }
        else
        {
            COverlayLuaMixer<COverlayMixer> mix(&info, color, alpha);
            runCallback(mix);
        }
#ifndef _M_X64
        _mm_empty();
#endif
        return bbox;
    }
#endif

    if (activeOverlayDraws)
    {
#ifdef _VSMOD
        draw.captureOwner = activeOverlayDraws;
        if (draw.gradient.positionedOpacity) activeOverlayDraws->hasPositionedAlpha = true;
#endif
        activeOverlayDraws->draws.push_back(std::move(draw));
    }
#ifdef _VSMOD
    else if (draw.gradient.positionedOpacity)
    {
        const OverlayDraw* command = &draw;
        BlendPositionedOverlays(&command, 1, bbox.top, bbox.bottom);
    }
#endif
    else
        draw.Blend(bbox.top, bbox.bottom);
    return bbox;
}


#if 0
void Rasterizer::FillSolidRect(SubPicDesc& spd, int x, int y, int nWidth, int nHeight, DWORD lColor)
{
    bool fSSE2 = !!(g_cpuid.m_flags & CCpuID::sse2);

    // Warning : lColor is AARRGGBB (same format as DirectX D3DCOLOR_ARGB)
    for(int wy = y; wy < y + nHeight; wy++)
    {
        DWORD* dst = (DWORD*)((BYTE*)spd.bits + spd.pitch * wy) + x;

        memsetd(dst, lColor, nWidth * 4);
    }
}
#else
void Rasterizer::FillSolidRect(SubPicDesc& spd, int x, int y, int nWidth, int nHeight, DWORD lColor)
{
    bool fSSE2 = !!(g_cpuid.m_flags & CCpuID::sse2);

    for(int wy = y; wy < y + nHeight; wy++)
    {
        DWORD* dst = (DWORD*)((BYTE*)spd.bits + spd.pitch * wy) + x;

        if(fSSE2)
            for(int wt = 0; wt < nWidth; ++wt)
                pixmix_sse2(&dst[wt], lColor, 0x40);	// 0x40 because >> 6 in pixmix (to preserve tranparency)
        else
            for(int wt = 0; wt < nWidth; ++wt)
                pixmix(&dst[wt], lColor, 0x40);
    }
}
#endif

#ifdef _VSMOD // patch m006. moveable vector clip
MOD_MOVEVC::MOD_MOVEVC()
{
    clear();
}

void MOD_MOVEVC::clear()
{
    enable = false;
    size = CSize(0, 0);
    pos = CPoint(0, 0);
//	canvas = CSize(0,0);
    spd = CSize(0, 0);
    curpos = CPoint(0, 0);
    hfull = 0;
    alphamask = NULL;
    isInverse = false;
}

byte MOD_MOVEVC::GetAlphaValue(int wx, int wy) const
{
    PROFILE_RENDER_STAGE(PaintClip);
    byte alpham;
    if(!enable)
    {
//		return 0xFF;
        if((wx < 0) || (wx >= spd.cx)) return 0;
        if((wy <= 0) || (wy > spd.cy)) return 0;

        return alphamask[wx+((hfull-wy)*spd.cx)];
    }

    int xInCanvas = wx - pos.x + curpos.x;
    int yInCanvas = -pos.y + (hfull - wy) + curpos.y;

    //check if the point is in canvas, in case of crash
    if (xInCanvas < 0 || xInCanvas >= spd.cx)
        alpham = isInverse ? 0x40 : 0; // assume content outside canvas is visible when using iclip. mask is 6 bit
    else if (yInCanvas < 0 || yInCanvas >= spd.cy)
        alpham = isInverse ? 0x40 : 0;
    else
    {
        if ((wx - pos.x) < -curpos.x + 1) alpham = 0;
        else if ((wx - pos.x) > spd.cx - 1) alpham = 0; //canvas.cx
        else if ((hfull - wy - pos.y) < -curpos.y + 1) alpham = 0;
        else if ((hfull - wy - pos.y) > spd.cy - 1) alpham = 0;
        else alpham = alphamask[wx - pos.x - pos.y*spd.cx + ((hfull - wy)*spd.cx)];
    }

    return alpham;
}
#endif
