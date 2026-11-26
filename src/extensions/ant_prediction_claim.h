#pragma once

// A node that takes an ant score on trust walks nothing, yet it has to know which solution makes an identity
// qualify for the prediction phase: every qualified identity adds log events, and a log id must be the same
// on a node that walked. Where the walk rules leave no doubt, the answer is read off the claim.

#include "public_settings.h"
#include "mining/prediction_phase.h"

// Answer of a network this node never built. Lite only: a computed answer is 0, 1 or PREDICTION_NO_ANSWER.
static constexpr unsigned char PREDICTION_ANSWER_NOT_COMPUTED = 3;

// Set once an entry holds an answer this node did not compute. Its counted hours, prediction digest and
// accuracy keys are not the quorum's from then on.
static bool gPredictionAnswersIncomplete = false;

// Set for the whole run by --skip-verify-solutions: a score taken on trust does not tell whether its identity qualified.
static bool gPredictionQualificationUnknown = false;

static bool predictionHasUncomputedAnswers(const PredictionPhase& phase)
{
    for (unsigned int i = 0; i < phase.count; i++)
    {
        for (unsigned int hour = 0; hour < PREDICTION_MAX_HOURS; hour++)
        {
            if (phase.entries[i].answers[hour] == PREDICTION_ANSWER_NOT_COMPUTED)
            {
                return true;
            }
        }
    }
    return false;
}

enum class AntClaimVerdict : unsigned char
{
    DoesNotQualify,
    Qualifies,
    // Only the walk can tell.
    Undecided,
};

struct AntClaimInference
{
    AntClaimVerdict verdict;
    // Lowest shift an honest walk can have ended at. Above 0 whenever the verdict rests on the shift.
    unsigned int shiftLowerBound;
    // Which of the checks in antQualifiesFromClaim() decided, 1 for the first.
    unsigned char rule;
};

// A claim at or below the advance threshold would end at the shift limit, which the claim does not tell apart
// from frame 0 unless it qualifies on its error alone.
static_assert(BPP9000_PREDICTION_THRESHOLD >= BPP9000_ADVANCE_THRESHOLD, "a mastered frame must qualify on its error");

// Whether an accepted solution clears the prediction floor (shift > 0 or error <= BPP9000_PREDICTION_THRESHOLD),
// for a claim that is honest; a false one is refunded here only and rolled back.
// parentShift and parentError are the stored parent's, ignored for a root parent. rootFirstFrameError() gives the
// errors of the identity's root on frame 0 and is called only when nothing cheaper decides.
//
// Rests on the walk in mining/score_bpp9000.h (advanceShift, rollingStepsFrom):
// - a step that slid the frame ends with at most BPP9000_ADVANCE_THRESHOLD + 1 errors,
// - an explore step is kept only if its error is not below the current one, an exploit step only if it is not worse,
// - so a walk that starts at BPP9000_ADVANCE_THRESHOLD + 2 errors or more and ends there never left its frame.
template<typename RootErrorFn>
static AntClaimInference antQualifiesFromClaim(unsigned int claimedError, unsigned int shiftLimit, bool parentIsRoot,
    unsigned int parentShift, unsigned int parentError, RootErrorFn&& rootFirstFrameError)
{
    const unsigned int startShift = parentIsRoot ? 0u : parentShift;

    if (claimedError <= BPP9000_PREDICTION_THRESHOLD)
    {
        return { AntClaimVerdict::Qualifies, startShift, 1 };
    }
    if (startShift >= 1)
    {
        return { AntClaimVerdict::Qualifies, startShift, 2 };
    }
    if (shiftLimit == 0)
    {
        return { AntClaimVerdict::DoesNotQualify, 0, 3 };
    }
    // One error past the threshold is what a frame holds right after a slide, and also a plain frame-0 result.
    if (claimedError == BPP9000_ADVANCE_THRESHOLD + 1)
    {
        return { AntClaimVerdict::Undecided, 0, 4 };
    }

    if (!parentIsRoot)
    {
        // The walk slides before its first step, and a time-out there leaves the frame moved.
        if (parentError <= BPP9000_ADVANCE_THRESHOLD)
        {
            return { AntClaimVerdict::Undecided, 0, 5 };
        }
        // A child is accepted only if it beats its parent, which without fewer errors takes a higher shift.
        if (claimedError >= parentError)
        {
            return { AntClaimVerdict::Qualifies, startShift + 1, 6 };
        }
        return { AntClaimVerdict::DoesNotQualify, 0, 7 };
    }

    // A root child needs no better rating than its root, so only a root that starts clear of a slide is decided.
    if (rootFirstFrameError() >= BPP9000_ADVANCE_THRESHOLD + 2)
    {
        return { AntClaimVerdict::DoesNotQualify, 0, 8 };
    }
    return { AntClaimVerdict::Undecided, 0, 9 };
}
