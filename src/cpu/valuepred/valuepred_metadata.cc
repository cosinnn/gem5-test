#include "cpu/valuepred/valuepred_metadata.hh"

#include <cassert>

namespace gem5
{

namespace valuepred
{

VPPredMetaData*
VPDataStructFactory::buildPredMetaData(ValuePredType type)
{
    switch (type) {
        case ValuePredType::IdealConstantLVP:
            return new VPPredMetaData();
        default:
            assert(0);
    }
    return nullptr;
}

VPUpdateMetaData*
VPDataStructFactory::buildUpdateMetaData(ValuePredType type)
{
    switch (type) {
        case ValuePredType::IdealConstantLVP:
            return new VPUpdateMetaData();
        default:
            assert(0);
    }
    return nullptr;
}

VPSpecUpdateMetaData*
VPDataStructFactory::buildSpecUpdateMetaData(ValuePredType type)
{
    switch (type) {
        case ValuePredType::IdealConstantLVP:
            return new VPSpecUpdateMetaData();
        default:
            assert(0);
    }
    return nullptr;
}


}

}
