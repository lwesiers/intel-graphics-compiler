#===================== begin_copyright_notice ==================================

#Copyright (c) 2017 Intel Corporation

#Permission is hereby granted, free of charge, to any person obtaining a
#copy of this software and associated documentation files (the
#"Software"), to deal in the Software without restriction, including
#without limitation the rights to use, copy, modify, merge, publish,
#distribute, sublicense, and/or sell copies of the Software, and to
#permit persons to whom the Software is furnished to do so, subject to
#the following conditions:

#The above copyright notice and this permission notice shall be included
#in all copies or substantial portions of the Software.

#THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
#OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
#MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
#IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
#CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
#TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
#SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


#======================= end_copyright_notice ==================================


############# Currently Supported Types ######################
#PointerTypes = ["ptr_private","ptr_global","ptr_constant","ptr_local","ptr_generic"]
#FloatingPointTypes = ["half","float","double"]
#IntegerTypes = ["bool","char","short","int","long"]
#IntrinsicsProperties = ["None","NoMem","ReadArgMem","ReadMem","ReadWriteArgMem","NoReturn","NoDuplicate", "Convergent"]
#IntrinsicsProperties may be specified as a comma separated list (e.g., "Convergent,NoMem")

# EX. "GenISA_blah": [{return_type},[arg1_type,arg2_type.....],Property]

# The "any" type can be followed by a default type if a type is not explicitly specified: Ex. "any:int"

# 0 - LLVMMatchType<0>
# 1 - LLVMMatchType<1>
# {int} - LLVMMatchType<{int}>
# See Intrinsics.json file for entries

Imported_Intrinsics = \
{
    "GenISA_ROUNDNE": ["float",["float"],"NoMem"],
    "GenISA_imulH": ["anyint",[0,0],"NoMem"],
    "GenISA_umulH": ["anyint",[0,0],"NoMem"],
    "GenISA_f32tof16_rtz": ["float",["float"],"NoMem"],
    "GenISA_fsat": ["anyfloat",[0],"NoMem"],
    "GenISA_rsq": ["anyfloat",[0],"NoMem"],
    "GenISA_uaddc": ["anyvector",["anyint",1],"NoMem"],
    "GenISA_usubb": ["anyvector",["anyint",1],"NoMem"],
    "GenISA_bfi": ["int",["int","int","int","int"],"NoMem"],
    "GenISA_ibfe": ["int",["int","int","int"],"NoMem"],
    "GenISA_ubfe": ["int",["int","int","int"],"NoMem"],
    "GenISA_bfrev": ["int",["int"],"NoMem"],
    "GenISA_firstbitLo": ["int",["int"],"NoMem"],
    "GenISA_firstbitHi": ["int",["int"],"NoMem"],
    "GenISA_firstbitShi": ["int",["int"],"NoMem"],
    "GenISA_IEEE_Sqrt": ["float",["float"],"NoMem"],
    "GenISA_IEEE_Divide": ["anyfloat",[0,0],"NoMem"],
    "GenISA_ftoi_rte": ["anyint",["anyfloat"],"NoMem"],
    "GenISA_ftoi_rtp": ["anyint",["anyfloat"],"NoMem"],
    "GenISA_ftoi_rtn": ["anyint",["anyfloat"],"NoMem"],
    "GenISA_ftoui_rte": ["anyint",["anyfloat"],"NoMem"],
    "GenISA_ftoui_rtp": ["anyint",["anyfloat"],"NoMem"],
    "GenISA_ftoui_rtn": ["anyint",["anyfloat"],"NoMem"],
    "GenISA_ftof_rtn": ["anyfloat",["anyfloat"],"NoMem"],
    "GenISA_ftof_rtp": ["anyfloat",["anyfloat"],"NoMem"],
    "GenISA_ftof_rtz": ["anyfloat",["anyfloat"],"NoMem"],
    "GenISA_itof_rtn": ["anyfloat",["anyint"],"NoMem"],
    "GenISA_itof_rtp": ["anyfloat",["anyint"],"NoMem"],
    "GenISA_itof_rtz": ["anyfloat",["anyint"],"NoMem"],
    "GenISA_uitof_rtn": ["anyfloat",["anyint"],"NoMem"],
    "GenISA_uitof_rtp": ["anyfloat",["anyint"],"NoMem"],
    "GenISA_uitof_rtz": ["anyfloat",["anyint"],"NoMem"],
    "GenISA_mul_rtz" : ["anyfloat", [0, 0], "NoMem"],
    "GenISA_fma_rtz" : ["anyfloat", [0, 0, 0], "NoMem"],
    "GenISA_add_rtz" : ["anyfloat", [0, 0], "NoMem"],
    "GenISA_ldstructured": ["float4",["anyptr","int","int"],"ReadArgMem"],
    "GenISA_storestructured1": ["void",["anyptr","int","int","float"],"None"],
    "GenISA_storestructured2": ["void",["anyptr","int","int","float","float"],"None"],
    "GenISA_storestructured3": ["void",["anyptr","int","int","float","float","float"],"None"],
    "GenISA_storestructured4": ["void",["anyptr","int","int","float","float","float","float"],"None"],
    "GenISA_typedread": ["float4",["anyptr","int","int","int", "int"],"ReadArgMem"],
    "GenISA_typedwrite": ["void",["anyptr","int","int","int","int","float","float","float","float"],"None"],
    "GenISA_ldraw_indexed": ["any:float",["anyptr","int", "int"],"ReadArgMem"],
    "GenISA_ldrawvector_indexed": ["anyvector",["anyptr","int", "int"],"ReadArgMem"],
    "GenISA_storeraw_indexed": ["void",["anyptr","int","any:float", "int"],"None"],
    "GenISA_storerawvector_indexed": ["void",["anyptr","int","anyvector", "int"],"None"],
    "GenISA_intatomicraw": ["anyint",["anyptr","int",0,"int"],"ReadWriteArgMem"],
    "GenISA_floatatomicraw": ["anyfloat",["anyptr","int",0,"int"],"ReadWriteArgMem"],
    "GenISA_intatomicrawA64": ["anyint",["anyptr","anyptr",0,"int"],"ReadWriteArgMem"],
    "GenISA_floatatomicrawA64": ["anyfloat",["anyptr","anyptr",0,"int"],"ReadWriteArgMem"],
    "GenISA_dwordatomicstructured": ["int",["anyptr","int","int","int","int"],"ReadWriteArgMem"],
    "GenISA_floatatomicstructured": ["float",["anyptr","int","int","float","int"],"ReadWriteArgMem"],
    "GenISA_intatomictyped": ["anyint",["anyptr","int","int","int",0,"int"],"ReadWriteArgMem"],
    "GenISA_icmpxchgatomicraw": ["anyint",["anyptr","int",0,0],"None"],
    "GenISA_fcmpxchgatomicraw": ["anyfloat",["anyptr","int",0,0],"None"],
    "GenISA_icmpxchgatomicrawA64": ["anyint",["anyptr","anyptr",0,0],"ReadWriteArgMem"],
    "GenISA_fcmpxchgatomicrawA64": ["anyfloat",["anyptr","anyptr",0,0],"ReadWriteArgMem"],
    "GenISA_cmpxchgatomicstructured": ["int",["anyptr","int","int","int","int"],"ReadWriteArgMem"],
    "GenISA_fcmpxchgatomicstructured": ["float",["anyptr","int","int","float","float"],"ReadWriteArgMem"],
    "GenISA_icmpxchgatomictyped": ["anyint",["anyptr","int","int","int",0,0],"ReadWriteArgMem"],
    "GenISA_atomiccounterinc": ["int",["anyptr"],"ReadWriteArgMem"],
    "GenISA_atomiccounterpredec": ["int",["anyptr"],"ReadWriteArgMem"],
    "GenISA_threadgroupbarrier": ["void",[],"Convergent"],
    "GenISA_threadgroupbarrier_signal": ["void",[],"Convergent"],
    "GenISA_threadgroupbarrier_wait": ["void",[],"Convergent"],
    "GenISA_wavebarrier": ["void",[],"Convergent"],
    "GenISA_memoryfence": ["void",["bool","bool","bool","bool","bool","bool","bool"],"Convergent"],
    "GenISA_flushsampler": ["void",[],"None"],
    "GenISA_globalSync": ["void",[],"Convergent"],
    "GenISA_uavSerializeOnResID": ["void",["int"],"None"],
    "GenISA_uavSerializeAll": ["void",[],"None"],
    "GenISA_WorkGroupAny": ["int",["int"],"None"],
    "GenISA_sampleKillPix": ["anyvector",["anyfloat",1,1,"anyptr","anyptr","int","int","int"],"NoMem"],
    "GenISA_evaluateSampler": ["void",["anyvector"],"None"],
    "GenISA_ldmsptr16bit": ["anyvector",["short","short","short","short","short","short","short","short","short","anyptr","int","int","int"],"NoMem"],
    "GenISA_ldmsptr": ["anyvector",["int","int","int","int","int","int","int","anyptr","int","int","int"],"NoMem"],
    "GenISA_ldmcsptr": ["anyvector",["anyint",1,1,1,"anyptr","int","int","int"],"NoMem"],
    "GenISA_lodptr": ["anyvector",["anyfloat",1,1,1,"anyptr","anyptr"],"NoMem"],
    "GenISA_sampleptr": ["anyvector",["anyfloat",1,1,1,1,"anyptr","anyptr","int","int","int"],"NoMem"],
    "GenISA_sampleBptr": ["anyvector",["anyfloat",1,1,1,1,1,"anyptr","anyptr","int","int","int"],"NoMem"],
    "GenISA_sampleCptr": ["anyvector",["anyfloat",1,1,1,1,1,"anyptr","anyptr","int","int","int"],"NoMem"],
    "GenISA_sampleDptr": ["anyvector",["anyfloat",1,1,1,1,1,1,1,1,1,1,"anyptr","anyptr","int","int","int"],"NoMem"],
    "GenISA_sampleDCptr": ["anyvector",["anyfloat",1,1,1,1,1,1,1,1,1,1,"anyptr","anyptr","int","int","int"],"NoMem"],
    "GenISA_sampleLptr": ["anyvector",["anyfloat",1,1,1,1,"anyptr","anyptr","int","int","int"],"NoMem"],
    "GenISA_sampleLCptr": ["anyvector",["anyfloat",1,1,1,1,1,"anyptr","anyptr","int","int","int"],"NoMem"],
    "GenISA_sampleBCptr": ["anyvector",["anyfloat",1,1,1,1,1,"anyptr","anyptr","int","int","int"],"NoMem"],
    "GenISA_ldptr": ["anyvector",["int","int","int","int","anyptr","int","int","int"],"ReadArgMem"],
    "GenISA_readsurfaceinfoptr": ["int16",["anyptr","int"],"NoMem"],
    "GenISA_resinfoptr": ["int4",["anyptr","int"],"NoMem"],
    "GenISA_sampleinfoptr": ["int4",["anyptr"],"NoMem"],
    "GenISA_gather4ptr": ["anyvector",["anyfloat",1,1,1,"anyptr","anyptr","int","int","int","int"],"NoMem"],
    "GenISA_gather4Cptr": ["anyvector",["anyfloat",1,1,1,1,"anyptr","anyptr","int","int","int","int"],"NoMem"],
    "GenISA_gather4POptr": ["anyvector",["anyfloat",1,"int","int",1,"anyptr","anyptr","int","int","int","int"],"NoMem"],
    "GenISA_gather4POCptr": ["anyvector",["anyfloat",1,1,"int","int",1,"anyptr","anyptr","int","int","int","int"],"NoMem"],
    "GenISA_RuntimeValue": ["any:float",["int"],"NoMem"],
    "GenISA_GetBufferPtr": ["anyptr",["int","int"],"NoMem"],
    "GenISA_DCL_inputVec": ["anyfloat",["int","int"],"NoMem"],
    # (dwordAttributeOrSetupIndex, e_interpolation_PSOnly)->anyvector
    "GenISA_DCL_ShaderInputVec": ["anyvector",["int","int"],"NoMem"],
    "GenISA_DCL_GSinputVec": ["float4",["int","int"],"NoMem"],
    "GenISA_DCL_SystemValue": ["anyfloat",["int"],"NoMem"],
    "GenISA_SampleOffsetX": ["float",["int"],"NoMem"],
    "GenISA_SampleOffsetY": ["float",["int"],"NoMem"],
    "GenISA_PixelPositionX": ["short",[],"NoMem"],
    "GenISA_PixelPositionY": ["short",[],"NoMem"],
    "GenISA_DCL_GSsystemValue": ["float",["int","int"],"NoMem"],
    "GenISA_DCL_input": ["int",["int","int"],"None"],
    "GenISA_OUTPUT": ["void",["anyfloat",0,0,0,"int","int"],"NoDuplicate"],
    "GenISA_PatchConstantOutput": ["void",["anyfloat",0,0,0,"int","int"],"None"],
    "GenISA_PHASE_OUTPUT": ["void",["float","int"],"None"],
    "GenISA_PHASE_INPUT": ["float",["int"],"NoMem"],
    "GenISA_cycleCounter": ["int2",[],"None"],
    "GenISA_PullSampleIndexBarys": ["float2",["int","bool"],"NoMem"],
    "GenISA_PullSnappedBarys": ["float2",["int","int","bool"],"NoMem"],
    "GenISA_Interpolate": ["float",["int","float2"],"NoMem"],
    "GenISA_GradientX": ["anyfloat",[0],"NoMem"],
    "GenISA_GradientXfine": ["float",["float"],"NoMem"],
    "GenISA_GradientY": ["anyfloat",[0],"NoMem"],
    "GenISA_GradientYfine": ["float",["float"],"NoMem"],
    "GenISA_discard": ["void",["bool"],"None"],
    "GenISA_OUTPUTGS": ["void",["float","float","float","float","int","int","int"],"None"],
    "GenISA_OUTPUTGS2": ["void",["float","float","float","float","float","float","float","float","int","int","int"],"None"],
    "GenISA_EndPrimitive": ["void",["int"],"None"],
    "GenISA_SetStream": ["void",["int","int"],"None"],
    "GenISA_GsCutControlHeader": ["void",["int","int","int","int","int","int","int","int","int","int",
                                          "int","int","int","int","int","int","int"],"None"],
    "GenISA_GsStreamHeader": ["void",["int","int","int","int","int","int","int","int","int","int","int",
                                      "int","int","int","int","int","int","int","int","int","int","int",
                                      "int","int","int","int","int","int","int","int","int","int","int",
                                      "int","int","int","int","int","int","int","int","int","int","int",
                                      "int","int","int","int","int","int","int","int","int","int","int",
                                      "int","int","int","int","int","int","int","int","int","int"],"None"],
    "GenISA_DCL_HSControlPointID": ["int",[],"None"],
    "GenISA_OutputTessControlPoint": ["void",["float","float","float","float","int","int","int"],"None"],
    "GenISA_OutputTessFactors": ["void",["float","float","float","float","float","float"],"None"],
    # (owordAttributeIndex)->float4
    "GenISA_DCL_HSPatchConstInputVec": ["float4",["int"],"ReadMem"],
    "GenISA_OuterScalarTessFactors": ["void",["int","float"],"None"],
    "GenISA_InnerScalarTessFactors": ["void",["int","float"],"None"],
    "GenISA_DCL_DSPatchConstInputVec": ["float4",["int"],"NoMem"],
    "GenISA_DCL_DSInputTessFactor": ["float",["int"],"NoMem"],
    "GenISA_DCL_DSCntrlPtInputVec": ["float4",["int","int"],"NoMem"],
    "GenISA_DCL_HSinputVec": ["float4",["int","int"],"NoMem"],
    # (owordVertexIndex, owordAttributeIndex)->float4
    "GenISA_DCL_HSOutputCntrlPtInputVec": ["float4",["int","int"],"ReadMem"],
    "GenISA_HSURBPatchHeaderRead": ["float8",[],"NoMem"],
    "GenISA_RenderTargetRead": ["float4",["int"],"ReadMem"],
    "GenISA_RenderTargetReadSampleFreq": ["float4",["int","int"],"ReadMem"],
    "GenISA_patchInstanceId": ["int",[],"NoMem"],
    "GenISA_simdLaneId": ["short",[],"NoMem"],
    "GenISA_simdSize": ["int",[],"NoMem"],
    "GenISA_simdShuffleDown": ["anyint",[0,0,"int"],"Convergent,NoMem"],
    "GenISA_simdBlockRead": ["anyvector",["anyptr"],"ReadMem"],
    "GenISA_simdBlockReadBindless": ["anyvector",["anyptr", "int"],"ReadMem"],
    "GenISA_simdBlockWrite": ["void",["anyptr","anyvector"],"None"],
    "GenISA_simdBlockWriteBindless": ["void",["anyptr","anyvector", "int"],"None"],
    "GenISA_MediaBlockRead": ["anyint",["int","int","int","int","int","int"],"None"],
    "GenISA_MediaBlockWrite": ["void",["int","int","int","int","int","int","anyint"],"None"],
    "GenISA_MediaBlockRectangleRead": ["void",["int","int","int","int","int","int","int"],"None"],
    "GenISA_simdMediaBlockRead": ["anyvector",["int","int","int","int"],"None"],
    "GenISA_simdMediaBlockWrite": ["void",["int","int","int","int","anyvector"],"None"],
    "GenISA_simdMediaRegionCopy": ["void",["int","int","int","int","int","int","int","int","int","int","int","int"],"None"],
    "GenISA_vaErode": ["void",["ptr_local","float2","int","int"],"None"],
    "GenISA_vaDilate": ["void",["ptr_local","float2","int","int"],"None"],
    "GenISA_vaMinMaxFilter": ["void",["ptr_local","float2","int","int"],"None"],
    "GenISA_vaConvolve": ["void",["ptr_local","float2","int","int"],"None"],
    "GenISA_vaConvolveGRF_16x1": ["short",["float2","int","int"],"None"],
    "GenISA_vaConvolveGRF_16x4": ["short4",["float2","int","int"],"None"],
    "GenISA_vaMinMax": ["void",["ptr_local","float2","int","int"],"None"],
    "GenISA_vaCentroid": ["void",["ptr_local","float2","int2","int","int"],"None"],
    "GenISA_vaBoolCentroid": ["void",["ptr_local","float2","int2","int","int"],"None"],
    "GenISA_vaBoolSum": ["void",["ptr_local","float2","int2","int","int"],"None"],
    "GenISA_vmeSendIME": ["void",["int","int","int","int","int","int","int","int"],"None"],
    "GenISA_vmeSendIME2": ["anyvector",["anyvector","int","int","int","int"],"None"],
    "GenISA_vmeSendFBR": ["void",["int","int","int","int","int","int","int","int"],"None"],
    "GenISA_vmeSendFBR2": ["int4",["int4","int","int","int"],"None"],
    "GenISA_vmeSendSIC": ["void",["int","int","int","int","int","int"],"None"],
    "GenISA_vmeSendSIC2": ["int4",["int4","int","int","int"],"None"],
    "GenISA_source_value": ["void",["int"],"None"],
    "GenISA_movreg": ["int",["int"],"None"],
    "GenISA_movflag": ["int",["int"],"None"],
    "GenISA_movcr": ["int",["int"],"None"],
    "GenISA_hw_thread_id": ["int",[],"NoMem"],
    "GenISA_slice_id": ["int",[],"NoMem"],
    "GenISA_subslice_id": ["int",[],"NoMem"],
    "GenISA_eu_id": ["int",[],"NoMem"],
    "GenISA_getSR0" : ["int", ["int"], "None"],
    "GenISA_eu_thread_id": ["int",[],"NoMem"],
    "GenISA_eu_thread_pause": ["void",["int"],"None"],
    "GenISA_setMessagePhaseX_legacy": ["void",["int","int","int","anyint"],"None"],
    "GenISA_setMessagePhase_legacy": ["void",["int","int","int"],"None"],
    "GenISA_createMessagePhases": ["int",["int"],"None"],
    "GenISA_createMessagePhasesV": ["anyvector",["int"],"None"],
    "GenISA_createMessagePhasesNoInit": ["int",["int"],"None"],
    "GenISA_createMessagePhasesNoInitV": ["anyvector",["int"],"None"],
    "GenISA_getMessagePhaseX": ["anyint",["int","int","int"],"None"],
    "GenISA_getMessagePhaseXV": ["anyint",["anyvector","int","int"],"None"],
    "GenISA_setMessagePhaseX": ["int",["int","int","int","anyint"],"None"],
    "GenISA_setMessagePhaseXV": ["anyvector",[0,"int","int","anyint"],"None"],
    "GenISA_getMessagePhase": ["int",["int","int"],"None"],
    "GenISA_getMessagePhaseV": ["int",["anyvector","int"],"None"],
    "GenISA_setMessagePhase": ["int",["int","int","int"],"None"],
    "GenISA_setMessagePhaseV": ["anyvector",[0,"int","int"],"None"],
    "GenISA_broadcastMessagePhase": ["anyint",["int","int","int","int"],"None"],
    "GenISA_broadcastMessagePhaseV": ["anyint",["anyvector","int","int","int"],"None"],
    "GenISA_simdSetMessagePhase": ["int",["int","int","int","int","int","anyint"],"None"],
    "GenISA_simdSetMessagePhaseV": ["anyvector",[0,"int","int","int","int","anyint"],"None"],
    "GenISA_simdGetMessagePhase": ["anyint",["int","int","int"],"None"],
    "GenISA_simdGetMessagePhaseV": ["anyint",["anyvector","int","int"],"None"],
    "GenISA_extractMVAndSAD": ["void",["int","int","int","int"],"None"],
    "GenISA_cmpSADs": ["void",["int","int","int","int"],"None"],
    "GenISA_OWordPtr": ["anyptr",["int"],"NoMem"],
    "GenISA_StackAlloca": ["ptr_private",["int"],"NoMem"],
    "GenISA_RTDualBlendSource": ["void",["float","bool","anyfloat",0,0,0,0,0,0,0,"float","float",
                                 "int","bool","bool","bool","bool","int"],"None"],
    "GenISA_RTWrite": ["void",["anyfloat","float","bool",0,0,0,0,"float","float","int","int","bool",
                       "bool","bool","bool","int"],"None"],
    # (owordOffset, mask, x1, y1, z1, w1, x2, y2, z2, w2)
    "GenISA_URBWrite": ["void",["int","int","float","float","float","float","float","float","float","float"],"None"],
    # (index, owordOffset)->float8
    "GenISA_URBRead": ["float8",["int","int"],"NoMem"],
    # In-place data read using URB Write Handle. (owordOffset)->float8
    "GenISA_URBReadOutput": ["float8",["int"],"NoMem"],
    "GenISA_SetDebugReg": ["int",["int"],"None"],
    "GenISA_add_pair": [["int","int"],["int","int","int","int"],"NoMem"],
    "GenISA_sub_pair": [["int","int"],["int","int","int","int"],"NoMem"],
    "GenISA_mul_pair": [["int","int"],["int","int","int","int"],"NoMem"],
    "GenISA_pair_to_ptr": ["anyptr",["int","int"],"NoMem"],
    "GenISA_ptr_to_pair": [["int","int"],["anyptr"],"NoMem"],
    "GenISA_WaveBallot": ["int",["bool"],"Convergent,InaccessibleMemOnly"],
    # Arg 0  - Mask value
    # Return - assigns each lane the value of its corresponding bit.
    "GenISA_WaveInverseBallot": ["bool",["int"],"Convergent,InaccessibleMemOnly"],
    "GenISA_WaveShuffleIndex": ["anyint",[0,"int"],"Convergent,NoMem"],
    "GenISA_WaveAll": ["anyint",[0,"char"],"Convergent,InaccessibleMemOnly"],
    # Arg 0  - Src value
    # Arg 1  - Operation type
    # Arg 2  - Is the operation inclusive (1) or exclusive (0)?
    # Arg 3  - a mask that specifies a subset of lanes to participate
    #          in the computation.
    # Return - The computed prefix/postfix result
    "GenISA_WavePrefix": ["anyint",[0,"char","bool","bool"],"Convergent,InaccessibleMemOnly"],
    "GenISA_QuadPrefix": ["anyint",[0,"char","bool"],"Convergent,InaccessibleMemOnly"],
    "GenISA_InitDiscardMask": ["bool",[],"None"],
    "GenISA_UpdateDiscardMask": ["bool",["bool","bool"],"None"],
    "GenISA_GetPixelMask": ["bool",["bool"],"None"],
    "GenISA_Copy": ["anyvector",[0],"None"],
    "GenISA_dp4a_ss": ["int",["int","int","int"],"NoMem"],
    "GenISA_dp4a_uu": ["int",["int","int","int"],"NoMem"],
    "GenISA_dp4a_su": ["int",["int","int","int"],"NoMem"],
    "GenISA_dp4a_us": ["int",["int","int","int"],"NoMem"],
    "GenISA_is_uniform": ["bool",["any"],"NoMem"]
}
