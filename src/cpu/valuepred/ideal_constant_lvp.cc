#include "cpu/valuepred/ideal_constant_lvp.hh"

#include <cassert>

#include "base/trace.hh"
#include "cpu/valuepred/valuepred_metadata.hh"
#include "debug/IdealConstantLVP.hh"

namespace gem5
{

namespace valuepred
{

IdealConstantLVP::IdealConstantLVP(const Params &params)
    : VPUnit(params),
      idealConstTables(params.numThreads),
      satCounterBits(params.satCounterBits),
      resetConfidence(params.resetConfidence)
{
}

VPResult
IdealConstantLVP::valuePredict(VPPredMetaData *predMetaData)
{
    assertValidTid(predMetaData->tid);
    auto &idealConstTable = idealConstTables[predMetaData->tid];
    auto it = idealConstTable.find(predMetaData->pc);
    DPRINTF(IdealConstantLVP,
            "[IdealConstantLVP::valuePredict] Tid:%i Seq:%lu PC:%#lx | ",
            predMetaData->tid, predMetaData->seq_no, predMetaData->pc);
    if (it != idealConstTable.end()) {
        const auto &entry = it->second;
        bool saturated = entry.confidence.isSaturated();
        DPRINTF(IdealConstantLVP,
                "HIT (conf=%u/%u, saturated=%s, stored_value=%#lx) => ",
                (unsigned)entry.confidence, (1U << satCounterBits) - 1,
                saturated ? "yes" : "no", entry.value);
        if (saturated) {
            DPRINTF(IdealConstantLVP, "PREDICT value=%#lx\n", entry.value);
            return {true, entry.value};
        }
        DPRINTF(IdealConstantLVP, "SKIP (not saturated)\n");
        return {false, 0};
    }
    DPRINTF(IdealConstantLVP, "MISS => NO_PREDICTION\n");
    return {false, 0};
}

void
IdealConstantLVP::updateValuePredictor(VPUpdateMetaData *updateMetaData)
{
    assertValidTid(updateMetaData->tid);
    auto &idealConstTable = idealConstTables[updateMetaData->tid];
    auto it = idealConstTable.find(updateMetaData->pc);
    if (it == idealConstTable.end()) {
        auto [it, success] = idealConstTable.emplace(std::piecewise_construct,
            std::forward_as_tuple(updateMetaData->pc),
            std::forward_as_tuple(satCounterBits, updateMetaData->actualValue));

        assert(success);
        DPRINTF(IdealConstantLVP,
                "[IdealConstantLVP::update] Tid:%i Seq:%lu PC:%#lx | "
                "ALLOC (new_entry, init_value=%#lx)\n",
                updateMetaData->tid, updateMetaData->seq_no,
                updateMetaData->pc, updateMetaData->actualValue);
    } else {
        bool validActualValue = updateMetaData->actualValue != 0xdeadbeefULL;
        unsigned old_conf = (unsigned)it->second.confidence;
        if (validActualValue && updateMetaData->actualValue == it->second.value) {
            it->second.confidence++;
            DPRINTF(IdealConstantLVP,
                    "[IdealConstantLVP::update] Tid:%i Seq:%lu PC:%#lx | "
                    "TRAIN: MATCH (stored=%#lx, actual=%#lx) "
                    "conf: %u -> %u [INC]\n",
                    updateMetaData->tid, updateMetaData->seq_no,
                    updateMetaData->pc, it->second.value,
                    updateMetaData->actualValue, old_conf,
                    (unsigned)it->second.confidence);
        } else {
            if (resetConfidence) {
                it->second.confidence.reset();
                DPRINTF(IdealConstantLVP,
                        "[IdealConstantLVP::update] Tid:%i Seq:%lu PC:%#lx | "
                        "TRAIN: MISMATCH (stored=%#lx, actual=%#lx) "
                        "conf: %u -> 0 [RESET], new_value=%#lx\n",
                        updateMetaData->tid, updateMetaData->seq_no,
                        updateMetaData->pc, it->second.value,
                        updateMetaData->actualValue, old_conf,
                        updateMetaData->actualValue);
            } else {
                it->second.confidence--;
                DPRINTF(IdealConstantLVP,
                        "[IdealConstantLVP::update] Tid:%i Seq:%lu PC:%#lx | "
                        "TRAIN: MISMATCH (stored=%#lx, actual=%#lx) "
                        "conf: %u -> %u [DEC], new_value=%#lx\n",
                        updateMetaData->tid, updateMetaData->seq_no,
                        updateMetaData->pc, it->second.value,
                        updateMetaData->actualValue, old_conf,
                        (unsigned)it->second.confidence,
                        updateMetaData->actualValue);
            }
            it->second.value = updateMetaData->actualValue;
        }
    }
}

void
IdealConstantLVP::specUpdateValuePredictor(VPSpecUpdateMetaData *specUpdateMetaData)
{
    DPRINTF(IdealConstantLVP,
            "[IdealConstantLVP::specUpdate] Tid:%i | NO-OP (not used)\n",
            specUpdateMetaData->tid);
}

void
IdealConstantLVP::squash(ThreadID tid, const uint64_t seq_no)
{
    DPRINTF(IdealConstantLVP,
            "[IdealConstantLVP::squash] Tid:%i Seq:%lu | NO-OP (stateless)\n",
            tid, seq_no);
}

} // namespace valuepred

} // namespace gem5
