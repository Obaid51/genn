#include "optimiser.h"

// Standard C++ includes
#include <iostream>
#include <map>
#include <numeric>

// CUDA includes
#include <cuda.h>
#include <cuda_runtime.h>

// PLOG includes
#include <plog/Log.h>

// Filesystem includes
#include "path.h"

// GeNN includes
#include "modelSpec.h"

// GeNN code generator includes
#include "code_generator/generateAll.h"

// CUDA backend includes
#include "utils.h"

namespace
{
typedef std::map<unsigned int, std::pair<bool, size_t>> KernelOptimisationOutput;

void getDeviceArchitectureProperties(const cudaDeviceProp &deviceProps, size_t &warpAllocGran, size_t &regAllocGran,
                                     size_t &smemAllocGran, size_t &maxBlocksPerSM)
{
    if(deviceProps.major == 1) {
        smemAllocGran = 512;
        warpAllocGran = 2;
        regAllocGran = (deviceProps.minor < 2) ? 256 : 512;
        maxBlocksPerSM = 8;
    }
    else if(deviceProps.major == 2) {
        smemAllocGran = 128;
        warpAllocGran = 2;
        regAllocGran = 64;
        maxBlocksPerSM = 8;
    }
    else if(deviceProps.major == 3) {
        smemAllocGran = 256;
        warpAllocGran = 4;
        regAllocGran = 256;
        maxBlocksPerSM = 16;
    }
    else if(deviceProps.major == 5) {
        smemAllocGran = 256;
        warpAllocGran = 4;
        regAllocGran = 256;
        maxBlocksPerSM = 32;
    }
    else if(deviceProps.major == 6) {
        smemAllocGran = 256;
        warpAllocGran = (deviceProps.minor == 0) ? 2 : 4;
        regAllocGran = 256;
        maxBlocksPerSM = 32;
    }
    else {
        smemAllocGran = 256;
        warpAllocGran = 4;
        regAllocGran = 256;
        maxBlocksPerSM = 32;

        if(deviceProps.major > 7) {
            LOGW << "Unsupported CUDA device major version: " << deviceProps.major;
            LOGW << "This is a bug! Please report it at https://github.com/genn-team/genn.";
            LOGW << "Falling back to next latest SM version parameters.";
        }
    }
}
//--------------------------------------------------------------------------
void calcGroupSizes(const NNmodel &model, std::vector<unsigned int> (&groupSizes)[CodeGenerator::CUDA::Backend::KernelMax])
{
    using namespace CodeGenerator;
    using namespace CUDA;

    // **TODO** this belongs in code generator somewhere

    // Loop through neuron groups
    for(const auto &n : model.getLocalNeuronGroups()) {
        // Add number of neurons to vector of neuron kernels
        groupSizes[Backend::KernelNeuronUpdate].push_back(n.second.getNumNeurons());

        // If this neuron group requires on-device initialisation
        if(n.second.isSimRNGRequired() || n.second.isInitCodeRequired()) {
            groupSizes[Backend::KernelInitialize].push_back(n.second.getNumNeurons());
        }
    }

    // Loop through synapse groups
    for(const auto &s : model.getLocalSynapseGroups()) {
        groupSizes[Backend::KernelPresynapticUpdate].push_back(Backend::getNumPresynapticUpdateThreads(s.second));

        if(!s.second.getWUModel()->getLearnPostCode().empty()) {
            groupSizes[Backend::KernelPostsynapticUpdate].push_back(Backend::getNumPostsynapticUpdateThreads(s.second));
        }

        if (!s.second.getWUModel()->getLearnPostCode().empty()) {
            groupSizes[Backend::KernelSynapseDynamicsUpdate].push_back(Backend::getNumSynapseDynamicsThreads(s.second));
        }

        // If synapse group has individual weights and needs device initialisation
        if((s.second.getMatrixType() & SynapseMatrixWeight::INDIVIDUAL) && s.second.isWUVarInitRequired()) {
            const unsigned int numSrcNeurons = s.second.getSrcNeuronGroup()->getNumNeurons();
            const unsigned int numTrgNeurons = s.second.getTrgNeuronGroup()->getNumNeurons();
            if(s.second.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                groupSizes[Backend::KernelInitializeSparse].push_back(numSrcNeurons);
            }
            else {
                groupSizes[Backend::KernelInitialize].push_back(numSrcNeurons * numTrgNeurons);
            }
        }
    }

    // Add group sizes for reset kernels
    groupSizes[Backend::KernelPreNeuronReset].push_back(model.getLocalNeuronGroups().size());
    groupSizes[Backend::KernelPreSynapseReset].push_back(model.getNumPreSynapseResetRequiredGroups());
}
//--------------------------------------------------------------------------
KernelOptimisationOutput optimizeBlockSize(int deviceID, const NNmodel &model, CodeGenerator::CUDA::Backend::KernelBlockSize &blockSize, 
                                           const CodeGenerator::CUDA::Backend::Preferences &preferences, const filesystem::path &outputPath)
{
    using namespace CodeGenerator;
    using namespace CUDA;

    // Calculate model group sizes
    std::vector<unsigned int> groupSizes[Backend::KernelMax];
    calcGroupSizes(model, groupSizes);

    // Create CUDA drive API device and context for accessing kernel attributes
    CUdevice cuDevice;
    CUcontext cuContext;
    CHECK_CU_ERRORS(cuDeviceGet(&cuDevice, deviceID));
    CHECK_CU_ERRORS(cuCtxCreate(&cuContext, 0, cuDevice));

    // Bitset to mark which kernels are present and array of their attributes for each repetition
    cudaFuncAttributes krnlAttr[2][Backend::KernelMax];

    // Do two repititions with different candidate kernel size
    const size_t warpSize = 32;
    const size_t repBlockSizes[2] = {warpSize, warpSize * 2};
    KernelOptimisationOutput kernelsToOptimise;
    for(unsigned int r = 0; r < 2; r++) {
        LOGD  << "Generating code with block size:" << repBlockSizes[r];

        // Start with all group sizes set to warp size
        std::fill(blockSize.begin(), blockSize.end(), repBlockSizes[r]);

        // Create backend
        Backend backend(blockSize, preferences, 0, deviceID);

        // Generate code
        const auto moduleNames = generateAll(model, backend, outputPath);

        // Set context
        // **NOTE** CUDA calls in code generation seem to lose driver context
        CHECK_CU_ERRORS(cuCtxSetCurrent(cuContext));

        // Loop through generated modules
        for(const auto &m : moduleNames) {
            // Build module
            const std::string modulePath = (outputPath / m).str();
            
            const std::string nvccCommand = "nvcc -cubin " + backend.getNVCCFlags() + " -o " + modulePath + ".cubin " + modulePath + ".cc";
            if(system(nvccCommand.c_str()) != 0) {
                throw std::runtime_error("optimizeBlockSize: NVCC failed");
            }

            // Load compiled module
            CUmodule module;
            CHECK_CU_ERRORS(cuModuleLoad(&module, (modulePath + ".cubin").c_str()));

            // Loop through kernels
            for (unsigned int k = 0; k < Backend::KernelMax; k++) {
                // If function is found
                CUfunction kern;
                CUresult res = cuModuleGetFunction(&kern, module, Backend::KernelNames[k]);
                if (res == CUDA_SUCCESS) {
                    LOGD << "\tKernel '" << Backend::KernelNames[k] << "' found";

                    // Read it's attributes and add blank entry to map of kernels to optimise
                    cudaFuncGetAttributes(&krnlAttr[r][k], kern);
                    kernelsToOptimise.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(k),
                                              std::forward_as_tuple(false, 0));

                    LOGD << "\t\tShared memory bytes:" << krnlAttr[r][k].sharedSizeBytes;
                    LOGD << "\t\tNum registers:" << krnlAttr[r][k].numRegs;
                }
            }

            // Unload module
            CHECK_CU_ERRORS(cuModuleUnload(module));
        }
    }

    // Destroy context
    CHECK_CU_ERRORS(cuCtxDestroy(cuContext));

    // Get device properties
    cudaDeviceProp deviceProps;
    CHECK_CUDA_ERRORS(cudaGetDeviceProperties(&deviceProps, deviceID));

    // Get properties of device architecture
    size_t warpAllocGran;
    size_t regAllocGran;
    size_t smemAllocGran;
    size_t maxBlocksPerSM;
    getDeviceArchitectureProperties(deviceProps, warpAllocGran, regAllocGran, smemAllocGran, maxBlocksPerSM);

    // Zero block sizes
    std::fill(blockSize.begin(), blockSize.end(), 0);

    // Loop through kernels to optimise
    for(auto &k : kernelsToOptimise) {
        LOGD << "Kernel '" << Backend::KernelNames[k.first] << "':";

        // Get required number of registers and shared memory bytes for this kernel
        // **NOTE** register requirements are assumed to remain constant as they're vector-width
        const size_t reqNumRegs = krnlAttr[0][k.first].numRegs;
        const size_t reqSharedMemBytes[2] = {krnlAttr[0][k.first].sharedSizeBytes, krnlAttr[1][k.first].sharedSizeBytes};

        // Calculate coefficients for requiredSharedMemBytes = (A * blockThreads) + B model
        const size_t reqSharedMemBytesA = (reqSharedMemBytes[1] - reqSharedMemBytes[0]) / (repBlockSizes[1] - repBlockSizes[0]);
        const size_t reqSharedMemBytesB = reqSharedMemBytes[0] - (reqSharedMemBytesA * repBlockSizes[0]);

        // Loop through possible
        const size_t maxBlockWarps = deviceProps.maxThreadsPerBlock / warpSize;
        for(size_t blockWarps = 1; blockWarps < maxBlockWarps; blockWarps++) {
            const size_t blockThreads = blockWarps * warpSize;
            LOGD << "\tCandidate block size:" << blockThreads;

            // Estimate shared memory for block size and padd
            const size_t reqSharedMemBytes = Utils::padSize((reqSharedMemBytesA * blockThreads) + reqSharedMemBytesB, smemAllocGran);
            LOGD << "\t\tEstimated shared memory required:" << reqSharedMemBytes << " bytes (padded)";

            // Calculate number of blocks the groups used by this kernel will require
            const size_t reqBlocks = std::accumulate(groupSizes[k.first].begin(), groupSizes[k.first].end(), 0,
                                                        [blockThreads](size_t acc, size_t size)
                                                        {
                                                            return acc + Utils::ceilDivide(size, blockThreads);
                                                        });
            LOGD << "\t\tBlocks required (according to padded sum):" << reqBlocks;

            // Start estimating SM block limit - the number of blocks of this size that can run on a single SM
            size_t smBlockLimit = deviceProps.maxThreadsPerMultiProcessor / blockThreads;
            LOGD << "\t\tSM block limit due to maxThreadsPerMultiProcessor:" << smBlockLimit;

            smBlockLimit = std::min(smBlockLimit, maxBlocksPerSM);
            LOGD << "\t\tSM block limit corrected for maxBlocksPerSM:" << smBlockLimit;

            // If register allocation is per-block
            if (deviceProps.major == 1) {
                // Pad size of block based on warp allocation granularity
                const size_t paddedNumBlockWarps = Utils::padSize(blockWarps, warpAllocGran);

                // Calculate number of registers per block and pad with register allocation granularity
                const size_t paddedNumRegPerBlock = Utils::padSize(paddedNumBlockWarps * reqNumRegs * warpSize, regAllocGran);

                // Update limit based on maximum registers per block
                // **NOTE** this doesn't quite make sense either
                smBlockLimit = std::min(smBlockLimit, deviceProps.regsPerBlock / paddedNumRegPerBlock);
            }
            // Otherwise, if register allocation is per-warp
            else {
                // Caculate number of registers per warp and pad with register allocation granularity
                const size_t paddedNumRegPerWarp = Utils::padSize(reqNumRegs * warpSize, regAllocGran);

                // **THINK** I don't understand this
                //blockLimit = floor(deviceProps.regsPerBlock / (paddedNumRegPerWarp * warpAllocGran)*warpAllocGran;

                // **NOTE** this doesn't quite make sense either
                //smBlockLimit = std::min(smBlockLimit, blockLimit / blockWarps);
            }
            LOGD << "\t\tSM block limit corrected for registers:" << smBlockLimit;

            // If this kernel requires any shared memory, update limit to reflect shared memory available in each multiprocessor
            // **NOTE** this used to be sharedMemPerBlock but that seems incorrect
            if(reqSharedMemBytes != 0) {
                smBlockLimit = std::min(smBlockLimit, deviceProps.sharedMemPerMultiprocessor / reqSharedMemBytes);
                LOGD << "\t\tSM block limit corrected for shared memory:" << smBlockLimit;
            }

            // Calculate occupancy
            const size_t newOccupancy = blockWarps * smBlockLimit * deviceProps.multiProcessorCount;

            // Use a small block size if it allows all groups to occupy the device concurrently
            if (reqBlocks <= (smBlockLimit * deviceProps.multiProcessorCount)) {
                blockSize[k.first] = blockThreads;
                k.second.second = newOccupancy;
                k.second.first = true;

                LOGD << "\t\tSmall model situation detected - block size:" << blockSize[k.first];

                // For small model the first (smallest) block size allowing it is chosen
                break;
            }
            // Otherwise, if we've improved on previous best occupancy
            else if(newOccupancy > k.second.second) {
                blockSize[k.first] = blockThreads;
                k.second.second = newOccupancy;

                LOGD << "\t\tNew highest occupancy: " << newOccupancy << ", block size:" << blockSize[k.first];
            }

        }

        LOGI << "Kernel: " << Backend::KernelNames[k.first] << ", block size:" << blockSize[k.first];
    }

    // Return optimisation data
    return kernelsToOptimise;
}
//--------------------------------------------------------------------------
int chooseOptimalDevice(const NNmodel &model, CodeGenerator::CUDA::Backend::KernelBlockSize &blockSize, 
                        const CodeGenerator::CUDA::Backend::Preferences &preferences, const filesystem::path &outputPath)
{
    using namespace CodeGenerator;
    using namespace CUDA;
    
    // Get number of devices
    int deviceCount;
    CHECK_CUDA_ERRORS(cudaGetDeviceCount(&deviceCount));
    if(deviceCount == 0) {
        throw std::runtime_error("No CUDA devices found");
    }

    // Loop through devices
    typedef std::tuple<int, size_t, size_t, Backend::KernelBlockSize> Device;
    std::vector<Device> devices;
    devices.reserve(deviceCount);
    for(int d = 0; d < deviceCount; d++) {
        // Get properties
        cudaDeviceProp deviceProps;
        CHECK_CUDA_ERRORS(cudaGetDeviceProperties(&deviceProps, d));
        const int smVersion = (deviceProps.major * 10) + deviceProps.minor;

        // Optimise block size for this device
        Backend::KernelBlockSize optimalBlockSize;
        const auto kernels = optimizeBlockSize(d, model, optimalBlockSize, preferences, outputPath);

        // Sum up occupancy of each kernel
        const size_t totalOccupancy = std::accumulate(kernels.begin(), kernels.end(), 0,
                                                      [](size_t acc, const KernelOptimisationOutput::value_type &kernel)
                                                      {
                                                          return acc + kernel.second.second;
                                                      });

        // Count number of kernels that count as small models
        const size_t numSmallModelKernels = std::accumulate(kernels.begin(), kernels.end(), 0,
                                                            [](size_t acc, const KernelOptimisationOutput::value_type &kernel)
                                                            {
                                                                return acc + (kernel.second.first ? 1 : 0);
                                                            });

        LOGD << "Device " << d << " - total occupancy:" << totalOccupancy << ", number of small models:" << numSmallModelKernels << ", SM version:" << smVersion;
        devices.emplace_back(smVersion, totalOccupancy, numSmallModelKernels, optimalBlockSize);
    }

    // Find best device
    const auto bestDevice = std::min_element(devices.cbegin(), devices.cend(),
        [](const Device &a, const Device &b)
        {
            // If 'a' have a higher number of small model kernels -  return true - it is better
            const size_t numSmallModelKernelsA = std::get<2>(a);
            const size_t numSmallModelKernelsB = std::get<2>(b);
            if (numSmallModelKernelsA > numSmallModelKernelsB) {
                return true;
            }
            // Otherwise, if the two devices have an identical small model kernel count
            else if(numSmallModelKernelsA == numSmallModelKernelsB) {
                // If 'a' has a higher total occupancy - return true - it is better
                const size_t totalOccupancyA = std::get<1>(a);
                const size_t totalOccupancyB = std::get<1>(b);
                if(totalOccupancyA > totalOccupancyB) {
                    return true;
                }
                // Otherwise, if the two devices have identical occupancy
                else if(totalOccupancyA == totalOccupancyB) {
                    // If 'a' has a higher SM version - return true - it's better
                    const int smVersionA = std::get<0>(a);
                    const int smVersionB = std::get<0>(b);
                    if(smVersionA > smVersionB) {
                        return true;
                    }
                }
            }

            // 'a' is not better - return false
            return false;
        });

    // Find ID of best device
    const int bestDeviceID = (int)std::distance(devices.cbegin(), bestDevice);
    LOGI << "Optimal  device " << bestDeviceID << " - total occupancy:" << std::get<1>(*bestDevice) << ", number of small models:" << std::get<2>(*bestDevice) << ", SM version:" << std::get<0>(*bestDevice);

    // Get optimal block size from best device
    blockSize = std::get<3>(*bestDevice);


    // Return ID of best device
    return bestDeviceID;
}
//--------------------------------------------------------------------------
int chooseDeviceWithMostGlobalMemory()
{
    // Get number of devices
    int deviceCount;
    CHECK_CUDA_ERRORS(cudaGetDeviceCount(&deviceCount));
    if(deviceCount == 0) {
        throw std::runtime_error("No CUDA devices found");
    }

    // Loop through devices
    size_t mostGlobalMemory = 0;
    int bestDevice = -1;
    for(int d = 0; d < deviceCount; d++) {
        // Get properties
        cudaDeviceProp deviceProps;
        CHECK_CUDA_ERRORS(cudaGetDeviceProperties(&deviceProps, d));

        // If this device improves on previous best
        if(deviceProps.totalGlobalMem > mostGlobalMemory) {
            mostGlobalMemory = deviceProps.totalGlobalMem;
            bestDevice = d;
        }
    }

    LOGI << "Using device " << bestDevice << " which has " << mostGlobalMemory << " bytes of global memory";
    return bestDevice;
}
}
// CodeGenerator::Backends::Optimiser
namespace CodeGenerator
{
namespace CUDA
{
namespace Optimiser
{
Backend createBackend(const NNmodel &model, const filesystem::path &outputPath, int localHostID, 
                      const Backend::Preferences &preferences)
{
    if(preferences.autoChooseDevice) {
        // Choose optimal device
        Backend::KernelBlockSize cudaBlockSize;
        const int deviceID = chooseOptimalDevice(model, cudaBlockSize, preferences, outputPath);

        // Create backends
        Backend backend(cudaBlockSize, preferences, localHostID, deviceID);
        return std::move(backend);
    }
    else {
        const int deviceID = chooseDeviceWithMostGlobalMemory();
        
        // Optimise block size
        Backend::KernelBlockSize cudaBlockSize;
        optimizeBlockSize(deviceID, model, cudaBlockSize, preferences, outputPath);
        
        // Create backends
        Backend backend(cudaBlockSize, preferences, localHostID, deviceID);
        return std::move(backend);
    }
}
}   // namespace Optimiser
}   // namespace CUDA
}   // namespace CodeGenerator