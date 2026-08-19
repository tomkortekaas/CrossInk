#pragma once

#include <cstdint>

namespace dashboard {

// Een dikke boog, pixel voor pixel via een callback. Losgekoppeld van
// GfxRenderer om twee redenen: zo is hij op de host te testen, en zo blijft de
// fork mergebaar met upstream.
//
// GfxRenderer::drawArc bestaat wel maar tekent kwadranten via xDir/yDir - geen
// willekeurige hoek, dus geen 51%.
//
// Schermcoordinaten, y naar beneden: punt(t) = (cx + r*cos t, cy + r*sin t),
// hoeken in graden vanaf de positieve x-as, met de klok mee.
//
// Een functiepointer plus context in plaats van std::function, per de
// resource-regels in CLAUDE.md.
void forEachArcPixel(int centerX, int centerY, int radius, int thickness, int startDegrees,
                     int sweepDegrees, void (*plot)(int x, int y, void* context), void* context);

// De veeg die bij een vulling van 0..100 hoort, begrensd op de volle veeg.
// Een waarde boven 100 mag de boog niet voorbij zijn eindpunt laten lopen.
int arcSweepForFill(uint8_t fill, int fullSweepDegrees);

}  // namespace dashboard
