/*
 * EStride is largely based on the open-source implementation of the
 * 1st-place Championship Value Prediction (CVP-1) submission.
 *
 * The version in this codebase is adapted and modified for our environment
 * by Yibo Zhang <yb_zhang@mail.ustc.edu.cn>; it is not intended to be a
 * bit-exact copy of the original code.
 *
 * For detailed background and reference material:
 * Paper: https://microarch.org/cvp1/papers/Seznec.pdf
 * Open-source implementation: https://www.microarch.org/cvp1/code/Seznec.tar.gz
 * Official website: https://www.microarch.org/cvp1/
 */

#ifndef __ENHANCED_STTIDE_HH__
#define __ENHANCED_STTIDE_HH__

#include <climits>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

#include "base/random.hh"
#include "base/types.hh"
#include "cpu/valuepred/es_metadata.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/EStride.hh"

namespace gem5
{

namespace valuepred
{

class EStride : public VPUnit
{

  private:
    using Params = EStrideParams;
    using UpdateConfDecision = std::pair<bool, int>;

    static constexpr uint64_t FASTINSTTIME = 100u;
    static constexpr uint64_t L1HITMAXTIME = 100u;
    static constexpr uint64_t L2HITMAXTIME = 100u;
    static constexpr uint64_t L3HITMAXTIME = 100u;

  private:
    class ESEntry
    {
      public:
        uint32_t tag = 0;
        int confidence = 0;
        int64_t stride = 0;
        RegVal lastValue = 0;
        int useful = 0;
        unsigned NotFirstAppear = 0;
    };

    class InflightWindow : public Named
    {
      public:
        int windowTagLength;
        bool idealWindow;
        std::unordered_map<uint64_t, int> windows;

        int windowActiveCount;

        uint64_t lastSeqNo;

      public:
        std::string name() const override { return "inflight window"; }

        InflightWindow(int windowTagLength, bool idealWindow);

        int addToInflightWindow(Addr pc);

        void removeFromWindow(Addr pc, uint64_t seq_no);

        void squash(uint64_t seq_no);

      private:
        using HashMethod = uint64_t (*)(uint64_t, int);
        HashMethod hashMethod;
    };

  private:
    const int ways;
    const int strideWidth;
    const int tagWidth;
    const int logESTBEntrys;
    const int entryCounts;
    const int logMaxConfidence;
    const int MAXCONFIDENCE;
    const int confidenceThreshold;
    Random::RandomPtr rng;
    std::vector<InflightWindow> inflightWindows;
    const bool enableTimeMsgInUpdate;

  private:
    std::vector<std::vector<std::vector<ESEntry>>> ESTables;

  private:
    VPResult doPredict(ESPredMetaData *esPredMetaData, int inflights);

    int64_t extendStride(int64_t entryStride);

    uint32_t pcHashToWayIndex(Addr pc, int way);
    uint32_t pcHashToTag(Addr pc, int way);

    uint32_t compareTags(uint32_t tag1, uint32_t tag2);

    UpdateConfDecision decideToUpdate(const ESUpdateMetaData *esUpdateMetaData,
                                      int64_t stride);

    uint32_t tryDecUseful(const ESEntry &entry);

  public:
    EStride(const Params &params);

    std::string name() const override { return "EStride"; }

    virtual VPResult valuePredict(VPPredMetaData *predMetaData) override;

    virtual void updateValuePredictor(VPUpdateMetaData *updateMetaData) override;

    virtual void specUpdateValuePredictor(
        VPSpecUpdateMetaData *specUpdateMetaData) override;

    virtual void squash(ThreadID tid, const uint64_t seq_no) override;

    virtual ValuePredType getValuePredictorType() override
    {
        return ValuePredType::EStride;
    }

  private:
    std::unordered_map<Addr, std::pair<int32_t, std::string>>
        fluentInstructions;

  public:
    struct EStrideStats : public statistics::Group
    {
        statistics::Vector2d allocate;
        statistics::Vector2d strideNotEquals;
        statistics::Vector2d strideEquals;

        statistics::SparseHistogram inflightSH;

        EStrideStats(statistics::Group *parent)
            : statistics::Group(parent),
              ADD_STAT(allocate, "Record the assignment of valuepred_unit entries"),
              ADD_STAT(strideNotEquals,
                       "Record the situations of stride not equals"),
              ADD_STAT(strideEquals,
                       "Record the situations of stride equals"),
              ADD_STAT(inflightSH,
                       "Records the number of inflight instructions")
        {
        }
    } esstats;
};

}

}

#endif
