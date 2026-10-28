/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

#include "tremolorenderer.h"

#include "dom/chord.h"
#include "dom/tremolosinglechord.h"
#include "dom/tremolotwochord.h"

#include "playback/metaparsers/notearticulationsparser.h"
#include "playback/utils/expressionutils.h"
#include "playback/utils/repeatutils.h"

#include "noterenderer.h"

using namespace mu::engraving;
using namespace muse;
using namespace muse::mpe;

static bool containEqualTremolo(const Chord* c1, const Chord* c2)
{
    if (c1->tremoloSingleChord() && c2->tremoloSingleChord()) {
        return c1->tremoloSingleChord()->tremoloType() == c2->tremoloSingleChord()->tremoloType();
    }

    if (c1->tremoloTwoChord() && c2->tremoloTwoChord()) {
        return c1->tremoloTwoChord()->tremoloType() == c2->tremoloTwoChord()->tremoloType();
    }

    return false;
}

static timestamp_t tremoloTimestampFrom(const Note* note, const RenderingContext& ctx)
{
    const Note* originalNote = note;
    std::unordered_set<const Note*> notes { note };
    int positionTickOffset = ctx.positionTickOffset;

    if (note->incomingPartialTie()) {
        const PartiallyTiedNoteInfo outgoingNoteInfo = findOutgoingNoteInPreviousRepeat(note, ctx);
        if (outgoingNoteInfo.isValid() && containEqualTremolo(outgoingNoteInfo.note->chord(), note->chord())) {
            note = outgoingNoteInfo.note;
            positionTickOffset = outgoingNoteInfo.repeat->utick - outgoingNoteInfo.repeat->tick;
        }
    }

    while (note && note->tieBackNonPartial() && note->tieBackNonPartial()->playSpanner()) {
        const Note* prevNote = note->tieBackNonPartial()->startNote();
        const Chord* prevChord = prevNote ? prevNote->chord() : nullptr;

        if (!prevChord || !containEqualTremolo(prevChord, note->chord())) {
            break;
        }

        if (muse::contains(notes, prevNote)) {
            break; // prevents infinite loop
        }

        note = prevNote;
        notes.insert(note);
    }

    if (note && originalNote != note) {
        return timestampFromTicks(ctx.score, note->tick().ticks() + positionTickOffset);
    }

    return ctx.nominalTimestamp;
}

static duration_t tremoloDuration(const Note* note, const timestamp_t tremoloTimestampFrom, const RenderingContext& ctx)
{
    std::unordered_set<const Note*> notes { note };
    int positionTickOffset = ctx.positionTickOffset;

    if (note->outgoingPartialTie()) {
        const PartiallyTiedNoteInfo incomingNoteInfo = findIncomingNoteInNextRepeat(note, ctx);
        if (incomingNoteInfo.isValid() && containEqualTremolo(incomingNoteInfo.note->chord(), note->chord())) {
            note = incomingNoteInfo.note;
            positionTickOffset = incomingNoteInfo.repeat->utick - incomingNoteInfo.repeat->tick;
        }
    }

    while (note && note->tieForNonPartial() && note->tieForNonPartial()->playSpanner()) {
        const Note* nextNote = note->tieForNonPartial()->endNote();
        const Chord* nextChord = nextNote ? nextNote->chord() : nullptr;

        if (!nextChord || !containEqualTremolo(nextChord, note->chord())) {
            break;
        }

        if (muse::contains(notes, nextNote)) {
            break; // prevents infinite loop
        }

        note = nextNote;
        notes.insert(note);
    }

    if (note) {
        const int tickTo = note->chord()->endTick().ticks();
        const timestamp_t tremoloTimestampTo = timestampFromTicks(ctx.score, tickTo + positionTickOffset);
        return tremoloTimestampTo - tremoloTimestampFrom;
    }

    return ctx.nominalDuration;
}

// Articulations that happen once per written note, not once per repeated tremolo note: attacks
// and bowings (start of the tremolo), and release gestures (end of the tremolo). Techniques that
// last the whole note (mute, sul pont., harmonics, pizz., vibrato, noteheads...) stay on every step.
static const ArticulationTypeSet& tremoloOnsetOnlyTypes()
{
    static const ArticulationTypeSet types = {
        ArticulationType::Staccato, ArticulationType::Staccatissimo, ArticulationType::Tenuto,
        ArticulationType::Marcato, ArticulationType::Accent, ArticulationType::SoftAccent,
        ArticulationType::Subito, ArticulationType::FadeIn,
        ArticulationType::UpBow, ArticulationType::DownBow, ArticulationType::Detache,
        ArticulationType::Martele, ArticulationType::Jete, ArticulationType::SnapPizzicato,
        ArticulationType::Scoop, ArticulationType::Plop, ArticulationType::BrassBend,
        ArticulationType::SlideInAbove, ArticulationType::SlideInBelow,
    };
    return types;
}

static const ArticulationTypeSet& tremoloReleaseOnlyTypes()
{
    static const ArticulationTypeSet types = {
        ArticulationType::FadeOut, ArticulationType::LaissezVibrer,
        ArticulationType::Fall, ArticulationType::QuickFall, ArticulationType::Doit,
        ArticulationType::SlideOutDown, ArticulationType::SlideOutUp,
    };
    return types;
}

const ArticulationTypeSet& TremoloRenderer::supportedTypes()
{
    static const mpe::ArticulationTypeSet types = {
        mpe::ArticulationType::Tremolo8th, mpe::ArticulationType::Tremolo16th,
        mpe::ArticulationType::Tremolo32nd, mpe::ArticulationType::Tremolo64th,
        mpe::ArticulationType::TremoloBuzz,
    };

    return types;
}

void TremoloRenderer::doRender(const EngravingItem* item, const mpe::ArticulationType preferredType,
                               const RenderingContext& ctx,
                               mpe::PlaybackEventList& result)
{
    const Chord* chord = item_cast<const Chord*>(item);
    IF_ASSERT_FAILED(chord) {
        return;
    }

    struct TremoloAdapter {
        const TremoloSingleChord* single = nullptr;
        const TremoloTwoChord* two = nullptr;

        TremoloAdapter(const Chord* ch)
            : single(ch->tremoloSingleChord()), two(ch->tremoloTwoChord()) {}

        bool hasTremolo() const { return single != nullptr || two != nullptr; }

        int lines() const { return single ? single->lines() : (two ? two->lines() : 0); }
    };

    TremoloAdapter tremolo = TremoloAdapter(chord);
    IF_ASSERT_FAILED(tremolo.hasTremolo()) {
        return;
    }

    // TODO: We need a member like articulationData.overallDurationTicks (ticks rather than duration),
    // so that we are not duplicating this calculation (see TremoloTwoMetaParser::doParse)
    //const ArticulationAppliedData& articulationData = context.commonArticulations.at(preferredType);
    int overallDurationTicks = ctx.nominalDurationTicks;
    if (tremolo.two && tremolo.two->chord1() && tremolo.two->chord2()) {
        overallDurationTicks = tremolo.two->chord1()->actualTicks().ticks() + tremolo.two->chord2()->actualTicks().ticks();
    }

    int stepDurationTicks = 0;
    if (preferredType == ArticulationType::TremoloBuzz) {
        stepDurationTicks = overallDurationTicks;
    } else {
        stepDurationTicks = TremoloRenderer::stepDurationTicks(chord, tremolo.lines());
    }

    if (stepDurationTicks <= 0) {
        LOGE() << "Unable to render unsupported tremolo type";
        return;
    }

    // ... and use that here
    const int stepsCount = std::round(overallDurationTicks / (float)stepDurationTicks);
    if (stepsCount == 0) {
        return;
    }

    stepDurationTicks = overallDurationTicks / stepsCount;

    TremoloTimeCache tremoloTimeCache;

    if (tremolo.two) {
        const Chord* firstTremoloChord = tremolo.two->chord1();
        const Chord* secondTremoloChord = tremolo.two->chord2();

        IF_ASSERT_FAILED(firstTremoloChord && secondTremoloChord) {
            return;
        }

        for (int i = 0; i < stepsCount; ++i) {
            const Chord* currentChord = firstTremoloChord;

            if (i % 2 != 0) {
                currentChord = secondTremoloChord;
            }

            // Each chord's first appearance starts it, its last appearance ends it.
            const bool firstOfChord = i < 2;
            const bool lastOfChord = i >= stepsCount - 2;
            buildAndAppendEvents(currentChord, preferredType, stepDurationTicks, ctx.nominalPositionStartTick + i * stepDurationTicks,
                                 ctx, tremoloTimeCache, result, firstOfChord, lastOfChord);
        }

        return;
    }

    for (int i = 0; i < stepsCount; ++i) {
        buildAndAppendEvents(chord, preferredType, stepDurationTicks, ctx.nominalPositionStartTick + i * stepDurationTicks,
                             ctx, tremoloTimeCache, result, i == 0, i == stepsCount - 1);
    }
}

int TremoloRenderer::stepDurationTicks(const Chord* chord, int tremoloLines)
{
    int ticks = Constants::DIVISION / (1 << (chord->beams() + tremoloLines));
    if (ticks <= 0) {
        return 1;
    }
    return ticks * chord->timeStretchFactor();
}

void TremoloRenderer::buildAndAppendEvents(const Chord* chord, const ArticulationType type,
                                           const int stepDurationTicks,
                                           const int startTick, const RenderingContext& ctx,
                                           TremoloTimeCache& tremoloCache,
                                           mpe::PlaybackEventList& result,
                                           bool firstStep, bool lastStep)
{
    for (const Note* note : chord->notes()) {
        if (!NoteRenderer::shouldRender(note, ctx, ctx.commonArticulations)) {
            continue;
        }

        auto noteTnD = timestampAndDurationFromStartAndDurationTicks(
            ctx.score, startTick, stepDurationTicks, ctx.positionTickOffset);

        NominalNoteCtx noteCtx(note, ctx);
        noteCtx.duration = noteTnD.duration;
        noteCtx.timestamp = noteTnD.timestamp;

        int utick = timestampToTick(ctx.score, noteCtx.timestamp);
        noteCtx.dynamicLevel = ctx.playbackCtx->appliableDynamicLevel(note->track(), utick);

        NoteArticulationsParser::buildNoteArticulationMap(note, ctx, noteCtx.articulations);

        // An accent (staccato, bowing, ...) on a tremolo note belongs to its attack only, and a
        // fall / let-ring to its end: don't repeat them on every repeated note.
        if (!firstStep) {
            for (ArticulationType t : tremoloOnsetOnlyTypes()) {
                noteCtx.articulations.erase(t);
            }
        }
        if (!lastStep) {
            for (ArticulationType t : tremoloReleaseOnlyTypes()) {
                noteCtx.articulations.erase(t);
            }
        }

        const TimestampAndDuration& tremoloTnD = tremoloTimeAndDuration(note, ctx, tremoloCache);
        muse::mpe::ArticulationAppliedData& articulationData = noteCtx.articulations.at(type);
        articulationData.meta.timestamp = tremoloTnD.timestamp;
        articulationData.meta.overallDuration = tremoloTnD.duration;

        updateArticulationBoundaries(type, noteCtx.timestamp, noteCtx.duration, noteCtx.articulations);

        result.emplace_back(buildNoteEvent(noteCtx));
    }
}

const TimestampAndDuration& TremoloRenderer::tremoloTimeAndDuration(const Note* note, const RenderingContext& ctx,
                                                                    TremoloTimeCache& cache)
{
    auto cacheIt = cache.find(note);
    if (cacheIt != cache.end()) {
        return cacheIt->second;
    }

    TimestampAndDuration& tnd = cache[note];
    tnd.timestamp = tremoloTimestampFrom(note, ctx);
    tnd.duration = tremoloDuration(note, tnd.timestamp, ctx);

    return tnd;
}
