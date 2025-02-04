/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include "common/LLVMWarningsPush.hpp"
#include <llvmWrapper/IR/Function.h>
#include "common/LLVMWarningsPop.hpp"

#include "AdaptorCommon/ImplicitArgs.hpp"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/DeSSA.hpp"
#include "Compiler/CISACodeGen/GenCodeGenModule.h"
#include "Compiler/CISACodeGen/messageEncoding.hpp"
#include "Compiler/CISACodeGen/VariableReuseAnalysis.hpp"
#include "Compiler/CISACodeGen/PixelShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/VertexShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/GeometryShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/ComputeShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/HullShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/DomainShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/OpenCLKernelCodeGen.hpp"
#include "DebugInfo.hpp"
#include "Compiler/MetaDataApi/MetaDataApi.h"
#include "common/secure_mem.h"

using namespace llvm;
using namespace IGC;
using namespace IGC::IGCMD;

CShader::CShader(Function* pFunc, CShaderProgram* pProgram)
    : entry(pFunc)
    , m_parent(pProgram)
    , encoder()
    , m_HasBarrier(false)
{
    m_ctx = m_parent->GetContext();
    m_WI = nullptr;
    m_deSSA = nullptr;
    m_coalescingEngine = nullptr;
    m_DL = nullptr;
    m_FGA = nullptr;
    m_VRA = nullptr;
    m_shaderStats = nullptr;
    m_constantBufferMask = 0;
    m_constantBufferLoaded = 0;
    isInputsPulled = false;
    m_cbSlot = -1;
    m_statelessCBPushedSize = 0;
    isMessageTargetDataCacheDataPort = false;
    m_BindingTableEntryCount = 0;
    m_BindingTableUsedEntriesBitmap = 0;
    memset(&m_simdProgram, 0, sizeof(m_simdProgram));
    // [OCL] preAnalysis()/ParseShaderSpecificOpcode() must
    // set this to ture if there is any stateless access.
    m_HasGlobalStatelessMemoryAccess = false;
    m_HasConstantStatelessMemoryAccess = false;

}

void CShader::InitEncoder(SIMDMode simdSize, bool canAbortOnSpill, ShaderDispatchMode shaderMode)
{
    m_sendStallCycle = 0;
    m_staticCycle = 0;
    m_maxBlockId = 0;
    m_ScratchSpaceSize = 0;
    m_R0 = nullptr;
    m_NULL = nullptr;
    m_TSC = nullptr;
    m_SR0 = nullptr;
    m_CR0 = nullptr;
    m_CE0 = nullptr;
    m_DBG = nullptr;
    m_HW_TID = nullptr;
    m_SP = nullptr;
    m_SavedSP = nullptr;
    m_ARGV = nullptr;
    m_RETV = nullptr;

    // SIMD32 is a SIMD16 shader with 2 instance of each instruction
    m_SIMDSize = (simdSize == SIMDMode::SIMD8 ? SIMDMode::SIMD8 : SIMDMode::SIMD16);
    m_ShaderDispatchMode = shaderMode;
    m_numberInstance = simdSize == SIMDMode::SIMD32 ? 2 : 1;
    m_dispatchSize = simdSize;
    globalSymbolMapping.clear();
    symbolMapping.clear();
    ccTupleMapping.clear();
    ConstantPool.clear();
    setup.clear();
    patchConstantSetup.clear();
    encoder.SetProgram(this);
}

// Pre-analysis pass to be executed before call to visa builder so we can pass scratch space offset
void CShader::PreAnalysisPass()
{
    ExtractGlobalVariables();

    auto funcMDItr = m_ModuleMetadata->FuncMD.find(entry);
    if (funcMDItr != m_ModuleMetadata->FuncMD.end())
    {
        if (funcMDItr->second.privateMemoryPerWI != 0)
        {
            if (GetContext()->getModuleMetaData()->compOpt.UseScratchSpacePrivateMemory)
            {
                m_ScratchSpaceSize = funcMDItr->second.privateMemoryPerWI * numLanes(m_dispatchSize);
                // Round up to GENX_GRF_REG_SIZ-byte aligned.
                m_ScratchSpaceSize =
                    ((GENX_GRF_REG_SIZ + m_ScratchSpaceSize - 1) / GENX_GRF_REG_SIZ) * GENX_GRF_REG_SIZ;

            }
        }
    }

    for (auto BB = entry->begin(), BE = entry->end(); BB != BE; ++BB) {
        llvm::BasicBlock* pLLVMBB = &(*BB);
        llvm::BasicBlock::InstListType& instructionList = pLLVMBB->getInstList();
        for (auto I = instructionList.begin(), E = instructionList.end(); I != E; ++I) {
            llvm::Instruction* inst = &(*I);
            ParseShaderSpecificOpcode(inst);
        }
    }
}

SProgramOutput* CShader::ProgramOutput()
{
    return &m_simdProgram;
}

void CShader::EmitEOTURBWrite()
{
    CEncoder& encoder = GetEncoder();
    uint messageLength = 3;

    // Creating a payload of size 3 = header + channelmask + undef data
    // As EOT message cant have message length == 0, setting channel mask = 0 and data = undef.
    CVariable* pEOTPayload = GetNewVariable(messageLength * numLanes(SIMDMode::SIMD8), ISA_TYPE_D, IGC::EALIGN_GRF);

    CVariable* zero = ImmToVariable(0x0, ISA_TYPE_D);
    // write at handle 0
    CopyVariable(pEOTPayload, zero, 0);
    // use 0 as write mask
    CopyVariable(pEOTPayload, zero, 1);

    uint exDesc = EU_MESSAGE_TARGET_URB | (1 << 5);

    uint desc = UrbMessage(
        messageLength,
        0,
        true,
        false,
        true,
        0,
        EU_GEN8_URB_OPCODE_SIMD8_WRITE);

    CVariable* pMessDesc = ImmToVariable(desc, ISA_TYPE_D);

    encoder.Send(nullptr, pEOTPayload, exDesc, pMessDesc);
    encoder.Push();
}

void CShader::EOTRenderTarget()
{
    // dummy render target to do EOT and free release the pixel scoreboard
    EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL msgControl =
        (m_SIMDSize == SIMDMode::SIMD8)
        ? EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD8_SINGLE_SOURCE_LOW
        : EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD16_SINGLE_SOURCE;

    bool perCoarse = false;
    if(GetShaderType() == ShaderType::PIXEL_SHADER)
    {
        perCoarse = (static_cast<CPixelShader*>(this)->GetPhase() != PSPHASE_LEGACY);
    }

    uint Desc = PixelDataPort(
        false,
        m_SIMDSize == SIMDMode::SIMD8 ? 4 : 8,
        0,
        false,
        perCoarse,
        false,
        true, // scoreboard release
        false,
        msgControl,
        m_pBtiLayout->GetNullSurfaceIdx());

    this->SetBindingTableEntryCountAndBitmap(true, m_pBtiLayout->GetNullSurfaceIdx());

    // EOT = bit 5
    // NULL Render target = bit 20
    uint exDesc = EU_MESSAGE_TARGET_DATA_PORT_WRITE | BIT(5) | BIT(20);

    CVariable* payload = GetNewVariable(4 * numLanes(m_SIMDSize), ISA_TYPE_UD, EALIGN_GRF);
    encoder.SendC(nullptr, payload, exDesc, ImmToVariable(Desc, ISA_TYPE_UD));
    encoder.Push();
}

void CShader::AddEpilogue(llvm::ReturnInst* ret)
{
    encoder.EndOfThread();
    encoder.Push();
}

CVariable* CShader::CreateSP(bool ptr64bits)
{
    // create argument-value register, limited to 12 GRF
    m_ARGV = GetNewVariable(getGRFSize() * 3, ISA_TYPE_D, getGRFAlignment(), false, 1);
    encoder.GetVISAPredefinedVar(m_ARGV, PREDEFINED_ARG);
    // create return-value register, limited to 4 GRF
    m_RETV = GetNewVariable(getGRFSize(), ISA_TYPE_D, getGRFAlignment(), false, 1);
    encoder.GetVISAPredefinedVar(m_RETV, PREDEFINED_RET);
    // create stack-pointer register
    if (ptr64bits) {
        encoder.SetKernelStackPointer64();
    }
    if (ptr64bits) {
        m_SP = GetNewVariable(1, ISA_TYPE_UQ, EALIGN_QWORD, true, 1);
        encoder.GetVISAPredefinedVar(m_SP, PREDEFINED_FE_SP);
    } else {
        m_SP = GetNewVariable(1, ISA_TYPE_UD, EALIGN_DWORD, true, 1);
        encoder.GetVISAPredefinedVar(m_SP, PREDEFINED_FE_SP);
    }
    return m_SP;
}

/// initial stack-pointer at the beginning of the kernel
void CShader::InitKernelStack(bool ptr64bits)
{
    CreateSP(ptr64bits);
    ImplicitArgs implicitArgs(*entry, m_pMdUtils);
    unsigned numPushArgs = m_ModuleMetadata->pushInfo.pushAnalysisWIInfos.size();
    unsigned numImplicitArgs = implicitArgs.size();
    unsigned numFuncArgs = IGCLLVM::GetFuncArgSize(entry) - numImplicitArgs - numPushArgs;

    Argument* kerArg = nullptr;
    llvm::Function::arg_iterator arg = entry->arg_begin();
    for (unsigned i = 0; i < numFuncArgs; ++i, ++arg);
    for (unsigned i = 0; i < numImplicitArgs; ++i, ++arg) {
        ImplicitArg implicitArg = implicitArgs[i];
        if (implicitArg.getArgType() == ImplicitArg::ArgType::PRIVATE_BASE)
        {
            kerArg = (&*arg);
            break;
        }
    }
    assert(kerArg);

    CVariable* pHWTID = GetNewVariable(1, ISA_TYPE_UD, EALIGN_DWORD, true, 1);
    encoder.SetSrcRegion(0, 0, 1, 0);
    encoder.SetSrcSubReg(0, 5);
    encoder.And(pHWTID, GetR0(), ImmToVariable(0x1ff, ISA_TYPE_UD));
    encoder.Push();
    // hard-code per-workitem private-memory size to 8k
    CVariable* pSize = ImmToVariable(8 * 1024 * numLanes(m_dispatchSize), ISA_TYPE_UD);
    CVariable* pTemp = GetNewVariable(1, ISA_TYPE_UD, EALIGN_DWORD, true, 1);
    encoder.Mul(pTemp, pHWTID, pSize);
    encoder.Push();
    // reserve space for alloca

    auto funcMDItr = m_ModuleMetadata->FuncMD.find(entry);
    if (funcMDItr != m_ModuleMetadata->FuncMD.end())
    {
        if (funcMDItr->second.privateMemoryPerWI != 0)
        {
            unsigned totalAllocaSize = funcMDItr->second.privateMemoryPerWI * numLanes(m_dispatchSize);
            encoder.Add(pTemp, pTemp, ImmToVariable(totalAllocaSize, ISA_TYPE_UD));
            encoder.Push();
        }
    }

    // modify private-memory size to a large setting
    m_ModuleMetadata->FuncMD[entry].privateMemoryPerWI = 8192;
    CVariable* pBase = GetSymbol(kerArg);
    encoder.Add(m_SP, pBase, pTemp);
    encoder.Push();
}

/// save stack-pointer when entering a stack-call function
void CShader::SaveSP()
{
    assert(!m_SavedSP && m_SP);
    m_SavedSP = GetNewVariable(m_SP);
    encoder.Copy(m_SavedSP, m_SP);
    encoder.Push();
}

/// restore stack-pointer when exiting a stack-call function
void CShader::RestoreSP()
{
    assert(m_SavedSP && m_SP);
    encoder.Copy(m_SP, m_SavedSP);
    encoder.Push();
    m_SavedSP = nullptr;
}

void CShader::CreateImplicitArgs()
{
    m_numBlocks = entry->size();
    m_R0 = GetNewVariable(getGRFSize() >> 2, ISA_TYPE_D, EALIGN_GRF, false, 1);
    encoder.GetVISAPredefinedVar(m_R0, PREDEFINED_R0);

    // create variables for implicit args
    ImplicitArgs implicitArgs(*entry, m_pMdUtils);
    unsigned numImplicitArgs = implicitArgs.size();

    // Push Args are only for entry function
    unsigned numPushArgsEntry = m_ModuleMetadata->pushInfo.pushAnalysisWIInfos.size();
    unsigned numPushArgs = (isEntryFunc(m_pMdUtils, entry) ? numPushArgsEntry : 0);
    unsigned numFuncArgs = IGCLLVM::GetFuncArgSize(entry) - numImplicitArgs - numPushArgs;

    // Create symbol for every arguments [5/2019]
    //   (Previously, symbols are created only for implicit args.)
    //   Since vISA requires input var (argument) to be root symbol (CVariable)
    //   and GetSymbol() does not guarantee this due to coalescing of argument
    //   values and others. Here, we handle arguments specially by creating
    //   a CVariable symbol for each argument, and use this newly-created symbol
    //   as the root symbol for its congruent class if any. This should always
    //   work as it does not matter which value in a coalesced set is going to
    //   be a root symbol.
    //
    //   Once a root symbol is created, the root value of its conguent class
    //   needs to have as its symbol an alias to this root symbol.

    // Update SymbolMapping for argument value.
    auto updateArgSymbolMapping = [&](Value* Arg, CVariable* CVarArg) {
        symbolMapping.insert(std::make_pair(Arg, CVarArg));
        Value *Node = m_deSSA ? m_deSSA->getRootValue(Arg) : nullptr;
        if (Node)
        {
            // If Arg isn't root, must setup symbolMapping for root.
            if (Node != Arg) {
                // 'Node' should not have a symbol entry at this moment.
                assert(symbolMapping.count(Node) == 0 &&
                    "Root symbol of arg should not be set at this point!");
                CVariable* aV = CVarArg;
                if (IGC_GET_FLAG_VALUE(EnableDeSSAAlias) >= 2)
                {
                    aV = createAliasIfNeeded(Node, CVarArg);
                }
                symbolMapping[Node] = aV;
            }
        }
    };

    llvm::Function::arg_iterator arg = entry->arg_begin();
    for (unsigned i = 0; i < numFuncArgs; ++i, ++arg)
    {
        Value* ArgVal = arg;
        if (ArgVal->use_empty())
            continue;
        e_alignment algn = GetPreferredAlignment(ArgVal, m_WI, m_ctx);
        CVariable* ArgCVar = GetNewVector(ArgVal, algn);
        updateArgSymbolMapping(ArgVal, ArgCVar);
    }

    for (unsigned i = 0; i < numImplicitArgs; ++i, ++arg) {
        ImplicitArg implictArg = implicitArgs[i];
        assert((implictArg.getNumberElements() < (UINT16_MAX)) && "getNumberElements > higher than 64k");

        bool isUniform = implictArg.getDependency() == WIAnalysis::UNIFORM;
        uint16_t nbElements = (uint16_t)implictArg.getNumberElements();

        CVariable* var = GetNewVariable(nbElements,
            implictArg.getVISAType(*m_DL),
            implictArg.getAlignType(*m_DL),
            isUniform,
            isUniform ? 1 : m_numberInstance);

        if (implictArg.getArgType() == ImplicitArg::R0) {
            encoder.GetVISAPredefinedVar(var, PREDEFINED_R0);
        }

        // This is a per function symbol mapping, that is, only available for a
        // llvm function which will be cleared for each run of EmitVISAPass.
        updateArgSymbolMapping(arg, var);

        // Kernel's implicit arguments's symbols will be available for the
        // whole kernel CodeGen. With this, there is no need to pass implicit
        // arguments and this should help to reduce the register pressure with
        // presence of subroutines.
        assert(!globalSymbolMapping.count(&(*arg)) && "should not exist already");
        globalSymbolMapping.insert(std::make_pair(&(*arg), var));
    }

    for (unsigned i = 0; i < numPushArgs; ++i, ++arg)
    {
        Value* ArgVal = arg;
        if (ArgVal->use_empty())
            continue;
        e_alignment algn = GetPreferredAlignment(ArgVal, m_WI, m_ctx);
        CVariable* ArgCVar = GetNewVector(ArgVal, algn);
        updateArgSymbolMapping(ArgVal, ArgCVar);
    }

    CreateAliasVars();
}

// For sub-vector aliasing, pre-allocating cvariables for those
// valeus that have sub-vector aliasing before emit instructions.
// (The sub-vector aliasing is done in VariableReuseAnalysis.)
void CShader::CreateAliasVars()
{
    // Create CVariables for vector aliasing (This is more
    // efficient than doing it on-fly inside getSymbol()).
    if (IGC_IS_FLAG_ENABLED(VATemp) &&
        !m_VRA->m_aliasMap.empty())
    {
        // For each vector alias root, generate cvariable
        // for it and all its component sub-vector
        for (auto& II : m_VRA->m_aliasMap)
        {
            SSubVecDesc* SV = II.second;
            Value* rootVal = SV->BaseVector;
            if (SV->Aliaser != rootVal)
                continue;
            CVariable* rootCVar = GetSymbol(rootVal);

            // Generate all vector aliasers and their
            // dessa root if any.
            for (int i = 0, sz = (int)SV->Aliasers.size(); i < sz; ++i)
            {
                SSubVecDesc* aSV = SV->Aliasers[i];
                Value* V = aSV->Aliaser;
                // Create alias cvariable for Aliaser and its dessa root if any
                Value* Vals[2] = { V, nullptr };
                if (m_deSSA) {
                    Value* dessaRootVal = m_deSSA->getRootValue(V);
                    if (dessaRootVal && dessaRootVal != V)
                        Vals[1] = dessaRootVal;
                }
                int startIx = aSV->StartElementOffset;

                for (int i = 0; i < 2; ++i)
                {
                    V = Vals[i];
                    if (!V)
                        continue;

                    Type *Ty = V->getType();
                    VectorType* VTy = dyn_cast<VectorType>(Ty);
                    Type *BTy = VTy ? VTy->getElementType() : Ty;
                    int nelts = (VTy ? (int)VTy->getNumElements() : 1);

                    VISA_Type visaTy = GetType(BTy);
                    int typeBytes = (int)CEncoder::GetCISADataTypeSize(visaTy);
                    int offsetInBytes = typeBytes * startIx;
                    int nbelts = nelts;
                    if (!rootCVar->IsUniform())
                    {
                        int width = (int)numLanes(m_SIMDSize);
                        offsetInBytes *= width;
                        nbelts *= width;
                    }
                    CVariable* Var = GetNewAlias(rootCVar, visaTy, offsetInBytes, nbelts);
                    symbolMapping.insert(std::pair<llvm::Value*, CVariable*>(V, Var));
                }
            }
        }
    }
}

void CShader::AddSetup(uint index, CVariable* var)
{
    if (setup.size() < index + 1) {
        setup.resize(index + 1, nullptr);
    }
    if (setup[index] == nullptr) {
        setup[index] = var;
    }
}

void CShader::AddPatchConstantSetup(uint index, CVariable* var)
{
    if (patchConstantSetup.size() < index + 1) {
        patchConstantSetup.resize(index + 1, nullptr);
    }
    if (patchConstantSetup[index] == nullptr) {
        patchConstantSetup[index] = var;
    }
}

void CShader::AllocateInput(CVariable* var, uint offset, uint instance)
{
    // the input offset must respect the variable alignment
    assert(offset % as[var->GetAlign()] == 0);
    encoder.DeclareInput(var, offset, instance);
}

void CShader::AllocateOutput(CVariable* var, uint offset, uint instance)
{
    assert(offset % as[var->GetAlign()] == 0);
    encoder.DeclareInput(var, offset, instance);
    encoder.MarkAsOutput(var);
}

void CShader::AllocateConstants3DShader(uint& offset)
{
    if (m_Platform->WaForceCB0ToBeZeroWhenSendingPC() && m_DriverInfo->implementPushConstantWA()) {
        // Allocate space for constant pushed from the constant buffer
        AllocateConstants(offset);
        AllocateSimplePushConstants(offset);
        // Allocate space for constant set by driver
        AllocateNOSConstants(offset);
    } else {
        // Allocate space for constant set by driver
        AllocateNOSConstants(offset);
        // Allocate space for constant pushed from the constant buffer
        AllocateConstants(offset);
        AllocateSimplePushConstants(offset);
    }
}

void CShader::AllocateConstants(uint& offset)
{
    m_ConstantBufferLength = 0;
    for (auto I = pushInfo.constants.begin(), E = pushInfo.constants.end(); I != E; I++) {
        CVariable* var = GetSymbol(m_argListCache[I->second]);
        AllocateInput(var, offset + m_ConstantBufferLength);
        m_ConstantBufferLength += var->GetSize();
    }

    m_ConstantBufferLength = iSTD::Align(m_ConstantBufferLength, getGRFSize());
    offset += m_ConstantBufferLength;
}

void CShader::AllocateSimplePushConstants(uint& offset)
{
    for(unsigned int i = 0; i < pushInfo.simplePushBufferUsed; i++)
    {
        for(auto I : pushInfo.simplePushInfoArr[i].simplePushLoads)
        {
            uint subOffset = I.first;
            CVariable* var = GetSymbol(m_argListCache[I.second]);
            AllocateInput(var, subOffset - pushInfo.simplePushInfoArr[i].offset + offset);
        }
        offset += pushInfo.simplePushInfoArr[i].size;
    }
}

void CShader::AllocateNOSConstants(uint& offset)
{
    uint maxConstantPushed = 0;

    for (auto I = pushInfo.constantReg.begin(), E = pushInfo.constantReg.end(); I != E; I++) {
        CVariable* var = GetSymbol(m_argListCache[I->second]);
        AllocateInput(var, offset + I->first * SIZE_DWORD);
        maxConstantPushed = std::max(maxConstantPushed, I->first + 1);
    }
    maxConstantPushed = iSTD::Max(maxConstantPushed, static_cast<uint>(m_ModuleMetadata->MinNOSPushConstantSize));
    m_NOSBufferSize = iSTD::Align(maxConstantPushed * SIZE_DWORD, getGRFSize());
    offset += m_NOSBufferSize;
}


void CShader::CreateGatherMap()
{
    int index = -1;
    gatherMap.reserve(pushInfo.constants.size());
    for(auto I = pushInfo.constants.begin(), E = pushInfo.constants.end();I!=E;I++)
    {
        unsigned int address = (I->first.bufId * 256 * 4) + (I->first.eltId);
        unsigned int cstOffset = address / 4;
        unsigned int cstChannel = address % 4;
        if(cstOffset!=index)
        {
            USC::SConstantGatherEntry entry;
            entry.GatherEntry.Fields.constantBufferOffset = cstOffset%256;
            entry.GatherEntry.Fields.channelMask = BIT(cstChannel);
            // with 3DSTATE_DX9_CONSTANT if buffer is more than 4Kb,
            //  the constant after 255 can be accessed in constant buffer 1
            int CBIndex = cstOffset/256;
            entry.GatherEntry.Fields.constantBufferIndex = CBIndex;
            m_constantBufferMask |= BIT(CBIndex);
            gatherMap.push_back(entry);
            index = cstOffset;
        }
        else
        {
            gatherMap[gatherMap.size()-1].GatherEntry.Fields.channelMask |= BIT(cstChannel);
        }
    }

    // The size of the gather map must be even
    if(gatherMap.size()%2!=0)
    {
        USC::SConstantGatherEntry entry;
        entry.GatherEntry.Value = 0;
        gatherMap.push_back(entry);
    }
}

void  CShader::CreateConstantBufferOutput(SKernelProgram *pKernelProgram)
{
    pKernelProgram->ConstantBufferMask = m_constantBufferMask;
    pKernelProgram->gatherMapSize = gatherMap.size();
    if (pKernelProgram->gatherMapSize > 0)
    {
        pKernelProgram->gatherMap = new char[pKernelProgram->gatherMapSize*sizeof(USC::SConstantGatherEntry)];
        memcpy_s(pKernelProgram->gatherMap, pKernelProgram->gatherMapSize *
                                              sizeof(USC::SConstantGatherEntry),
                 &gatherMap[0],
                 gatherMap.size() * sizeof(USC::SConstantGatherEntry));
        pKernelProgram->ConstantBufferLength = m_ConstantBufferLength / getGRFSize(); // in number of GRF bits
    }

    if (m_cbSlot != -1)
    {
        pKernelProgram->bufferSlot = m_cbSlot;
        pKernelProgram->statelessCBPushedSize = m_statelessCBPushedSize;
    }

    // for simple push
    for(unsigned int i = 0; i < pushInfo.simplePushBufferUsed; i++)
    {
        pKernelProgram->simplePushInfoArr[i].m_cbIdx = pushInfo.simplePushInfoArr[i].cbIdx;
        pKernelProgram->simplePushInfoArr[i].m_offset = pushInfo.simplePushInfoArr[i].offset;
        pKernelProgram->simplePushInfoArr[i].m_size = pushInfo.simplePushInfoArr[i].size;
        pKernelProgram->simplePushInfoArr[i].isStateless = pushInfo.simplePushInfoArr[i].isStateless;
    }

    if (GetContext()->m_ConstantBufferReplaceShaderPatterns)
    {
        pKernelProgram->m_ConstantBufferReplaceShaderPatterns = GetContext()->m_ConstantBufferReplaceShaderPatterns;
        pKernelProgram->m_ConstantBufferReplaceShaderPatternsSize = GetContext()->m_ConstantBufferReplaceShaderPatternsSize;
        pKernelProgram->m_ConstantBufferUsageMask = GetContext()->m_ConstantBufferUsageMask;
        pKernelProgram->m_ConstantBufferReplaceSize = GetContext()->m_ConstantBufferReplaceSize;
    }
}

void CShader::CreateFunctionSymbol(llvm::Function* pFunc)
{
    // Functions with uses in this module requires relocation
    CVariable* funcAddr = GetSymbol(pFunc);
    std::string funcName = pFunc->getName().str();
    encoder.AddVISASymbol(funcName, funcAddr);
    encoder.Push();
}

void CShader::CreateGlobalSymbol(llvm::GlobalVariable* pGlobal)
{
    CVariable* globalAddr = GetSymbol(pGlobal);
    std::string globalName = pGlobal->getName().str();
    encoder.AddVISASymbol(globalName, globalAddr);
    encoder.Push();
}

void CShader::CacheArgumentsList()
{
    m_argListCache.clear();
    for (auto arg = entry->arg_begin(); arg != entry->arg_end(); ++arg)
        m_argListCache.push_back(&(*arg));
}

void CShader::MapPushedInputs()
{
   for(auto I = pushInfo.inputs.begin(), E = pushInfo.inputs.end();I!=E;I++)
   {
       // We need to map the value associated with the value pushed to a physical register
       if( I->second.interpolationMode == EINTERPOLATION_CONSTANT )
       {
           if(GetShaderType() == ShaderType::PIXEL_SHADER)
           {
               static_cast<CPixelShader*>(this)->MarkConstantInterpolation(I->second.index);
           }
       }
       CVariable* var = GetSymbol(m_argListCache[I->second.argIndex]);
       AddSetup(I->second.index, var);
   }
}

CVariable* CShader::GetR0()
{
    return m_R0;
}

CVariable* CShader::GetNULL()
{
    if (!m_NULL)
    {
        m_NULL = new (Allocator)CVariable(2, true, ISA_TYPE_D, EVARTYPE_GENERAL, EALIGN_DWORD, false, 1);
        encoder.GetVISAPredefinedVar(m_NULL, PREDEFINED_NULL);
    }
    return m_NULL;
}

CVariable* CShader::GetTSC()
{
    if(!m_TSC)
    {
        m_TSC = new (Allocator) CVariable(2, true, ISA_TYPE_D, EVARTYPE_GENERAL, EALIGN_DWORD, false, 1);
        encoder.GetVISAPredefinedVar(m_TSC, PREDEFINED_TSC);
    }
    return m_TSC;
}

CVariable* CShader::GetSR0()
{
    if(!m_SR0)
    {
        m_SR0 = GetNewVariable(4, ISA_TYPE_UD, EALIGN_DWORD, true);
        encoder.GetVISAPredefinedVar(m_SR0, PREDEFINED_SR0);
    }
    return m_SR0;
}

CVariable* CShader::GetCR0()
{
    if (!m_CR0)
    {
        m_CR0 = GetNewVariable(3, ISA_TYPE_UD, EALIGN_DWORD, true);
        encoder.GetVISAPredefinedVar(m_CR0, PREDEFINED_CR0);
    }
    return m_CR0;
}

CVariable* CShader::GetCE0()
{
    if(!m_CE0)
    {
        m_CE0 = GetNewVariable(1, ISA_TYPE_UD, EALIGN_DWORD, true);
        encoder.GetVISAPredefinedVar(m_CE0, PREDEFINED_CE0);
    }
    return m_CE0;
}

CVariable* CShader::GetDBG()
{
    if (!m_DBG)
    {
        m_DBG = GetNewVariable(2, ISA_TYPE_D, EALIGN_DWORD, true);
        encoder.GetVISAPredefinedVar(m_DBG, PREDEFINED_DBG);
    }
    return m_DBG;
}

CVariable* CShader::GetHWTID()
{
    if (!m_HW_TID)
    {
        m_HW_TID = GetNewVariable(1, ISA_TYPE_UD, EALIGN_DWORD, true, 1);
        encoder.GetVISAPredefinedVar(m_HW_TID, PREDEFINED_HW_TID);
    }
    return m_HW_TID;
}


CVariable* CShader::GetSP()
{
    assert(m_SP);
    return m_SP;
}

CVariable* CShader::GetARGV()
{
    assert(m_ARGV);
    return m_ARGV;
}

CVariable* CShader::GetRETV()
{
    assert(m_RETV);
    return m_RETV;
}

CEncoder&  CShader::GetEncoder()
{
    return encoder;
}

CShader::~CShader()
{
    // free all the memory allocated
    Destroy();
}

bool CShader::IsValueUsed(llvm::Value* value)
{
    auto it = symbolMapping.find(value);
    if( it != symbolMapping.end() )
    {
        return true;
    }
    return false;
}

CVariable* CShader::GetGlobalCVar(llvm::Value* value)
{
    auto it = globalSymbolMapping.find(value);
    if (it != globalSymbolMapping.end())
        return it->second;
    return nullptr;
}

CVariable* CShader::BitCast( CVariable* var, VISA_Type newType )
{
    CVariable* bitCast = nullptr;
    if( var->IsImmediate() )
    {
        bitCast = ImmToVariable( var->GetImmediateValue(), newType );
    }
    else
    {
        // TODO: we need to store this bitCasted var to avoid creating many times
        bitCast = GetNewAlias( var, newType, 0, 0 );
    }
    return bitCast;
}

CVariable* CShader::ImmToVariable(uint64_t immediate, VISA_Type type)
{
    VISA_Type immType = type;

    if (type == ISA_TYPE_BOOL)
    {
        // bool immediates cannot be inlined
        uint immediateValue = immediate ? 0xFFFFFFFF : 0;
        CVariable* immVar = new (Allocator)  CVariable(immediateValue, ISA_TYPE_UD);
        // src-variable is no longer a boolean, V-ISA cannot take boolean-src immed.

        CVariable* dst = GetNewVariable(numLanes(m_dispatchSize), ISA_TYPE_BOOL, EALIGN_BYTE);
        // FIXME: We need to pop/push the encoder context
        //encoder.save();
        encoder.SetP(dst, immVar);
        encoder.Push();
        return dst;
    }

    CVariable* var = new (Allocator) CVariable(immediate, immType);
    return var;
}

CVariable* CShader::GetNewVariable(uint16_t nbElement, VISA_Type type, e_alignment align, bool isUniform, uint16_t numberInstance)
{
    e_varType varType;
    if(type == ISA_TYPE_BOOL)
    {
        varType = EVARTYPE_PREDICATE;
    }
    else
    {
        assert(align >= CEncoder::GetCISADataTypeAlignment(type));
        varType = EVARTYPE_GENERAL;
    }
    CVariable* var = new (Allocator) CVariable(nbElement, isUniform, type, varType,  align, false, numberInstance);
    encoder.CreateVISAVar(var);
    return var;
}

CVariable*  CShader::GetNewVariable(const CVariable* from)
{
    CVariable* var = new (Allocator) CVariable(*from);
    encoder.CreateVISAVar(var);
    return var;
}

CVariable* CShader::GetNewAddressVariable(uint16_t nbElement, VISA_Type type, bool isUniform, bool isVectorUniform)
{
    CVariable* var = new (Allocator) CVariable(nbElement, isUniform, type, EVARTYPE_ADDRESS, EALIGN_DWORD, isVectorUniform, 1);
    encoder.CreateVISAVar(var);
    return var;
}

bool CShader::GetIsUniform(llvm::Value* v) const
{
    return m_WI ? (m_WI->whichDepend(v) == WIAnalysis::UNIFORM) : false;
}

bool CShader::InsideDivergentCF(llvm::Instruction* inst)
{
    return m_WI ? m_WI->insideDivergentCF(inst) : true;
}

uint CShader::GetNbVectorElementAndMask(llvm::Value* val, uint32_t& mask)
{
    llvm::Type* type = val->getType();
    uint nbElement = int_cast<uint>(cast<VectorType>(type)->getNumElements());
    mask = 0;
    // we don't process vector bigger than 31 elements as the mask has only 32bits
    // If we want to support longer vectors we need to extend the mask size
    //
    // If val has been coalesced, don't prune it.
    if(IsCoalesced(val) || nbElement > 31)
    {
        return nbElement;
    }
    bool gpgpuPreemptionWANeeded =
        ((GetShaderType() == ShaderType::OPENCL_SHADER) || (GetShaderType() == ShaderType::COMPUTE_SHADER)) &&
        (m_SIMDSize == SIMDMode::SIMD8) &&
        m_Platform->WaSamplerResponseLengthMustBeGreaterThan1() &&
        m_Platform->supportGPGPUMidThreadPreemption();

    if(llvm::GenIntrinsicInst* inst = llvm::dyn_cast<GenIntrinsicInst>(val))
    {
        // try to prune the destination size
        GenISAIntrinsic::ID IID = inst->getIntrinsicID();
        if( IID == GenISAIntrinsic::GenISA_ldstructured ||
            IID == GenISAIntrinsic::GenISA_typedread)
        {
            // prune with write-mask if possible
            uint elemCnt = 0;
            for(auto I = inst->user_begin(), E = inst->user_end(); I!=E; ++I)
            {
                if(llvm::ExtractElementInst* extract = llvm::dyn_cast<llvm::ExtractElementInst>(*I))
                {
                    if(llvm::ConstantInt* index = llvm::dyn_cast<ConstantInt>(extract->getIndexOperand()))
                    {
                        elemCnt++;
                        assert( index->getZExtValue() < 5 );
                        mask |= (1 << index->getZExtValue());
                        continue;
                    }
                }
                // if the vector is accessed by anything else than direct Extract we cannot prune it
                elemCnt = nbElement;
                mask = 0;
                break;
            }

            if(mask)
            {
                nbElement = elemCnt;
            }
        }
        else if (isSampleInstruction(inst) || isLdInstruction(inst) || isInfoInstruction(inst))
        {
            // sampler can return selected channel ony with extra header, when
            // returning only 1~2 channels, it suppose to have better performance.
            uint nbExtract = 0, maxIndex = 0;
            uint8_t maskExtract = 0;
            bool allExtract = true;

            for (auto I = inst->user_begin(), E = inst->user_end(); I != E; ++I)
            {
                ExtractElementInst* extract = llvm::dyn_cast<ExtractElementInst>(*I);
                if (extract != nullptr)
                {
                    llvm::ConstantInt* indexVal;
                    indexVal = llvm::dyn_cast<ConstantInt>(extract->getIndexOperand());
                    if (indexVal != nullptr)
                    {
                        uint index = static_cast<uint>(indexVal->getZExtValue());
                        maxIndex = std::max(maxIndex, index + 1);

                        maskExtract |= (1 << index);
                        nbExtract++;
                    }
                    else
                    {
                        // if extractlement with dynamic index
                        maxIndex = nbElement;
                        allExtract = false;
                        break;
                    }
                }
                else
                {
                    // if the vector is accessed by anything else than direct Extract we cannot prune it
                    maxIndex = nbElement;
                    allExtract = false;
                    break;
                }
            }

            // TODO: there are some issues in EmitVISAPass prevents enabling
            // selected channel return for info intrinsics.
            if (!allExtract ||
                gpgpuPreemptionWANeeded ||
                IGC_IS_FLAG_DISABLED(EnableSamplerChannelReturn) ||
                isInfoInstruction(inst) ||
                maskExtract > 0xf)
            {
                if (gpgpuPreemptionWANeeded)
                {
                    maxIndex = std::max((uint)2, maxIndex);
                }

                mask = BIT(maxIndex) - 1;
                nbElement = maxIndex;
            }
            else
            {
                // based on return channels, decide whether do partial
                // return with addtional header
                static const bool selectReturnChannels[] = {
                    false,      // 0 0000 - should not happen
                    false,      // 1 0001 - r
                    false,      // 2 0010 -  g
                    false,      // 3 0011 - rg
                    true,       // 4 0100 -   b
                    false,      // 5 0101 - r b
                    false,      // 6 0110 -  gb
                    false,      // 7 0111 - rgb
                    true,       // 8 1000 -    a
                    true,       // 9 1001 - r  a
                    true,       // a 1010 -  g a
                    false,      // b 1011 - rg a
                    true,       // c 1100 -   ba
                    false,      // d 1101 - r ba
                    false,      // e 1110 -  gba
                    false       // f 1111 - rgba
                };
                assert(maskExtract != 0 && maskExtract <= 0xf);

                if (selectReturnChannels[maskExtract])
                {
                    mask = maskExtract;
                    nbElement = nbExtract;
                }
                else
                {
                    mask = BIT(maxIndex) - 1;
                    nbElement = maxIndex;
                }
            }
        }
        else
        {
            GenISAIntrinsic::ID IID = inst->getIntrinsicID();
            if (isLdInstruction(inst) ||
                IID == GenISAIntrinsic::GenISA_URBRead ||
                IID == GenISAIntrinsic::GenISA_URBReadOutput ||
                IID == GenISAIntrinsic::GenISA_DCL_ShaderInputVec ||
                IID == GenISAIntrinsic::GenISA_DCL_HSinputVec)
            {
                // prune without write-mask
                uint maxIndex = 0;
                for (auto I = inst->user_begin(), E = inst->user_end(); I != E; ++I)
                {
                    if (llvm::ExtractElementInst* extract = llvm::dyn_cast<llvm::ExtractElementInst>(*I))
                    {
                        if (llvm::ConstantInt* index = llvm::dyn_cast<ConstantInt>(extract->getIndexOperand()))
                        {
                            maxIndex = std::max(maxIndex, static_cast<uint>(index->getZExtValue()) + 1);
                            continue;
                        }
                    }
                    // if the vector is accessed by anything else than direct Extract we cannot prune it
                    maxIndex = nbElement;
                    break;
                }

                mask = BIT(maxIndex) - 1;
                nbElement = maxIndex;
            }
        }
    }
    else if(llvm::BitCastInst* inst = dyn_cast<BitCastInst>(val))
    {
        for(auto I = inst->user_begin(), E = inst->user_end(); I != E; ++I)
        {
            if(llvm::ExtractElementInst* extract = llvm::dyn_cast<llvm::ExtractElementInst>(*I))
            {
                if(llvm::ConstantInt* index = llvm::dyn_cast<ConstantInt>(extract->getIndexOperand()))
                {
                    uint indexBit = BIT(static_cast<uint>(index->getZExtValue()));
                    mask |= indexBit;
                    continue;
                }
            }
            mask = BIT(nbElement) - 1;
            break;
        }
        if(mask)
        {
            nbElement = iSTD::BitCount(mask);
        }
    }
    return nbElement;
}



uint32_t CShader::GetExtractMask(llvm::Value *vecVal)
{
    auto it = extractMasks.find(vecVal);
    if (it != extractMasks.end() )
    {
        return it->second;
    }
    unsigned int numChannels = vecVal->getType()->isVectorTy() ? vecVal->getType()->getVectorNumElements() : 1;
    assert(numChannels <= 32 && "Mask has 32 bits maximally!");
    return (1ULL << numChannels) - 1;
}

uint16_t CShader::AdjustExtractIndex(llvm::Value *vecVal, uint16_t index)
{
    uint16_t result = index;
    if (vecVal->getType()->getVectorNumElements() < 32)
    {
        uint32_t mask = GetExtractMask(vecVal);
        for (uint i = 0; i < index; ++i)
        {
            if ((mask & (1 << i)) == 0)
            {
                result--;
            }
        }
        return result;
    }
    else
    {
        return index;
    }
}

void CShader::GetSimdOffsetBase(CVariable*& pVar)
{
    encoder.SetSimdSize(SIMDMode::SIMD8);
    encoder.SetNoMask();
    encoder.Cast(pVar, ImmToVariable(0x76543210, ISA_TYPE_V));
    encoder.Push();

    if (m_dispatchSize >= SIMDMode::SIMD16)
    {
        encoder.SetSimdSize(SIMDMode::SIMD8);
        encoder.SetDstSubReg(8);
        encoder.SetNoMask();
        encoder.Add(pVar, pVar, ImmToVariable(8, ISA_TYPE_W));
        encoder.Push();
    }

    if (encoder.IsSecondHalf())
    {
        encoder.SetNoMask();
        encoder.Add(pVar, pVar, ImmToVariable(16, ISA_TYPE_W));
        encoder.Push();
    }
}

CVariable* CShader::GetPerLaneOffsetsReg(uint typeSizeInBytes)
{
    CVariable* pPerLaneOffsetsRaw = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_UW, EALIGN_GRF);
    GetSimdOffsetBase(pPerLaneOffsetsRaw);

    // per-lane offsets need to be added to address register
    CVariable* pConst2 = ImmToVariable(typeSizeInBytes, ISA_TYPE_UW);

    CVariable* pPerLaneOffsetsReg = GetNewVariable(numLanes(m_SIMDSize),ISA_TYPE_UW, EALIGN_GRF, false);

    // perLaneOffsets = 4 * perLaneOffsetsRaw
    encoder.SetNoMask();
    encoder.Mul(pPerLaneOffsetsReg, pPerLaneOffsetsRaw, pConst2);
    encoder.Push();

    return pPerLaneOffsetsReg;
}

void
CShader::CreatePayload(uint regCount, uint idxOffset, CVariable*& payload,
                       llvm::Instruction* inst, uint paramOffset,
                       uint8_t hfFactor)
{
    for(uint i = 0; i < regCount ; ++i)
    {
        uint subVarIdx = ((numLanes(m_SIMDSize) / (getGRFSize() >> 2)) >> hfFactor) * i + idxOffset;
        CopyVariable(payload, GetSymbol(inst->getOperand(i + paramOffset)), subVarIdx);
    }
}

unsigned CShader::GetIMEReturnPayloadSize(GenIntrinsicInst *I)
{
    assert(I->getIntrinsicID() == GenISAIntrinsic::GenISA_vmeSendIME2);

    const auto streamMode =
        (COMMON_ISA_VME_STREAM_MODE)(
            cast<ConstantInt>(I->getArgOperand(4))->getZExtValue());
    auto* refImgBTI = I->getArgOperand(2);
    auto* bwdRefImgBTI = I->getArgOperand(3);
    const bool isDualRef = (refImgBTI != bwdRefImgBTI);

    uint32_t regs2rcv = 7;
    if ((streamMode == VME_STREAM_OUT) || (streamMode == VME_STREAM_IN_OUT))
    {
        regs2rcv += 2;
        if (isDualRef)
        {
            regs2rcv += 2;
        }
    }
    return regs2rcv;
}

uint CShader::GetNbElementAndMask(llvm::Value* value, uint32_t& mask)
{
    mask = 0;
    // Special case for VME's GenISA_createMessagePhases intrinsic
    if (GenIntrinsicInst* inst = dyn_cast<GenIntrinsicInst>(value)) {
        GenISAIntrinsic::ID IID = inst->getIntrinsicID();
        switch (IID)
        {
        case GenISAIntrinsic::GenISA_createMessagePhases:
        case GenISAIntrinsic::GenISA_createMessagePhasesNoInit:
        case GenISAIntrinsic::GenISA_createMessagePhasesV:
        case GenISAIntrinsic::GenISA_createMessagePhasesNoInitV:
        {
            Value* numGRFs = inst->getArgOperand(0);
            assert(isa<ConstantInt>(numGRFs) && "Number GRFs operand is expected to be constant int!");
            // Number elements = {num GRFs} * {num DWords in GRF} = {num GRFs} * 8;
            return int_cast<unsigned int>(cast<ConstantInt>(numGRFs)->getZExtValue() * 8);
        }
        default:
            break;
        }
    }
    else if (auto *PN = dyn_cast<PHINode>(value))
    {
        // We could have case like below that payload is undef on some path.
        //
        // BB1:
        //   %147 = call i32 @llvm.genx.GenISA.createMessagePhasesNoInit(i32 11)
        //   call void @llvm.genx.GenISA.vmeSendIME2(i32 % 147, ...)
        //   br label %BB2
        // BB2:
        //   ... = phi i32[%147, %BB1], [0, %BB]
        //
        for (uint i = 0, e = PN->getNumOperands(); i != e; ++i)
        {
            if (GenIntrinsicInst* inst = dyn_cast<GenIntrinsicInst>(PN->getOperand(i)))
            {
                GenISAIntrinsic::ID IID = inst->getIntrinsicID();
                switch (IID)
                {
                case GenISAIntrinsic::GenISA_createMessagePhases:
                case GenISAIntrinsic::GenISA_createMessagePhasesNoInit:
                case GenISAIntrinsic::GenISA_createMessagePhasesV:
                case GenISAIntrinsic::GenISA_createMessagePhasesNoInitV:
                    return GetNbElementAndMask(inst, mask);
                default:
                    break;
                }
            }
        }
    }

    uint nbElement = 0;
    uint bSize = 0;
    llvm::Type* type = value->getType();
    switch(type->getTypeID())
    {
    case llvm::Type::FloatTyID:
    case llvm::Type::HalfTyID:
        nbElement = GetIsUniform(value) ? 1 : numLanes(m_SIMDSize);
        break;
    case llvm::Type::IntegerTyID:
        bSize = llvm::cast<llvm::IntegerType>(type)->getBitWidth();
        nbElement = GetIsUniform(value) ? 1 : numLanes(m_SIMDSize);
        if (bSize==1 && !m_CG->canEmitAsUniformBool(value))
        {
            nbElement = numLanes(m_SIMDSize);
        }
        break;
    case llvm::Type::VectorTyID:
        {
            uint nElem = GetNbVectorElementAndMask(value, mask);
            nbElement = GetIsUniform(value) ? nElem : (nElem * numLanes(m_SIMDSize));
        }
        break;
    case llvm::Type::PointerTyID:
        // Assumes 32-bit pointers
        nbElement = GetIsUniform(value) ? 1 : numLanes(m_SIMDSize);
        break;
    case llvm::Type::DoubleTyID:
        nbElement = GetIsUniform(value) ? 1 : numLanes(m_SIMDSize);
        break;
    default:
        assert(0);
        break;
    }
    return nbElement;
}

CVariable* CShader::GetUndef(VISA_Type type)
{
    CVariable* var = nullptr;
    if(type == ISA_TYPE_BOOL)
    {
        var = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_BOOL, EALIGN_BYTE);
    }
    else
    {
        var = new (Allocator) CVariable(type);
    }
    return var;
}

// TODO: Obviously, lots of works are needed to support constant expression
// better.
uint64_t CShader::GetConstantExpr(ConstantExpr *CE) {
    switch (CE->getOpcode()) {
    default:
        break;
    case Instruction::IntToPtr: {
        Constant *C = CE->getOperand(0);
        if (isa<ConstantInt>(C) || isa<ConstantFP>(C) || isa<ConstantPointerNull>(C))
            return GetImmediateVal(C);
        if (ConstantExpr *CE1 = dyn_cast<ConstantExpr>(C))
            return GetConstantExpr(CE1);
        break;
    }
    case Instruction::PtrToInt: {
        Constant *C = CE->getOperand(0);
        if (ConstantExpr *CE1 = dyn_cast<ConstantExpr>(C))
            return GetConstantExpr(CE1);
        if (GlobalVariable *GV = dyn_cast<GlobalVariable>(C))
            return GetGlobalMappingValue(GV);
        break;
    }
    case Instruction::Trunc: {
        Constant *C = CE->getOperand(0);
        if (ConstantExpr *CE1 = dyn_cast<ConstantExpr>(C)) {
            if (IntegerType *ITy = dyn_cast<IntegerType>(CE1->getType())) {
                return GetConstantExpr(CE1) & ITy->getBitMask();
            }
        }
        break;
    }
    case Instruction::LShr: {
        Constant *C = CE->getOperand(0);
        if (ConstantExpr *CE1 = dyn_cast<ConstantExpr>(C)) {
            if (IntegerType *ITy = dyn_cast<IntegerType>(CE1->getType())) {
                uint64_t ShAmt = GetImmediateVal(CE->getOperand(1));
                return GetConstantExpr(CE1) >> ShAmt;
            }
        }
        break;
    }
    }

    errs() << "CE: " << *CE << '\n';
    assert(false && "Unsupported constant expression!");
    return 0xBADDCAFEbaddcafeU;

}

unsigned int CShader::GetGlobalMappingValue(llvm::Value* c)
{
    assert(0 && "The global variables are not handled");

    return 0;
}

CVariable* CShader::GetGlobalMapping(llvm::Value* c)
{
    assert(0 && "The global variables are not handled");

    VISA_Type type = GetType(c->getType());
    return ImmToVariable(0, type);
}

CVariable* CShader::GetScalarConstant(llvm::Value* c)
{
    VISA_Type type = GetType(c->getType());

    // Constants
    if (isa<ConstantInt>(c) || isa<ConstantFP>(c) || isa<ConstantPointerNull>(c))
    {
        return ImmToVariable(GetImmediateVal(c), type);
    }

    // Undefined values
    if (isa<UndefValue>(c))
    {
        return GetUndef(type);
    }

    // GlobalVariables
    if (isa<GlobalVariable>(c))
    {
        return GetGlobalMapping(c);
    }

    // Constant Expression
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(c))
        return ImmToVariable(GetConstantExpr(CE), type);

    assert(false && "Unhandled flavor of constant!");
    return 0;
}

// Return true if can be encoded as mini float and return the encoding in value
static bool getByteFloatEncoding(ConstantFP *fp, uint8_t& value)
{
    value = 0;
    if(fp->getType()->isFloatTy())
    {
        if(fp->isZero())
        {
            value = fp->isNegative() ? 0x80 : 0;
            return true;
        }
        APInt api = fp->getValueAPF().bitcastToAPInt();
        FLOAT32 bitFloat;
        bitFloat.value.u = int_cast<unsigned int>(api.getZExtValue());
        // check that fraction doesn't have any bots set below bit 23 - 4
        // Byte float can only encode the higer 4 bits of the fraction
        if((bitFloat.fraction & (~(0xF << (23 - 4)))) == 0 &&
            ((bitFloat.exponent > 124 && bitFloat.exponent <= 131) ||
            (bitFloat.exponent == 124 && bitFloat.fraction != 0)))
        {
            // convert to float 8bits format
            value |= bitFloat.sign << 7;
            value |= (bitFloat.fraction >> (23 - 4));
            value |= (bitFloat.exponent & 0x3) << 4;
            value |= (bitFloat.exponent & BIT(7)) >> 1;
            return true;
        }
    }
    return false;
}

// Return the most commonly used constant. Return null if all constant are different.
llvm::Constant* CShader::findCommonConstant(llvm::Constant *C, uint elts, uint currentEmitElts, bool &allSame)
{
    if (elts == 1)
    {
        return nullptr;
    }

    llvm::MapVector<llvm::Constant*, int> constMap;
    constMap.clear();
    Constant* constC = nullptr;
    bool cannotPackVF = false;
    for (uint32_t i = currentEmitElts; i < currentEmitElts + elts; i++)
    {
        constC = C->getAggregateElement(i);
        if (!constC)
        {
            return nullptr;
        }
        constMap[constC]++;

        // check if the constant can be packed in vf.
        if (!isa<UndefValue>(constC) && elts >= 4)
        {
            llvm::VectorType *VTy = llvm::dyn_cast<llvm::VectorType>(C->getType());
            if (VTy->getScalarType()->isFloatTy())
            {
                uint8_t encoding = 0;
                cannotPackVF = !getByteFloatEncoding(cast<ConstantFP>(constC), encoding);
            }
        }
    }
    int mostUsedCount = 1;
    Constant* mostUsedValue = nullptr;
    for (auto iter = constMap.begin(); iter != constMap.end(); iter++)
    {
        if (iter->second > mostUsedCount)
        {
            mostUsedValue = iter->first;
            mostUsedCount = iter->second;
        }
    }

    constMap.clear();
    allSame = (mostUsedCount == elts);

    if (allSame)
    {
        return mostUsedValue;
    }
    else if (mostUsedCount > 1 && cannotPackVF)
    {
        return mostUsedValue;
    }
    else
    {
        return nullptr;
    }
}

auto sizeToSIMDMode = [](uint32_t size)
{
    switch (size)
    {
    case 1:
        return SIMDMode::SIMD1;
    case 2:
        return SIMDMode::SIMD2;
    case 4:
        return SIMDMode::SIMD4;
    case 8:
        return SIMDMode::SIMD8;
    case 16:
        return SIMDMode::SIMD16;
    default:
        assert(false && "unexpected simd size");
        return SIMDMode::SIMD1;
    }
};

CVariable* CShader::GetConstant(llvm::Constant* C, CVariable* dstVar)
{
    llvm::VectorType *VTy = llvm::dyn_cast<llvm::VectorType>(C->getType());
    if (C && VTy)
    {   // Vector constant
        llvm::Type *eTy = VTy->getElementType();
        assert((VTy->getNumElements() < (UINT16_MAX)) && "getNumElements more than 64k elements");
        uint16_t elts = (uint16_t)VTy->getNumElements();

        if (elts == 1)
        {
            llvm::Constant *EC = C->getAggregateElement((uint)0);
            assert(EC && "Vector Constant has no valid constant element!");
            return GetScalarConstant(EC);
        }

        // Emit a scalar move to load the element of index k.
        auto copyScalar = [=](int k, CVariable* Var)
        {
            Constant *EC = C->getAggregateElement(k);
            assert(EC && "Constant Vector: Invalid non-constant element!");
            if (isa<UndefValue>(EC))
                return;

            CVariable *eVal = GetScalarConstant(EC);
            if (Var->IsUniform())
            {
                GetEncoder().SetDstSubReg(k);
            }
            else
            {
                auto input_size = eTy->getScalarSizeInBits() / 8;
                Var = GetNewAlias(Var, Var->GetType(), k * input_size * numLanes(m_SIMDSize),0);
            }
            GetEncoder().Copy(Var, eVal);
            GetEncoder().Push();
        };

        // Emit a simd4 move to load 4 byte float.
        auto copyV4 = [=](int k, uint32_t vfimm, CVariable* Var)
        {
            CVariable *Imm = ImmToVariable(vfimm, ISA_TYPE_VF);
            GetEncoder().SetUniformSIMDSize(SIMDMode::SIMD4);
            GetEncoder().SetDstSubReg(k);
            GetEncoder().Copy(Var, Imm);
            GetEncoder().Push();
        };


        if (dstVar != nullptr && !(dstVar->IsUniform()))
        {
            for (uint i = 0; i < elts; i++)
            {
                copyScalar(i, dstVar);
            }
            return dstVar;
        }

        CVariable *CVar = (dstVar == nullptr) ? GetNewVariable(elts, GetType(eTy), EALIGN_GRF, true) : dstVar;
        uint remainElts = elts;
        uint currentEltsOffset = 0;
        uint size = 8;
        while (remainElts != 0)
        {
            bool allSame = 0;

            while (size > remainElts && size != 1)
            {
                size /= 2;
            }

            Constant* commonConstant = findCommonConstant(C, size, currentEltsOffset, allSame);
            // case 2: all constants the same
            if (commonConstant && allSame)
            {
                GetEncoder().SetUniformSIMDSize(sizeToSIMDMode(size));
                GetEncoder().SetDstSubReg(currentEltsOffset);
                GetEncoder().Copy(CVar, GetScalarConstant(commonConstant));
                GetEncoder().Push();
            }

            // case 3: some constants the same
            else if (commonConstant)
            {
                GetEncoder().SetUniformSIMDSize(sizeToSIMDMode(size));
                GetEncoder().SetDstSubReg(currentEltsOffset);
                GetEncoder().Copy(CVar, GetScalarConstant(commonConstant));
                GetEncoder().Push();

                Constant* constC = nullptr;
                for (uint i = currentEltsOffset; i < currentEltsOffset + size; i++)
                {
                    constC = C->getAggregateElement(i);
                    if (constC != commonConstant && !isa<UndefValue>(constC))
                    {
                        GetEncoder().SetDstSubReg(i);
                        GetEncoder().Copy(CVar, GetScalarConstant(constC));
                        GetEncoder().Push();
                    }
                }
            }
            // case 4: VFPack
            else if (VTy->getScalarType()->isFloatTy() && size >= 4)
            {
                unsigned Step = 4;
                for (uint i = currentEltsOffset; i < currentEltsOffset + size; i += Step)
                {
                    // pack into vf if possible.
                    uint32_t vfimm = 0;
                    bool canUseVF = true;
                    for (unsigned j = 0; j < Step; ++j)
                    {
                        Constant *EC = C->getAggregateElement(i + j);
                        // Treat undef as 0.0f.
                        if (isa<UndefValue>(EC))
                            continue;
                        uint8_t encoding = 0;
                        canUseVF = getByteFloatEncoding(cast<ConstantFP>(EC), encoding);
                        if (canUseVF)
                        {
                            uint32_t v = encoding;
                            v <<= j * 8;
                            vfimm |= v;
                        }
                        else
                        {
                            break;
                        }
                    }

                    if (canUseVF)
                    {
                        copyV4(i, vfimm, CVar);
                    }
                    else
                    {
                        for (unsigned j = i; j < i + Step; ++j)
                            copyScalar(j, CVar);
                    }
                }
            }
            // case 5: single copy
            else
            {
                // Element-wise copy or trailing elements copy if partially packed.
                for (uint i = currentEltsOffset; i < currentEltsOffset + size; i++)
                {
                    copyScalar(i, CVar);
                }
            }
            remainElts -= size;
            currentEltsOffset += size;
        }
        return CVar;
    }

    return GetScalarConstant(C);
}

VISA_Type IGC::GetType(llvm::Type* type, CodeGenContext* pContext)
{
    switch(type->getTypeID())
    {
    case llvm::Type::FloatTyID:
        return ISA_TYPE_F;
    case llvm::Type::IntegerTyID:
        switch(type->getIntegerBitWidth())
        {
        case 1:
            return ISA_TYPE_BOOL;
        case 8:
            return ISA_TYPE_B;
        case 16:
            return ISA_TYPE_W;
        case 32:
            return ISA_TYPE_D;
        case 64:
            return ISA_TYPE_Q;
        default:
            assert(0 && "illegal type");
            break;
        }
        break;
    case llvm::Type::VectorTyID:
        return GetType(type->getContainedType(0), pContext);
    case llvm::Type::PointerTyID:
        {
            unsigned int AS = type->getPointerAddressSpace();
            uint numBits = pContext->getRegisterPointerSizeInBits(AS);
            if (numBits == 32)
            {
                return ISA_TYPE_UD;
            }
            else
            {
                return ISA_TYPE_UQ;
            }
        }
    case llvm::Type::DoubleTyID:
        return ISA_TYPE_DF;
    case llvm::Type::HalfTyID:
        return ISA_TYPE_HF;
    default:
        break;
    }
    assert(0);
    return ISA_TYPE_F;
}

VISA_Type CShader::GetType(llvm::Type* type)
{
    return IGC::GetType(type, GetContext());
}

uint64_t IGC::GetImmediateVal(llvm::Value* Const)
{
    // Constant integer
    if (llvm::ConstantInt *CInt = llvm::dyn_cast<llvm::ConstantInt>(Const))
    {
        return CInt->getZExtValue();
    }

    // Constant float/double
    if (llvm::ConstantFP *CFP = llvm::dyn_cast<llvm::ConstantFP>(Const))
    {
        APInt api = CFP->getValueAPF().bitcastToAPInt();
        return api.getZExtValue();
    }

    // Null pointer
    if (llvm::isa<ConstantPointerNull>(Const))
    {
        return 0;
    }

    assert(false && "Unhandled constant value!");
    return 0;
}

/// IsRawAtomicIntrinsic - Check wether it's RAW atomic, which is optimized
/// potentially by scalarized atomic operation.
static bool IsRawAtomicIntrinsic(llvm::Value *V) {
    GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(V);
    if (!GII)
        return false;

    switch (GII->getIntrinsicID()) {
    default:
        break;
    case GenISAIntrinsic::GenISA_intatomicraw:
    case GenISAIntrinsic::GenISA_floatatomicraw:
    case GenISAIntrinsic::GenISA_intatomicrawA64:
    case GenISAIntrinsic::GenISA_floatatomicrawA64:
    case GenISAIntrinsic::GenISA_icmpxchgatomicraw:
    case GenISAIntrinsic::GenISA_fcmpxchgatomicraw:
    case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
    case GenISAIntrinsic::GenISA_fcmpxchgatomicrawA64:
        return true;
    }

    return false;
}

/// GetPreferredAlignmentOnUse - Return preferred alignment based on how the
/// specified value is being used.
static e_alignment GetPreferredAlignmentOnUse(llvm::Value *V, WIAnalysis *WIA,
                                              CodeGenContext *pContext)
{
    // If uniform variables are once used by uniform loads, stores, or atomic
    // ops, they need being GRF aligned.
    for (auto UI = V->user_begin(), UE = V->user_end(); UI != UE; ++UI) {
        if (LoadInst *ST = dyn_cast<LoadInst>(*UI)) {
            Value *Ptr = ST->getPointerOperand();
            if (WIA->whichDepend(Ptr) == WIAnalysis::UNIFORM) {
                if (IGC::isA64Ptr(cast<PointerType>(Ptr->getType()), pContext))
                    return EALIGN_2GRF;
                return EALIGN_GRF;
            }
        }
        if (StoreInst *ST = dyn_cast<StoreInst>(*UI)) {
            Value *Ptr = ST->getPointerOperand();
            if (WIA->whichDepend(Ptr) == WIAnalysis::UNIFORM) {
                if(IGC::isA64Ptr(cast<PointerType>(Ptr->getType()), pContext))
                    return EALIGN_2GRF;
                return EALIGN_GRF;
            }
        }

        // Last, check Gen intrinsic.
        GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(*UI);
        if (!GII) {
            continue;
        }

        if (IsRawAtomicIntrinsic(GII)) {
            Value *Ptr = GII->getArgOperand(1);
            if (WIA->whichDepend(Ptr) == WIAnalysis::UNIFORM) {
                if (PointerType *PtrTy = dyn_cast<PointerType>(Ptr->getType())) {
                    if (IGC::isA64Ptr(PtrTy, pContext))
                        return EALIGN_2GRF;
                }
                return EALIGN_GRF;
            }
        }
    }

    // Otherwise, naturally aligned is always assumed.
    return EALIGN_AUTO;
}

/// GetPreferredAlignment - Return preferred alignment based on how the
/// specified value is being defined/used.
e_alignment IGC::GetPreferredAlignment(llvm::Value *V, WIAnalysis *WIA,
                                       CodeGenContext* pContext)
{
    // So far, non-uniform variables are always naturally aligned.
    if (WIA->whichDepend(V) != WIAnalysis::UNIFORM)
        return EALIGN_AUTO;

    // As the layout of argument is fixed, only naturally aligned could be
    // assumed.
    if (isa<Argument>(V))
        return CEncoder::GetCISADataTypeAlignment(GetType(V->getType(), pContext));

    // For values not being mapped to variables directly, always assume
    // natually aligned.
    if (!isa<Instruction>(V))
        return EALIGN_AUTO;

    // If uniform variables are results from uniform loads, they need being GRF
    // aligned.
    if (LoadInst *LD = dyn_cast<LoadInst>(V)) {
        Value *Ptr = LD->getPointerOperand();
        // For 64-bit load, we have to check how the loaded value being used.
        e_alignment Align = EALIGN_GRF;
        if (IGC::isA64Ptr(cast<PointerType>(Ptr->getType()), pContext))
            Align = GetPreferredAlignmentOnUse(V, WIA, pContext);
        return (Align == EALIGN_AUTO) ? EALIGN_GRF : Align;
    }

    // If uniform variables are results from uniform atomic ops, they need
    // being GRF aligned.
    if (IsRawAtomicIntrinsic(V)) {
        GenIntrinsicInst *GII = cast<GenIntrinsicInst>(V);
        Value *Ptr = GII->getArgOperand(1);
        // For 64-bit atomic ops, we have to check how the return value being
        // used.
        e_alignment Align = EALIGN_GRF;
        if (PointerType *PtrTy = dyn_cast<PointerType>(Ptr->getType())) {
            if(IGC::isA64Ptr(PtrTy, pContext))
                Align = GetPreferredAlignmentOnUse(V, WIA, pContext);
        }
        return (Align == EALIGN_AUTO) ? EALIGN_GRF : Align;
    }

    // Check how that value is used.
    return GetPreferredAlignmentOnUse(V, WIA, pContext);
}

CVariable* CShader::LazyCreateCCTupleBackingVariable(
    CoalescingEngine::CCTuple* ccTuple,
    VISA_Type baseVisaType)
{
    CVariable* var = NULL;
    auto it = ccTupleMapping.find(ccTuple);
    if (it != ccTupleMapping.end()){
        var = ccTupleMapping[ccTuple];
    }
    else{
        auto mult = (m_SIMDSize == SIMDMode::SIMD8) ? 1 : 2;
        mult = (baseVisaType == ISA_TYPE_HF) ? 1 : mult;
        unsigned int numRows = ccTuple->GetNumElements() * mult;
        unsigned int numElts = numRows * GENX_GRF_REG_SIZ / CEncoder::GetCISADataTypeSize(ISA_TYPE_F);

        //int size = numLanes(m_SIMDSize) * ccTuple->GetNumElements();
        if( ccTuple->HasNonHomogeneousElements() )
        {
            numElts += m_coalescingEngine->GetLeftReservedOffset(ccTuple->GetRoot(), m_SIMDSize) / CEncoder::GetCISADataTypeSize(ISA_TYPE_F);
            numElts += m_coalescingEngine->GetRightReservedOffset(ccTuple->GetRoot(), m_SIMDSize) / CEncoder::GetCISADataTypeSize(ISA_TYPE_F);
        }

        assert((numElts < (UINT16_MAX)) && "tuple byte size higher than 64k");

        //create one
        var = GetNewVariable(
            (uint16_t)numElts,
            ISA_TYPE_F,
            EALIGN_GRF,
            false,
            m_numberInstance);
        ccTupleMapping.insert(std::pair<CoalescingEngine::CCTuple*, CVariable*>(ccTuple, var));
    }

    return var;
}

/// F should be a non-kernel function.
///
/// For a subroutine call, symbols (CVariables) are created as follows:
///
/// (1) If subroutine returns non-void value, then a unified return CVarable
/// is created to communicate between callee and caller. Function
/// 'getOrCreateReturnSymbol' creates such a unique symbol (CVariable)
/// on-demand. This return symbol is cached inside 'globalSymbolMapping'
/// object and it is *NOT* part of local symbol table 'symbolMapping'.
/// Currently return symbols are non-uniform.
///
/// (2) Subroutine formal arguments are also created on-demand, which may be
/// created from their first call sites or ahead of any call site. Symbols for
/// subroutine formal arguments are also stored inside 'globalSymbolMapping'
/// during entire module codegen. During each subroutine vISA emission,
/// value-to-symbol mapping are also copied into 'symbolMapping' to allow
/// EmitVISAPass to emit code in a uniform way.
///
/// In some sense, all formal arguments are pre-allocated. Those symbols must be
/// non-alias cvariable (ie root cvariable) as required by visa.
///
/// Currently, all explicit arguments are non-uniform and most implicit
/// arguments are uniform. Some implicit arguments may share the same symbol
/// with their caller's implicit argument of the same kind. This is a subroutine
/// optimization implemented in 'getOrCreateArgumentSymbol'.
///
void CShader::BeginFunction(llvm::Function *F)
{
    // TODO: merge InitEncoder with this function.

    // Dont clear symolMapping when debug info is enabled
    // since this map is used to query location information
    // after VISA compilation.
    if (!DebugInfoData::hasDebugInfo(this))
        symbolMapping.clear();
    ccTupleMapping.clear();
    ConstantPool.clear();

    bool useStackCall = m_FGA && m_FGA->useStackCall(F);
    if (useStackCall)
    {
        m_R0 = nullptr;
        globalSymbolMapping.clear();
        encoder.BeginStackFunction(F);
    }
    else
    {
        encoder.BeginSubroutine(F);
    }
    // Set already created symbols for formal arguments.
    for (auto &Arg : F->args())
    {
        if (!Arg.use_empty())
        {
            // the treatment of argument is more complex for subroutine and simpler for stack-call function
            CVariable *Var = getOrCreateArgumentSymbol(&Arg, false, useStackCall);
            symbolMapping[&Arg] = Var;

            if (Value *Node = m_deSSA->getRootValue(&Arg))
            {
                if (Node != (Value*)&Arg &&
                    symbolMapping.count(Node) == 0)
                {
                    CVariable* aV = Var;
                    if (IGC_GET_FLAG_VALUE(EnableDeSSAAlias) >= 2)
                    {
                        aV = createAliasIfNeeded(Node, Var);
                    }
                    symbolMapping[Node] = aV;
                }
            }
        }
    }

    CreateAliasVars();
    PreCompileFunction(*F);
}

/// This method is used to create the vISA variable for function F's formal return value
CVariable *CShader::getOrCreateReturnSymbol(llvm::Function *F)
{
    assert(F && "null function");
    auto it = globalSymbolMapping.find(F);
    if (it != globalSymbolMapping.end())
    {
        return it->second;
    }

    auto retType = F->getReturnType();
    if (F->isDeclaration() || retType->isVoidTy())
        return nullptr;

    assert(retType->isSingleValueType());
    VISA_Type type = GetType(retType);
    uint16_t nElts = numLanes(m_SIMDSize);
    if (retType->isVectorTy())
    {
        nElts *= (uint16_t)retType->getVectorNumElements();
    }
    e_alignment align = getGRFAlignment();
    CVariable *var = GetNewVariable(nElts, type, align, /*uniform*/false, m_numberInstance);
    globalSymbolMapping.insert(std::make_pair(F, var));
    return var;
}

/// This method is used to create the vISA variable for function F's formal argument
CVariable* CShader::getOrCreateArgumentSymbol(
    Argument *Arg,
    bool ArgInCallee,
    bool useStackCall)
{
    llvm::DenseMap<llvm::Value*, CVariable*> *pSymMap = &globalSymbolMapping;
    auto it = pSymMap->find(Arg);
    if (it != pSymMap->end())
    {
        return it->second;
    }

    // An explicit argument is not uniform, and for an implicit argument, it
    // is predefined. Note that it is not necessarily uniform.
    Function *F = Arg->getParent();
    ImplicitArgs implicitArgs(*F, m_pMdUtils);
    unsigned numImplicitArgs = implicitArgs.size();
    unsigned numPushArgsEntry = m_ModuleMetadata->pushInfo.pushAnalysisWIInfos.size();
    unsigned numPushArgs = (isEntryFunc(m_pMdUtils, F) ? numPushArgsEntry : 0);
    unsigned numFuncArgs = IGCLLVM::GetFuncArgSize(F) - numImplicitArgs - numPushArgs;

    CVariable* var = nullptr;
    llvm::Function::arg_iterator arg = F->arg_begin();
    std::advance(arg, numFuncArgs);
    for (unsigned i = 0; i < numImplicitArgs; ++i, ++arg)
    {
        Argument *argVal = &(*arg);
        if (argVal == Arg)
        {
            ImplicitArg implictArg = implicitArgs[i];
            auto ArgType = implictArg.getArgType();

            // Just reuse the kernel arguments for the following.
            // Note that for read only general arguments, we may do similar
            // optimization, with some advanced analysis.
            if (!useStackCall &&
                 ( ArgType == ImplicitArg::ArgType::R0 ||
                   ArgType == ImplicitArg::ArgType::PAYLOAD_HEADER ||
                   ArgType == ImplicitArg::ArgType::WORK_DIM ||
                   ArgType == ImplicitArg::ArgType::NUM_GROUPS ||
                   ArgType == ImplicitArg::ArgType::GLOBAL_SIZE ||
                   ArgType == ImplicitArg::ArgType::LOCAL_SIZE ||
                   ArgType == ImplicitArg::ArgType::ENQUEUED_LOCAL_WORK_SIZE ||
                   ArgType == ImplicitArg::ArgType::CONSTANT_BASE ||
                   ArgType == ImplicitArg::ArgType::GLOBAL_BASE ||
                   ArgType == ImplicitArg::ArgType::PRIVATE_BASE ||
                   ArgType == ImplicitArg::ArgType::PRINTF_BUFFER ) )
            {
                Function &K = *m_FGA->getSubGroupMap(F);
                ImplicitArgs IAs(K, m_pMdUtils);
                uint32_t nIAs = (uint32_t)IAs.size();
                uint32_t iArgIx = IAs.getArgIndex(ArgType);
                uint32_t argIx = (uint32_t)IGCLLVM::GetFuncArgSize(K) - nIAs + iArgIx;
                if (isEntryFunc(m_pMdUtils, &K)) {
                    argIx = argIx - numPushArgsEntry;
                }
                Function::arg_iterator arg = K.arg_begin();
                for (uint32_t j = 0; j < argIx; ++j, ++arg);
                Argument* kerArg = &(*arg);

                // Pre-condition: all kernel arguments have been created already.
                assert(pSymMap->count(kerArg));
                return (*pSymMap)[kerArg];
            }
            else
            {
                bool isUniform = implictArg.getDependency() == WIAnalysis::UNIFORM;
                uint16_t nbElements = (uint16_t)implictArg.getNumberElements();


                var = GetNewVariable(nbElements,
                    implictArg.getVISAType(*m_DL),
                    implictArg.getAlignType(*m_DL), isUniform,
                    isUniform ? 1 : m_numberInstance);
            }
            break;
        }
    }

    // This is not implicit.
    if (var == nullptr)
    {
        // GetPreferredAlignment treats all arguments as kernel ones, which have
        // predefined alignments; but this is not true for subroutines.
        // Conservatively use GRF aligned.
        e_alignment align = getGRFAlignment();

        bool isUniform = false;
        if (!ArgInCallee) {
            // Arg is for the current function and m_WI is available
            isUniform = (m_WI->whichDepend(&*Arg) == WIAnalysis::UNIFORM);
        }

        VISA_Type type = GetType(Arg->getType());
        uint16_t nElts = numLanes(m_SIMDSize);
        if (Arg->getType()->isVectorTy())
        {
            assert(Arg->getType()->getVectorElementType()->isIntegerTy() ||
                Arg->getType()->getVectorElementType()->isFloatingPointTy());
            nElts *= (uint16_t)Arg->getType()->getVectorNumElements();
        }
        var = GetNewVariable(nElts, type, align, isUniform, m_numberInstance);
    }
    pSymMap->insert(std::make_pair(Arg, var));
    return var;
}

CVariable* CShader::getOrCreateArgSymbolForIndirectCall(llvm::CallInst* cInst, unsigned argIdx)
{
    assert(argIdx < cInst->getNumArgOperands());

    CVariable* var = nullptr;
    Value* Arg = cInst->getArgOperand(argIdx);

    llvm::DenseMap<llvm::Value*, CVariable*> *pSymMap = &globalSymbolMapping;
    auto it = pSymMap->find(Arg);
    if (it != pSymMap->end())
    {
        return it->second;
    }

    // Last 3 operands for an indirect call are always R0, PayloadHeader, and PrivateBase
    unsigned numImplicitArgs = 3;
    unsigned numExplicitArgs = cInst->getNumArgOperands() - numImplicitArgs;
    if (argIdx < numExplicitArgs)
    {
        // GetPreferredAlignment treats all arguments as kernel ones, which have
        // predefined alignments; but this is not true for subroutines.
        // Conservatively use GRF aligned.
        e_alignment align = getGRFAlignment();
        VISA_Type type = GetType(Arg->getType());
        uint16_t nElts = numLanes(m_SIMDSize);
        if (Arg->getType()->isVectorTy())
        {
            assert(Arg->getType()->getVectorElementType()->isIntegerTy() ||
                Arg->getType()->getVectorElementType()->isFloatingPointTy());
            nElts *= (uint16_t)Arg->getType()->getVectorNumElements();
        }
        var = GetNewVariable(nElts, type, align, /*isUniform*/ false, m_numberInstance);
    }
    else
    {
        // Can be mapped to the parent's implicit arg
        Function* parentFunc = cInst->getParent()->getParent();
        ImplicitArgs implicitArgs(*parentFunc, m_pMdUtils);
        for (unsigned i = 0; i < implicitArgs.size(); i++)
        {
            ImplicitArg implictArg = implicitArgs[i];
            auto argType = implictArg.getArgType();
            if (argType == ImplicitArg::ArgType::R0 ||
                argType == ImplicitArg::ArgType::PAYLOAD_HEADER ||
                argType == ImplicitArg::ArgType::PRIVATE_BASE)
            {
                Argument* implicitArgInFunc = implicitArgs.getImplicitArg(*parentFunc, argType);
                if (Arg == implicitArgInFunc)
                {
                    bool isUniform = implictArg.getDependency() == WIAnalysis::UNIFORM;
                    var = GetNewVariable((uint16_t)implictArg.getNumberElements(),
                        implictArg.getVISAType(*m_DL),
                        implictArg.getAlignType(*m_DL), isUniform,
                        isUniform ? 1 : m_numberInstance);

                    break;
                }
            }
        }
    }
    assert(var && "Argument not matched!");
    pSymMap->insert(std::make_pair(Arg, var));
    return var;
}

// Reuse a varable in the following case
// %x = op1...
// %y = op2 (%x, ...)
// with some constraints:
// - %x and %y belong to the same block
// - %x and %y do not live out of this block
// - %x does not interfere with %y
// - %x is not phi
// - %y has no phi use
// - %x and %y have the same uniformity, and the same size
// - %x is not an alias
// - alignment is OK
//
CVariable *CShader::reuseSourceVar(Instruction *UseInst, Instruction *DefInst,
                                   e_alignment preferredAlign)
{
    // Only when DefInst has been assigned a CVar.
    assert(DefInst && UseInst);
    auto It = symbolMapping.find(DefInst);
    if (It == symbolMapping.end())
        return nullptr;

    // If the def is an alias/immediate, then do not reuse.
    // TODO: allow alias.
    CVariable *DefVar = It->second;
    if (DefVar->GetAlias() || DefVar->IsImmediate())
        return nullptr;

    // LLVM IR level checks and RPE based heuristics.
    if (!m_VRA->checkDefInst(DefInst, UseInst, m_deSSA->getLiveVars()))
       return nullptr;

    // Do not reuse when variable size exceeds the threshold.
    //
    // TODO: If vISA global RA can better deal with fragmentation, this will
    // become unnecessary.
    //
    // TODO: Remove this check if register pressure is low, or very high.
    //
    unsigned Threshold = IGC_GET_FLAG_VALUE(VariableReuseByteSize);
    if (DefVar->GetSize() > Threshold)
        return nullptr;

    // Only reuse when they have the same uniformness.
    if (GetIsUniform(UseInst) != GetIsUniform(DefInst))
        return nullptr;

    // Check alignments. If UseInst has a stricter alignment then do not reuse.
    e_alignment DefAlign = DefVar->GetAlign();
    e_alignment UseAlign = preferredAlign;
    if (DefAlign == EALIGN_AUTO)
    {
        VISA_Type Ty = GetType(DefInst->getType());
        DefAlign = CEncoder::GetCISADataTypeAlignment(Ty);
    }
    if (UseAlign == EALIGN_AUTO)
    {
        VISA_Type Ty = GetType(UseInst->getType());
        UseAlign = CEncoder::GetCISADataTypeAlignment(Ty);
    }
    if (UseAlign > DefAlign)
        return nullptr;

    // Reuse this source when types match.
    if (DefInst->getType() == UseInst->getType())
    {
        return DefVar;
    }

    // Check cast instructions and create an alias if necessary.
    if (CastInst *CI = dyn_cast<CastInst>(UseInst))
    {
        VISA_Type UseTy = GetType(UseInst->getType());
        if (UseTy == DefVar->GetType())
        {
            return DefVar;
        }

        if (encoder.GetCISADataTypeSize(UseTy) != encoder.GetCISADataTypeSize(DefVar->GetType()))
        {
            // trunc/zext is needed, reuse not possible
            // this extra check is needed because in code gen we implicitly convert all private pointers
            // to 32-bit when LLVM assumes it's 64-bit based on DL
            return nullptr;
        }

        // TODO: allow %y = trunc i32 %x to i8
        assert(CI->isNoopCast(*m_DL));
        return GetNewAlias(DefVar, UseTy, 0, 0);
    }

    // No reuse yet.
    return nullptr;;
}

CVariable *CShader::GetSymbolFromSource(Instruction *UseInst,
                                        e_alignment preferredAlign)
{
    if (UseInst->isBinaryOp() || isa<SelectInst>(UseInst))
    {
        if (!m_VRA->checkUseInst(UseInst, m_deSSA->getLiveVars()))
            return nullptr;

        for (unsigned i = 0; i < UseInst->getNumOperands(); ++i)
        {
            Value *Opnd = UseInst->getOperand(i);
            auto DefInst = dyn_cast<Instruction>(Opnd);
            // Only for non-uniform binary instructions.
            if (!DefInst || GetIsUniform(DefInst))
                continue;

            if (IsCoalesced(DefInst))
            {
                continue;
            }

            CVariable *Var = reuseSourceVar(UseInst, DefInst, preferredAlign);
            if (Var)
                return Var;
        }
        return nullptr;
    }
    else if (auto CI = dyn_cast<CastInst>(UseInst))
    {
        if (!m_VRA->checkUseInst(UseInst, m_deSSA->getLiveVars()))
            return nullptr;

        Value *Opnd = UseInst->getOperand(0);
        auto DefInst = dyn_cast<Instruction>(Opnd);
        if (!DefInst)
            return nullptr;

        if (!IsCoalesced(DefInst))
        {
            return nullptr;
        }

        // TODO: allow %y = trunc i32 %x to i16
        if (!CI->isNoopCast(*m_DL))
            return nullptr;

        // WA: vISA does not optimize the following reuse well yet.
        // %398 = bitcast i16 %vCastload to <2 x i8>
        // produces
        // mov (16) r7.0<1>:w r18.0<2;1,0>:w
        // mov (16) r7.0<1>:b r7.0<2;1,0>:b
        // mov (16) r20.0<1>:f r7.0<8;8,1>:ub
        // not
        // mov (16) r7.0<1>:w r18.0<2;1,0>:w
        // mov (16) r20.0<1>:f r7.0<2;1,0>:ub
        //
        if (CI->getOpcode() == Instruction::BitCast)
        {
            if (CI->getSrcTy()->getScalarSizeInBits() !=
                CI->getDestTy()->getScalarSizeInBits())
                return nullptr;
        }

        return reuseSourceVar(UseInst, DefInst, preferredAlign);
    }

    // TODO, allow insert element/value, gep, intrinsic calls etc..
    //
    // No source for reuse.
    return nullptr;
}

unsigned int CShader::EvaluateSIMDConstExpr(Value* C)
{
    if(BinaryOperator* op = dyn_cast<BinaryOperator>(C))
    {
        switch(op->getOpcode())
        {
        case Instruction::Add:
            return EvaluateSIMDConstExpr(op->getOperand(0)) + EvaluateSIMDConstExpr(op->getOperand(1));
        case Instruction::Mul:
            return EvaluateSIMDConstExpr(op->getOperand(0)) * EvaluateSIMDConstExpr(op->getOperand(1));
        case Instruction::Shl:
            return EvaluateSIMDConstExpr(op->getOperand(0)) << EvaluateSIMDConstExpr(op->getOperand(1));
        default:
            break;
        }
    }
    if(llvm::GenIntrinsicInst *genInst = dyn_cast<GenIntrinsicInst>(C))
    {
        if(genInst->getIntrinsicID() == GenISAIntrinsic::GenISA_simdSize)
        {
            return numLanes(m_dispatchSize);

        }
    }
    if(ConstantInt* constValue = dyn_cast<ConstantInt>(C))
    {
        return (unsigned int)constValue->getZExtValue();
    }
    assert(0 &&"unknow SIMD constant expression");
    return 0;
}

CVariable* CShader::GetSymbol(llvm::Value *value, bool fromConstantPool)
{
    CVariable* var = nullptr;

    if (Constant *C = llvm::dyn_cast<llvm::Constant>(value))
    {
        if (isa<GlobalValue>(value))
        {
            // Function Pointer
            bool isFunction = IGC_IS_FLAG_ENABLED(EnableFunctionPointer) &&
                value->getType()->isPointerTy() &&
                value->getType()->getPointerElementType()->isFunctionTy();
            // Global Relocation
            bool isGlobalVar = IGC_IS_FLAG_ENABLED(EnableGlobalRelocation) &&
                isa<GlobalVariable>(value);

            if (isFunction || isGlobalVar)
            {
                auto it = symbolMapping.find(value);
                if (it != symbolMapping.end())
                {
                    return it->second;
                }
                var = GetNewVariable(1, ISA_TYPE_UQ, EALIGN_QWORD, true, 1);
                symbolMapping.insert(std::pair<llvm::Value*, CVariable*>(value, var));
                return var;
            }
        }

        if (fromConstantPool) {
            CVariable *cvar = ConstantPool.lookup(C);
            if (cvar)
                return cvar;
            // Generate constant initialization.
            SEncoderState S = encoder.CopyEncoderState();
            encoder.Push();
            cvar = GetConstant(C);
            if (!C->getType()->isVectorTy()) {
                CVariable *dst = GetNewVector(C);
                encoder.Copy(dst, cvar);
                encoder.Push();
                cvar = dst;
            }
            encoder.SetEncoderState(S);
            addConstantInPool(C, cvar);
            return cvar;
        }
        var = GetConstant(C);
        return var;
    }

    else if(Instruction *inst = dyn_cast<Instruction>(value))
    {
        if(m_CG->SIMDConstExpr(inst))
        {
            return ImmToVariable(EvaluateSIMDConstExpr(inst), ISA_TYPE_D);
        }
    }

    auto it = symbolMapping.find(value);

    // mapping exists, return
    if( it != symbolMapping.end() )
    {
        return it->second;
    }

    if (IGC_IS_FLAG_ENABLED(EnableDeSSAAlias) &&
        m_deSSA && value != m_deSSA->getNodeValue(value))
    {
        // Generate CVariable alias.
        // Value and its aliasee must be of the same size.
        Value* nodeVal = m_deSSA->getNodeValue(value);
        assert(nodeVal != value && "ICE: value must be aliaser!");

        // For non node value, get symbol for node value first.
        // Then, get an alias to that node value.
        CVariable *Base = GetSymbol(nodeVal);
        CVariable *AliasVar = createAliasIfNeeded(value, Base);
        symbolMapping.insert(std::pair<llvm::Value*, CVariable*>(value, AliasVar));
        return AliasVar;
    }

    if (IGC_IS_FLAG_ENABLED(EnableVariableAlias) &&
        IGC_GET_FLAG_VALUE(VATemp) == 0)
    {
        if (m_VRA->m_ValueAliasMap.count(value))
        {
            // Generate alias
            SSubVecDesc& SV = m_VRA->m_ValueAliasMap[value];
            Value* BaseVec = SV.BaseVector;
            int startIx = SV.StartElementOffset;

            CVariable *Base = GetSymbol(BaseVec);

            Type *Ty = value->getType();
            VectorType* VTy = dyn_cast<VectorType>(Ty);
            Type *BTy = VTy ? VTy->getElementType() : Ty;
            int nelts = (VTy ? (int)VTy->getNumElements() : 1);

            VISA_Type visaTy = GetType(BTy);
            int typeBytes = (int)CEncoder::GetCISADataTypeSize(visaTy);
            int offsetInBytes = typeBytes * startIx;
            int nbelts = nelts;
            if (!Base->IsUniform())
            {
                int width = (int)numLanes(m_SIMDSize);
                offsetInBytes *= width;
                nbelts *= width;
            }
            CVariable* AliasVar = GetNewAlias(Base, visaTy, offsetInBytes, nbelts);
            symbolMapping.insert(std::pair<llvm::Value*, CVariable*>(value, AliasVar));
            return AliasVar;
        }
    }

    if (!isa<InsertElementInst>(value) && value->hasOneUse()) {
        auto IEI = dyn_cast<InsertElementInst>(value->user_back());
        if (IEI && CanTreatScalarSourceAsAlias(IEI)) {
            CVariable *Var = GetSymbol(IEI);
            llvm::ConstantInt *Idx = llvm::cast<llvm::ConstantInt>(IEI->getOperand(2));
            unsigned short NumElts = 1;
            unsigned EltSz = CEncoder::GetCISADataTypeSize(GetType(IEI->getType()->getScalarType()));
            unsigned Offset = unsigned(Idx->getZExtValue() * EltSz);
            if (!Var->IsUniform()) {
                NumElts = numLanes(m_SIMDSize);
                Offset *= Var->getOffsetMultiplier() * numLanes(m_SIMDSize);
            }
            CVariable *Alias = GetNewAlias(Var, Var->GetType(), (uint16_t)Offset, NumElts);
            // FIXME: It makes no sense to map it as this `value` is
            // single-used implied from CanTreatScalarSourceAsAlias().
            symbolMapping.insert(std::pair<llvm::Value *, CVariable *>(value, Alias));
            return Alias;
        }
    }

    if(llvm::ExtractElementInst* EEI = llvm::dyn_cast<ExtractElementInst>(value))
    {
        if(CanTreatAsAlias(EEI))
        {
            llvm::ConstantInt* pConstElem = llvm::dyn_cast<llvm::ConstantInt>(EEI->getIndexOperand());
            assert(pConstElem);
            Value* vecOperand = EEI->getVectorOperand();
            // need to call GetSymbol() before AdjustExtractIndex(), since
            // GetSymbol may update mask of the vector operand.
            CVariable* vec = GetSymbol(vecOperand);

            uint element = AdjustExtractIndex(vecOperand, (uint16_t)pConstElem->getZExtValue());
            assert (( element < (UINT16_MAX ))&& "ExtractElementInst element index > higher than 64k");

            // see if distinct CVariables were created during vector bitcast copy
            if (auto vectorBCI = dyn_cast<BitCastInst>(vecOperand))
            {
                CVariable* EEIVar = getCVarForVectorBCI(vectorBCI, element);
                if (EEIVar)
                {
                    return EEIVar;
                }
            }

            uint offset = 0;
            unsigned EltSz = CEncoder::GetCISADataTypeSize(GetType(EEI->getType()));
            if( GetIsUniform( EEI->getOperand(0) ) )
            {
                offset = int_cast<unsigned int>(element * EltSz);
            }
            else
            {
                offset = int_cast<unsigned int>(vec->getOffsetMultiplier() * element * numLanes(m_SIMDSize) * EltSz);
            }
            assert (( offset < (UINT16_MAX ))&& "computed alias offset higher than 64k");

            // You'd expect the number of elements of the extracted variable to be
            // vec->GetNumberElement() / vecOperand->getType()->getVectorNumElements().
            // However, vec->GetNumberElement() is not always what you'd expect it to be because of
            // the pruning code in GetNbVectorElement().
            // So, recompute the number of elements from scratch.
            uint16_t numElements = 1;
            if (!vec->IsUniform())
            {
                numElements = numLanes(m_SIMDSize);
            }
            var = GetNewAlias(vec, vec->GetType(), (uint16_t)offset, numElements);
            symbolMapping.insert(std::pair<llvm::Value*,CVariable*>(value,var));
            return var;
        }
    }

    if (GenIntrinsicInst *genInst = dyn_cast<GenIntrinsicInst>(value))
    {
        if (VMECoalescePattern(genInst))
        {
            auto *Sym   = GetSymbol(genInst->getOperand(0));
            auto *Alias = GetNewAlias(Sym, Sym->GetType(), 0, Sym->GetNumberElement());
            symbolMapping.insert(std::pair<Value *, CVariable *>(value, Alias));
            return Alias;
        }
        if (genInst->getIntrinsicID() == GenISAIntrinsic::GenISA_UpdateDiscardMask)
        {
            assert(GetShaderType() == ShaderType::PIXEL_SHADER);
            return (static_cast<CPixelShader*>(this))->GetDiscardPixelMask();
        }
    }

    if( m_coalescingEngine ){
        CoalescingEngine::CCTuple* ccTuple = m_coalescingEngine->GetValueCCTupleMapping(value);
        if( ccTuple ){
            VISA_Type type = GetType(value->getType());
            CVariable* var = LazyCreateCCTupleBackingVariable( ccTuple, type );

            int mult = 1;
            if (value->getType()->isHalfTy() && m_SIMDSize == SIMDMode::SIMD8)
            {
                mult = 2;
            }

            //FIXME: Could improve by copying types from value

            unsigned EltSz = CEncoder::GetCISADataTypeSize(type);
            int offset = int_cast<int>(mult * (m_coalescingEngine->GetValueOffsetInCCTuple(value) - ccTuple->GetLeftBound()) *
                numLanes(m_SIMDSize) * EltSz);

            if (ccTuple->HasNonHomogeneousElements())
            {
                offset += m_coalescingEngine->GetLeftReservedOffset(ccTuple->GetRoot(), m_SIMDSize);
            }

            TODO("NumElements in this alias is 0 to preserve previous behavior. I have no idea what it should be.");
            assert (( offset < (UINT16_MAX ))&& "alias offset > higher than 64k");
            CVariable* newVar = GetNewAlias(var, type, (uint16_t) offset, 0);
            symbolMapping.insert(std::pair<llvm::Value*,CVariable*>(value,newVar));
            return newVar;
        }
    }

    // If we use a value which is not marked has needed by the pattern matching something went wrong
    assert(!isa<Instruction>(value) || isa<PHINode>(value) || m_CG->NeedInstruction(cast<Instruction>(*value)));

    e_alignment preferredAlign = GetPreferredAlignment(value, m_WI, GetContext());

    // simple de-ssa, always creates a new svar, and return
    if (!m_deSSA)
    {
        var = GetNewVector(value, preferredAlign);
        symbolMapping.insert(std::pair<llvm::Value*,CVariable*>(value, var));
        return var;
    }

    llvm::Value *rootValue =  m_deSSA->getRootValue(value, &preferredAlign);
    // belong to a congruent class
    if (rootValue)
    {
        it = symbolMapping.find(rootValue);
        if (it != symbolMapping.end())
        {
            var = it->second;
            CVariable* aV = var;
            if (IGC_GET_FLAG_VALUE(EnableDeSSAAlias) >= 2)
            {
                aV = createAliasIfNeeded(value, var);
            }
            symbolMapping.insert(std::pair<llvm::Value*, CVariable*>(value, aV));
            /*
            *  When we don't scalarize vectors, vector may come from phi/insert-element
            *  We cannot adjust extract-mask
            */
            if (value->getType()->isVectorTy())
            {
                extractMasks.erase(value);
            }
            return aV;
        }
    }

    if (IGC_IS_FLAG_ENABLED(EnableVariableReuse))
    {
        // Only for instrunctions and do not reuse flag variables.
        if (!value->getType()->getScalarType()->isIntegerTy(1))
        {
            if (auto Inst = dyn_cast<Instruction>(value))
            {
                var = GetSymbolFromSource(Inst, preferredAlign);
            }
        }
    }

    // need to create a new mapping
    if (!var)
    {
        var = GetNewVector(value, preferredAlign);
    }

    symbolMapping.insert(std::pair<llvm::Value*,CVariable*>(value, var));
    if (rootValue)
    {
        CVariable* aV = var;
        if (IGC_GET_FLAG_VALUE(EnableDeSSAAlias) >= 2)
        {
            aV = createAliasIfNeeded(rootValue, var);
        }
        symbolMapping.insert(std::pair<llvm::Value*, CVariable*>(rootValue, aV));
    }
    return var;
}

/// WHEN implement vector-coalescing, want to be more conservative in
/// treating extract-element as alias in order to reduce the complexity of
/// the problem
bool CShader::CanTreatAsAlias(llvm::ExtractElementInst *inst)
{
    llvm::Value *idxSrc = inst->getIndexOperand();
    if (!isa<llvm::ConstantInt>(idxSrc))
    {
        return false;
    }

    llvm::Value* vecSrc = inst->getVectorOperand();
    if (isa<llvm::InsertElementInst>(vecSrc))
    {
        return false;
    }

    if (IsCoalesced(inst) || IsCoalesced(vecSrc))
    {
        return false;
    }

    for (auto I = vecSrc->user_begin(), E = vecSrc->user_end(); I != E; ++I)
    {
        llvm::ExtractElementInst* extract = llvm::dyn_cast<llvm::ExtractElementInst>(*I);
        if (!extract)
        {
            return false;
        }
        if (!isa<ConstantInt>(extract->getIndexOperand()))
        {
            return false;
        }
    }

    return true;
}

static bool isUsedInPHINode(llvm::Instruction *I) {
    for (auto U : I->users()) {
        if (isa<PHINode>(U))
            return true;
        if (auto BC = dyn_cast<BitCastInst>(U)) {
            if (isUsedInPHINode(BC))
                return true;
        }
        if (auto IEI = dyn_cast<InsertElementInst>(U)) {
            if (isUsedInPHINode(IEI))
                return true;
        }
    }
    return false;
}

bool CShader::CanTreatScalarSourceAsAlias(llvm::InsertElementInst *IEI) {
    // Skip if it's not enabled.
    if (!IGC_IS_FLAG_ENABLED(EnableInsertElementScalarCoalescing))
        return false;
    // Skip if IEI is used in PHI.
    // FIXME: Should skip PHI if this IEI is from its backedge.
    if (isUsedInPHINode(IEI))
        return false;
    // Skip if the index is not constant.
    llvm::ConstantInt *IdxOp = dyn_cast<llvm::ConstantInt>(IEI->getOperand(2));
    if (!IdxOp)
        return false;
    // Skip if the scalar operand is not single-used.
    Value *ScalarOp = IEI->getOperand(1);
    if (!ScalarOp->hasOneUse())
        return false;
    // Skip if the scalar operand is not an instruction.
    if (!isa<llvm::Instruction>(ScalarOp))
        return false;
    // Skip the scalar operand may be treated as alias.
    if (auto PN = llvm::dyn_cast<llvm::PHINode>(ScalarOp))
        return false;
    if (auto EEI = llvm::dyn_cast<llvm::ExtractElementInst>(ScalarOp)) {
        if (CanTreatAsAlias(EEI))
            return false;
    }
    auto Def = cast<llvm::Instruction>(ScalarOp);
    auto BB = Def->getParent();
    // Skip that scalar value is not defined locally.
    if (BB != IEI->getParent())
        return false;
    if (!m_deSSA)
        return isa<llvm::UndefValue>(IEI->getOperand(0));
    // Since we will define that vector element ahead from the previous
    // position, check whether such hoisting is safe.
    auto BI = std::prev(llvm::BasicBlock::reverse_iterator(IEI->getIterator()));
    auto BE = std::prev(llvm::BasicBlock::reverse_iterator(Def->getIterator()));
    auto Idx = IdxOp->getZExtValue();
    for (; BI != BE && BI != BB->rend(); ++BI) {
        if (&*BI != IEI)
            continue;
        Value *VecOp = IEI->getOperand(0);
        // If the source operand is `undef`, `insertelement` could be always
        // treated as alias (of the destination of the scalar operand).
        if (isa<UndefValue>(VecOp))
            return true;
        Value *SrcRoot = m_deSSA->getRootValue(VecOp);
        Value *DstRoot = m_deSSA->getRootValue(IEI);
        // `dst` vector will be copied from `src` vector if they won't coalese.
        // Hoisting this insertion is unsafe.
        if (SrcRoot != DstRoot)
            return false;
        IEI = dyn_cast<llvm::InsertElementInst>(VecOp);
        // However, if `src` is not defined through `insertelement`, it's still
        // unsafe to hoist this insertion.
        if (!IEI)
            return false;
        // If that's dynamically indexed insertion or insertion on the same
        // index, it's unsafe to hoist this insertion.
        llvm::ConstantInt *IdxOp = dyn_cast<llvm::ConstantInt>(IEI->getOperand(2));
        if (!IdxOp)
            return false;
        if (IdxOp->getZExtValue() == Idx)
            return false;
    }
    return true;
}

bool CShader::HasBecomeNoop(Instruction *inst) {
    return m_VRA->m_HasBecomeNoopInsts.count(inst);
}

bool CShader::IsCoalesced(Value* V) {
    if ((m_VRA && m_VRA->isAliasedValue(V)) ||
        (m_deSSA && m_deSSA->getRootValue(V)) ||
        (m_coalescingEngine && m_coalescingEngine->GetValueCCTupleMapping(V)))
    {
        return true;
    }
    return false;
}

#define SET_INTRINSICS()                              \
         GenISAIntrinsic::GenISA_setMessagePhaseX:    \
    case GenISAIntrinsic::GenISA_setMessagePhaseXV:   \
    case GenISAIntrinsic::GenISA_setMessagePhase:     \
    case GenISAIntrinsic::GenISA_setMessagePhaseV:    \
    case GenISAIntrinsic::GenISA_simdSetMessagePhase: \
    case GenISAIntrinsic::GenISA_simdSetMessagePhaseV

static bool IsSetMessageIntrinsic(GenIntrinsicInst *I)
{
    switch (I->getIntrinsicID())
    {
    case SET_INTRINSICS():
        return true;
    default:
        return false;
    }
}

bool CShader::VMECoalescePattern(GenIntrinsicInst *genInst)
{
    if (!IsSetMessageIntrinsic(genInst))
        return false;

    if (IsCoalesced(genInst))
    {
        return false;
    }

    if (GenIntrinsicInst *argInst = dyn_cast<GenIntrinsicInst>(genInst->getOperand(0)))
    {
        if (IsCoalesced(argInst))
        {
            return false;
        }

        switch (argInst->getIntrinsicID())
        {
        case GenISAIntrinsic::GenISA_createMessagePhases:
        case GenISAIntrinsic::GenISA_createMessagePhasesV:
        case GenISAIntrinsic::GenISA_createMessagePhasesNoInit:
        case GenISAIntrinsic::GenISA_createMessagePhasesNoInitV:
        case SET_INTRINSICS():
        {
            bool OneUse = argInst->hasOneUse();

            if (OneUse)
            {
                return (argInst->getParent() == genInst->getParent());
            }

            // If we don't succeed in the quick check above, also match if there
            // is a single set intrinsic and all of the other users dominate the
            // set intrinsic in the block.

            SmallPtrSet<Value*, 4> Users(argInst->user_begin(), argInst->user_end());

            uint32_t SetMessageCnt = 0U;
            for (auto U : Users)
            {
                if (!isa<GenIntrinsicInst>(U))
                    return false;

                auto *GII = cast<GenIntrinsicInst>(U);
                if (GII->getParent() != argInst->getParent())
                    return false;

                if (IsSetMessageIntrinsic(GII))
                    SetMessageCnt++;
            }

            if (SetMessageCnt > 1)
                return false;

            uint32_t NonSetInsts = Users.size() - SetMessageCnt;

            auto E = argInst->getParent()->end();
            for (auto I = argInst->getIterator(); I != E; I++)
            {
                if (Users.count(&*I) != 0)
                {
                    if (IsSetMessageIntrinsic(cast<GenIntrinsicInst>(&*I)))
                    {
                        return false;
                    }
                    else
                    {
                        if (--NonSetInsts == 0)
                            break;
                    }
                }
            }

            return true;
        }
        default:
            return false;
        }
    }

    return false;

}

#undef SET_INTRINSICS

bool CShader::isUnpacked(llvm::Value* value)
{
    bool isUnpacked = false;
    if(m_SIMDSize == SIMDMode::SIMD8)
    {
        if(isa<SampleIntrinsic>(value) || isa<LdmcsInstrinsic>(value))
        {
            if(value->getType()->getVectorElementType()->isHalfTy() ||
                value->getType()->getVectorElementType()->isIntegerTy(16))
            {
                isUnpacked = true;
                auto uses = value->user_begin();
                auto endUses = value->user_end();
                while(uses != endUses)
                {
                    if(llvm::ExtractElementInst* extrElement = dyn_cast<llvm::ExtractElementInst>(*uses))
                    {
                        if(CanTreatAsAlias(extrElement))
                        {
                            ++uses;
                            continue;
                        }
                    }
                    isUnpacked = false;
                    break;
                }
            }
        }
    }
    return isUnpacked;
}
/// GetNewVector
///
CVariable* CShader::GetNewVector(llvm::Value* value, e_alignment preferredAlign)
{
    VISA_Type type = GetType(value->getType());
    bool uniform = GetIsUniform(value);
    uint32_t mask = 0;
    bool isUnpackedBool = isUnpacked(value);
    uint8_t multiplier = (isUnpackedBool) ? 2 : 1;
    uint nElem =  GetNbElementAndMask(value, mask) * multiplier;
    assert (( nElem < (UINT16_MAX))&& "getNumElements more than 64k elements");
    const uint16_t nbElement = (uint16_t)nElem;
    // TODO: Non-uniform variable should be naturally aligned instead of GRF
    // aligned. E.g., <8 x i16> should be aligned to 16B instead of 32B or GRF.
    e_alignment align = EALIGN_GRF;
    if (uniform) {
        // So far, preferredAlign is only applied to uniform variable.
        // TODO: Add preferred alignment for non-uniform variables.
        align = preferredAlign;
        if (align == EALIGN_AUTO)
            align = CEncoder::GetCISADataTypeAlignment(type);
    }
    uint16_t numberOfInstance = m_numberInstance;
    if (uniform)
    {
        if (type != ISA_TYPE_BOOL || m_CG->canEmitAsUniformBool(value))
        {
            numberOfInstance = 1;
        }
    }
    if (mask)
    {
        extractMasks[value] = mask;
    }
    CVariable* var = GetNewVariable(nbElement, type, align, uniform, numberOfInstance);
    if (isUnpackedBool)
        var->setisUnpacked();
    return var;
}

/// GetNewAlias
CVariable* CShader::GetNewAlias(CVariable* var, VISA_Type type, uint16_t offset, uint16_t numElements)
{
    assert(!var->IsImmediate() && "Trying to create an alias of an immediate");
    CVariable* alias = new (Allocator)CVariable(var, type, offset, numElements, var->IsUniform());
    encoder.CreateVISAVar(alias);
    return alias;
}

// createAliasIfNeeded() returns the Var that is either BaseVar or
// its alias of the same size.
//
// If BaseVar's type matches V's, return BaseVar; otherwise, create an
// new alias CVariable to BaseVar. The new CVariable has V's size, which
// should not be larger than BaseVar's.
//
// Note that V's type is either vector or scalar.
CVariable*  CShader::createAliasIfNeeded(Value* V, CVariable* BaseVar)
{
    Type *Ty = V->getType();
    VectorType* VTy = dyn_cast<VectorType>(Ty);
    Type *BTy = VTy ? VTy->getElementType() : Ty;
    VISA_Type visaTy = GetType(BTy);
    if (visaTy == BaseVar->GetType())
    {
        return BaseVar;
    }

    uint16_t visaTy_sz = CEncoder::GetCISADataTypeSize(visaTy);
    uint16_t nbe = BaseVar->GetSize() / visaTy_sz;
    assert((BaseVar->GetSize() % visaTy_sz) == 0 &&
           "V's Var should be the same size as BaseVar!");
    CVariable* NewAliasVar = GetNewAlias(BaseVar, visaTy, 0, nbe);
    return NewAliasVar;
}

/// GetNewAlias
CVariable* CShader::GetNewAlias(CVariable* var, VISA_Type type, uint16_t offset, uint16_t numElements, bool uniform)
{
    assert(!var->IsImmediate() && "Trying to create an alias of an immediate");
    CVariable* alias = new (Allocator) CVariable(var, type, offset, numElements, uniform);
    encoder.CreateVISAVar(alias);
    return alias;
}

CVariable* CShader::GetVarHalf(CVariable* var, unsigned int half)
{
    assert(!var->IsImmediate() && "Trying to create an alias of an immediate");
    CVariable* alias = new (Allocator) CVariable(
        var->GetNumberElement(),
        var->IsUniform(),
        var->GetType(),
        var->GetVarType(),
        var->GetAlign(),
        var->IsVectorUniform(),
        1);
    alias->visaGenVariable[0] = var->visaGenVariable[half];
    return alias;
}

void CShader::GetPayloadElementSymbols(llvm::Value *inst, CVariable *payload[], int vecWidth)
{
    llvm::ConstantDataVector *cv = llvm::dyn_cast<llvm::ConstantDataVector>(inst);
    if (cv) {
        assert(vecWidth == cv->getNumElements());
        for (int i = 0; i < vecWidth; ++i) {
            payload[i] = GetSymbol(cv->getElementAsConstant(i));
        }
        return;
    }

    llvm::InsertElementInst *ie = llvm::dyn_cast<llvm::InsertElementInst>(inst);
    assert(ie);

    for (int i = 0; i < vecWidth; ++i) {
        payload[i] = NULL;
    }

    int count = 0;
    //Gather elements of vector
    while (ie != NULL) {
        int64_t iOffset =  llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2))->getSExtValue();
        assert(iOffset >= 0 && iOffset < vecWidth);

        // Get the scalar value from this insert
        if (payload[iOffset] == NULL) {
            payload[iOffset] = GetSymbol(ie->getOperand(1));
            count++;
        }

        // Do we have another insert?
        llvm::Value *insertBase = ie->getOperand(0);
        ie = llvm::dyn_cast<llvm::InsertElementInst>(insertBase);
        if (ie != NULL) {
            continue;
        }

        if (llvm::isa<llvm::UndefValue>(insertBase)) {
            break;
        }
    }
    assert(count == vecWidth);
}

void CShader::Destroy()
{
}

void CShader::SampleHeader(CVariable* payload, uint offset, uint writeMask, uint rti)
{
    uint dword2 = offset | ((~writeMask & 0xF) << 12);
    if(rti)
    {
        //For sampler to RT write, set the Render target binding table index
        dword2 = dword2 | (rti << 24);
    }
    CVariable* temp = ImmToVariable(dword2, ISA_TYPE_D);

    encoder.SetNoMask();
    encoder.SetSimdSize(SIMDMode::SIMD8);
    encoder.Copy(payload, m_R0);
    encoder.Push();

    encoder.SetNoMask();
    encoder.SetSimdSize(SIMDMode::SIMD1);
    encoder.SetDstSubReg(2);
    encoder.SetSrcRegion(0, 0, 1, 0);
    //TODO : Should be or. But having some issue
    //encoder.or(immoffset, immoffset, temp);
    encoder.Copy(payload, temp);
    encoder.Push();
}

void CShader::ConstantBufferAccesed(uint index)
{
    m_constantBufferLoaded |= BIT(index);
}

// Helper function to copy raw register
void CShader::CopyVariable(
    CVariable* dst,
    CVariable* src,
    uint dstSubVar,
    uint srcSubVar)
{
    CVariable* rawDst = dst;
    // The source have to match for a raw copy
    if(src->GetType() != dst->GetType())
    {
        rawDst = BitCast(dst, src->GetType());
    }
    encoder.SetSrcSubVar(0, srcSubVar);
    encoder.SetDstSubVar(dstSubVar);
    encoder.Copy(rawDst, src);
    encoder.Push();
}

// Helper function to copy and pack raw register
void CShader::PackAndCopyVariable(
    CVariable* dst,
    CVariable* src,
    uint subVar)
{
    CVariable* rawDst = dst;
    // The source have to match for a raw copy
    if(src->GetType() != dst->GetType())
    {
        rawDst = BitCast(dst, src->GetType());
    }
    encoder.SetDstSubVar(subVar);
    if(!src->IsUniform())
    {
        encoder.SetSrcRegion(0, 16, 8, 2);
    }
    encoder.Copy(rawDst, src);
    encoder.Push();
}

bool CShader::CompileSIMDSizeInCommon()
{
    return (m_ScratchSpaceSize <= m_ctx->platform.maxPerThreadScratchSpace());
}

CShader* CShaderProgram::GetShader(SIMDMode simd, ShaderDispatchMode mode)
{
    return GetShaderPtr(simd, mode);
}

CShader*& CShaderProgram::GetShaderPtr(SIMDMode simd, ShaderDispatchMode mode)
{
    if(mode == ShaderDispatchMode::DUAL_PATCH)
    {
        return m_SIMDshaders[3];
    }
    switch(simd)
    {
    case SIMDMode::SIMD8:
        return m_SIMDshaders[0];
    case SIMDMode::SIMD16:
        return m_SIMDshaders[1];
    case SIMDMode::SIMD32:
        return m_SIMDshaders[2];
    default:
        assert(0 && "wrong SIMD size");
    }
    return m_SIMDshaders[0];
}

void CShaderProgram::ClearShaderPtr(SIMDMode simd)
{
    switch (simd)
    {
    case SIMDMode::SIMD8:   m_SIMDshaders[0] = nullptr; break;
    case SIMDMode::SIMD16:  m_SIMDshaders[1] = nullptr; break;
    case SIMDMode::SIMD32:  m_SIMDshaders[2] = nullptr; break;
    default:
        assert(0 && "wrong SIMD size");
    }
}

CShader* CShaderProgram::GetOrCreateShader(SIMDMode simd, ShaderDispatchMode mode)
{
    CShader*& pShader = GetShaderPtr(simd, mode);
    if(pShader == nullptr)
    {
        pShader = CreateNewShader(simd);
    }
    return pShader;
}

CShader* CShaderProgram::CreateNewShader(SIMDMode simd)
{
    CShader* pShader = nullptr;
    {
        switch(m_context->type)
        {
        case ShaderType::OPENCL_SHADER:
            pShader = new COpenCLKernel((OpenCLProgramContext*)m_context, m_kernel, this);
            break;
        case ShaderType::PIXEL_SHADER:
            pShader = new CPixelShader(m_kernel, this);
            break;
        case ShaderType::VERTEX_SHADER:
            pShader = new CVertexShader(m_kernel, this);
            break;
        case ShaderType::GEOMETRY_SHADER:
            pShader = new CGeometryShader(m_kernel, this);
            break;
        case ShaderType::HULL_SHADER:
            pShader = new CHullShader(m_kernel, this);
            break;
        case ShaderType::DOMAIN_SHADER:
            pShader = new CDomainShader(m_kernel, this);
            break;
        case ShaderType::COMPUTE_SHADER:
            pShader = new CComputeShader(m_kernel, this);
            break;
        default:
            assert(0 && "wrong shader type");
        }
    }
    pShader->m_shaderStats = m_shaderStats;
    pShader->m_DriverInfo = &m_context->m_DriverInfo;
    pShader->m_Platform = &m_context->platform;
    pShader->m_pBtiLayout = &m_context->btiLayout;
    pShader->m_ModuleMetadata = m_context->getModuleMetaData();
    return pShader;
}

unsigned int CShader::GetSamplerCount(unsigned int samplerCount)
{
    if (samplerCount > 0)
    {
        if (samplerCount <= 4)
            return 1; // between 1 and 4 samplers used
        else if (samplerCount >= 5 && samplerCount <= 8)
            return 2; // between 5 and 8 samplers used
        else if (samplerCount >= 9 && samplerCount <= 12)
            return 3; // between 9 and 12 samplers used
        else if (samplerCount >= 13 && samplerCount <= 16)
            return 4; // between 13 and 16 samplers used
        else
            // Samplers count out of range. Force value 0 to avoid undefined behavior.
            return 0;
    }
    return 0;
}

CShaderProgram::CShaderProgram(CodeGenContext* ctx, llvm::Function* kernel)
  : m_context(ctx)
  , m_kernel(kernel)
  , m_shaderStats(nullptr)
{
    memset(m_SIMDshaders, 0, 4 * sizeof(CShader*));
}

CShaderProgram::~CShaderProgram()
{
    for(unsigned int i = 0; i < 4; i++)
    {
        delete m_SIMDshaders[i];
    }
    m_context = nullptr;
}
