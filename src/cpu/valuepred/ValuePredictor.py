from m5.params import *
from m5.proxy import *
from m5.SimObject import *

class ValuePredType(ScopedEnum):
    vals = ["IdealConstantLVP"]

class ValuePredictor(SimObject):
    type = "ValuePredictor"
    cxx_class = "gem5::valuepred::VPUnit"
    cxx_header = "cpu/valuepred/valuepred_unit.hh"
    abstract = True
    numThreads = Param.Unsigned(Parent.numThreads, "Number of threads")

class IdealConstantLVP(ValuePredictor):
    type = "IdealConstantLVP"
    cxx_class = "gem5::valuepred::IdealConstantLVP"
    cxx_header = "cpu/valuepred/ideal_constant_lvp.hh"
    abstract = False

    satCounterBits = Param.Unsigned(9, "bits of saturating counter, initial value is 0")
    resetConfidence = Param.Bool(True, "reset confidence to 0 when mispredict")
