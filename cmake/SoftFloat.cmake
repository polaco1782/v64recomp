# Berkeley SoftFloat Release 3e, pinned to its upstream source revision.
include(FetchContent)
FetchContent_Declare(softfloat
    GIT_REPOSITORY https://github.com/ucb-bar/berkeley-softfloat-3.git
    GIT_TAG 07ebd29db0cfb55723a79fbbffe99103ef38e305)
FetchContent_MakeAvailable(softfloat)
set(softfloat_names
    f32_add f32_sub f32_mul f32_div f32_sqrt f64_add f64_sub f64_mul f64_div f64_sqrt
    f32_to_f64 f64_to_f32 f32_to_i32 f32_to_i64 f64_to_i32 f64_to_i64
    i32_to_f32 i32_to_f64 i64_to_f32 i64_to_f64
    s_addMagsF32 s_subMagsF32 s_addMagsF64 s_subMagsF64
    s_normSubnormalF32Sig s_normSubnormalF64Sig
    s_roundPackToF32 s_roundPackToF64 s_normRoundPackToF32 s_normRoundPackToF64
    s_roundToI32 s_roundToI64 s_shiftRightJam32 s_shiftRightJam64 s_shiftRightJam64Extra
    s_shortShiftRightJam64 s_shortShiftRightJam64Extra
    s_countLeadingZeros8 s_countLeadingZeros32 s_countLeadingZeros64
    s_approxRecip_1Ks s_approxRecip32_1 s_approxRecipSqrt_1Ks s_approxRecipSqrt32_1
    s_mul64To128 softfloat_state)
set(softfloat_sources)
foreach(name IN LISTS softfloat_names)
    list(APPEND softfloat_sources "${softfloat_SOURCE_DIR}/source/${name}.c")
endforeach()
foreach(name softfloat_raiseFlags s_propagateNaNF32UI s_propagateNaNF64UI
        s_f32UIToCommonNaN s_f64UIToCommonNaN s_commonNaNToF32UI s_commonNaNToF64UI)
    list(APPEND softfloat_sources "${softfloat_SOURCE_DIR}/source/8086-SSE/${name}.c")
endforeach()
add_library(recomp_softfloat STATIC ${softfloat_sources})
target_include_directories(recomp_softfloat PUBLIC "${softfloat_SOURCE_DIR}/source/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/cmake/softfloat" "${softfloat_SOURCE_DIR}/source/8086-SSE")
target_compile_definitions(recomp_softfloat PUBLIC SOFTFLOAT_FAST_INT64 THREAD_LOCAL=_Thread_local
    PRIVATE INLINE_LEVEL=0)
configure_file("${softfloat_SOURCE_DIR}/COPYING.txt" "${CMAKE_BINARY_DIR}/bin/licenses/SoftFloat.txt" COPYONLY)
