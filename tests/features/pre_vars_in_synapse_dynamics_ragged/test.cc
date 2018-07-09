// Google test includes
#include "gtest/gtest.h"

// Autogenerated simulation code includess
#include DEFINITIONS_HEADER

// **NOTE** base-class for simulation tests must be
// included after auto-generated globals are includes
#include "../../utils/simulation_test_vars.h"
#include "../../utils/simulation_neuron_policy_pre_var.h"
#include "../../utils/simulation_synapse_policy_ragged.h"

// Combine neuron and synapse policies together to build variable-testing fixture
typedef SimulationTestVars<SimulationNeuronPolicyPreVar, SimulationSynapsePolicyRagged> SimTest;

TEST_P(SimTest, AcceptableError)
{
  INIT_SPARSE(MODEL_NAME);

  float err = Simulate(
    [](unsigned int i, unsigned int d, unsigned int j, float t, float &newX)
    {
        if (t > 0.0001+(d+1)*DT)
        {
            newX = t-DT-(d+1)*DT+10*j;
            return true;
        }
        else
        {
          return false;
        }
    });

  // Check total error is less than some tolerance
  EXPECT_LT(err, 5e-3);
}

#ifndef CPU_ONLY
auto simulatorBackends = ::testing::Values(true, false);
#else
auto simulatorBackends = ::testing::Values(false);
#endif

WRAPPED_INSTANTIATE_TEST_CASE_P(MODEL_NAME,
                                SimTest,
                                simulatorBackends);