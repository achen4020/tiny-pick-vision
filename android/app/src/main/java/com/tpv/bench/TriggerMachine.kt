package com.tpv.bench

import kotlin.math.abs

enum class FramePresence { PRESENT, EMPTY, DROP }

data class FrameObservation(
    val presence: FramePresence,
    val x: Int, val y: Int,
    val classId: Int,
    val frameIdxInRun: Long,
    val detection: TpvDetectionDebugV2?
)

enum class MachineState { IDLE, CANDIDATE, COMMITTED }

sealed class StateMachineOutput {
    object None : StateMachineOutput()
    data class Commit(val event: CommittedEvent) : StateMachineOutput()
}

data class CommittedEvent(
    val eventIdx: Long,
    val triggerFrameIdx: Long,
    val triggerFrameDebug: TpvDetectionDebugV2,
    val eventClassId: Int,
    val classIdHistogram: Map<Int, Int>,
    val flicker: Boolean,
)

class TriggerMachine(
    private val nStable: Int,
    private val kEmpty: Int,
    private val mDriftPx: Int,
) {
    init {
        require(nStable >= 1) { "nStable must be ≥ 1" }
        require(kEmpty  >= 1) { "kEmpty must be ≥ 1" }
        require(mDriftPx >= 0) { "mDriftPx must be ≥ 0" }
    }

    var state: MachineState = MachineState.IDLE ; private set

    // Window of accumulated PRESENT observations during CANDIDATE.
    // window[0] = first frame (used as position anchor per §4.2).
    private val window = ArrayList<FrameObservation>(nStable)
    private var emptyCount = 0
    private var nextEventIdx = 1L

    fun onFrame(obs: FrameObservation): StateMachineOutput {
        // DROP frames are never passed to the state machine per spec §4.2
        // ("frame dropped, does not advance state, does not enter window").
        if (obs.presence == FramePresence.DROP) return StateMachineOutput.None

        return when (state) {
            MachineState.IDLE      -> handleIdle(obs)
            MachineState.CANDIDATE -> handleCandidate(obs)
            MachineState.COMMITTED -> handleCommitted(obs)
        }
    }

    private fun handleIdle(obs: FrameObservation): StateMachineOutput {
        if (obs.presence != FramePresence.PRESENT) return StateMachineOutput.None
        window.clear()
        window.add(obs)
        state = MachineState.CANDIDATE
        // Promote on this same frame when nStable == 1 — SettingsState allows
        // N=1 and spec §4.2 says the window size alone decides. Refactored
        // into checkPromote() so both handleIdle and handleCandidate share it.
        return checkPromote()
    }

    private fun handleCandidate(obs: FrameObservation): StateMachineOutput {
        if (obs.presence == FramePresence.EMPTY) {
            reset()
            return StateMachineOutput.None
        }
        // PRESENT: check position stability against window[0]
        val first = window[0]
        if (abs(obs.x - first.x) > mDriftPx ||
            abs(obs.y - first.y) > mDriftPx) {
            reset()
            return StateMachineOutput.None
        }
        window.add(obs)
        return checkPromote()
    }

    /** Promote to COMMITTED once the window has accumulated N_stable
     *  PRESENT frames. Shared between handleIdle (covers N=1) and
     *  handleCandidate (covers N>=2). */
    private fun checkPromote(): StateMachineOutput {
        if (window.size < nStable) return StateMachineOutput.None
        val event = buildCommit()
        state = MachineState.COMMITTED
        emptyCount = 0
        window.clear()
        return StateMachineOutput.Commit(event)
    }

    private fun handleCommitted(obs: FrameObservation): StateMachineOutput {
        if (obs.presence == FramePresence.EMPTY) {
            emptyCount += 1
            if (emptyCount >= kEmpty) {
                state = MachineState.IDLE
                emptyCount = 0
            }
        } else {
            // PRESENT (DROP already filtered above)
            emptyCount = 0
        }
        return StateMachineOutput.None
    }

    private fun reset() {
        window.clear()
        state = MachineState.IDLE
    }

    private fun buildCommit(): CommittedEvent {
        val nth = window.last()                // window[nStable-1]
        val histogram = HashMap<Int, Int>()
        for (f in window) histogram[f.classId] = (histogram[f.classId] ?: 0) + 1

        // Majority vote with tie-break: real classes (0..4) > AMBIGUOUS (0xFE) > REJECTED (0xFF)
        val eventClass = histogram.entries
            .sortedWith(
                compareByDescending<Map.Entry<Int, Int>> { it.value }
                    .thenBy { classPriority(it.key) }
            )
            .first().key

        val ev = CommittedEvent(
            eventIdx = nextEventIdx,
            triggerFrameIdx = nth.frameIdxInRun,
            triggerFrameDebug = requireNotNull(nth.detection) {
                "PRESENT frame must carry detection (frameIdx=${nth.frameIdxInRun})"
            },
            eventClassId = eventClass,
            classIdHistogram = histogram.toMap(),
            flicker = histogram.size >= 2,
        )
        nextEventIdx += 1
        return ev
    }

    /** Lower value wins the tie-break. */
    private fun classPriority(classId: Int) = when {
        classId in 0..4 -> 0
        classId == 0xFE -> 1
        classId == 0xFF -> 2
        else -> 3
    }
}
