#ifndef __VALUEPRED_UNIT_HH__
#define __VALUEPRED_UNIT_HH__

#include <string>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/valuepred/valuepred_metadata.hh"
#include "enums/ValuePredType.hh"
#include "params/ValuePredictor.hh"
#include "sim/sim_object.hh"
#include "sim/stats.hh"

namespace gem5
{

namespace valuepred
{

class VPUnit : public SimObject
{
  private:
    using Params = ValuePredictorParams;

  protected:
    const unsigned numThreads;

    void assertValidTid(ThreadID tid) const;

  public:
    VPUnit(const Params &params);

    std::string name() const { return "valuePredict.base"; }

    virtual VPResult valuePredict(VPPredMetaData *predMetadata) = 0;

    virtual void updateValuePredictor(VPUpdateMetaData *updateMetadata) = 0;

    virtual void specUpdateValuePredictor(VPSpecUpdateMetaData *specupdateMetadata) = 0;

    virtual void squash(ThreadID tid, const uint64_t seq_no) = 0;

    virtual ValuePredType getValuePredictorType() = 0;

  public:
    struct ValuePredUnitStats : public statistics::Group
    {
        ValuePredUnitStats(VPUnit *vp);

        statistics::Scalar VPcorrected;
        statistics::Scalar VPpredicted;
        statistics::Formula VPaccuracy;
        statistics::Scalar VPsupported;
        statistics::Formula VPcoverage;

    } stats;
};


}

}

#endif
