/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2022 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "chordarticulationsrenderer.h"

#include <cstdlib>
#include <vector>

#include "bendsrenderer.h"
#include "noterenderer.h"
#include "gracechordcontext.h"
#include "ornamentsrenderer.h"
#include "tremolorenderer.h"
#include "arpeggiorenderer.h"

#include "playback/utils/expressionutils.h"

using namespace mu::engraving;
using namespace muse;
using namespace muse::mpe;

const mpe::ArticulationTypeSet& ChordArticulationsRenderer::supportedTypes()
{
    static mpe::ArticulationTypeSet SUPPORTED_TYPES;

    if (SUPPORTED_TYPES.empty()) {
        SUPPORTED_TYPES = GRACE_NOTE_ARTICULATION_TYPES;

        SUPPORTED_TYPES.insert(OrnamentsRenderer::supportedTypes().cbegin(),
                               OrnamentsRenderer::supportedTypes().cend());
        SUPPORTED_TYPES.insert(TremoloRenderer::supportedTypes().cbegin(),
                               TremoloRenderer::supportedTypes().cend());
        SUPPORTED_TYPES.insert(ArpeggioRenderer::supportedTypes().cbegin(),
                               ArpeggioRenderer::supportedTypes().cend());
    }

    return SUPPORTED_TYPES;
}

void ChordArticulationsRenderer::doRender(const EngravingItem* item, const mpe::ArticulationType /*type*/, const RenderingContext& ctx,
                                          mpe::PlaybackEventList& result)
{
    IF_ASSERT_FAILED(item->isChord()) {
        return;
    }

    for (const auto& type : ctx.commonArticulations) {
        if (muse::contains(GRACE_NOTE_ARTICULATION_TYPES, type.first)) {
            renderChordWithGraceChords(toChord(item), type.first, ctx, result);
            return;
        }
    }

    renderChord(toChord(item), ctx, result);
}

void ChordArticulationsRenderer::renderChord(const Chord* chord, const RenderingContext& ctx, muse::mpe::PlaybackEventList& result)
{
    if (renderChordArticulations(chord, ctx, result)) {
        return;
    }

    const bool supportsMultibend = ctx.profile->contains(ArticulationType::Multibend);

    for (const Note* note: chord->notes()) {
        if (supportsMultibend && BendsRenderer::isMultibendPart(note)) {
            BendsRenderer::render(note, ctx, result);
        } else {
            NoteRenderer::render(note, ctx, result);
        }
    }
}

// Grace chords are rendered with a copy of the principal chord's context.
// Ornaments (trills etc.), tremolos and arpeggios belong to the principal chord
// only; without this, a grace note before a trilled note is trilled itself.
static RenderingContext graceCtxWithoutPrincipalOnlyArticulations(const RenderingContext& ctx)
{
    RenderingContext result(ctx);

    std::vector<muse::mpe::ArticulationType> toErase;
    for (auto it = result.commonArticulations.cbegin(); it != result.commonArticulations.cend(); ++it) {
        const muse::mpe::ArticulationType type = it->first;
        if (OrnamentsRenderer::isAbleToRender(type) || TremoloRenderer::isAbleToRender(type)
            || ArpeggioRenderer::isAbleToRender(type)) {
            toErase.push_back(type);
        }
    }

    if (toErase.empty()) {
        return result;
    }

    for (const muse::mpe::ArticulationType type : toErase) {
        result.commonArticulations.erase(type);
    }
    result.commonArticulations.preCalculateAverageData();

    return result;
}

// The principal chord's ornament/tremolo/arpeggio spans the whole written note, grace notes
// included. Now that the grace notes no longer carry it, let it span only the principal
// chord's own (shortened) time; otherwise its first/last sub-notes never reach 0%/100% of
// the articulation and Muse Sounds never gets the start/end of the trill range.
static RenderingContext principalCtxWithRebasedArticulations(const RenderingContext& original, const RenderingContext& principal)
{
    RenderingContext result(principal);

    const muse::mpe::timestamp_t origStart = original.nominalTimestamp;
    const muse::mpe::timestamp_t origEnd = original.nominalTimestamp + original.nominalDuration;
    const muse::mpe::timestamp_t newStart = principal.nominalTimestamp;
    const muse::mpe::timestamp_t newEnd = principal.nominalTimestamp + principal.nominalDuration;
    auto near = [](muse::mpe::timestamp_t a, muse::mpe::timestamp_t b) { return std::llabs(a - b) <= 1000; };

    std::vector<muse::mpe::ArticulationType> types;
    for (auto it = result.commonArticulations.cbegin(); it != result.commonArticulations.cend(); ++it) {
        const muse::mpe::ArticulationType type = it->first;
        if (OrnamentsRenderer::isAbleToRender(type) || TremoloRenderer::isAbleToRender(type)
            || ArpeggioRenderer::isAbleToRender(type)) {
            types.push_back(type);
        }
    }

    for (const muse::mpe::ArticulationType type : types) {
        muse::mpe::ArticulationMeta& meta = result.commonArticulations.at(type).meta;
        const muse::mpe::timestamp_t start = meta.timestamp;
        const muse::mpe::timestamp_t end = meta.timestamp + meta.overallDuration;
        const muse::mpe::timestamp_t s = near(start, origStart) ? newStart : start;
        const muse::mpe::timestamp_t e = near(end, origEnd) ? newEnd : end;
        if (e > s && (s != start || e != end)) {
            meta.timestamp = s;
            meta.overallDuration = e - s;
        }
    }

    return result;
}

void ChordArticulationsRenderer::renderChordWithGraceChords(const Chord* chord, const muse::mpe::ArticulationType type,
                                                            const RenderingContext& ctx,
                                                            muse::mpe::PlaybackEventList& result)
{
    const GraceChordCtx graceChordCtx = GraceChordCtx::buildCtx(chord, type, ctx);

    if (isGraceNotePlacedBeforePrincipalNote(type)) {
        for (const auto& pair : graceChordCtx.graceChordCtxList) {
            renderChord(pair.first, graceCtxWithoutPrincipalOnlyArticulations(pair.second), result);
        }
        renderChord(chord, principalCtxWithRebasedArticulations(ctx, graceChordCtx.principalChordCtx), result);
    } else {
        renderChord(chord, principalCtxWithRebasedArticulations(ctx, graceChordCtx.principalChordCtx), result);
        for (const auto& pair : graceChordCtx.graceChordCtxList) {
            renderChord(pair.first, graceCtxWithoutPrincipalOnlyArticulations(pair.second), result);
        }
    }
}

bool ChordArticulationsRenderer::renderChordArticulations(const Chord* chord, const RenderingContext& ctx,
                                                          mpe::PlaybackEventList& result)
{
    for (const auto& pair : ctx.commonArticulations) {
        const mpe::ArticulationType type = pair.first;

        if (OrnamentsRenderer::isAbleToRender(type)) {
            OrnamentsRenderer::render(chord, type, ctx, result);
            return true;
        }

        if (TremoloRenderer::isAbleToRender(type)) {
            TremoloRenderer::render(chord, type, ctx, result);
            return true;
        }

        if (ArpeggioRenderer::isAbleToRender(type)) {
            ArpeggioRenderer::render(chord, type, ctx, result);
            return true;
        }
    }

    return false;
}
