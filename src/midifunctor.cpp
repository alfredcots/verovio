/////////////////////////////////////////////////////////////////////////////
// Name:        midifunctor.cpp
// Author:      David Bauer
// Created:     2023
// Copyright (c) Authors and others. All rights reserved.
/////////////////////////////////////////////////////////////////////////////

#include "midifunctor.h"

//----------------------------------------------------------------------------

#include <algorithm>
#include <cmath>

//----------------------------------------------------------------------------

#include "arpeg.h"
#include "artic.h"
#include "beatrpt.h"
#include "btrem.h"
#include "chord.h"
#include "doc.h"
#include "featureextractor.h"
#include "ftrem.h"
#include "gracegrp.h"
#include "instrdef.h"
#include "keysig.h"
#include "layer.h"
#include "mordent.h"
#include "mrest.h"
#include "multirest.h"
#include "octave.h"
#include "pedal.h"
#include "refrain.h"
#include "rest.h"
#include "staff.h"
#include "staffdef.h"
#include "syl.h"
#include "tabgrp.h"
#include "tempo.h"
#include "text.h"
#include "tie.h"
#include "transposition.h"
#include "trill.h"
#include "turn.h"
#include "timemap.h"
#include "tuplet.h"
#include "verse.h"
#include "volta.h"
#include "vrv.h"

//----------------------------------------------------------------------------

#include "MidiFile.h"
#include "Tunings.h"

namespace vrv {

//----------------------------------------------------------------------------
// InitOnsetOffsetFunctor
//----------------------------------------------------------------------------

InitOnsetOffsetFunctor::InitOnsetOffsetFunctor(Doc *doc) : DocFunctor(doc)
{
    static const std::map<int, data_DURATION> durationEq{
        { DURATION_EQ_brevis, DURATION_brevis }, //
        { DURATION_EQ_semibrevis, DURATION_semibrevis }, //
        { DURATION_EQ_minima, DURATION_minima }, //
    };

    m_currentScoreTime = 0;
    m_currentRealTimeSeconds = 0.0;
    m_notationType = NOTATIONTYPE_cmn;
    m_currentTempo = MIDI_TEMPO;
    m_meterParams.equivalence = durationEq.at(m_doc->GetOptions()->m_durationEquivalence.GetValue());
}

FunctorCode InitOnsetOffsetFunctor::VisitChordEnd(Chord *chord)
{
    LayerElement *element = chord->ThisOrSameasLink();

    Fraction incrementScoreTime = element->GetAlignmentDuration(m_meterParams, true, m_notationType) * SCORE_TIME_UNIT;
    double realTimeIncrementSeconds = incrementScoreTime.ToDouble() * 60.0 / m_currentTempo;

    m_currentScoreTime = m_currentScoreTime + incrementScoreTime;
    m_currentRealTimeSeconds += realTimeIncrementSeconds;

    return FUNCTOR_CONTINUE;
}

FunctorCode InitOnsetOffsetFunctor::VisitLayer(Layer *layer)
{
    m_currentScoreTime = 0;
    m_currentRealTimeSeconds = 0.0;

    m_meterParams.mensur = layer->GetCurrentMensur();
    m_meterParams.meterSig = layer->GetCurrentMeterSig();
    m_meterParams.proport = layer->GetCurrentProport();
    std::tie(m_meterParams.num, m_meterParams.numbase) = layer->GetCurrentNumAndNumbase();

    return FUNCTOR_CONTINUE;
}

FunctorCode InitOnsetOffsetFunctor::VisitLayerElement(LayerElement *layerElement)
{
    if (layerElement->IsScoreDefElement()) return FUNCTOR_SIBLINGS;

    LayerElement *element = layerElement->ThisOrSameasLink();

    Fraction incrementScoreTime;

    if (element->Is(REST) || element->Is(SPACE)) {
        incrementScoreTime = element->GetAlignmentDuration(m_meterParams, true, m_notationType) * SCORE_TIME_UNIT;
        // For rests to be possibly added to the timemap
        if (element->Is(REST)) {
            Rest *rest = vrv_cast<Rest *>(element);
            double realTimeIncrementSeconds = incrementScoreTime.ToDouble() * 60.0 / m_currentTempo;
            rest->SetScoreTimeOnset(m_currentScoreTime);
            rest->SetRealTimeOnsetSeconds(m_currentRealTimeSeconds);
            rest->SetScoreTimeOffset(m_currentScoreTime + incrementScoreTime);
            rest->SetRealTimeOffsetSeconds(m_currentRealTimeSeconds + realTimeIncrementSeconds);
        }
        m_currentScoreTime = m_currentScoreTime + incrementScoreTime;
        m_currentRealTimeSeconds += incrementScoreTime.ToDouble() * 60.0 / m_currentTempo;
    }
    else if (element->Is(NOTE)) {
        Note *note = vrv_cast<Note *>(element);
        assert(note);

        if (note->IsGraceNote()) {
            // Just store the current onset - only used for grace notes at the end of the layer
            note->SetScoreTimeOnset(m_currentScoreTime);
            note->SetRealTimeOnsetSeconds(m_currentRealTimeSeconds);
            return FUNCTOR_CONTINUE;
        }

        Chord *chord = note->IsChordTone();
        TabGrp *tabGrp = note->IsTabGrpNote();

        // If the note has a @dur or a @dur.ges, take it into account
        // This means that overwriting only @dots or @dots.ges will not be taken into account
        if (chord && !note->HasDur() && !note->HasDurGes()) {
            incrementScoreTime = chord->GetAlignmentDuration(m_meterParams, true, m_notationType) * SCORE_TIME_UNIT;
        }
        else if (tabGrp && !note->HasDur() && !note->HasDurGes()) {
            incrementScoreTime = tabGrp->GetAlignmentDuration(m_meterParams, true, m_notationType) * SCORE_TIME_UNIT;
        }
        else {
            incrementScoreTime = note->GetAlignmentDuration(m_meterParams, true, m_notationType) * SCORE_TIME_UNIT;
        }
        double realTimeIncrementSeconds = incrementScoreTime.ToDouble() * 60.0 / m_currentTempo;

        // LogDebug("Note Alignment Duration %f - Dur %d - Diatonic Pitch %d - Track %d", GetAlignmentDuration(),
        // note->GetNoteOrChordDur(element), note->GetDiatonicPitch(), *midiTrack);
        // LogDebug("Oct %d - Pname %d - Accid %d", note->GetOct(), note->GetPname(), note->GetAccid());

        // When we have a @sameas, do store the onset / offset values of the pointed note in the pointing note
        Note *storeNote = (layerElement == element) ? note : dynamic_cast<Note *>(layerElement);
        if (storeNote) {
            storeNote->SetScoreTimeOnset(m_currentScoreTime);
            storeNote->SetRealTimeOnsetSeconds(m_currentRealTimeSeconds);
            storeNote->SetScoreTimeOffset(m_currentScoreTime + incrementScoreTime);
            storeNote->SetRealTimeOffsetSeconds(m_currentRealTimeSeconds + realTimeIncrementSeconds);
        }

        // increase the currentTime accordingly, but only if not in a chord or tabGrp
        if (!note->IsChordTone() && !note->IsTabGrpNote()) {
            m_currentScoreTime = m_currentScoreTime + incrementScoreTime;
            m_currentRealTimeSeconds += realTimeIncrementSeconds;
        }
    }
    else if (element->Is(BEATRPT)) {
        BeatRpt *rpt = vrv_cast<BeatRpt *>(element);
        assert(rpt);

        incrementScoreTime = rpt->GetAlignmentDuration(m_meterParams, true, m_notationType) * SCORE_TIME_UNIT;
        rpt->SetScoreTimeOnset(m_currentScoreTime);
        m_currentScoreTime = m_currentScoreTime + incrementScoreTime;
        m_currentRealTimeSeconds += incrementScoreTime.ToDouble() * 60.0 / m_currentTempo;
    }
    else if (layerElement->IsAnyOf(std::array{ BEAM, LIGATURE, FTREM, TUPLET }) && layerElement->HasSameasLink()) {
        incrementScoreTime
            = layerElement->GetSameAsContentAlignmentDuration(m_meterParams, true, m_notationType) * SCORE_TIME_UNIT;
        m_currentScoreTime = m_currentScoreTime + incrementScoreTime;
        m_currentRealTimeSeconds += incrementScoreTime.ToDouble() * 60.0 / m_currentTempo;
    }
    else if (layerElement->Is(MENSUR)) {
        this->m_meterParams.mensur = vrv_cast<Mensur *>(layerElement);
    }
    else if (layerElement->Is(METERSIG)) {
        this->m_meterParams.meterSig = vrv_cast<MeterSig *>(layerElement);
    }
    else if (layerElement->Is(PROPORT)) {
        if (layerElement->GetType() == "cmme_tempo_change") return FUNCTOR_SIBLINGS;
        // replace the current proport
        const Proport *previous = (m_meterParams.proport) ? (m_meterParams.proport) : NULL;
        m_meterParams.proport = vrv_cast<Proport *>(layerElement);
        assert(m_meterParams.proport);
        if (previous) {
            m_meterParams.proport->Cumulate(previous);
        }
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode InitOnsetOffsetFunctor::VisitMeasure(Measure *measure)
{
    m_currentTempo = measure->GetCurrentTempo();

    return FUNCTOR_CONTINUE;
}

FunctorCode InitOnsetOffsetFunctor::VisitStaff(Staff *staff)
{
    StaffDef *drawingStaffDef = staff->m_drawingStaffDef;
    assert(drawingStaffDef);

    m_notationType = (drawingStaffDef->HasNotationtype()) ? drawingStaffDef->GetNotationtype() : NOTATIONTYPE_cmn;

    return FUNCTOR_CONTINUE;
}

FunctorCode InitOnsetOffsetFunctor::VisitTabGrpEnd(TabGrp *tabGrp)
{
    LayerElement *element = tabGrp->ThisOrSameasLink();

    Fraction incrementScoreTime = element->GetAlignmentDuration(m_meterParams, true, m_notationType) * SCORE_TIME_UNIT;
    double realTimeIncrementSeconds = incrementScoreTime.ToDouble() * 60.0 / m_currentTempo;

    m_currentScoreTime = m_currentScoreTime + incrementScoreTime;
    m_currentRealTimeSeconds += realTimeIncrementSeconds;

    return FUNCTOR_CONTINUE;
}

//----------------------------------------------------------------------------
// InitMaxMeasureDurationFunctor
//----------------------------------------------------------------------------

InitMaxMeasureDurationFunctor::InitMaxMeasureDurationFunctor() : Functor()
{
    m_currentScoreTime = 0;
    m_currentRealTimeSeconds = 0.0;
    m_currentTempo = MIDI_TEMPO;
    m_tempoAdjustment = 1.0;
    m_multiRestFactor = 1;
}

FunctorCode InitMaxMeasureDurationFunctor::VisitLayerElement(LayerElement *layerElement)
{
    if (layerElement->Is(MULTIREST)) {
        MultiRest *multiRest = vrv_cast<MultiRest *>(layerElement);
        assert(multiRest);
        m_multiRestFactor = multiRest->GetNum();
    }

    return FUNCTOR_SIBLINGS;
}

FunctorCode InitMaxMeasureDurationFunctor::VisitMeasure(Measure *measure)
{
    measure->ClearScoreTimeOnset();
    measure->AddScoreTimeOnset(m_currentScoreTime);

    measure->ClearRealTimeOnsetMilliseconds();
    measure->AddRealTimeOnsetMilliseconds(m_currentRealTimeSeconds * 1000.0);

    return FUNCTOR_CONTINUE;
}

FunctorCode InitMaxMeasureDurationFunctor::VisitMeasureEnd(Measure *measure)
{
    const double tempo = this->GetAdjustedTempo();
    measure->SetCurrentTempo(tempo);

    const Fraction scoreTimeIncrement
        = measure->m_measureAligner.GetRightAlignment()->GetTime() * m_multiRestFactor * SCORE_TIME_UNIT;
    m_currentScoreTime = m_currentScoreTime + scoreTimeIncrement;
    m_currentRealTimeSeconds += scoreTimeIncrement.ToDouble() * 60.0 / tempo;

    measure->ClearScoreTimeOffset();
    measure->AddScoreTimeOffset(m_currentScoreTime);

    measure->ClearRealTimeOffsetMilliseconds();
    measure->AddRealTimeOffsetMilliseconds(m_currentRealTimeSeconds * 1000.0);

    m_multiRestFactor = 1;

    return FUNCTOR_CONTINUE;
}

FunctorCode InitMaxMeasureDurationFunctor::VisitScoreDef(ScoreDef *scoreDef)
{
    if (scoreDef->HasMidiBpm()) {
        m_currentTempo = scoreDef->GetMidiBpm();
    }
    else if (scoreDef->HasMm()) {
        m_currentTempo = Tempo::CalcTempo(scoreDef);
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode InitMaxMeasureDurationFunctor::VisitTempo(Tempo *tempo)
{
    if (tempo->HasMidiBpm()) {
        m_currentTempo = tempo->GetMidiBpm();
    }
    else if (tempo->HasMm()) {
        m_currentTempo = Tempo::CalcTempo(tempo);
    }

    return FUNCTOR_CONTINUE;
}

//----------------------------------------------------------------------------
// InitTimemapTiesFunctor
//----------------------------------------------------------------------------

InitTimemapTiesFunctor::InitTimemapTiesFunctor() : Functor() {}

FunctorCode InitTimemapTiesFunctor::VisitTie(Tie *tie)
{
    Note *note1 = dynamic_cast<Note *>(tie->GetStart());
    Note *note2 = dynamic_cast<Note *>(tie->GetEnd());

    if (!note1 || !note2) {
        return FUNCTOR_CONTINUE;
    }

    Fraction sttd2 = note2->GetScoreTimeTiedDuration();
    Fraction std2 = note2->GetScoreTimeDuration();

    if (sttd2 > 0) {
        note1->SetScoreTimeTiedDuration(sttd2 + std2);
    }
    else {
        note1->SetScoreTimeTiedDuration(std2);
    }
    note2->SetScoreTimeTiedDuration(-1);

    return FUNCTOR_SIBLINGS;
}

//----------------------------------------------------------------------------
// InitTimemapAdjustNotesFunctor
//----------------------------------------------------------------------------

InitTimemapAdjustNotesFunctor::InitTimemapAdjustNotesFunctor() : Functor()
{
    m_noCue = false;
    m_lastNote = NULL;
    m_currentTempo = MIDI_TEMPO;
}

FunctorCode InitTimemapAdjustNotesFunctor::VisitArpeg(Arpeg *arpeg)
{
    // Sort the involved notes by playing order
    const bool playTopDown = (arpeg->GetOrder() == arpegLog_ORDER_down);
    std::set<Note *> notes = arpeg->GetNotes();
    if (notes.empty()) return FUNCTOR_CONTINUE;

    std::vector<Note *> sortedNotes;
    std::copy(notes.begin(), notes.end(), std::back_inserter(sortedNotes));
    std::sort(sortedNotes.begin(), sortedNotes.end(), [playTopDown](const Note *note1, const Note *note2) {
        const int pitch1 = note1->GetMIDIPitch();
        const int pitch2 = note2->GetMIDIPitch();
        return playTopDown ? (pitch1 > pitch2) : (pitch1 < pitch2);
    });

    // Defer the notes in playing order
    Fraction shift = 0;
    Fraction startTime = sortedNotes.front()->GetScoreTimeOnset();
    const Fraction increment = UNACC_GRACENOTE_FRACTION * (int)m_currentTempo;
    for (Note *note : sortedNotes) {
        if (shift != 0) this->SetNoteStartStop(note, startTime + shift);
        shift = shift + increment;
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode InitTimemapAdjustNotesFunctor::VisitChord(Chord *chord)
{
    if (chord->IsGraceNote()) {
        std::list<Note *> notes;
        const ListOfObjects &chordNotes = chord->GetList();
        for (Object *obj : chordNotes) {
            Note *note = vrv_cast<Note *>(obj);
            assert(note);
            notes.push_back(note);
        }

        m_accentedGraceNote = (chord->GetGrace() == GRACE_acc);
        double time = chord->HasGraceTime() ? chord->GetGraceTime() : 50.0;
        const GraceGrp *graceGrp = vrv_cast<const GraceGrp *>(chord->GetFirstAncestor(GRACEGRP));
        if (graceGrp) {
            if (graceGrp->GetGrace() == GRACE_acc) m_accentedGraceNote = true;
            time = (graceGrp->HasGraceTime()) ? graceGrp->GetGraceTime() : 50.0;
        }
        m_graces.push_back({ notes, chord->GetActualDur(), time });

        return FUNCTOR_SIBLINGS;
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode InitTimemapAdjustNotesFunctor::VisitGraceGrpEnd(GraceGrp *graceGrp)
{
    // Handling of Nachschlag
    if (!m_graces.empty() && (graceGrp->GetAttach() == graceGrpLog_ATTACH_pre) && !m_accentedGraceNote && m_lastNote) {
        Fraction startTime = m_lastNote->GetScoreTimeOffset();
        const Fraction graceNoteDur = UNACC_GRACENOTE_FRACTION * (int)m_currentTempo;
        const Fraction totalDur = graceNoteDur * (int)m_graces.size();
        startTime = startTime - totalDur;
        startTime = (startTime < 0) ? 0 : startTime;

        for (const auto &grace : m_graces) {
            const Fraction stopTime = startTime + graceNoteDur;
            for (const auto &note : grace.notes) {
                // Set the start (onset) and end (offset) of the grace note
                this->SetNoteStartStop(note, startTime, stopTime);
            }
            startTime = stopTime;
        }

        m_graces.clear();
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode InitTimemapAdjustNotesFunctor::VisitLayerEnd(Layer *layer)
{
    // Process grace notes at the end of the layer - always treat them as "unacc"
    if (!m_graces.empty() && !m_graces.front().notes.empty()) {
        Fraction startTime = m_graces.front().notes.front()->GetScoreTimeOnset();
        Fraction graceNoteDur = UNACC_GRACENOTE_FRACTION * (int)m_currentTempo;
        const Fraction totalDur = graceNoteDur * (int)m_graces.size();
        startTime = startTime - totalDur;
        startTime = (startTime < 0) ? 0 : startTime;

        // If we have a previous note ending after the start, correct its duration
        if (m_lastNote && (m_lastNote->GetScoreTimeOffset() > startTime)
            && m_lastNote->GetScoreTimeOnset() < startTime) {
            this->SetNoteOrChordStartStop(m_lastNote, m_lastNote->GetScoreTimeOnset(), startTime);
        }

        for (const auto &grace : m_graces) {
            const Fraction stopTime = startTime + graceNoteDur;
            for (const auto &note : grace.notes) {
                // Set the start (onset) and end (offset) of the grace note
                this->SetNoteStartStop(note, startTime, stopTime);
            }
            startTime = stopTime;
        }
        m_graces.clear();
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode InitTimemapAdjustNotesFunctor::VisitMeasure(Measure *measure)
{
    // Update the current tempo
    m_currentTempo = measure->GetCurrentTempo();

    return FUNCTOR_CONTINUE;
}

FunctorCode InitTimemapAdjustNotesFunctor::VisitNote(Note *note)
{
    // Skip linked notes
    if (note->HasSameasLink()) {
        return FUNCTOR_SIBLINGS;
    }

    // Skip cue notes when midiNoCue is activated
    if ((note->GetCue() == BOOLEAN_true) && m_noCue) {
        return FUNCTOR_SIBLINGS;
    }

    // If the note is a secondary tied note, then ignore it
    if (note->GetScoreTimeTiedDuration() < 0) {
        return FUNCTOR_SIBLINGS;
    }

    // Handle grace notes
    if (note->IsGraceNote()) {
        m_accentedGraceNote = (note->GetGrace() == GRACE_acc);
        double time = note->HasGraceTime() ? note->GetGraceTime() : 50.0;
        const GraceGrp *graceGrp = vrv_cast<const GraceGrp *>(note->GetFirstAncestor(GRACEGRP));
        if (graceGrp) {
            if (graceGrp->GetGrace() == GRACE_acc) m_accentedGraceNote = true;
            time = (graceGrp->HasGraceTime()) ? graceGrp->GetGraceTime() : 50.0;
        }
        m_graces.push_back({ { note }, note->GetDur(), time });

        return FUNCTOR_SIBLINGS;
    }

    // Check if some grace notes must be performed
    if (!m_graces.empty()) {
        this->SetGraceNotesFor(note);
        m_graces.clear();
    }

    // Store reference, i.e. for Nachschlag
    m_lastNote = note;

    return FUNCTOR_SIBLINGS;
}

void InitTimemapAdjustNotesFunctor::SetGraceNotesFor(Note *refNote)
{
    Fraction startTime = refNote->GetScoreTimeOnset();

    Fraction graceNoteDur = 0;
    if (m_accentedGraceNote && !m_graces.empty()) {
        // Arbitrarily looks at the first note, not sure what to do if we have contradictory values
        double percent = m_graces.front().time;
        // Arbitrarily constraint the time between 5% and 95%
        percent = std::min(95.0, std::max(5.0, percent));
        const Fraction totalDur = refNote->GetScoreTimeDuration() * (int)percent / 100;
        // Adjust the start of the main note
        this->SetNoteOrChordStartStop(refNote, startTime + totalDur);
        graceNoteDur = totalDur / (int)m_graces.size();
    }
    else {
        graceNoteDur = UNACC_GRACENOTE_FRACTION * (int)m_currentTempo;
        const Fraction totalDur = graceNoteDur * (int)m_graces.size();
        if (startTime >= totalDur) {
            startTime = startTime - totalDur;
        }
        else {
            // Adjust the start of the main note or chord
            this->SetNoteOrChordStartStop(refNote, startTime + totalDur);
        }
    }

    for (const auto &grace : m_graces) {
        const Fraction stopTime = startTime + graceNoteDur;
        for (const auto &note : grace.notes) {
            // Set the start (onset) and end (offset) of the grace note
            this->SetNoteStartStop(note, startTime, stopTime);
        }
        startTime = stopTime;
    }
}

void InitTimemapAdjustNotesFunctor::SetNoteOrChordStartStop(
    Note *note, const Fraction &startTime, const Fraction &stopTime)
{
    if (note->IsChordTone()) {
        Chord *chord = note->IsChordTone();
        ListOfObjects notes = chord->FindAllDescendantsByType(NOTE);
        for (Object *child : notes) {
            Note *chordNote = vrv_cast<Note *>(child);
            this->SetNoteStartStop(chordNote, startTime, stopTime);
        }
    }
    else {
        this->SetNoteStartStop(note, startTime, stopTime);
    }
}

void InitTimemapAdjustNotesFunctor::SetNoteStartStop(Note *note, const Fraction &startTime, const Fraction &stopTime)
{
    assert(note);

    note->SetScoreTimeOnset(startTime);
    double startRealTimeSeconds = startTime.ToDouble() * 60.0 / m_currentTempo;
    note->SetRealTimeOnsetSeconds(startRealTimeSeconds);
    if (stopTime != VRV_UNSET) {
        note->SetScoreTimeOffset(stopTime);
        double stopRealTimeSeconds = stopTime.ToDouble() * 60.0 / m_currentTempo;
        note->SetRealTimeOffsetSeconds(stopRealTimeSeconds);
    }
}

//----------------------------------------------------------------------------
// InitMIDIFunctor
//----------------------------------------------------------------------------

InitMIDIFunctor::InitMIDIFunctor() : ConstFunctor()
{
    m_currentTempo = MIDI_TEMPO;
}

FunctorCode InitMIDIFunctor::VisitMeasure(const Measure *measure)
{
    m_currentTempo = measure->GetCurrentTempo();

    return FUNCTOR_CONTINUE;
}

FunctorCode InitMIDIFunctor::VisitMordent(const Mordent *mordent)
{
    this->AddOrnament(mordent, mordent->GetStart());

    return FUNCTOR_CONTINUE;
}

FunctorCode InitMIDIFunctor::VisitTrill(const Trill *trill)
{
    this->AddOrnament(trill, trill->GetStart());

    return FUNCTOR_CONTINUE;
}

FunctorCode InitMIDIFunctor::VisitTurn(const Turn *turn)
{
    this->AddOrnament(turn, turn->GetStart());

    return FUNCTOR_CONTINUE;
}

void InitMIDIFunctor::AddOrnament(const Object *ornament, const LayerElement *start)
{
    // An ornament without a start (e.g., an unresolved @startid or only a @tstamp) is not played
    if (!start) return;

    const Note *note = NULL;
    if (start->Is(NOTE)) {
        note = vrv_cast<const Note *>(start);
    }
    else if (start->Is(CHORD)) {
        // In a chord, only the top note plays it
        note = vrv_cast<const Chord *>(start)->GetTopNote();
    }
    if (!note) return;

    // With more than one, the first one is played
    m_ornaments.emplace(note, OrnamentInfo{ ornament->GetClassId(), ornament });
}

FunctorCode InitMIDIFunctor::VisitOctave(const Octave *octave)
{
    // An octave without start or end (e.g., an unresolved @endid) is not rendered and is ignored
    if (!octave->HasStartAndEnd()) return FUNCTOR_CONTINUE;

    const Measure *measure = vrv_cast<const Measure *>(octave->GetFirstAncestor(MEASURE));
    assert(measure);
    std::vector<const Staff *> staffList = octave->GetTstampStaves(measure, octave);
    if (staffList.size() != 1) return FUNCTOR_CONTINUE;
    const Staff *staff = staffList[0];

    const bool raisePitch = (octave->GetDisPlace() != STAFFREL_basic_below);
    int shift = 0;
    switch (octave->GetDis()) {
        case OCTAVE_DIS_8: shift = 1; break;
        case OCTAVE_DIS_15: shift = 2; break;
        case OCTAVE_DIS_22: shift = 3; break;
        default: break;
    }

    const Layer *layer = vrv_cast<const Layer *>(raisePitch ? staff->GetFirst(LAYER) : staff->GetLast(LAYER));
    assert(layer);

    m_octaves.push_back({ octave, staff->GetN(), layer->GetN(), (raisePitch ? shift : -shift), false });

    return FUNCTOR_CONTINUE;
}

//----------------------------------------------------------------------------
// GenerateMIDIFunctor
//----------------------------------------------------------------------------

GenerateMIDIFunctor::GenerateMIDIFunctor(smf::MidiFile *midiFile) : ConstFunctor()
{
    m_midiFile = midiFile;
    m_midiTrack = 1;
    m_midiChannel = 0;
    m_totalTime = 0.0;
    m_staffN = 0;
    m_transSemi = 0;
    m_octaveShift = 0;
    m_currentTempo = MIDI_TEMPO;
    m_lastNote = NULL;
    m_noCue = false;
    m_controlEvents = false;
    m_instrDef = NULL;
    m_customTuning = NULL;
}

FunctorCode GenerateMIDIFunctor::VisitBeatRpt(const BeatRpt *beatRpt)
{
    // Sameas not taken into account for now
    AlignMeterParams params;
    double beatLength = beatRpt->GetAlignmentDuration(params).ToDouble() * SCORE_TIME_UNIT;
    double startTime = m_totalTime + beatRpt->GetScoreTimeOnset().ToDouble();
    int tpq = m_midiFile->getTPQ();

    // filter last beat and copy all notes
    smf::MidiEvent event;
    int eventCount = m_midiFile->getEventCount(m_midiTrack);
    for (int i = 0; i < eventCount; ++i) {
        event = m_midiFile->getEvent(m_midiTrack, i);
        if (event.tick > startTime * tpq)
            break;
        else if (event.tick >= (startTime - beatLength) * tpq) {
            if (((event[0] & 0xf0) == 0x80) || ((event[0] & 0xf0) == 0x90)) {
                m_midiFile->addEvent(m_midiTrack, event.tick + beatLength * tpq, event);
            }
        }
    }

    for (int i = 0; i < beatLength * tpq; ++i) {
        // LogWarning("%i", i);
        // smf::MidiEvent event = m_midiFile->getEvent(m_midiTrack, startTime * tpq);
        // event.clearVariables();
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitBTrem(const BTrem *bTrem)
{
    // Do nothing if the tremolo is unmeasured
    if (bTrem->GetForm() == tremForm_FORM_unmeas) {
        return FUNCTOR_CONTINUE;
    }

    // Adjust duration of the bTrem if it's nested within tuplet
    int num = 0;
    const Tuplet *tuplet = vrv_cast<const Tuplet *>(bTrem->GetFirstAncestor(TUPLET, MAX_TUPLET_DEPTH));
    if (tuplet) {
        num = (tuplet->GetNum() > 0) ? tuplet->GetNum() : 0;
    }
    // Get num value if it's set
    if (bTrem->HasNum()) {
        num = std::clamp(bTrem->GetNum(), 0, 256);
    }

    // Calculate duration of individual note in tremolo
    const data_DURATION individualNoteDur = bTrem->CalcIndividualNoteDuration();
    if ((individualNoteDur == DURATION_NONE) || (individualNoteDur > DURATION_2048)) return FUNCTOR_CONTINUE;
    const double noteInQuarterDur = pow(2.0, (DURATION_4 - individualNoteDur));

    // Define lambda which expands one note into multiple individual notes of the same pitch
    auto expandNote = [this, noteInQuarterDur, num](const Object *obj) {
        const Note *note = vrv_cast<const Note *>(obj);
        assert(note);
        // Secondary tied notes are not played, their duration is already added to the first note
        if (note->GetScoreTimeTiedDuration() < 0) return;
        const int pitch = this->GetMIDIPitch(note);
        const double totalInQuarterDur
            = note->GetScoreTimeDuration().ToDouble() + note->GetScoreTimeTiedDuration().ToDouble();
        int multiplicity = totalInQuarterDur / noteInQuarterDur;
        double noteDuration = noteInQuarterDur;
        // if NUM has been set for the bTrem, override calculated values
        if (num) {
            multiplicity = num;
            noteDuration = totalInQuarterDur / double(num);
        }
        if (multiplicity < 1 || totalInQuarterDur <= 0.0) return;
        m_expandedNotes[note] = MIDINoteSequence(multiplicity, { pitch, noteDuration });
    };

    // Apply expansion either to all notes in chord or to first note
    const Chord *chord = vrv_cast<const Chord *>(bTrem->FindDescendantByType(CHORD));
    if (chord) {
        ListOfConstObjects notes = chord->FindAllDescendantsByType(NOTE, false);
        std::for_each(notes.begin(), notes.end(), expandNote);
    }
    else {
        const Object *note = bTrem->FindDescendantByType(NOTE);
        if (note) {
            expandNote(note);
        }
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitChord(const Chord *chord)
{
    this->HandleOctave(chord);

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitFTrem(const FTrem *fTrem)
{
    this->HandleOctave(fTrem);

    if (fTrem->HasUnitdur()) {
        LogWarning("FTrem produces incorrect MIDI output");
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitHalfmRpt(const HalfmRpt *halfmRpt)
{
    LogWarning("HalfmRpt produces empty MIDI output");

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitLayer(const Layer *layer)
{
    if ((layer->GetCue() == BOOLEAN_true) && m_noCue) return FUNCTOR_SIBLINGS;

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitLayerEnd(const Layer *layer)
{
    // stop all previously held notes
    for (auto &held : m_heldNotes) {
        if (held.m_pitch > 0) {
            m_midiFile->addNoteOff(
                m_midiTrack, std::max(0.0, held.m_stopTime * m_midiFile->getTPQ() - 1), m_midiChannel, held.m_pitch);
        }
    }

    m_heldNotes.clear();

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitLayerElement(const LayerElement *layerElement)
{
    if (layerElement->IsScoreDefElement()) return FUNCTOR_SIBLINGS;

    this->HandleOctave(layerElement);

    // Only resolve simple sameas links to avoid infinite recursion
    const LayerElement *sameas = dynamic_cast<const LayerElement *>(layerElement->GetSameasLink());
    if (sameas && !sameas->HasSameasLink()) {
        sameas->Process(*this);
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitMeasure(const Measure *measure)
{
    // Here we need to update the m_totalTime from the starting time of the measure.
    m_totalTime = measure->GetScoreTimeOnset().ToDouble();

    if ((m_totalTime == 0.0) || (measure->GetCurrentTempo() != m_currentTempo)) {
        m_currentTempo = measure->GetCurrentTempo();
        const int tick = m_totalTime * m_midiFile->getTPQ();
        // Check if there was already a tempo event added for the given tick
        if (m_tempoEventTicks.insert(tick).second) {
            m_midiFile->addTempo(0, tick, m_currentTempo);
        }
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitMRpt(const MRpt *mRpt)
{
    LogWarning("MRpt produces empty MIDI output");

    return FUNCTOR_CONTINUE;
}

namespace {

/**
 * What the articulations of a note change in what it plays: the part of its duration that sounds and
 * its velocity. The start of the note doesn't change, so neither do the timemap nor the score times.
 * In MusicXML every note of a chord has its own; in MEI they can also be on the chord.
 */
void GetArticulationFactors(const Note *note, double &durationFactor, double &velocityFactor)
{
    durationFactor = 1.0;
    velocityFactor = 1.0;

    std::set<data_ARTICULATION> values;
    auto collect = [&values](const Object *object) {
        if (!object) return;
        const ListOfConstObjects artics = object->FindAllDescendantsByType(ARTIC, false, 1);
        for (const Object *child : artics) {
            const Artic *artic = vrv_cast<const Artic *>(child);
            for (const data_ARTICULATION value : artic->GetArtic()) values.insert(value);
        }
    };
    collect(note);
    collect(note->IsChordTone());
    if (values.empty()) return;

    auto has = [&values](data_ARTICULATION value) { return values.count(value) > 0; };

    // The shortest of them, and the tenuto holds the note (with a staccato, it's the portato)
    if (has(ARTICULATION_stacciss) || has(ARTICULATION_spicc)) durationFactor = 0.3;
    else if (has(ARTICULATION_stacc)) durationFactor = has(ARTICULATION_ten) ? 0.75 : 0.5;
    else if (has(ARTICULATION_marc)) durationFactor = 0.75;

    // The strongest of them
    if (has(ARTICULATION_marc)) velocityFactor = 1.25;
    else if (has(ARTICULATION_acc)) velocityFactor = 1.15;
    else if (has(ARTICULATION_acc_soft)) velocityFactor = 1.08;
}

/// The shortest a shortened note sounds, in seconds: shorter, it would be a click
const double MIDI_ARTICULATION_MIN_SECONDS = 0.06;

/// Each note of a trill, in seconds (about 12 notes per second)
const double MIDI_TRILL_NOTE_SECONDS = 0.08;

/// Each short note of a mordent or a turn, in seconds
const double MIDI_ORNAMENT_NOTE_SECONDS = 0.065;

/// The shortest a note of an ornament can be, in seconds: with a shorter note, the ornament isn't played
const double MIDI_ORNAMENT_MIN_SECONDS = 0.03;

} // namespace

FunctorCode GenerateMIDIFunctor::VisitNote(const Note *note)
{
    this->HandleOctave(note);

    // Skip linked notes
    if (note->HasSameasLink()) {
        return FUNCTOR_SIBLINGS;
    }

    // Skip cue notes when midiNoCue is activated
    if ((note->GetCue() == BOOLEAN_true) && m_noCue) {
        return FUNCTOR_SIBLINGS;
    }

    // If the note is a secondary tied note, then ignore it
    if (note->GetScoreTimeTiedDuration() < 0) {
        return FUNCTOR_SIBLINGS;
    }

    int velocity = (note->HasVel()) ? note->GetVel() : MIDI_VELOCITY;
    if (!velocity) return FUNCTOR_SIBLINGS;

    double durationFactor = 1.0;
    double velocityFactor = 1.0;
    GetArticulationFactors(note, durationFactor, velocityFactor);
    if (velocityFactor != 1.0) {
        velocity = std::clamp(static_cast<int>(std::lround(velocity * velocityFactor)), 1, 127);
    }

    const int channel = m_midiChannel;
    const int tpq = m_midiFile->getTPQ();
    double startTime = m_totalTime + note->GetScoreTimeOnset().ToDouble();

    // Check if note is deferred
    if (m_deferredNotes.find(note) != m_deferredNotes.end()) {
        startTime += m_deferredNotes.at(note);
        m_deferredNotes.erase(note);
    }

    // The ornament of the note (a tremolo is played instead)
    MIDINoteSequence ornamentNotes;
    const bool hasTabCourse = note->HasTabCourse() && (note->GetTabCourse() >= 1);
    if ((m_expandedNotes.find(note) == m_expandedNotes.end()) && !hasTabCourse) {
        const auto ornamentIter = m_ornaments.find(note);
        if (ornamentIter != m_ornaments.end()) {
            const double stopTime
                = m_totalTime + note->GetScoreTimeOffset().ToDouble() + note->GetScoreTimeTiedDuration().ToDouble();
            ornamentNotes
                = this->ExpandOrnament(note, ornamentIter->second, this->GetMIDIPitch(note), stopTime - startTime);
        }
    }

    // Check if note was expanded into sequence of short notes due to trills/tremolandi
    // Play either the expanded note sequence or a single note
    if (!ornamentNotes.empty() || (m_expandedNotes.find(note) != m_expandedNotes.end())) {
        const MIDINoteSequence &sequence = ornamentNotes.empty() ? m_expandedNotes.at(note) : ornamentNotes;
        for (const auto &midiNote : sequence) {
            const double stopTime = startTime + midiNote.duration;

            m_midiFile->addNoteOn(m_midiTrack, startTime * tpq, channel, midiNote.pitch, velocity);
            m_midiFile->addNoteOff(m_midiTrack, std::max(0.0, stopTime * tpq - 1), channel, midiNote.pitch);

            startTime = stopTime;
        }
    }
    else {
        const int pitch = this->GetMIDIPitch(note);

        if (note->HasTabCourse() && (note->GetTabCourse() >= 1)) {
            // Tablature 'rule of holds'.  A note on a course is held until the next note
            // on that course is required, or until a default hold duration is reached.

            const int course = note->GetTabCourse();
            if (m_heldNotes.size() < static_cast<size_t>(course)) {
                m_heldNotes.resize(course); // make room
            }

            // if a previously held note on this course is already sounding, end it now.
            if (m_heldNotes[course - 1].m_pitch > 0) {
                m_heldNotes[course - 1].m_stopTime = startTime; // stop now
            }

            // end all previously held notes that have reached their stoptime
            // or if the new pitch is already sounding, on any course
            for (auto &held : m_heldNotes) {
                if ((held.m_pitch > 0) && ((held.m_stopTime <= startTime) || (held.m_pitch == pitch))) {
                    m_midiFile->addNoteOff(
                        m_midiTrack, std::max(0.0, held.m_stopTime * tpq - 1), channel, held.m_pitch);
                    held.m_pitch = 0;
                    held.m_stopTime = 0;
                }
            }

            // hold this note until the greater of its rhythm sign and the default duration.
            // TODO optimize the default hold duration
            const double defaultHoldTime = 4; // quarter notes
            m_heldNotes[course - 1].m_pitch = pitch;
            m_heldNotes[course - 1].m_stopTime = m_totalTime
                + std::max(defaultHoldTime,
                    note->GetScoreTimeOffset().ToDouble() + note->GetScoreTimeTiedDuration().ToDouble());

            // start this note
            m_midiFile->addNoteOn(m_midiTrack, startTime * tpq, channel, pitch, velocity);
        }
        else {
            double stopTime
                = m_totalTime + note->GetScoreTimeOffset().ToDouble() + note->GetScoreTimeTiedDuration().ToDouble();

            // Shortened by its articulation, never below the shortest it can sound
            if ((durationFactor < 1.0) && (stopTime > startTime)) {
                const double minDuration = MIDI_ARTICULATION_MIN_SECONDS * m_currentTempo / 60.0;
                const double duration = stopTime - startTime;
                stopTime = startTime + std::min(duration, std::max(duration * durationFactor, minDuration));
            }

            m_midiFile->addNoteOn(m_midiTrack, startTime * tpq, channel, pitch, velocity);
            m_midiFile->addNoteOff(m_midiTrack, std::max(0.0, stopTime * tpq - 1), channel, pitch);
        }
    }

    // Store reference, i.e. for Nachschlag
    m_lastNote = note;

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitPedal(const Pedal *pedal)
{
    if (!pedal->HasDir()) return FUNCTOR_CONTINUE;
    // The start is needed to calculate the pedal time below; skip pedals without one (e.g., an unresolved @startid)
    if (!pedal->GetStart()) return FUNCTOR_CONTINUE;

    // Check the functor flag - filters should always be there, but just in case we change how it is called
    if (!m_controlEvents || !this->GetFilters()) return FUNCTOR_CONTINUE;

    // Check if the pedal applies to the staff filtered
    const Measure *measure = vrv_cast<const Measure *>(pedal->GetFirstAncestor(MEASURE));
    assert(measure);
    std::vector<const Staff *> staffList = pedal->GetTstampStaves(measure, pedal);
    bool applies = false;
    for (const Staff *staff : staffList) {
        applies = (applies || this->GetFilters()->Apply(staff));
    }
    if (!applies) return FUNCTOR_CONTINUE;

    double pedalTime = pedal->GetStart()->GetAlignment()->GetTime().ToDouble() * SCORE_TIME_UNIT;
    double startTime = m_totalTime + pedalTime;
    int tpq = m_midiFile->getTPQ();

    // todo: check pedal @func to switch between sustain/soften/damper pedals?
    switch (pedal->GetDir()) {
        case pedalLog_DIR_down: m_midiFile->addSustainPedalOn(m_midiTrack, (startTime * tpq), m_midiChannel); break;
        case pedalLog_DIR_up: m_midiFile->addSustainPedalOff(m_midiTrack, (startTime * tpq), m_midiChannel); break;
        case pedalLog_DIR_bounce:
            m_midiFile->addSustainPedalOff(m_midiTrack, (startTime * tpq), m_midiChannel);
            m_midiFile->addSustainPedalOn(m_midiTrack, (startTime * tpq) + 0.1, m_midiChannel);
            break;
        default: return FUNCTOR_CONTINUE;
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitScoreDef(const ScoreDef *scoreDef)
{
    double totalTime = m_totalTime;
    // check next measure for the time offset
    const Object *parent = scoreDef->GetParent();
    if (parent && (parent->GetLast() != scoreDef)) {
        const Object *next = parent->GetNext(scoreDef);
        if (next && next->Is(MEASURE)) {
            const Measure *nextMeasure = vrv_cast<const Measure *>(next);
            totalTime = nextMeasure->GetScoreTimeOnset().ToDouble();
        }
    }
    const double currentTick = totalTime * m_midiFile->getTPQ();

    smf::MidiEvent midiEvent;
    midiEvent.tick = currentTick;
    // calculate reference pitch class based on @tune.pname
    int referencePitchClass = 0;
    if (scoreDef->HasTunePname()) {
        referencePitchClass = Note::PnameToPclass(scoreDef->GetTunePname());
    }
    // set and remember custom tuning if available
    if (scoreDef->HasCustomTuning() && scoreDef->GetCustomTuning()->IsValid()) {
        m_customTuning = scoreDef->GetCustomTuning();
        const int program = m_instrDef && m_instrDef->HasMidiInstrnum() ? m_instrDef->GetMidiInstrnum() : 0;
        const Tunings::Tuning &tuneCustom = m_customTuning->GetTuning();
        std::vector<std::pair<int, double>> mapping;
        // sysex messages should not have the high bit (0x80) set, which means that the full 128 MIDI notes
        // cannot be retuned in the same sysex message. For now, we skip MIDI key 0.
        for (int i = 1; i < 128; i++) {
            mapping.push_back(std::make_pair(i, tuneCustom.frequencyForMidiNote(i)));
        }
        midiEvent.makeMts2_KeyTuningsByFrequency(mapping, program);
        m_midiFile->addEvent(m_midiTrack, midiEvent);
    }
    // set temperament event if corresponding attribute present
    else if (scoreDef->HasTuneTemper()) {
        switch (scoreDef->GetTuneTemper()) {
            case TEMPERAMENT_equal: midiEvent.makeTemperamentEqual(referencePitchClass); break;
            case TEMPERAMENT_just: midiEvent.makeTemperamentBad(100.0, referencePitchClass); break;
            case TEMPERAMENT_mean: midiEvent.makeTemperamentMeantone(referencePitchClass); break;
            case TEMPERAMENT_pythagorean: midiEvent.makeTemperamentPythagorean(referencePitchClass); break;
            default: break;
        }
        m_midiFile->addEvent(m_midiTrack, midiEvent);
    }
    // set tuning
    if (scoreDef->HasTuneHz()) {
        const double tuneHz = scoreDef->GetTuneHz();
        // Add tuning for all keys from 0 to 127
        std::vector<std::pair<int, double>> tuneFrequencies;
        for (int i = 0; i < 127; ++i) {
            double freq = pow(2.0, (i - 69.0) / 12.0) * tuneHz;
            tuneFrequencies.push_back(std::make_pair(i, freq));
        }
        midiEvent.makeMts2_KeyTuningsByFrequency(tuneFrequencies);
        m_midiFile->addEvent(m_midiTrack, midiEvent);
    }
    // set MIDI key signature
    if (scoreDef->HasKeySigInfo()) {
        const KeySig *keySig = vrv_cast<const KeySig *>(scoreDef->GetKeySig());
        if (keySig && keySig->HasSig()) {
            // m_midiFile->addKeySignature(
            //     m_midiTrack, currentTick, keySig->GetFifthsInt(), (keySig->GetMode() == MODE_minor));
        }
    }
    // set MIDI time signature
    if (scoreDef->HasMeterSigInfo()) {
        const MeterSig *meterSig = vrv_cast<const MeterSig *>(scoreDef->GetMeterSig());
        if (meterSig && meterSig->HasCount() && meterSig->HasUnit()) {
            m_midiFile->addTimeSignature(m_midiTrack, currentTick, meterSig->GetTotalCount(), meterSig->GetUnit());
        }
        else if (meterSig && meterSig->HasUnit()) {
            m_midiFile->addTimeSignature(
                m_midiTrack, currentTick, meterSig->GetTotalCount(), meterSig->GetSymImplicitUnit());
        }
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitStaff(const Staff *staff)
{
    if (staff->IsOssia()) return FUNCTOR_SIBLINGS;

    m_expandedNotes.clear();

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitStaffDef(const StaffDef *staffDef)
{
    if (staffDef->GetN() == m_staffN) {
        // Update the semitone transposition
        if (staffDef->HasTransSemi()) m_transSemi = staffDef->GetTransSemi();
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitSyl(const Syl *syl)
{
    if (syl->IsEmpty()) return FUNCTOR_CONTINUE;

    const Note *note = NULL;
    if (syl->GetFirstAncestor(CHORD)) {
        const Chord *parentChord = vrv_cast<const Chord *>(syl->GetFirstAncestor(CHORD));
        note = vrv_cast<const Note *>(parentChord->GetFirst(NOTE));
    }
    else {
        note = vrv_cast<const Note *>(syl->GetFirstAncestor(NOTE));
    }
    if (!note) return FUNCTOR_CONTINUE;

    const double startTime = m_totalTime + note->GetScoreTimeOnset().ToDouble();
    const std::string sylText = UTF32to8(syl->GetText());

    m_midiFile->addLyric(m_midiTrack, startTime * m_midiFile->getTPQ(), sylText);

    return FUNCTOR_SIBLINGS;
}

FunctorCode GenerateMIDIFunctor::VisitVolta(const Volta *volta)
{
    return (volta->GetDrawingVoltaN() == 1) ? FUNCTOR_CONTINUE : FUNCTOR_SIBLINGS;
}

FunctorCode GenerateMIDIFunctor::VisitVerse(const Verse *verse)
{
    const LayerElement *parent = vrv_cast<const Note *>(verse->GetFirstAncestor(NOTE));
    if (!parent) parent = vrv_cast<const Chord *>(verse->GetFirstAncestor(CHORD));
    assert(parent);

    const Verse *previousVerse = vrv_cast<const Verse *>(parent->GetPrevious(verse, VERSE));

    if (previousVerse) return FUNCTOR_SIBLINGS;

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateMIDIFunctor::VisitRefrain(const Refrain *refrain)
{
    const LayerElement *parent = vrv_cast<const Note *>(refrain->GetFirstAncestor(NOTE));
    if (!parent) parent = vrv_cast<const Chord *>(refrain->GetFirstAncestor(CHORD));
    assert(parent);

    if (parent->FindDescendantByType(VERSE, 1)) return FUNCTOR_SIBLINGS;
    if (parent->GetPrevious(refrain, REFRAIN)) return FUNCTOR_SIBLINGS;

    return FUNCTOR_CONTINUE;
}

void GenerateMIDIFunctor::DeferMIDINote(const Note *refNote, double shift, bool includeChordSiblings)
{
    // Recursive call for chords
    const Chord *chord = refNote->IsChordTone();
    if (chord && includeChordSiblings) {
        const ListOfConstObjects &notes = chord->GetList();

        for (const Object *obj : notes) {
            const Note *note = vrv_cast<const Note *>(obj);
            assert(note);
            this->DeferMIDINote(note, shift, false);
        }
        return;
    }

    // Register the shift
    if (shift < refNote->GetScoreTimeDuration().ToDouble() + refNote->GetScoreTimeTiedDuration().ToDouble()) {
        m_deferredNotes[refNote] = shift;
    }
}

void GenerateMIDIFunctor::HandleOctave(const LayerElement *layerElement)
{
    // Handle octave end
    auto octaveIter = std::find_if(m_octaves.begin(), m_octaves.end(), [this, layerElement](const OctaveInfo &octave) {
        if (octave.isActive && (octave.staffN == m_staffN) && (octave.layerN == m_layerN)) {
            const Alignment *endAlignment = octave.octave->GetEnd()->GetAlignment();
            const Alignment *alignment = layerElement->GetAlignment();
            if (endAlignment && alignment) {
                return *endAlignment < *alignment;
            }
        }
        return false;
    });
    if (octaveIter != m_octaves.end()) {
        m_octaveShift -= octaveIter->octaveShift;
        m_octaves.erase(octaveIter);
    }

    // Handle octave begin
    octaveIter = std::find_if(m_octaves.begin(), m_octaves.end(), [this, layerElement](const OctaveInfo &octave) {
        if (!octave.isActive && (octave.staffN == m_staffN) && (octave.layerN == m_layerN)) {
            const Alignment *startAlignment = octave.octave->GetStart()->GetAlignment();
            const Alignment *alignment = layerElement->GetAlignment();
            if (startAlignment && alignment) {
                return *startAlignment <= *alignment;
            }
        }
        return false;
    });
    if (octaveIter != m_octaves.end()) {
        m_octaveShift += octaveIter->octaveShift;
        octaveIter->isActive = true;
    }
}

MIDINoteSequence GenerateMIDIFunctor::ExpandOrnament(
    const Note *note, const OrnamentInfo &info, int pitch, double duration)
{
    MIDINoteSequence sequence;
    if (duration <= 0.0) return sequence;

    // From seconds to quarter notes at the tempo of the measure
    const double quarterNotesPerSecond = m_currentTempo / 60.0;
    const double minNote = MIDI_ORNAMENT_MIN_SECONDS * quarterNotesPerSecond;

    // The short notes first, then the main note for the rest of the duration
    auto shortNotesThenMain = [&sequence, pitch, duration, minNote, quarterNotesPerSecond](
                                  const std::vector<int> &shortPitches) {
        const int count = static_cast<int>(shortPitches.size());
        const double shortNote
            = std::min(MIDI_ORNAMENT_NOTE_SECONDS * quarterNotesPerSecond, duration / (count + 1));
        if (shortNote < minNote) return;
        for (const int shortPitch : shortPitches) sequence.push_back({ shortPitch, shortNote });
        sequence.push_back({ pitch, duration - count * shortNote });
    };

    if (info.type == TRILL) {
        const Trill *trill = vrv_cast<const Trill *>(info.ornament);
        const int upper = this->GetNeighborPitch(note, pitch, true, trill->GetAccidupper());
        // Main, upper, main... with an odd number of notes, so that it ends on the main note
        int count = static_cast<int>(duration / (MIDI_TRILL_NOTE_SECONDS * quarterNotesPerSecond));
        if (count % 2 == 0) --count;
        count = std::max(count, 3);
        const double trillNote = duration / count;
        if (trillNote < minNote) return sequence;
        for (int i = 0; i < count; ++i) sequence.push_back({ (i % 2) ? upper : pitch, trillNote });
    }
    else if (info.type == MORDENT) {
        const Mordent *mordent = vrv_cast<const Mordent *>(info.ornament);
        // Without a form, the mordent is the lower one
        const bool above = (mordent->GetForm() == mordentLog_FORM_upper);
        const int neighbor = this->GetNeighborPitch(
            note, pitch, above, above ? mordent->GetAccidupper() : mordent->GetAccidlower());
        if (mordent->GetLong() == BOOLEAN_true) {
            shortNotesThenMain({ pitch, neighbor, pitch, neighbor });
        }
        else {
            shortNotesThenMain({ pitch, neighbor });
        }
    }
    else if (info.type == TURN) {
        const Turn *turn = vrv_cast<const Turn *>(info.ornament);
        const int upper = this->GetNeighborPitch(note, pitch, true, turn->GetAccidupper());
        const int lower = this->GetNeighborPitch(note, pitch, false, turn->GetAccidlower());
        // Without a form, the turn starts above
        const bool inverted = (turn->GetForm() == turnLog_FORM_lower);
        const int first = inverted ? lower : upper;
        const int third = inverted ? upper : lower;
        if (turn->GetDelayed() == BOOLEAN_true) {
            // The main note, and the turn at its end, leading to the next note
            const double shortNote = std::min(MIDI_ORNAMENT_NOTE_SECONDS * quarterNotesPerSecond, duration / 4);
            if (shortNote < minNote) return sequence;
            sequence.push_back({ pitch, duration - 3 * shortNote });
            sequence.push_back({ first, shortNote });
            sequence.push_back({ pitch, shortNote });
            sequence.push_back({ third, shortNote });
        }
        else {
            shortNotesThenMain({ first, pitch, third });
        }
    }

    return sequence;
}

int GenerateMIDIFunctor::GetNeighborPitch(
    const Note *note, int pitch, bool above, data_ACCIDENTAL_WRITTEN accid) const
{
    // Without a written pitch, a whole tone
    if (!note->HasPname() || !note->HasOct()) return pitch + (above ? 2 : -2);

    const int step = note->GetDiatonicPitch() + (above ? 1 : -1);
    const int oct = (step >= 0) ? (step / 7) : ((step - 6) / 7);
    const data_PITCHNAME pname = static_cast<data_PITCHNAME>(step - oct * 7 + PITCHNAME_c);

    int alteration = 0;
    if (accid != ACCIDENTAL_WRITTEN_NONE) {
        alteration = TransPitch::GetChromaticAlteration(ACCIDENTAL_GESTURAL_NONE, accid);
    }
    else {
        bool found = false;
        // The last note of that pitch earlier in the measure, on any layer of the staff
        const Staff *staff = note->GetAncestorStaff(ANCESTOR_ONLY, false);
        if (staff) {
            const double onset = note->GetScoreTimeOnset().ToDouble();
            double lastOnset = -1.0;
            const ListOfConstObjects notes = staff->FindAllDescendantsByType(NOTE);
            for (const Object *object : notes) {
                const Note *previous = vrv_cast<const Note *>(object);
                if ((previous->GetPname() != pname) || (previous->GetOct() != oct)) continue;
                const double previousOnset = previous->GetScoreTimeOnset().ToDouble();
                if ((previousOnset >= onset) || (previousOnset < lastOnset)) continue;
                lastOnset = previousOnset;
                alteration = previous->GetTransPitch().m_accid;
                found = true;
            }
        }
        // Otherwise, the key signature
        if (!found && staff && staff->m_drawingStaffDef) {
            const KeySig *keySig = staff->m_drawingStaffDef->GetCurrentKeySig();
            if (keySig) {
                MapOfOctavedPitchAccid keyAccids;
                keySig->FillMap(keyAccids);
                const auto keyIter = keyAccids.find(pname + oct * 7);
                if (keyIter != keyAccids.end()) {
                    alteration = TransPitch::GetChromaticAlteration(ACCIDENTAL_GESTURAL_NONE, keyIter->second);
                }
            }
        }
    }

    // The interval from the written note to its neighbor, added to the sounding pitch
    const int mainWritten
        = Note::PnameToPclass(note->GetPname()) + note->GetTransPitch().m_accid + (note->GetOct() + 1) * 12;
    const int neighborWritten = Note::PnameToPclass(pname) + alteration + (oct + 1) * 12;
    return pitch + (neighborWritten - mainWritten);
}

int GenerateMIDIFunctor::GetMIDIPitch(const Note *note)
{
    // The MIDI key of unpitched notes is neither transposed nor tuned
    if (note->HasPnum() && note->HasLoc() && !note->HasPname()) return note->GetPnum();
    if (m_customTuning && m_customTuning->IsValid()) {
        return m_customTuning->GetMIDIPitch(note, m_transSemi, m_octaveShift);
    }
    return note->GetMIDIPitch(m_transSemi, m_octaveShift);
}

//----------------------------------------------------------------------------
// GenerateTimemapFunctor
//----------------------------------------------------------------------------

GenerateTimemapFunctor::GenerateTimemapFunctor(Timemap *timemap) : ConstFunctor()
{
    m_currentScoreTime = 0;
    m_currentRealTimeMilliseconds = 0.0;
    m_currentTempo = MIDI_TEMPO;
    m_noCue = false;
    m_timemap = timemap;
}

FunctorCode GenerateTimemapFunctor::VisitLayerElement(const LayerElement *layerElement)
{
    if (layerElement->IsScoreDefElement()) return FUNCTOR_SIBLINGS;

    // Only resolve simple sameas links to avoid infinite recursion
    const LayerElement *sameas = dynamic_cast<const LayerElement *>(layerElement->GetSameasLink());
    if (sameas && !sameas->HasSameasLink()) {
        sameas->Process(*this);
    }

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateTimemapFunctor::VisitMeasure(const Measure *measure)
{
    m_currentScoreTime = measure->GetScoreTimeOnset();
    m_currentRealTimeMilliseconds = measure->GetRealTimeOnsetMilliseconds();
    m_currentTempo = measure->GetCurrentTempo();

    this->AddTimemapEntry(measure);

    return FUNCTOR_CONTINUE;
}

FunctorCode GenerateTimemapFunctor::VisitMRest(const MRest *mRest)
{
    this->AddTimemapEntry(mRest);

    return FUNCTOR_SIBLINGS;
}

FunctorCode GenerateTimemapFunctor::VisitMultiRest(const MultiRest *multiRest)
{
    this->AddTimemapEntry(multiRest);

    return FUNCTOR_SIBLINGS;
}

FunctorCode GenerateTimemapFunctor::VisitNote(const Note *note)
{
    // Skip cue notes when midiNoCue is activated
    if ((note->GetCue() == BOOLEAN_true) && m_noCue) {
        return FUNCTOR_SIBLINGS;
    }

    note = dynamic_cast<const Note *>(note->ThisOrSameasLink());
    assert(note);

    this->AddTimemapEntry(note);

    return FUNCTOR_SIBLINGS;
}

FunctorCode GenerateTimemapFunctor::VisitRest(const Rest *rest)
{
    this->AddTimemapEntry(rest);

    return FUNCTOR_SIBLINGS;
}

FunctorCode GenerateTimemapFunctor::VisitStaff(const Staff *staff)
{
    if (staff->IsOssia()) return FUNCTOR_SIBLINGS;

    return FUNCTOR_CONTINUE;
}

void GenerateTimemapFunctor::AddTimemapEntry(const Object *object)
{
    assert(object);

    if (object->IsAnyOf(std::array{ NOTE, REST })) {
        const DurationInterface *interface = object->GetDurationInterface();
        assert(interface);

        Fraction scoreTimeStart = m_currentScoreTime + interface->GetScoreTimeOnset();
        double realTimeStart = round(m_currentRealTimeMilliseconds + interface->GetRealTimeOnsetMilliseconds());

        Fraction scoreTimeEnd = m_currentScoreTime + interface->GetScoreTimeOffset();
        double realTimeEnd = round(m_currentRealTimeMilliseconds + interface->GetRealTimeOffsetMilliseconds());

        bool isRest = (object->Is(REST));

        /*********** start values ***********/

        TimemapEntry &startEntry = m_timemap->GetEntry(scoreTimeStart);

        // Should check if value for realTimeStart already exists and if so, then
        // ensure that it is equal to scoreTimeStart:
        startEntry.tstamp = realTimeStart;

        // Store the element ID in list to turn on at given time - note or rest
        if (!isRest) startEntry.notesOn.push_back(object->GetID());
        if (isRest) startEntry.restsOn.push_back(object->GetID());

        // Also add the tempo
        startEntry.tempo = m_currentTempo;

        /*********** end values ***********/

        TimemapEntry &endEntry = m_timemap->GetEntry(scoreTimeEnd);

        // Should check if value for realTimeEnd already exists and if so, then
        // ensure that it is equal to scoreTimeEnd:
        endEntry.tstamp = realTimeEnd;

        // Store the element ID in list to turn off at given time - notes or rest
        if (!isRest) endEntry.notesOff.push_back(object->GetID());
        if (isRest) endEntry.restsOff.push_back(object->GetID());
    }
    else if (object->Is(MEASURE)) {
        const Measure *measure = vrv_cast<const Measure *>(object);
        assert(measure);

        // Deal with repeated music later, for now get the last times.
        Fraction scoreTimeStart = m_currentScoreTime;
        double realTimeStart = round(m_currentRealTimeMilliseconds);

        TimemapEntry &startEntry = m_timemap->GetEntry(scoreTimeStart);

        // Should check if value for realTimeStart already exists and if so, then
        // ensure that it is equal to scoreTimeStart:
        startEntry.tstamp = realTimeStart;

        // Add the measureOn
        startEntry.measureOn = measure->GetID();
    }
    else if (object->IsAnyOf(std::array{ MREST, MULTIREST })) {
        // Get the ancestor measure
        const Measure *measure = vrv_cast<const Measure *>(object->GetFirstAncestor(MEASURE));
        assert(measure);

        Fraction scoreTimeStart = m_currentScoreTime;
        double realTimeStart = round(m_currentRealTimeMilliseconds);
        Fraction scoreTimeEnd = measure->GetScoreTimeOffset();
        double realTimeEnd = round(measure->GetRealTimeOffsetMilliseconds());

        /*********** start values ***********/

        TimemapEntry &startEntry = m_timemap->GetEntry(scoreTimeStart);

        // Should check if a value for realTimeStart already exists and if so, then
        // ensure that it is equal to scoreTimeStart:
        startEntry.tstamp = realTimeStart;

        // Store the element ID in list to turn on at given time
        startEntry.restsOn.push_back(object->GetID());

        /*********** end values ***********/

        TimemapEntry &endEntry = m_timemap->GetEntry(scoreTimeEnd);

        endEntry.tstamp = realTimeEnd;

        // Store the element ID in list to turn off at given time
        endEntry.restsOff.push_back(object->GetID());
    }
}

//----------------------------------------------------------------------------
// GenerateFeaturesFunctor
//----------------------------------------------------------------------------

GenerateFeaturesFunctor::GenerateFeaturesFunctor(FeatureExtractor *extractor) : ConstFunctor()
{
    m_extractor = extractor;
}

FunctorCode GenerateFeaturesFunctor::VisitObject(const Object *object)
{
    m_extractor->Extract(object);

    return FUNCTOR_CONTINUE;
}

} // namespace vrv
